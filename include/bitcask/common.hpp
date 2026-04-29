#pragma once

#include "kio/kio.hpp"

#define XXH_INLINE_ALL
#include "library/cpp/xxhash/xxhash.h"

#include <bit>
#include <cstring>
#include <expected>
#include <vector>

#include <sys/stat.h>


namespace bitcask
{
    using namespace std::literals;

    struct BitcaskConfig
    {
        std::filesystem::path directory;

        size_t max_tasks_per_io_engine = 16800;

        mode_t file_mode = 0644;
        mode_t dir_mode = 0755;
        int read_flags = O_RDONLY;

        /**
         * @brief Flags for opening data files for writing.
         *
         * WARNING: Do NOT include O_APPEND!
         *
         * DataFile uses pwrite() with explicit offsets for concurrent coroutine safety.
         * On Linux, O_APPEND causes pwrite() to IGNORE the offset and always write at EOF.
         * This breaks our internal offset tracking and causes data corruption.
         *
         * The constructor will throw if O_APPEND is detected.
         *
         * Safe flags: O_CREAT | O_RDWR (default)
         * We need O_RDWR because Partition::Get() may read from the active file
         * to satisfy read-your-writes consistency.
         */
        int write_flags = O_CREAT | O_RDWR;

        // File rotation
        size_t max_file_size = 100 * 1024 * 1024; // 100MB

        // Durability
        bool sync_on_write = false;
        std::chrono::milliseconds sync_interval{1000ms};

        // Compaction
        bool auto_compact = true;
        double fragmentation_threshold = 0.5; // 50% dead data
        std::chrono::milliseconds compaction_interval_s{120s};

        // Performance
        size_t read_buffer_size = 64 * 1024; // 64KB
        size_t write_buffer_size = 4096;

        // Limit open FDs per partition
        size_t max_open_sealed_files = 100;

        /**
         * @brief Validate configuration.
         * @throws std::invalid_argument if the configuration is invalid
         */
        kio::Result<> Validate() const noexcept
        {
            if (directory.empty())
            {
                ALOG_ERROR("BitcaskConfig: directory cannot be empty");
                return ErrorFromErrc(std::errc::invalid_argument);
            }

            if ((write_flags & O_APPEND) != 0)
            {
                ALOG_ERROR(
                    "BitcaskConfig: write_flags must NOT include O_APPEND. "
                    "O_APPEND breaks pwrite() offset semantics and causes data corruption.");
                return ErrorFromErrc(std::errc::invalid_argument);
            }

            if (max_file_size == 0)
            {
                ALOG_ERROR("BitcaskConfig: max_file_size must be > 0");
                return ErrorFromErrc(std::errc::invalid_argument);
            }
        }
    };

    //============================================================================
    //
    //============================================================================
    constexpr std::uint8_t kFlagNone = 0x00;
    constexpr uint8_t kFlagTombstone = 0x01;
    // [crc(4)][Timestamp(8)][Flag(1)][KeyLen(4)][ValueLen(4)]
    constexpr std::size_t kEntryFixedHeaderSize = 21;
    constexpr std::size_t kHintHeaderSize = 24;
    constexpr std::size_t kFSReadChunkSize = 32 * 1024;
    constexpr std::size_t kKeydirDefaultShardCount = 2;

    constexpr uint32_t CLUSTER_PARTITION_COUNT = 256;

    // file formats
    constexpr std::string_view kDataFilePrefix = "data_";
    constexpr std::string_view kHintFilePrefix = "hint_";
    constexpr std::string_view kDataFileExtension = ".db";
    constexpr std::string_view kHintFileExtension = ".ht";
    constexpr std::string_view kManifestFileName = "MANIFEST";
    // Magic number to identify our file format (ASCII 'BKV1')
    constexpr uint32_t kManifestMagic = 0x31564B42;

    /**
     * @brief Database Manifest to ensure topology consistency
     */
    struct Manifest
    {
        uint32_t magic = kManifestMagic;
        uint32_t version = 1;
        uint32_t partition_count{};
    };

    /**
     * @brief Gets the current time as a 64-bit integer.
     * @return The number of T since the UNIX epoch.
     */
    template <typename T = std::chrono::nanoseconds>
    std::uint64_t GetCurrentTimestamp()
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<T>(now.time_since_epoch()).count();
    }

    // =================================================================
    // Universal Endian Helpers (C++23)
    // =================================================================

    /**
     * @brief Writes an integer to a memory location in Little Endian format.
     * @note Accepts void* so it works with char*, uint8_t*, std::byte*, etc.
     */
    template <std::integral T>
    void WriteLe(void* dest, T value)
    {
        // 1. Swap bytes if running on a Big Endian machine (network order)
        if constexpr (std::endian::native == std::endian::big)
        {
            value = std::byteswap(value);
        }

        std::memcpy(dest, &value, sizeof(T));
    }

    /**
     * @brief Reads an integer from a memory location in Little Endian format.
     */
    template <std::integral T>
    T ReadLe(const void* src)
    {
        T value;
        std::memcpy(&value, src, sizeof(T));

        if constexpr (std::endian::native == std::endian::big)
        {
            return std::byteswap(value);
        }

        return value;
    }

    /**
     * @brief Convenience overload to write at an offset from a byte pointer.
     * Handles the pointer arithmetic casting for you.
     */
    template <std::integral T>
    void WriteLe(std::byte* base, std::size_t offset, T value)
    {
        // Internally cast to uint8_t* to perform the addition safely
        WriteLe(reinterpret_cast<uint8_t*>(base) + offset, value);
    }

    template <std::integral T>
    T ReadLe(const std::byte* base, std::size_t offset)
    {
        return ReadLe<T>(reinterpret_cast<const uint8_t*>(base) + offset);
    }

    /// Read the hint file entirely, they are small
    kio::Task<kio::Result<std::vector<char>>> ReadFileContent(kio::IoContext& ctx, int fd);

    inline kio::Result<size_t> GetFileSize(const int fd)
    {
        struct stat st{};
        if (::fstat(fd, &st) < 0)
        {
            return kio::ErrorFromErrno(errno);
        }
        return st.st_size;
    }

    inline uint64_t Hash(std::string_view data)
    {
        return XXH3_64bits(data.data(), data.size());
    }

    inline uint64_t Hash(std::span<char> data)
    {
        return XXH3_64bits(data.data(), data.size());
    }

    inline uint64_t Hash(std::span<const char> data)
    {
        return XXH3_64bits(data.data(), data.size());
    }
} // namespace bitcask
