# uring_exec

A minimal io_uring coroutine executor for storage engines and proxies.

## Philosophy

- **Simple** — ~1000 lines of code you can read and understand
- **Focused** — Storage and network I/O, nothing else
- **No dependencies** — Just liburing and C++23 standard library
- **No exceptions on hot path** — `std::expected` for error handling

## Requirements

- Linux 6.1+ (for `DEFER_TASKRUN`)
- liburing
- C++23 compiler

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                             Runtime                                     │
│                                                                         │
│   ┌─────────────────┐   ┌─────────────────┐   ┌─────────────────┐      │
│   │  ThreadContext  │   │  ThreadContext  │   │  ThreadContext  │      │
│   │  ┌───────────┐  │   │  ┌───────────┐  │   │  ┌───────────┐  │      │
│   │  │ Executor  │  │   │  │ Executor  │  │   │  │ Executor  │  │      │
│   │  │ (io_ring) │  │   │  │ (io_ring) │  │   │  │ (io_ring) │  │      │
│   │  └───────────┘  │   │  └───────────┘  │   │  └───────────┘  │      │
│   │  ┌───────────┐  │   │  ┌───────────┐  │   │  ┌───────────┐  │      │
│   │  │ eventfd   │◄─┼───┼──│   MPSC    │──┼───┼─►│ eventfd   │  │      │
│   │  └───────────┘  │   │  └───────────┘  │   │  └───────────┘  │      │
│   └─────────────────┘   └─────────────────┘   └─────────────────┘      │
│                                                                         │
│   • Thread-per-ring: no contention on hot path                         │
│   • Cross-thread scheduling via eventfd wakeup                         │
│   • Shutdown drains pending work                                        │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

## Components

| File             | Lines | Purpose                                         |
|------------------|-------|-------------------------------------------------|
| `uring_exec.hpp` | ~600  | Executor, Task<T>, I/O operations               |
| `runtime.hpp`    | ~370  | Multi-threaded runtime, cross-thread scheduling |
| `mpsc_queue.hpp` | ~60   | Simple thread-safe queue                        |

## Quick Start

### Single-Threaded

```cpp
#include "uring_exec.hpp"
using namespace uring;

Task<void> example(Executor& ex, int fd) {
    char buf[4096];
    
    auto res = co_await read(ex, fd, buf, sizeof(buf), 0);
    if (!res) {
        // res.error() is errno
        co_return;
    }
    
    int bytes_read = *res;
}

int main() {
    Executor ex;
    ex.spawn(example(ex, some_fd));
    ex.run();
}
```

### Multi-Threaded

```cpp
#include "runtime.hpp"
using namespace uring;

Task<void> handler(Executor& ex, int client_fd) {
    char buf[4096];
    while (true) {
        auto res = co_await recv(ex, client_fd, buf, sizeof(buf));
        if (!res || *res == 0) break;
        co_await send(ex, client_fd, buf, *res);
    }
    co_await close(ex, client_fd);
}

int main() {
    Runtime rt(RuntimeConfig{
        .num_threads = 4,
        .pin_threads = true
    });
    rt.start(true);
    
    // Schedule work on specific thread
    rt.thread(0).schedule(some_task());
    
    // Or round-robin
    rt.schedule(some_task());
    
    // ... wait for shutdown signal ...
    rt.stop();  // Drains pending work
}
```

## Operations

### Storage

| Function                          | Description                             |
|-----------------------------------|-----------------------------------------|
| `read(ex, fd, buf, len, offset)`  | Positioned read                         |
| `write(ex, fd, buf, len, offset)` | Positioned write                        |
| `fsync(ex, fd, datasync)`         | Sync file (datasync=true for fdatasync) |

### Network

| Function                        | Description           |
|---------------------------------|-----------------------|
| `accept(ex, fd, addr*, len*)`   | Accept connection     |
| `connect(ex, fd, addr, len)`    | Connect to address    |
| `recv(ex, fd, buf, len, flags)` | Receive data          |
| `send(ex, fd, buf, len, flags)` | Send data             |
| `close(ex, fd)`                 | Close file descriptor |

### Utility

| Function                       | Description                   |
|--------------------------------|-------------------------------|
| `timeout(ex, nanoseconds)`     | Sleep                         |
| `timeout_ms(ex, milliseconds)` | Sleep (milliseconds)          |
| `cancel(ex, op_ptr)`           | Cancel operation by user_data |
| `cancel_fd(ex, fd)`            | Cancel all operations on fd   |

