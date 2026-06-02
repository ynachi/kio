#pragma once
//
// iouring_coro.h - a small, high-performance stackful coroutine I/O library.
//
// Single-header build: stackful fibers via Boost.Context (fcontext) driven by
// an io_uring event loop. Targets Linux 6+ and C++23.
//
// Usage:
//   #include "iouring_coro.h"
//   namespace io = iouring_coro;
//
//   io::scheduler sched;
//   int listener = io::make_tcp_listener(9000);
//   sched.spawn([listener] {
//       int fd = *io::accept(listener);
//       ...
//   });
//   sched.run();
//
// Link against:  -luring -lboost_context
//
#include <boost/context/detail/fcontext.hpp>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <expected>
#include <functional>
#include <new>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

// Older liburing headers may lack the modern setup flags. Define them to 0 so
// the code still compiles; at runtime we feature-probe and fall back anyway.
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER 0
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN 0
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN 0
#endif

// ===========================================================================
// Low-level Boost.Context (fcontext) API
// ===========================================================================
//
// We declare the three assembly entry points ourselves rather than pulling in
// <boost/context/...>. The symbols below have C linkage and a stable ABI, so a
// hand-written declaration links cleanly against -lboost_context while keeping
// this header free of Boost's internal machinery.
//
namespace iouring_coro::detail
{
    using fcontext_t = boost::context::detail::fcontext_t;
    using transfer_t = boost::context::detail::transfer_t;
    using boost::context::detail::jump_fcontext;
    using boost::context::detail::make_fcontext;
    using boost::context::detail::ontop_fcontext;
} // namespace iouring_coro::detail

namespace iouring_coro
{
    // ===========================================================================
    // Stack allocation
    // ===========================================================================
    //
    // Each fiber needs its own stack. We mmap an anonymous region and mprotect the
    // lowest page to PROT_NONE so that a stack overflow (stacks grow downward)
    // faults immediately with SIGSEGV instead of silently corrupting another
    // fiber's memory.
    //
    struct stack_context
    {
        void* sp = nullptr; // top of usable stack (highest address)
        std::size_t size = 0; // usable size in bytes (excludes guard page)
        void* base = nullptr; // mmap base (lowest address, the guard page)
        std::size_t total = 0; // full mapping size (guard + usable)
    };

    class stack_allocator
    {
    public:
        static constexpr std::size_t default_size = 128 * 1024; // 128 KiB

        static std::size_t page_size() noexcept
        {
            static const std::size_t pg =
                static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
            return pg;
        }

        // Round a requested size up to a multiple of the page size.
        static std::size_t round_up(std::size_t n) noexcept
        {
            const std::size_t pg = page_size();
            return (n + pg - 1) & ~(pg - 1);
        }

        static stack_context allocate(std::size_t requested = default_size)
        {
            const std::size_t pg = page_size();
            const std::size_t usable = round_up(requested);
            const std::size_t total = usable + pg; // one extra page for the guard

            void* base = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
            if (base == MAP_FAILED) throw std::bad_alloc{};

            // Guard page sits at the lowest address; the stack grows down into it.
            if (::mprotect(base, pg, PROT_NONE) != 0)
            {
                const int err = errno;
                ::munmap(base, total);
                throw std::system_error(err, std::system_category(),
                                        "stack guard mprotect");
            }

            stack_context ctx;
            ctx.base = base;
            ctx.total = total;
            ctx.size = usable;
            ctx.sp = static_cast<char*>(base) + total; // highest address
            return ctx;
        }

        static void deallocate(stack_context& ctx) noexcept
        {
            if (ctx.base)
            {
                ::munmap(ctx.base, ctx.total);
                ctx.base = nullptr;
                ctx.sp = nullptr;
            }
        }
    };

    // ===========================================================================
    // Fiber: a stackful, cooperatively-scheduled unit of execution
    // ===========================================================================
    //
    // Control-flow model (the classic fcontext dance):
    //
    //   scheduler                       fiber
    //   ---------                       -----
    //   resume(): jump_fcontext(ctx_) ->  trampoline()/return-from-suspend()
    //                                      runs user code...
    //   <- jump_fcontext(return_ctx_)   suspend(): yields back to scheduler
    //
    // Every jump hands the *caller's* continuation to the *callee*, so each side
    // refreshes its idea of where the other currently is:
    //   * the scheduler stores the fiber's continuation in ctx_
    //   * the fiber stores the scheduler's continuation in return_ctx_
    //
    class fiber
    {
    public:
        using fn_t = std::move_only_function<void()>;

        // The stack is owned elsewhere (the scheduler pools it); the fiber only
        // borrows it for the lifetime of make_fcontext-derived contexts.
        fiber(fn_t fn, stack_context stk)
            : fn_(std::move(fn)), stack_(stk)
        {
            ctx_ = detail::make_fcontext(stack_.sp, stack_.size, &fiber::trampoline);
        }

