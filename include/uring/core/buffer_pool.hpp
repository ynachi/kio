#pragma once

#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/uio.h>

#include "uring/error.hpp"

namespace kio
{
    class RegisteredBufferPool;

    // ============================================================================
    // BufferLease: Movable, non-copyable RAII handle to a pooled buffer slot
    // ============================================================================
    /**
     * @brief An indivisible RAII lease representing exclusive ownership of one slot
     *        in a RegisteredBufferPool.
     *
     * Invariants:
     *  1. Exclusive Ownership: While a BufferLease is valid, its slot ordinal is
     *     guaranteed not to be held by any other lease or free list in the pool.
     *  2. Stable Memory: The memory range [data(), data() + size()) resides inside
     *     a pinned/registered mmap region that remains alive for the lifetime of
     *     the owning RegisteredBufferPool.
     *  3. Automatic Return: Upon destruction or explicit reset(), the slot ordinal
     *     is returned to the pool (or handed off directly to the oldest FIFO waiter).
     *  4. Move-Only: Copying is disabled to prevent double-return of the underlying slot.
     */
    class BufferLease
    {
    public:
        BufferLease() noexcept = default;

        BufferLease(const BufferLease&) = delete;
        BufferLease& operator=(const BufferLease&) = delete;

        BufferLease(BufferLease&& other) noexcept
            : pool_(std::exchange(other.pool_, nullptr)),
              data_(std::exchange(other.data_, nullptr)),
              size_(std::exchange(other.size_, 0)),
              ordinal_(std::exchange(other.ordinal_, 0))
        {
        }

        BufferLease& operator=(BufferLease&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                pool_ = std::exchange(other.pool_, nullptr);
                data_ = std::exchange(other.data_, nullptr);
                size_ = std::exchange(other.size_, 0);
                ordinal_ = std::exchange(other.ordinal_, 0);
            }
            return *this;
        }

        ~BufferLease() noexcept { reset(); }

        /// @brief Return the leased slot back to the pool immediately. Idempotent.
        void reset() noexcept;

        /// @brief Test if this lease holds a valid, non-empty buffer slot.
        [[nodiscard]] explicit operator bool() const noexcept { return pool_ != nullptr; }

        /// @brief View the leased memory as a mutable byte span.
        [[nodiscard]] std::span<std::byte> bytes() noexcept { return {data_, size_}; }

        /// @brief View the leased memory as a const byte span.
        [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return {data_, size_}; }

        /// @brief Raw pointer to the start of the leased slot.
        [[nodiscard]] std::byte* data() noexcept { return data_; }
        [[nodiscard]] const std::byte* data() const noexcept { return data_; }

        /// @brief Size of the slot in bytes.
        [[nodiscard]] size_t size() const noexcept { return size_; }

        /// @brief Unique slot ordinal within the pool [0, capacity()).
        [[nodiscard]] uint32_t ordinal() const noexcept { return ordinal_; }

        /// @brief Pointer to the owning pool.
        [[nodiscard]] RegisteredBufferPool* pool() const noexcept { return pool_; }

    private:
        friend class RegisteredBufferPool;

        BufferLease(RegisteredBufferPool& pool, std::byte* data, size_t size, uint32_t ordinal) noexcept
            : pool_(&pool), data_(data), size_(size), ordinal_(ordinal)
        {
        }

