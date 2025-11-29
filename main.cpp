#include <bitcask/include/bitcask.h>
#include <iostream>

#include "core/include/async_logger.h"
#include "core/include/coro.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;

// Task<Result<void>> simple_example()
// {
//     // Configure database
//     BitcaskConfig config{
//             .directory = "./data/simple_db",
//             .max_file_size = 10 * 1024 * 1024,  // 10MB per file
//             .fragmentation_threshold = 0.5,  // Compact at 50% fragmentation
//             .compaction_interval_s = std::chrono::seconds(300),  // Every 5 minutes
//             .read_buffer_size = 4096,
//             .write_buffer_size = 4096,
//             .max_open_sealed_files = 100,
//     };
//
//     io::WorkerConfig io_config{.uring_queue_depth = 1024,
//                                .uring_submit_batch_size = 128,
//                                .tcp_backlog = 128,
//                                .uring_submit_timeout_ms = 100,
//                                .default_op_slots = 1024,
//                                .op_slots_growth_factor = 1.5f,
//                                .max_op_slots = 1024 * 1024};
//
//     // Open database with 4 partitions (one per core)
//     auto db = KIO_TRY(co_await BitKV::open(config, io_config, 4));
//
//     ALOG_INFO("Database opened successfully");
//
//     // Put some data
//     KIO_TRY(co_await db->put("user:123", "Alice"));
//     KIO_TRY(co_await db->put("user:456", "Bob"));
//     KIO_TRY(co_await db->put("user:789", "Charlie"));
//
//     ALOG_INFO("Wrote 3 users");
//
//     // Get data
//     if (auto alice = KIO_TRY(co_await db->get("user:123")); alice.has_value())
//     {
//         std::string name(alice->begin(), alice->end());
//         ALOG_INFO("Found user:123 = {}", name);
//     }
//
//     // Update data
//     KIO_TRY(co_await db->put("user:123", "Alice Smith"));
//     ALOG_INFO("Updated user:123");
//
//     // Delete data
//     KIO_TRY(co_await db->del("user:789"));
//     ALOG_INFO("Deleted user:789");
//
//     // Verify deletion
//     if (auto charlie = KIO_TRY(co_await db->get("user:789")); !charlie.has_value())
//     {
//         ALOG_INFO("user:789 successfully deleted");
//     }
//
//     // Database will close automatically when db goes out of scope
//     ALOG_INFO("Done!");
//
//     co_return {};
// }

int main()
{
    // Setup logging
    alog::configure(4096, LogLevel::Info);

    ALOG_INFO("Starting BitKV simple example");

    io::WorkerConfig io_config{};
    io_config.uring_queue_depth = 2048;
    io_config.default_op_slots = 4096;

    const BitcaskConfig db_config{
            .directory = "/home/ynachi/data", .max_file_size = 50 * 1024 * 1024, .auto_compact = false
            // Defaults for everything else
    };

    auto db = SyncWait(BitKV::open(db_config, io_config, 1));
    if (!db.has_value())
    {
        ALOG_ERROR("Error opening database: {}", db.error());
        std::exit(EXIT_FAILURE);
    }

    std::cout << "press a key to exit";
    std::cin.get();
    SyncWait(db.value()->close());

    ALOG_INFO("Example complete");
    return 0;
}
