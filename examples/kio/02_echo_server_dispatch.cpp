#include "uring/context.h"

#include <chrono>
#include <csignal>
#include <future>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

#include "uring/io.hpp"
#include "uring/logger.hpp"
#include "uring/task.hpp"
#include "uring/tcp_listener.hpp"

using namespace URing;

constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 16\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, io_uring!";

std::stop_source global_stop_source;

void signal_handler(int)
{
    global_stop_source.request_stop();
}

DetachedTask handle_client(IoWorker& ctx, Fd client_fd)
{
    std::byte buf[1024];

    while (true)
    {
        auto read_res = co_await read(ctx, client_fd, std::span{buf});

        if (!read_res.has_value() || *read_res == 0)
            break;

        std::span<const std::byte> out_buf(reinterpret_cast<const std::byte*>(kHttpResponse.data()),
                                           kHttpResponse.size());

        auto write_res = co_await write(ctx, client_fd, out_buf);

        if (!write_res.has_value() || *write_res == 0)
            break;
    }
}

DetachedTask dispatcher_loop(IoWorker& dispatcher_ctx, std::vector<IoWorker*> workers, uint16_t port)
{
    auto listener = TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "[Dispatcher] Failed to bind: " << listener.error().value() << "\n";
        co_return;
    }

    std::cout << "[Dispatcher] Listening on http://0.0.0.0:" << port << " and routing to " << workers.size()
              << " workers.\n";

    Fd server_fd = std::move(*listener);
    size_t worker_idx = 0;

    while (!global_stop_source.stop_requested())
    {
        auto client_res = co_await accept(dispatcher_ctx, server_fd);

        if (client_res)
        {
            const size_t selected_worker = worker_idx % workers.size();
            IoWorker* target_worker = workers[selected_worker];
            worker_idx++;

            ALOG_DEBUG("Dispatching accepted client to worker {}", selected_worker);

            // Cross-thread spawn: the dispatcher owns the accepted fd, but the
            // handler coroutine is constructed and started on the worker ctx.
            try
            {
                if (!target_worker->spawn([fd = std::move(*client_res)](IoWorker& worker_ctx) mutable -> DetachedTask
                                          { return handle_client(worker_ctx, std::move(fd)); }))
                {
                    std::cerr << "[Dispatcher] Failed to dispatch client to worker\n";
                }
            }
            catch (const std::exception& e)
            {
                std::cerr << "[Dispatcher] Failed to dispatch client: " << e.what() << "\n";
            }
            catch (...)
            {
                std::cerr << "[Dispatcher] Failed to dispatch client: unknown error\n";
            }
        }
        else
        {
            std::cerr << "[Dispatcher] Accept failed: " << client_res.error().value() << "\n";
        }
    }
}

// Construct the IoContext ON the thread that will run it to satisfy SINGLE_ISSUER
void run_worker(std::promise<IoWorker*> init_promise, int id)
{
    try
    {
        // 1. Setup ring (Owner is now correctly this worker thread)
        IoWorker ctx{16384};

        // 2. Pass the pointer back to the main thread so the dispatcher can use it
        init_promise.set_value(&ctx);

        // 3. Block and process incoming cross-thread spawns and I/O
        ctx.run(global_stop_source.get_token());
        std::cout << "[Worker " << id << "] Shutdown.\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "[Worker " << id << "] Fatal error: " << e.what() << "\n";
        try
        {
            init_promise.set_exception(std::current_exception());
        }
        catch (...)
        {
        }
    }
}

int main()
{
    URing::ALOG::set_level(ALOG::Level::Debug);
    ALOG_DEBUG("Debug logging enabled");

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    constexpr int num_workers = 4;

    std::vector<std::thread> threads;
    std::vector<IoWorker*> worker_ptrs;

    // Start workers and wait for them to initialize their rings
    for (int i = 0; i < num_workers; ++i)
    {
        std::promise<IoWorker*> p;
        auto future = p.get_future();

        threads.emplace_back(run_worker, std::move(p), i + 1);

        // Block until the worker thread has safely constructed its IoContext
        worker_ptrs.push_back(future.get());
    }

    // Start the dispatcher on the main thread
    IoWorker dispatcher_ctx{4096};
    dispatcher_loop(dispatcher_ctx, worker_ptrs, 8080);

    std::cout << "Starting cross-thread spawn demo...\n";
    dispatcher_ctx.run(global_stop_source.get_token());

    for (auto& t : threads)
    {
        if (t.joinable())
            t.join();
    }

    return 0;
}