        RegisteredBufferPool* pool_ = nullptr;
        std::byte* data_ = nullptr;
        size_t size_ = 0;
        uint32_t ordinal_ = 0;
    };

    // Backward-compatibility alias during migration
    using FixedBuffer = BufferLease;

    // ============================================================================
    // BufferPoolConfig: Declarative configuration for pool initialization
    // ============================================================================
    struct BufferPoolConfig
    {
        size_t slot_size = 65536; // 64 KiB default
        uint32_t slots = 1024; // 64 MiB total
    };

    // ============================================================================
    // RegisteredBufferPool
    // ============================================================================
    /**
     * @brief High-performance, single-threaded buffer pool registered with io_uring.
     *
     * Memory Layout & 1 GiB Registration Slicing:
     *
     * base_
     *   │
     *   ▼ 0                   1 GiB                 2 GiB               2.5 GiB       aligned (2 MiB)
     *   ┌─────────────────────┬─────────────────────┬─────────────────────┬──────────────┬───┐
     *   │      Chunk 0        │      Chunk 1        │      Chunk 2        │   Chunk 3    │PAD│
     *   │     (1024 MiB)      │     (1024 MiB)      │     (1024 MiB)      │  (512 MiB)   │   │
     *   └─────────────────────┴─────────────────────┴─────────────────────┴──────────────┴───┘
     *   ▲                     ▲                     ▲                     ▲
     *   │ iovec[0]            │ iovec[1]            │ iovec[2]            │ iovec[3]
     *   │ buf_index = 0       │ buf_index = 1       │ buf_index = 2       │ buf_index = 3
     *
     * Fast branchless lookup:
     *   buf_index = first_index_ + ((ptr - base_) >> 30)
     *
     * Core Architectural Invariants:
     *
     * 1. Single Contiguous Mapping & 2 MiB THP Alignment:
     *    -------------------------------------------------
     *    All slots reside within a single contiguous virtual memory region mapped
     *    via ::mmap(MAP_PRIVATE | MAP_ANONYMOUS).
     *    The total mapping size is rounded up to the nearest 2 MiB boundary, and
     *    ::madvise(..., MADV_HUGEPAGE) is applied.
     *    WHY IT HOLDS & MATTERS:
     *    The Linux kernel's Transparent Huge Page (THP) subsystem only collapses
     *    contiguous ranges that naturally align with 2 MiB physical compound pages.
     *    Aligning the mapping reduces kernel page-table overhead and minimizes TLB
     *    misses during direct DMA transfers (O_DIRECT).
     *
     * 2. Power-of-Two Divisibility (Chunk Boundary Guarantee):
     *    ------------------------------------------------------
     *    The slot size MUST be a power of two >= 4096 that evenly divides 1 GiB:
     *        (slot_size >= 4096) && ((slot_size & (slot_size - 1)) == 0) && ((1 GiB % slot_size) == 0)
     *    WHY IT HOLDS & MATTERS:
     *    io_uring's kernel buffer registration (io_uring_register_buffers) enforces
     *    a hard limit of 1 GiB (1ULL << 30 bytes) per registered iovec entry.
     *    To satisfy this kernel constraint on pools > 1 GiB, the contiguous mmap
     *    is chunked into 1 GiB iovecs during registration.
     *    Because 1 GiB is an exact multiple of slot_size, EVERY 1 GiB CHUNK BOUNDARY
     *    FALLS PRECISELY ON A SLOT BOUNDARY. No slot ever straddles two 1 GiB chunks.
     *    Therefore, any sub-slice [ptr, ptr + len] within a leased slot is 100%
     *    guaranteed to reside within a single registered kernel buffer entry.
     *
     * 3. Branchless Registered Index Resolution:
     *    ----------------------------------------
     *    Because the virtual mapping is contiguous and registration chunks are
     *    strictly 1 GiB (except possibly the final chunk), the registered table index
     *    for any pointer `ptr` inside the pool is computed as:
     *        buf_index = first_index_ + ((ptr - base_) >> 30)
     *    WHY IT HOLDS & MATTERS:
     *    Dividing by 1 GiB (2^30) is a single bitshift (`>> 30`). This eliminates
     *    interval trees, binary searches, or per-slot index metadata. Resolving the
     *    registration index during I/O submission is O(1) and branchless.
     *
     * 4. Intrusive Zero-Allocation Wait Queue:
     *    -------------------------------------
     *    When the pool is exhausted, acquiring coroutines suspend asynchronously.
     *    The wait node (Waiter) is embedded directly inside AcquireAwaiter, which
     *    resides in the coroutine's stack frame.
     *    WHY IT HOLDS & MATTERS:
     *    - Zero Heap Allocations: Suspending a coroutine never allocates memory.
     *    - Cancellation Safety: The wait queue is an intrusive doubly-linked list.
     *      If an awaiting coroutine is cancelled or destroyed before resumption
     *      (e.g., timeout or structured concurrency cancellation), ~AcquireAwaiter()
     *      removes itself from the queue in O(1) without memory leaks or dangling pointers.
     *
     * 5. LIFO Cache Warmth vs. FIFO Waiter Fairness:
     *    -------------------------------------------
     *    - Free slots are returned and popped via LIFO stack semantics (vector back).
     *      Slots that were recently returned remain hot in CPU L1/L2 caches when
     *      re-acquired by active tasks.
     *    - Waiting coroutines are serviced strictly FIFO (head pop).
     *      No coroutine can be starved by newly arriving acquisitions.
     *
     * 6. Non-Reentrant Resumption Hook:
     *    ------------------------------
     *    When a slot is released inside a destructor (~BufferLease()), resuming a
     *    waiting coroutine inline (h.resume()) would execute arbitrary user code
     *    directly on top of the destructor's stack frame.
     *    RegisteredBufferPool provides `set_reschedule_hook` so the IO event loop
     *    can defer waiter execution to its local task queue, preventing stack overflow.
     *
     * 7. Clean, Idempotent Shutdown:
     *    ---------------------------
     *    Calling `close()` drains the wait queue immediately, resuming all pending
     *    waiters with Error::fail_errc(std::errc::operation_canceled).
     *    Subsequent acquisitions fail immediately. Outstanding leases can still be
     *    safely destroyed after close() without errors.
     */
    class RegisteredBufferPool
    {
    public:
        static constexpr size_t kOneGiB = 1ULL << 30; // 1,073,741,824 bytes
        static constexpr size_t kHugePageSize = 2ULL * 1024 * 1024; // 2 MiB THP alignment
        static constexpr size_t kPageSize = 4096; // 4 KiB page size

        using RescheduleFn = void (*)(void* ctx, std::coroutine_handle<> h) noexcept;

        // Intrusive wait node embedded in AcquireAwaiter's frame
        struct Waiter
        {
            std::coroutine_handle<> handle{};
            Waiter* prev = nullptr;
            Waiter* next = nullptr;
            Result<BufferLease> result{Error::fail_errc(std::errc::operation_canceled, "pool closed")};
        };

        // Zero-allocation coroutine awaiter for pool acquisitions
        struct AcquireAwaiter
        {
            RegisteredBufferPool& pool;
            Waiter waiter{};

            explicit AcquireAwaiter(RegisteredBufferPool& p) noexcept : pool(p)
            {
            }

            AcquireAwaiter(const AcquireAwaiter&) = delete;
            AcquireAwaiter& operator=(const AcquireAwaiter&) = delete;
            AcquireAwaiter(AcquireAwaiter&&) = delete;
            AcquireAwaiter& operator=(AcquireAwaiter&&) = delete;

            ~AcquireAwaiter() noexcept
            {
                // O(1) clean removal if coroutine frame is destroyed before resumption
                if (waiter.handle)
                {
                    pool.remove_waiter(&waiter);
                }
            }

            bool await_ready() noexcept
            {
                if (pool.closed_)
                    return true;

                auto lease = pool.try_acquire();
                if (lease)
                {
                    waiter.result = std::move(lease);
                    return true;
                }
                return false;
            }

            void await_suspend(std::coroutine_handle<> h) noexcept
            {
                waiter.handle = h;
                pool.enqueue_waiter(&waiter);
            }

            Result<BufferLease> await_resume() noexcept
            {
                return std::move(waiter.result);
            }
        };

        /**
         * @brief Construct and map a registered buffer pool.
         * @param slot_size Size of each slot in bytes. Must be a power of two >= 4096 and divide 1 GiB.
         * @param slots Total number of slots. Must be > 0.
         * @throws std::invalid_argument if parameters violate alignment/divisibility invariants.
         * @throws std::overflow_error if total size exceeds size_t.
         * @throws std::system_error if mmap fails.
         */
        RegisteredBufferPool(size_t slot_size, uint32_t slots);

        ~RegisteredBufferPool() noexcept;

        RegisteredBufferPool(const RegisteredBufferPool&) = delete;
        RegisteredBufferPool& operator=(const RegisteredBufferPool&) = delete;
        
        RegisteredBufferPool(RegisteredBufferPool&& other) noexcept
            : base_(std::exchange(other.base_, nullptr)),
              slot_size_(std::exchange(other.slot_size_, 0)),
              slots_(std::exchange(other.slots_, 0)),
              total_bytes_(std::exchange(other.total_bytes_, 0)),
              aligned_total_bytes_(std::exchange(other.aligned_total_bytes_, 0)),
              first_index_(std::exchange(other.first_index_, 0)),
              closed_(std::exchange(other.closed_, true)),
              free_slots_(std::move(other.free_slots_)),
              wait_head_(std::exchange(other.wait_head_, nullptr)),
              wait_tail_(std::exchange(other.wait_tail_, nullptr)),
              reschedule_ctx_(std::exchange(other.reschedule_ctx_, nullptr)),
              reschedule_fn_(std::exchange(other.reschedule_fn_, nullptr))
        {
        }

        RegisteredBufferPool& operator=(RegisteredBufferPool&& other) noexcept
        {
            if (this != &other)
            {
                this->~RegisteredBufferPool();
                new (this) RegisteredBufferPool(std::move(other));
            }
            return *this;
        }


        /**
         * @brief Configure a deferred resumption hook for the owning IO worker.
         * @param ctx Opaque pointer passed to the callback (typically `this` pointer of IO).
         * @param fn Callback that enqueues the coroutine handle onto the IO worker's local task batch.
         */
        void set_reschedule_hook(void* ctx, RescheduleFn fn) noexcept
        {
            reschedule_ctx_ = ctx;
            reschedule_fn_ = fn;
        }

        /**
         * @brief Non-blocking buffer acquisition.
         * @return A valid BufferLease if available, or an empty BufferLease if exhausted/closed.
         */
        [[nodiscard]] BufferLease try_acquire() noexcept;

        /**
         * @brief Asynchronous coroutine buffer acquisition with FIFO queueing.
         * @return AcquireAwaiter yielding Result<BufferLease> upon completion.
         */
        [[nodiscard]] AcquireAwaiter acquire() noexcept { return AcquireAwaiter{*this}; }

        /**
         * @brief Return a slot back to the pool.
         *        Hands off directly to the oldest FIFO waiter if one exists,
         *        or recycles the slot into the LIFO free list.
         */
        void release(uint32_t ordinal) noexcept;

        /**
         * @brief Permanently shut down the pool.
         *        Resumes all queued waiters with std::errc::operation_canceled.
         */
        void close() noexcept;

        /**
         * @brief Append this pool's 1 GiB chunks to an io_uring registration table.
         * @param table Target vector of iovec structures for io_uring_register_buffers.
         * @return The starting table index assigned to this pool.
         */
        uint16_t append_regions(std::vector<iovec>& table);

        /**
         * @brief Fast, branchless calculation of the io_uring registered buffer index.
         * @param ptr Target pointer within the pool.
         * @return The io_uring buffer table index.
         */
        [[nodiscard]] uint16_t registration_index(const std::byte* ptr) const noexcept
        {
            assert(ptr >= base_ && ptr < base_ + total_bytes_);
            return first_index_ + static_cast<uint16_t>((ptr - base_) >> 30);
        }

        /// @brief Starting virtual address of the mapping.
        [[nodiscard]] std::byte* base() const noexcept { return base_; }

        /// @brief Uniform size of each buffer slot in bytes.
        [[nodiscard]] size_t slot_size() const noexcept { return slot_size_; }

        /// @brief Total slot capacity of the pool.
        [[nodiscard]] uint32_t capacity() const noexcept { return slots_; }

        /// @brief Currently available slots not held by any lease or waiter.
        [[nodiscard]] uint32_t available() const noexcept { return static_cast<uint32_t>(free_slots_.size()); }

        /// @brief Total mapped bytes usable for slots (excluding 2 MiB THP padding).
        [[nodiscard]] size_t total_bytes() const noexcept { return total_bytes_; }

        /// @brief Total memory allocated via mmap (rounded to 2 MiB).
        [[nodiscard]] size_t allocated_bytes() const noexcept { return aligned_total_bytes_; }

        /// @brief Check if the pool is closed.
        [[nodiscard]] bool closed() const noexcept { return closed_; }

    private:
        friend class BufferLease;
        friend struct AcquireAwaiter;

        BufferLease make_lease(uint32_t ordinal) noexcept
        {
            return BufferLease{*this, base_ + size_t{ordinal} * slot_size_, slot_size_, ordinal};
        }

        void enqueue_waiter(Waiter* w) noexcept;
        Waiter* pop_waiter() noexcept;
        void remove_waiter(Waiter* w) noexcept;
        void resume_waiter(Waiter* w) const noexcept;

        std::byte* base_ = nullptr;
        size_t slot_size_ = 0;
        uint32_t slots_ = 0;
        size_t total_bytes_ = 0;
        size_t aligned_total_bytes_ = 0;
        uint16_t first_index_ = 0;
        bool closed_ = false;

        // Free slots stack (LIFO for CPU cache locality)
        std::vector<uint32_t> free_slots_;

        // Waiters queue (intrusive doubly-linked FIFO list)
        Waiter* wait_head_ = nullptr;
        Waiter* wait_tail_ = nullptr;

        // Resumption hook
        void* reschedule_ctx_ = nullptr;
        RescheduleFn reschedule_fn_ = nullptr;
    };

    inline void BufferLease::reset() noexcept
    {
        if (pool_)
        {
            auto* p = std::exchange(pool_, nullptr);
            data_ = nullptr;
            size_ = 0;
            p->release(ordinal_);
        }
    }
} // namespace kio
