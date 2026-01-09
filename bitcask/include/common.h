//
// Created by Yao ACHI on 08/11/2025.
//

// Shared const for bitcask

#ifndef KIO_CONST_H
#define KIO_CONST_H
#include "kio/core/coro.h"
#include "kio/core/errors.h"
#include "kio/core/worker.h"

#define XXH_INLINE_ALL
#include "kio/third_party/xxhash/xxhash.h"

#include <bit>
#include <cstring>
#include <expected>
#include <vector>

#include <sys/stat.h>


namespace bitcask
{
constexpr std::uint8_t kFlagNone = 0x00;
constexpr uint8_t kFlagTombstone = 0x01;
// [crc(4)][Timestamp(8)][Flag(1)][KeyLen(4)][ValueLen(4)]
constexpr std::size_t kEntryFixedHeaderSize = 21;
constexpr std::size_t kHintHeaderSize = 24;
constexpr std::size_t kFSReadChunkSize = 32 * 1024;
constexpr std::size_t kKeydirDefaultShardCount = 2;

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

// Helper: ByteSwap (Uses std::byteswap in C++23, or a C++20 fallback)
template <typename T>
constexpr T ByteSwap(T value) noexcept
{
#if __cpp_lib_byteswap
    return std::byteswap(value);
#else
    auto value_representation = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    std::ranges::reverse(value_representation);
    return std::bit_cast<T>(value_representation);
#endif
}

// Read Little Endian from the buffer
template <typename T>
constexpr T ReadLe(const char* buffer)
{
    T val;
    std::memcpy(&val, buffer, sizeof(T));
    if constexpr (std::endian::native == std::endian::big)
    {
        return ByteSwap(val);
    }
    else
    {
        return val;
    }
}

// Write Little Endian to buffer
template <typename T>
constexpr void WriteLe(char* buffer, T value)
{
    if constexpr (std::endian::native == std::endian::big)
    {
        value = ByteSwap(value);
    }
    std::memcpy(buffer, &value, sizeof(T));
}

/// Read the hint file entirely, they are small
kio::Task<kio::Result<std::vector<char>>> ReadFileContent(kio::io::Worker& io_worker, int fd);

inline kio::Result<size_t> GetFileSize(const int fd)
{
    struct stat st{};
    if (::fstat(fd, &st) < 0)
    {
        return std::unexpected(kio::Error::FromErrno(errno));
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

}  // namespace bitcask

#endif  // KIO_CONST_H
