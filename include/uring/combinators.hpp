#pragma once
#include <expected>
#include <memory>
#include <system_error>
#include <utility>

#include "task.hpp"

namespace URing
{
namespace detail
{
// Helper to extract Result<T> from Task<Result<T>>
template <typename TaskT>
using TaskResultType = decltype(std::declval<TaskT&>().get());

// Initialize tuple with "pending/cancelled" state
template <typename... Results>
constexpr auto make_pending_tuple()
{
    if constexpr (sizeof...(Results) == 0)
        return std::tuple<Results...>{};
    else
        return std::tuple<Results...>{std::unexpected<std::error_code>{}...};
}

// Runner for when_all: decrements counter, wakes when ALL complete
template <typename State, std::size_t I>
struct WhenAllRunner
{
    template <typename TaskT>
    static void run(TaskT&& task, std::shared_ptr<State> state) noexcept
    {
        [](auto t, std::shared_ptr<State> s) -> DetachedTask
        {
            auto res = co_await std::move(t);
            std::get<I>(s->results) = std::move(res);

            // fetch_sub returns the PREVIOUS value. If it was 1, we just hit 0.
            if (s->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                if (s->waiter)
                {
                    s->waiter.resume();
                }
            }
            co_return;
        }(std::forward<TaskT>(task), std::move(state));
    }
};

// Runner for when_any: first to complete wins, wakes immediately
template <typename State, std::size_t I>
struct WhenAnyRunner
{
    template <typename TaskT>
    static void run(TaskT&& task, std::shared_ptr<State> state) noexcept
    {
        [](auto t, std::shared_ptr<State> s) -> DetachedTask
        {
            auto res = co_await std::move(t);
            bool expected = false;
            // Only the FIRST task to reach here succeeds
            if (s->finished.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                s->winner_index = I;
                std::get<I>(s->results) = std::move(res);
                if (s->waiter)
                    s->waiter.resume();
            }
            // Losers complete normally but their results are discarded
            co_return;
        }(std::forward<TaskT>(task), std::move(state));
    }
};

template <typename State, typename... Tasks, std::size_t... Is>
void launch_all(std::shared_ptr<State> state, std::index_sequence<Is...>, Tasks&&... tasks)
{
    (WhenAllRunner<State, Is>::run(std::forward<Tasks>(tasks), state), ...);
}

template <typename State, typename... Tasks, std::size_t... Is>
void launch_any(std::shared_ptr<State> state, std::index_sequence<Is...>, Tasks&&... tasks)
{
    (WhenAnyRunner<State, Is>::run(std::forward<Tasks>(tasks), state), ...);
}

/**
 * Concurrently wait for multiple heterogeneous Task<Result<T>> objects.
 * Returns Task<std::tuple<Result<T>...>> with results in input order.
 *
 * Lifetime-safe: Uses shared_ptr to keep state alive until ALL runners finish.
 * Does NOT trigger Task::safe_destroy() std::terminate() guard.
 */
template <typename... Tasks>
[[nodiscard]] Task<std::tuple<TaskResultType<std::decay_t<Tasks>>...>> when_all(Tasks&&... tasks)
{
    using TupleResult = std::tuple<TaskResultType<std::decay_t<Tasks>>...>;

    struct State
    {
        TupleResult results = detail::make_pending_tuple<TaskResultType<std::decay_t<Tasks>>...>();
        std::atomic<std::size_t> remaining{sizeof...(Tasks)};
        std::coroutine_handle<> waiter{nullptr};
    };

    auto state = std::make_shared<State>();

    if constexpr (sizeof...(Tasks) > 0)
    {
        detail::launch_all(state, std::index_sequence_for<Tasks...>(), std::forward<Tasks>(tasks)...);
    }

    struct Awaiter
    {
        std::shared_ptr<State> state;
        bool await_ready() const noexcept { return state->remaining.load(std::memory_order_acquire) == 0; }
        bool await_suspend(std::coroutine_handle<> h) noexcept
        {
            state->waiter = h;
            // Re-check after registration to prevent deadlock on synchronous completion
            return state->remaining.load(std::memory_order_acquire) != 0;
        }
        TupleResult await_resume() noexcept { return std::move(state->results); }
    };

    co_await Awaiter{std::move(state)};
}

/**
 * Wait for the first of multiple heterogeneous Task<Result<T>> objects to complete.
 * Returns Task<std::pair<std::size_t, std::tuple<Result<T>...>>> where:
 *   .first = index of the winning task (0-based)
 *   .second = tuple of results (only winner is populated)
 *
 * Lifetime-safe: Losers run to completion but silently discard results.
 */
template <typename... Tasks>
[[nodiscard]] Task<std::pair<std::size_t, std::tuple<TaskResultType<std::decay_t<Tasks>>...>>> when_any(
    Tasks&&... tasks)
{
    using TupleResult = std::tuple<TaskResultType<std::decay_t<Tasks>>...>;

    struct State
    {
        TupleResult results = detail::make_pending_tuple<TaskResultType<std::decay_t<Tasks>>...>();
        std::atomic<bool> finished{false};
        std::size_t winner_index = static_cast<std::size_t>(-1);
        std::coroutine_handle<> waiter{nullptr};
    };

    auto state = std::make_shared<State>();

    if constexpr (sizeof...(Tasks) > 0)
    {
        detail::launch_any(state, std::index_sequence_for<Tasks...>(), std::forward<Tasks>(tasks)...);
    }

    struct Awaiter
    {
        std::shared_ptr<State> state;
        bool await_ready() const noexcept { return state->finished.load(std::memory_order_acquire); }
        bool await_suspend(std::coroutine_handle<> h) noexcept
        {
            state->waiter = h;
            return !state->finished.load(std::memory_order_acquire);
        }
        std::pair<std::size_t, TupleResult> await_resume() noexcept
        {
            return {state->winner_index, std::move(state->results)};
        }
    };

    co_await Awaiter{std::move(state)};
}
}  // namespace detail
}  // namespace URing