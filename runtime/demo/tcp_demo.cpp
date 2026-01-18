////////////////////////////////////////////////////////////////////////
// tcp_demo.cpp - High-performance HTTP Benchmark Server
//
// Updates:
// 1. Uses utilities/net.hpp for Socket RAII and setup
// 2. Clean shutdown and signal handling preserved
////////////////////////////////////////////////////////////////////////////////

#include <atomic>
#include <csignal>

#include "runtime/runtime.hpp"
#include "utilities/kio_logger.hpp"
#include "utilities/net.hpp"

using namespace uring;
using namespace uring::net;

// Global counters
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_connections{0};

// Fire-and-forget task
struct DetachedTask
{
    struct promise_type
    {
        DetachedTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };
};

static Task<> handleClientLogic(Executor& ex, Socket client_sock)
{
    // RAII: client_sock closes automatically when this task ends

    // Performance tuning
    client_sock.set_nodelay(true);
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
            auto read_res = co_await recv(ex, fd, buffer, sizeof(buffer));

            // Handle errors/close
            if (!read_res)
            {
                // If EAGAIN, we can retry, but usually recv handles that.
                // Just exit on real errors.
                break;
            }
            if (*read_res == 0)
                break;  // EOF

            g_requests.fetch_add(1, std::memory_order_relaxed);

            // Simple send loop
            size_t bytes_sent = 0;
            while (bytes_sent < response.size())
            {
                auto send_res =
                    co_await send(ex, fd, std::span(response.data() + bytes_sent, response.size() - bytes_sent));
                if (!send_res)
                    goto cleanup;
                bytes_sent += *send_res;
            }
        }
    }
    catch (...)
    {
    }

cleanup:
    co_return;  // Socket closes here via destructor
}

static DetachedTask launchClientHandler(Executor& ex, Socket sock)
{
    co_await handleClientLogic(ex, std::move(sock));
}

static Task<> acceptLoop(Executor& ex, int listen_fd)
{
    while (true)
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // We use the raw listen_fd here because it is shared
        auto res = co_await accept(ex, listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

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
                co_await timeout_ms(ex, 10);
                continue;
            }
            continue;
        }

        g_connections.fetch_add(1, std::memory_order_relaxed);

        // Wrap the raw FD in our RAII Socket immediately
        launchClientHandler(ex, Socket(*res));
    }
}

static std::atomic<bool> g_running{true};
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

        Runtime rt(config);

        Log::info("Binding port {}...", PORT);

        auto listener = TcpListener::bind(PORT);
        if (!listener)
        {
            Log::error("Failed to bind: {}", listener.error().message());
            return 1;
        }

        // Keep the raw FD for the workers (shared), but keep the object alive in main
        int listen_fd = listener->get();

        Log::info("Server started on port {} (Threads: {})", PORT, config.num_threads);

        rt.loop_forever(config.pin_threads);

        for (size_t i = 0; i < rt.size(); ++i)
        {
            auto& ctx = rt.thread(i);
            ctx.schedule(acceptLoop(ctx.executor(), listen_fd));
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