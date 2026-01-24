# Bitcask Storage Engine - Implementation Fixes

This document provides detailed implementation suggestions for critical issues identified in the code review.

---

## 1. Hash Collision Handling (CRITICAL)

### Problem
Currently, the KeyDir stores only the hash of keys without collision detection:
```cpp
keydir_[key_hash] = new_loc;  // Data loss if two keys hash to same value!
```

### Solution A: Store Full Key in ValueLocation (Recommended)

**Rationale:** Simple, reliable, small memory overhead (typically <50 bytes per key)

```cpp
// entry.h - Update ValueLocation
struct ValueLocation {
    uint64_t file_id{};
    uint64_t offset{};
    uint32_t total_size{};
    uint64_t timestamp_ns = GetCurrentTimestamp();
    std::string key;  // Add actual key for verification
};

// Use std::pmr::string to leverage memory pool
using Keydir = std::pmr::unordered_map<uint64_t, ValueLocation>;
```

**Update Put operation:**
```cpp
// partition.cpp - Put()
const auto key_hash = Hash(entry.GetKeyView());
if (const auto it = keydir_.find(key_hash); it != keydir_.end()) {
    // Verify it's actually the same key (collision detection)
    if (it->second.key != entry.GetKeyView()) {
        // Hash collision detected - log and handle
        ALOG_ERROR("Hash collision detected: '{}' vs '{}'",
                   it->second.key, entry.GetKeyView());
        co_return std::unexpected(Error{ErrorCategory::kApplication, kHashCollision});
    }

    // Update existing
    auto& old_stats = stats_.data_files[it->second.file_id];
    old_stats.live_bytes -= it->second.total_size;
    old_stats.live_entries--;

    it->second = new_loc;
    it->second.key = std::string(entry.GetKeyView());  // Update key
} else {
    // New key
    keydir_[key_hash] = new_loc;
    keydir_[key_hash].key = std::string(entry.GetKeyView());
}
```

**Update Get operation:**
```cpp
// partition.cpp - Get()
const auto key_hash = Hash(key);
const auto it = keydir_.find(key_hash);
if (it == keydir_.end()) {
    stats_.gets_miss_total++;
    co_return std::nullopt;
}

// Verify key matches (detect collisions)
if (it->second.key != key) {
    ALOG_ERROR("Hash collision on Get: expected '{}', keydir has '{}'",
               key, it->second.key);
    stats_.gets_miss_total++;
    co_return std::nullopt;
}

// Continue with read...
```

### Solution B: Chaining for Collisions

**Rationale:** More memory-efficient if collisions are rare, but adds complexity

```cpp
// entry.h
struct ValueLocation {
    uint64_t file_id{};
    uint64_t offset{};
    uint32_t total_size{};
    uint64_t timestamp_ns;
    std::pmr::string key;
};

// Chain entries with same hash
using KeydirChain = std::pmr::vector<ValueLocation>;
using Keydir = std::pmr::unordered_map<uint64_t, KeydirChain>;
```

**Recommendation:** Use Solution A. Hash collisions with XXH3_64bits are astronomically rare (~1 in 2^64), but when they occur, you want simple, debuggable code. The memory overhead is negligible.

---

## 2. Hint File Generation on Rotation

### Problem
Hint files are only generated during compaction, not on rotation. This means every non-compacted file requires slow data file scanning on recovery.

### Solution: Generate Hint Files on Rotation

