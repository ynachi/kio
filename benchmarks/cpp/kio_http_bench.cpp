#include "uring/core/task.hpp"
#include "uring/extention/io_pool.hpp"
#include "uring/logger.hpp"
#include "uring/tcp_listener.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
std::atomic_bool g_stop_requested{false};

constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

struct Options
{
    uint16_t port = 8080;
    size_t workers = 4;
    bool dispatch = true;
};

void signal_handler(int)
{
    g_stop_requested.store(true, std::memory_order_relaxed);
}

uint64_t parse_u64(std::string_view text)
{
    uint64_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last)
    {
        throw std::invalid_argument("invalid integer argument");
    }
    return value;
}

Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i)
    {
        std::string_view arg = argv[i];
        auto value_for = [&](std::string_view name) -> std::string_view
        {
            if (!arg.starts_with(name) || arg.size() <= name.size() || arg[name.size()] != '=')
            {
                return {};
            }
            return arg.substr(name.size() + 1);
        };

        if (auto value = value_for("--port"); !value.empty())
        {
            opts.port = static_cast<uint16_t>(parse_u64(value));
        }
        else if (auto value = value_for("--workers"); !value.empty())
        {
            opts.workers = static_cast<size_t>(parse_u64(value));
        }
        else if (arg == "--no-dispatch")
        {
            opts.dispatch = false;
        }
        else
        {
            throw std::invalid_argument("unknown argument");
        }
    }
    return opts;
}

URing::Task<void> write_all(URing::IO& io, URing::Fd& fd, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        auto write_res = co_await io.write(fd, data);
        if (!write_res)
        {
            co_return std::unexpected(write_res.error());
        }
        if (*write_res <= 0)
        {
            co_return std::unexpected(URing::error_from_errc(std::errc::io_error));
        }
        data = data.subspan(static_cast<size_t>(*write_res));
    }
    co_return {};
}

URing::Task<void> handle_client(URing::IO& worker, URing::Fd client_fd)
{
    std::byte buf[4096];
    const auto response = std::as_bytes(std::span{kHttpResponse.data(), kHttpResponse.size()});

    while (!g_stop_requested.load(std::memory_order_relaxed))
    {
        auto read_res = co_await worker.read(client_fd, std::span{buf});
        if (!read_res)
        {
            co_return {};
        }
        if (*read_res == 0)
        {
            co_return {};
        }

        auto write_res = co_await write_all(worker, client_fd, response);
        if (!write_res)
        {
            co_return {};
        }
    }
    co_return {};
}

URing::Task<void> dispatch_accept_loop(URing::IoContext& ctx, URing::IO& dispatcher, uint16_t port,
                                       std::stop_token st)
{
    auto listener = URing::TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "bind failed: " << listener.error().message() << '\n';
        co_return std::unexpected(listener.error());
    }

    URing::Fd server_fd = std::move(*listener);
    size_t next_worker = 1;
    while (!st.stop_requested())
    {
        auto client_res = co_await dispatcher.accept(server_fd);
        if (!client_res)
        {
            if (!st.stop_requested())
            {
                co_return std::unexpected(client_res.error());
            }
            co_return {};
        }

        const size_t worker_count = ctx.worker_count();
        const size_t target_idx = worker_count > 1 ? 1 + ((next_worker++ - 1) % (worker_count - 1)) : 0;
        URing::IO& target = ctx.worker(target_idx);
        target.schedule(handle_client(target, std::move(*client_res)));
    }
    co_return {};
}

URing::Task<void> reuseport_accept_loop(URing::IO& worker, uint16_t port, std::stop_token st)
{
    auto listener = URing::TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        co_return std::unexpected(listener.error());
    }

    URing::Fd server_fd = std::move(*listener);
    while (!st.stop_requested())
    {
        auto client_res = co_await worker.accept(server_fd);
        if (!client_res)
        {
            if (!st.stop_requested())
            {
                co_return std::unexpected(client_res.error());
            }
            co_return {};
        }

        worker.schedule(handle_client(worker, std::move(*client_res)));
    }
    co_return {};
}
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options opts = parse_args(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::signal(SIGPIPE, SIG_IGN);
        URing::ALOG::set_level(URing::ALOG::Level::Disabled);

        const size_t io_threads = opts.dispatch ? opts.workers + 1 : opts.workers;
        URing::IoContext ctx(io_threads);
        const auto st = ctx.stop_token();

        if (opts.dispatch)
        {
            ctx.worker(0).schedule(dispatch_accept_loop(ctx, ctx.worker(0), opts.port, st));
        }
        else
        {
            for (size_t i = 0; i < ctx.worker_count(); ++i)
            {
                ctx.worker(i).schedule(reuseport_accept_loop(ctx.worker(i), opts.port, st));
            }
        }

        std::cout << "kio_http_bench listening on port " << opts.port << " workers=" << opts.workers
                  << " dispatch=" << (opts.dispatch ? "true" : "false") << '\n';
        while (!g_stop_requested.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        ctx.join();
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
