# URing: High-Performance C++20 io_uring Framework

URing is a low-latency, "thread-per-core" asynchronous I/O framework for Linux. It provides a type-safe C++20 coroutine
interface over `io_uring`, designed for maximum throughput and predictable latency in systems programming.

## Architectural Philosophy

Unlike high-level "managed" runtimes, URing follows a **Resource-Provider** model:

1. **Inert Initialization**: `IO` contexts are initialized in a "disabled" state. This allows you to perform heavy
   setup (FD allocation, memory reservation, shared backend attachment) on a main thread before moving the context to a
   dedicated worker.
2. **Explicit Ownership**: Leverages `IORING_SETUP_SINGLE_ISSUER`. The thread that calls `run_blocking()` becomes the
   registered owner, ensuring zero-mutex submission paths.
3. **Shared Backend (WQ Attachment)**: Enables a "Leader-Follower" model where multiple rings share a single async
   worker pool, reducing kernel-thread sprawl and context-switching overhead.
4. **Stable Memory Architecture**: Designed for stack-friendly usage with strict move-semantics that prevent
   invalidating coroutine references.

## Key Features

- **C++20 Coroutines**: Native `Task<T>` and `IoAwaiter` implementation with symmetric transfer for flat stack frames.
- **Zero-Allocation Hot Path**: MPSC intrusive queues and optimized SQE management.
- **Shared Async Workers**: Attach multiple rings to a single leader's kernel worker pool.
- **Direct I/O & Buffering**: Unified support for file and network descriptors via `Fd` RAII wrappers.
- **Observability**: Built-in async logging and performance counters.

## Requirements

- **OS**: Linux Kernel 5.11+ (6.0+ recommended for best performance)
- **Compiler**: GCC 14+ or Clang 18+ (C++23 support required)
- **Library**: `liburing` 2.5+

## Quick Start

### 1. Basic Setup (Standalone)

```cpp
#include "uring/core/io.h"
#include <iostream>

using namespace URing;

Task<void> HelloWorld(IO& io) {
    auto open_res = co_await io.open("test.txt", O_RDONLY);
    if (!open_res) {
        std::cerr << "Failed to open file\n";
        co_return {};
    }

    Fd file = std::move(*open_res);
    std::byte buffer[1024];
    auto read_res = co_await io.read(file, buffer);
    
    std::cout << "Read " << *read_res << " bytes\n";
    co_return {};
}

int main() {
    IO io(0); // Create inert IO context
    
    std::stop_source stop;
    io.schedule(HelloWorld(io));
    
    // run_blocking activates the ring and takes ownership
    io.run_blocking(stop.get_token());
    
    return 0;
}
```

### 2. Multi-Threaded Shared Backend (Leader/Follower)

```cpp
#include "uring/extention/io_pool.hpp"

void StartNetworkSystem() {
    IoOptions opts;
    opts.entries = 4096;
    opts.worker_cpu_affinity = {0, 1, 2, 3}; // Pin to physical cores

    // IoContext manages Leader/Follower initialization automatically
    IoContext pool(4, opts); 

    // Schedule work on specific workers
    pool.worker(0).schedule(MyServerTask(pool.worker(0)));
    
    // Join or stop via stop_source
    pool.join();
}
```

## Lifecycle & State Machine

An `IO` object exists in three states:

| State         | Transition             | Action                                                |
|:--------------|:-----------------------|:------------------------------------------------------|
| **Inert**     | Constructor / `init()` | `io_uring` created but disabled. **Move is allowed.** |
| **Activated** | `activate()`           | Ring enabled, `eventfd` armed. **Move is forbidden.** |
| **Running**   | `run_blocking()`       | Loop entering `tick()`. Thread ownership registered.  |

> **Warning**: Moving an `IO` object while it is in the **Running** state will trigger `std::terminate()`. This protects
> suspended coroutines from holding dangling references to the `IO` context.

## API Reference

### Core I/O Operations

All operations are available as methods on the `IO` instance:

- `accept(Fd& server_fd, ...)`
- `connect(Fd& fd, const SocketAddress& addr)`
- `read(Fd& fd, std::span<std::byte> buf, off_t offset)`
- `write(Fd& fd, std::span<const std::byte> buf, off_t offset)`
- `open(std::filesystem::path, flags, mode)`
- `close(Fd&& fd)`
- `timeout(std::chrono::duration)`

### Performance Tuning

Configure `IoOptions` before initialization:

- `entries`: Submission Queue size.
- `sq_thread_idle_ms`: Kernel thread sleep time (for `SQPOLL`).
- `worker_cpu_affinity`: List of CPU cores for thread pinning.
- `batch_max_size`: Max completions processed per tick.

## Build System

### CMake Integration

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibUring REQUIRED liburing)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE uring)
```

### Building from Source

```bash
cmake --preset dev
cmake --build build -j$(nproc)
cd build && make check
```

## Contributing

URing is built for high-performance networking and storage. Tests and demos are verified with **TSAN** and **ASAN** to
ensure memory safety and race-free operation.

---
**Author**: ynachi  
**License**: MIT

```