```cpp
// partition.cpp - Add new method
Task<Result<void>> Partition::GenerateHintFile(uint64_t file_id) {
    co_await SwitchToWorker(worker_);

    const auto data_path = GetDataFilePath(file_id);
    const auto hint_path = GetHintFilePath(file_id);

    // Open data file for reading
    const int data_fd = KIO_TRY(
        co_await worker_.AsyncOpenat(data_path, config_.read_flags, config_.file_mode)
    );
    FileHandle data_handle(data_fd);

    // Create hint file
    const int hint_fd = KIO_TRY(
        co_await worker_.AsyncOpenat(hint_path, config_.write_flags, config_.file_mode)
    );
    FileHandle hint_handle(hint_fd);

    // Read data file and extract metadata
    BytesMut buffer(config_.read_buffer_size * 2);
    uint64_t file_offset = 0;
    std::vector<char> hint_batch;
    hint_batch.reserve(512 * 1024);  // 512KB batching

    const auto file_size = KIO_TRY(GetFileSize(data_fd));

    while (file_offset < file_size) {
        buffer.Reserve(config_.read_buffer_size);
        auto writable = buffer.WritableSpan();

        const uint64_t bytes_to_read = std::min(writable.size(), file_size - file_offset);
        const auto bytes_read = KIO_TRY(
            co_await worker_.AsyncReadAt(data_fd, writable.subspan(0, bytes_to_read), file_offset)
        );

        if (bytes_read == 0) break;

        buffer.CommitWrite(bytes_read);
        file_offset += bytes_read;

        // Parse entries and build hint records
        while (buffer.Remaining() >= kEntryFixedHeaderSize) {
            const auto readable = buffer.ReadableSpan();
            auto entry_result = DataEntry::Deserialize(readable);

            if (!entry_result.has_value()) {
                if (entry_result.error().value == kIoNeedMoreData) break;

                // Corruption - log and stop
                ALOG_WARN("Hint generation stopped at offset {} due to corruption", file_offset);
                goto done_parsing;
            }

            const auto& entry = entry_result.value();
            const uint64_t entry_offset = file_offset - buffer.Remaining();

            // Only write hints for non-tombstones
            if (!entry.IsTombstone()) {
                HintEntry hint{
                    entry.GetTimestamp(),
                    entry_offset,
                    entry.Size(),
                    std::string(entry.GetKeyView())
                };

                auto serialized = hint.Serialize();
                hint_batch.insert(hint_batch.end(), serialized.begin(), serialized.end());

                // Flush batch if needed
                if (hint_batch.size() >= 512 * 1024) {
                    KIO_TRY(co_await worker_.AsyncWriteExact(hint_fd, std::span(hint_batch)));
                    hint_batch.clear();
                }
            }

            buffer.Advance(entry.Size());
        }

        if (buffer.ShouldCompact()) {
            buffer.Compact();
        }
    }

done_parsing:
    // Final flush
    if (!hint_batch.empty()) {
        KIO_TRY(co_await worker_.AsyncWriteExact(hint_fd, std::span(hint_batch)));
    }

    // Sync hint file
    KIO_TRY(co_await worker_.AsyncFsync(hint_fd));

    ALOG_INFO("Generated hint file for file_id={}, size={} bytes", file_id, hint_batch.size());
    co_return {};
}

// Update RotateActiveFile to generate hint
Task<Result<void>> Partition::RotateActiveFile() {
    assert(active_file_ && "Partition not initialized - active_file_ is null");

    const uint64_t sealed_file_id = active_file_->FileId();
    const uint64_t actual_size = active_file_->Size();
    const int active_fd = active_file_->Handle().Get();

    // Flush and truncate
    KIO_TRY(co_await worker_.AsyncFsync(active_fd));
    KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));
    KIO_TRY(co_await active_file_->AsyncClose());

    // Generate hint file for sealed file (background task or inline)
    if (config_.auto_generate_hints) {  // Add config option
        if (auto res = co_await GenerateHintFile(sealed_file_id); !res.has_value()) {
            // Log error but don't fail rotation
            ALOG_ERROR("Failed to generate hint file for {}: {}", sealed_file_id, res.error());
        }
    }

    // Create new active file
    KIO_TRY(co_await CreateAndSetActiveFile());
    stats_.file_rotations_total++;

    co_return {};
}
```

**Add to config:**
```cpp
// config.h
struct BitcaskConfig {
    // ... existing fields ...

    // Hint file generation
    bool auto_generate_hints = true;  // Generate hints on rotation
};
```

---

## 3. Hint File Checksums

### Problem
Hint files lack integrity checks. Corrupted hint files cause silent data corruption.

### Solution: Add CRC32 to Hint Entries

