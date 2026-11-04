// SPDX-License-Identifier: Apache-2.0
//
// Seastar counterpart to kio's benchmarks/cpp/kio_http_bench.cpp.
//
// The point of this file is to be as close to a *diff* against the kio
// benchmark as possible, so that what the numbers measure is the two event
// loops and not the surrounding scaffolding. Concretely, it deliberately does
// NOT use seastar::http::server:
//
//   kio_http_bench does not parse HTTP either. It accepts, reads once into a
//   4096-byte buffer, writes one fixed response, and loops. That makes it a
//   raw request/response echo with a fixed payload, which is exactly the
//   comparison we want between an io_uring coroutine runtime and Seastar's
//   reactor -- HTTP parsing would only add noise, and it would add *different*
//   noise to each side.
//
// Everything that could bias the comparison one way is called out at the end
// of this file; read it before quoting a number.

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/future.hh>
#include <seastar/core/iostream.hh>
#include <seastar/net/api.hh>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

using namespace seastar;

namespace {

// Byte-for-byte identical to kHttpResponse in kio_http_bench.cpp. If you change
// one, change the other -- an unequal response size silently changes how many
// bytes the load generator has to push and pull, which changes the result.
//
// This is a string_view, NOT a const char*. With a pointer, sizeof() yields the
// pointer width (8) rather than the payload length, and the benchmark happily
// ships a 7-byte response while looking like it works.
constexpr std::string_view kHttpResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: keep-alive\r\n"
    "\r\n"
    "Hello, World!";

// One accepted connection. Deliberately leaked (never freed) the way Seastar's
// own demos leak connection objects: the object lives until the process exits.
// A malloc/free per connection would otherwise show up in the accept path,
// which is the thing under test.
class connection {
    connected_socket cs_;
    input_stream<char> in_;
    output_stream<char> out_;

public:
    explicit connection(connected_socket cs)
        : cs_(std::move(cs))
        , in_(cs_.input())
        // kio writes straight to the fd. Seastar's output_stream buffers, so
        // this is the closest it gets; a small buffer keeps the handicap from
        // being larger than it needs to be. See FAIRNESS NOTES below.
        , out_(cs_.output(512)) {
        // kio_http_bench does not set TCP_NODELAY, so neither do we -- setting
        // it on one side and not the other would be a silent, huge bias.
    }

    void run() {
        in_.read_up_to(4096)
            .then([this](temporary_buffer<char>) {
                if (in_.eof()) {
                    return make_ready_future<>();
                }
                // kio ignores how much it read and always writes the whole
                // canned response; match that rather than being cleverer.
                return out_.write(kHttpResponse.data(), kHttpResponse.size()).then([this] { return out_.flush(); });
            })
            .then([this] { run(); })
            .handle_exception([](std::exception_ptr) {
                // Connection reset / peer closed. kio's handle_client also treats
                // any read error as "this connection is finished".
            });
    }
};

// Classic Seastar accept loop: accept, start the connection, accept again.
// Seastar migrates each new socket to whichever shard owns it, so this is the
// equivalent of kio's --dispatch mode (one accepting thread handing connections
// to workers). The listener is threaded through by value so the recursive call
// keeps it alive.
future<> accept_loop(server_socket listener) {
    return listener.accept().then([l = std::move(listener)](accept_result ar) mutable {
        (new connection(std::move(ar.connection)))->run();
        return accept_loop(std::move(l));
    });
}

} // anonymous namespace

int main(int ac, char** av) {
    // NOTE: the port is read from the environment, not argv. app_template
    // parses argv itself with boost::program_options and hard-errors on any
    // option it does not recognise, so passing --port here would kill the
    // process before main's body ever ran. BENCH_PORT keeps the CLI free for
    // Seastar's own flags (-c, --overprovisioned, --reactor-backend, ...),
    // which is exactly what run_kio_vs_seastar.sh needs to drive.
    uint16_t port = 8080;
    if (const char* p = std::getenv("BENCH_PORT")) {
        port = static_cast<uint16_t>(std::atoi(p));
    }

    app_template app;

    return app.run(ac, av, [port] {
        listen_options lo;
        lo.reuse_address = true;
        lo.listen_backlog = 4096;  // matches kio's TcpListener::Bind(port, ..., 4096)

        // seastar::listen() returns a server_socket by value, synchronously --
        // only accept() is a future.
        auto listener = listen(ipv4_addr{"0.0.0.0", port}, lo);

        std::printf("seastar_http_bench listening on port %u shards=%u\n", port, smp::count);
        std::fflush(stdout);

        return accept_loop(std::move(listener));
    });
}

// FAIRNESS NOTES -- read before quoting any number from this binary.
//
//  1. Neither side parses HTTP. Both do accept / read / write-fixed / loop.
//     This is a comparison of the two async runtimes, not of HTTP stacks.
//
//  2. Seastar's output_stream(512) buffers, kio writes directly to the fd.
//     This is a real, small handicap for Seastar and is why 512 was chosen
//     rather than the 8192 default. It is still not zero.
//
//  3. Seastar defaults to the epoll reactor. `--reactor-backend io_uring`
//     switches it to io_uring and is the *fair* comparison against kio, since
//     kio is io_uring-only. Both numbers are worth having: io_uring-vs-epoll
//     shows what the syscall interface costs, epoll shows Seastar as it is
//     normally deployed.
//
//  4. Seastar busy-polls its shards; kio's workers block in the kernel. Seastar
//     will therefore show ~1 core of CPU per shard while idle. That is inherent
//     to its design, not a bug, but it means "CPU per request" and "total CPU"
//     are not comparable between the two -- only throughput is.
//
//  5. Shard count vs worker count: Seastar's -c N spins N shards, one per core,
//     and pins itself. kio's --dispatch spins N+1 threads. Equal *threads* does
//     not mean equal *cores pinned*, so run_kio_vs_seastar.sh accounts for that
//     explicitly by pinning both sides to the same fixed core set.