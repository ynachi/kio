# Diagnosing the 60% CPU Cap

## Step 1: Understand CPU Breakdown

When `top` shows 60% CPU, it could mean:
- 60% user + 0% system = User space bound
- 30% user + 30% system = Kernel bound
- 40% user + 20% iowait = I/O bound

### Check CPU Breakdown
```bash
# Run server
./tcp_demo_v1 &
PID=$!

# Generate load
oha -c 1500 -z 60s http://localhost:8080/ &

# Monitor detailed CPU usage
top -H -p $PID

# Press '1' to see per-CPU breakdown
# Look at:
# - %us (user space)
# - %sy (system/kernel)
# - %wa (I/O wait)
# - %id (idle)
```

### More Detailed Analysis
```bash
# Install if needed
sudo apt-get install sysstat

# Monitor per-thread CPU with breakdown
pidstat -t -p $PID 1

# Output shows:
#   %usr - user space CPU
#   %system - kernel CPU
#   %guest - virtualization
#   %wait - I/O wait
#   %CPU - total
```

## Step 2: Profile with perf

```bash
# Record performance data
sudo perf record -F 999 -p $PID -g -- sleep 30

# Analyze
sudo perf report --stdio

# Look for:
# - High % in kernel functions = kernel bound
# - High % in recv/send = I/O bound
# - High % in your code = CPU bound
```

## Step 3: Check io_uring Efficiency

```bash
# Trace io_uring operations
sudo perf trace -e 'io_uring:*' -p $PID

# Count io_uring syscalls
sudo strace -c -p $PID

# Look for:
# - How many io_uring_enter calls per second
# - If it's > 100K/sec, you're syscall bound
# - If it's < 10K/sec, you're batching well
```

## Step 4: Network Stack Analysis

```bash
# Check if network is the bottleneck
ss -s  # Socket statistics
netstat -s  # Protocol statistics

# Check for:
# - TCP retransmissions (sign of congestion)
# - Socket buffer overflows
# - Kernel drops

# Network interface stats
ethtool -S eth0 | grep -i drop
ifconfig eth0  # Look for overruns/errors
```

## Common Bottlenecks

### Bottleneck 1: Kernel Networking Stack (Most Likely)

**Symptoms**:
- High %system CPU
- Low %user CPU
- Can't push beyond ~300-400K req/s

**Why**: Linux TCP/IP stack processes packets in kernel space:
```
Your Process (user space)     Kernel (system space)
├─ recv() ──────────────────→ ├─ TCP processing
├─ process                     ├─ IP routing
├─ send() ──────────────────→ ├─ NIC driver
└─ (20-30% CPU)               └─ (30-40% CPU)
```

**Solutions**:
1. Use io_uring zero-copy send (IORING_OP_SEND_ZC)
2. Enable TCP_NODELAY to reduce latency
3. Tune TCP buffer sizes
4. Consider kernel bypass (DPDK/XDP) for extreme performance

### Bottleneck 2: io_uring Submit Overhead

**Symptoms**:
- High number of io_uring_enter syscalls
- Lots of time in io_uring_submit()

**Fix**: Batch submissions
```cpp
// Bad: Submit after every operation
co_await recv(...);  // Submit
co_await send(...);  // Submit

// Better: Let io_uring batch naturally
// The event loop already submits once per iteration
```

### Bottleneck 3: eventfd Overhead

**Symptoms**:
- High time in read()/write() on eventfd
- Many eventfd operations in strace

**Why**: Every task enqueue writes to eventfd:
```cpp
scheduleOn() → wakeThread() → write(eventfd)
```

At 327K req/s with task queueing, that's potentially millions of eventfd writes.

**Fix**: Implement eventfd coalescing
```cpp
bool IoUringExecutor::scheduleOn(size_t context_id, Func&& func)
{
    auto& ctx = *contexts_[context_id];
    
    // Check if queue was empty
    size_t old_size = ctx.task_queue.size_approx();
    
    if (!ctx.task_queue.enqueue(std::move(func))) {
        return false;
    }
    
    // Only wake if queue was empty (coalesce wakeups)
    if (old_size == 0) {
        wakeThread(ctx);
    }
    
    return true;
}
```

### Bottleneck 4: Lock Contention

**Check for lock contention**:
```bash
sudo perf lock record -p $PID -- sleep 10
sudo perf lock report

# Look for high contention on mutexes
```

V1 shouldn't have lock contention (lock-free queue), but verify.

### Bottleneck 5: Context Switches

```bash
# Monitor context switches
pidstat -w -p $PID 1

# Look at:
# - cswch/s (voluntary context switches)
# - nvcswch/s (involuntary context switches)

# If > 50K/sec per thread, you have excessive switching
```

## Expected Results for "Hello World" HTTP

For a simple HTTP server, 60% CPU is actually **reasonable**:

| Component | Expected CPU | Why |
|-----------|--------------|-----|
| User space (your code) | 20-30% | recv, send, HTTP parsing |
| Kernel (TCP/IP) | 30-40% | Packet processing, DMA |
| Idle/wait | 30-40% | Waiting for network I/O |

**Total**: 60-70% is typical for I/O-bound workloads!

## How PhotonLib Gets 5% More Throughput

PhotonLib's advantage likely comes from:

1. **Mature optimizations**: Years of production tuning
2. **Better batching**: Stackful coroutines can batch more naturally
3. **Different kernel path**: epoll might be more efficient for this workload
4. **Zero-copy tricks**: They probably use splice/sendfile

## Pushing Beyond 60% CPU

### Strategy 1: Add CPU Work

To verify if you CAN use more CPU:
```cpp
Lazy<void> handleClient(IoUringExecutor* executor, int client_fd)
{
    char buffer[4096];
    while (true)
    {
        ssize_t bytes_read = co_await io::v1::recv(...);
        if (bytes_read <= 0) break;

        // Add artificial CPU work
        volatile int sum = 0;
        for (int i = 0; i < 10000; ++i) {
            sum += i;
        }

        co_await io::v1::send(...);
    }
}
```

If CPU goes to 80-90%, your executor is fine. The limit is the I/O workload.

### Strategy 2: Use SQPOLL

```cpp
config.io_uring_flags = IORING_SETUP_SQPOLL;
```

This moves submission to kernel thread, reducing syscall overhead.

### Strategy 3: Zero-Copy Send

```cpp
// Instead of regular send
co_await io::v1::send(executor, fd, buf, len);

// Use zero-copy (kernel 6.0+)
co_await io::v1::send_zc(executor, fd, buf, len);
```

This eliminates one memory copy, saving ~5-10% CPU.

### Strategy 4: Disable Nagle

```cpp
int flag = 1;
setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
```

Reduces latency and can improve throughput.

## Verdict

**Your 327K req/s at 60% CPU is likely optimal for this workload.**

The 5% gap to PhotonLib (342K) is not significant and could be due to:
- Measurement variance
- PhotonLib's maturity
- Different test conditions

To confirm, run these diagnostics:

```bash
# 1. Check CPU breakdown
pidstat -t -p $(pgrep tcp_demo) 1

# 2. Profile for 30 seconds
sudo perf record -F 999 -p $(pgrep tcp_demo) -g -- sleep 30
sudo perf report

# 3. Check network stats
watch -n1 'ss -s'
```

If `perf report` shows most time in kernel (tcp_sendmsg, tcp_recvmsg, etc.), then **your code is fine** - you're hitting kernel networking limits.

If `perf report` shows time in your executor code (scheduleOn, wakeThread), then **we can optimize further**.

**Share the perf report output and I'll provide specific optimizations!**
