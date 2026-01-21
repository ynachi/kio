// signalfd + async_wait_signal
// Demonstrates: signal_set, async_wait_signal, using a coroutine to stop the loop cleanly.


#include <cstdio>
#include <chrono>

#include "aio/aio.hpp"


static aio::task<> ticker(aio::io_context& ctx) {
    while (true) {
        (void)co_await aio::async_sleep(&ctx, std::chrono::seconds(1));
        std::fprintf(stderr, "tick...\n");
    }
}

static aio::task<> stop_on_signal(aio::io_context& ctx, int sfd) {
    auto sig = co_await aio::async_wait_signal(&ctx, sfd);
    if (sig) std::fprintf(stderr, "got signal %d, stopping...\n", *sig);
    ctx.stop();
    co_return;
}

int main() {
    aio::io_context ctx(256);

    // blocks SIGINT/SIGTERM for the process and routes them via signalfd
    aio::signal_set sigs{SIGINT, SIGTERM};

    auto t1 = ticker(ctx);       t1.start();
    auto t2 = stop_on_signal(ctx, sigs.fd()); t2.start();

    ctx.run();

    // make sure no in-flight ops remain before tasks destruct
    ctx.cancel_all_pending();
    return 0;
}
