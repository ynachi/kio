//
// Created by Yao ACHI on 10/01/2026.
//

#ifndef KIO_CORE_IO_URING_AWAITABLE_H
#define KIO_CORE_IO_URING_AWAITABLE_H
#include "errors.h"
#include "op_pool.h"

#include <expected>
#include <optional>
#include <tuple>

namespace kio::io
{
class Worker;

/**
 * IoUringAwaitable - Using OpPool for zero-alloc operation tracking
 */
template <typename Prep, typename... Args>
struct IoUringAwaitable
{
    Worker& worker;
    Prep io_uring_prep;
    using OnSuccess = void (*)(Worker&, int);
    OnSuccess on_success;
    std::tuple<Args...> io_args;

    // RAII handle to the operation slot.
    // When this awaitable is destroyed (even if cancelled), the slot is released.
    std::optional<OpPool::OpHandle> op_handle;

    bool await_ready() const noexcept { return false; }  // NOLINT

    bool await_suspend(std::coroutine_handle<> h);  // NOLINT

    [[nodiscard]] Result<int> await_resume() noexcept  // NOLINT
    {
        if (!op_handle)
        {
            return std::unexpected(Error::FromErrno(ECANCELED));
        }

        // Retrieve result from the pool using our ID
        // Note: The slot is still valid because op_handle is alive.
        // We must access the pool pointer from op_handle (which we know is valid if op_handle has value)
        OpPool::OpSlot* slot = op_handle->pool->Get(op_handle->GetID());

        // If slot is null here, it means the pool was corrupted or reset (unlikely in normal flow)
        if (slot == nullptr)
        {
            return std::unexpected(Error::FromErrno(EIO));
        }

        int const res = slot->result;

        // Release the slot immediately by destroying the handle
        op_handle.reset();

        if (res < 0)
        {
            return std::unexpected(Error::FromErrno(-res));
        }
        if (on_success != nullptr)
        {
            on_success(worker, res);
        }
        return res;
    }

    explicit IoUringAwaitable(Worker& worker, Prep prep, OnSuccess on_success, Args... args)
        : worker(worker), io_uring_prep(std::move(prep)), on_success(on_success), io_args(args...)
    {
    }

    IoUringAwaitable(const IoUringAwaitable&) = delete;
    IoUringAwaitable& operator=(const IoUringAwaitable&) = delete;
    IoUringAwaitable(IoUringAwaitable&&) = delete;
    IoUringAwaitable& operator=(IoUringAwaitable&&) = delete;
};

template <typename Prep, typename... Args>
auto MakeUringAwaitable(Worker& worker, Prep&& prep, void (*on_success)(Worker&, int), Args&&... args)
{
    return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), on_success,
                                                                       std::forward<Args>(args)...);
}

template <typename Prep, typename... Args>
auto MakeUringAwaitable(Worker& worker, Prep&& prep, Args&&... args)
{
    return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), nullptr,
                                                                       std::forward<Args>(args)...);
}
}  // namespace kio::io

#endif  // KIO_CORE_IO_URING_AWAITABLE_H