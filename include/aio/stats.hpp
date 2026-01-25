#pragma once

#include <atomic>
#include <cstdint>

#ifndef AIO_STATS
#define AIO_STATS 0
#endif

namespace aio
{

struct IoContextStats
{
    /// Total operations submitted to this IoContext (tracked).
    std::atomic<uint64_t> ops_submitted{0};
    /// Total operations completed (including externally completed ops).
    std::atomic<uint64_t> ops_completed{0};
    /// Operations completed with a negative result code (includes cancellations/timeouts).
    std::atomic<uint64_t> ops_errors{0};
    /// Current number of tracked operations.
    std::atomic<uint64_t> ops_inflight{0};
    /// Peak inflight operations observed.
    std::atomic<uint64_t> ops_max_inflight{0};
    /// Operations that completed due to a timeout.
    std::atomic<uint64_t> timeouts{0};

    /// Operations completed outside the loop and injected for resume.
    std::atomic<uint64_t> external_completions{0};

    /// Total event loop iterations.
    std::atomic<uint64_t> loop_iterations{0};
    /// Iterations that processed at least one completion.
    std::atomic<uint64_t> loop_busy_iterations{0};
    /// Iterations that processed zero completions.
    std::atomic<uint64_t> loop_idle_iterations{0};
    /// Wake signals observed by the loop.
    std::atomic<uint64_t> loop_wakeups{0};
    /// Total completions processed by the loop (sum of batch sizes).
    std::atomic<uint64_t> loop_completions{0};
    /// Largest completion batch processed in a single iteration.
    std::atomic<uint64_t> loop_max_batch{0};

    struct Snapshot
    {
        uint64_t ops_submitted;
        uint64_t ops_completed;
        uint64_t ops_errors;
        uint64_t ops_inflight;
        uint64_t ops_max_inflight;
        uint64_t timeouts;
        uint64_t external_completions;
        uint64_t loop_iterations;
        uint64_t loop_busy_iterations;
        uint64_t loop_idle_iterations;
        uint64_t loop_wakeups;
        uint64_t loop_completions;
        uint64_t loop_max_batch;
    };

    Snapshot GetSnapshot() const
    {
        return {
            .ops_submitted = ops_submitted.load(std::memory_order_relaxed),
            .ops_completed = ops_completed.load(std::memory_order_relaxed),
            .ops_errors = ops_errors.load(std::memory_order_relaxed),
            .ops_inflight = ops_inflight.load(std::memory_order_relaxed),
            .ops_max_inflight = ops_max_inflight.load(std::memory_order_relaxed),
            .timeouts = timeouts.load(std::memory_order_relaxed),
            .external_completions = external_completions.load(std::memory_order_relaxed),
            .loop_iterations = loop_iterations.load(std::memory_order_relaxed),
            .loop_busy_iterations = loop_busy_iterations.load(std::memory_order_relaxed),
            .loop_idle_iterations = loop_idle_iterations.load(std::memory_order_relaxed),
            .loop_wakeups = loop_wakeups.load(std::memory_order_relaxed),
            .loop_completions = loop_completions.load(std::memory_order_relaxed),
            .loop_max_batch = loop_max_batch.load(std::memory_order_relaxed),
        };
    }
};

#if AIO_STATS
namespace detail
{
inline void StatsSetMax(std::atomic<uint64_t>& target, uint64_t value)
{
    uint64_t cur = target.load(std::memory_order_relaxed);
    while (cur < value && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed))
    {
    }
}
}  // namespace detail

#define AIO_STATS_INC(stats, field) ((stats).field.fetch_add(1, std::memory_order_relaxed))
#define AIO_STATS_DEC(stats, field) ((stats).field.fetch_sub(1, std::memory_order_relaxed))
#define AIO_STATS_ADD(stats, field, value) ((stats).field.fetch_add((value), std::memory_order_relaxed))
#define AIO_STATS_SET_MAX(stats, field, value) ::aio::detail::StatsSetMax((stats).field, (value))
#else
#define AIO_STATS_INC(stats, field) ((void)0)
#define AIO_STATS_DEC(stats, field) ((void)0)
#define AIO_STATS_ADD(stats, field, value) ((void)0)
#define AIO_STATS_SET_MAX(stats, field, value) ((void)0)
#endif

}  // namespace aio
