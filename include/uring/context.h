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
#include "operation.hpp"

namespace URing
{
template <typename T>
struct Task;

class IoContext
{
public:
    static constexpr unsigned kUringDefaultFlag =
        IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;

private:
    template <typename T>
    friend struct Task;

    OpPool op_pool_;
    io_uring ring_{};
    std::vector<std::coroutine_handle<>> ready_queue_;
    std::vector<std::coroutine_handle<>> process_queue_;
    static constexpr size_t MAX_RESUMES_PER_TICK = 128;
    std::thread::id owner_thread_;
    std::stop_token stop_token_;

    void request_cancel(uint32_t op_idx) noexcept;

    void tick(std::chrono::nanoseconds timeout_ns) noexcept;

public:
    explicit IoContext(std::uint32_t entries = 16800, unsigned flags = kUringDefaultFlag);
    IoContext(const IoContext&) = delete;
    IoContext& operator=(const IoContext&) = delete;
    IoContext(IoContext&&) = delete;
    IoContext& operator=(IoContext&&) = delete;

    ~IoContext();

    OpPool& pool() noexcept { return op_pool_; }
    io_uring& ring() noexcept { return ring_; }

    // Templated run accepts any chrono duration, defaults to 10ms
    /// tick_timeout is the time a tick will wait for the kernel IO to arrive
    /// And alternative would have been an eventfd but lets start here for now
    template <typename Rep, typename Period>
    void run(std::stop_token st, std::chrono::duration<Rep, Period> tick_timeout) noexcept
    {
        auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_timeout);
        while (!st.stop_requested())
        {
            tick(timeout_ns);
        }
    }

    [[nodiscard]] bool is_owner_thread() const noexcept { return owner_thread_ == std::this_thread::get_id(); }
};
}  // namespace URing
