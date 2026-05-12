#pragma once
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <memory>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>

#include "task.hpp"

namespace URing
{
namespace detail
{
// Helper to extract Result<T> from Task<Result<T>>
template <typename TaskT>
using TaskResultType = decltype(std::declval<TaskT&>().get());

template <typename TaskT>
struct TaskAwaitType;

template <typename T>
struct TaskAwaitType<Task<T>>
{
    using type = T;
};

// Initialize tuple with "pending/cancelled" state
template <typename... Results>
constexpr auto make_pending_tuple()
{
    if constexpr (sizeof...(Results) == 0)
    {
        return std::tuple<Results...>{};
    }
    else
    {
        return std::tuple<Results...>{Results{std::unexpected{MakeErrorCode(ECANCELED)}}...};
    }
}

enum class CompletionStatus : std::uint8_t
{
    idle,
    waiting,
    completed,
};

struct CompletionSignal
{
    std::atomic<CompletionStatus> status{CompletionStatus::idle};
    std::coroutine_handle<> waiter{nullptr};

    [[nodiscard]] bool ready() const noexcept
    {
        return status.load(std::memory_order_acquire) == CompletionStatus::completed;
    }

    void mark_completed() noexcept { status.store(CompletionStatus::completed, std::memory_order_release); }

    [[nodiscard]] bool suspend(std::coroutine_handle<> h) noexcept
    {
        waiter = h;

        CompletionStatus expected = CompletionStatus::idle;
        return status.compare_exchange_strong(expected, CompletionStatus::waiting, std::memory_order_release,
                                              std::memory_order_acquire);
    }

    void complete() noexcept
    {
        if (status.exchange(CompletionStatus::completed, std::memory_order_acq_rel) == CompletionStatus::waiting)
        {
            waiter.resume();
        }
    }
};

template <typename TupleResult>
struct WhenAllState
{
    TupleResult results;
    std::atomic<std::size_t> remaining;
    CompletionSignal completion;

    explicit WhenAllState(const std::size_t count) : results(), remaining(count)
    {
        if (count == 0)
        {
            completion.mark_completed();
        }
    }
};

// Runner for when_all: decrements counter, wakes when ALL complete
template <typename State, std::size_t I>
struct WhenAllRunner
{
    template <typename TaskT>
    static void run(TaskT&& task, std::shared_ptr<State> state) noexcept
    {
        [](auto t, std::shared_ptr<State> s) -> DetachedTask
        {
            using AwaitType = typename TaskAwaitType<std::decay_t<decltype(t)>>::type;

            if constexpr (std::is_void_v<AwaitType>)
            {
                co_await std::move(t);
                std::get<I>(s->results) = Result<void>{};
            }
            else
            {
                auto res = co_await std::move(t);
                std::get<I>(s->results) = std::move(res);
            }

            // fetch_sub returns the PREVIOUS value. If it was 1, we just hit 0.
            if (s->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1)
            {
                s->completion.complete();
            }
            co_return;
        }(std::forward<TaskT>(task), std::move(state));
    }
};

template <typename TupleResult>
struct WhenAnyState
{
    TupleResult results;
    std::atomic<bool> winner_claimed{false};
    std::size_t winner_index = static_cast<std::size_t>(-1);
    CompletionSignal completion;
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
            using AwaitType = typename TaskAwaitType<std::decay_t<decltype(t)>>::type;

            if constexpr (std::is_void_v<AwaitType>)
            {
                co_await std::move(t);

                // Only the FIRST task to reach here succeeds in claiming the "winner" slot
                if (bool expected = false;
                    s->winner_claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                {
                    s->winner_index = I;
                    std::get<I>(s->results) = Result<void>{};
                    s->completion.complete();
                }
            }
            else
            {
                auto res = co_await std::move(t);

                // Only the FIRST task to reach here succeeds in claiming the "winner" slot
                if (bool expected = false;
                    s->winner_claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
                {
                    s->winner_index = I;
                    std::get<I>(s->results) = std::move(res);
                    s->completion.complete();
                }
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
}  // namespace detail

/**
 * Concurrently wait for multiple heterogeneous Task<Result<T>> objects.
 * Returns Task<std::tuple<Result<T>...>> with results in input order.
 *
 * Lifetime-safe: Uses shared_ptr to keep state alive until ALL runners finish.
 * Does NOT trigger Task::safe_destroy() std::terminate() guard.
 */
template <typename... Tasks>
[[nodiscard]] Task<std::tuple<detail::TaskResultType<std::decay_t<Tasks>>...>> when_all(Tasks&&... tasks)
{
    using TupleResult = std::tuple<detail::TaskResultType<std::decay_t<Tasks>>...>;
    using State = detail::WhenAllState<TupleResult>;

    auto state = std::make_shared<State>(sizeof...(Tasks));
    state->results = detail::make_pending_tuple<detail::TaskResultType<std::decay_t<Tasks>>...>();

    if constexpr (sizeof...(Tasks) > 0)
    {
        detail::launch_all(state, std::index_sequence_for<Tasks...>(), std::forward<Tasks>(tasks)...);
    }

    struct Awaiter
    {
        std::shared_ptr<State> state;
        bool await_ready() const noexcept { return state->completion.ready(); }
        bool await_suspend(std::coroutine_handle<> h) noexcept { return state->completion.suspend(h); }
        TupleResult await_resume() noexcept { return std::move(state->results); }
    };

    co_return co_await Awaiter{std::move(state)};
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
[[nodiscard]] Task<std::pair<std::size_t, std::tuple<detail::TaskResultType<std::decay_t<Tasks>>...>>> when_any(
    Tasks&&... tasks)
{
    static_assert(sizeof...(Tasks) > 0, "when_any requires at least one task");

    using TupleResult = std::tuple<detail::TaskResultType<std::decay_t<Tasks>>...>;
    using State = detail::WhenAnyState<TupleResult>;

    auto state = std::make_shared<State>();
    state->results = detail::make_pending_tuple<detail::TaskResultType<std::decay_t<Tasks>>...>();

    if constexpr (sizeof...(Tasks) > 0)
    {
        detail::launch_any(state, std::index_sequence_for<Tasks...>(), std::forward<Tasks>(tasks)...);
    }

    struct Awaiter
    {
        std::shared_ptr<State> state;
        bool await_ready() const noexcept { return state->completion.ready(); }
        bool await_suspend(std::coroutine_handle<> h) noexcept { return state->completion.suspend(h); }
        std::pair<std::size_t, TupleResult> await_resume() noexcept
        {
            return {state->winner_index, std::move(state->results)};
        }
    };

    co_return co_await Awaiter{std::move(state)};
}
}  // namespace URing