        fiber(const fiber&) = delete;
        fiber& operator=(const fiber&) = delete;

        // Enter the fiber. Called only by the scheduler, only when not done().
        void resume()
        {
            detail::transfer_t t = detail::jump_fcontext(ctx_, this);
            ctx_ = t.fctx; // remember where the fiber is now suspended
        }

        // Yield back to the scheduler. Called only from inside the fiber.
        void suspend()
        {
            detail::transfer_t t = detail::jump_fcontext(return_ctx_, this);
            return_ctx_ = t.fctx; // refresh the scheduler's current continuation
        }

        [[nodiscard]] bool done() const noexcept { return finished_; }
        [[nodiscard]] std::exception_ptr exception() const noexcept { return ex_; }
        [[nodiscard]] const stack_context& stack() const noexcept { return stack_; }

    private:
        static void trampoline(detail::transfer_t t)
        {
            auto* self = static_cast<fiber*>(t.data);
            self->return_ctx_ = t.fctx; // the scheduler's continuation
            try
            {
                self->fn_();
            }
            catch (...)
            {
                self->ex_ = std::current_exception();
            }
            self->finished_ = true;
            // Final hand-off; the scheduler sees done()==true and never resumes us.
            detail::jump_fcontext(self->return_ctx_, self);
            __builtin_unreachable();
        }

        fn_t fn_;
        stack_context stack_;
        detail::fcontext_t ctx_ = nullptr; // fiber's suspended continuation
        detail::fcontext_t return_ctx_ = nullptr; // scheduler's continuation
        std::exception_ptr ex_ = nullptr;
        bool finished_ = false;
    };

    // ===========================================================================
    // Scheduler: owns one io_uring instance, drives the run loop on one thread
    // ===========================================================================
    //
    // This single-issuer design is exactly what io_uring is fastest at: no locks
    // on the submission path, and the kernel can take the DEFER_TASKRUN /
    // SINGLE_ISSUER fast paths.
    //
    // Loop shape:
    //   1. Run every ready fiber to its next suspension point. While running they
    //      prep SQEs (without submitting) and call await(), which parks them.
    //   2. Once no fiber is ready, submit the whole batch of SQEs at once and wait
    //      for at least one completion (io_uring_submit_and_wait).
    //   3. Reap all available CQEs, mark the owning fibers ready, repeat.
    //
    // Batching submissions across all fibers in step 2 is what keeps the syscall
    // count low under load: thousands of in-flight operations, a handful of
    // io_uring_enter calls.
    //
    class scheduler;

    namespace detail
    {
        // The currently-running scheduler on this thread. Lets the free-function I/O
        // API (read/write/accept/...) find the ring without threading a handle through
        // every call.
        inline thread_local scheduler* tls_sched = nullptr;
    } // namespace detail

    // One in-flight operation. Lives on the issuing fiber's stack (safe, because a
    // stackful fiber's locals persist across suspension) and is referenced by the
    // SQE's user_data. Filled in by the scheduler when the matching CQE arrives.
    struct io_operation
    {
        fiber* fib = nullptr;
        int res = 0; // cqe->res: >=0 on success, negative errno on failure
        unsigned flags = 0; // cqe->flags
    };

    class scheduler
    {
    public:
        explicit scheduler(unsigned entries = 1024)
        {
            io_uring_params params{};
            params.flags = IORING_SETUP_SINGLE_ISSUER |
                IORING_SETUP_DEFER_TASKRUN |
                IORING_SETUP_COOP_TASKRUN;
            int rc = io_uring_queue_init_params(entries, &ring_, &params);
            if (rc < 0)
            {
                // Kernel too old for the fast-path flags: retry with defaults.
                io_uring_params plain{};
                rc = io_uring_queue_init_params(entries, &ring_, &plain);
                if (rc < 0)
                {
                    throw std::system_error(-rc, std::system_category(),
                                            "io_uring_queue_init_params");
                }
            }
        }

        ~scheduler()
        {
            io_uring_queue_exit(&ring_);
            for (auto& s : stack_pool_) stack_allocator::deallocate(s);
        }

        scheduler(const scheduler&) = delete;
        scheduler& operator=(const scheduler&) = delete;

        // Launch a new fiber. Safe to call before run() (to seed the system) or
        // from inside a running fiber (to spawn children).
        void spawn(fiber::fn_t fn,
                   std::size_t stack_size = stack_allocator::default_size)
        {
            stack_context stk = acquire_stack(stack_size);
            auto* f = new fiber(std::move(fn), stk);
            ready_.push_back(f);
        }

