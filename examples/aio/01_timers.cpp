#include "aio/aio.hpp"

#include <iostream>
#include <chrono>

using namespace aio;
using namespace std::chrono_literals;

task<> ticker(io_context& ctx, int id, std::chrono::milliseconds interval, int count) {
    for (int i = 0; i < count; ++i) {
        co_await async_sleep{&ctx, interval};
        std::cout << "Tick " << id << ": " << (i + 1) << "/" << count << "\n";
    }
    std::cout << "Ticker " << id << " done\n";
}

task<> run_demo(io_context& ctx) {
    std::cout << "Starting concurrent timers...\n";

    auto start = std::chrono::steady_clock::now();

    // Start three concurrent tickers with different intervals
    auto t1 = ticker(ctx, 1, 100ms, 10);  // 100ms x 10 = 1s total
    auto t2 = ticker(ctx, 2, 250ms, 4);   // 250ms x 4  = 1s total
    auto t3 = ticker(ctx, 3, 500ms, 2);   // 500ms x 2  = 1s total

    t1.start();
    t2.start();
    t3.start();
    // co_await t1;
    // co_await t2;
    // co_await t3;

    // Wait for all to complete using simple polling
    // (In a real app, you'd use a proper join mechanism)
    while (!t1.done() || !t2.done() || !t3.done()) {
        co_await async_sleep{&ctx, 50ms};
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "All tickers done in " << ms << "ms\n";
    ctx.stop();
}

int main() {
    try {
        io_context ctx;

        auto t = run_demo(ctx);
        t.resume();

        ctx.run();

    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
