//
// Created by Yao ACHI on 14/11/2025.
//
#include "bitcask/include/compactor.h"

#include <format>
#include <ylt/struct_pack.hpp>

#include "bitcask/include/entry.h"

using namespace kio;
using namespace kio::io;

namespace bitcask
{
    Compactor::Compactor(Worker& io_worker, const BitcaskConfig& config, SimpleKeydir& keydir, const CompactionLimits& limits) :
        io_worker_(io_worker), config_(config), keydir_(keydir), limits_(limits), current_data_offset_(0), current_hint_offset_(0)
    {
        ALOG_INFO("Compactor initialized for N-to-1 merge compaction");
        data_batch_.reserve(config.write_buffer_size);
        hint_batch_.reserve(limits.max_hint_batch);
        keydir_batch_.reserve(limits.max_keydir_batch);
        // decode_buffer_ will be resized dynamically
        decode_buffer_.reserve(config.read_buffer_size * 2);
    }

    Compactor::~Compactor()
    {
        ALOG_DEBUG("Compactor shutting down...");
        if (!io_worker_.request_stop())
        {
            ALOG_ERROR("Failed to request compactor shutdown");
        }
        io_worker_.wait_shutdown();
        ALOG_INFO("Compactor shut down successfully");
    }

    void Compactor::reset_for_new_merge()
    {
        current_data_offset_ = 0;
        current_hint_offset_ = 0;
        reset_batches();
        decode_buffer_.clear();
        ALOG_DEBUG("Compactor state reset for new merge");
    }

    Task<Result<std::pair<int, uint64_t>>> Compactor::open_source_file(uint64_t src_file_id) const
    {
        const auto src_path = config_.directory / std::format("data_{}.db", src_file_id);
        const auto src_fd = KIO_TRY(co_await io_worker_.async_openat(src_path, config_.read_flags, config_.file_mode));

        // Get file size
        struct stat st{};
        uint64_t file_size = 0;
        if (::fstat(src_fd, &st) == 0)
        {
            file_size = st.st_size;
        }

        ALOG_DEBUG("Opened source file {} ({}KB)", src_file_id, file_size / 1024);
        co_return std::pair{src_fd, file_size};
    }

    Task<Result<void>> Compactor::cleanup_source_files(uint64_t src_file_id) const
    {
        const auto data_path = config_.directory / std::format("data_{}.db", src_file_id);
        const auto hint_path = config_.directory / std::format("hint_{}.ht", src_file_id);

        // Remove data file
        if (auto res_data = co_await io_worker_.async_unlink_at(AT_FDCWD, data_path, 0); !res_data.has_value())
        {
            // Log but don't necessarily fail compaction because of this
            ALOG_WARN("Failed to remove data file {}: {}", src_file_id, res_data.error());
        }

        // Remove the hint file
        if (auto res_hint = co_await io_worker_.async_unlink_at(AT_FDCWD, hint_path, 0); !res_hint.has_value())
        {
            // Hint file might not exist, so this is often expected or minor
            ALOG_DEBUG("Failed to remove hint file {}: {}", src_file_id, res_hint.error());
        }

        co_return {};
    }

    bool Compactor::should_flush_batch() const
    {
        return data_batch_.size() >= config_.write_buffer_size || hint_batch_.size() >= limits_.max_hint_batch || keydir_batch_.size() >= limits_.max_keydir_batch;
    }

    void Compactor::reset_batches()
    {
        data_batch_.clear();
        hint_batch_.clear();
        keydir_batch_.clear();
    }

