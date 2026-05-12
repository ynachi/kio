#include "uring/context.h"

#include <chrono>
#include <cstdio>
#include <print>
#include <stop_token>
#include <string>
#include <tuple>

#include "uring/combinators.hpp"
#include "uring/io.hpp"
#include "uring/task.hpp"

using namespace URing;
using namespace std::chrono_literals;

Task<int> value_after(IoContext& ctx, std::chrono::milliseconds delay, int value)
{
    auto slept = co_await sleep(ctx, delay);
    if (!slept)
    {
        std::println(stderr, "sleep failed: {}", slept.error().message());
        co_return -1;
    }

    co_return value;
}

static Task<std::string> text_after(IoContext& ctx, std::chrono::milliseconds delay, std::string value)
{
    if (auto slept = co_await sleep(ctx, delay); !slept)
    {
        std::println(stderr, "sleep failed: {}", slept.error().message());
        co_return "error";
    }

    co_return value;
}

Task<void> combinator_demo(IoContext& ctx, std::stop_source& stop)
{
    auto all = co_await when_all(value_after(ctx, 25ms, 42), text_after(ctx, 50ms, "all done"));
    const auto& first = std::get<0>(all);
    const auto& second = std::get<1>(all);

    if (first && second)
    {
        std::println("when_all completed in input order: {}, {}", *first, *second);
    }

    auto [winner, results] = co_await when_any(value_after(ctx, 20ms, 1), value_after(ctx, 60ms, 2));
    if (winner == 0)
    {
        std::println("when_any winner: task 0 -> {}", *std::get<0>(results));
    }
    else
    {
        std::println("when_any winner: task 1 -> {}", *std::get<1>(results));
    }

    // when_any does not cancel losing tasks; give the loser enough time to complete before stopping the reactor.
    co_await sleep(ctx, 75ms);
    stop.request_stop();
}

int main()
{
    IoContext ctx;
    std::stop_source stop;

    auto task = combinator_demo(ctx, stop);
    task.handle_.resume();

    ctx.run(stop.get_token());
    return 0;
}
