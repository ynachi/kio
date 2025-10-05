#ifndef SYNC_WAIT_H
#define SYNC_WAIT_H

#include <latch>
#include <optional>
#include "coro.h"

namespace kio
{
    /**
     * Synchronously wait for a Task to complete and get its value.
     * This is for testing only.
     * @tparam T
     * @param task
     * @return
     */
    template<typename T>
    T sync_wait(Task<T> task)
    {
        std::latch done(1);
        std::exception_ptr ex_ptr;

        if constexpr (std::is_void_v<T>) {
            auto wrapper = [&]() -> DetachedTask
            {
                try
                {
                    co_await task;
                }
                catch (...)
                {
                    ex_ptr = std::current_exception();
                }
                done.count_down();
            };

            wrapper().detach();
            done.wait();

            if (ex_ptr)
            {
                std::rethrow_exception(ex_ptr);
            }
        } else {
            std::optional<T> result;

            auto wrapper = [&]() -> DetachedTask
            {
                try
                {
                    result = co_await task;
                }
                catch (...)
                {
                    ex_ptr = std::current_exception();
                }
                done.count_down();
            };

            wrapper().detach();
            done.wait();

            if (ex_ptr)
            {
                std::rethrow_exception(ex_ptr);
            }

            return std::move(*result);
        }
    }
}

#endif // SYNC_WAIT_H