    Task<Result<void>> Compactor::commit_batch(int dst_data_fd, int dst_hint_fd, Stats& stats)
    {
        if (data_batch_.empty()) co_return {};

        ALOG_TRACE("Committing batch: data={}KB, hint={}KB, keydir={} entries", data_batch_.size() / 1024, hint_batch_.size() / 1024, keydir_batch_.size());

        // Write data and hints
        KIO_TRY(co_await io_worker_.async_write_exact(dst_data_fd, std::span(data_batch_)));
        KIO_TRY(co_await io_worker_.async_write_exact(dst_hint_fd, std::span(hint_batch_)));

        // Update KeyDir with CAS
        for (const auto& update: keydir_batch_)
        {
            if (const bool success = update_keydir_if_matches(update.key, update.new_loc, update.expected_file_id, update.expected_offset); !success)
            {
                // This entry was optimistically counted as "kept"
                // Now we know it's actually stale, so fix the counts
                stats.entries_kept--;
                stats.entries_discarded++;
                stats.bytes_written -= update.new_loc.total_size;

                ALOG_TRACE("CAS failed for key '{}': concurrent update detected", update.key);
            }
        }

        current_data_offset_ += data_batch_.size();
        current_hint_offset_ += hint_batch_.size();
        reset_batches();

        co_return {};
    }

    void Compactor::process_parsed_entry(const DataEntry& data_entry, uint64_t decoded_size, uint64_t entry_absolute_offset, uint64_t src_file_id, uint64_t dst_file_id, size_t window_start,
                                         Stats& stats)
    {
        if (!is_live_entry(data_entry, entry_absolute_offset, src_file_id))
        {
            stats.entries_discarded++;
            return;
        }

        std::span<const char> raw_entry(decode_buffer_.data() + window_start, decoded_size);
        ValueLocation new_loc{dst_file_id, current_data_offset_ + data_batch_.size(), static_cast<uint32_t>(decoded_size), data_entry.timestamp_ns};

        HintEntry hint{new_loc.timestamp_ns, new_loc.offset, new_loc.total_size, std::string(data_entry.key)};

        data_batch_.insert(data_batch_.end(), raw_entry.begin(), raw_entry.end());
        struct_pack::serialize_to(hint_batch_, hint);
        keydir_batch_.emplace_back(std::string(data_entry.key), new_loc, src_file_id, entry_absolute_offset);

        // Track both entries and bytes
        stats.entries_kept++;
        stats.bytes_written += decoded_size;
    }

    void Compactor::compact_decode_buffer(size_t& window_start)
    {
        if (window_start > 0)
        {
            // Move remaining data to the front
            std::span leftover(decode_buffer_.data() + window_start, decode_buffer_.size() - window_start);
            std::memmove(decode_buffer_.data(), leftover.data(), leftover.size());
            decode_buffer_.resize(leftover.size());
            window_start = 0;
        }
    }

    Task<Result<void>> Compactor::parse_entries_from_buffer(uint64_t file_read_pos, uint64_t src_file_id, uint64_t dst_file_id, int dst_data_fd, int dst_hint_fd, size_t& window_start, Stats& stats)
    {
        while (true)
        {
            std::span decode_span(decode_buffer_.data() + window_start, decode_buffer_.size() - window_start);
            auto entry_result = DataEntry::deserialize(decode_span);

            if (!entry_result.has_value())
            {
                if (entry_result.error().value == kIoNeedMoreData) break;
                ALOG_ERROR("Deserialization error in file {}: {}", src_file_id, entry_result.error());
                co_return std::unexpected(entry_result.error());
            }

            const auto& [data_entry, decoded_size] = entry_result.value();

            // Calculate absolute file offset of this entry:
            // file_read_pos points PAST the data we just read
            // decode_buffer_ contains everything we've read but not yet processed
            // window_start is the offset within decode_buffer_ where this entry starts
            // So, entry offset = (file_read_pos - buffer_size) + window_start
            const uint64_t buffer_start_in_file = file_read_pos - decode_buffer_.size();
            const uint64_t entry_absolute_offset = buffer_start_in_file + window_start;

            process_parsed_entry(data_entry, decoded_size, entry_absolute_offset, src_file_id, dst_file_id, window_start, stats);

            window_start += decoded_size;

            if (should_flush_batch())
            {
                KIO_TRY(co_await commit_batch(dst_data_fd, dst_hint_fd, stats));
            }
        }
        co_return {};
    }


