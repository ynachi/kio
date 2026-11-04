#pragma once

#include "absl/log/check.h"
#include "detail/trivial_ring_buffer.h"

#include "kio/error.hpp"

#include <algorithm>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/uio.h>

namespace kio
{
    class RegisteredBufferPool;

    // ============================================================================
    // BufferLease: Movable, non-copyable RAII handle to a pooled buffer slot
    // ============================================================================
    // An indivisible RAII lease representing exclusive ownership of one slot in
    // a RegisteredBufferPool. Usable like a normal buffer: bytes()/first()/
    // subspan()/span conversions, so ordinary span code works on it.
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

        /// @brief First n bytes of the slot. Same contract as std::span::first: n <= size().
        [[nodiscard]] std::span<std::byte> first(const size_t n) noexcept
        {
            CHECK(n <= size_) << "BufferLease::first: n=" << n << " exceeds slot size " << size_;
            return bytes().first(n);
        }

        /// @brief First n bytes of the slot. Same contract as std::span::first: n <= size().
        [[nodiscard]] std::span<const std::byte> first(const size_t n) const noexcept
        {
            CHECK(n <= size_) << "BufferLease::first: n=" << n << " exceeds slot size " << size_;
            return bytes().first(n);
        }

        /// @brief Sub-range [off, off+n) of the slot. Same contract as std::span::subspan.
        [[nodiscard]] std::span<std::byte> subspan(const size_t off, const size_t n) noexcept
        {
            CHECK(off <= size_ && n <= size_ - off)
                << "BufferLease::subspan: [" << off << ", " << off << "+" << n << ") outside slot of " << size_;
            return bytes().subspan(off, n);
        }

        [[nodiscard]] std::span<const std::byte> subspan(const size_t off, const size_t n) const noexcept
        {
            CHECK(off <= size_ && n <= size_ - off)
                << "BufferLease::subspan: [" << off << ", " << off << "+" << n << ") outside slot of " << size_;
            return bytes().subspan(off, n);
        }

        /// @brief Explicit span conversions for span-taking APIs.
        [[nodiscard]] explicit operator std::span<std::byte>() noexcept { return bytes(); }
        [[nodiscard]] explicit operator std::span<const std::byte>() const noexcept { return bytes(); }

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
    // High-performance, single-threaded buffer pool registered with io_uring.
    //
    // Memory Layout & 1 GiB Registration Slicing:
    //
    // base_
    //   |
    //   v 0                   1 GiB                 2 GiB               2.5 GiB       aligned (2 MiB)
    //   +---------------------+---------------------+---------------------+--------------+---+
    //   |      Chunk 0        |      Chunk 1        |      Chunk 2        |   Chunk 3    |PAD|
    //   |     (1024 MiB)      |     (1024 MiB)      |     (1024 MiB)      |  (512 MiB)   |   |
    //   +---------------------+---------------------+---------------------+--------------+---+
    //   ^                     ^                     ^                     ^
    //   | iovec[0]            | iovec[1]            | iovec[2]            | iovec[3]
    //   | buf_index = 0       | buf_index = 1       | buf_index = 2       | buf_index = 3
    //
    // Fast branchless lookup:
    //   buf_index = first_index_ + ((ptr - base_) >> 30)
    //
    // Core Architectural Invariants:
    //
    // 1. Single Contiguous Mapping, 2 MiB-aligned Base AND Length (THP):
    //    The mapping is placed so base_ itself sits on a 2 MiB boundary (one
    //    guard 2 MiB is mapped, the base rounded up inside it, head/tail
    //    slivers returned), and the length is rounded up to 2 MiB. Then
    //    MADV_HUGEPAGE lets THP collapse the region into 2 MiB compound
    //    pages -- fewer page-table walks and TLB misses during DMA.
    //
    // 2. Power-of-Two Divisibility (Chunk Boundary Guarantee):
    //    slot_size must be a power of two >= 4096 that divides 1 GiB.
    //    io_uring caps each registered iovec at 1 GiB, so pools larger than
    //    that are registered as consecutive 1 GiB iovecs; because 1 GiB is a
    //    multiple of slot_size, every chunk boundary falls exactly on a slot
    //    boundary -- no slot ever straddles two registered entries, so any
    //    sub-slice of a lease lives in exactly one registered buffer.
    //
    // 3. Branchless Registered Index Resolution:
    //    buf_index = first_index_ + ((ptr - base_) >> 30) -- one shift, no
    //    interval trees, no per-slot metadata.
    //
    // 4. Coroutine-Thin Wait Queue (reserved-wake protocol):
    //    When exhausted, acquire() parks the caller's handle in a plain FIFO
    //    (detail::ReadyRing) and nothing else -- no intrusive nodes, no
    //    result channel into parked frames, no unlink-on-destroy. release()
    //    pushes the slot to the free list and, if waiters exist, pops the
    //    oldest handle, counts it in reserved_, and wakes it through the
    //    callback the owning Context installed (Context::post -- Phase 1 of
    //    the run loop, NEVER inline on the releaser's stack). The woken
    //    coroutine consumes its reservation in await_resume and retries
    //    try_acquire. Free-list slots covered by reserved_ are not available
    //    to new arrivals (the surplus rule in try_acquire), so the woken
    //    waiter's retry always succeeds and wakes are issued strictly FIFO.
    //    Handles in the queue cannot dangle: kio Tasks CHECK-abort when
    //    destroyed while suspended, and detached tasks run to completion.
    //    This pool RELIES on that Task-lifetime invariant.
    //
    // 5. LIFO Cache Warmth vs FIFO Waiter Fairness:
    //    Free slots recycle via a LIFO stack (recently returned slots stay
    //    hot in cache); waiters are serviced strictly FIFO. Both hold under
    //    the reserved-wake protocol: surplus slots (beyond reserved_) go to
    //    new arrivals via the LIFO stack, reserved slots to woken waiters in
    //    wake order.
    //
    // 6. Non-Reentrant Resumption:
    //    Wakes go exclusively through the installed callback, so no user
    //    coroutine ever runs on top of ~BufferLease()/release(). Parking on a
    //    pool with no callback installed is a CHECK failure at the bug site
    //    (park()), not a silent inline resume.
    //
    // 7. Clean, Idempotent Shutdown:
    //    close() wakes every still-parked waiter; each resumes into a
    //    std::errc::operation_canceled Result. Waiters already woken (popped
    //    at wake time) resume into the same branch on their own. Outstanding
    //    leases remain safe to destroy after close().
    class RegisteredBufferPool
    {
    public:
        static constexpr size_t kOneGiB = 1ULL << 30U; // 1,073,741,824 bytes
        static constexpr size_t kHugePageSize = 2ULL * 1024 * 1024; // 2 MiB THP alignment
        static constexpr size_t kPageSize = 4096; // 4 KiB page size

