#pragma once
// aio/blocking_pool.hpp
// Thread pool using std::move_only_function (C++23)
//
// Benefits over void(*)(void*):
// - Type-safe lambda captures
// - Can capture move-only types (unique_ptr, etc)
// - No manual type erasure needed
// - Better compiler optimization

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "events.hpp"
#include "io_context.hpp"
#include "operation_base.hpp"

namespace aio {

namespace detail
{

// Thread-local waker for blocking pool threads
inline thread_local RingWaker tls_waker;

}

class BlockingPool {
public:
    using job_t = std::move_only_function<void() noexcept>;
    
    explicit BlockingPool(std::size_t threads, std::size_t capacity = 4096)
        : cap_(capacity)
        , q_(capacity) {
        if (threads == 0) threads = 1;
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }
    
    ~BlockingPool() {
        Stop();
    }
    
    BlockingPool(const BlockingPool&) = delete;
    BlockingPool& operator=(const BlockingPool&) = delete;
    
    void Stop() {
        bool expected = false;
        if (!stopping_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
        {
            std::scoped_lock lk(m_);
            cv_.notify_all();
        }
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }
    
    // Submit a callable (lambda, function, etc)
    // Returns false if queue is full
    bool TrySubmit(job_t&& job) {
        std::scoped_lock lk(m_);
        if (count_ == cap_) return false;
        
        q_[tail_] = std::move(job);
        tail_ = (tail_ + 1) % cap_;
        ++count_;
        cv_.notify_one();
        return true;
    }
    
    // Convenience: submit with automatic move
    template<typename F>
    bool Submit(F&& f) {
        return TrySubmit(job_t{std::forward<F>(f)});
    }

private:
    void WorkerLoop() {
        while (true) {
            job_t job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] {
                    return stopping_.load(std::memory_order_relaxed) || count_ > 0;
                });
                if (stopping_.load(std::memory_order_relaxed) && count_ == 0) {
                    return;
                }
                job = std::move(q_[head_]);
                head_ = (head_ + 1) % cap_;
                --count_;
            }
            
            // Execute outside lock
            job();
        }
    }
    
    std::atomic<bool> stopping_{false};
    std::mutex m_;
    std::condition_variable cv_;
    
    const std::size_t cap_;
    std::vector<job_t> q_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
    
    std::vector<std::thread> workers_;
};

// Awaitable that runs Fn on the blocking pool, then resumes on ctx's thread.
//
// IMPORTANT: Like your uring ops, this awaitable is embedded in the coroutine
// frame. Destroying the coroutine while the offload is in flight is UB.
// We "detect" this by tracking the awaitable in io_context's intrusive list
// and untracking on completion (the same fail-fast model you use for io_uring).
template<class Fn>
struct OffloadOp : OperationState {
    BlockingPool* pool = nullptr;
    Fn fn;
    
    using R = std::invoke_result_t<Fn>;
    std::conditional_t<std::is_void_v<R>, bool, std::optional<R>> value;
    std::exception_ptr ep;
    
    OffloadOp(IoContext* c, BlockingPool* p, Fn&& f)
        : pool(p), fn(std::forward<Fn>(f)) {
        ctx = c;
    }
    
    bool await_ready() const noexcept { return false; }
    
    void await_suspend(std::coroutine_handle<> h) {
        handle = h;
        ctx->Track(this);
        
        // Create lambda that captures 'this' - safe because we track lifetime
        auto job = [this]() noexcept {
            auto* ctx_local = ctx;
            const int ring_fd = ctx_local->RingFd();
            try {
                if constexpr (std::is_void_v<R>) {
                    fn();
                    value = true;
                } else {
                    value.emplace(fn());
                }
            } catch (...) {
                ep = std::current_exception();
            }
            
            // Resume on io_context thread
            if (ctx_local->EnqueueExternalDone(this)) {
                detail::tls_waker.Wake(ring_fd);
            }
        };
        
        if (!pool->TrySubmit(std::move(job))) {
            ctx->Untrack(this);
            throw std::runtime_error("blocking_pool queue full");
        }
    }
    
    R await_resume() {
        if (ep) std::rethrow_exception(ep);
        if constexpr (std::is_void_v<R>) {
            return;
        } else {
            return std::move(*value);
        }
    }
};

template<class Fn>
auto Offload(IoContext& ctx, BlockingPool& pool, Fn&& fn) {
    return OffloadOp<std::decay_t<Fn>>{&ctx, &pool, std::forward<Fn>(fn)};
}

} // namespace aio