        // Run until there is no ready fiber and nothing in flight. Re-throws the
        // first uncaught exception that escaped a fiber.
        void run()
        {
            detail::tls_sched = this;
            struct guard
            {
                ~guard() { detail::tls_sched = nullptr; }
            } g;

            while (!ready_.empty() || inflight_ > 0)
            {
                while (!ready_.empty())
                {
                    fiber* f = ready_.front();
                    ready_.pop_front();
                    current_ = f;
                    f->resume();
                    current_ = nullptr;

                    if (f->done())
                    {
                        std::exception_ptr ex = f->exception();
                        stack_context stk = f->stack();
                        delete f;
                        release_stack(stk);
                        if (ex) std::rethrow_exception(ex);
                    }
                }
                if (inflight_ > 0) submit_and_reap();
            }
        }

        // ---- API used by the I/O free functions -------------------------------

        [[nodiscard]] fiber* current() const noexcept { return current_; }

        // Obtain an SQE, flushing the queue to the kernel if the ring is full.
        io_uring_sqe* get_sqe()
        {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr) [[unlikely]]
            {
                io_uring_submit(&ring_);
                sqe = io_uring_get_sqe(&ring_);
            }
            return sqe;
        }

        // Park the current fiber until `op` completes; returns the cqe result.
        // The caller must have already prepped an SQE with user_data == &op.
        int await(io_operation& op)
        {
            op.fib = current_;
            ++inflight_;
            current_->suspend();
            return op.res;
        }

        // Cooperative reschedule: go to the back of the ready queue.
        void yield()
        {
            ready_.push_back(current_);
            current_->suspend();
        }

    private:
        void submit_and_reap()
        {
            // Submit everything prepped so far and block until >=1 completion.
            int rc = io_uring_submit_and_wait(&ring_, 1);
            if (rc < 0 && rc != -EINTR)
            {
                throw std::system_error(-rc, std::system_category(),
                                        "io_uring_submit_and_wait");
            }

            unsigned head;
            io_uring_cqe* cqe;
            unsigned reaped = 0;
            io_uring_for_each_cqe(&ring_, head, cqe)
            {
                auto* op = static_cast<io_operation*>(io_uring_cqe_get_data(cqe));
                if (op != nullptr)
                {
                    op->res = cqe->res;
                    op->flags = cqe->flags;
                    ready_.push_back(op->fib);
                    --inflight_;
                }
                ++reaped;
            }
            io_uring_cq_advance(&ring_, reaped);
        }

        // ---- stack pooling ----------------------------------------------------
        // Reusing default-sized stacks avoids an mmap + mprotect + munmap per fiber,
        // which matters a great deal for connection-per-fiber servers.

        stack_context acquire_stack(std::size_t size)
        {
            if (size == stack_allocator::default_size && !stack_pool_.empty())
            {
                stack_context s = stack_pool_.back();
                stack_pool_.pop_back();
                return s;
            }
            return stack_allocator::allocate(size);
        }

        void release_stack(stack_context& s)
        {
            if (s.size == stack_allocator::default_size &&
                stack_pool_.size() < max_pooled_stacks_)
            {
                stack_pool_.push_back(s);
            }
            else
            {
                stack_allocator::deallocate(s);
            }
        }

