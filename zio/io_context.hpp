#pragma once

#include "result.hpp"

#include <boost/context/detail/fcontext.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>
#include <liburing.h>
#include <linux/time_types.h>

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <new>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <stop_token>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace zio
{
namespace bi = boost::intrusive;

struct Fiber
{
    using list_hook_t = bi::list_member_hook<bi::link_mode<bi::safe_link>>;
    using set_hook_t  = bi::set_member_hook<bi::link_mode<bi::safe_link>>;

    list_hook_t run_hook;
    set_hook_t  timer_hook;

    boost::context::detail::fcontext_t ctx = nullptr;
    boost::context::detail::fcontext_t scheduler_ctx = nullptr;

    std::move_only_function<void()> entry;
    std::error_code                error;

    int      io_result = 0;
    unsigned io_flags  = 0;

    std::chrono::steady_clock::time_point expiry{};

    enum class State : std::uint8_t
    {
        FREE,
        READY,
        RUNNING,
        IO_WAIT,
        TIMER_WAIT,
        DONE,
    };

    State state = State::FREE;

    Fiber() = default;
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;
};

inline void fiber_entry(boost::context::detail::transfer_t t) noexcept
{
    auto* self = static_cast<Fiber*>(t.data);
    self->scheduler_ctx = t.fctx;

    for (;;)
    {
        try
        {
            if (self->entry)
                self->entry();
        }
        catch (...)
        {
            self->error = make_error_code(EFAULT);
        }

        self->state = Fiber::State::DONE;
        boost::context::detail::jump_fcontext(self->scheduler_ctx, nullptr);
    }
}

struct TimerLess
{
    bool operator()(const Fiber& a, const Fiber& b) const noexcept
    {
        if (a.expiry != b.expiry)
            return a.expiry < b.expiry;
        return std::less<const Fiber*>{}(&a, &b);
    }
};

struct RemoteSpawn
{
    std::atomic<RemoteSpawn*> next{nullptr};
    std::move_only_function<void()> fn;
};

class RemoteSpawnQueue
{
public:
    RemoteSpawnQueue()
    {
        stub_.next.store(nullptr, std::memory_order_relaxed);
        head_.store(&stub_, std::memory_order_relaxed);
        tail_ = &stub_;
    }

    void enqueue(RemoteSpawn* node) noexcept
    {
        node->next.store(nullptr, std::memory_order_relaxed);
        RemoteSpawn* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    RemoteSpawn* dequeue() noexcept
    {
        RemoteSpawn* tail = tail_;
        RemoteSpawn* next = tail->next.load(std::memory_order_acquire);

        if (tail == &stub_)
        {
            if (next == nullptr)
                return nullptr;
            tail_ = next;
            tail = next;
            next = next->next.load(std::memory_order_acquire);
        }

        if (next != nullptr)
        {
            tail_ = next;
            return tail;
        }

        if (const RemoteSpawn* head = head_.load(std::memory_order_acquire); tail != head)
            return nullptr;

        enqueue(&stub_);
        next = tail->next.load(std::memory_order_acquire);
        if (next != nullptr)
        {
            tail_ = next;
            return tail;
        }
        return nullptr;
    }

    template <typename Fn>
    std::size_t drain(Fn&& fn, std::size_t max_count)
    {
        std::size_t count = 0;
        while (count < max_count)
        {
            RemoteSpawn* node = dequeue();
            if (node == nullptr)
                break;
            fn(node);
            ++count;
        }
        return count;
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire) == tail_;
    }

private:
    alignas(64) std::atomic<RemoteSpawn*> head_;
    alignas(64) RemoteSpawn* tail_ = nullptr;
    RemoteSpawn stub_{};
};

struct io_context_options
{
    unsigned    ring_entries = 4096;
    unsigned    ring_flags   = 0;
    std::size_t fiber_count  = 8192;
    std::size_t stack_size   = 64 * 1024;
    std::size_t ready_budget = 256;
};

class io_context
{
public:
    static constexpr std::uint64_t kWakeupSentinel = 0xDEAD'C0DE'DEAD'C0DEULL;

    using ReadyList = bi::list<Fiber,
                               bi::member_hook<Fiber, Fiber::list_hook_t, &Fiber::run_hook>,
                               bi::constant_time_size<true>>;
    using FreeList = ReadyList;
    using TimerSet = bi::multiset<Fiber,
                                  bi::member_hook<Fiber, Fiber::set_hook_t, &Fiber::timer_hook>,
                                  bi::compare<TimerLess>>;

    explicit io_context(io_context_options opts = {}) : opts_(opts)
    {
        io_uring_params params{};
        params.flags = opts_.ring_flags;

        const int rc = io_uring_queue_init_params(opts_.ring_entries, &ring_, &params);
        if (rc < 0)
        {
            throw std::runtime_error(std::string{"io_uring init failed: "} + std::strerror(-rc));
        }
        ring_live_ = true;

        wake_fd_ = ::eventfd(0, EFD_CLOEXEC);
        if (wake_fd_ < 0)
        {
            cleanup();
            throw std::runtime_error(std::string{"eventfd failed: "} + std::strerror(errno));
        }

        try
        {
            init_stacks();
            init_fibers();
            if (auto wake_res = arm_wake_read(); !wake_res)
                throw std::runtime_error("failed to arm zio wake eventfd: " + wake_res.error().message());
        }
        catch (...)
        {
            cleanup();
            throw;
        }
    }

    ~io_context() { cleanup(); }

    io_context(const io_context&) = delete;
    io_context& operator=(const io_context&) = delete;

    template <std::invocable F>
    Result<> spawn(F&& fn)
    {
        return spawn_local(std::forward<F>(fn));
    }

    template <std::invocable F>
    Result<> spawn_local(F&& fn)
    {
        if (free_list_.empty())
            return error_from_errno(ENOMEM);

        Fiber& f = free_list_.front();
        free_list_.pop_front();

        try
        {
            f.entry = std::forward<F>(fn);
        }
        catch (const std::bad_alloc&)
        {
            f.state = Fiber::State::FREE;
            free_list_.push_front(f);
            return error_from_errno(ENOMEM);
        }
        catch (...)
        {
            f.state = Fiber::State::FREE;
            free_list_.push_front(f);
            return error_from_errno(EFAULT);
        }

        enqueue_ready(f);
        return {};
    }

    template <std::invocable F>
    Result<> schedule(F&& fn)
    {
        if (stop_requested_.load(std::memory_order_acquire))
            return error_from_errc(std::errc::operation_canceled);

        auto* node = new (std::nothrow) RemoteSpawn();
        if (node == nullptr)
            return error_from_errno(ENOMEM);

        try
        {
            node->fn = std::forward<F>(fn);
        }
        catch (const std::bad_alloc&)
        {
            delete node;
            return error_from_errno(ENOMEM);
        }
        catch (...)
        {
            delete node;
            return error_from_errno(EFAULT);
        }

        remote_spawns_.enqueue(node);
        wake();
        return {};
    }

    Result<> run()
    {
        stop_requested_.store(false, std::memory_order_release);
        return run_loop(false);
    }

    Result<> run_blocking(std::stop_token st = {})
    {
        stop_requested_.store(false, std::memory_order_release);
        std::stop_callback on_stop(st, [this] { request_stop(); });
        return run_loop(true);
    }

    void request_stop() noexcept
    {
        stop_requested_.store(true, std::memory_order_release);
        wake();
    }

    Result<> yield()
    {
        ZIO_TRY(Fiber* f, require_current());
        enqueue_ready(*f);
        auto t = boost::context::detail::jump_fcontext(f->scheduler_ctx, nullptr);
        f->scheduler_ctx = t.fctx;
        return {};
    }

    /// Suspend the currently running zio fiber until a relative timer expires.
    ///
    /// This is the timer API for zio. It must be called from inside a function
    /// that was started with io_context::spawn(); calling it from outside a
    /// running zio fiber returns std::errc::operation_not_permitted.
    ///
    /// sleep() does not block the scheduler thread. It only parks the current
    /// fiber:
    ///
    ///   1. The current fiber's absolute expiry time is computed as
    ///      steady_clock::now() + dur.
    ///   2. The fiber is marked TIMER_WAIT and inserted into timer_set_.
    ///   3. The fiber jumps back to the scheduler context.
    ///   4. io_context::run() continues running other ready fibers, processes
    ///      completed io_uring CQEs, or waits for the nearest timer/IO event.
    ///   5. When process_expired_timers() observes that the expiry has passed,
    ///      it removes the fiber from timer_set_ and pushes it back to the
    ///      ready queue.
    ///   6. The next time the scheduler resumes that fiber, sleep() returns.
    ///
    /// Example:
    ///
    /// @code
    /// zio::io_context ctx;
    /// ctx.spawn([&] {
    ///     ZIO_TRY_VOID(ctx.sleep(std::chrono::seconds(1)));
    ///     std::println("one second later");
    /// });
    /// ZIO_TRY_VOID(ctx.run());
    /// @endcode
    ///
    /// Repeating timers are just a loop around sleep():
    ///
    /// @code
    /// ctx.spawn([&] {
    ///     using namespace std::chrono_literals;
    ///     for (;;) {
    ///         ZIO_TRY_VOID(ctx.sleep(500ms));
    ///         std::println("tick");
    ///     }
    /// });
    /// @endcode
    ///
    /// The API takes a relative duration, not an absolute deadline. For an
    /// absolute deadline, calculate the remaining duration before calling
    /// sleep():
    ///
    /// @code
    /// auto deadline = std::chrono::steady_clock::now() + 5s;
    /// auto now = std::chrono::steady_clock::now();
    /// if (deadline > now)
    ///     ZIO_TRY_VOID(ctx.sleep(deadline - now));
    /// @endcode
    ///
    /// A zero or already-expired duration is allowed. The fiber still yields to
    /// the scheduler, then becomes ready again on the next timer processing
    /// pass. This gives other ready fibers a chance to run.
    ///
    /// The scheduler exits only when there are no ready fibers, no pending I/O
    /// operations, and no timers left in timer_set_. A fiber that loops forever
    /// around sleep() intentionally keeps the io_context alive.
    Result<> sleep(std::chrono::nanoseconds dur)
    {
        ZIO_TRY(Fiber* f, require_current());
        assert(!f->timer_hook.is_linked());

        f->expiry = std::chrono::steady_clock::now() + dur;
        f->state  = Fiber::State::TIMER_WAIT;
        timer_set_.insert(*f);

        auto t = boost::context::detail::jump_fcontext(f->scheduler_ctx, nullptr);
        f->scheduler_ctx = t.fctx;
        return {};
    }

    Result<std::size_t> read(int fd, std::span<std::byte> buf, off_t off = -1)
    {
        return submit_wait<std::size_t>(
            [&](io_uring_sqe* sqe)
            {
                io_uring_prep_read(sqe, fd, buf.data(), buf.size(), static_cast<__u64>(off));
            });
    }

    Result<std::size_t> write(int fd, std::span<const std::byte> buf, off_t off = -1)
    {
        return submit_wait<std::size_t>(
            [&](io_uring_sqe* sqe)
            {
                io_uring_prep_write(sqe, fd, buf.data(), buf.size(), static_cast<__u64>(off));
            });
    }

    Result<int> accept(int fd)
    {
        return submit_wait<int>(
            [&](io_uring_sqe* sqe)
            {
                io_uring_prep_accept(sqe, fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            });
    }

    Result<std::size_t> recv(int fd, std::span<std::byte> buf, int flags = 0)
    {
        return submit_wait<std::size_t>(
            [&](io_uring_sqe* sqe)
            {
                io_uring_prep_recv(sqe, fd, buf.data(), buf.size(), flags);
            });
    }

    Result<std::size_t> send(int fd, std::span<const std::byte> buf, int flags = 0)
    {
        return submit_wait<std::size_t>(
            [&](io_uring_sqe* sqe)
            {
                io_uring_prep_send(sqe, fd, buf.data(), buf.size(), flags);
            });
    }

private:
    io_context_options opts_;
    io_uring           ring_{};
    bool               ring_live_ = false;

    std::size_t page_size_    = 0;
    std::size_t stack_stride_ = 0;
    std::byte*  stack_pool_   = nullptr;

    void*       fiber_mem_   = nullptr;
    Fiber*      fibers_      = nullptr;
    std::size_t constructed_ = 0;

    ReadyList ready_queue_;
    TimerSet  timer_set_;
    FreeList  free_list_;

    unsigned pending_io_ = 0;
    Fiber*   current_fiber_ = nullptr;

    int wake_fd_ = -1;
    std::uint64_t wake_value_ = 0;
    bool wake_read_armed_ = false;
    RemoteSpawnQueue remote_spawns_;
    std::thread::id owner_thread_{};
    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_sleeping_{false};
    std::atomic<bool> stop_requested_{false};

    static std::size_t align_up(std::size_t n, std::size_t a) noexcept
    {
        return (n + a - 1) & ~(a - 1);
    }

    void init_stacks()
    {
        page_size_ = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        opts_.stack_size = align_up(opts_.stack_size, page_size_);
        stack_stride_ = opts_.stack_size + page_size_;

        const std::size_t total = opts_.fiber_count * stack_stride_;
        stack_pool_ = static_cast<std::byte*>(
            ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));
        if (stack_pool_ == MAP_FAILED)
            throw std::bad_alloc{};

        for (std::size_t i = 0; i < opts_.fiber_count; ++i)
        {
            void* guard = stack_pool_ + i * stack_stride_;
            if (::mprotect(guard, page_size_, PROT_NONE) != 0)
                throw std::runtime_error("mprotect guard page failed");
        }
    }

    void init_fibers()
    {
        fiber_mem_ = ::operator new[](opts_.fiber_count * sizeof(Fiber),
                                      std::align_val_t{alignof(Fiber)});
        fibers_ = static_cast<Fiber*>(fiber_mem_);

        for (; constructed_ < opts_.fiber_count; ++constructed_)
        {
            Fiber* f = new (&fibers_[constructed_]) Fiber();
            void* stack_top = stack_pool_ + constructed_ * stack_stride_ + stack_stride_;

            f->ctx = boost::context::detail::make_fcontext(stack_top, opts_.stack_size, fiber_entry);
            f->state = Fiber::State::FREE;
            free_list_.push_back(*f);
        }
    }

    Result<> run_loop(bool blocking)
    {
        Fiber* old = current_fiber_;
        current_fiber_ = nullptr;
        owner_thread_ = std::this_thread::get_id();
        is_running_.store(true, std::memory_order_release);

        while (blocking ? (!stop_requested_.load(std::memory_order_acquire) || has_work())
                        : has_work())
        {
            drain_remote_spawns();
            process_expired_timers();
            process_cqes();

            auto ready_res = run_ready_budget();
            if (!ready_res)
            {
                current_fiber_ = old;
                is_running_.store(false, std::memory_order_release);
                return ready_res;
            }

            if (!ready_queue_.empty())
                continue;

            drain_remote_spawns();
            process_expired_timers();
            process_cqes();

            if (!ready_queue_.empty())
                continue;

            if (!blocking && !has_work())
                break;

            is_sleeping_.store(true, std::memory_order_seq_cst);
            drain_remote_spawns();
            process_expired_timers();
            process_cqes();

            if (!ready_queue_.empty() || (!blocking && !has_work()) ||
                stop_requested_.load(std::memory_order_acquire))
            {
                is_sleeping_.store(false, std::memory_order_relaxed);
                continue;
            }

            auto wait_res = wait_for_event(blocking);
            is_sleeping_.store(false, std::memory_order_relaxed);
            if (!wait_res)
            {
                current_fiber_ = old;
                is_running_.store(false, std::memory_order_release);
                return wait_res;
            }
        }

        is_running_.store(false, std::memory_order_release);
        current_fiber_ = old;
        return {};
    }

    bool has_work() const noexcept
    {
        return !ready_queue_.empty() || !timer_set_.empty() || pending_io_ > 0 ||
               !remote_spawns_.empty();
    }

    Result<> arm_wake_read()
    {
        if (wake_read_armed_)
            return {};
        if (wake_fd_ < 0)
            return error_from_errno(EBADF);

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (sqe == nullptr)
        {
            const int rc = io_uring_submit(&ring_);
            if (rc < 0)
                return std::unexpected(make_error_code(rc));

            sqe = io_uring_get_sqe(&ring_);
            if (sqe == nullptr)
                return error_from_errno(EAGAIN);
        }

        io_uring_prep_read(sqe, wake_fd_, &wake_value_, sizeof(wake_value_), 0);
        io_uring_sqe_set_data64(sqe, kWakeupSentinel);
        wake_read_armed_ = true;
        return {};
    }

    void wake() noexcept
    {
        if (!is_sleeping_.load(std::memory_order_seq_cst))
            return;

        constexpr std::uint64_t one = 1;
        for (;;)
        {
            const ssize_t n = ::write(wake_fd_, &one, sizeof(one));
            if (n == sizeof(one))
                return;
            if (n == -1 && errno == EINTR)
                continue;
            ZIO_LOG_WARN("failed to wake zio context with eventfd: {}", std::strerror(errno));
            return;
        }
    }

    void drain_remote_spawns()
    {
        remote_spawns_.drain(
            [this](RemoteSpawn* node)
            {
                auto spawn_res = spawn_local(std::move(node->fn));
                if (!spawn_res)
                    ZIO_LOG_ERROR("remote spawn failed: {}", spawn_res.error().message());
                delete node;
            },
            opts_.ready_budget);
    }

    Result<Fiber*> require_current()
    {
        if (current_fiber_ == nullptr)
            return error_from_errc(std::errc::operation_not_permitted);
        return current_fiber_;
    }

    void enqueue_ready(Fiber& f)
    {
        assert(!f.run_hook.is_linked());
        f.state = Fiber::State::READY;
        ready_queue_.push_back(f);
    }

    void recycle(Fiber& f)
    {
        assert(!f.run_hook.is_linked());
        if (f.timer_hook.is_linked())
            timer_set_.erase(timer_set_.iterator_to(f));

        f.entry = nullptr;
        f.error = {};
        f.io_result = 0;
        f.io_flags = 0;
        f.expiry = {};
        f.state = Fiber::State::FREE;

        free_list_.push_back(f);
    }

    Result<io_uring_sqe*> get_sqe()
    {
        if (auto* sqe = io_uring_get_sqe(&ring_))
            return sqe;

        const int rc = io_uring_submit(&ring_);
        if (rc < 0)
            return std::unexpected(make_error_code(rc));

        process_cqes();

        if (auto* sqe = io_uring_get_sqe(&ring_))
            return sqe;

        return error_from_errno(EAGAIN);
    }

    template <class R = int, class Prep>
    Result<R> submit_wait(Prep&& prep)
    {
        ZIO_TRY(Fiber* f, require_current());
        assert(f->state == Fiber::State::RUNNING);

        ZIO_TRY(io_uring_sqe* sqe, get_sqe());

        std::forward<Prep>(prep)(sqe);
        io_uring_sqe_set_data(sqe, f);

        f->state = Fiber::State::IO_WAIT;
        ++pending_io_;

        auto t = boost::context::detail::jump_fcontext(f->scheduler_ctx, nullptr);
        f->scheduler_ctx = t.fctx;

        if (f->io_result < 0)
            return std::unexpected(make_error_code(f->io_result));

        if constexpr (std::same_as<R, void>)
            return {};
        else
            return static_cast<R>(f->io_result);
    }

    Result<> run_ready_budget()
    {
        std::size_t budget = opts_.ready_budget;
        while (!ready_queue_.empty() && budget-- > 0)
        {
            Fiber& f = ready_queue_.front();
            ready_queue_.pop_front();

            current_fiber_ = &f;
            f.state = Fiber::State::RUNNING;
            auto t = boost::context::detail::jump_fcontext(f.ctx, &f);
            current_fiber_ = nullptr;

            if (f.state == Fiber::State::DONE)
            {
                auto error = f.error;
                recycle(f);
                if (error)
                    return std::unexpected(error);
            }
            else
            {
                f.ctx = t.fctx;
            }
        }
        return {};
    }

    void process_expired_timers()
    {
        const auto now = std::chrono::steady_clock::now();
        while (!timer_set_.empty())
        {
            auto it = timer_set_.begin();
            if (it->expiry > now)
                break;

            Fiber& f = *it;
            timer_set_.erase(it);
            enqueue_ready(f);
        }
    }

    void process_cqes()
    {
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring_, head, cqe)
        {
            ++count;
            const auto user_data = io_uring_cqe_get_data64(cqe);
            if (user_data == kWakeupSentinel)
            {
                wake_read_armed_ = false;
                if (auto arm_res = arm_wake_read(); !arm_res)
                    ZIO_LOG_WARN("failed to re-arm zio wake eventfd: {}", arm_res.error().message());
                continue;
            }

            auto* f = reinterpret_cast<Fiber*>(user_data);
            if (f == nullptr)
                continue;

            assert(pending_io_ > 0);
            --pending_io_;
            f->io_result = cqe->res;
            f->io_flags = cqe->flags;

            if (f->state == Fiber::State::IO_WAIT)
                enqueue_ready(*f);
        }

        if (count > 0)
            io_uring_cq_advance(&ring_, count);
    }

    Result<> wait_for_event(bool block_idle)
    {
        if (!block_idle && pending_io_ == 0 && timer_set_.empty())
            return {};

        ZIO_TRY_VOID(arm_wake_read());
        int rc = io_uring_submit(&ring_);
        if (rc < 0)
            return std::unexpected(make_error_code(rc));

        __kernel_timespec ts{};
        __kernel_timespec* ts_ptr = nullptr;

        if (!timer_set_.empty())
        {
            const auto now = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
                timer_set_.begin()->expiry - now);
            if (dur.count() < 0)
                dur = std::chrono::nanoseconds{0};

            auto sec = std::chrono::duration_cast<std::chrono::seconds>(dur);
            auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dur - sec);
            ts.tv_sec = sec.count();
            ts.tv_nsec = nsec.count();
            ts_ptr = &ts;
        }

        io_uring_cqe* cqe = nullptr;
        rc = ts_ptr ? io_uring_wait_cqe_timeout(&ring_, &cqe, ts_ptr)
                    : io_uring_wait_cqe(&ring_, &cqe);
        if (rc == 0)
        {
            process_cqes();
        }
        else if (rc != -ETIME && rc != -EINTR)
        {
            return std::unexpected(make_error_code(rc));
        }
        return {};
    }

    void cleanup() noexcept
    {
        if (ring_live_)
        {
            io_uring_queue_exit(&ring_);
            ring_live_ = false;
        }
        wake_read_armed_ = false;

        remote_spawns_.drain([](RemoteSpawn* node) { delete node; },
                             static_cast<std::size_t>(-1));

        ready_queue_.clear();
        timer_set_.clear();
        free_list_.clear();

        for (std::size_t i = 0; i < constructed_; ++i)
            fibers_[i].~Fiber();
        constructed_ = 0;

        if (fiber_mem_)
        {
            ::operator delete[](fiber_mem_, std::align_val_t{alignof(Fiber)});
            fiber_mem_ = nullptr;
            fibers_ = nullptr;
        }

        if (wake_fd_ >= 0)
        {
            ::close(wake_fd_);
            wake_fd_ = -1;
        }

        if (stack_pool_ && stack_pool_ != MAP_FAILED)
        {
            ::munmap(stack_pool_, opts_.fiber_count * stack_stride_);
            stack_pool_ = nullptr;
        }
    }
};

class TcpListener
{
    int fd_ = -1;

public:
    static Result<TcpListener> bind(std::string_view ip, std::uint16_t port)
    {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return error_from_errno(errno);

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        std::string ip_copy{ip};
        if (::inet_pton(AF_INET, ip_copy.c_str(), &addr.sin_addr) <= 0)
        {
            ::close(fd);
            return error_from_errc(std::errc::invalid_argument);
        }

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            int e = errno;
            ::close(fd);
            return error_from_errno(e);
        }

        if (::listen(fd, SOMAXCONN) < 0)
        {
            int e = errno;
            ::close(fd);
            return error_from_errno(e);
        }

        return TcpListener{fd};
    }

    explicit TcpListener(int fd) noexcept : fd_(fd) {}
    TcpListener(TcpListener&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    TcpListener& operator=(TcpListener&& other) noexcept
    {
        if (this != &other)
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    ~TcpListener()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    [[nodiscard]] int fd() const noexcept { return fd_; }
};
}  // namespace zio