        using WakeFn = void (*)(void* ctx, std::coroutine_handle<> h) noexcept;

        // Coroutine-thin awaiter: the fast path fills fast_; the parking path
        // pushes a plain handle; the woken path consumes its reservation and
        // retries try_acquire (Invariants 4 & 5).
        struct AcquireAwaiter
        {
            RegisteredBufferPool& pool;
            BufferLease fast_{}; // set only on the await_ready fast path

            explicit AcquireAwaiter(RegisteredBufferPool& p) noexcept : pool(p)
            {
            }

            AcquireAwaiter(const AcquireAwaiter&) = delete;
            AcquireAwaiter& operator=(const AcquireAwaiter&) = delete;
            AcquireAwaiter(AcquireAwaiter&&) = delete;
            AcquireAwaiter& operator=(AcquireAwaiter&&) = delete;

            // No destructor: there is no node to unlink. A parked coroutine
            // cannot be destroyed while suspended (Task-lifetime invariant).

            bool await_ready() noexcept
            {
                if (pool.closed_)
                {
                    return true;
                }
                fast_ = pool.try_acquire();
                return static_cast<bool>(fast_);
            }

            void await_suspend(std::coroutine_handle<> h) noexcept { pool.park(h); }

            Result<BufferLease> await_resume() noexcept
            {
                if (fast_)
                {
                    return std::move(fast_);
                }
                if (pool.closed_)
                {
                    return Error::fail_errc(std::errc::operation_canceled, "pool closed");
                }
                // Woken path: consume the reservation release() made for us,
                // then take the slot it backed. Both CHECKs are invariants,
                // not error paths.
                CHECK(pool.reserved_ > 0) << "woken pool waiter without a reservation";
                --pool.reserved_;
                auto lease = pool.try_acquire();
                CHECK(lease) << "woken pool waiter found no free slot";
                return lease;
            }
        };

        RegisteredBufferPool(size_t slot_size, uint32_t slots);
        ~RegisteredBufferPool() noexcept;

        RegisteredBufferPool(const RegisteredBufferPool&) = delete;
        RegisteredBufferPool& operator=(const RegisteredBufferPool&) = delete;
        // Pinned, registered memory plus parked waiters make the pool
        // address-fixed: moving is meaningless (and was unsound with
        // waiters). Construct it where it will live.
        RegisteredBufferPool(RegisteredBufferPool&&) = delete;
        RegisteredBufferPool& operator=(RegisteredBufferPool&&) = delete;

