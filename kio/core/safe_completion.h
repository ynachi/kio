#ifndef KIO_SAFE_COMPLETION_H
#define KIO_SAFE_COMPLETION_H

#include <atomic>
#include <cassert>
#include <coroutine>
#include <memory>
#include <mutex>

namespace kio::io
{

// Simple spinlock for ultra-low latency critical sections
struct SpinLock
{
    std::atomic_flag flag = ATOMIC_FLAG_INIT;

    void Lock()
    {
        while (flag.test_and_set(std::memory_order_acquire))
        {
#if defined(__cpp_lib_atomic_wait)
            flag.wait(true, std::memory_order_relaxed);
#endif
        }
    }

    void Unlock()
    {
        flag.clear(std::memory_order_release);
#if defined(__cpp_lib_atomic_wait)
        flag.notify_one();
#endif
    }
};

/**
 * @brief A reference-counted completion state that survives coroutine destruction.
 */
struct SafeIoCompletion
{
    std::atomic<int> ref_count{0};
    std::coroutine_handle<> handle{nullptr};
    int result{0};
    SpinLock lock;

    // Reset state for reuse
    void Reset(std::coroutine_handle<> h)
    {
        // 1 for User (Awaitable), 1 for Kernel
        ref_count.store(2, std::memory_order_relaxed);
        handle = h;
        result = 0;
        // Ensure unlocked state
        lock.Unlock();
    }

    void Release();

    // Called by IoUringAwaitable destructor (User side)
    void Abandon()
    {
        lock.Lock();
        // Detach: Coroutine is gone, don't resume!
        handle = nullptr;
        lock.Unlock();
        Release();
    }

    // Called by Worker (Kernel side)
    void Complete(int res)
    {
        std::coroutine_handle<> h_to_resume = nullptr;

        lock.Lock();
        result = res;
        // Copy handle (might be null if Abandon() was called)
        h_to_resume = handle;
        lock.Unlock();

        // Resume outside lock
        if (h_to_resume)
        {
            h_to_resume.resume();
        }

        Release();
    }
};

// Optimized Thread-Local Object Pool (Fixed-size Stack)
class CompletionPool
{
    static constexpr size_t kPoolCapacity = 1024;

    struct LocalCache
    {
        SafeIoCompletion* items[kPoolCapacity];
        size_t count{0};

        ~LocalCache()
        {
            // Clean up pooled objects on thread exit
            for (size_t i = 0; i < count; ++i)
            {
                delete items[i];
            }
        }
    };

    static LocalCache& GetCache()
    {
        thread_local LocalCache cache;
        return cache;
    }

public:
    static SafeIoCompletion* Acquire(std::coroutine_handle<> h)
    {
        auto& cache = GetCache();
        SafeIoCompletion* ptr = nullptr;

        // Fast path: Pop from stack
        if (cache.count > 0)
        {
            ptr = cache.items[--cache.count];
        }
        else
        {
            // Slow path: Allocate new
            ptr = new SafeIoCompletion();
        }

        ptr->Reset(h);
        return ptr;
    }

    static void Release(SafeIoCompletion* ptr)
    {
        auto& cache = GetCache();

        // Fast path: Push to stack
        if (cache.count < kPoolCapacity)
        {
            cache.items[cache.count++] = ptr;
        }
        else
        {
            // Slow path: Pool full, delete
            delete ptr;
        }
    }
};

inline void SafeIoCompletion::Release()
{
    // If we are the last one holding the ref, recycle it.
    if (ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        CompletionPool::Release(this);
    }
}

}  // namespace kio::io

#endif  // KIO_SAFE_COMPLETION_H