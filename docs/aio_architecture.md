# kio - High-Performance Async I/O Library

A modern C++23 asynchronous I/O library built on Linux's io_uring and coroutines.
Designed for high-throughput, low-latency server applications.

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [Core Components](#core-components)
- [I/O Operations](#io-operations)
- [Networking](#networking)
- [Helper Functions](#helper-functions)
- [Usage Examples](#usage-examples)
- [Safety & Lifetime](#safety--lifetime)

---

## Overview

**kio** is a minimal, zero-overhead async I/O library that combines:

- **io_uring** - Linux's high-performance async I/O interface
- **C++23 coroutines** - Ergonomic async/await syntax
- **Thread-per-core model** - No cross-thread synchronization in the hot path

### Design Principles

1. **Caller owns buffers** - Library never allocates in the I/O path
2. **Explicit lifetimes** - Operations are tracked; dangling pointers terminate rather than corrupt
3. **Minimal abstraction** - Thin wrappers over io_uring, not a framework
4. **Composable primitives** - Build higher-level abstractions from simple ops

### Requirements

- Linux kernel >= 6.0
- liburing
- C++23 compiler (GCC 13+, Clang 17+)

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Application Layer                           │
│   ┌─────────────┐  ┌─────────────┐  ┌─────────────┐                │
│   │  Coroutine  │  │  Coroutine  │  │  Coroutine  │  ...           │
│   │   Task<T>   │  │   Task<T>   │  │   Task<T>   │                │
│   └──────┬──────┘  └──────┬──────┘  └──────┬──────┘                │
│          │                │                │                        │
│          └────────────────┼────────────────┘                        │
│                           │ co_await                                │
├───────────────────────────┼─────────────────────────────────────────┤
│                     kio I/O Layer                                   │
│   ┌───────────────────────┴───────────────────────┐                │
│   │              Async Operations                  │                │
│   │  AsyncRead, AsyncWrite, AsyncRecv, AsyncSend  │                │
│   │  AsyncAccept, AsyncConnect, AsyncClose, ...   │                │
│   └───────────────────────┬───────────────────────┘                │
│                           │                                         │
│   ┌───────────────────────┴───────────────────────┐                │
│   │               IoContext                        │                │
│   │  - Owns io_uring ring                         │                │
│   │  - Tracks pending operations                  │                │
│   │  - Manages completion queue                   │                │
│   │  - Resumes coroutines on completion           │                │
│   └───────────────────────┬───────────────────────┘                │
├───────────────────────────┼─────────────────────────────────────────┤
│                     Linux Kernel                                    │
│   ┌───────────────────────┴───────────────────────┐                │
│   │               io_uring                         │                │
│   │  ┌─────────────┐       ┌─────────────┐        │                │
│   │  │ Submission  │       │ Completion  │        │                │
│   │  │   Queue     │  ───► │   Queue     │        │                │
│   │  │   (SQ)      │       │   (CQ)      │        │                │
│   │  └─────────────┘       └─────────────┘        │                │
│   └───────────────────────────────────────────────┘                │
└─────────────────────────────────────────────────────────────────────┘
```

### Event Loop Flow

```
┌──────────────────────────────────────────────────────────────┐
│                      IoContext::Run()                        │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
              ┌───────────────────────────────┐
              │  io_uring_submit_and_wait()   │◄─────────────┐
              │  (Submit SQEs, wait for CQEs) │              │
              └───────────────┬───────────────┘              │
                              │                              │
                              ▼                              │
              ┌───────────────────────────────┐              │
              │   Process Completion Queue    │              │
              │   for each CQE:               │              │
              │     - Extract user_data (op)  │              │
              │     - Store result in op      │              │
              │     - Untrack operation       │              │
              │     - Queue handle for resume │              │
              └───────────────┬───────────────┘              │
                              │                              │
                              ▼                              │
              ┌───────────────────────────────┐              │
              │   Resume Coroutines           │              │
              │   for each queued handle:     │              │
              │     - handle.resume()         │──────────────┘
              │     (may submit new SQEs)     │
              └───────────────────────────────┘
```

---

## Core Components

### IoContext

The event loop that owns the io_uring ring and drives coroutine execution.

```cpp
#include <kio/IoContext.hpp>

kio::IoContext ctx(256);  // 256 SQ entries

// Run until task completes
kio::Task<int> my_task = do_work(ctx);
ctx.RunUntilDone(my_task);

// Or run indefinitely with a tick callback
ctx.Run([&]() {
    // Called after each batch of completions
});

// Stop from another context
ctx.Stop();
```

**Key Features:**
- Operation tracking for safe cancellation
- Cross-thread completion injection (for blocking pool integration)
- File descriptor registration for reduced syscall overhead

### Task<T>

The coroutine return type representing an async computation.

```cpp
kio::Task<int> fetch_data(kio::IoContext& ctx, int fd) {
    std::array<std::byte, 1024> buffer;

    auto result = co_await kio::AsyncRead(ctx, fd, buffer);
    if (!result) {
        co_return -1;
    }

    co_return static_cast<int>(*result);
}
```

### Result<T>

Alias for `std::expected<T, std::error_code>`. All operations return results rather than throwing.

```cpp
kio::Result<size_t> result = co_await kio::AsyncRead(ctx, fd, buffer);

if (!result) {
    // Handle error
    std::error_code ec = result.error();
    log_error("Read failed: {}", ec.message());
    co_return;
}

size_t bytes_read = *result;
```

### UringOp<Derived>

CRTP base class for all io_uring operations. Handles:
- Coroutine suspension/resumption
- Operation tracking
- SQE preparation
- Result extraction

---

## I/O Operations

### File Operations

| Operation | Description |
|-----------|-------------|
| `AsyncRead` | Read from file descriptor at offset |
| `AsyncWrite` | Write to file descriptor at offset |
| `AsyncReadv` | Scatter read (multiple buffers) |
| `AsyncWritev` | Gather write (multiple buffers) |
| `AsyncClose` | Close file descriptor |
| `AsyncFsync` | Flush data + metadata to disk |
| `AsyncFdatasync` | Flush data only (faster) |
| `AsyncFallocate` | Pre-allocate file space |
| `AsyncFtruncate` | Truncate/extend file |

### Network Operations

| Operation | Description |
|-----------|-------------|
| `AsyncAccept` | Accept incoming connection |
| `AsyncConnect` | Connect to remote address |
| `AsyncRecv` | Receive from socket |
| `AsyncSend` | Send to socket |
| `AsyncSendmsg` | Send with scatter-gather or ancillary data |
| `AsyncPoll` | Wait for fd events (POLLIN, POLLOUT, etc.) |

### Fixed File Operations

For high-frequency I/O, register files once and use indices:

```cpp
std::array fds = {fd1, fd2, fd3};
ctx.RegisterFiles(fds);

// Use index instead of fd - reduced kernel overhead
co_await kio::AsyncReadFixed(ctx, 0, buffer, offset);  // Reads from fd1
```

### Timing

```cpp
using namespace std::chrono_literals;

co_await kio::AsyncSleep(ctx, 100ms);  // Non-blocking sleep
```

### Timeouts

Any operation can have a timeout attached:

```cpp
auto result = co_await kio::AsyncRecv(ctx, socket, buffer)
    .WithTimeout(5s);

if (!result && result.error() == std::errc::timed_out) {
    // Operation timed out
}
```

---

## Networking

### Socket

RAII wrapper for socket file descriptors:

```cpp
#include <kio/net.hpp>

kio::net::Socket client(fd);

// Configure options
client.SetNonBlocking();
client.SetNodelay(true);      // Disable Nagle
client.SetReuseAddr(true);
client.SetReusePort(true);    // Load balancing

// Move semantics - no double close
kio::net::Socket other = std::move(client);
```

### SocketAddress

IPv4/IPv6 address wrapper:

```cpp
// Direct construction
auto addr_v4 = kio::net::SocketAddress::V4(8080, "127.0.0.1");
auto addr_v6 = kio::net::SocketAddress::V6(8080, "::1");
auto addr_any = kio::net::SocketAddress::V4(8080);  // 0.0.0.0

// Blocking DNS resolution (use at startup)
auto addr = kio::net::SocketAddress::Resolve("example.com", 443);

// Async DNS resolution (use in coroutines)
auto addr = co_await kio::net::SocketAddress::ResolveAsync(
    ctx, blocking_pool, "api.example.com", 443);
```

### TcpListener

Factory for server sockets with high-performance defaults:

```cpp
auto listener = kio::net::TcpListener::Bind(8080);
// Automatically sets: SO_REUSEADDR, SO_REUSEPORT, TCP_NODELAY, non-blocking

kio::net::Socket server = std::move(*listener);

while (running) {
    auto client_fd = co_await kio::AsyncAccept(ctx, server.Get());
    if (client_fd) {
        spawn_handler(*client_fd);
    }
}
```

---

## Helper Functions

Higher-level operations built on primitives.

### Exact Read/Write

Loop until exact byte count is transferred:

```cpp
#include <kio/io_helpers.hpp>

// Receive exactly N bytes (loops on partial reads)
co_await kio::AsyncRecvExact(ctx, socket, buffer);

// Send exactly N bytes
co_await kio::AsyncSendExact(ctx, socket, data);

// File I/O equivalents
co_await kio::AsyncReadExact(ctx, fd, buffer, offset);
co_await kio::AsyncWriteExact(ctx, fd, data, offset);
```

### Sendfile

Zero-copy file-to-socket transfer with automatic pipe pooling:

```cpp
int file_fd = open("large_file.bin", O_RDONLY);
struct stat st;
fstat(file_fd, &st);

// Efficient zero-copy transfer using splice
co_await kio::AsyncSendfile(ctx, client_socket, file_fd, 0, st.st_size);

close(file_fd);
```

```
┌──────────────────────────────────────────────────────────────┐
│                  AsyncSendfile Internals                     │
│                                                              │
│   ┌─────────┐     splice      ┌─────────┐     splice        │
│   │  File   │ ───────────────►│  Pipe   │────────────────►  │
│   │  (in)   │                 │ (pooled)│                   │
│   └─────────┘                 └─────────┘                   │
│                                    │                         │
│                                    ▼                         │
│                               ┌─────────┐                    │
│                               │ Socket  │                    │
│                               │  (out)  │                    │
│                               └─────────┘                    │
│                                                              │
│   * Pipes are reused from IoContext's pool                  │
│   * Zero-copy: data never enters userspace                  │
│   * Chunked: 64KB at a time for flow control                │
└──────────────────────────────────────────────────────────────┘
```

---

## Usage Examples

### Echo Server

```cpp
#include <kio/IoContext.hpp>
#include <kio/io.hpp>
#include <kio/net.hpp>

kio::Task<void> handle_client(kio::IoContext& ctx, int client_fd) {
    kio::net::Socket client(client_fd);
    std::array<std::byte, 4096> buffer;

    while (true) {
        auto recv_result = co_await kio::AsyncRecv(ctx, client.Get(), buffer);
        if (!recv_result || *recv_result == 0) {
            break;  // Error or connection closed
        }

        auto send_result = co_await kio::AsyncSendExact(
            ctx, client.Get(),
            std::span(buffer.data(), *recv_result)
        );
        if (!send_result) {
            break;
        }
    }
}

kio::Task<void> server(kio::IoContext& ctx) {
    auto listener = kio::net::TcpListener::Bind(8080);
    if (!listener) {
        co_return;
    }
    kio::net::Socket server = std::move(*listener);

    while (true) {
        auto client = co_await kio::AsyncAccept(ctx, server.Get());
        if (client) {
            // Spawn handler (detached coroutine)
            handle_client(ctx, *client);
        }
    }
}

int main() {
    kio::IoContext ctx;
    auto task = server(ctx);
    ctx.RunUntilDone(task);
}
```

### File Copy with Progress

```cpp
kio::Task<kio::Result<void>> copy_file(
    kio::IoContext& ctx,
    int src_fd, int dst_fd,
    size_t total_size,
    std::function<void(size_t)> on_progress
) {
    constexpr size_t kChunkSize = 64 * 1024;
    std::vector<std::byte> buffer(kChunkSize);

    size_t copied = 0;
    while (copied < total_size) {
        size_t to_read = std::min(kChunkSize, total_size - copied);

        auto read_result = co_await kio::AsyncRead(
            ctx, src_fd,
            std::span(buffer.data(), to_read),
            copied
        );
        if (!read_result || *read_result == 0) {
            co_return std::unexpected(read_result.error());
        }

        auto write_result = co_await kio::AsyncWriteExact(
            ctx, dst_fd,
            std::span(buffer.data(), *read_result),
            copied
        );
        if (!write_result) {
            co_return std::unexpected(write_result.error());
        }

        copied += *read_result;
        on_progress(copied);
    }

    co_await kio::AsyncFdatasync(ctx, dst_fd);
    co_return {};
}
```

### Concurrent Requests with Timeout

```cpp
kio::Task<std::vector<Response>> fetch_all(
    kio::IoContext& ctx,
    std::span<const std::string> urls
) {
    std::vector<kio::Task<Response>> tasks;

    for (const auto& url : urls) {
        tasks.push_back(fetch_with_timeout(ctx, url, 5s));
    }

    std::vector<Response> results;
    for (auto& task : tasks) {
        results.push_back(co_await task);
    }

    co_return results;
}
```

---

## Safety & Lifetime

### Buffer Lifetime

**Critical**: Buffers must remain valid until the operation completes.

```cpp
// CORRECT - buffer outlives operation
std::array<std::byte, 1024> buffer;
auto result = co_await kio::AsyncRead(ctx, fd, buffer);

// WRONG - undefined behavior!
auto op = kio::AsyncRead(ctx, fd, temp_buffer);
// temp_buffer destroyed here
co_await op;  // Buffer is gone - kernel writes to freed memory
```

### Operation Tracking

kio tracks all pending operations. If a coroutine is destroyed while I/O is pending, the program terminates rather than risking memory corruption.

```cpp
{
    auto task = read_file(ctx, fd);
    // task goes out of scope with pending I/O
}  // std::terminate() - intentional crash
```

**Safe cancellation patterns:**

1. Use timeouts so operations eventually complete
2. Let IoContext::CancelAllPending() drain on destruction
3. Don't destroy tasks with pending I/O - wait for completion

### Thread Safety

- One `IoContext` per thread (thread-per-core model)
- No cross-thread submission to the same context
- Use `BlockingPool` + `Offload()` for blocking work
- Use `MsgRingOp` for cross-context communication

---

## Multi-Threading Example

kio follows a **thread-per-core** model where each thread owns its own `IoContext`.
Here's one approach to building a multi-threaded server:

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Main Thread                                  │
│  - Creates listening socket with SO_REUSEPORT                       │
│  - Spawns worker threads                                            │
│  - Monitors stats / handles signals                                 │
│  - Initiates graceful shutdown                                      │
└─────────────────────────────────────────────────────────────────────┘
          │
          │ spawns N threads
          ▼
┌─────────────┐  ┌─────────────┐  ┌─────────────┐
│  Worker 0   │  │  Worker 1   │  │  Worker N   │
│ ┌─────────┐ │  │ ┌─────────┐ │  │ ┌─────────┐ │
│ │IoContext│ │  │ │IoContext│ │  │ │IoContext│ │
│ └────┬────┘ │  │ └────┬────┘ │  │ └────┬────┘ │
│      │      │  │      │      │  │      │      │
│ accept_loop │  │ accept_loop │  │ accept_loop │
│      │      │  │      │      │  │      │      │
│   ┌──┴──┐   │  │   ┌──┴──┐   │  │   ┌──┴──┐   │
│   │conns│   │  │   │conns│   │  │   │conns│   │
│   └─────┘   │  │   └─────┘   │  │   └─────┘   │
└─────────────┘  └─────────────┘  └─────────────┘
       ▲                ▲                ▲
       └────────────────┴────────────────┘
              All accept on same port
              (kernel load-balances via SO_REUSEPORT)
```

### Worker Structure

```cpp
struct Worker {
    std::unique_ptr<kio::IoContext> ctx;
    int ring_fd;  // For cross-thread wake-up
    std::thread thread;
};
```

### Creating Workers

```cpp
const int num_workers = std::thread::hardware_concurrency();
std::vector<Worker> workers;

// Create listening socket with SO_REUSEPORT
auto listener = kio::net::TcpListener::Bind(8080);
int listen_fd = listener->Release();  // Workers share this fd

for (int i = 0; i < num_workers; ++i) {
    Worker w;
    w.ctx = std::make_unique<kio::IoContext>(4096);
    w.ring_fd = w.ctx->RingFd();

    w.thread = std::thread([&w, listen_fd] {
        // Storage for active connections
        std::vector<kio::Task<void>> connections;
        connections.reserve(1024);

        // Start accept loop
        auto acceptor = accept_loop(*w.ctx, listen_fd, connections);
        acceptor.resume();

        // Run until stopped
        w.ctx->Run();

        // Cleanup
        w.ctx->CancelAllPending();
    });

    workers.push_back(std::move(w));
}
```

### Accept Loop Per Worker

```cpp
kio::Task<void> accept_loop(
    kio::IoContext& ctx,
    int listen_fd,
    std::vector<kio::Task<void>>& connections
) {
    while (g_running.load(std::memory_order_relaxed)) {
        auto result = co_await kio::AsyncAccept(ctx, listen_fd);
        if (!result) {
            if (result.error().value() == EBADF) break;  // Socket closed
            continue;
        }

        int client_fd = *result;

        // Spawn handler coroutine
        auto handler = handle_client(ctx, client_fd);
        handler.resume();
        connections.push_back(std::move(handler));

        // Periodic cleanup of completed connections
        if (connections.size() > 1000) {
            std::erase_if(connections, [](auto& t) { return t.done(); });
        }
    }
}
```

### Graceful Shutdown

To stop workers blocked in `io_uring_submit_and_wait()`, send a wake-up message:

```cpp
void shutdown_workers(std::vector<Worker>& workers) {
    // Create a temporary ring just for sending wake messages
    io_uring waker;
    io_uring_queue_init(64, &waker, 0);

    for (auto& w : workers) {
        // Signal the context to stop
        w.ctx->Stop();

        // Wake the ring in case it's blocked waiting for completions
        io_uring_sqe* sqe = io_uring_get_sqe(&waker);
        io_uring_prep_msg_ring(sqe, w.ring_fd, 0, kio::WAKE_TAG, 0);
    }

    io_uring_submit(&waker);
    io_uring_queue_exit(&waker);

    // Join all threads
    for (auto& w : workers) {
        if (w.thread.joinable()) {
            w.thread.join();
        }
    }
}
```

### Key Points

1. **One IoContext per thread** - No locks in the hot path
2. **SO_REUSEPORT** - Kernel distributes connections across workers
3. **Shared listen socket** - All workers accept from the same fd
4. **MSG_RING for wake-up** - Break out of blocked `submit_and_wait()`
5. **Local connection storage** - Each worker owns its coroutines
6. **Periodic cleanup** - Remove completed tasks to bound memory

### Alternative: Dedicated Acceptor

Another pattern uses a single acceptor thread that distributes connections:

```
┌──────────────┐     round-robin     ┌─────────────┐
│   Acceptor   │ ──────────────────► │  Worker 0   │
│   Thread     │                     ├─────────────┤
│              │ ──────────────────► │  Worker 1   │
│ AsyncAccept  │                     ├─────────────┤
│              │ ──────────────────► │  Worker N   │
└──────────────┘                     └─────────────┘
```

This requires cross-thread fd passing (via Unix socket + SCM_RIGHTS or a queue),
adding complexity but giving more control over load distribution.

---

## Performance Tips

1. **Register frequently-used files** with `IoContext::RegisterFiles()` to avoid fd lookup overhead

2. **Use vectored I/O** (`AsyncReadv`/`AsyncWritev`) to reduce syscall count

3. **Pre-allocate files** with `AsyncFallocate()` to avoid fragmentation

4. **Disable Nagle** with `SetNodelay()` for latency-sensitive protocols

5. **Use `AsyncFdatasync`** instead of `AsyncFsync` when metadata durability isn't needed

6. **Batch operations** - io_uring batches SQE submissions automatically

7. **Reuse buffers** - avoid allocation in the hot path

---

## License

[Your license here]
