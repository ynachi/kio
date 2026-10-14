//
// Created by Yao ACHI on 14/10/2026.
//
#include <chrono>

#include "kio/operations.h"
#include "kio/task.h"
#include "kio/try.hpp"

using namespace kio::io;
using namespace std::chrono_literals;

namespace
{
    kio::Task<> tick(Context& io, const char* name, std::chrono::microseconds duration)
    {
        for (int i = 0; i < 5; ++i)
        {
            KIO_CO_TRY_VOID(co_await async_sleep(io, duration));
            LOG(INFO) << name << " " << i;
        }

        co_return{};
    }
} // namespace


int main()
{
    Context io{0};
    io.spawn(tick(io, "tick", 100ms));
    io.run();
    return 0;
}