        /// @brief Install the wake callback (the owning Context at setup).
        ///        The ONLY resume path for parked waiters: Context::post,
        ///        i.e. Phase 1 of the run loop.
        void set_wake(void* ctx, WakeFn fn) noexcept
        {
            wake_ctx_ = ctx;
            wake_fn_ = fn;
        }

        /// @brief Non-blocking acquisition. New arrivals may only take the
        ///        SURPLUS free slots (those not reserved for woken waiters).
        /// @return A valid BufferLease, or an empty one when exhausted,
        ///         closed, or every free slot is reserved for a waiter.
        [[nodiscard]] BufferLease try_acquire() noexcept;

        /// @brief Asynchronous acquisition with FIFO queueing.
        [[nodiscard]] AcquireAwaiter acquire() noexcept { return AcquireAwaiter{*this}; }

        /// @brief Permanently shut down: wake all parked waiters with
        ///        operation_canceled. Idempotent; leases stay releasable.
        void close() noexcept;

        /// @brief Append this pool's <= 1 GiB chunks to a registration table.
        /// @return The starting table index assigned to this pool.
        uint16_t append_regions(std::vector<iovec>& table);

        /// @brief Branchless registered-buffer index for a pointer in the pool.
        [[nodiscard]] uint16_t registration_index(const std::byte* ptr) const noexcept
        {
            CHECK(ptr >= base_ && ptr < base_ + total_bytes_) << "registration_index on a pointer outside the pool mapping";
            const auto offset = static_cast<size_t>(ptr - base_);
            const size_t index = size_t{first_index_} + (offset >> 30U);
            CHECK(index <= 65535) << "registered buffer index overflows the SQE buf_index field";
            return static_cast<uint16_t>(index);
        }

        /// @brief Coroutines currently parked on acquire(). Feeds run()'s
        ///        quiescence check: parked waiters keep the loop alive.
        [[nodiscard]] size_t waiting() const noexcept { return waiters_.size(); }

        [[nodiscard]] std::byte* base() const noexcept { return base_; }
        [[nodiscard]] size_t slot_size() const noexcept { return slot_size_; }
        [[nodiscard]] uint32_t capacity() const noexcept { return slots_; }

        /// @brief Slots in the free list (some may be reserved for woken
        ///        waiters -- see try_acquire's surplus rule).
        [[nodiscard]] uint32_t available() const noexcept { return static_cast<uint32_t>(free_slots_.size()); }

        [[nodiscard]] size_t total_bytes() const noexcept { return total_bytes_; }
        [[nodiscard]] size_t allocated_bytes() const noexcept { return aligned_total_bytes_; }
        [[nodiscard]] bool closed() const noexcept { return closed_; }

    private:
        friend class BufferLease;
        friend struct AcquireAwaiter;

        // The ONLY legal release path is BufferLease::reset() (a friend).
        // Private because a public release would let a caller return the
        // same ordinal twice: two leases on one slot (silent corruption)
        // and a free-list push past its reserve (realloc inside a noexcept
        // function -> terminate). If a manual return is ever wanted, it
        // must be a consuming operation, not a raw ordinal.
        //
        /// @brief Return a slot: LIFO-push, then wake the oldest waiter if
        ///        one is parked (reserved-wake protocol, Invariant 4).
        void release(uint32_t ordinal) noexcept;

        BufferLease make_lease(uint32_t ordinal) noexcept
        {
            return BufferLease{*this, base_ + size_t{ordinal} * slot_size_, slot_size_, ordinal};
        }

        void park(std::coroutine_handle<> h) noexcept
        {
            CHECK(wake_fn_ != nullptr) << "coroutine parked on a pool with no wake callback installed";
            waiters_.push(h);
        }

        std::byte* base_ = nullptr;
        size_t slot_size_ = 0;
        uint32_t slots_ = 0;
        size_t total_bytes_ = 0;
        size_t aligned_total_bytes_ = 0;
        uint16_t first_index_ = 0;
        bool closed_ = false;

        // Free slots stack (LIFO for CPU cache locality). Pre-reserved so
        // release()/try_acquire() stay noexcept in steady state.
        std::vector<uint32_t> free_slots_;

        // Parked acquire() handles, FIFO. Woken-but-not-yet-resumed waiters
        // have already left this queue and are counted in reserved_ instead.
        detail::ReadyRing waiters_{};
        size_t reserved_ = 0;

        // Wake callback: installed by the owning Context (Context::post) --
        // the ONLY resume path, never inline on the releaser's stack.
        void* wake_ctx_ = nullptr;
        WakeFn wake_fn_ = nullptr;
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
