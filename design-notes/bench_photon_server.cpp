// Throwaway bench server: PhotonLibOS equivalent of kio's 16-byte HTTP
// responder. Same pattern: accept loop -> per-connection coroutine -> recv
// into 4K buffer -> send fixed response, keep-alive until peer closes.
// Usage: bench_photon_server <port>
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <string_view>

#include <photon/net/socket.h>
#include <photon/photon.h>

namespace
{
    constexpr std::string_view kResp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 16\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "Hello, io_uring!";

    int handle_conn(photon::net::ISocketStream* s)
    {
        // Photon's dispatcher passes through whatever accept() returned --
        // under connection churn that can be a null stream.
        if (!s) return -1;
        static std::atomic<int> seen{0};
        char buf[4096];
        for (;;)
        {
            ssize_t n = s->read(buf, sizeof buf);
            if (n <= 0)
            {
                if (seen.fetch_add(1) < 3)
                    std::fprintf(stderr, "recv returned %zd errno=%d\n", n, n < 0 ? errno : 0);
                return -1;
            }
            ssize_t w = s->write(kResp.data(), kResp.size());
            if (w <= 0)
            {
                if (seen.fetch_add(1) < 3)
                    std::fprintf(stderr, "send returned %zd errno=%d\n", w, w < 0 ? errno : 0);
                return -1;
            }
        }
    }
} // namespace

int main(int argc, char** argv)
{
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    // INIT_EVENT_DEFAULT tries io_uring first, epoll as fallback.
    #ifdef FORCE_URING_ENGINE
    const uint64_t engine = photon::INIT_EVENT_IOURING;
#else
    const uint64_t engine = photon::INIT_EVENT_DEFAULT;
#endif
    if (photon::init(engine, photon::INIT_IO_NONE) < 0) return 1;

    auto server = photon::net::new_tcp_socket_server();
    server->set_handler({&handle_conn});
    if (server->bind_v4localhost(static_cast<uint16_t>(port)) < 0 || server->listen() < 0)
    {
        std::fprintf(stderr, "bind/listen failed\n");
        return 1;
    }
    std::printf("photon listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);
    server->start_loop(true);
    photon::fini();
}
