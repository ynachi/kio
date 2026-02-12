// 04_ctx_switch_benchmark.cpp
//
// Context Switch Benchmark:
// 1. Starts N workers (threads).
// 2. Accepts connections on any worker (SO_REUSEPORT).
// 3. For EVERY request, migrates the task to a *different* worker via Schedule().
// 4. Sends the response from the new worker.
//
// This isolates the cost of cross-thread scheduling (MSG_RING) compared to
// the locality-optimized 03_http_services.cpp.

#include "kio/kio.hpp"

#include <array>
#include <chrono>
#include <latch>
#include <vector>

#include <gflags/gflags.h>

DEFINE_string(host, "0.0.0.0", "Server host");
DEFINE_uint32(port, 8081, "Server port");
DEFINE_uint32(cores, 4, "Number of worker cores");

using namespace std::chrono_literals;

namespace
{
    // Global registry of contexts to allow workers to find each other
    std::vector<kio::IoContext*> g_contexts;

    kio::Task<> HandleHttp(kio::IoContext& /*accept_ctx*/, kio::net::Socket sock, const std::stop_token st)
    {
        std::array<std::byte, 1024> buf{};

        // We use IoContext::Current() because we might migrate threads,
        // so the reference passed in 'accept_ctx' will become stale (wrong thread).

        while (!st.stop_requested())
        {
            auto* current_ctx = kio::IoContext::Current();

            // 1. Read Request (on current thread)
            auto recv_res = co_await kio::AsyncRecv(*current_ctx, sock, buf);

            if (!recv_res.has_value() || recv_res.value() == 0)
            {
                break;
            }

            // 2. FORCE CONTEXT SWITCH
            // Pick the next worker in a round-robin fashion relative to the current one.
            // This ensures we ALWAYS pay the cost of a migration.
            if (g_contexts.size() > 1)
            {
                // Simple heuristic to pick a different target.
                // In a real app, this might be hash(request_key) % cores.
                // Here we just pick a "next" context pointer based on current thread ID hash or similar?
                // Actually, since we don't know our index easily, let's just pick random/next.
                // For simplicity in this demo, we'll just pick a target based on the socket FD
                // to be deterministic per connection, but likely different from accept thread.

                size_t target_idx = sock.Get() % g_contexts.size();
                kio::IoContext* target = g_contexts[target_idx];

                // If we happened to pick the same one, force move to the next (if enough cores)
                if (target == current_ctx)
                {
                    target_idx = (target_idx + 1) % g_contexts.size();
                    target = g_contexts[target_idx];
                }

                // *** THE BENCHMARK OPERATION ***
                co_await target->Schedule();
            }

            // 3. We are now on the Target Thread
            current_ctx = kio::IoContext::Current(); // Must update pointer!

            // Prepare simple response
            std::string_view body = "Hello from Context Switch!";
            std::string resp = std::format(
                "HTTP/1.1 200 OK\r\n"
                "Content-Length: {}\r\n"
                "Connection: keep-alive\r\n"
                "\r\n{}",
                body.size(), body);

            // 4. Send Response (from the new thread)
            // Note: writing to the socket from a different thread is safe
            // as long as the socket isn't being used concurrently by the old thread.
            // Since we awaited Schedule(), the old thread is done with this coroutine.
            auto send_res = co_await kio::AsyncSend(*current_ctx, sock, resp);
            if (!send_res)
            {
                break;
            }
        }
    }

    kio::Task<> Server(kio::IoContext& ctx, const std::string& host, uint16_t port, std::stop_token st)
    {
        auto bind_res = kio::net::TcpListener::BindV4(port, host);
        if (!bind_res)
        {
            // std::println(stderr, "Bind failed: {}", bind_res.error().message());
            co_return;
        }

        const auto listener = std::move(bind_res.value());
        kio::TaskGroup tasks;

        while (!st.stop_requested())
        {
            auto accept_res = co_await kio::AsyncAccept(ctx, listener).WithTimeout(1s);

            if (!accept_res)
            {
                continue;
            }

            auto socket = kio::net::Socket(accept_res.value().fd);
            // Spawn the handler. It starts on 'ctx' but may migrate.
            tasks.Spawn(HandleHttp(ctx, std::move(socket), st));
        }

        co_await tasks.JoinAll(ctx);
    }
}

int main(int argc, char* argv[])
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    kio::alog::g_level = kio::alog::Level::Warn; // Keep logs quiet for benchmarking

    std::stop_source ss;
    std::stop_token st = ss.get_token();

    const size_t cores = FLAGS_cores;
    g_contexts.resize(cores);

    // Barrier to ensure all contexts are created before any server starts accepting
    std::latch init_latch(cores);

    // std::println("Starting Context Switch Benchmark on port {} with {} cores...", FLAGS_port, cores);
    // std::println("  - Each request will force a thread migration via MSG_RING.");

    std::vector<kio::Worker> workers;
    for (size_t i = 0; i < cores; ++i)
    {
        workers.emplace_back(i);
        workers.back().Start(
            [&, i](kio::IoContext& ctx)
            {
                // Register context globally
                g_contexts[i] = &ctx;
                init_latch.count_down();
                init_latch.wait(); // Wait for all peers

                ctx.RunUntilDone(Server(ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port), st));
            },
            static_cast<int>(i));
    }

    // Main thread Wait
    kio::IoContext main_ctx;
    kio::SignalSet signals{SIGINT, SIGTERM};
    main_ctx.RunUntilDone([&]() -> kio::Task<>
    {
        co_await kio::AsyncWaitSignal(main_ctx, signals);
        // std::println("\nStopping...");
        ss.request_stop();
    }());

    for (auto& w : workers) w.Join();
    return 0;
}
