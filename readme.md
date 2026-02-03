# AIO - Modern C++ Async I/O Library

A high-performance, coroutine-based asynchronous I/O library for Linux built on `io_uring`.
Tests and demo are built with TSAN enabled and are currently free from any issue.

## Features

- **C++20 Coroutines** - Natural async/await syntax with zero-overhead abstractions
- **io_uring Backend** - Leverages Linux's most efficient async I/O interface
- **Single-Threaded Per-Core** - Thread-per-core architecture for predictable performance
- **Zero-Copy I/O** - Efficient `splice`/`sendfile` support via pipe pools
- **Lazy Timeout Management** - O(1) per-operation overhead, optimized for the common case
- **Structured Concurrency** - `TaskGroup` for safe concurrent task management
- **Blocking Pool** - Offload blocking operations without stalling the event loop
- **Built-in Observability** - Optional statistics and async logging

## Requirements

- Linux kernel 5.11+ (for full io_uring feature support)
- GCC 14+ or Clang 15+ (C++20 coroutine support and c++23 features)
- liburing 2.5+
- OpenSSL 3.2+ (for TLS support)

## Quick Start

### Hello World, TCP Echo Server

```cpp
#include <aio/aio.hpp>

using namespace aio;
using namespace std::chrono_literals;

Task<> HandleClient(IoContext& ctx, net::Socket client)
{
    std::array<std::byte, 4096> buf{};
    
    while (true)
    {
        // Read with 30 second timeout
        auto result = co_await AsyncRecv(ctx, client, buf).WithTimeout(30s);
        
        if (!result || *result == 0)
            break;  // Error or client disconnected
        
        // Echo back
        co_await AsyncSend(ctx, client, std::span{buf.data(), *result});
    }
}

Task<> Server(IoContext& ctx, uint16_t port)
{
    auto listener = net::TcpListener::BindV4(port);
    if (!listener)
        throw std::runtime_error("Failed to bind");
    
    TaskGroup tasks;
    
    while (true)
    {
        auto accepted = co_await AsyncAccept(ctx, *listener);
        if (!accepted)
            continue;
        
        tasks.Spawn(HandleClient(ctx, net::Socket(accepted->fd)));
    }
}

int main()
{
    IoContext ctx;
    ctx.RunUntilDone(Server(ctx, 8080));
    return 0;
}
```

### Multi-Core Server

```cpp
#include <aio/aio.hpp>

int main()
{
    const size_t num_cores = std::thread::hardware_concurrency();
    std::vector<aio::Worker> workers;
    
    std::stop_source stop;
    
    for (size_t i = 0; i < num_cores; ++i)
    {
        workers.emplace_back(i);
        workers.back().Start(
            [st = stop.get_token()](aio::IoContext& ctx)
            {
                ctx.RunUntilDone(Server(ctx, 8080, st));
            },
            i  // Pin to CPU core
        );
    }
    
    // Wait for signal
    aio::IoContext main_ctx;
    aio::SignalSet signals{SIGINT, SIGTERM};
    main_ctx.RunUntilDone([&]() -> aio::Task<> {
        co_await aio::AsyncWaitSignal(main_ctx, signals);
        stop.request_stop();
    }());
    
    for (auto& w : workers)
        w.Join();
    
    return 0;
}
```

## Core Concepts

### IoContext

The event loop. Each thread should have exactly one `IoContext`.

```cpp
IoContext ctx(1024);  // 1024 SQ entries

// Run until stopped
ctx.Run();

// Run until a specific task completes
ctx.RunUntilDone(MyTask(ctx));

// Run with periodic tick callback
ctx.Run([&] { ProcessMetrics(); });

// Stop the loop (thread-safe)
ctx.Stop();
ctx.Notify();  // Wake if blocked in io_uring_wait
```

### Task<T>

The coroutine return type. They are lazy, they don't start until awaited.

