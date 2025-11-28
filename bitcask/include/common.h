//
// Created by Yao ACHI on 08/11/2025.
//

// Shared const for bitcask

#ifndef KIO_CONST_H
#define KIO_CONST_H
#include <sys/stat.h>

#include "core/include/coro.h"
#include "core/include/errors.h"
#include "core/include/io/worker.h"

namespace bitcask
{
    constexpr std::uint8_t kFlagNone = 0x00;
    constexpr uint8_t kFlagTombstone = 0x01;
    constexpr std::size_t kEntryFixedHeaderSize = 12;  // at least CRC + PAYLOAD SIZE
    constexpr std::size_t kHintHeaderSize = 24;
    constexpr std::size_t kFSReadChunkSize = 32 * 1024;
    constexpr std::size_t kKeydirDefaultShardCount = 2;

    // file formats
    constexpr std::string_view kDataFilePrefix = "data_";
    constexpr std::string_view kHintFilePrefix = "hint_";
    constexpr std::string_view kDataFileExtension = ".db";
    constexpr std::string_view kHintFileExtension = ".ht";

    /**
     * @brief Gets the current time as a 64-bit integer.
     * @return The number of T since the UNIX epoch.
     */
    template<typename T = std::chrono::nanoseconds>
    std::uint64_t get_current_timestamp()
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<T>(now.time_since_epoch()).count();
    }

    /**
     * @brief Reads a Little-Endian integer from a raw buffer.
     *
     * This function safely reads a value of type T (e.g., uint32_t, uint64_t)
     * from a byte buffer that is known to be in Little-Endian format.
     * It correctly handles the byte order, swapping if the host machine
     * is Big-Endian.
     *
     * @tparam T The integer type to read (e.g., uint32_t, uint64_t).
     * @param buffer A pointer to the start of the byte buffer.
     * @return The integer value in the host machine's native format.
     */
    template<typename T>
    constexpr T read_le(const char* buffer)
    {
        // Safely copy the bytes from the buffer into the integer.
        // This avoids strict-aliasing violations from reinterpret_cast.
        T val;
        std::memcpy(&val, buffer, sizeof(T));

        // At compile-time, check if our machine is Big-Endian.
        if constexpr (std::endian::native == std::endian::big)
        {
            // If so, swap the bytes to convert from LE to BE.
            return std::byteswap(val);
        }
        else
        {
            // If we are on a Little-Endian machine, the bytes are already correct. Do nothing.
            return val;
        }
    }

    /// Read the hint file entirely, they are small
    kio::Task<kio::Result<std::vector<char>>> read_file_content(kio::io::Worker& io_worker, int fd);

    inline kio::Result<size_t> get_file_size(const int fd)
    {
        struct stat st{};
        // this is a blocking system call, but it's ok. We call this method only during database init.
        // Otherwise, do not use it anywhere else as it would block the whole event loop
        if (::fstat(fd, &st) < 0)
        {
            return std::unexpected(kio::Error::from_errno(errno));
        }
        return st.st_size;
    }

}  // namespace bitcask

#endif  // KIO_CONST_H
