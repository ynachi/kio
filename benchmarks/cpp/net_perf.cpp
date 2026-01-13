//
// KIO Echo Server - Simple benchmark target
//
// Usage:
//   ./kio_echo --port=8080 --workers=4
//
// Benchmark with standard tools:
//   wrk -t4 -c400 -d30s http://localhost:8080/
//   ab -n 100000 -c 400 http://localhost:8080/
//   hey -n 100000 -c 400 http://localhost:8080/
//

#include <csignal>
#include <gflags/gflags.h>
#include <iostream>

#include "kio/core/async_logger.h"
#include "kio/core/bytes_mut.h"
#include "kio/core/coro.h"
#include "kio/core/worker_pool.h"
#include "kio/net/net.h"

using namespace kio;
using namespace kio::io;

// Command-line flags
DEFINE_string(ip, "0.0.0.0", "listen ip address");
DEFINE_uint64(port, 8080, "port to listen on");
DEFINE_uint64(workers, 4, "number of worker threads");
DEFINE_uint64(backlog, 4096, "listen backlog");
DEFINE_uint64(uring_queue_depth, 16384, "io_uring queue depth");
DEFINE_uint64(op_slots, 8192, "operation slots");

using namespace kio;
using namespace kio::io;
// Static HTTP response - pre-formatted to avoid allocations
static constexpr std::string_view HTTP_RESPONSE =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 13\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, World!";

// HTTP request handler with BytesMut
DetachedTask handle_client(Worker& worker, const int client_fd)
{
    std::vector<char> buf(8192);
    const auto st = worker.GetStopToken();

    while (!st.stop_requested())
    {
        // Read data (maybe partial)
        auto n = co_await worker.AsyncRead(client_fd, buf);
        if (!n.has_value() || n.value() == 0)
        {
            break;  // Error or disconnect
        }

        const size_t read_size = n.value();

        std::string_view view(buf.data(), read_size);

        if (const auto pos = view.find("\r\n\r\n"); pos != std::string_view::npos)
        {
            // We have a complete request, send response
            // Use async_write_exact to ensure all bytes are sent (matching Photon behavior)

            if (auto sent = co_await worker.AsyncWriteExact(client_fd,
                                                            std::span(HTTP_RESPONSE.data(), HTTP_RESPONSE.size()));
                !sent.has_value())
            {
                break;
            }
        }

        // If the buffer gets too large without finding a complete request, disconnect
        if (buf.size() > 16384)
        {
            ALOG_WARN("Request too large, closing connection");
            break;
        }
    }

    close(client_fd);
}

// Accept loop
DetachedTask accept_loop(Worker& worker, const int listen_fd)
{
    const auto st = worker.GetStopToken();

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        auto client_fd = co_await worker.AsyncAccept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!client_fd.has_value())
        {
            if (!st.stop_requested())
            {
                ALOG_ERROR("Accept failed: {}", client_fd.error());
            }
            continue;
        }

        handle_client(worker, client_fd.value());
    }
}

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    signal(SIGPIPE, SIG_IGN);
    alog::Configure(8192, LogLevel::kDisabled);

    // Create a listening socket
    auto server_fd_exp = net::create_tcp_server_socket(FLAGS_ip, FLAGS_port, static_cast<int>(FLAGS_backlog));
    if (!server_fd_exp.has_value())
    {
        ALOG_ERROR("Failed to create server socket: {}", server_fd_exp.error());
        return 1;
    }

    auto server_fd = server_fd_exp.value();
    ALOG_INFO("Listening on {}:{}", FLAGS_ip, FLAGS_port);

    // Configure workers
    WorkerConfig config{};
    config.uring_queue_depth = FLAGS_uring_queue_depth;

    // Create a worker pool
    IOPool pool(FLAGS_workers, config, [server_fd](Worker& worker) { accept_loop(worker, server_fd); });

    ALOG_INFO("Server running with {} workers", FLAGS_workers);
    std::cout << "\nPress Enter to stop...\n";
    std::cin.get();

    pool.Stop();
    close(server_fd);
    ALOG_INFO("Server stopped");

    return 0;
}
