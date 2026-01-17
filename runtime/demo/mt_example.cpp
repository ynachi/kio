////////////////////////////////////////////////////////////////////////////////
// example.cpp - Mutithread usage examples for the runtime
//
// Build: g++ -std=c++23 -o example example.cpp -luring
////////////////////////////////////////////////////////////////////////////////

#include <cstring>
#include <span>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/socket.h>

#include "runtime/runtime.hpp"
#include "utilities/kio_logger.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>

using namespace uring;

////////////////////////////////////////////////////////////////////////////////
// Example 1: Simple Storage Engine Pattern
////////////////////////////////////////////////////////////////////////////////

// Simulates writing a WAL entry then syncing
static Task<> write_wal_entry(Executor& ex, int fd, uint64_t offset, std::span<const char> buf)
{
    auto res = co_await write(ex, fd, buf, offset);
    if (!res)
    {
        Log::error("write failed: {}", res.error().message());
        co_return;
    }
    Log::info("Wrote {} bytes at offset {}", *res, offset);

    // Datasync is usually sufficient for WAL (metadata doesn't matter)
    auto sync_res = co_await fsync(ex, fd, /*datasync=*/true);
    if (!sync_res)
    {
        Log::error("fsync failed: {}", sync_res.error().message());
        co_return;
    }
    Log::info("Synced successfully");
}

// Batched read pattern (common in storage engines)
static Task<> read_blocks(Executor& ex, int fd, std::vector<std::pair<uint64_t, std::span<char>>>& blocks)
{
    for (auto& [offset, buffer] : blocks)
    {
        auto res = co_await read(ex, fd, buffer, offset);
        if (!res)
        {
            Log::error("read at {} failed: {}", offset, res.error().message());
            continue;
        }
        Log::info("Read {} bytes from offset {}", *res, offset);
    }
}

static void storage_example()
{
    Log::info("=== Storage Example ===");

    // Create a test file
    int fd = open("/tmp/uring_test.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        Log::error("open failed: {}", std::strerror(errno));
        return;
    }

    Executor ex;

    // Write some data
    std::string text("Hello, io_uring storage!");
    std::span data(text);
    ex.spawn(write_wal_entry(ex, fd, 0, data));
    ex.loop();

    // Read it back with a block pattern
    auto vec1 = std::vector<char>(8);
    auto vec2 = std::vector<char>(8);
    auto span1 = std::span(vec1.data(), vec1.size());
    auto span2 = std::span(vec2.data(), vec2.size());
    std::vector<std::pair<uint64_t, std::span<char>>> blocks = {
        {0, span1},
        {8, span2},
    };
    ex.spawn(read_blocks(ex, fd, blocks));
    ex.loop();

    ::close(fd);
    unlink("/tmp/uring_test.dat");
}

////////////////////////////////////////////////////////////////////////////////
// Example 2: Echo Proxy Pattern
////////////////////////////////////////////////////////////////////////////////

static Task<> handle_connection(Executor& ex, int client_fd)
{
    char buf[4096];

    while (true)
    {
        auto recv_res = co_await recv(ex, client_fd, buf, sizeof(buf));
        if (!recv_res || *recv_res == 0)
        {
            if (!recv_res)
                Log::error("recv failed: {}", recv_res.error().message());
            // Error or connection closed
            break;
        }

        int n = *recv_res;
        Log::info("Received {} bytes", n);

        // Echo back
        auto send_res = co_await send(ex, client_fd, buf, n);
        if (!send_res)
        {
            Log::error("send failed: {}", send_res.error().message());
            break;
        }
    }

    co_await close(ex, client_fd);
    Log::info("Connection closed");
}

static Task<> accept_loop(Executor& ex, int listen_fd, int max_connections)
{
    for (int i = 0; i < max_connections; ++i)
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        auto res = co_await accept(ex, listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (!res)
        {
            Log::error("accept failed: {}", res.error().message());
            continue;
        }

        int client_fd = *res;
        Log::info("Accepted connection from {}:{} (fd={})", inet_ntoa(client_addr.sin_addr),
                  ntohs(client_addr.sin_port), client_fd);

        // Spawn handler - runs concurrently
        ex.spawn(handle_connection(ex, client_fd));
    }
}

static void proxy_example()
{
    Log::info("=== Proxy Example (accepts 2 connections then exits) ===");
    Log::info("Connect with: nc localhost 8080");

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        Log::error("socket failed: {}", std::strerror(errno));
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(8080);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        Log::error("bind failed: {}", std::strerror(errno));
        ::close(listen_fd);
        return;
    }

    if (listen(listen_fd, 128) < 0)
    {
        Log::error("listen failed: {}", std::strerror(errno));
        ::close(listen_fd);
        return;
    }

    Log::info("Listening on port 8080...");

    Executor ex;
    ex.spawn(accept_loop(ex, listen_fd, 2));
    ex.loop();

    ::close(listen_fd);
}

////////////////////////////////////////////////////////////////////////////////
// Example 3: Timeout Pattern
////////////////////////////////////////////////////////////////////////////////

static Task<> timeout_example_task(Executor& ex)
{
    Log::info("Waiting 500ms...");
    co_await timeout_ms(ex, 500);
    Log::info("Done waiting!");
}

static void timeout_example()
{
    Log::info("=== Timeout Example ===");

    Executor ex;
    ex.spawn(timeout_example_task(ex));
    ex.loop();
}

////////////////////////////////////////////////////////////////////////////////
// Example 4: Concurrent Operations
////////////////////////////////////////////////////////////////////////////////

static Task<int> delayed_value(Executor& ex, int value, uint64_t delay_ms)
{
    co_await timeout_ms(ex, delay_ms);
    Log::info("Returning value {} after {}ms", value, delay_ms);
    co_return value;
}

static Task<> concurrent_example_task(Executor& ex)
{
    // These run concurrently - we just can't await them simultaneously
    // without additional machinery (like WhenAll)
    // For now, sequential but non-blocking:

    Log::info("Starting concurrent tasks...");

    int a = co_await delayed_value(ex, 1, 300);
    int b = co_await delayed_value(ex, 2, 200);
    int c = co_await delayed_value(ex, 3, 100);

    Log::info("Results: {}, {}, {} (sum={})", a, b, c, a + b + c);
}

static void concurrent_example()
{
    Log::info("=== Concurrent Example ===");

    Executor ex;
    ex.spawn(concurrent_example_task(ex));
    ex.loop();
}

////////////////////////////////////////////////////////////////////////////////
// Main
////////////////////////////////////////////////////////////////////////////////

int main(int argc, char** argv)
{
    Log::info("uring_exec examples");
    Log::info("===================");
    if (argc > 1 && std::strcmp(argv[1], "proxy") == 0)
    {
        proxy_example();
    }
    else
    {
        storage_example();
        timeout_example();
        concurrent_example();
    }

    return 0;
}