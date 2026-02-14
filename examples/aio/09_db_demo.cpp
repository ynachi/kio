#include <iostream>
#include <string>
#include <vector>
#include <span>

#include "bitcask/database.hpp"
#include "kio/kio.hpp"

using namespace bitcask;
using namespace kio;

// =============================================================================
// Helpers for the Demo
// =============================================================================

// Convert string to byte vector (BitKV expects bytes)
std::vector<std::byte> ToBytes(std::string_view s)
{
    auto span = std::as_bytes(std::span(s));
    return {span.begin(), span.end()};
}

// Convert byte vector back to string for printing
std::string ToString(const std::vector<std::byte>& bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// =============================================================================
// Main Demo Coroutine
// =============================================================================

Task<> RunDatabaseDemo()
{
    // 1. Configure the Database
    BitcaskConfig config;
    config.directory = "/tmp/bitcask_demo";
    config.max_file_size = 10 * 1024 * 1024; // 10 MB limit for rotation

    std::cout << "[Demo] 1. Opening Database at " << config.directory << "...\n";

    // 2. Open the DB (Async Factory)
    // We rely on the internal error propagation; if this fails, we catch it below.
    auto open_res = co_await BitKV::Open(config);
    if (!open_res)
    {
        std::cerr << "[Fatal] Failed to open DB: " << open_res.error().message() << "\n";
        co_return;
    }

    // Move the unique_ptr out of the Result
    auto db = std::move(*open_res);
    std::cout << "[Demo]    -> Database Opened Successfully!\n";

    // 3. PUT Operation
    std::string key = "user:1001";
    std::string value = "{ name: 'Alice', role: 'Engineer' }";

    std::cout << "[Demo] 2. Writing Key: '" << key << "'\n";
    auto put_res = co_await db->Put(key, ToBytes(value));

    if (!put_res)
    {
        std::cerr << "[Error] Put failed: " << put_res.error().message() << "\n";
    }
    else
    {
        std::cout << "[Demo]    -> Write Success.\n";
    }

    // 4. GET Operation
    std::cout << "[Demo] 3. Reading Key: '" << key << "'\n";
    auto get_res = co_await db->Get(key);

    if (get_res && get_res->has_value())
    {
        std::string found_val = ToString(**get_res);
        std::cout << "[Demo]    -> Found Value: " << found_val << "\n";

        if (found_val == value)
        {
            std::cout << "[Demo]    -> Integrity Check: PASSED\n";
        }
        else
        {
            std::cerr << "[Demo]    -> Integrity Check: FAILED (Mismatch)\n";
        }
    }
    else
    {
        std::cerr << "[Demo]    -> Key not found or error!\n";
    }

    // 5. DEL Operation
    std::cout << "[Demo] 4. Deleting Key: '" << key << "'\n";
    co_await db->Del(key);

    // 6. Verify Deletion
    auto verify_res = co_await db->Get(key);
    if (verify_res && !verify_res->has_value())
    {
        std::cout << "[Demo]    -> Verification: Key is gone (Correct).\n";
    }
    else
    {
        std::cerr << "[Demo]    -> Verification: FAILED, key still exists.\n";
    }

    // 7. Graceful Shutdown
    std::cout << "[Demo] 5. Closing Database...\n";
    co_await db->Close();
    std::cout << "[Demo]    -> Closed. Bye!\n";
}

// =============================================================================
// Entry Point
// =============================================================================

int main()
{
    // Create the IO Runtime
    // (This is the main thread's context, usually handling signals or the root task)
    IoContext ctx;

    std::cout << "=== BitKV Async Demo ===\n";

    try
    {
        // Run the coroutine until it completes
        ctx.RunUntilDone(RunDatabaseDemo());
    }
    catch (const std::exception& e)
    {
        std::cerr << "Unhandled Exception: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