```cpp
// entry.h - Update HintEntry format
struct HintEntry {
    // Disk Layout (Little Endian):
    // [0-3]   CRC32 (of everything after)
    // [4-11]  Timestamp (ns)
    // [12-15] Offset
    // [16-19] Size
    // [20-23] Key Length
    // [24...] Key Bytes

    uint32_t offset{};
    uint32_t size{};
    uint64_t timestamp_ns{};
    std::string key;

    [[nodiscard]] size_t Size() const {
        return kHintHeaderSize + key.size();
    }

    HintEntry() = default;
    HintEntry(uint64_t timestamp_ns, uint64_t offset, uint32_t size, std::string&& key)
        : offset(offset), size(size), timestamp_ns(timestamp_ns), key(std::move(key)) {}

    [[nodiscard]] std::vector<char> Serialize() const;
    static kio::Result<std::pair<HintEntry, size_t>> Deserialize(std::span<const char> buffer);
};

// Update constant
constexpr std::size_t kHintHeaderSize = 24;  // Was 20, now includes CRC
```

**Implementation:**
```cpp
// entry.cpp
std::vector<char> HintEntry::Serialize() const {
    const auto kLen = static_cast<uint32_t>(key.size());
    std::vector<char> buffer(kHintHeaderSize + kLen);
    char* ptr = buffer.data();

    // Write fields (leave CRC blank)
    WriteLe(ptr, static_cast<uint32_t>(0));     // CRC placeholder
    WriteLe(ptr + 4, timestamp_ns);
    WriteLe(ptr + 8, offset);
    WriteLe(ptr + 12, size);
    WriteLe(ptr + 16, kLen);

    if (kLen > 0) {
        std::memcpy(ptr + kHintHeaderSize, key.data(), kLen);
    }

    // Compute CRC over everything after CRC field
    const uint32_t crc = crc32c::Crc32c(ptr + 4, buffer.size() - 4);
    WriteLe(ptr, crc);

    return buffer;
}

Result<std::pair<HintEntry, size_t>> HintEntry::Deserialize(std::span<const char> buffer) {
    if (buffer.size() < kHintHeaderSize) {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }

    const char* ptr = buffer.data();

    // Read and verify CRC
    const uint32_t stored_crc = ReadLe<uint32_t>(ptr);
    const uint32_t kLen = ReadLe<uint32_t>(ptr + 16);

    if (buffer.size() < kHintHeaderSize + kLen) {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }

    const uint32_t computed_crc = crc32c::Crc32c(ptr + 4, kHintHeaderSize - 4 + kLen);
    if (stored_crc != computed_crc) {
        ALOG_ERROR("Hint entry CRC mismatch: expected {}, got {}", stored_crc, computed_crc);
        return std::unexpected(Error{ErrorCategory::kSerialization, kIoDataCorrupted});
    }

    HintEntry entry;
    entry.timestamp_ns = ReadLe<uint64_t>(ptr + 4);
    entry.offset = ReadLe<uint32_t>(ptr + 8);
    entry.size = ReadLe<uint32_t>(ptr + 12);

    if (kLen > 0) {
        entry.key.assign(ptr + kHintHeaderSize, kLen);
    }

    return std::make_pair(entry, kHintHeaderSize + kLen);
}
```

---

## 4. Fix File Descriptor Bug in SealActiveFile

### Problem
```cpp
// partition.cpp:613 - WRONG!
KIO_TRY(co_await active_file_->AsyncClose());
KIO_TRY(co_await worker_.AsyncUnlinkAt(active_fd, path, config_.file_mode));
// active_fd is closed and invalid here!
```

### Solution

```cpp
// partition.cpp
Task<Result<void>> Partition::SealActiveFile() {
    co_await SwitchToWorker(worker_);

    if (active_file_ == nullptr) {
        co_return {};
    }

    const uint64_t actual_size = active_file_->Size();
    const int active_fd = active_file_->Handle().Get();
    const uint64_t actual_file_id = ActiveFileId();

    if (actual_size > 0) {
        // File has content - seal it
        KIO_TRY(co_await worker_.AsyncFsync(active_fd));
        KIO_TRY(co_await worker_.AsyncFtruncate(active_fd, static_cast<off_t>(actual_size)));
        KIO_TRY(co_await active_file_->AsyncClose());
    } else {
        // File is empty - remove it
        ALOG_DEBUG("Active file is empty, removing it, file_id={}", actual_file_id);

        // Close first
        KIO_TRY(co_await active_file_->AsyncClose());

        // Then unlink using AT_FDCWD (current working directory)
        const auto path = GetDataFilePath(actual_file_id);
        KIO_TRY(co_await worker_.AsyncUnlinkAt(AT_FDCWD, path, 0));  // FIX: Use AT_FDCWD, not closed fd
    }

    // Remove pointer to prevent double-closing
    active_file_.reset();

    co_return {};
}
```

