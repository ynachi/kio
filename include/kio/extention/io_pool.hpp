#pragma once

// ============================================================================
// kio::IoContext -- a pool of IO workers, one jthread each
// ============================================================================
//
// A convenience wrapper, not a core type. It owns N IO objects and the N jthreads
// that drive them, plus the stop_source that shuts them all down at once.
//
// THE ONE THING THAT IS LOAD-BEARING HERE
// ---------------------------------------
//
//     workers_.reserve(num_threads);   <-- before the first emplace_back
//
// IO is move-constructible, but ONLY while inert: its move ctor calls
// std::terminate if the source is activated, running, or non-empty. All of
// that becomes true the moment activate() runs. So if the vector ever had to
// grow, the reallocation would move live, activated workers and terminate the
// process.
//
// reserve() guarantees it never reallocates. The same guarantee is what makes
// `const IO* leader = &workers_[0]` a stable pointer across the construction
// loop below -- without it, the leader pointer could dangle before it is used.
//
// Member declaration order also matters, twice over:
//   * construction:   workers_ is filled completely BEFORE any thread starts,
//                     so a worker thread never observes a partially built pool.
//   * destruction:   members are destroyed in reverse declaration order, so
//                     threads_ is destroyed (which stops AND joins them via
//                     ~jthread) before workers_ is destroyed. An IO is never
//                     destroyed while a thread is still running its loop.
// ============================================================================

#include "../core/io.h"

namespace kio
{
    /// IoContext serves as an example of IO pool.
    /// Advanced use-cases should prefer working with the IO class
    /// directly.
    ///
    /// Prefer working with IO directly for anything beyond this shape. The
    /// reasons it is an example rather than the recommended entry point:
    ///   * work distribution is manual -- there is no run-queue, no per-worker
    ///     affinity, and no automatic hand-off. A connection is pinned to
    ///     whichever worker you scheduled it on, for life.
    ///   * every worker shares one kernel workqueue by default
    ///     (IoOptions::share_work_queues), which is a throughput choice, not a
    ///     correctness one; set it false for isolated per-worker pools.
    ///   * buffers are per-IO, so a fixed-buffer lease cannot migrate between
    ///     workers (see IO::owns in io.h).
    class IoContext
    {
        std::stop_source stop_source_;
        IoOptions opts_;
        std::vector<IO> workers_;
        std::vector<std::jthread> threads_;

    public:
        // A live IoContext owns threads whose lambdas captured `this`. Moving it would
        // leave those threads pointing at the old object.
        //
        // Note this is stronger than the usual reason: the threads capture `this`
        // AND index into workers_. A move would invalidate both, and unlike a
        // normal self-referential member there is no way to fix up the running
        // threads from the outside.
        IoContext(const IoContext&) = delete;
        IoContext& operator=(const IoContext&) = delete;
        IoContext(IoContext&&) = delete;
        IoContext& operator=(IoContext&&) = delete;

