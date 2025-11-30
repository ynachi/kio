#include <bitcask/include/bitcask.h>
#include <iostream>

#include "core/include/async_logger.h"
#include "core/include/coro.h"
#include "core/include/sync_wait.h"

using namespace bitcask;
using namespace kio;

// Helper to convert string to vector<char>
std::vector<char> to_vec(const std::string& s) { return std::vector(s.begin(), s.end()); }

// Helper to convert vector<char> to string
std::string to_string(const std::vector<char>& v) { return std::string(v.begin(), v.end()); }

Task<Result<std::unique_ptr<BitKV>>> simple_example()
{
    io::WorkerConfig io_config{};
    io_config.uring_queue_depth = 2048;
    io_config.default_op_slots = 4096;

    const BitcaskConfig db_config{
            .directory = "/home/ynachi/data", .max_file_size = 50 * 1024 * 1024, .auto_compact = false
            // Defaults for everything else
    };

    auto db_res = co_await BitKV::open(db_config, io_config, 1);
    if (!db_res.has_value())
    {
        ALOG_ERROR("Error opening database: {}", db_res.error());
        co_return std::unexpected(db_res.error());
    }

    auto db = std::move(db_res.value());

    ALOG_INFO("Database opened successfully");

    // Put some data (value is vector<char>)
    KIO_TRY(co_await db->put("user:123", to_vec("Alice")));
    KIO_TRY(co_await db->put("user:456", to_vec("Bob")));
    KIO_TRY(co_await db->put("user:789", to_vec("Charlie")));

    ALOG_INFO("Wrote 3 users");

    // Get data (returns optional<vector<char>>)
    auto alice_result = KIO_TRY(co_await db->get("user:123"));
    if (alice_result.has_value())
    {
        std::string name = to_string(alice_result.value());
        ALOG_INFO("Found user:123 = {}", name);
    }

    // Update data
    KIO_TRY(co_await db->put("user:123", to_vec("Alice Smith")));
    ALOG_INFO("Updated user:123");

    // Delete data
    KIO_TRY(co_await db->del("user:789"));
    ALOG_INFO("Deleted user:789");

    // Verify deletion
    auto charlie_result = KIO_TRY(co_await db->get("user:789"));
    if (!charlie_result.has_value())
    {
        ALOG_INFO("user:789 successfully deleted");
    }

    // Store binary data (not just strings)
    std::vector<char> binary_data = {0x01, 0x02, 0x03, 0x04, 0xFF, 0xFE};
    KIO_TRY(co_await db->put("binary:key", std::move(binary_data)));
    ALOG_INFO("Stored binary data");

    // Retrieve binary data
    auto binary_result = KIO_TRY(co_await db->get("binary:key"));
    if (binary_result.has_value())
    {
        ALOG_INFO("Retrieved binary data, size: {}", binary_result->size());
        ALOG_INFO("First byte: 0x{:02x}", static_cast<unsigned char>((*binary_result)[0]));
    }

    // Database will close automatically when db goes out of scope
    ALOG_INFO("Done!");

    co_return std::move(db);
}

int main()
{
    // Setup logging
    alog::configure(4096, LogLevel::Debug);

    ALOG_INFO("Starting BitKV simple example");

    auto db = SyncWait(simple_example());
    if (!db.has_value())
    {
        ALOG_ERROR("Example failed: {}", db.error());
        return 1;
    }

    SyncWait(db.value()->close());

    ALOG_INFO("Example complete");
    return 0;
}