## Error Handling

All operations return `std::expected<T, int>` where the error is `errno`:

```cpp
auto res = co_await read(ex, fd, buf, len, 0);

if (!res) {
    int err = res.error();  // errno value
    std::fprintf(stderr, "failed: %s\n", std::strerror(err));
    co_return;
}

int bytes = *res;  // Success value
```

You can also use C++23 monadic operations:

```cpp
co_await read(ex, fd, buf, len, 0)
    .transform([](int n) { return process(n); })
    .or_else([](int e) { return handle_error(e); });
```

## Cross-Thread Scheduling

Move execution between threads with `schedule_on`:

```cpp
Task<void> mixed_workload(Runtime& rt, int fd) {
    auto& io_thread = rt.thread(0);
    auto& compute_thread = rt.thread(1);
    
    // Read on I/O thread
    char buf[4096];
    auto res = co_await read(io_thread.executor(), fd, buf, sizeof(buf), 0);
    
    // Move to compute thread for CPU work
    co_await schedule_on(compute_thread);
    auto processed = expensive_parse(buf, *res);
    
    // Back to I/O thread to respond
    co_await schedule_on(io_thread);
    co_await write(io_thread.executor(), fd, processed.data(), processed.size(), 0);
}
```

## Configuration

### Executor

```cpp
ExecutorConfig cfg{
    .entries = 256,             // SQ/CQ ring size
    .uring_flags = 0,           // Raw io_uring setup flags
    .sq_thread_idle_ms = 0      // For IORING_SETUP_SQPOLL
};
Executor ex(cfg);

// Example with flags:
ExecutorConfig cfg{
    .entries = 4096,
    .uring_flags = IORING_SETUP_SQPOLL | IORING_SETUP_SINGLE_ISSUER,
    .sq_thread_idle_ms = 1000
};
```

### Runtime

```cpp
RuntimeConfig cfg{
    .num_threads = 4,                    // Thread count
    .pin_threads = false,                // CPU affinity
    .executor_config = ExecutorConfig{}  // Per-thread executor config
};
Runtime rt(cfg);
rt.start(true);  // true = pin threads
```

## Kernel Flags

Pass directly via `uring_flags`. Your responsibility to know what works on your kernel:

| Flag                         | Kernel | Notes                                           |
|------------------------------|--------|-------------------------------------------------|
| `IORING_SETUP_SQPOLL`        | 5.1+   | Kernel-side submission, set `sq_thread_idle_ms` |
| `IORING_SETUP_SINGLE_ISSUER` | 5.20+  | Single thread submits                           |
| `IORING_SETUP_COOP_TASKRUN`  | 5.19+  | Cooperative completions                         |
| `IORING_SETUP_DEFER_TASKRUN` | 6.1+   | Requires `COOP_TASKRUN`                         |

Default is `0` — maximum compatibility, proven ~300K req/sec.

## Patterns

### Accept-Scatter (Proxy)

```cpp
Task<void> acceptor(Runtime& rt, int listen_fd) {
    size_t next = 0;
    while (true) {
        auto res = co_await accept(rt.thread(0).executor(), listen_fd);
        if (!res) continue;
        
        // Round-robin to worker threads
        size_t worker = 1 + (next++ % (rt.size() - 1));
        rt.thread(worker).schedule(handle_client(rt.thread(worker).executor(), *res));
    }
}
```

### Partitioned Storage

```cpp
// Each thread owns a partition
for (size_t i = 0; i < rt.size(); ++i) {
    auto& ctx = rt.thread(i);
    ctx.schedule(manage_partition(ctx.executor(), partition_path(i)));
}
```

## What's Not Included

Intentionally omitted to keep things simple:

| Feature            | When to Add                        |
|--------------------|------------------------------------|
| WhenAll/WhenAny    | When you need concurrent awaits    |
| Registered buffers | When profiling shows copy overhead |
| Fixed files        | When fd lookup is a bottleneck     |
| Multishot accept   | High connection rate servers       |
| Work stealing      | Unlikely to need; adds complexity  |

## Building

```bash
g++ -std=c++23 -o myapp myapp.cpp -luring -pthread
```

Or with CMake:

```cmake
add_subdirectory(uring_exec)
target_link_libraries(myapp PRIVATE uring_exec)
```

## License

Public domain / your choice.