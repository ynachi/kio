#pragma once
#include <coroutine>
#include <exception>
#include <utility>
#include "absl/log/check.h"
#include "absl/log/log.h"

#include "context.h"
#include "kio/error.hpp"

namespace kio
{
    template <class T = void>
    class Task;

    namespace detail
    {
        struct PromiseBase
        {
            // The awaiting coroutine, or nullptr when nobody awaits this task.
            // Written exactly once by Task::await_suspend (CHECK-enforced) before
            // the body ever runs -- initial_suspend is suspend_always -- so there
            // is no race. The sentinel is nullptr, not a comparison against
            // std::noop_coroutine(): the standard promises nothing about handle
            // equality with noop (v1 learned this the hard way).
            std::coroutine_handle<> cont{};

            // Set by Task::await_suspend: the coroutine was actually handed
            // to a caller and will run. A Task destroyed before that is a
            // value that was never started; its frame is suspended at
            // initial_suspend, so destroying it is well-defined and must not
            // trip the "destroyed while suspended" abort.
            bool started{false};

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct Final
            {
                bool await_ready() const noexcept { return false; }

                template <class P>
                std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) const noexcept
                {
                    // Symmetric transfer to whoever awaited us. A completed task
                    // with no awaiter parks on noop_coroutine(): returning nullptr
                    // from await_suspend is UB, so the sentinel is mapped here and
                    // the parked frame stays owned by its ~Task.
                    const std::coroutine_handle<> continuation = h.promise().cont;
                    return continuation ? continuation : std::noop_coroutine();
                }

                void await_resume() const noexcept
                {
                }
            };

            Final final_suspend() noexcept { return {}; }
            [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
        };

        template <class T>
        struct ResultMixin
        {
            // The promise owns the error channel: every co_return funnels into a
            // Result<T>, and await_resume hands the whole Result to the awaiter.
            //
            // Seeded with a sentinel error instead of default-initializing:
            // expected<T,E>'s default constructor requires T to be default
            // constructible, which would silently constrain every Task<T>.
            // A completed coroutine has always run return_value (flowing off the
            // end is UB, and unhandled_exception terminates), so the sentinel is
            // only observable through a bug -- it reads as operation_canceled,
            // exactly what v1 reported for its empty optional.
            Result<T> result{std::unexpect, Error::from_errc(std::errc::operation_canceled)};

            // Set by Task::await_resume when the result is consumed. Guards
            // double-consumption and lets ~Task tell a dropped error from a read one.
            bool result_taken{false};

            // One overload, not three: expected's implicit constructors accept
            // `co_return value`, `co_return std::unexpected(err)`,
            // `co_return Result<T>(...)`, and even `co_return Error::fail_errno(...)`
            // through one converting constructor at the co_return site.
            //
            // There is deliberately no return_void here: a promise declaring both
            // is ill-formed, and bare `co_return;` on a Task<void> should be a
            // compile error so every exit names its outcome. The success form is
            // `co_return {};`.
            void return_value(Result<T> res) noexcept
            {
                result = std::move(res);
            }
        };

        template <class T>
        struct Promise : PromiseBase, ResultMixin<T>
        {
            Task<T> get_return_object() noexcept
            {
                return Task<T>(std::coroutine_handle<Promise>::from_promise(*this));
            }
        };
    } // namespace detail

    template <class T>
    class [[nodiscard]] Task
    {
    public:
        using promise_type = detail::Promise<T>;

        Task() = default;

        explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h)
        {
        }

        Task(Task&& o) noexcept : h_(std::exchange(o.h_, {}))
        {
        }

        Task& operator=(Task&& o) noexcept
        {
            if (this != &o)
            {
                reset();
                h_ = std::exchange(o.h_, {});
            }
            return *this;
        }

        Task(const Task&) = delete;
        Task& operator=(const Task&) = delete;
        ~Task() { reset(); }

        // Empty (moved-from) or completed tasks resume immediately; await_resume
        // rejects the empty case with CHECK.
        bool await_ready() const noexcept { return !h_ || h_.done(); }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept
        {
            // One parent per task: a second await while this one is suspended
            // would silently steal the first caller's continuation and strand it.
            CHECK(h_.promise().cont == nullptr) << "Task awaited by a second coroutine";
            h_.promise().cont = caller;
            h_.promise().started = true;
            return h_;
        }

        [[nodiscard]] Result<T> await_resume() noexcept
        {
            // An empty (moved-from) Task reaches here through await_ready(); an
            // unfinished one only through a bug. Terminate: a deterministic abort
            // at the bug site beats UB, and beats a silently canceled Result that
            // would travel through TRY macros disguised as real cancellation.
            CHECK(h_ && h_.done()) << "await resumed on an empty or unfinished Task";
            CHECK(!h_.promise().result_taken) << "Task result consumed twice";
            h_.promise().result_taken = true;
            return std::move(h_.promise().result);
        }

    private:
        void reset() noexcept
        {
            if (h_)
            {
                if (!h_.promise().started)
                {
                    // Never awaited: the frame is suspended at
                    // initial_suspend, so destroying it is well-defined.
                    // Dropping an unstarted Task is a legal way to give up
                    // on it -- nothing is in flight and no result exists.
                    h_.destroy();
                    h_ = {};
                    return;
                }
                CHECK(h_.done()) << "Task destroyed while suspended (abandons in-flight ops)";
                const auto& p = h_.promise();
                // Defensive: today every completion path consumes the Result in
                // await_resume (spawn's wrapper logs detached errors), so this is
                // only reachable once other abandon paths exist, e.g. when_all.
                if (!p.result_taken && !p.result.has_value()) [[unlikely]]
                {
                    LOG(ERROR) << "Task destroyed with an unconsumed error result: "
                               << p.result.error().message() << " [" << p.result.error().context()
                               << "]";
                }
                h_.destroy();
                h_ = {};
            }
        }

        std::coroutine_handle<promise_type> h_{};
    };

    namespace detail
    {
        struct Detached
        {
            struct promise_type
            {
                // Removed `Op start;` here. We can just queue the handle directly!
                Detached get_return_object() noexcept
                {
                    return Detached{std::coroutine_handle<promise_type>::from_promise(*this)};
                }

                std::suspend_always initial_suspend() noexcept { return {}; }
                std::suspend_never final_suspend() noexcept { return {}; }

                void return_void() noexcept
                {
                }

                [[noreturn]] void unhandled_exception() noexcept { std::terminate(); }
            };

            std::coroutine_handle<promise_type> h;
        };

        template <class T>
        Detached run_detached(Task<T> t)
        {
            // spawn() is the only sanctioned detach path, so this wrapper is the
            // last thing standing between a task's Result and oblivion: no one
            // else will ever read it. An error arriving here must not vanish.
            auto r = co_await t;
            if (!r) [[unlikely]]
            {
                LOG(ERROR) << "detached Task discarded an error result: " << r.error().message()
                           << " [" << r.error().context() << "]";
            }
        }
    } // namespace detail

    namespace io
    {
        template <class T>
        void Context::spawn(Task<T> task) noexcept
        {
            auto detached = detail::run_detached<T>(std::move(task));
            post(detached.h);
        }
    }
} // namespace kio