---

## 5. Add Key/Value Size Validation

### Problem
Malicious or corrupted data could specify huge key/value sizes, causing OOM.

### Solution

```cpp
// config.h - Add limits
struct BitcaskConfig {
    // ... existing fields ...

    // Safety limits
    size_t max_key_size = 64 * 1024;      // 64KB max key
    size_t max_value_size = 512 * 1024 * 1024;  // 512MB max value
};
```

**Add validation in deserialization:**
```cpp
// entry.cpp
Result<DataEntry> DataEntry::Deserialize(std::span<const char> buffer) {
    if (buffer.size() < kEntryFixedHeaderSize) {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }

    const char* base = buffer.data();

    // Decode lengths
    const auto kLen = ReadLe<uint32_t>(base + 13);
    const auto kVLen = ReadLe<uint32_t>(base + 17);

    // VALIDATION: Check reasonable limits
    constexpr uint32_t MAX_KEY_SIZE = 64 * 1024;         // 64KB
    constexpr uint32_t MAX_VALUE_SIZE = 512 * 1024 * 1024;  // 512MB

    if (kLen > MAX_KEY_SIZE) {
        ALOG_ERROR("Key size {} exceeds maximum {}", kLen, MAX_KEY_SIZE);
        return std::unexpected(Error{ErrorCategory::kSerialization, kIoDataCorrupted});
    }

    if (kVLen > MAX_VALUE_SIZE) {
        ALOG_ERROR("Value size {} exceeds maximum {}", kVLen, MAX_VALUE_SIZE);
        return std::unexpected(Error{ErrorCategory::kSerialization, kIoDataCorrupted});
    }

    if (buffer.size() < kEntryFixedHeaderSize + kLen + kVLen) {
        return std::unexpected(Error(ErrorCategory::kSerialization, kIoNeedMoreData));
    }

    // Verify CRC
    auto stored_crc = ReadLe<uint32_t>(base);
    auto computed_crc = crc32c::Crc32c(base + 4, kEntryFixedHeaderSize - 4 + kLen + kVLen);

    if (computed_crc != stored_crc) {
        return std::unexpected(Error{ErrorCategory::kSerialization, kIoDataCorrupted});
    }

    // Continue with deserialization...
    auto timestamp = ReadLe<uint64_t>(base + 4);
    auto flag = ReadLe<uint8_t>(base + 12);

    std::string_view key(base + kEntryFixedHeaderSize, kLen);
    std::span value(base + kEntryFixedHeaderSize + kLen, kVLen);

    return DataEntry(key, value, flag, timestamp);
}
```

**Add runtime validation in Put:**
```cpp
// partition.cpp
Task<Result<void>> Partition::Put(std::string&& key, std::vector<char>&& value) {
    assert(active_file_ && "Partition not initialized");

    co_await SwitchToWorker(worker_);

    // Validate sizes before creating entry
    if (key.size() > config_.max_key_size) {
        co_return std::unexpected(Error{ErrorCategory::kApplication, kKeyTooLarge});
    }

    if (value.size() > config_.max_value_size) {
        co_return std::unexpected(Error{ErrorCategory::kApplication, kValueTooLarge});
    }

    // Continue with normal Put logic...
    if (active_file_->ShouldRotate(config_.max_file_size)) {
        KIO_TRY(co_await RotateActiveFile());
    }

    // ... rest of implementation
}
```

**Add error codes:**
```cpp
// common.h or errors.h
constexpr int kHashCollision = 2001;
constexpr int kKeyTooLarge = 2002;
constexpr int kValueTooLarge = 2003;
```

---

## 6. Make Stats Counters Thread-Safe

### Problem
Stats are updated without synchronization, causing data races if read from another thread.

### Solution A: Atomic Counters (Simple)