        io_uring ring_{};
        std::deque<fiber*> ready_;
        std::vector<stack_context> stack_pool_;
        fiber* current_ = nullptr;
        std::size_t inflight_ = 0;
        static constexpr std::size_t max_pooled_stacks_ = 1024;
    };

    // Convenience accessor for the scheduler driving the current fiber.
    inline scheduler& this_scheduler() noexcept { return *detail::tls_sched; }

    // ===========================================================================
    // High-level async I/O operations
    // ===========================================================================
    //
    // Each call looks blocking but actually preps an SQE, parks the fiber, and
    // resumes when the CQE lands. Because fibers are stackful, the io_operation and
    // any timespec/sockaddr can simply live as locals here and survive suspension.
    //
    // Results use std::expected: the value on success, a std::error_code carrying
    // the (positive) errno on failure.
    //
    template <class T>
    using result = std::expected<T, std::error_code>;

    namespace detail
    {
        inline std::error_code errc(int negative_res)
        {
            return std::error_code(-negative_res, std::system_category());
        }
    } // namespace detail

    // Internal helper: prep with the supplied callable, then await.
    template <class Prep>
    inline int submit_and_await(Prep prep)
    {
        scheduler& s = this_scheduler();
        io_operation op;
        io_uring_sqe* sqe = s.get_sqe();
        prep(sqe);
        io_uring_sqe_set_data(sqe, &op);
        return s.await(op);
    }

    // ---- file / generic fd ----------------------------------------------------

    inline result<std::size_t> read(int fd, std::span<std::byte> buf,
                                    std::uint64_t offset = ~0ULL)
    {
        int r = submit_and_await([&](io_uring_sqe* sqe)
        {
            io_uring_prep_read(sqe, fd, buf.data(),
                               static_cast<unsigned>(buf.size()), offset);
        });
        if (r < 0) return std::unexpected(detail::errc(r));
        return static_cast<std::size_t>(r);
    }

    inline result<std::size_t> write(int fd, std::span<const std::byte> buf,
                                     std::uint64_t offset = ~0ULL)
    {
        int r = submit_and_await([&](io_uring_sqe* sqe)
        {
            io_uring_prep_write(sqe, fd, buf.data(),
                                static_cast<unsigned>(buf.size()), offset);
        });
        if (r < 0) return std::unexpected(detail::errc(r));
        return static_cast<std::size_t>(r);
    }

    inline result<void> close(int fd)
    {
        int r = submit_and_await(
            [&](io_uring_sqe* sqe) { io_uring_prep_close(sqe, fd); });
        if (r < 0) return std::unexpected(detail::errc(r));
        return {};
    }

    // ---- sockets --------------------------------------------------------------

    inline result<std::size_t> recv(int fd, std::span<std::byte> buf,
                                    int flags = 0)
    {
        int r = submit_and_await([&](io_uring_sqe* sqe)
        {
            io_uring_prep_recv(sqe, fd, buf.data(), buf.size(), flags);
        });
        if (r < 0) return std::unexpected(detail::errc(r));
        return static_cast<std::size_t>(r);
    }

    inline result<std::size_t> send(int fd, std::span<const std::byte> buf,
                                    int flags = 0)
    {
        int r = submit_and_await([&](io_uring_sqe* sqe)
        {
            io_uring_prep_send(sqe, fd, buf.data(), buf.size(), flags);
        });
        if (r < 0) return std::unexpected(detail::errc(r));
        return static_cast<std::size_t>(r);
    }

    // Write the whole buffer, looping over partial sends.
    inline result<void> send_all(int fd, std::span<const std::byte> buf)
    {
        while (!buf.empty())
        {
            auto n = send(fd, buf);
            if (!n) return std::unexpected(n.error());
            if (*n == 0)
                return std::unexpected(
                    std::make_error_code(std::errc::connection_reset));
            buf = buf.subspan(*n);
        }
        return {};
    }

    inline result<int> accept(int fd, sockaddr* addr = nullptr,
                              socklen_t* addrlen = nullptr, int flags = 0)
    {
        int r = submit_and_await([&](io_uring_sqe* sqe)
        {
            io_uring_prep_accept(sqe, fd, addr, addrlen, flags);
        });
        if (r < 0) return std::unexpected(detail::errc(r));
        return r; // new connected fd
    }

    inline result<void> connect(int fd, const sockaddr* addr, socklen_t addrlen)
    {
        int r = submit_and_await([&](io_uring_sqe* sqe)
        {
            io_uring_prep_connect(sqe, fd, addr, addrlen);
        });
        if (r < 0) return std::unexpected(detail::errc(r));
        return {};
    }

    // ---- timers / scheduling --------------------------------------------------

    inline void sleep_for(std::chrono::nanoseconds dur)
    {
        __kernel_timespec ts{};
        ts.tv_sec = dur.count() / 1'000'000'000;
        ts.tv_nsec = dur.count() % 1'000'000'000;
        // A relative timeout completes with -ETIME, which is the normal, expected
        // outcome, so we don't surface it as an error.
        submit_and_await(
            [&](io_uring_sqe* sqe) { io_uring_prep_timeout(sqe, &ts, 0, 0); });
    }

    template <class Rep, class Period>
    inline void sleep_for(std::chrono::duration<Rep, Period> d)
    {
        sleep_for(std::chrono::duration_cast<std::chrono::nanoseconds>(d));
    }

    // Yield the CPU to other ready fibers without doing any I/O.
    inline void yield() { this_scheduler().yield(); }

    // Spawn a fiber from inside another fiber (or before run()).
    inline void spawn(fiber::fn_t fn,
                      std::size_t stack_size = stack_allocator::default_size)
    {
        this_scheduler().spawn(std::move(fn), stack_size);
    }

    // ===========================================================================
    // TCP listener helper
    // ===========================================================================
    //
    // Create, bind, and listen on a TCP socket synchronously. This setup is cheap
    // and one-shot, so there is no benefit to routing it through io_uring.
    // Returns the listening fd or throws std::system_error.
    //
    inline int make_tcp_listener(std::uint16_t port, int backlog = 512)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            throw std::system_error(errno, std::system_category(), "socket");

        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
        {
            int e = errno;
            ::close(fd);
            throw std::system_error(e, std::system_category(), "bind");
        }
        if (::listen(fd, backlog) != 0)
        {
            int e = errno;
            ::close(fd);
            throw std::system_error(e, std::system_category(), "listen");
        }
        return fd;
    }
} // namespace iouring_coro
