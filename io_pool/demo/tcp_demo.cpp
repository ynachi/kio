////////////////////////////////////////////////////////////////////////////////
// tcp_demo.cpp - High-performance HTTP Benchmark Server
//
// Migration Notes:
// 1. Removed DetachedTask (replaced with ctx.schedule(Task<>)).
// 2. Updated all IO calls to use ThreadContext&.
// 3. Updated acceptLoop to schedule client handlers directly.
////////////////////////////////////////////////////////////////////////////////

#include <atomic>
#include <csignal>
#include <span>
#include <string>
#include <thread>
#include <vector>

// Assumes these files are in the same directory
#include "io_pool/io.hpp"
#include "io_pool/runtime.hpp"
#include "kio_logger.hpp"
#include "net.hpp"

using namespace uring;
using namespace uring::io;
using namespace uring::net;

// Global counters
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_connections{0};

// --- Client Handling Logic ---

static Task<> handleClientLogic(ThreadContext& ctx, Socket client_sock)
{
    // RAII: client_sock closes automatically when this task ends

    // Performance tuning
    (void)client_sock.set_nodelay(true);
    // client_sock.set_recv_buffer(256 * 1024); // Optional tuning

    int fd = client_sock.get();
    char buffer[4096];

    static const std::string response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, World!";

    try
    {
        while (true)
        {
            // Note: recv takes ThreadContext&
            auto read_res = co_await recv(ctx, fd, std::span<char>(buffer));

            // Handle errors/close
            if (!read_res)
            {
                break;
            }
            if (*read_res == 0)
                break;  // EOF

            g_requests.fetch_add(1, std::memory_order_relaxed);

            // Simple send loop
            size_t bytes_sent = 0;
            while (bytes_sent < response.size())
            {
                // Construct span for the remaining part of the response
                std::span<const char> chunk(response.data() + bytes_sent, response.size() - bytes_sent);

                auto send_res = co_await send(ctx, fd, chunk);
                if (!send_res)
                    goto cleanup;
                bytes_sent += *send_res;
            }
        }
    }
    catch (...)
    {
        // Swallow exceptions to keep the server alive
    }

cleanup:
    co_return;  // Socket closes here via destructor
}

// --- Accept Loop ---

static Task<> acceptLoop(ThreadContext& ctx, int listen_fd)
{
    while (true)
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept takes ThreadContext&
        auto res = co_await accept(ctx, listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!res)
        {
            // Check for cancellation (shutdown)
            if (res.error() == std::errc::operation_canceled || res.error() == std::errc::bad_file_descriptor)
            {
                break;
            }
            // Backoff on file limit (EMFILE)
            if (res.error() == std::errc::too_many_files_open)
            {
                co_await timeout_ms(ctx, 10);
                continue;
            }
            continue;
        }

        g_connections.fetch_add(1, std::memory_order_relaxed);

        // MIGRATION: Instead of calling a detached coroutine wrapper,
        // we schedule the Task<> directly onto the current ThreadContext.
        // The Runtime takes ownership of the Task and ensures it runs.
        ctx.schedule(handleClientLogic(ctx, Socket(*res)));
    }
}

// --- Main / Signal Handling ---

static std::atomic g_running{true};
static void signal_handler(int)
{
    g_running = false;
}

int main()
{
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try
    {
        constexpr uint16_t PORT = 8080;

        RuntimeConfig config;
        config.num_threads = 4;
        config.pin_threads = true;
        // config.sq_thread_idle_ms = 10; // Optional: Enable SQPOLL

        Runtime rt(config);

        Log::info("Binding port {}...", PORT);

        auto listener = TcpListener::bind(PORT);
        if (!listener)
        {
            Log::error("Failed to bind: {}", listener.error().message());
            return 1;
        }

        // Keep the raw FD for the workers (shared), but keep the RAII object alive in main
        int listen_fd = listener->get();

        Log::info("Server started on port {} (Threads: {})", PORT, config.num_threads);

        rt.loop_forever(config.pin_threads);

        // Distribute the accept loop across all threads
        for (size_t i = 0; i < rt.size(); ++i)
        {
            auto& ctx = rt.thread(i);
            // MIGRATION: Pass 'ctx' directly, not 'ctx.executor()'
            ctx.schedule(acceptLoop(ctx, listen_fd));
        }

        // Monitor loop
        while (g_running)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            uint64_t conns = g_connections.exchange(0, std::memory_order_relaxed);
            uint64_t reqs = g_requests.exchange(0, std::memory_order_relaxed);
            if (conns > 0 || reqs > 0)
            {
                Log::info("[Stats] New Conns: {}, Reqs: {}", conns, reqs);
            }
        }

        Log::info("Shutting down...");

        // Clean shutdown: Close socket to cancel accepts
        listener->close();

        // Give threads a moment to wake up from accept() cancellation
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        rt.stop();
        Log::info("Bye.");
    }
    catch (const std::exception& e)
    {
        Log::error("Fatal: {}", e.what());
        return 1;
    }
}