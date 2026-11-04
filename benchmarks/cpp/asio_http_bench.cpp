#define BOOST_ERROR_CODE_HEADER_ONLY
#define BOOST_SYSTEM_NO_DEPRECATED

#include <boost/asio.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

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
    uint16_t port = 8081;
    size_t workers = 4;
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
        else
        {
            throw std::invalid_argument("unknown argument");
        }
    }
    return opts;
}

class Session : public std::enable_shared_from_this<Session>
{
    tcp::socket socket_;
    std::array<char, 4096> buffer_{};

public:
    explicit Session(tcp::socket socket) : socket_(std::move(socket)) {}

    void start() { do_read(); }

private:
    void do_read()
    {
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(buffer_),
                                [self](const boost::system::error_code& ec, std::size_t bytes)
                                {
                                    if (ec || bytes == 0)
                                    {
                                        return;
                                    }
                                    self->do_write();
                                });
    }

    void do_write()
    {
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(kHttpResponse.data(), kHttpResponse.size()),
                          [self](const boost::system::error_code& ec, std::size_t)
                          {
                              if (ec)
                              {
                                  return;
                              }
                              self->do_read();
                          });
    }
};

class Server
{
    asio::io_context& io_;
    tcp::acceptor acceptor_;

public:
    Server(asio::io_context& io, uint16_t port) : io_(io), acceptor_(io)
    {
        tcp::endpoint endpoint(tcp::v4(), port);
        acceptor_.open(endpoint.protocol());
        acceptor_.set_option(asio::socket_base::reuse_address(true));
        acceptor_.bind(endpoint);
        acceptor_.listen(4096);
    }

    void start() { do_accept(); }

private:
    void do_accept()
    {
        acceptor_.async_accept(
            asio::make_strand(io_),
            [this](const boost::system::error_code& ec, tcp::socket socket)
            {
                if (!ec)
                {
                    socket.set_option(tcp::no_delay(true));
                    std::make_shared<Session>(std::move(socket))->start();
                }
                if (!g_stop_requested.load(std::memory_order_relaxed))
                {
                    do_accept();
                }
            });
    }
};
}  // namespace

int main(int argc, char** argv)
{
    try
    {
        const Options opts = parse_args(argc, argv);
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        std::signal(SIGPIPE, SIG_IGN);

        asio::io_context io;
        Server server(io, opts.port);
        server.start();

        auto guard = asio::make_work_guard(io);
        std::vector<std::jthread> threads;
        threads.reserve(opts.workers);
        for (size_t i = 0; i < opts.workers; ++i)
        {
            threads.emplace_back([&] { io.run(); });
        }

        std::cout << "asio_http_bench listening on port " << opts.port << " workers=" << opts.workers << '\n';
        while (!g_stop_requested.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        guard.reset();
        io.stop();
    }
    catch (const std::exception& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
