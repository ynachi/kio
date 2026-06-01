#include "uring/core/fiber_io.hpp"
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
    size_t stack_size = 64 * 1024;
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
        else if (auto value = value_for("--stack-size"); !value.empty())
        {
            opts.stack_size = static_cast<size_t>(parse_u64(value));
        }
        else
        {
            throw std::invalid_argument("unknown argument");
        }
    }

    if (opts.workers == 0 || opts.stack_size == 0)
    {
        throw std::invalid_argument("workers and stack-size must be positive");
    }
    return opts;
}

URing::Result<void> write_all(URing::FiberIO& fio, URing::Fd& fd, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        iovec iov{.iov_base = const_cast<std::byte*>(data.data()), .iov_len = data.size()};
        FIBER_TRY(auto written, fio.writev(fd, std::span<const iovec>{&iov, 1}));
        if (written <= 0)
        {
            return URing::error_from_errc(std::errc::io_error);
        }
        data = data.subspan(static_cast<size_t>(written));
    }
    return {};
}

URing::Result<void> handle_client(URing::FiberIO& fio, int raw_fd)
{
    URing::Fd client_fd{raw_fd};
    std::byte buf[4096];
    const auto response = std::as_bytes(std::span{kHttpResponse.data(), kHttpResponse.size()});

    while (!g_stop_requested.load(std::memory_order_relaxed))
    {
        auto read_res = fio.read(client_fd, std::span{buf});
        if (!read_res || *read_res == 0)
        {
            return {};
        }

        auto write_res = write_all(fio, client_fd, response);
        if (!write_res)
        {
            return {};
        }
    }

    return {};
}

URing::Task<void> accept_loop(URing::IO& worker, uint16_t port, size_t stack_size, std::stop_token st)
{
    auto listener = URing::TcpListener::Bind(port, "0.0.0.0", 4096);
    if (!listener)
    {
        std::cerr << "bind failed on worker " << worker.id() << ": " << listener.error().message() << '\n';
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

        const int raw_fd = client_res->Release();
        worker.spawn_fiber([raw_fd](URing::FiberIO& fio) { return handle_client(fio, raw_fd); }, stack_size);
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

        URing::IoContext ctx(opts.workers);
        const auto st = ctx.stop_token();
        for (size_t i = 0; i < ctx.worker_count(); ++i)
        {
            ctx.worker(i).schedule(accept_loop(ctx.worker(i), opts.port, opts.stack_size, st));
        }

        std::cout << "kio_fiber_http_bench listening on port " << opts.port << " workers=" << opts.workers
                  << " stack_size=" << opts.stack_size << '\n';

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
}
