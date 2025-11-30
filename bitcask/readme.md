# BitKV - High-Performance Embedded Key-Value Store

A modern, async-native implementation of the Bitcask storage engine for C++20.

## What is BitKV?

BitKV is an embedded key-value database optimized for:

- **High write throughput** - Append-only log structure
- **Fast reads** - In-memory hash table (KeyDir)
- **Predictable latency** - No compaction pauses during writes
- **Crash recovery** - All writes are logged to disk
- **Simple API** - Put, Get, Delete operations with async/await

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                        BitKV                            │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌─────────┐│
│  │Partition0│  │Partition1│  │Partition2│  │Partition3││
│  │ Worker 0 │  │ Worker 1 │  │ Worker 2 │  │ Worker 3 ││
│  │  KeyDir  │  │  KeyDir  │  │  KeyDir  │  │  KeyDir  ││
│  │DataFiles │  │DataFiles │  │DataFiles │  │DataFiles ││
│  └──────────┘  └──────────┘  └──────────┘  └─────────┘│
└─────────────────────────────────────────────────────────┘
```

**Key Properties:**

- **Share-nothing architecture** - Each partition is independent
- **One partition per CPU core** - Scales linearly with cores
- **Consistent hashing** - Same key always routes to the same partition
- **Async I/O** - Built on io_uring (Linux) for maximum performance

## Quick Start

### Installation

```cmake
# CMakeLists.txt
add_subdirectory(bitcask)
target_link_libraries(your_app PRIVATE kio_bitcask kio_lib)
```

### Basic Usage

```cpp
#include <bitcask/include/bitcask.h>
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;

Task<void> example() {
    // Configure database
    BitcaskConfig config{
        .directory = "/var/lib/myapp/data",
        .max_file_size = 100 * 1024 * 1024,  // 100MB
        .sync_on_write = false,              // Async writes (faster)
        .auto_compact = true                 // Background compaction
    };
    
    io::WorkerConfig io_config{
        .uring_queue_depth = 2048
    };
    
    // Open database with N partitions (typically = CPU cores)
    auto db = co_await BitKV::open(
        config, 
        io_config, 
        std::thread::hardware_concurrency()
    );
    
    if (!db.has_value()) {
        std::cerr << "Failed to open: " << db.error() << "\n";
        co_return;
    }
    
    // Put data (string → string)
    co_await db.value()->put("user:1001", "Alice");
    co_await db.value()->put("user:1002", "Bob");
    
    // Get data
    auto result = co_await db.value()->get_string("user:1001");
    if (result.has_value() && result.value().has_value()) {
        std::cout << "Found: " << result.value().value() << "\n";
    }
    
    // Update (overwrites previous value)
    co_await db.value()->put("user:1001", "Alice Smith");
    
    // Delete
    co_await db.value()->del("user:1002");
    
    // Binary data (vector<char>)
    std::vector<char> binary_data = {0x01, 0x02, 0xFF, 0xFE};
    co_await db.value()->put("image:logo", std::move(binary_data));
    
    // Close cleanly (important!)
    co_await db.value()->close();
}

int main() {
    auto result = SyncWait(example());
    return 0;
}
```

### Important: Close Pattern

**⚠️ BitKV must be closed from the main thread, not from within coroutines:**

```cpp
// ✅ CORRECT
Task<Result<std::unique_ptr<BitKV>>> my_task() {
    auto db = co_await BitKV::open(...);
    // ... use db ...
    co_return std::move(db);  // Return, don't close
}

int main() {
    auto db = SyncWait(my_task());
    SyncWait(db.value()->close());  // Close from main thread
}

// ❌ WRONG - Will deadlock!
Task<void> bad_task() {
    auto db = co_await BitKV::open(...);
    co_await db->close();  // ☠️ Deadlock!
}
```

**Why?** Workers cannot stop themselves. The close operation must be initiated from outside the worker pool.

## Performance Characteristics

### Write Performance

**Factors:**

- Scales linearly with partition count (up to core count)
- `sync_on_write=false` provides significantly better throughput
- Small keys/values perform better (less I/O)
- Performance depends on hardware (CPU, disk type, RAM)

### Read Performance

**Factors:**

- Hot reads (keys in KeyDir) are O(1) hash lookups
- Cold reads require disk I/O via io_uring
- SSD strongly recommended for production
- LRU file descriptor cache reduces open/close overhead

### Memory Usage

```
Memory = (N_keys × (key_size + 24 bytes)) per partition

Examples:
- 1M keys, 20 byte avg key:   ~44 MB per partition
- 10M keys, 50 byte avg key:  ~740 MB per partition
```

**KeyDir is in-memory** - All keys must fit in RAM.

> **Note:** Detailed performance benchmarks will be added after comprehensive testing on production hardware.

## Scaling Guidelines

### Choosing Partition Count

```cpp
// Rule of thumb: 1 partition per CPU core
size_t partitions = std::thread::hardware_concurrency();

