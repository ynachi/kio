// examples/integrated_file_server.cpp
// Real-world example: HTTP file server with task_group + blocking_pool
//
// Demonstrates:
// - task_group for connection management
// - blocking_pool for file I/O offload
// - move_only_function for clean captures
// - Proper shutdown integration with io_context

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "aio/blocking_pool.hpp"
#include "aio/net.hpp"
#include "aio/task_group.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;

// Helper: send complete string
aio::task<> send_full(aio::io_context& ctx, int fd, std::string_view data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto result = co_await aio::async_send(ctx, fd,
            data.substr(sent), 0);
        if (!result || *result == 0) break;
        sent += *result;
    }
}

// Helper: send byte buffer
aio::task<> send_bytes(aio::io_context& ctx, int fd,
                          std::span<const std::byte> data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto result = co_await aio::async_send(ctx, fd,
            data.subspan(sent), 0);
        if (!result || *result == 0) break;
        sent += *result;
    }
}


// =============================================================================
// Configuration
// =============================================================================

struct ServerConfig {
    uint16_t port = 8080;
    size_t io_threads = 4;
    size_t blocking_threads = 8;
    std::string document_root = "./public";
};

// =============================================================================
// File Service (uses blocking_pool)
// =============================================================================

struct FileContent {
    std::vector<std::byte> data;
    std::string content_type;
    bool found = false;
};

class FileService {
public:
    explicit FileService(aio::blocking_pool& pool, std::string root)
        : pool_(pool), root_(std::move(root)) {}

    // Async file read using blocking pool
    aio::task<FileContent> read_file(aio::io_context& ctx, std::string path) {
        // Move path into lambda - this is where move_only_function shines!
        auto content = co_await aio::offload(ctx, pool_,
            [root = root_, requested = std::move(path)]() -> FileContent {

                // Build full path
                fs::path full_path = fs::path(root) / requested;

                // Security: prevent path traversal
                auto canonical = fs::weakly_canonical(full_path);
                if (!canonical.string().starts_with(root)) {
                    return FileContent{};  // Not found
                }

                // Check if file exists
                if (!fs::exists(canonical) || !fs::is_regular_file(canonical)) {
                    return FileContent{};
                }

                // Read file (blocking I/O)
                std::ifstream file(canonical, std::ios::binary);
                if (!file) {
                    return FileContent{};
                }

                file.seekg(0, std::ios::end);
                size_t size = file.tellg();
                file.seekg(0, std::ios::beg);

                std::vector<std::byte> data(size);
                file.read(reinterpret_cast<char*>(data.data()), size);

                // Determine content type
                std::string content_type = "application/octet-stream";
                auto ext = canonical.extension();
                if (ext == ".html") content_type = "text/html";
                else if (ext == ".css") content_type = "text/css";
                else if (ext == ".js") content_type = "text/javascript";
                else if (ext == ".json") content_type = "application/json";
                else if (ext == ".png") content_type = "image/png";
                else if (ext == ".jpg" || ext == ".jpeg") content_type = "image/jpeg";

                return FileContent{
                    .data = std::move(data),
                    .content_type = std::move(content_type),
                    .found = true
                };
            });

        co_return content;
    }

private:
    aio::blocking_pool& pool_;
    std::string root_;
};

// =============================================================================
// HTTP Request Parser (simplified)
// =============================================================================

struct HttpRequest {
    std::string method;
    std::string path;
    bool keep_alive = true;

    static HttpRequest parse(std::span<const std::byte> data) {
        // Simplified parser
        std::string_view view{
            reinterpret_cast<const char*>(data.data()),
            data.size()
        };

        HttpRequest req;

        // Extract method and path
        auto space1 = view.find(' ');
        if (space1 == std::string_view::npos) return req;

        req.method = std::string(view.substr(0, space1));

        auto space2 = view.find(' ', space1 + 1);
        if (space2 == std::string_view::npos) return req;

        req.path = std::string(view.substr(space1 + 1, space2 - space1 - 1));

        // Check Connection header
        req.keep_alive = view.find("Connection: close") == std::string_view::npos;

        return req;
    }
};

