// Throwaway bench server: PhotonLibOS HTTP (net::http stack), adapted from
// photon's own examples/perf/http/http-server.cpp. One instance per core,
// SO_REUSEPORT so the kernel spreads connections -- the same model as kio's
// 4-thread server. Run 4 instances with different --core values.
// Usage: bench_photon_http <port> <core>
#include <netinet/tcp.h>
#include <sys/fcntl.h>

#include <cstdio>
#include <string>

#include <photon/common/alog.h>
#include <photon/io/signal.h>
#include <photon/net/http/server.h>
#include <photon/net/socket.h>
#include <photon/thread/thread.h>
#include <photon/photon.h>

using namespace photon;

namespace
{
    constexpr uint32_t kBodySize = 16;

    class SimpleHandler final : public net::http::HTTPHandler
    {
    public:
        int handle_request(net::http::Request&, net::http::Response& resp, std::string_view) override
        {
            char body[kBodySize];
            for (auto& c : body) c = '0';
            resp.set_result(200);
            resp.headers.content_length(kBodySize);
            resp.headers.insert("Content-Type", "text/plain");
            auto w = resp.write(body, kBodySize);
            return w == kBodySize ? 0 : -1;
        }
    };
} // namespace

int main(int argc, char** argv)
{
    const int port = argc > 1 ? std::atoi(argv[1]) : 8080;
    const int core = argc > 2 ? std::atoi(argv[2]) : 0;
    set_log_output_level(ALOG_ERROR);
    if (photon::init(photon::INIT_EVENT_DEFAULT, photon::INIT_IO_NONE) < 0) return -1;
    DEFER(photon::fini());

    std::string data(kBodySize, '0');

    auto tcpserv = net::new_tcp_socket_server();
    tcpserv->setsockopt<int>(SOL_SOCKET, SO_REUSEADDR, 1);
    tcpserv->setsockopt<int>(SOL_SOCKET, SO_REUSEPORT, 1);
    if (tcpserv->bind_v4localhost(static_cast<uint16_t>(port)) < 0 || tcpserv->listen() < 0)
    {
        std::fprintf(stderr, "bind/listen failed on %d\n", port);
        return -1;
    }
    DEFER(delete tcpserv);
    auto http_srv = net::http::new_http_server();
    DEFER(delete http_srv);
    SimpleHandler handler;
    http_srv->add_handler(&handler);
    tcpserv->set_handler(http_srv->get_connection_handler());
    std::printf("photon-http instance on 127.0.0.1:%d (core %d)\n", port, core);
    std::fflush(stdout);
    tcpserv->start_loop();
    while (true) photon::thread_sleep(1);
}
