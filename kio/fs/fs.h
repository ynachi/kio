//
// Created by Yao ACHI on 06/10/2025.
//

#ifndef KIO_FS_H
#define KIO_FS_H
#include <expected>

#include "../core/coro.h"
#include "../core/errors.h"
#include "../core/worker_pool.h"

namespace kio
{
    class File
    {
        int fd_{-1};
        io::IOPool &pool_;
        size_t worker_id_{0};

    public:
        File(const int fd, io::IOPool &pool, size_t worker_id) : fd_(fd), pool_(pool), worker_id_(worker_id) { assert(fd_ >= 0); }

        // File is not copyable
        File(const File &) = delete;

        File &operator=(const File &) = delete;

        File(File &&other) noexcept : fd_(other.fd_), pool_(other.pool_), worker_id_(other.worker_id_) { other.fd_ = -1; }

        [[nodiscard]]
        int fd() const noexcept
        {
            return fd_;
        }

        File &operator=(File &&other) noexcept
        {
            if (this != &other)
            {
                if (fd_ != -1) ::close(fd_);  // Close existing fd first
                fd_ = other.fd_;
                // pool_ reference stays bound to the same pool (can't be rebound)
                worker_id_ = other.worker_id_;
                other.fd_ = -1;
            }
            return *this;
        }

        ~File()
        {
            if (fd_ != -1) ::close(fd_);
        }

        void close()
        {
            if (fd_ != -1) ::close(fd_);
            fd_ = -1;
        }

        /**
         * Read a chunk of the file. This method submits only one call to read from the underlining IO.
         * It can read enough data to fill the buffer or less. Having less does not necessarily mean
         * EOF was seen or an error occurred. A read of 0 bytes signals EOF.
         * @param buf Buffer to read into
         * @param offset Offset to start reading from
         * @return The number of bytes read or an error.
         * */
        [[nodiscard]]
        Task<Result<size_t>> async_read(std::span<char> buf, uint64_t offset) const;

        /**
         *  This method makes a single write() call to the underlined IO. It may or may not write the total of the data.
         *  It returns the number of bytes read, and returning lower bytes written than the buffer size may not be an error.
         * @param buf Non owning reference of the data to write.
         * @param offset
         * @return The number of bytes written or an error
         */
        [[nodiscard]]
        Task<Result<size_t>> async_write(std::span<const char> buf, uint64_t offset) const;
    };

    /**
     * FileManager own an io pool which each coroutine refers to via a non-owning reference.
     * So, the developer must make sure the instance of a FileManager outlives each file and coroutine object.
     * It's a tradeoff we made to avoid paying the cost of shared pointers.
     * A rule of thumbs is to not let a coroutine own the file manager instance.
     */
    class FileManager
    {
        io::IOPool pool_;

    public:
        FileManager(size_t io_worker_count, const io::WorkerConfig &config);

        FileManager(const FileManager &) = delete;

        FileManager &operator=(const FileManager &) = delete;

        // FileManager is not movable
        FileManager(FileManager &&) = delete;

        FileManager &operator=(FileManager &&) = delete;

        io::IOPool &pool() { return pool_; }

        ~FileManager() { pool_.stop(); }

        /**
         * Creates a file asynchronously. The file gets assigned an io context based on its path hash.
         * This means that every operation related to that file will go through that same context.
         * @param path File path as a string can be absolute or relative.
         * @param flags File opening flags
         * @param mode Permission mode
         * @return The file created or an Io error.
         */
        [[nodiscard]]
        Task<Result<File>> async_open(std::filesystem::path path, int flags, mode_t mode);
    };
}  // namespace kio

#endif  // KIO_FS_H
