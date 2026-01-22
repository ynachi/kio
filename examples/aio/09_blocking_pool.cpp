// examples/blocking_pool_examples.cpp
// Demonstrates improved ergonomics with std::move_only_function

#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <netdb.h>

#include "aio/blocking_pool.hpp"
#include "aio/task_group.hpp"
#include <arpa/inet.h>

// =============================================================================
// Example 1: Capturing Move-Only Types
// =============================================================================

// OLD: Had to use raw pointers and manual cleanup
aio::task<std::vector<std::string>> old_parallel_parse(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    std::vector<std::string> files) {

    // Awkward: had to pass raw pointer or shared_ptr
    auto shared_files = std::make_shared<std::vector<std::string>>(std::move(files));

    auto result = co_await aio::offload(ctx, pool, [shared_files]() {
        std::vector<std::string> parsed;
        for (const auto& file : *shared_files) {
            parsed.push_back("parsed: " + file);
        }
        return parsed;
    });

    co_return result;
}

// NEW: Can capture move-only types directly!
aio::task<std::vector<std::string>> new_parallel_parse(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    std::vector<std::string> files) {

    // Clean: move the vector directly into the lambda
    auto result = co_await aio::offload(ctx, pool, [files = std::move(files)]() {
        std::vector<std::string> parsed;
        for (const auto& file : files) {
            parsed.push_back("parsed: " + file);
        }
        return parsed;
    });

    co_return result;
}

// =============================================================================
// Example 2: Capturing unique_ptr
// =============================================================================

struct LargeObject {
    std::vector<char> data;
    explicit LargeObject(size_t size) : data(size) {}

    void process() {
        // Heavy computation...
        for (auto& byte : data) {
            byte ^= 0xFF;
        }
    }
};


aio::task<> new_process_large_object(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    std::unique_ptr<LargeObject> obj) {

    // Clean: move unique_ptr, no shared ownership needed
    co_await aio::offload(ctx, pool, [obj = std::move(obj)]() {
        obj->process();
    });
}

// =============================================================================
// Example 3: Complex Captures
// =============================================================================

struct FileMetadata {
    std::string path;
    std::unique_ptr<std::vector<char>> buffer;
    size_t offset;
};

aio::task<> process_file_new(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    FileMetadata metadata) {

    co_await aio::offload(ctx, pool, [meta = std::move(metadata)]() mutable {
        // Everything is safely captured by value/move
        std::cout << "Processing " << meta.path << "\n";
        // Work with meta.buffer...
    });
}

// =============================================================================
// Example 4: task_group + blocking_pool Integration
// =============================================================================

aio::task<void> process_one_file(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    std::string path) {

    // Offload I/O to blocking pool
    auto data = co_await aio::offload(ctx, pool, [path = std::move(path)]() {
        // Blocking read (can't use io_uring for metadata ops)
        std::vector<char> buffer(4096);
        FILE* f = fopen(path.c_str(), "rb");
        if (f) {
            fread(buffer.data(), 1, buffer.size(), f);
            fclose(f);
        }
        return buffer;
    });

    // Process data on io thread
    std::cout << "Read " << data.size() << " bytes\n";
}

aio::task<void> parallel_file_processing(
    aio::io_context& ctx,
    aio::blocking_pool& pool) {

    std::vector<std::string> files = {
        "file1.txt", "file2.txt", "file3.txt", /* ... */
    };

    // Spawn all file processing tasks
    aio::task_group<void> workers;
    for (auto& file : files) {
        workers.spawn(process_one_file(ctx, pool, std::move(file)));
    }

    // Wait for completion
    co_await workers.join_all(ctx);
    std::cout << "Processed " << workers.total_spawned() << " files\n";
}

// =============================================================================
// Example 5: Real-World DNS Resolution
// =============================================================================

struct DNSResult {
    std::string hostname;
    std::string ip;
    std::chrono::milliseconds duration;
};

