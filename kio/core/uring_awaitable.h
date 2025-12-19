// //
// // Created by Yao ACHI on 17/10/2025.
// //
//
// #ifndef KIO_URING_AWAITABLE_H
// #define KIO_URING_AWAITABLE_H
//
// #include <coroutine>
// #include <liburing.h>
// #include <type_traits>
// #include <utility>
//
// #include "worker.h"
//
// namespace kio::io
// {
//     struct IoCompletion
//     {
//         std::coroutine_handle<> handle;
//         int result{0};
//
//         void complete(const int res) {
//             result = res;
//             if (handle) handle.resume();
//         }
//     };
//
//     /**
//      * IoUringAwaitable, c++20 coroutine awaiter, controls the suspension and awaiting of
//      * the io_uring operations.
//      */
//     template<typename Prep, typename... Args>
//     struct IoUringAwaitable
//     {
//         Worker& worker_;
//         io_uring_sqe* sqe_ = nullptr;
//         uint64_t op_id_{0};
//         Prep io_uring_prep_;
//         std::tuple<Args...> args_;
//         IoCompletion completion_;
//
//         bool await_ready() const noexcept { return false; }
//
//         bool await_suspend(std::coroutine_handle<> h)  // NOLINT
//         {
//             // All async operations have to begin on the correct thread.
//             // So in case the async operations are not registered as a callback in the worker,
//             // the developer has to explicitly switch context (co_await SwitchToWorker(worker);).
//             assert(worker_.is_on_worker_thread() &&
//                    "kio::async_* operation was called from the wrong thread. "
//                    "You must co_await SwitchToWorker(worker) at the start of your task.");
//
//             completion_.handle = h;
//
//             auto& ring = kio::internal::WorkerAccess::get_ring(worker_);
//             io_uring_sqe* sqe = io_uring_get_sqe(&ring);
//
//             if (sqe_ == nullptr)
//             {
//                 // no SQE available right now; return error immediately
//                 completion_.result = -EAGAIN;
//                 return false;
//             }
//
//             // allocate an op id and record the coroutine handle
//             // Call prep function
//             std::apply([this, sqe]<typename... T0>(T0&&... unpacked) {
//                 prep_(sqe, std::forward<T0>(unpacked)...);
//             }, std::move(args_));
//
//             // Call prep sqe
//             //std::apply([this]<typename... T>(T&&... unpacked_args) { io_uring_prep_(sqe_, std::forward<T>(unpacked_args)...); }, std::move(args_));
//
//             // save the op_id to the submission entry
//             io_uring_sqe_set_data(sqe, &completion_);
//
//             return true;
//         }
//
//         [[nodiscard]] int await_resume() const noexcept
//         {
//             return completion_.result;
//         }
//
//         explicit IoUringAwaitable(Worker& worker, Prep prep, Args... args) : worker_(worker), io_uring_prep_(std::move(prep)), args_(args...) {}
//     };
//
//     template<typename Prep, typename... Args>
//     auto make_uring_awaitable(Worker& worker, Prep&& prep, Args&&... args)
//     {
//         return IoUringAwaitable<std::decay_t<Prep>, std::decay_t<Args>...>(worker, std::forward<Prep>(prep), std::forward<Args>(args)...);
//     }
//
// }  // namespace kio::io
//
// #endif  // KIO_URING_AWAITABLE_H
