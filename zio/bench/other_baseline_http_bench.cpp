#include "../other_baseline.hpp.cpp"

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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
namespace io = iouring_coro;

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
    std::size_t workers = 4;
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
        else
            throw std::invalid_argument("unknown argument");
    }

    if (opts.workers == 0)
        throw std::invalid_argument("workers must be positive");
    return opts;
}

int make_reuseport_listener(std::uint16_t port)
{
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        throw std::runtime_error("socket failed");

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        int e = errno;
        ::close(fd);
        throw std::runtime_error("bind failed: " + std::generic_category().message(e));
    }
    if (::listen(fd, SOMAXCONN) != 0)
    {
        int e = errno;
        ::close(fd);
        throw std::runtime_error("listen failed: " + std::generic_category().message(e));
    }
    return fd;
}

void handle_client(int raw_fd)
{
    Fd client{raw_fd};
    std::byte buf[4096];
    const auto response = std::as_bytes(std::span{kHttpResponse.data(), kHttpResponse.size()});

    for (;;)
    {
        auto read = io::recv(client.fd, std::span{buf});
        if (!read || *read == 0)
            return;

        auto sent = io::send_all(client.fd, response);
        if (!sent)
            return;
    }
}

void run_worker(std::uint16_t port, std::atomic_size_t& ready_count)
{
    Fd listener{make_reuseport_listener(port)};
    io::scheduler sched{4096};

    sched.spawn(
        [&]
        {
            for (;;)
            {
                auto accepted = io::accept(listener.fd, nullptr, nullptr, SOCK_CLOEXEC);
                if (!accepted)
                    continue;

                const int raw_fd = *accepted;
                io::spawn([raw_fd] { handle_client(raw_fd); });
            }
        });

    ready_count.fetch_add(1, std::memory_order_release);
    sched.run();
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
            workers.emplace_back(run_worker, opts.port, std::ref(ready_count));

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (ready_count.load(std::memory_order_acquire) != opts.workers &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        if (ready_count.load(std::memory_order_acquire) != opts.workers)
            throw std::runtime_error("not all workers became ready");

        std::cout << "other_baseline_http_bench listening on port " << opts.port
                  << " workers=" << opts.workers << std::endl;

        for (auto& worker : workers)
            worker.join();
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}
