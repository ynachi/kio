#include "uring/core/buffer_pool.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <system_error>

namespace kio
{
    RegisteredBufferPool::RegisteredBufferPool(const size_t slot_size, const uint32_t slots)
        : slot_size_(slot_size), slots_(slots)
    {
        // Invariant 2: slot_size must be a power of two >= 4096 and divide 1 GiB evenly
        if (slot_size < kPageSize || (slot_size & (slot_size - 1)) != 0 || (kOneGiB % slot_size) != 0)
        {
            throw std::invalid_argument(
                "slot_size must be a power of two >= 4096 that evenly divides 1 GiB");
        }

        if (slots == 0)
        {
            throw std::invalid_argument("slots must be > 0");
        }

        // Invariant 1: Check for multiplication overflow
        if (__builtin_mul_overflow(slot_size_, size_t{slots_}, &total_bytes_))
        {
            throw std::overflow_error("Total buffer pool size overflows size_t");
        }

        // Invariant 1: Round up to 2 MiB boundary for Transparent Huge Pages (THP)
        aligned_total_bytes_ = (total_bytes_ + kHugePageSize - 1) & ~(kHugePageSize - 1);

        // Pre-reserve free-list to guarantee that release() and try_acquire() are strictly noexcept
        free_slots_.reserve(slots_);
        for (uint32_t i = slots_; i != 0; --i)
        {
            free_slots_.push_back(i - 1);
        }

        void* mapping = ::mmap(
            nullptr,
            aligned_total_bytes_,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0);

        if (mapping == MAP_FAILED)
        {
            throw std::system_error(errno, std::system_category(), "mmap buffer pool failed");
        }

        base_ = static_cast<std::byte*>(mapping);

        // Advise kernel to back the mapping with huge pages
        (void)::madvise(base_, aligned_total_bytes_, MADV_HUGEPAGE);
    }

    RegisteredBufferPool::~RegisteredBufferPool() noexcept
    {
        close();

        if (base_ != nullptr)
        {
            (void)::munmap(base_, aligned_total_bytes_);
            base_ = nullptr;
        }
    }

    BufferLease RegisteredBufferPool::try_acquire() noexcept
    {
        if (closed_ || free_slots_.empty())
        {
            return {};
        }

        // LIFO pop: returns the most recently used slot for CPU cache warmth
        const uint32_t ordinal = free_slots_.back();
        free_slots_.pop_back();

        return make_lease(ordinal);
    }

    void RegisteredBufferPool::release(const uint32_t ordinal) noexcept
    {
        assert(ordinal < slots_);

        if (!closed_ && wait_head_ != nullptr)
        {
            // Direct handoff to the oldest FIFO waiter without passing through free_slots_
            auto* waiter = pop_waiter();
            waiter->result = make_lease(ordinal);
            resume_waiter(waiter);
            return;
        }

        // LIFO push: keeps recently freed memory warm in cache
        free_slots_.push_back(ordinal);
    }

    void RegisteredBufferPool::close() noexcept
    {
        if (closed_)
        {
            return;
        }

        closed_ = true;

        // Wake all pending FIFO waiters with operation_canceled
        while (auto* waiter = pop_waiter())
        {
            waiter->result = Error::fail_errc(std::errc::operation_canceled, "pool closed");
            resume_waiter(waiter);
        }
    }

    uint16_t RegisteredBufferPool::append_regions(std::vector<iovec>& table)
    {
        first_index_ = static_cast<uint16_t>(table.size());

        // Invariant 2 & 3: Slice contiguous mapping into <= 1 GiB iovec chunks
        for (size_t offset = 0; offset < total_bytes_; offset += kOneGiB)
        {
            const size_t len = std::min(kOneGiB, total_bytes_ - offset);
            table.push_back(iovec{
                .iov_base = base_ + offset,
                .iov_len = len
            });
        }

        return first_index_;
    }

    void RegisteredBufferPool::enqueue_waiter(Waiter* w) noexcept
    {
        assert(w != nullptr);
        w->next = nullptr;
        w->prev = wait_tail_;

        if (wait_tail_ != nullptr)
        {
            wait_tail_->next = w;
        }
        else
        {
            wait_head_ = w;
        }
        wait_tail_ = w;
    }

    RegisteredBufferPool::Waiter* RegisteredBufferPool::pop_waiter() noexcept
    {
        if (wait_head_ == nullptr)
        {
            return nullptr;
        }

        auto* w = wait_head_;
        wait_head_ = w->next;

        if (wait_head_ != nullptr)
        {
            wait_head_->prev = nullptr;
        }
        else
        {
            wait_tail_ = nullptr;
        }

        w->next = nullptr;
        w->prev = nullptr;
        return w;
    }

    void RegisteredBufferPool::remove_waiter(Waiter* w) noexcept
    {
        assert(w != nullptr);

        if (w->prev != nullptr)
        {
            w->prev->next = w->next;
        }
        else if (wait_head_ == w)
        {
            wait_head_ = w->next;
        }

        if (w->next != nullptr)
        {
            w->next->prev = w->prev;
        }
        else if (wait_tail_ == w)
        {
            wait_tail_ = w->prev;
        }

        w->next = nullptr;
        w->prev = nullptr;
        w->handle = {};
    }

    void RegisteredBufferPool::resume_waiter(Waiter* w) const noexcept
    {
        assert(w != nullptr);
        const auto h = w->handle;
        w->handle = {}; // Mark unlinked/consumed

        if (h)
        {
            if (reschedule_fn_ != nullptr)
            {
                // Invariant 6: Reschedule through IO worker to prevent stack overflow
                reschedule_fn_(reschedule_ctx_, h);
            }
            else
            {
                h.resume();
            }
        }
    }
} // namespace kio
