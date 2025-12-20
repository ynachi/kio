//
// Created by Yao ACHI on 08/11/2025.
//

// Shared const for bitcask

#ifndef KIO_CONST_H
#define KIO_CONST_H
#include <bit>
#include <cstring>
#include <expected>
#include <sys/stat.h>
#include <vector>
#include <ylt/struct_pack.hpp>

#include "kio/core/coro.h"
#include "kio/core/errors.h"
#include "kio/core/worker.h"

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
template<typename T = std::chrono::nanoseconds>
std::uint64_t GetCurrentTimestamp()
{
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<T>(now.time_since_epoch()).count();
}

/**
 * @brief Reads a Little-Endian integer from a raw buffer.
 */
template<typename T>
constexpr T read_le(const char* buffer)
{
    T val;
    std::memcpy(&val, buffer, sizeof(T));
    if constexpr (std::endian::native == std::endian::big)
    {
        return std::byteswap(val);
    }
    else
    {
        return val;
    }
}

/// Read the hint file entirely, they are small
kio::Task<kio::Result<std::vector<char>>> read_file_content(kio::io::Worker& io_worker, int fd);

inline kio::Result<size_t> get_file_size(const int fd)
{
    struct stat st{};
    if (::fstat(fd, &st) < 0)
    {
        return std::unexpected(kio::Error::FromErrno(errno));
    }
    return st.st_size;
}

// Reflection for struct_pack
YLT_REFL(Manifest, magic, version, partition_count);

}  // namespace bitcask

#endif  // KIO_CONST_H
