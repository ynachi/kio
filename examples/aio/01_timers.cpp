#include <chrono>
#include <iostream>

#include "aio/IoContext.hpp"
#include "aio/io.hpp"

using namespace aio;
using namespace std::chrono_literals;

Task<> ticker(IoContext& ctx, int id, std::chrono::milliseconds interval, int count) {
    for (int i = 0; i < count; ++i) {
        co_await AsyncSleep(ctx, interval);
        std::cout << "Tick " << id << ": " << (i + 1) << "/" << count << "\n";
    }
    std::cout << "Ticker " << id << " done\n";
}

Task<> run_demo(IoContext& ctx) {
    std::cout << "Starting concurrent timers...\n";

    auto start = std::chrono::steady_clock::now();

    // Start three concurrent tickers with different intervals
    auto t1 = ticker(ctx, 1, 100ms, 10);  // 100ms x 10 = 1s total
    auto t2 = ticker(ctx, 2, 250ms, 4);   // 250ms x 4  = 1s total
    auto t3 = ticker(ctx, 3, 500ms, 2);   // 500ms x 2  = 1s total

    t1.Start();
    t2.Start();
    t3.Start();
    // co_await t1;
    // co_await t2;
    // co_await t3;

    // Wait for all to complete using simple polling
    // (In a real app, you'd use a proper join mechanism)
    while (!t1.Done() || !t2.Done() || !t3.Done()) {
        co_await AsyncSleep(ctx, 50ms);
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "All tickers done in " << ms << "ms\n";
    ctx.Stop();
}

int main() {
    try {
        IoContext ctx;

        auto t = run_demo(ctx);
        t.resume();

        ctx.Run();

    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
