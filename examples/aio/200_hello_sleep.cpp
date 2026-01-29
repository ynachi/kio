//
// Created by Yao ACHI on 24/01/2026.
//

#include <chrono>

#include "aio/aio.hpp"

using namespace std::chrono_literals;

namespace
{
aio::Task<> AsyncMain(aio::IoContext& ctx)
{
    ALOG_INFO("Starting ...");

    for (int i = 3; i > 0; --i)
    {
        ALOG_INFO("{}...", i);
        co_await aio::AsyncSleep(ctx, 1s);
    }

    ALOG_INFO("Liftoff!");
}
}  // namespace

int main()
{
    aio::IoContext ctx;

    auto task = AsyncMain(ctx);
    ctx.RunUntilDone(std::move(task));

    return 0;
}