```cpp
// stats.h
struct PartitionStats {
    struct FileStats {
        std::atomic<uint64_t> total_bytes{0};
        std::atomic<uint64_t> live_bytes{0};
        std::atomic<uint64_t> live_entries{0};

        [[nodiscard]] double Fragmentation() const {
            uint64_t total = total_bytes.load(std::memory_order_relaxed);
            uint64_t live = live_bytes.load(std::memory_order_relaxed);
            return total > 0 ? 1.0 - (static_cast<double>(live) / static_cast<double>(total)) : 0.0;
        }

        [[nodiscard]] uint64_t ReclaimableBytes() const {
            return total_bytes.load(std::memory_order_relaxed) -
                   live_bytes.load(std::memory_order_relaxed);
        }
    };

    // Per-file stats (map itself needs synchronization)
    std::unordered_map<uint64_t, FileStats> data_files;  // Protected by partition's single-threaded access

    // Atomic counters for runtime metrics
    std::atomic<bool> compaction_running{false};
    std::atomic<uint64_t> puts_total{0};
    std::atomic<uint64_t> gets_total{0};
    std::atomic<uint64_t> gets_miss_total{0};
    std::atomic<uint64_t> deletes_total{0};
    std::atomic<uint64_t> compactions_total{0};
    std::atomic<uint64_t> compactions_failed{0};
    std::atomic<uint64_t> bytes_reclaimed_total{0};
    std::atomic<uint64_t> files_compacted_total{0};
    std::atomic<uint64_t> file_rotations_total{0};

    // ... rest of methods
};
```

**Update increment sites:**
```cpp
// partition.cpp
stats_.puts_total.fetch_add(1, std::memory_order_relaxed);
stats_.gets_total.fetch_add(1, std::memory_order_relaxed);
```

### Solution B: Document Single-Threaded Access (Preferred)

If stats are only accessed from the partition's worker thread, document this:

```cpp
// partition.h
/**
 * @brief Partition statistics
 *
 * @note Thread Safety: All stats are updated and read exclusively on the partition's
 *       worker thread. External access (e.g., metrics collection) must use
 *       SwitchToWorker or accept eventual consistency with relaxed atomics.
 */
struct PartitionStats {
    // Non-atomic counters - partition thread only
    uint64_t puts_total{0};
    uint64_t gets_total{0};
    // ...
};
```

For metrics collection:
```cpp
// bitcask.cpp - Add stats collection method
Task<PartitionStats> BitKV::GetPartitionStats(size_t partition_id) const {
    // Switch to partition's worker to safely read stats
    auto& partition = GetPartition(partition_id);
    co_await SwitchToWorker(partition.GetWorker());  // Add GetWorker() accessor
    co_return partition.GetStats();
}
```

---

## 7. Recovery-Safe File ID Generation

### Problem
Timestamp-based IDs could duplicate if clock resolution is coarse or system restarts quickly.

### Solution: Scan Existing Files on Recovery

```cpp
// file_id.h (create if missing)
class FileIdGenerator {
    std::atomic<uint64_t> next_id_;
    size_t partition_id_;

public:
    explicit FileIdGenerator(size_t partition_id)
        : next_id_(GenerateInitialId(partition_id)), partition_id_(partition_id) {}

    uint64_t Next() {
        return next_id_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecoverFromExisting(const std::vector<uint64_t>& existing_ids) {
        if (existing_ids.empty()) {
            return;
        }

        // Set next_id to max(existing) + 1
        uint64_t max_id = *std::ranges::max_element(existing_ids);
        uint64_t current = next_id_.load(std::memory_order_relaxed);

        // Only update if max_id is greater
        while (max_id >= current) {
            if (next_id_.compare_exchange_weak(current, max_id + 1,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
                ALOG_INFO("Partition {}: Recovered file_id generator, next_id={}",
                         partition_id_, max_id + 1);
                break;
            }
        }
    }

private:
    static uint64_t GenerateInitialId(size_t partition_id) {
        // Combine timestamp with partition_id to ensure uniqueness
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

        // Upper 48 bits: timestamp, lower 16 bits: partition_id
        return (static_cast<uint64_t>(timestamp) << 16) | (partition_id & 0xFFFF);
    }
};
```

