#pragma once
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <limits>
#include <mutex>
#include <stop_token>
#include <utility>
#include <vector>

#include <liburing.h>

#ifdef BLOCK_SIZE
    #pragma push_macro("BLOCK_SIZE")
    #undef BLOCK_SIZE
    #define URING_RESTORE_BLOCK_SIZE_MACRO
#endif
#include "libs/concurrentqueue.hpp"
#ifdef URING_RESTORE_BLOCK_SIZE_MACRO
    #pragma pop_macro("BLOCK_SIZE")
    #undef URING_RESTORE_BLOCK_SIZE_MACRO
#endif
#include "token.hpp"

namespace URing
{
constexpr unsigned kUringDefaultFlag =
    IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

class IoContext
{
public:
    using Job = std::move_only_function<void()>;

private:
    static constexpr std::uint64_t kWakeUserData = std::numeric_limits<std::uint64_t>::max();
    static constexpr std::uint16_t kMaxJobsPerWakeup = 64;

    io_uring m_ring_{};
    std::vector<OpState> m_op_slab_;
    uint32_t m_head_free_idx_ = 0;
    uint32_t m_pending_ops_ = 0;
    uint32_t m_kernel_ops_in_flight_ = 0; // SQEs submitted to kernel

    // Local queue for immediate resumptions to avoid deep call stacks
    std::vector<std::coroutine_handle<>> m_runnable_queue;
    moodycamel::ConcurrentQueue<Job> m_foreign_queue_;

    int m_eventfd = -1;
    uint64_t m_eventfd_buf = 0;  // Buffer for the 8-byte read

    void arm_eventfd() noexcept;
    void write_eventfd() const;

    void assert_owner() const noexcept;
    void tick(std::chrono::nanoseconds timeout_ns) noexcept;
    void process_foreign_jobs() noexcept;

    std::thread::id m_owner_thread_;

public:
    explicit IoContext(std::uint32_t entries = 16800, unsigned flags = kUringDefaultFlag);
    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    ~IoContext();

    Token allocate_token() noexcept;

    void free_token(Token t) noexcept;
    void on_sqe_submitted() noexcept { ++m_kernel_ops_in_flight_; }

    void submit_job(Job job) noexcept;
    void flush() noexcept
    {
        assert_owner();
        io_uring_submit(&m_ring_);
    }

    OpState& get_state(const Token t) noexcept { return m_op_slab_[t.idx]; }
    io_uring_sqe* get_sqe() noexcept
    {
        assert_owner();
        return io_uring_get_sqe(&m_ring_);
    }
    io_uring_sqe* get_sqe_safe() noexcept
    {
        assert_owner();
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring_);
        if (!sqe)
        {
            // The SQ ring is full. Flush the current batch to the kernel.
            io_uring_submit(&m_ring_);
            sqe = io_uring_get_sqe(&m_ring_);
            assert(sqe != nullptr && "SQ ring still full after flush!");
        }
        return sqe;
    }

    void submit_cancel(const Token t) noexcept
    {
        assert_owner();
        io_uring_sqe* sqe = get_sqe_safe();
        io_uring_prep_cancel64(sqe, t.to_u64(), 0);
        io_uring_sqe_set_data64(sqe, 0);
    }

    void on_cqe(const io_uring_cqe* cqe) noexcept;

    // Templated run accepts any chrono duration, defaults to 10ms
    /// tick_timeout is the time a tick will wait for the kernel IO to arrive
    /// And alternative would have been an eventfd but lets start here for now
    template <typename Rep, typename Period>
    void run(std::stop_token st, std::chrono::duration<Rep, Period> tick_timeout) noexcept
    {
        assert_owner();
        auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_timeout);
        while (!st.stop_requested())
        {
            tick(timeout_ns);
        }
    }

    // Convenience overload for the default timeout
    void run(std::stop_token st) noexcept { run(std::move(st), std::chrono::milliseconds(10)); }

    [[nodiscard]] bool is_owner_thread() const noexcept {
        return m_owner_thread_ == std::this_thread::get_id();
    }
};
}  // namespace URing