aio::task<DNSResult> resolve_one(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    std::string hostname) {

    auto start = std::chrono::steady_clock::now();

    // Offload blocking DNS to thread pool
    // OLD: Had to copy hostname or use shared_ptr
    // NEW: Move it directly!
    auto ip = co_await aio::offload(ctx, pool, [host = std::move(hostname)]() {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host.c_str(), "80", &hints, &res) != 0 || !res) {
            return std::string{"FAILED"};
        }

        char buf[INET6_ADDRSTRLEN] = {};
        if (res->ai_family == AF_INET) {
            inet_ntop(AF_INET,
                &reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr,
                buf, sizeof(buf));
        } else if (res->ai_family == AF_INET6) {
            inet_ntop(AF_INET6,
                &reinterpret_cast<sockaddr_in6*>(res->ai_addr)->sin6_addr,
                buf, sizeof(buf));
        }

        freeaddrinfo(res);
        return std::string{buf};
    });

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    co_return DNSResult{hostname, ip, elapsed};
}

aio::task<std::vector<DNSResult>> parallel_dns_resolve(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    std::vector<std::string> domains) {

    // Spawn all DNS lookups concurrently
    aio::task_group<DNSResult> lookups;
    for (auto& domain : domains) {
        lookups.spawn(resolve_one(ctx, pool, std::move(domain)));
    }

    // Wait with timeout
    bool completed = co_await lookups.join_all_timeout(ctx,
        std::chrono::seconds(5));

    if (!completed) {
        std::cerr << "Some DNS lookups timed out!\n";
    }

    // Collect results
    std::vector<DNSResult> results;
    results.reserve(lookups.size());

    for (auto& task : lookups.tasks()) {
        if (task.done()) {
            results.push_back(task.result());
        }
    }

    co_return results;
}

// =============================================================================
// Example 6: Progressive Offloading Pattern
// =============================================================================

struct ProcessingStage {
    std::string name;
    std::vector<char> data;
};

// Chain of processing stages, some on pool, some on io thread
aio::task<> multi_stage_processing(
    aio::io_context& ctx,
    aio::blocking_pool& pool,
    std::string input_file) {

    // Stage 1: Blocking read
    auto stage1 = co_await aio::offload(ctx, pool,
        [path = std::move(input_file)]() -> ProcessingStage {
            std::vector<char> data(1024 * 1024);  // 1MB
            // ... read file ...
            return {"read", std::move(data)};
        });

    std::cout << "Stage 1 complete: " << stage1.name << "\n";

    // Stage 2: CPU-intensive processing
    auto stage2 = co_await aio::offload(ctx, pool,
        [s = std::move(stage1)]() mutable -> ProcessingStage {
            // Heavy processing...
            for (auto& byte : s.data) {
                byte = (byte * 31) ^ 0xAA;
            }
            s.name = "processed";
            return std::move(s);
        });

    std::cout << "Stage 2 complete: " << stage2.name << "\n";

    // Stage 3: Async write back to disk
     co_await aio::offload(ctx, pool,
        [s = std::move(stage2)]() {
            FILE* f = fopen("output.dat", "wb");
            if (f) {
                fwrite(s.data.data(), 1, s.data.size(), f);
                fclose(f);
            }
        });

    std::cout << "Pipeline complete!\n";
}

// =============================================================================
// Main Example
// =============================================================================

int main() {
    aio::io_context ctx;
    aio::blocking_pool pool{4};  // 4 worker threads

    std::vector<std::string> domains = {
        "google.com", "github.com", "stackoverflow.com"
    };

    auto task = parallel_dns_resolve(ctx, pool, std::move(domains));

    ctx.run_until_done(task);

    auto results = task.result();
    for (const auto& r : results) {
        std::cout << r.hostname << " -> " << r.ip
                  << " (" << r.duration.count() << "ms)\n";
    }

    return 0;
}

// =============================================================================
// BENEFITS SUMMARY
// =============================================================================
//
// std::move_only_function enables:
// ✓ Capture move-only types (unique_ptr, vector, etc)
// ✓ No forced shared_ptr (avoid atomic refcount overhead)
// ✓ Natural lambda syntax (no manual struct + void*)
// ✓ Type safety (compiler catches errors)
// ✓ Exception safety (RAII types work correctly)
//
// Performance:
// ✓ Zero overhead vs function pointer (inlines same way)
// ✓ Eliminates shared_ptr atomic ops when not needed
// ✓ Better optimization (compiler sees through lambdas)
//
// Integration with task_group:
// ✓ Clean fan-out/fan-in patterns
// ✓ Automatic lifetime management
// ✓ Natural error propagation via exceptions
//
// =============================================================================