```cpp
Task<int> ComputeAsync(IoContext& ctx)
{
    co_await AsyncSleep(ctx, 100ms);
    co_return 42;
}

Task<> Caller(IoContext& ctx)
{
    int result = co_await ComputeAsync(ctx);
    // result == 42
}
```

### Result<T>

Error handling via `std::expected<T, std::error_code>`.

```cpp
Task<> Example(IoContext& ctx, int fd)
{
    auto result = co_await AsyncRead(ctx, fd, buffer);
    
    if (!result)
    {
        // Handle error
        std::cerr << "Read failed: " << result.error().message() << "\n";
        co_return;
    }
    
    size_t bytes_read = *result;
}
```

## Async Operations

### File I/O

```cpp
// Open
auto fd = co_await AsyncOpen(ctx, "/path/to/file", O_RDONLY);

// Read/Write
auto n = co_await AsyncRead(ctx, fd, buffer, offset);
auto n = co_await AsyncWrite(ctx, fd, data, offset);

// Vectored I/O
auto n = co_await AsyncReadv(ctx, fd, iovecs, offset);
auto n = co_await AsyncWritev(ctx, fd, iovecs, offset);

// Close
co_await AsyncClose(ctx, fd);

// File operations
co_await AsyncFsync(ctx, fd);
co_await AsyncFallocate(ctx, fd, mode, offset, len);
co_await AsyncFtruncate(ctx, fd, length);
co_await AsyncUnlink(ctx, "/path/to/file");
co_await AsyncRename(ctx, "/old/path", "/new/path");
co_await AsyncMkdir(ctx, "/path/to/dir", 0755);
```

### Network I/O

```cpp
// TCP Server
auto listener = net::TcpListener::BindV4(8080);
auto accepted = co_await AsyncAccept(ctx, *listener);
net::Socket client(accepted->fd);

// TCP Client
net::Socket sock(socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
auto addr = net::SocketAddress::V4(8080, "127.0.0.1");
co_await AsyncConnect(ctx, sock, addr);

// Send/Recv
auto n = co_await AsyncSend(ctx, sock, data);
auto n = co_await AsyncRecv(ctx, sock, buffer);

// With flags
auto n = co_await AsyncSend(ctx, sock, data, MSG_NOSIGNAL);
auto n = co_await AsyncRecv(ctx, sock, buffer, MSG_WAITALL);

// Sendmsg/Recvmsg (for advanced use cases)
co_await AsyncSendmsg(ctx, sock, &msg, flags);
co_await AsyncRecvmsg(ctx, sock, &msg, flags);
```

### Zero-Copy File Transfer

```cpp
// Send file to socket efficiently using splice
co_await AsyncSendfile(ctx, socket, file_fd, offset, count);
```

### Timeouts

```cpp
// Per-operation timeout
auto result = co_await AsyncRecv(ctx, sock, buf).WithTimeout(5s);

if (!result && result.error() == std::errc::timed_out)
{
    // Handle timeout
}

// Sleep
co_await AsyncSleep(ctx, 100ms);
```

### Polling

```cpp
// Wait for readability
auto events = co_await AsyncPoll(ctx, fd, POLLIN);

// Wait for writability
auto events = co_await AsyncPoll(ctx, fd, POLLOUT);
```

## Structured Concurrency

### TaskGroup

Manages a collection of concurrent tasks with automatic lifetime management.

```cpp
Task<> ProcessConnections(IoContext& ctx)
{
    TaskGroup tasks;
    
    // Spawn concurrent tasks
    tasks.Spawn(HandleClient(ctx, client1));
    tasks.Spawn(HandleClient(ctx, client2));
    tasks.Spawn(HandleClient(ctx, client3));
    
    // Or spawn multiple at once
    tasks.SpawnAll(
        Task1(ctx),
        Task2(ctx),
        Task3(ctx)
    );
    
    // Wait for all to complete
    co_await tasks.JoinAll(ctx);
    
    // Or with timeout
    bool completed = co_await tasks.JoinAllTimeout(ctx, 30s);
}
```

