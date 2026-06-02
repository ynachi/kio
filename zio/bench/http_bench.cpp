#include "../io_context.hpp"

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
#include <vector>

#include <unistd.h>

namespace
{
constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

struct Options
{
    std::uint16_t port = 8080;
    std::size_t   workers = 4;
    std::size_t   fibers = 8192;
    std::size_t   stack_size = 64 * 1024;
};

struct Fd
{
    int fd = -1;

    explicit Fd(int value) noexcept : fd(value) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    ~Fd()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

std::uint64_t parse_u64(std::string_view text)
{
    std::uint64_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last)
        throw std::invalid_argument("invalid integer argument");
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
                return {};
            return arg.substr(name.size() + 1);
        };

        if (auto value = value_for("--port"); !value.empty())
            opts.port = static_cast<std::uint16_t>(parse_u64(value));
        else if (auto value = value_for("--workers"); !value.empty())
            opts.workers = static_cast<std::size_t>(parse_u64(value));
        else if (auto value = value_for("--fibers"); !value.empty())
            opts.fibers = static_cast<std::size_t>(parse_u64(value));
        else if (auto value = value_for("--stack-size"); !value.empty())
            opts.stack_size = static_cast<std::size_t>(parse_u64(value));
        else
            throw std::invalid_argument("unknown argument");
    }

    if (opts.workers == 0 || opts.fibers == 0 || opts.stack_size == 0)
        throw std::invalid_argument("workers, fibers, and stack-size must be positive");
    return opts;
}

zio::Result<> send_all(zio::io_context& ctx, int fd, std::span<const std::byte> data)
{
    while (!data.empty())
    {
        ZIO_TRY(auto written, ctx.send(fd, data));
        if (written == 0)
            return zio::error_from_errc(std::errc::connection_reset);
        data = data.subspan(written);
    }

    return {};
}

void handle_client(zio::io_context& ctx, int raw_fd)
{
    Fd client{raw_fd};
    std::byte buf[4096];
    const auto response = std::as_bytes(std::span{kHttpResponse.data(), kHttpResponse.size()});

    for (;;)
    {
        auto read = ctx.recv(client.fd, std::span{buf});
        if (!read || *read == 0)
            return;

        auto sent = send_all(ctx, client.fd, response);
        if (!sent)
            return;
    }
}

void run_worker(std::uint16_t port, std::size_t fibers, std::size_t stack_size,
                std::atomic_size_t& ready_count)
{
    zio::io_context ctx{zio::io_context_options{
        .ring_entries = 4096,
        .ring_flags = 0,
        .fiber_count = fibers,
        .stack_size = stack_size,
        .ready_budget = 256,
    }};

    auto listener = zio::TcpListener::bind("0.0.0.0", port);
    if (!listener)
        throw std::runtime_error("bind failed: " + listener.error().message());

    auto spawn_res = ctx.spawn(
        [&ctx, listener = std::move(*listener)]() mutable
        {
            for (;;)
            {
                auto accepted = ctx.accept(listener.fd());
                if (!accepted)
                    continue;

                const int raw_fd = *accepted;
                auto client_spawn = ctx.spawn(
                    [&ctx, raw_fd]
                    {
                        handle_client(ctx, raw_fd);
                    });
                if (!client_spawn)
                    ::close(raw_fd);
            }
        });
    if (!spawn_res)
        throw std::runtime_error("failed to spawn accept fiber");

    ready_count.fetch_add(1, std::memory_order_release);
    auto run_res = ctx.run();
    if (!run_res)
        std::cerr << "zio worker failed: " << run_res.error().message() << '\n';
}
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options opts = parse_args(argc, argv);
        std::signal(SIGPIPE, SIG_IGN);

        std::atomic_size_t ready_count{0};
        std::vector<std::thread> workers;
        workers.reserve(opts.workers);

        for (std::size_t i = 0; i < opts.workers; ++i)
        {
            workers.emplace_back(run_worker, opts.port, opts.fibers, opts.stack_size, std::ref(ready_count));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ready_count.load(std::memory_order_acquire) != opts.workers &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (ready_count.load(std::memory_order_acquire) != opts.workers)
            throw std::runtime_error("not all workers became ready");

        std::cout << "zio_http_bench listening on port " << opts.port << " workers=" << opts.workers
                  << " fibers_per_worker=" << opts.fibers << " stack_size=" << opts.stack_size << std::endl;

        for (auto& worker : workers)
            worker.join();
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