**Update Partition::Recover:**
```cpp
// partition.cpp
Task<Result<void>> Partition::Recover() {
    const auto files = ScanDataFiles();

    // Recover file ID generator from existing files
    file_id_gen_.RecoverFromExisting(files);

    ALOG_INFO("Found {} data files", files.size());

    // ... rest of recovery logic
}
```

---

## 8. Add Disk Space Checks

### Problem
Writing to full disk causes partial writes and corruption.

### Solution: Pre-flight Disk Space Check

```cpp
// common.h - Add utility
inline Result<uint64_t> GetAvailableDiskSpace(const std::filesystem::path& path) {
    std::error_code ec;
    auto space_info = std::filesystem::space(path, ec);

    if (ec) {
        return std::unexpected(Error::FromErrno(ec.value()));
    }

    return space_info.available;
}
```

**Check before writes:**
```cpp
// partition.cpp - Put()
Task<Result<void>> Partition::Put(std::string&& key, std::vector<char>&& value) {
    assert(active_file_ && "Partition not initialized");
    co_await SwitchToWorker(worker_);

    // Validate sizes
    if (key.size() > config_.max_key_size || value.size() > config_.max_value_size) {
        co_return std::unexpected(Error{ErrorCategory::kApplication, kValueTooLarge});
    }

    // Check disk space before rotation
    if (active_file_->ShouldRotate(config_.max_file_size)) {
        auto available = GetAvailableDiskSpace(config_.directory);
        if (available.has_value() && available.value() < config_.max_file_size * 2) {
            // Need at least 2x max_file_size for rotation
            ALOG_ERROR("Insufficient disk space: {} bytes available", available.value());
            co_return std::unexpected(Error{ErrorCategory::kApplication, kDiskFull});
        }

        KIO_TRY(co_await RotateActiveFile());
    }

    // ... rest of implementation
}
```

**Add to compaction:**
```cpp
// compactor.cpp - CompactFiles()
Task<Result<void>> CompactFiles(CompactionContext& ctx) {
    ALOG_INFO("Starting N-to-1 compaction: {} files -> file {}",
              ctx.src_file_ids.size(), ctx.dst_file_id);

    // Estimate required space
    uint64_t total_live_bytes = 0;
    for (uint64_t src_id : ctx.src_file_ids) {
        if (auto it = ctx.stats.data_files.find(src_id); it != ctx.stats.data_files.end()) {
            total_live_bytes += it->second.live_bytes;
        }
    }

    // Check available space
    auto available = GetAvailableDiskSpace(ctx.config.directory);
    if (available.has_value() && available.value() < total_live_bytes * 2) {
        ALOG_ERROR("Insufficient space for compaction: need {}, have {}",
                   total_live_bytes * 2, available.value());
        co_return std::unexpected(Error{ErrorCategory::kApplication, kDiskFull});
    }

    // ... rest of compaction
}
```

---

## 9. Error Handling in Cleanup Operations

### Problem
File deletion errors are silently ignored.

### Solution: Log and Track Cleanup Failures

```cpp
// compactor.cpp
Task<Result<void>> CleanupSourceFiles(const CompactionContext& ctx, const uint64_t src_file_id) {
    bool any_failed = false;

    // Try to delete data file
    auto data_result = co_await ctx.io_worker.AsyncUnlinkAt(
        AT_FDCWD, GetDataFilePath(ctx, src_file_id), 0
    );
    if (!data_result.has_value()) {
        ALOG_ERROR("Failed to delete compacted data file {}: {}",
                   src_file_id, data_result.error());
        any_failed = true;
    }

    // Try to delete hint file (may not exist)
    auto hint_result = co_await ctx.io_worker.AsyncUnlinkAt(
        AT_FDCWD, GetHintFilePath(ctx, src_file_id), 0
    );
    if (!hint_result.has_value() && hint_result.error().value != ENOENT) {
        ALOG_ERROR("Failed to delete compacted hint file {}: {}",
                   src_file_id, hint_result.error());
        any_failed = true;
    }

    if (any_failed) {
        // Don't fail compaction, but track for monitoring
        ctx.stats.cleanup_failures_total++;
        ALOG_WARN("Compacted file {} not fully deleted - manual cleanup may be needed",
                  src_file_id);
    }

    co_return {};
}
```