// =============================================================================
// HTTP Response Builder
// =============================================================================

class HttpResponse {
public:
    static std::string build_200(const FileContent& file, bool keep_alive) {
        return std::format(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: {}\r\n"
            "Content-Length: {}\r\n"
            "Connection: {}\r\n"
            "\r\n",
            file.content_type,
            file.data.size(),
            keep_alive ? "keep-alive" : "close"
        );
    }

    static std::string build_404(bool keep_alive) {
        static constexpr std::string_view body =
            "<html><body><h1>404 Not Found</h1></body></html>";

        return std::format(
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: {}\r\n"
            "Connection: {}\r\n"
            "\r\n"
            "{}",
            body.size(),
            keep_alive ? "keep-alive" : "close",
            body
        );
    }
};

// =============================================================================
// Connection Handler (uses both task_group and blocking_pool)
// =============================================================================

aio::task<void> handle_connection(
    aio::io_context& ctx,
    int fd,
    FileService& files,
    std::atomic<uint64_t>& request_count) {

    std::array<std::byte, 8192> buffer;

    while (true) {
        // Read request
        auto recv_result = co_await aio::async_recv(ctx, fd, buffer, 0);
        if (!recv_result || *recv_result == 0) {
            break;  // Connection closed
        }

        // Parse request
        auto request = HttpRequest::parse(
            std::span{buffer}.subspan(0, *recv_result)
        );

        request_count.fetch_add(1, std::memory_order_relaxed);

        // Handle GET requests only
        if (request.method != "GET") {
            auto resp = HttpResponse::build_404(request.keep_alive);
            co_await send_full(ctx, fd, resp);
            if (!request.keep_alive) break;
            continue;
        }

        // Read file from disk (via blocking pool)
        auto file = co_await files.read_file(ctx, request.path);

        if (!file.found) {
            auto resp = HttpResponse::build_404(request.keep_alive);
            co_await send_full(ctx, fd, resp);
            if (!request.keep_alive) break;
            continue;
        }

        // Send response header
        auto header = HttpResponse::build_200(file, request.keep_alive);
        co_await send_full(ctx, fd, header);

        // Send file content
        co_await send_bytes(ctx, fd, file.data);

        if (!request.keep_alive) break;
    }

    co_await aio::async_close(ctx, fd);
}

// =============================================================================
// Accept Loop (uses task_group for connection management)
// =============================================================================

aio::task<void> accept_loop(
    aio::io_context& ctx,
    int listen_fd,
    FileService& files,
    std::atomic<bool>& running,
    std::atomic<uint64_t>& request_count) {

    // THIS IS THE KEY INTEGRATION:
    // task_group automatically manages connection lifetimes
    aio::task_group<void> connections{&ctx, 2048};
    connections.set_sweep_interval(256);  // Sweep every 256 accepts

    while (running.load(std::memory_order_relaxed)) {
        auto fd_result = co_await aio::async_accept(ctx, listen_fd);

        if (!fd_result) {
            int err = fd_result.error().value();
            if (err == EBADF || err == ECANCELED) break;

            // Backpressure on file descriptor exhaustion
            if (err == EMFILE || err == ENFILE) {
                co_await aio::async_sleep(ctx, 10ms);
            }
            continue;
        }

        // Spawn connection handler - task_group keeps it alive
        connections.spawn(
            handle_connection(ctx, *fd_result, files, request_count)
        );

        // Optional: manual sweep if too many connections
        if (connections.size() > 4096) {
            size_t removed = connections.sweep();
            if (removed > 0) {
                std::cerr << "Swept " << removed << " completed connections\n";
            }
        }
    }

    std::cerr << "Accept loop stopping, waiting for "
              << connections.active_count() << " active connections...\n";

    // Graceful shutdown: wait for all connections to complete
    co_await connections.join_all(ctx, 100ms);

    std::cerr << "All connections closed\n";
}

// =============================================================================
// Worker Thread
// =============================================================================

