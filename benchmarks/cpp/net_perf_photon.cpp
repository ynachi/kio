#include <gflags/gflags.h>
#include <photon/common/alog.h>
#include <photon/net/socket.h>
#include <photon/thread/std-compat.h>
#include <photon/thread/workerpool.h>

// Command-line flags
DEFINE_string(ip, "0.0.0.0", "IP address to bind to");
DEFINE_uint64(port, 8080, "Port to listen on");
DEFINE_uint64(workers, 4, "Number of worker threads");
DEFINE_uint64(backlog, 4096, "Listen backlog");
DEFINE_uint64(buf_size, 8192, "Buffer size for reading");

static constexpr std::string_view HTTP_RESPONSE =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

void handle_connection(photon::net::ISocketStream* stream)
{
    int cpu = sched_getcpu();
    LOG_INFO("Handling connection from CPU %d", cpu);
    DEFER(delete stream);

    std::vector<char> buf(FLAGS_buf_size);

    while (true)
    {
        ssize_t ret = stream->recv(buf.data(), buf.size());
        if (ret == 0)
        {
            LOG_INFO("Connection closed");
            return;
        }
        if (ret < 0)
        {
            LOG_ERROR("Failed to read socket");
            return;
        }

        std::string_view view(buf.data(), ret);

        if (const auto pos = view.find("\r\n\r\n"); pos != std::string_view::npos)
        {
            stream->send(HTTP_RESPONSE.data(), HTTP_RESPONSE.size());
        }

        // If the buffer gets too large without finding a complete request, disconnect
        if (buf.size() > 16384)
        {
            LOG_WARN("Request too large, closing connection");
            break;
        }

        LOG_INFO("Received and sent back ", ret, " bytes.");
        photon::thread_yield();
    }
}

int main(int argc, char** argv)
{
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    set_log_output_level(ALOG_FATAL);

    if (const int ret = photon::init(photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE); ret != 0)
    {
        LOG_ERRNO_RETURN(0, -1, "failed to initialize io event loop");
    }
    DEFER(photon::fini());

    if (auto ret = photon_std::work_pool_init(FLAGS_workers, photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE);
        ret != 0)
    {
        LOG_FATAL("Work-pool init failed");
        abort();
    }
    DEFER(photon_std::work_pool_fini());

    const auto server = photon::net::new_tcp_socket_server();
    if (server == nullptr)
    {
        LOG_ERRNO_RETURN(0, -1, "failed to create tcp server");
    }
    DEFER(delete server);

    // Set SO_REUSEADDR and SO_REUSEPORT
    server->setsockopt<int>(SOL_SOCKET, SO_REUSEADDR, 1);
    server->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1);

    if (const photon::net::IPAddr addr{FLAGS_ip.c_str()}; server->bind(FLAGS_port, addr) < 0)
    {
        LOG_ERRNO_RETURN(0, -1, "failed to bind server to port `", FLAGS_port);
    }
    LOG_INFO("Server bound to ", server->getsockname());

    if (server->listen(FLAGS_backlog) != 0)
    {
        LOG_ERRNO_RETURN(0, -1, "failed to listen");
    }
    LOG_INFO("Server listening on port ` with ` workers", FLAGS_port, FLAGS_workers);

    photon::WorkPool wp(FLAGS_workers, photon::INIT_EVENT_IOURING, photon::INIT_IO_NONE, 128 * 1024);
    while (true)
    {
        auto* stream = server->accept();
        if (stream == nullptr)
        {
            LOG_ERRNO_RETURN(0, -1, "failed to accept connection");
        }

        // Add connection handling tasks to the work pool
        wp.async_call(new auto([con = stream] { handle_connection(con); }));
    }
}
