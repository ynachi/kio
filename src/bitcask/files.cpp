//
// Created by Yao ACHI on 04/02/2026.
//

#include "bitcask/files.hpp"

#include "kio/kio.hpp"

#include <crc32c/crc32c.h>

namespace bitcask
{
    namespace
    {
        // Helper to compute CRC32C over multiple spans using Extend
        uint32_t ComputeCrc32c(const uint64_t timestamp, const uint8_t flag, const uint32_t key_len,
                               const uint32_t val_len,
                               const std::string_view key, std::span<const std::byte> value)
        {
            // Serialize the metadata part used for CRC calculation (Timestamp..Value)
            // Note: DataEntry layout for CRC is [Timestamp(8)][Flag(1)][KeyLen(4)][ValueLen(4)][Key][Value]
            // The CRC field itself is at the start and excluded.

            // We construct a small buffer for the metadata part
            std::byte meta_buf[17]; // 8 + 1 + 4 + 4
            WriteLe(meta_buf, timestamp);
            WriteLe(meta_buf + 8, flag);
            WriteLe(meta_buf + 9, key_len);
            WriteLe(meta_buf + 13, val_len);

            // Calculate CRC incrementally to avoid allocating a large buffer
            uint32_t crc = crc32c::Crc32c(reinterpret_cast<const uint8_t*>(meta_buf), sizeof(meta_buf));
            crc = crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(key.data()), key.size());
            crc = crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(value.data()), value.size());

            return crc;
        }
    } // namespace

    kio::Task<kio::Result<SharedFD>> FDCache::GetOrOpen(kio::IoContext& ctx, uint64_t file_id,
                                                        const std::filesystem::path& path)
    {
        // Cache hit
        if (const auto it = cache_.find(file_id); it != cache_.end())
        {
            stats_.hits++;
            Touch(file_id);
            co_return it->second.handle;
        }

        // Cache miss
        stats_.misses++;

        // Evict if at capacity
        if (cache_.size() >= max_open_files_)
        {
            EvictOldest();
        }

        // Open file
        auto fd = KIO_CO_TRY_LOG(co_await kio::AsyncOpen(ctx, path, O_RDONLY, 0));

        auto shared_fd = std::make_shared<kio::FD>(std::move(fd));

        // Add to cache
        lru_list_.push_front(file_id);
        cache_.emplace(file_id, CacheEntry{.handle = shared_fd, .path = path, .lru_iter = lru_list_.begin()});

        ALOG_DEBUG("FdCache: Opened file {} (cache size: {})", file_id, cache_.size());

        co_return shared_fd;
    }

    void FDCache::Remove(uint64_t file_id)
    {
        if (const auto it = cache_.find(file_id); it != cache_.end())
        {
            lru_list_.erase(it->second.lru_iter);

            cache_.erase(it);
        }
    }

    void FDCache::Touch(const uint64_t file_id)
    {
        const auto it = cache_.find(file_id);
        if (it == cache_.end())
        {
            return;
        }

        auto& entry = it->second;
        lru_list_.erase(entry.lru_iter);
        lru_list_.push_front(file_id);
        entry.lru_iter = lru_list_.begin();
    }

    void FDCache::EvictOldest()
    {
        if (lru_list_.empty()) return;

        const uint64_t old_id = lru_list_.back();
        auto it = cache_.find(old_id);

        if (it != cache_.end())
        {
            cache_.erase(it);
            lru_list_.pop_back();
            stats_.evictions++;
        }
    }

    //=====================================================
    // Datafile
    //=====================================================
    kio::Task<kio::Result<uint64_t>> DataFile::AsyncWrite(kio::IoContext& ctx, const DataEntry& entry)
    {
        const size_t entry_size = entry.Size();

        const uint64_t entry_offset = size_;
        size_ += entry_size;

        // Now perform the writing - other coroutines will see updated size_
        KIO_CO_TRY_LOG(co_await kio::AsyncWriteExact(ctx, fd_->Get(), entry.GetPayloadSpan(), entry_offset));

        if (config_.sync_on_write)
        {
            KIO_CO_TRY_LOG(co_await kio::AsyncFdatasync(ctx, fd_->Get()));
        }

        co_return entry_offset;
    }

    kio::Task<kio::Result<uint64_t>> DataFile::AsyncWrite(kio::IoContext& ctx, std::string_view key,
                                                          std::span<const std::byte> value, const uint64_t timestamp,
                                                          const uint8_t flag)
    {
        // Calculate entry size
        const auto key_len = static_cast<uint32_t>(key.size());
        const auto val_len = static_cast<uint32_t>(value.size());
        const size_t entry_size = kEntryFixedHeaderSize + key_len + val_len;

        const uint64_t entry_offset = size_;
        size_ += entry_size;

        // Prepare Header (21 bytes)
        // Layout: [CRC(4)][Timestamp(8)][Flag(1)][KeyLen(4)][ValueLen(4)]
        char header_buf[kEntryFixedHeaderSize];

        // Calculate CRC
        uint32_t const crc = ComputeCrc32c(timestamp, flag, key_len, val_len, key, value);

        // Write Header
        WriteLe(header_buf, crc);
        WriteLe(header_buf + 4, timestamp);
        WriteLe(header_buf + 12, flag);
        WriteLe(header_buf + 13, key_len);
        WriteLe(header_buf + 17, val_len);

        // Prepare IO Vectors
        iovec iov[3];
        iov[0].iov_base = const_cast<void*>(static_cast<const void*>(header_buf));
        iov[0].iov_len = kEntryFixedHeaderSize;
        iov[1].iov_base = const_cast<void*>(static_cast<const void*>(key.data()));
        iov[1].iov_len = key.size();
        iov[2].iov_base = const_cast<void*>(static_cast<const void*>(value.data()));
        iov[2].iov_len = value.size();

        auto bytes_written = KIO_CO_TRY_LOG(co_await kio::AsyncWritev(ctx, fd_->Get(), iov, entry_offset));

        if (std::cmp_not_equal(bytes_written, entry_size))
        {
            // Partial write - also creates a hole/corrupt entry
            ALOG_ERROR("Partial write: expected {} bytes, wrote {} at offset {}", entry_size, bytes_written,
                       entry_offset);
            co_return std::unexpected(std::make_error_code(std::errc::io_error) // EIO
            );
        }

        if (config_.sync_on_write)
        {
            KIO_CO_TRY_LOG(co_await kio::AsyncFdatasync(ctx, fd_->Get()));
        }

        co_return entry_offset;
    }
} // namespace bitcask
