//
// Created by Yao ACHI on 22/11/2025.
//

#include "../../kio/sync/when_any.h"

#include <gtest/gtest.h>

#include "../../kio/core/worker.h"
#include "../../kio/sync/sync_wait.h"

using namespace kio;
using namespace kio::sync;
using namespace kio::io;

TEST(WhenAnyTest, FirstTaskWins)
{
    Worker worker(0, WorkerConfig{});
    std::jthread t([&] { worker.LoopForever(); });
    worker.WaitReady();

    auto test = [&]() -> Task<void>
    {
        co_await SwitchToWorker(worker);

        auto fast = [&]() -> Task<void> { co_await worker.AsyncSleep(std::chrono::milliseconds(10)); };

        auto slow = [&]() -> Task<void> { co_await worker.AsyncSleep(std::chrono::milliseconds(100)); };

        size_t winner = co_await when_any(worker, fast(), slow());

        EXPECT_EQ(winner, 0);  // Fast task should win
    };

    SyncWait(test());
    (void) worker.RequestStop();
}
