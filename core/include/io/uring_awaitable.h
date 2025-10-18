//
// Created by Yao ACHI on 17/10/2025.
//

#ifndef KIO_URING_AWAITABLE_H
#define KIO_URING_AWAITABLE_H

#include <coroutine>
#include <liburing.h>
#include <type_traits>
#include <utility>

#include "worker.h"

namespace kio::io
{
    /**
     * IoUringAwaitable, c++20 coroutine awaiter, controls the suspension and awaiting of
     * the io_uring operations.
     */
    template<typename Prep, typename... Args>
    struct IoUringAwaitable
    {
        Worker& worker_;
        io_uring_sqe* sqe_ = nullptr;
        uint64_t op_id_{0};
        Prep io_uring_prep_;
        std::tuple<Args...> args_;
        int immediate_result_{0};

        bool await_ready() noexcept { return false; }  // NOLINT

        bool await_suspend(std::coroutine_handle<> h)  // NOLINT
        {
            sqe_ = io_uring_get_sqe(&worker_.get_ring());
            if (sqe_ == nullptr)
            {
                // no SQE available right now; return error immediately
                immediate_result_ = -EAGAIN;
                return false;
            }

            // allocate an op id and record the coroutine handle
            op_id_ = worker_.get_op_id();
            worker_.init_op_slot(op_id_, h);

            // Call prep sqe
            std::apply([this]<typename... T>(T&&... unpacked_args) { io_uring_prep_(sqe_, std::forward<T>(unpacked_args)...); }, args_);

            // save the op_id to the submission entry
            io_uring_sqe_set_data64(sqe_, op_id_);
            return true;
        }

        [[nodiscard]] int await_resume() const
        {
            if (immediate_result_ != 0)
            {
                return immediate_result_;
            }

            return static_cast<int>(worker_.get_op_result(op_id_));
        };

        explicit IoUringAwaitable(Worker& worker, Prep prep, Args... args) : worker_(worker), io_uring_prep_(std::move(prep)), args_(std::move(args)...) {}
    };

    template<typename Prep, typename... Args>
    auto make_uring_awaitable(Worker& worker, Prep&& prep, Args&&... args)
    {
        return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), std::forward<Args>(args)...);
    }

}  // namespace kio::io

#endif  // KIO_URING_AWAITABLE_H
