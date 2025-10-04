//
// Created by ynachi on 10/3/25.
//

#ifndef KIO_IO_POOL_H
#define KIO_IO_POOL_H
#include <coroutine>
#include <liburing.h>
#include <vector>
#include <concepts>
#include <latch>
#include <memory>

namespace kio {

    // stop sentinel
    static char ioWorkerStopSentinelObj;
    constexpr void* ioWorkerStopSentinel = &ioWorkerStopSentinelObj;

    struct IoWorkerConfig
    {
        size_t uring_queue_depth{1024};
        size_t uring_submit_batch_size{128};
        size_t tcp_backlog{128};
        uint8_t uring_submit_timeout_ms{100};
        int uring_default_flags = 0;
        size_t default_op_slots{1024};

        constexpr IoWorkerConfig() = default;
        void check() const {
            if (uring_queue_depth == 0 || uring_submit_batch_size == 0 || tcp_backlog == 0) {
                throw std::invalid_argument("Invalid worker config, some fields cannot be zero");
            }
        }
    };

    class IOWorker;

    // to avoid passing the worker around every time.
    extern thread_local IOWorker *io_worker;

    class IOWorker {
        io_uring ring_;
        IoWorkerConfig config_;

        struct io_op_data {
            std::coroutine_handle<> handle_;
            int result{-1};
        };

        std::vector<io_op_data> op_data_pool_;
        std::vector<uint64_t> free_op_ids;

        std::shared_ptr<std::latch> init_latch_;
        std::shared_ptr<std::latch> shutdown_latch_;

        static void check_kernel_version();
        static void check_syscall_return(int ret);

    public:
        io_uring& get_ring() noexcept { return ring_; }
        [[nodiscard]]
        uint64_t get_op_id() noexcept;
        void release_op_id(uint64_t op_id) noexcept;
        void init_op_slot(uint64_t op_id, std::coroutine_handle<> h) noexcept {
            op_data_pool_[op_id] = {h, 0};
        }
        int64_t get_op_result(uint64_t op_id) noexcept {
            auto result = op_data_pool_[op_id].result;
            release_op_id(op_id);
            return result;
        }
        void set_op_result(uint64_t op_id, int result) noexcept {
            op_data_pool_[op_id].result = result;
        }
        IOWorker(const IOWorker&) = delete;
        IOWorker& operator=(const IOWorker&) = delete;
        IOWorker(IOWorker&&) = delete;
        IOWorker& operator=(IOWorker&&) = delete;
        IOWorker(const IoWorkerConfig& config, std::shared_ptr<std::latch> init_latch,
                           std::shared_ptr<std::latch> shutdown_latch);
        ~IOWorker();
    };

    template<typename T>
    concept IoUringPrep = requires(T t, io_uring_sqe* sqe) {
            { t.prepare(sqe) } -> std::same_as<void>;
    };


    /**
 * IoUringAwaitable, c++20 coroutine awaiter, controls the suspension and awaiting of
 * the io_uring operations.
 */
    template<IoUringPrep Prep, typename... Args>
    struct IoUringAwaitable
    {
        io_uring_sqe* sqe_ = nullptr;
        std::coroutine_handle<> handle_ = nullptr;
        uint64_t op_id_{0};
        Prep io_uring_pre_;
        std::tuple<Args...> args_;
        int immediate_result_{0};

        static bool await_ready() noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) noexcept
        {
            sqe_ = io_uring_get_sqe(&io_worker->get_ring());
            if (sqe_ == nullptr) {
                immediate_result_ = -EAGAIN;
                h.resume();
                return;
            }

            // apply io_uring prep

            std::apply(io_uring_pre_, std::tuple_cat(std::make_tuple(sqe_), args_));

            // assign an operation id
            op_id_ = io_worker->get_op_id();

            // save the op_id to the submission entry
            io_uring_sqe_set_data(sqe_, reinterpret_cast<void*>(op_id_));
        }

        [[nodiscard]] int await_resume() const noexcept {
            if (immediate_result_ != 0) {
                return immediate_result_;
            }

            return io_worker->get_op_result(op_id_);
        };

        explicit IoUringAwaitable(Prep&& prep, Args&&... args)
        : io_uring_pre_(std::forward<Prep>(prep))
        , args_(std::forward<Args>(args)...)
        {}

    };

}

#endif //KIO_IO_POOL_H
