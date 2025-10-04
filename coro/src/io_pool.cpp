//
// Created by ynachi on 10/3/25.
//

#include <sys/utsname.h>
#include "../include/io_pool.h"
#include "spdlog/spdlog.h"

namespace kio {

    void IOWorker::check_kernel_version()
    {
        utsname buf{};
        if (uname(&buf) != 0)
        {
            throw std::runtime_error("Failed to get kernel version");
        }

        std::string release(buf.release);
        const size_t dot_pos = release.find('.');
        if (dot_pos == std::string::npos)
        {
            throw std::runtime_error("Failed to parse kernel version");
        }

        if (const int major = std::stoi(release.substr(0, dot_pos)); major < 6)
        {
            throw std::runtime_error("Kernel version must be 6.0.0 or higher");
        }
    }

    void IOWorker::check_syscall_return(const int ret)
    {
        if (ret < 0)
        {
            // timeout
            if (-ret == ETIME || -ret == ETIMEDOUT)
            {
                spdlog::debug("system call has timedout: {}", strerror(-ret));
                return;
            }

            // Interrupted system call
            if (-ret == EINTR)
            {
                spdlog::debug("interrupted, will retry on next call: {}", strerror(-ret));
                return;
            }

            // resource limit reached
            if (-ret == EAGAIN || -ret == EBUSY)
            {
                spdlog::debug("resources limitation, will retry on next call: {}", strerror(-ret));
                return;
            }

            spdlog::error("failed to process io requests, fatal error: {}", strerror(-ret));
            throw std::system_error(-ret, std::system_category(), "system call failed failed");
        }
    }

    [[nodiscard]]
    uint64_t IOWorker::get_op_id() noexcept{
        if (!free_op_ids.empty()) {
            const auto id = free_op_ids.back();
            free_op_ids.pop_back();
            return id;
        }
        // TODO: manage the growth better
        // TODO: add metric for pool growth here
        // TODO: Log here
        const auto id = op_data_pool_.size();
        op_data_pool_.emplace_back();
        return id;
    }

    void IOWorker::release_op_id(const uint64_t op_id) noexcept {
        free_op_ids.push_back(op_id);
        op_data_pool_[op_id] = {};
    }

     IOWorker::IOWorker(
         const IoWorkerConfig &config,
         std::shared_ptr<std::latch> init_latch,
         std::shared_ptr<std::latch> shutdown_latch):
    config_(config), init_latch_(std::move(init_latch)), shutdown_latch_(std::move(shutdown_latch))
    {
        config.check();
        check_kernel_version();

        io_uring_params params{};
        params.flags |= IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

        if (const int ret = io_uring_queue_init_params(config_.uring_queue_depth, &ring_, &params); ret < 0)
        {
            throw std::system_error(-ret, std::system_category(), "io_uring_queue_init failed");
        }

        spdlog::info("IO uring initialized with queue size of {}", config_.uring_queue_depth);
    }

}


}