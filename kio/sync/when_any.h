//
// Created by Yao ACHI on 22/11/2025.
//

#ifndef KIO_WHEN_ANY_H
#define KIO_WHEN_ANY_H
#include <atomic>
#include <tuple>

#include "../core/coro.h"
#include "baton.h"

namespace kio::sync
{

namespace detail
{

/**
 * @brief Shared state for when_any operation.
 *
 * Tracks which task completes first and coordinates resumption.
 */
template<size_t N>
struct WhenAnyState
{
    std::atomic<size_t> winner{N};  // N means "no winner yet"
    AsyncBaton done;

    explicit WhenAnyState(io::Worker& worker) : done(worker) {}

    // Try to claim victory
    bool try_win(size_t index) noexcept
    {
        size_t expected = N;
        return winner.compare_exchange_strong(expected, index, std::memory_order_acq_rel, std::memory_order_acquire);
    }
};

/**
 * @brief Wrapper that runs a task and reports completion to shared state.
 */
template<size_t Index, size_t N, typename Task>
DetachedTask run_and_notify(std::shared_ptr<WhenAnyState<N>> state, Task task)
{
    // Run the task
    co_await task;

    // Try to win the race
    if (state->try_win(Index))
    {
        // We won! Notify the waiter
        state->done.notify();
    }
    // If we didn't win, someone else already did - do nothing
}

/**
 * @brief Helper to spawn all tasks with indices.
 */
template<size_t... Is, typename... Tasks>
void spawn_tasks_impl(std::shared_ptr<WhenAnyState<sizeof...(Tasks)>> state, std::index_sequence<Is...>,
                      Tasks&&... tasks)
{
    // Spawn all tasks with their indices
    (run_and_notify<Is>(state, std::forward<Tasks>(tasks)).detach(), ...);
}

}  // namespace detail

/**
 * @brief Wait for any of the given tasks to complete.
 *
 * Returns the index of the first task that completes.
 * All other tasks continue running in the background.
 *
 **/
template<typename... Tasks>
Task<size_t> when_any(io::Worker& worker, Tasks&&... tasks)
{
    constexpr size_t N = sizeof...(Tasks);
    static_assert(N > 0, "when_any requires at least one task");

    // Create a shared state
    auto state = std::make_shared<detail::WhenAnyState<N>>(worker);

    // Spawn all tasks
    detail::spawn_tasks_impl(state, std::index_sequence_for<Tasks...>{}, std::forward<Tasks>(tasks)...);

    // Wait for first completion
    co_await state->done.wait();

    // Return the winner
    co_return state->winner.load(std::memory_order_acquire);
}

}  // namespace kio::sync
#endif  // KIO_WHEN_ANY_H
