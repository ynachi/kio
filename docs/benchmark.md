# kio vs Tokio HTTP Benchmark

Comparison of a simple HTTP "Hello World" server between kio (C++20 + io_uring) and Tokio (Rust).

## Test Setup

**Hardware:**
- Provider: Hetzner ARM
- CPU: Ampere Altra (8 cores)
- RAM: 16GB
- Network: Internal (client and server on the same provider)
- 1 VM for the server, 1 VM for the client

**Software:**
- OS: Linux
- kio: C++20 coroutines with io_uring (SINGLE_ISSUER mode)
- Tokio: Rust async runtime with SO_REUSEPORT
- Both: 8 worker threads
- Benchmark tool: wrk

**Server Code:**
- Simple HTTP/1.1 response: "Hello, World!" (13 bytes)
- No logging or I/O other than network
- No request parsing beyond basic HTTP
- Connection handling: accept → read → write → repeat

## Results

### 50 Connections

| Metric       | kio (C++) | Tokio (Rust) |
|--------------|-----------|--------------|
| Requests/sec | 139,489   | 110,599      |
| Avg Latency  | 350µs     | 437µs        |
| P50 Latency  | 299µs     | 396µs        |
| P99 Latency  | 1.06ms    | 1.14ms       |
| CPU Usage    | ~40%      | ~20%         |

```bash
wrk -t12 -c50 -d30s --latency http://server:8080
```

### 500 Connections

| Metric       | kio (C++) | Tokio (Rust) |
|--------------|-----------|--------------|
| Requests/sec | 183,992   | 159,074      |
| Avg Latency  | 2.69ms    | 3.10ms       |
| P50 Latency  | 2.37ms    | 2.65ms       |
| P99 Latency  | 7.11ms    | 7.90ms       |
| CPU Usage    | ~40%      | ~40%         |

```bash
wrk -t12 -c500 -d30s --latency http://server:8080
```

### 2000 Connections

| Metric        | kio (C++)   | Tokio (Rust) |
|---------------|-------------|--------------|
| Requests/sec  | 179,944     | 176,440      |
| Avg Latency   | 5.58ms      | 5.66ms       |
| P50 Latency   | 5.30ms      | 5.10ms       |
| P99 Latency   | 9.74ms      | 12.95ms      |
| CPU Usage     | ~40%        | ~40%         |
| Socket Errors | 983 connect | 983 connect  |

```bash
wrk -t12 -c2000 -d30s --latency http://server:8080
```

## Summary

- **Low concurrency (50):** kio 26% higher throughput, 20% lower latency
- **Medium concurrency (500):** kio 16% higher throughput, 13% lower latency
- **High concurrency (2000):** kio 2% higher throughput, latency similar

Both implementations saturate around 180k req/sec on this hardware.

## Implementation Details

### kio (C++)

**Architecture:**
- Thread-per-core with CPU pinning
- io_uring with SINGLE_ISSUER flag
- SQPOLL for kernel polling
- Zero work-stealing between workers
- Each worker independently accepts connections

**Key Features:**
- Stack-allocated buffers (no heap allocations in hot path)
- C++20 coroutines compiled to state machines
- Direct io_uring SQE submission
- No cross-thread coordination

**Configuration:**
```cpp
WorkerConfig config{};
config.uring_queue_depth = 16800;
config.default_op_slots = 8096;
IOPool pool(8, config, ...);
```

### Tokio (Rust)

**Architecture:**
- Multithreaded work-stealing scheduler
- io_uring support (experimental, not default)
- SO_REUSEPORT for multithreaded accept
- Tasks can migrate between threads

**Key Features:**
- Heap-allocated buffers (Vec<u8>)
- Async/await syntax with runtime overhead
- Work-stealing helps with load balancing
- Rich ecosystem and tooling

**Configuration:**
```rust
#[tokio::main(flavor = "multi_thread", worker_threads = 8)]
socket.set_reuse_port(true)
```

## Differences Explained

### Why kio is faster at low-medium concurrency

1. **Direct io_uring usage:** No abstraction layer, direct SQE submission
2. **Thread affinity:** Workers pinned to cores, better cache locality
3. **Zero allocations:** Stack buffers vs heap-allocated Vec
4. **Simpler scheduling:** No work-stealing overhead
5. **SINGLE_ISSUER optimization:** Only one thread submits per ring

### Why they converge at high concurrency

At 2000 connections, both are bottlenecked by:
- Network bandwidth (approaching line rate)
- Kernel scheduling overhead
- Memory bandwidth for 2000 active buffers

The io_uring advantage matters less when saturated.

### CPU usage notes

- kio: Consistent 40% across all tests (efficient saturation)
- Tokio: 20% at 50 conn, 40% at 500+ (work-stealing overhead at low load)

## Caveats

1. **Synthetic benchmark:** Real applications have business logic, database queries, etc.

## Reproducing

### Build kio server

```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
./tcp_demo
```

### Build Tokio server

```bash
cargo build --release
./target/release/tokio_server
```

### Run benchmark

```bash
# On client machine
wrk -t12 -c500 -d30s --latency http://server_ip:8080
```

## Source Code

**kio server:** See [tcp_demo](../demo/tcp.cpp)
**Tokio server:** See [tcp_benchmark](../benchmarks/rust/tcp/src/main.rs)

Both implementations are kept as simple as possible for fair comparison.

## Conclusions

- kio shows 15-26% higher throughput at typical web server concurrency levels
- Both implementations scale similarly to high concurrency
- io_uring with thread-per-core provides measurable performance benefits
- Tokio's work-stealing may be beneficial for mixed CPU/IO workloads (not tested here)
- For pure I/O throughput, kio's simpler architecture has less overhead