    Task<Result<void>> Compactor::stream_and_compact_file(int src_fd, int dst_data_fd, int dst_hint_fd, uint64_t src_file_id, uint64_t dst_file_id, Stats& stats)
    {
        uint64_t file_read_pos = 0;
        size_t window_start = 0;

        for (;;)
        {
            // Zero-Copy Optimization: Read directly into the tail of decode_buffer_
            const size_t current_size = decode_buffer_.size();

            if (decode_buffer_.capacity() < current_size + config_.read_buffer_size)
            {
                decode_buffer_.reserve(std::max(decode_buffer_.capacity() * 2, current_size + config_.read_buffer_size));
            }

            // Resize to expose write area
            decode_buffer_.resize(current_size + config_.read_buffer_size);
            std::span write_area(decode_buffer_.data() + current_size, config_.read_buffer_size);

            // Read
            const auto bytes_read = KIO_TRY(co_await io_worker_.async_read_at(src_fd, write_area, file_read_pos));

            // Shrink back to actual size
            decode_buffer_.resize(current_size + bytes_read);

            if (bytes_read == 0)
            {
                if (window_start != decode_buffer_.size())
                {
                    ALOG_ERROR("Corrupt file {}: partial entry at EOF", src_file_id);
                    co_return std::unexpected(Error{ErrorCategory::File, kIoDataCorrupted});
                }
                break;
            }

            file_read_pos += bytes_read;
            stats.bytes_read += bytes_read;

            if (decode_buffer_.size() > limits_.max_decode_buffer)
            {
                ALOG_ERROR("Decode buffer overflow");
                co_return std::unexpected(Error{ErrorCategory::Application, kIoDataCorrupted});
            }

            KIO_TRY(co_await parse_entries_from_buffer(file_read_pos, src_file_id, dst_file_id, dst_data_fd, dst_hint_fd, window_start, stats));

            compact_decode_buffer(window_start);
        }
        co_return {};
    }

    Task<Result<Compactor::Stats>> Compactor::compact_one(uint64_t src_file_id, int dst_data_fd, int dst_hint_fd, uint64_t dst_file_id)
    {
        const auto start_time = std::chrono::steady_clock::now();
        Stats stats;
        reset_batches();
        decode_buffer_.clear();

        ALOG_INFO("Compacting file {} -> {}", src_file_id, dst_file_id);

        auto open_res = co_await open_source_file(src_file_id);
        if (!open_res.has_value())
        {
            co_return std::unexpected(open_res.error());
        }

        // wrap fd to a FileHandle to automatically close fd when it goes out of scope.
        auto src_handle = FileHandle(open_res.value().first);

        auto stream_result = co_await stream_and_compact_file(src_handle.get(), dst_data_fd, dst_hint_fd, src_file_id, dst_file_id, stats);

        if (!stream_result.has_value())
        {
            ALOG_ERROR("Failed compaction for {}: {}", src_file_id, stream_result.error());
            co_return std::unexpected(stream_result.error());
        }

        // Final flush
        KIO_TRY(co_await commit_batch(dst_data_fd, dst_hint_fd, stats));

        // clean up
        KIO_TRY(co_await cleanup_source_files(src_file_id));

        // Update stats
        stats.files_compacted = 1;
        stats.duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time);

        ALOG_INFO("Compacted file {}: kept={} discarded={} written={}KB in {}ms", src_file_id, stats.entries_kept, stats.entries_discarded, stats.bytes_written / 1024, stats.duration.count());

        co_return stats;
    }


    bool Compactor::is_live_entry(const DataEntry& entry, const uint64_t old_offset, const uint64_t old_file_id) const
    {
        if (const auto it = keydir_.find(entry.key); it != keydir_.end())
        {
            return it->second.timestamp_ns == entry.timestamp_ns && it->second.offset == old_offset && it->second.file_id == old_file_id;
        };
        return false;
    }

    bool Compactor::update_keydir_if_matches(const std::string& key, const ValueLocation& new_loc, uint64_t expected_file_id, uint64_t expected_offset) const
    {
        const auto it = keydir_.find(key);

        // If the key doesn't exist, or points to something else, fail.
        if (it == keydir_.end())
        {
            return false;
        }

        if (it->second.file_id != expected_file_id || it->second.offset != expected_offset)
        {
            return false;
        }

        // Match confirmed. Perform update and stats maintenance.
        it->second = new_loc;
        return true;
    }

}  // namespace bitcask