        /// Build the pool and start driving it.
        ///
        /// INVARIANT: the constructor does not return until every worker thread
        /// has been handed an IO and is running its loop. There is no "start()"
        /// step, so there is no window in which the pool exists but is idle --
        /// which also means it cannot be constructed in a state where a caller
        /// has already tried to schedule work onto it.
        ///
        /// Throws if num_threads == 0, or if any ring fails to initialise. A
        /// partially constructed context is not a thing: if the nth IO's
        /// constructor throws, the already-constructed IOs are destroyed by
        /// member unwinding and no threads have started yet.
        explicit IoContext(const std::size_t num_threads, const IoOptions& opts = {}) : opts_(opts)
        {
            if (num_threads == 0)
            {
                throw std::runtime_error("io context started with 0 thread");
            }

            // Load-bearing. See the file header: without this, growth would move
            // live IO objects and std::terminate.
            workers_.reserve(num_threads);
            threads_.reserve(num_threads);

            // Leader (Worker 0)
            //
            // The leader is the ONLY ring created without a parent. Every other
            // ring attaches to its workqueue via IORING_SETUP_ATTACH_WQ (see
            // IO::init), so N rings end up sharing one pool of kernel worker
            // threads. That is what opts_.share_work_queues == true means, and
            // why worker 0 must exist first: there is nothing else to attach to.
            workers_.emplace_back(0, nullptr, opts_);
            const IO* leader = &workers_[0];

            // Followers
            for (std::size_t i = 1; i < num_threads; ++i)
            {
                workers_.emplace_back(i, leader, opts_);
            }

            // One token for all workers. Sharing it is the point: request_stop()
            // in stop() shuts the whole pool down with a single call, and each
            // worker's own stop callback latches it so its loop exits.
            std::stop_token st = stop_token();

            // now run the threads
            //
            // Started only after every worker exists, so no lambda can observe
            // a half-built pool. The lambda captures `this` (hence the deleted
            // move) and its own index `i` (hence the reserve guaranteeing
            // workers_ never reallocates under it).
            for (std::size_t i = 0; i < num_threads; ++i)
            {
                // run here
                threads_.emplace_back(
                    [this, st, i]()
                    {
                        IO& io = workers_[i];
                        io.run_blocking(st);
                    });
            }
        }

        // Joins every worker, which requests stop on the way out. Safe to call
        // more than once (stop() is idempotent; threads_ is cleared afterwards),
        // and safe to never call -- ~IoContext calls it.
        ~IoContext() { join(); }

        bool stop() const
        {
            // The teardown itself lives in IO::drain_until_quiescent(): each worker
            // drains for IoOptions::shutdown_grace_ms, then escalates to
            // IORING_ASYNC_CANCEL_ANY and drains the cancellations so no coroutine
            // is left suspended on an operation nobody will ever complete.
            //
            // What this call does is only the FIRST half: it sets the flag. Each
            // worker's own stop callback (installed in run_blocking) latches it
            // into that worker's admission gate. The loop then notices within
            // one heartbeat interval, because the park is bounded -- there is
            // no wake to send and none is needed.
            //
            // stop() is also what closes each worker's admission gate, so no new
            // task or operation can be accepted between here and the drain.
            //
            // Returns whether a stop was newly requested; false means one had
            // already been, i.e. this was a no-op.
            if (stop_source_.stop_possible())
            {
                return stop_source_.request_stop();
            }
            return false;
        }

        std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

        /// Access a worker by index. The reference is stable for the lifetime of
        /// the IoContext -- see the reserve() note in the file header.
        ///
        /// No bounds checking: an out-of-range index is a caller bug, and this
        /// sits on the path of every server's accept loop.
        [[nodiscard]] IO& worker(const std::size_t idx) { return workers_[idx]; }
        [[nodiscard]] const IO& worker(const std::size_t idx) const { return workers_[idx]; }
        [[nodiscard]] std::size_t worker_count() const noexcept { return workers_.size(); }

        /// Request stop, then join every worker thread. Each worker runs its own
        /// full teardown before run_blocking() returns, so when this function
        /// returns every ring has been drained and cancelled to quiescence and
        /// every coroutine has been resumed at least once.
        ///
        /// ORDER MATTERS: threads_ is joined and cleared BEFORE workers_ is
        /// touched. Reversing it would destroy IO objects while threads were
        /// still running loops over them.
        void join() noexcept
        {
            (void)stop();

            for (auto& thread : threads_)
            {
                if (thread.joinable())
                {
                    // Blocks for as long as the slowest worker's drain takes.
                    // Bounded by 2 * shutdown_grace_ms per worker in the worst
                    // case (grace, then cancel, then a second bounded wait), so
                    // this cannot hang indefinitely.
                    thread.join();
                }
            }

            // Only now, with no thread running, is it safe to drop the IO
            // objects. ~IO will see a drained ring and no abandoned tasks.
            threads_.clear();
            workers_.clear();
        }
    };
} // namespace kio
