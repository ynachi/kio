#pragma once

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

namespace aio
{

/// A reusable pipe (read_fd, write_fd)
struct Pipe
{
    int read_fd = -1;
    int write_fd = -1;

    bool Valid() const { return read_fd >= 0 && write_fd >= 0; }

    void Close()
    {
        if (read_fd >= 0)
        {
            ::close(read_fd);
            read_fd = -1;
        }
        if (write_fd >= 0)
        {
            ::close(write_fd);
            write_fd = -1;
        }
    }
};

/**
 * A simple pool of pipes for efficient sendfile operations.
 *
 * Pipes are created lazily and reused to avoid the overhead of
 * pipe()/close() syscalls on every sendfile call.
 */
class PipePool
{
public:
    explicit PipePool(size_t max_size = 4) : max_size_(max_size) { pool_.reserve(max_size); }

    ~PipePool()
    {
        for (auto& p : pool_)
        {
            p.Close();
        }
    }

    PipePool(const PipePool&) = delete;
    PipePool& operator=(const PipePool&) = delete;
    PipePool(PipePool&&) = default;
    PipePool& operator=(PipePool&&) = default;

    /**
     * Acquire a pipe from the pool, or create a new one.
     * Returns std::nullopt if pipe creation fails.
     */
    std::optional<Pipe> Acquire()
    {
        if (!pool_.empty())
        {
            Pipe p = pool_.back();
            pool_.pop_back();
            return p;
        }

        // Create new pipe
        int fds[2];
        if (::pipe(fds) < 0)
        {
            return std::nullopt;
        }

        // Set non-blocking for async operations
        ::fcntl(fds[0], F_SETFL, O_NONBLOCK);
        ::fcntl(fds[1], F_SETFL, O_NONBLOCK);

        return Pipe{fds[0], fds[1]};
    }

    /**
     * Return a pipe to the pool for reuse.
     * If the pool is full, the pipe is closed.
     */
    void Release(Pipe p)
    {
        if (!p.Valid())
            return;

        if (pool_.size() < max_size_)
        {
            pool_.push_back(p);
        }
        else
        {
            p.Close();
        }
    }

    /**
     * RAII guard for automatic pipe release.
     */
    class Guard
    {
    public:
        Guard(PipePool& pool, Pipe p) : pool_(&pool), pipe_(p) {}
        ~Guard()
        {
            if (pool_)
                pool_->Release(pipe_);
        }

        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& o) noexcept : pool_(o.pool_), pipe_(o.pipe_) { o.pool_ = nullptr; }
        Guard& operator=(Guard&&) = delete;

        Pipe& get() { return pipe_; }
        const Pipe& get() const { return pipe_; }

    private:
        PipePool* pool_;
        Pipe pipe_;
    };

    /**
     * Acquire a pipe with RAII guard for automatic release.
     */
    std::optional<Guard> AcquireGuarded()
    {
        auto p = Acquire();
        if (!p)
            return std::nullopt;
        return Guard(*this, *p);
    }

private:
    std::vector<Pipe> pool_;
    size_t max_size_;
};

}  // namespace aio
