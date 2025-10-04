#ifndef CORO_H
#define CORO_H

#include <coroutine>
#include <exception>
#include <utility>
#include <variant>

#include"spdlog/spdlog.h"

namespace kio {
    template<typename T = void>
    struct Task {
        struct promise_type;

        std::coroutine_handle<promise_type> h_;

        explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
        Task(Task &&other) noexcept : h_(std::exchange(other.h_, {})) {}
        Task(const Task &) = delete;
        Task &operator=(const Task &) = delete;
        Task &operator=(Task &&other) noexcept {
            if (this != &other) {
                if (h_)
                    h_.destroy();
                h_ = other.h_;
                other.h_ = nullptr;
            }
            return *this;
        }
        ~Task() {
            if (h_) {
                h_.destroy();
            }
        }


        struct promise_type {
            std::variant<std::monostate, T, std::exception_ptr> result_;
            std::coroutine_handle<> continuation_;

            [[nodiscard]]
            Task get_return_object() noexcept {
                return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // we are creating lazy coroutines
            // Should be std::suspend_always to make the task "lazy" (it doesn't run until awaited).
            static std::suspend_always initial_suspend() noexcept { return {}; }
            // Crucially, this awaiter must resume any other coroutine that is co_awaiting this task's completion. This
            // is how you chain asynchronous operations together.
            // This awaiter is crucial for chaining coroutines.
            // When this task completes, it resumes whoever was awaiting it.
            struct final_awaiter {
                static bool await_ready() noexcept { return false; }
                std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                    // Resume the continuation or return to the original caller if there is none.
                    if (auto continuation = h.promise().continuation_)
                        return continuation;
                    return std::noop_coroutine();
                }
                static void await_resume() noexcept {}
            };

            static final_awaiter final_suspend() noexcept { return {}; }
            // Stores the exception pointer so it can be re-thrown by the awaiting coroutine.
            void unhandled_exception() { result_ = std::current_exception(); }

            // Stores the final result in the promise so the awaiting coroutine can retrieve it
            void return_value(T value) { result_ = std::move(value); }
            void return_void()
                requires std::is_void_v<T>
            {
                result_ = std::monostate{};
            }
        };

        [[nodiscard]]
        bool await_ready() const noexcept {
            // A task is ready if it's already done.
            return h_.done();
        }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting_coroutine) noexcept {
            // Store the awaiting coroutine as our continuation
            h_.promise().continuation_ = awaiting_coroutine;
            // Resume our handle to start the task's execution
            return h_;
        }

        T await_resume() {
            if (h_.promise().result_.index() == 2) {
                std::rethrow_exception(std::get<2>(h_.promise().result_));
            }
            // return void or value depending on the case
            if constexpr (std::is_void_v<T>) {
                return;
            } else {
                return std::get<1>(std::move(h_.promise().result_));
            }
        }
    };

    struct DetachedTask {
        struct promise_type {
            DetachedTask get_return_object() noexcept {
                return DetachedTask{std::coroutine_handle<promise_type>::from_promise(*this)};
            }
            static std::suspend_never initial_suspend() noexcept { return {}; }
            static std::suspend_never final_suspend() noexcept { return {}; }
            static void return_void() noexcept {}
            static void unhandled_exception() noexcept {
                // swallow or log exception
                try {
                    throw;
                } catch (const std::exception &e) {
                    spdlog::error("DetachedTask exception: {}", e.what());
                } catch (...) {
                    spdlog::error("DetachedTask unknown exception");
                }
            }
        };

        using handle_t = std::coroutine_handle<promise_type>;
        handle_t coro;

        explicit DetachedTask(handle_t h) : coro(h) {}
        DetachedTask(DetachedTask &&o) noexcept : coro(std::exchange(o.coro, {})) {}
        ~DetachedTask() {
            if (coro)
                coro.destroy();
        }

        void detach() && noexcept {
            // Forget the handle → let coroutine run to completion
            coro = {};
        }
    };


    /**
     * IoUringAwaitable, c++20 coroutine awaiter, controls the suspension and awaiting of
     * the io_uring operations.
     */
    struct IoUringAwaitable {
        io_uring_sqe *sqe_;
        std::function<void(io_uring_sqe *sqe)> prep_fn_;
        std::coroutine_handle<> handle_ = nullptr;
        int result_{0};

        static bool await_ready() noexcept { return false; }

        void await_suspend(std::coroutine_handle<> handle) noexcept {
            // null sqe are forbidden
            if (sqe_ == nullptr) {
                LOG(ERROR) << "Submission queue full: " << strerror(-errno);
                // resume immediately in case of error
                result_ = -EAGAIN;
                handle.resume();
                return;
            }

            // prepare io_uring ops
            prep_fn_(sqe_);
            handle_ = handle;
            // now set this as user data
            io_uring_sqe_set_data(sqe_, this);
            // do not submit the here, we do batch submission
        }

        [[nodiscard]] int await_resume() const noexcept { return result_; };
    };

} // namespace kio

#endif // CORO_H