### Notifier

Cross-thread notification primitive.

```cpp
Notifier notifier;

// Waiting side (coroutine)
Task<> Waiter(IoContext& ctx)
{
    co_await notifier.Wait(ctx);
    // Signaled!
}

// Signaling side (any thread)
notifier.Signal();
```

## Blocking Operations

Use `BlockingPool` to offload blocking operations without stalling the event loop.

```cpp
BlockingPool pool(4);  // 4 worker threads

Task<> Example(IoContext& ctx)
{
    // Offload CPU-intensive work
    auto result = co_await Offload(ctx, pool, [] {
        return ExpensiveComputation();
    });
    
    // Offload blocking syscalls
    auto dns_result = co_await Offload(ctx, pool, [] {
        return getaddrinfo(...);
    });
}
```

### Async DNS Resolution

```cpp
// Synchronous (blocking - use only at startup)
auto addr = net::Resolve("example.com", 443);

// Asynchronous (non-blocking)
auto addr = co_await net::ResolveAsync(ctx, pool, "example.com", 443);
```

## Signal Handling

```cpp
SignalSet signals{SIGINT, SIGTERM};

Task<> WaitForShutdown(IoContext& ctx)
{
    auto sig = co_await AsyncWaitSignal(ctx, signals);
    std::cout << "Received signal: " << *sig << "\n";
}
```

## Worker Threads

Convenience wrapper for running IoContext on dedicated threads.

```cpp
Worker worker;

// Low-level: full control
worker.Start([](IoContext& ctx) {
    ctx.Run();
}, 0);  // Pin to CPU 0

// Run a task to completion
worker.RunTask([](IoContext& ctx) -> Task<> {
    co_await MyServerTask(ctx);
}, 0);

// Run with tick callback
worker.RunLoop([&] { UpdateStats(); }, 0);

// Stop and wait
worker.RequestStop();
worker.Join();
```

## Logging

Built-in async logger with minimal overhead.

```cpp
#include <aio/logger.hpp>

// Configure
aio::alog::g_level = aio::alog::Level::Info;
aio::alog::g_colors = true;

// Log
ALOG_DEBUG("Debug message: {}", value);
ALOG_INFO("Connection from {}:{}", ip, port);
ALOG_WARN("High latency: {}ms", latency);
ALOG_ERROR("Failed to open file: {}", path);
ALOG_FATAL("Unrecoverable error");

// Check dropped messages (if queue overflows)
uint64_t dropped = aio::alog::dropped_count();
```

Compile-time filtering:

```cpp
// In CMakeLists.txt or compile flags
add_definitions(-DLOG_BUILD_LEVEL=1)  // 0=Debug, 1=Info, 2=Warn, 3=Error
```

## Statistics

Optional performance counters (compile with `-DAIO_STATS=1`).

```cpp
#define AIO_STATS 1
#include <aio/aio.hpp>

IoContext ctx;
// ... run workload ...

auto stats = ctx.Stats().GetSnapshot();
std::cout << "Submitted: " << stats.ops_submitted << "\n";
std::cout << "Completed: " << stats.ops_completed << "\n";
std::cout << "Errors: " << stats.ops_errors << "\n";
std::cout << "Timeouts: " << stats.timeouts << "\n";
std::cout << "Max inflight: " << stats.ops_max_inflight << "\n";
std::cout << "Loop iterations: " << stats.loop_iterations << "\n";
```

## IoBuffer

High-performance ring buffer for protocol parsing.

```cpp
IoBuffer buf(4096);

// Read into buffer
auto writable = buf.WritableBytesSpan();
auto n = co_await AsyncRecv(ctx, sock, writable);
buf.Commit(*n);

// Process data
auto readable = buf.ReadableSpan();
size_t consumed = ParseProtocol(readable);
buf.Consume(consumed);

// Build response
buf.Append("HTTP/1.1 200 OK\r\n");
buf.Append("Content-Length: 5\r\n\r\n");
buf.Append("Hello");
buf.Commit();

// Send
co_await AsyncSend(ctx, sock, buf.ReadableBytesSpan());
```

