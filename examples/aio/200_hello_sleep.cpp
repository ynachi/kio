//
// Created by Yao ACHI on 24/01/2026.
//

#include <chrono>
#include <print>

#include "aio/aio.hpp"

using namespace std::chrono_literals;

namespace
{
aio::Task<> AsyncMain(aio::IoContext& ctx)
{
    aio::alog::info("Starting ...");

    for (int i = 3; i > 0; --i)
    {
        aio::alog::info("{}...", i);
        co_await aio::AsyncSleep(ctx, 1s);
    }

    aio::alog::info("Liftoff!");
}
}  // namespace

int main()
{
    aio::IoContext ctx;

    auto task = AsyncMain(ctx);
    ctx.RunUntilDone(task);

    return 0;
}