// Scale with your hardware:
// 4-core laptop:  4 partitions
// 8-core server:  8 partitions
// 64-core server: 64 partitions
```

**Trade-offs:**

- More partitions = better write throughput (scales linearly)
- More partitions = more memory (KeyDir per partition)
- More partitions = more files (file descriptor limits)

## Configuration

### Essential Settings

```cpp
BitcaskConfig config{
    .directory = "/path/to/data",
    
    // File rotation threshold (default: 100MB)
    .max_file_size = 100 * 1024 * 1024,
    
    // Sync every write? (default: false)
    // true  = durable but slow (~10× slower)
    // false = fast but data loss on crash
    .sync_on_write = false,
    
    // Background compaction? (default: true)
    .auto_compact = true,
    
    // Trigger compaction when file is >50% dead data
    .fragmentation_threshold = 0.5,
};
```

### Durability vs Performance

```cpp
// Maximum durability (slower)
config.sync_on_write = true;
// Every write is fsync'd to disk
// Lower throughput, guaranteed durability

// Balanced (recommended)
config.sync_on_write = false;
// Writes are buffered, periodically synced
// Higher throughput, small data loss window on crash

// Maximum performance (risky)
config.sync_on_write = false;
config.auto_compact = false;  // Manual compaction only
// Highest throughput
// Risk: disk fills up without compaction
```

## Advanced Features

### Manual Compaction

```cpp
// Force compaction on all partitions
co_await db->compact();

// Useful for:
// - After bulk deletes
// - During off-peak hours
// - When disk space is low
```

### Recovery

BitKV automatically recovers on startup:

1. **Scans data files** in each partition
2. **Rebuilds KeyDir** from hint files (fast) or data files (slower)
3. **Validates CRC checksums** on all entries
4. **Skips corrupted entries** (logs warnings)

Recovery time depends on:

- Number of keys
- Presence of hint files (significantly faster)
- Disk I/O performance
- CPU speed

### Monitoring

```cpp
// Get partition statistics
auto& stats = partition.get_stats();

std::cout << "Puts: " << stats.puts_total << "\n";
std::cout << "Gets: " << stats.gets_total << "\n";
std::cout << "Misses: " << stats.gets_miss_total << "\n";
std::cout << "Deletes: " << stats.deletes_total << "\n";
std::cout << "Compactions: " << stats.compactions_total << "\n";
std::cout << "Fragmentation: " << stats.overall_fragmentation() << "\n";
```

## Error Handling

All operations return `Result<T>` types:

```cpp
auto result = co_await db->get("key");

if (!result.has_value()) {
    // Handle error
    std::cerr << "Error: " << result.error() << "\n";
    co_return;
}

if (!result.value().has_value()) {
    // Key not found (not an error)
    std::cout << "Key not found\n";
} else {
    // Success
    auto value = result.value().value();
}
```

## File Layout

```
/var/lib/myapp/data/
├── MANIFEST                    # Database metadata
├── partition_0/
│   ├── data_1234567890.db     # Active data file
│   ├── data_1234567800.db     # Sealed data file
│   ├── hint_1234567800.ht     # Hint file (for recovery)
│   └── ...
├── partition_1/
│   └── ...
└── partition_N/
    └── ...
```

**Data Files:** Append-only logs of key-value entries  
**Hint Files:** Lightweight summaries for fast recovery  
**MANIFEST:** Stores partition count (validates configuration)

## Troubleshooting

### "Worker shutdown timeout"

**Cause:** Didn't call `close()` properly  
**Fix:** Always `co_await db->close()` from main thread

### "File descriptor exhaustion"

**Cause:** Too many partitions or data files  
**Fix:** Increase `ulimit -n` or reduce partition count

### "Partition count mismatch"

**Cause:** Opened database with different partition count  
**Fix:** Always use same partition count as original creation

### Slow reads

**Cause:** Reading from many different files (cold reads)  
**Fix:** Run compaction to merge files, or increase file cache size

### High memory usage

**Cause:** Large KeyDir (many keys)  
**Fix:** Reduce key count or increase RAM

## Best Practices

1. **Use async/await** - BitKV is designed for coroutines
2. **Close cleanly** - Always call `close()` before exit
3. **Partition by cores** - 1 partition per CPU core
4. **Monitor fragmentation** - Run compaction when > 50%
5. **Plan for recovery** - 1-2 seconds per 1M keys
6. **Limit key size** - Shorter keys = less memory
7. **Use binary values** - More efficient than strings

**BitKV fits the following use cases well:**

- You need high write throughput
- You have RAM for KeyDir
- You don't need range scans
- You want simple embedded storage

## License

MIT

---

**Version:** active development  
**Requires:** C++23, Linux, io_uring  
**Dependencies:** crc32c, struct_pack