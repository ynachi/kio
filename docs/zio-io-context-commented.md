# zio `io_context` Baseline, Annotated

This is the explicit `io_context` version: no thread-local scheduler state, one scheduler owns one `io_uring`, a fixed pool of reusable stackful fibers, Boost.Intrusive queues, guarded stacks, and one centralized `submit_wait()` path for all asynchronous operations.

The comments are intentionally dense. The important thing to study is not only what each line does, but which invariants it preserves:

- A `Fiber` is in exactly one lifecycle state.
- A `Fiber`'s `run_hook` is linked into at most one list: ready or free.
- A `Fiber`'s `timer_hook` is linked only while sleeping.
- A submitted I/O operation increments `pending_io_` exactly once.
- A CQE decrements `pending_io_` exactly once.
- Stacks and fiber control blocks outlive the `io_uring` while the ring may still contain user-data pointers.

```cpp
#pragma once

// Boost.Context gives us stackful execution contexts.
// The benchmark version uses the low-level fcontext API to minimize wrapper
// overhead on every fiber resume/suspend.
#include <boost/context/detail/fcontext.hpp>

// Boost.Intrusive stores list/set links inside Fiber itself.
// This avoids per-queue node allocations and makes enqueue/dequeue O(1).
#include <boost/intrusive/list.hpp>
#include <boost/intrusive/set.hpp>

// Linux io_uring API.
#include <liburing.h>
#include <linux/time_types.h>

#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <exception>
#include <functional>
#include <new>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace zio {

namespace bi = boost::intrusive;

// One fiber control block.
//
// A Fiber is not heap-allocated per task. The scheduler preallocates a fixed
// array of Fiber objects and recycles them. Each Fiber also owns one stackful
// Boost.Context fcontext.
struct Fiber {
    // `safe_link` lets debug builds catch common intrusive-container mistakes:
    // double insertion, destruction while linked, etc.
    using list_hook_t = bi::list_member_hook<bi::link_mode<bi::safe_link>>;
    using set_hook_t = bi::set_member_hook<bi::link_mode<bi::safe_link>>;

    // Used by ready_queue_ and free_list_.
    //
    // A fiber can only be in one of those lists at a time, which matches the
    // lifecycle states READY and FREE. It must not be linked in both.
    list_hook_t run_hook;

    // Used only by timer_set_ while the fiber is sleeping.
    set_hook_t timer_hook;

    // The fiber's suspended execution state.
    //
    // Scheduler -> fiber:
    //     auto t = jump_fcontext(f.ctx, &f);
    //
    // Fiber -> scheduler:
    //     auto t = jump_fcontext(f.scheduler_ctx, nullptr);
    boost::context::detail::fcontext_t ctx = nullptr;

    // Scheduler context captured by the fiber entry point. Fiber operations
    // jump to this when they need to suspend.
    boost::context::detail::fcontext_t scheduler_ctx = nullptr;

    // User function run by this fiber.
    //
    // This is reset when the fiber returns to the free list. `move_only_function`
    // allows move-only captures without requiring heap allocation in every case.
    std::move_only_function<void()> entry;

    // If the user entry throws, the trampoline stores the exception here.
    // The scheduler rethrows after recycling the fiber.
    std::exception_ptr exception;

    // Result copied from the CQE before the fiber is resumed.
    int io_result = 0;
    unsigned io_flags = 0;

    // Timer key used only while state == TIMER_WAIT.
    std::chrono::steady_clock::time_point expiry{};

    // Explicit states make queue membership easier to reason about.
    //
    // FREE       -> linked in free_list_
    // READY      -> linked in ready_queue_
    // RUNNING    -> currently executing, not linked in run_hook containers
    // IO_WAIT    -> suspended on an io_uring operation
    // TIMER_WAIT -> linked in timer_set_
    // DONE       -> returned from entry, ready to recycle
    enum class State : std::uint8_t {
        FREE,
        READY,
        RUNNING,
        IO_WAIT,
        TIMER_WAIT,
        DONE,
    };

    State state = State::FREE;

    Fiber() = default;

    // Fibers are stable-address objects. They are referenced by intrusive
    // containers and by io_uring user_data, so moving/copying would be unsafe.
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;
    Fiber(Fiber&&) = delete;
    Fiber& operator=(Fiber&&) = delete;
};

inline void fiber_entry(boost::context::detail::transfer_t t) noexcept
{
    auto* self = static_cast<Fiber*>(t.data);
    self->scheduler_ctx = t.fctx;

    for (;;) {
        try {
            if (self->entry) self->entry();
        } catch (...) {
            self->exception = std::current_exception();
        }

        self->state = Fiber::State::DONE;
        boost::context::detail::jump_fcontext(self->scheduler_ctx, nullptr);
    }
}

// Ordering for the timer multiset.
//
// The address tie-breaker makes equal expiry times deterministic and ensures
// strict weak ordering even when many fibers sleep until the same timestamp.
struct TimerLess {
    bool operator()(const Fiber& a, const Fiber& b) const noexcept {
        if (a.expiry != b.expiry) return a.expiry < b.expiry;
        return std::less<const Fiber*>{}(&a, &b);
    }
};

// Tunables for one scheduler.
//
// A production program would normally create one io_context per worker thread.
struct io_context_options {
    unsigned ring_entries = 4096;
    unsigned ring_flags = 0;

    // Fiber count is fixed here. Pool exhaustion is explicit ENOMEM from spawn().
    std::size_t fiber_count = 8192;

    // Usable bytes per fiber stack. The implementation rounds this up to a page
    // boundary and adds one guard page.
    std::size_t stack_size = 64 * 1024;

    // Maximum number of ready fibers resumed before checking timers/CQEs again.
    // This prevents a yield-heavy workload from starving I/O and timers.
    std::size_t ready_budget = 256;
};

class io_context {
public:
    // Ready and free list share the same hook because a fiber is either READY
    // or FREE, never both.
    using ReadyList = bi::list<
        Fiber,
        bi::member_hook<Fiber, Fiber::list_hook_t, &Fiber::run_hook>,
        bi::constant_time_size<true>>;

    using FreeList = ReadyList;

    // Timer set uses its own hook, separate from run_hook.
    using TimerSet = bi::multiset<
        Fiber,
        bi::member_hook<Fiber, Fiber::set_hook_t, &Fiber::timer_hook>,
        bi::compare<TimerLess>>;

    explicit io_context(io_context_options opts = {}) : opts_(opts) {
        io_uring_params params{};
        params.flags = opts_.ring_flags;

        // Create the ring first. `ring_live_` records whether cleanup must call
        // io_uring_queue_exit().
        int rc = io_uring_queue_init_params(opts_.ring_entries, &ring_, &params);
        if (rc < 0)
            throw std::runtime_error(std::string{"io_uring init failed: "} + std::strerror(-rc));

        ring_live_ = true;

        // Every later allocation step is guarded so partial construction cleans
        // up correctly.
        try {
            init_stacks();
            init_fibers();
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~io_context() { cleanup(); }

    // Spawn a new fiber using an object from free_list_.
    //
    // This is exception-safe with respect to assigning the user entry. If the
    // move_only_function allocation throws, the fiber is returned to free_list_.
    template <std::invocable F>
    std::expected<void, int> spawn(F&& fn) {
        if (free_list_.empty()) return std::unexpected(ENOMEM);

        Fiber& f = free_list_.front();
        free_list_.pop_front();

        try {
            f.entry = std::forward<F>(fn);
        } catch (const std::bad_alloc&) {
            f.state = Fiber::State::FREE;
            free_list_.push_front(f);
            return std::unexpected(ENOMEM);
        } catch (...) {
            f.state = Fiber::State::FREE;
            free_list_.push_front(f);
            throw;
        }

        enqueue_ready(f);
        return {};
    }

    // Run until there is no ready work, no timers, and no pending I/O.
    //
    // This is a "run until drained" model. It is not yet a cancellation model.
    // Destroying the context while work is pending needs explicit cancellation
    // semantics if you want stack locals to unwind.
    void run() {
        Fiber* old = current_fiber_;
        current_fiber_ = nullptr;

        try {
            while (has_work()) {
                process_expired_timers();
                process_cqes();

                run_ready_budget();

                // If fibers requeued work, loop again before sleeping in the
                // kernel. This keeps purely-ready work fast.
                if (!ready_queue_.empty()) continue;

                process_expired_timers();
                process_cqes();

                if (!ready_queue_.empty()) continue;

                wait_for_event();
            }
        } catch (...) {
            current_fiber_ = old;
            throw;
        }

        current_fiber_ = old;
    }

    // Cooperative yield: put the current fiber at the end of ready_queue_ and
    // resume the scheduler.
    void yield() {
        Fiber* f = require_current();
        enqueue_ready(*f);
        auto t = boost::context::detail::jump_fcontext(f->scheduler_ctx, nullptr);
        f->scheduler_ctx = t.fctx;
    }

    // Sleep by inserting the current fiber into timer_set_.
    //
    // The scheduler wakes it when its expiry becomes the earliest due timer.
    void sleep(std::chrono::nanoseconds dur) {
        Fiber* f = require_current();
        assert(!f->timer_hook.is_linked());

        f->expiry = std::chrono::steady_clock::now() + dur;
        f->state = Fiber::State::TIMER_WAIT;
        timer_set_.insert(*f);

        auto t = boost::context::detail::jump_fcontext(f->scheduler_ctx, nullptr);
        f->scheduler_ctx = t.fctx;
    }

    // File descriptor read. offset -1 means "current file offset" for io_uring.
    std::expected<std::size_t, int> read(int fd, std::span<std::byte> buf, off_t off = -1) {
        return submit_wait<std::size_t>([&](io_uring_sqe* sqe) {
            io_uring_prep_read(sqe, fd, buf.data(), buf.size(), static_cast<__u64>(off));
        });
    }

    // File descriptor write. offset -1 means "current file offset".
    std::expected<std::size_t, int> write(int fd, std::span<const std::byte> buf, off_t off = -1) {
        return submit_wait<std::size_t>([&](io_uring_sqe* sqe) {
            io_uring_prep_write(sqe, fd, buf.data(), buf.size(), static_cast<__u64>(off));
        });
    }

    // Accept one connection. Accepted socket is nonblocking and close-on-exec.
    std::expected<int, int> accept(int fd) {
        return submit_wait<int>([&](io_uring_sqe* sqe) {
            io_uring_prep_accept(sqe, fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        });
    }

    std::expected<std::size_t, int> recv(int fd, std::span<std::byte> buf, int flags = 0) {
        return submit_wait<std::size_t>([&](io_uring_sqe* sqe) {
            io_uring_prep_recv(sqe, fd, buf.data(), buf.size(), flags);
        });
    }

    std::expected<std::size_t, int> send(int fd, std::span<const std::byte> buf, int flags = 0) {
        return submit_wait<std::size_t>([&](io_uring_sqe* sqe) {
            io_uring_prep_send(sqe, fd, buf.data(), buf.size(), flags);
        });
    }

private:
    io_context_options opts_;

    // io_uring must outlive pending user_data observations.
    io_uring ring_{};
    bool ring_live_ = false;

    // Stack allocation metadata.
    std::size_t page_size_ = 0;
    std::size_t stack_stride_ = 0;
    std::byte* stack_pool_ = nullptr;

    // Fiber control-block pool.
    void* fiber_mem_ = nullptr;
    Fiber* fibers_ = nullptr;
    std::size_t constructed_ = 0;

    ReadyList ready_queue_;
    TimerSet timer_set_;
    FreeList free_list_;

    // Number of submitted I/O operations whose CQEs have not been consumed.
    unsigned pending_io_ = 0;

    // Current fiber is per-context state, not TLS.
    // That makes the API explicit and avoids hidden cross-context state.
    Fiber* current_fiber_ = nullptr;

    static std::size_t align_up(std::size_t n, std::size_t a) noexcept {
        return (n + a - 1) & ~(a - 1);
    }

    void init_stacks() {
        page_size_ = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
        opts_.stack_size = align_up(opts_.stack_size, page_size_);

        // Each stack region:
        //
        //   [ guard page, PROT_NONE ][ usable stack, read/write ]
        //
        // Stack grows downward, so the initial stack pointer is the top of the
        // usable region.
        stack_stride_ = opts_.stack_size + page_size_;

        const std::size_t total = opts_.fiber_count * stack_stride_;

        stack_pool_ = static_cast<std::byte*>(
            ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));

        if (stack_pool_ == MAP_FAILED) throw std::bad_alloc{};

        for (std::size_t i = 0; i < opts_.fiber_count; ++i) {
            void* guard = stack_pool_ + i * stack_stride_;
            if (::mprotect(guard, page_size_, PROT_NONE) != 0)
                throw std::runtime_error("mprotect guard page failed");
        }
    }

    void init_fibers() {
        fiber_mem_ = ::operator new[](opts_.fiber_count * sizeof(Fiber),
                                      std::align_val_t{alignof(Fiber)});
        fibers_ = static_cast<Fiber*>(fiber_mem_);

        for (; constructed_ < opts_.fiber_count; ++constructed_) {
            Fiber* f = new (&fibers_[constructed_]) Fiber();

            void* stack_top = stack_pool_ + constructed_ * stack_stride_ + stack_stride_;

            f->ctx = boost::context::detail::make_fcontext(stack_top, opts_.stack_size, fiber_entry);
            f->state = Fiber::State::FREE;
            free_list_.push_back(*f);
        }
    }

    bool has_work() const noexcept {
        return !ready_queue_.empty() || !timer_set_.empty() || pending_io_ > 0;
    }

    Fiber* require_current() {
        if (current_fiber_ == nullptr) throw std::logic_error("zio operation outside fiber");
        return current_fiber_;
    }

    void enqueue_ready(Fiber& f) {
        assert(!f.run_hook.is_linked());
        f.state = Fiber::State::READY;
        ready_queue_.push_back(f);
    }

    void recycle(Fiber& f) {
        assert(!f.run_hook.is_linked());

        // A DONE fiber should normally not be linked in timer_set_, but this
        // makes recycle robust against future cancellation paths.
        if (f.timer_hook.is_linked())
            timer_set_.erase(timer_set_.iterator_to(f));

        f.entry = nullptr;
        f.exception = nullptr;
        f.io_result = 0;
        f.io_flags = 0;
        f.expiry = {};
        f.state = Fiber::State::FREE;

        free_list_.push_back(f);
    }

    // Best-effort SQE acquisition.
    //
    // If the submission queue is full, submit existing SQEs, reap completions,
    // and retry once. A production version could suspend on an internal SQE
    // waiter queue instead of returning EAGAIN.
    io_uring_sqe* get_sqe() {
        if (auto* sqe = io_uring_get_sqe(&ring_)) return sqe;

        io_uring_submit(&ring_);
        process_cqes();

        return io_uring_get_sqe(&ring_);
    }

    // Common I/O path:
    //
    // 1. acquire SQE
    // 2. let caller prepare operation
    // 3. store Fiber* in user_data
    // 4. mark IO_WAIT and increment pending count
    // 5. resume scheduler
    // 6. return CQE result when scheduler resumes us
    template <class R = int, class Prep>
    std::expected<R, int> submit_wait(Prep&& prep) {
        Fiber* f = require_current();
        assert(f->state == Fiber::State::RUNNING);

        io_uring_sqe* sqe = get_sqe();
        if (sqe == nullptr) return std::unexpected(EAGAIN);

        std::forward<Prep>(prep)(sqe);
        io_uring_sqe_set_data(sqe, f);

        f->state = Fiber::State::IO_WAIT;
        ++pending_io_;

        auto t = boost::context::detail::jump_fcontext(f->scheduler_ctx, nullptr);
        f->scheduler_ctx = t.fctx;

        if (f->io_result < 0) return std::unexpected(-f->io_result);

        if constexpr (std::same_as<R, void>)
            return {};
        else
            return static_cast<R>(f->io_result);
    }

    void run_ready_budget() {
        std::size_t budget = opts_.ready_budget;

        while (!ready_queue_.empty() && budget-- > 0) {
            Fiber& f = ready_queue_.front();
            ready_queue_.pop_front();

            current_fiber_ = &f;
            f.state = Fiber::State::RUNNING;

            auto t = boost::context::detail::jump_fcontext(f.ctx, &f);

            current_fiber_ = nullptr;

            if (f.state == Fiber::State::DONE) {
                auto ex = f.exception;
                recycle(f);
                if (ex) std::rethrow_exception(ex);
            }
            else
            {
                f.ctx = t.fctx;
            }
        }
    }

    void process_expired_timers() {
        const auto now = std::chrono::steady_clock::now();

        while (!timer_set_.empty()) {
            auto it = timer_set_.begin();
            if (it->expiry > now) break;

            Fiber& f = *it;
            timer_set_.erase(it);
            enqueue_ready(f);
        }
    }

    void process_cqes() {
        io_uring_cqe* cqe = nullptr;
        unsigned head = 0;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring_, head, cqe) {
            ++count;

            auto* f = static_cast<Fiber*>(io_uring_cqe_get_data(cqe));
            if (f == nullptr) continue;

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

    void wait_for_event() {
        if (pending_io_ == 0 && timer_set_.empty()) return;

        io_uring_submit(&ring_);

        __kernel_timespec ts{};
        __kernel_timespec* ts_ptr = nullptr;

        if (!timer_set_.empty()) {
            const auto now = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(
                timer_set_.begin()->expiry - now);

            if (dur.count() < 0) dur = std::chrono::nanoseconds{0};

            auto sec = std::chrono::duration_cast<std::chrono::seconds>(dur);
            auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(dur - sec);

            ts.tv_sec = sec.count();
            ts.tv_nsec = nsec.count();
            ts_ptr = &ts;
        }

        io_uring_cqe* cqe = nullptr;
        int rc = ts_ptr ? io_uring_wait_cqe_timeout(&ring_, &cqe, ts_ptr)
                        : io_uring_wait_cqe(&ring_, &cqe);

        if (rc == 0) {
            process_cqes();
        } else if (rc != -ETIME && rc != -EINTR) {
            std::println("io_uring wait error: {}", std::strerror(-rc));
        }
    }

    void cleanup() noexcept {
        // Exit the ring before freeing Fiber objects/stacks. The ring may have
        // operations whose user_data points at Fiber objects.
        if (ring_live_) {
            io_uring_queue_exit(&ring_);
            ring_live_ = false;
        }

        // Unlink containers before destroying safe hooks.
        ready_queue_.clear();
        timer_set_.clear();
        free_list_.clear();

        for (std::size_t i = 0; i < constructed_; ++i)
            fibers_[i].~Fiber();

        constructed_ = 0;

        if (fiber_mem_) {
            ::operator delete[](fiber_mem_, std::align_val_t{alignof(Fiber)});
            fiber_mem_ = nullptr;
            fibers_ = nullptr;
        }

        if (stack_pool_ && stack_pool_ != MAP_FAILED) {
            ::munmap(stack_pool_, opts_.fiber_count * stack_stride_);
            stack_pool_ = nullptr;
        }
    }
};

// Minimal RAII listener wrapper.
class TcpListener {
    int fd_ = -1;

public:
    static std::expected<TcpListener, int> bind(std::string_view ip, std::uint16_t port) {
        int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) return std::unexpected(errno);

        int opt = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);

        std::string ip_copy{ip};
        if (::inet_pton(AF_INET, ip_copy.c_str(), &addr.sin_addr) <= 0) {
            ::close(fd);
            return std::unexpected(EINVAL);
        }

        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            int e = errno;
            ::close(fd);
            return std::unexpected(e);
        }

        if (::listen(fd, SOMAXCONN) < 0) {
            int e = errno;
            ::close(fd);
            return std::unexpected(e);
        }

        return TcpListener{fd};
    }

    explicit TcpListener(int fd) noexcept : fd_(fd) {}

    TcpListener(TcpListener&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    TcpListener& operator=(TcpListener&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    ~TcpListener() {
        if (fd_ >= 0) ::close(fd_);
    }

    int fd() const noexcept { return fd_; }
};

} // namespace zio
```