struct Worker {
    std::unique_ptr<aio::io_context> ctx;
    std::unique_ptr<FileService> files;
    std::thread thread;
};

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    ServerConfig config;

    if (argc >= 2) config.port = std::atoi(argv[1]);
    if (argc >= 3) config.io_threads = std::atoi(argv[2]);
    if (argc >= 4) config.blocking_threads = std::atoi(argv[3]);
    if (argc >= 5) config.document_root = argv[4];

    std::cerr << std::format(
        "File Server:\n"
        "  Port: {}\n"
        "  I/O threads: {}\n"
        "  Blocking threads: {}\n"
        "  Document root: {}\n",
        config.port, config.io_threads,
        config.blocking_threads, config.document_root
    );

    // Create listen socket
    auto addr = aio::net::SocketAddress::v4(8080, "127.0.0.1");
    auto listen_res = aio::net::TcpListener::bind(addr);
    if (!listen_res.has_value()) {
        std::cerr << "Failed to create listen socket\n";
        return 1;
    }

    auto listen_fd = std::move(listen_res.value()).get();

    // Shared blocking pool for all workers
    aio::blocking_pool file_pool{config.blocking_threads};

    std::atomic<bool> running{true};
    std::atomic<uint64_t> total_requests{0};

    // Create workers
    std::vector<Worker> workers;
    workers.reserve(config.io_threads);

    for (size_t i = 0; i < config.io_threads; ++i) {
        Worker w;
        w.ctx = std::make_unique<aio::io_context>(4096);
        w.files = std::make_unique<FileService>(file_pool, config.document_root);
        workers.push_back(std::move(w));
    }

    // Start worker threads
    for (auto& w : workers) {
        w.thread = std::thread([&w, listen_fd, &running, &total_requests] {
            auto accept_task = accept_loop(
                *w.ctx,
                listen_fd,
                *w.files,
                running,
                total_requests
            );

            accept_task.start();
            w.ctx->run();
            w.ctx->cancel_all_pending();
        });
    }

    // Stats reporter
    std::thread stats_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(1s);
            auto count = total_requests.exchange(0, std::memory_order_relaxed);
            if (count > 0) {
                std::cerr << std::format("[stats] requests/sec: {}\n", count);
            }
        }
    });

    // Wait for signal (Ctrl+C)
    std::cerr << "Press Ctrl+C to stop\n";
    std::this_thread::sleep_for(std::chrono::hours(24));  // Or use signal handler

    // Shutdown
    std::cerr << "Shutting down...\n";
    running.store(false, std::memory_order_relaxed);

    // Wake all workers
    aio::ring_waker waker;
    for (auto& w : workers) {
        w.ctx->stop();
        waker.wake(*w.ctx);
    }

    // Wait for workers
    for (auto& w : workers) {
        if (w.thread.joinable()) {
            w.thread.join();
        }
    }

    stats_thread.join();

    std::cerr << "Goodbye!\n";
    return 0;
}

// =============================================================================
// INTEGRATION BENEFITS
// =============================================================================
//
// task_group benefits:
// ✓ No manual vector management
// ✓ Automatic periodic sweeping
// ✓ Clean shutdown with join_all()
// ✓ Can't forget to keep task alive
//
// blocking_pool with move_only_function benefits:
// ✓ Natural file path captures (no shared_ptr!)
// ✓ Move large buffers efficiently
// ✓ Clean lambda syntax
// ✓ Type-safe captures
//
// Integration:
// ✓ offload() returns awaitable, works with task_group
// ✓ Exceptions propagate naturally through co_await
// ✓ io_context cancellation works with both
// ✓ Clean separation: I/O on io_context, blocking on pool
//
// Real-world pattern:
// 1. Accept connection (io_context)
// 2. Spawn handler (task_group)
// 3. Read request (io_context)
// 4. Load file (blocking_pool via offload)
// 5. Send response (io_context)
// 6. Repeat or close
// 7. Graceful shutdown (task_group::join_all)
//
// =============================================================================