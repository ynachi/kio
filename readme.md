kio: A Modern, High-Performance C++ I/O Library

kio is a header-only C++23 library designed for building fast and scalable network and file I/O applications on Linux.
It leverages the power of the Linux io_uring interface and C++20/23 coroutines to provide a clean, modern, and highly
efficient asynchronous programming model.

The library follows a thread-per-core, share-nothing architecture to minimize contention and maximize performance,
making it an ideal foundation for servers, clients, and high-throughput data processing applications.

Core Features

Linux io_uring Backend: Uses the latest high-performance asynchronous I/O interface for maximum efficiency and minimal
system call overhead.

C++23 Coroutines (kio::Task): Write complex asynchronous logic in a simple, sequential style without callbacks ("
callback hell").

High-Performance Architecture: Built on a thread-per-core model with io_uring's SINGLE_ISSUER flag to eliminate locking
on the hot path.

Type-Safe, Modern API: Uses modern C++ features like std::expected, std::span, and std::chrono for a safe and expressive
API.

Robust Error Handling: Propagates detailed errors without exceptions using std::expected and a convenient KIO_TRY macro.

Built-in Timer Support: Includes async_sleep for handling timeouts, retries, and scheduled tasks.

CMake Integration: Simple to build and integrate into your own projects using modern CMake (FetchContent).

Requirements

Linux Kernel: Version 6.0 or higher is required for the necessary io_uring features.

C++ Compiler: A C++23 compatible compiler (e.g., GCC 12+, Clang 16+).

CMake: Version 3.22 or higher.

liburing: The liburing development library must be installed.

On Debian/Ubuntu: sudo apt-get install liburing-dev

On Fedora/CentOS: sudo dnf install liburing-devel

Building the Project

You can build the library, demos, and tests using standard CMake commands.

# 1. Clone the repository

git clone <repository_url>
cd kio

# 2. Configure the project using CMake

# This creates a 'build' directory with all the build files.

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 3. Build all targets

cmake --build build

# 4. (Optional) Run tests to verify the build

cd build && ctest && cd ..

The compiled demo applications will be located in the build/demo/ directory.

Core Concepts

The library is built around a few key abstractions:

kio::io::Worker: A single-threaded event loop that owns an io_uring instance. All I/O operations for a given file
descriptor or connection are "stuck" to a single worker to ensure thread safety.

kio::io::IOPool: Manages a pool of Worker threads, typically one for each available CPU core. It is the main entry point
for the I/O engine.

kio::Task<T>: The coroutine return type. A Task is a lazy, awaitable object that represents an asynchronous operation
that will eventually produce a value of type T.

co_await kio::io::SwitchToWorker(worker): The crucial mechanism for transferring the execution of a coroutine from an
external thread (like main) onto the correct Worker's thread before submitting I/O. This is mandatory to prevent data
races.

Usage Example: A Simple TCP Echo Server

This example demonstrates the core concepts working together to create a simple, multi-threaded TCP echo server.

#include "core/include/io_pool.h"
#include "core/include/net.h"
#include "core/include/sync_wait.h"
#include "spdlog/spdlog.h"
#include <iostream>

using namespace kio;
using namespace kio::io;
using namespace kio::net;

// Coroutine to handle a single client connection.
DetachedTask HandleClient(Worker& worker, int client_fd) {
// This task is already on the worker thread, so no switch is needed.
char buffer[8192];

    while (true) {
        // 1. Await an async read. The coroutine suspends here.
        auto read_result = co_await worker.async_read(client_fd, buffer, 0);

        if (!read_result || *read_result == 0) {
            // Error or client disconnected
            break; 
        }

        // 2. Await an async write (echo). The coroutine suspends again.
        co_await worker.async_write(client_fd, std::span(buffer, *read_result), 0);
    }
    close(client_fd);

}

// Coroutine that accepts new connections on a worker.
DetachedTask accept_loop(Worker& worker, int listen_fd) {
spdlog::info("Worker {} is accepting connections.", worker.get_id());
while (true) {
auto accept_result = co_await worker.async_accept(listen_fd, nullptr, nullptr);

        // Use KIO_TRY for clean error handling. If accept fails, it will log
        // and this coroutine will end, but the worker continues running.
        int client_fd = KIO_TRY(accept_result);

        // Spawn a new, detached coroutine to handle this client.
        HandleClient(worker, client_fd).detach();
    }

}

int main() {
spdlog::set_level(spdlog::level::info);

    // 1. Create a listening TCP socket.
    auto server_fd = create_tcp_socket("127.0.0.1", 8080, 128);
    if (!server_fd) {
        spdlog::error("Failed to create server socket: {}", server_fd.error().message());
        return 1;
    }
    
    // 2. Create an IOPool with a worker for each hardware thread.
    unsigned int num_threads = std::thread::hardware_concurrency();
    IOPool pool(num_threads);

    // 3. Start the accept loop on each worker.
    for (unsigned int i = 0; i < num_threads; ++i) {
        Worker* worker = pool.get_worker(i);
        // Schedule a task to run on the worker. SwitchToWorker ensures
        // the accept_loop runs on the correct thread.
        kio::SyncWait(accept_loop(*worker, *server_fd));
    }

    spdlog::info("Echo server listening on 127.0.0.1:8080 with {} workers.", num_threads);
    spdlog::info("Press Ctrl+C to stop.");

    // The main thread can now sleep or handle signals.
    // The IOPool destructor will automatically stop and join all worker threads.
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point::max());

    return 0;

}

Error Handling with KIO_TRY

The library uses std::expected<T, Error> to propagate errors without exceptions. The KIO_TRY macro simplifies checking
and returning errors.

Task<std::expected<size_t, Error>> read_file_safely(FileManager& fm) {
// KIO_TRY(expr) will:
// 1. If 'expr' is successful, unwrap the value.
// 2. If 'expr' is an error, immediately co_return that error from this function.

    auto file = KIO_TRY(co_await fm.async_open("my_file.txt", O_RDONLY, 0));
    
    std::vector<char> buffer(1024);
    size_t bytes_read = KIO_TRY(co_await file.async_read(buffer, 0));
    
    co_return bytes_read; // Success

}

Using Timers with async_sleep

You can suspend a coroutine for a specific duration, which is essential for timeouts, retries, and scheduled tasks.

Task<void> delayed_task(Worker& worker) {
co_await SwitchToWorker(worker); // Ensure we are on the worker thread

    spdlog::info("Starting a task...");
    co_await worker.async_sleep(std::chrono::seconds(5));
    spdlog::info("...5 seconds have passed. Task finished.");

}

License

This project is licensed under the MIT License. See the LICENCE file for details.