## Socket Options

```cpp
net::Socket sock(fd);

sock.SetNonBlocking();
sock.SetReuseAddr();
sock.SetReusePort();
sock.SetNodelay();
sock.SetSendBuffer(65536);
sock.SetRecvBuffer(65536);
```

## Performance Tips

### 1. Use Thread-Per-Core

Each `IoContext` is single-threaded. Scale by running one per CPU core.

```cpp
for (size_t i = 0; i < num_cores; ++i)
{
    workers.emplace_back(i);
    workers.back().Start(WorkerFunc, i);  // Pin to core i
}
```

### 2. Batch Operations

Use vectored I/O when possible:

```cpp
// Instead of multiple sends
co_await AsyncSend(ctx, sock, header);
co_await AsyncSend(ctx, sock, body);

// Use writev
std::array<iovec, 2> iov = {{{header.data(), header.size()}, {body.data(), body.size()}}};
co_await AsyncWritev(ctx, sock, iov);
```

### 3. Avoid Unnecessary Timeouts

Timeouts have overhead. For connection-level idle detection, consider a periodic scan instead of per-operation timeouts.
Per operation timeout is expensive as it requires 2X io uring submit system calls.

### 4. Use Zero-Copy for Large Transfers

```cpp
// For serving files
co_await AsyncSendfile(ctx, socket, file_fd, 0, file_size);
```

### 5. Pre-size Buffers

```cpp
IoBuffer buf;
buf.Reserve(expected_max_size);  // Avoid reallocations
```

## Building

Look at [build](./docs/build.md) docs.

### CMake

```cmake
cmake_minimum_required(VERSION 3.28)
project(myapp)

set(CMAKE_CXX_STANDARD 23)

add_executable(myapp main.cpp)
target_link_libraries(myapp kio)

# Optional: Enable stats
target_compile_definitions(myapp PRIVATE AIO_STATS=1)
```


## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Application Layer                           │
│   ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                 │
│   │  Coroutine  │  │  Coroutine  │  │  Coroutine  │  ...            │
│   │   Task<T>   │  │   Task<T>   │  │   Task<T>   │                 │
│   └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                 │
│          │                │                │                        │
│          └────────────────┼────────────────┘                        │
│                           │ co_await                                │
├───────────────────────────┼─────────────────────────────────────────┤
│                     kio I/O Layer                                   │
│   ┌───────────────────────┴───────────────────────┐                 │
│   │              Async Operations                 │                 │
│   │  AsyncRead, AsyncWrite, AsyncRecv, AsyncSend  │                 │
│   │  AsyncAccept, AsyncConnect, AsyncClose, ...   │                 │
│   └───────────────────────┬───────────────────────┘                 │
│                           │                                         │
│   ┌───────────────────────┴───────────────────────┐                 │
│   │               IoContext                       │                 │
│   │  - Owns io_uring ring                         │                 │
│   │  - Tracks pending operations                  │                 │
│   │  - Manages completion queue                   │                 │
│   │  - Resumes coroutines on completion           │                 │
│   └───────────────────────┬───────────────────────┘                 │
├───────────────────────────┼─────────────────────────────────────────┤
│                     Linux Kernel                                    │
│   ┌───────────────────────┴───────────────────────┐                 │
│   │               io_uring                        │                 │
│   │  ┌─────────────┐       ┌─────────────┐        │                 │
│   │  │ Submission  │       │ Completion  │        │                 │
│   │  │   Queue     │  ───► │   Queue     │        │                 │
│   │  │   (SQ)      │       │   (CQ)      │        │                 │
│   │  └─────────────┘       └─────────────┘        │                 │
│   └───────────────────────────────────────────────┘                 │
└─────────────────────────────────────────────────────────────────────┘
```