**Add tracking:**
```cpp
// stats.h
struct PartitionStats {
    // ... existing fields ...

    // Cleanup tracking
    std::atomic<uint64_t> cleanup_failures_total{0};
};
```

---

## 10. Buffer Overflow Protection in Recovery

### Problem
BytesMut can grow unbounded if entries are larger than read_buffer_size.

### Solution: Add Size Limits

```cpp
// partition.cpp - RecoverFromDataFile
Task<Result<void>> Partition::RecoverFromDataFile(const FileHandle& fh, uint64_t file_id) {
    const int fd = fh.Get();
    auto file_size = KIO_TRY(GetFileSize(fd));

    if (file_size == 0) {
        ALOG_DEBUG("File {} is empty, skipping recovery", file_id);
        co_return {};
    }

    ALOG_INFO("Recovering from data file {}, size: {} bytes", file_id, file_size);

    BytesMut buffer(config_.read_buffer_size * 2);
    uint64_t file_offset = 0;
    uint64_t entries_recovered = 0;
    uint64_t entries_skipped = 0;

    // Add maximum buffer limit
    constexpr size_t MAX_BUFFER_SIZE = 10 * 1024 * 1024;  // 10MB

    while (file_offset < file_size) {
        // Check buffer size before reserving more
        if (buffer.Capacity() > MAX_BUFFER_SIZE) {
            ALOG_ERROR("Recovery buffer overflow for file {}: {} bytes",
                       file_id, buffer.Capacity());
            co_return std::unexpected(Error{ErrorCategory::kApplication, kIoDataCorrupted});
        }

        buffer.Reserve(config_.read_buffer_size);
        auto writable = buffer.WritableSpan();

        // ... rest of recovery logic
    }

    co_return {};
}
```

---

## Summary of Changes

### Files to Modify:
1. `bitcask/include/entry.h` - Add key to ValueLocation, update HintEntry format
2. `bitcask/include/config.h` - Add limits and hint generation flag
3. `bitcask/include/stats.h` - Make counters atomic or document threading
4. `bitcask/include/common.h` - Add error codes, disk space utility
5. `bitcask/src/entry.cpp` - Add CRC to hints, validate sizes
6. `bitcask/src/partition.cpp` - Fix bugs, add hint generation, validation
7. `bitcask/src/compactor.cpp` - Add disk space checks, better error handling
8. `bitcask/include/file_id.h` - Create with recovery-safe ID generation

### Testing Checklist:
- [ ] Test hash collision handling with synthetic collisions
- [ ] Verify hint files generated on rotation
- [ ] Test recovery with corrupted hint files
- [ ] Verify file deletion with `AT_FDCWD` fix
- [ ] Test with maliciously large key/value sizes
- [ ] Test disk full scenarios
- [ ] Verify stats thread-safety under concurrent access
- [ ] Test recovery after crash with unsealed files
- [ ] Benchmark overhead of hint generation on rotation

### Migration Notes:
- Existing hint files without CRC will fail to load (breaking change)
- Consider adding version marker to hint files for forward compatibility
- KeyDir with full keys increases memory ~30-50 bytes per entry
- Run compaction on existing DB to regenerate hint files with new format

---

## Additional Recommendations

### For Production Readiness:
1. **Add Write-Ahead Log (WAL)** for metadata durability
2. **Implement Bloom Filters** per file for faster negative lookups
3. **Add Rate Limiting** for compaction to prevent foreground starvation
4. **Implement Health Checks** API for monitoring partition health
5. **Add Metrics Endpoint** with Prometheus format support
6. **Implement Graceful Degradation** when partitions fail
7. **Add Snapshot/Backup** utilities for operational safety

### Performance Tuning:
1. Consider `O_DIRECT` for active file writes (bypass page cache)
2. Use io_uring `IOSQE_IO_LINK` for write+sync chains
3. Batch hint file writes during recovery
4. Add memory pool sizing based on workload
5. Implement adaptive compaction triggers based on write rate

### Code Quality:
1. Add extensive unit tests for each fix
2. Add integration tests for crash recovery scenarios
3. Document thread-safety invariants clearly
4. Add assertions for preconditions in critical paths
5. Consider adding sanitizer builds (ASAN, TSAN) to CI
