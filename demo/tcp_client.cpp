//
// Created by Yao ACHI on 08/12/2025.
//
// Simple TCP client to test kio framework without TLS

#include <format>
#include <iostream>
#include <kio/sync/sync_wait.h>

#include "kio/core/async_logger.h"
#include "kio/core/worker.h"
#include "kio/net/net.h"

using namespace kio;
using namespace kio::io;

Task<Result<int>> connect_to_server(Worker& worker, std::string_view host, uint16_t port)
{
    co_await SwitchToWorker(worker);

    ALOG_INFO("Resolving {}:{}...", host, port);
    const net::SocketAddress addr = KIO_TRY(net::parse_address(host, port));

    ALOG_INFO("Creating socket...");
    int fd = KIO_TRY(net::create_raw_socket(addr.family));

    ALOG_INFO("Connecting to {}:{}...", host, port);
    auto connect_result = co_await worker.async_connect(fd, reinterpret_cast<const sockaddr*>(&addr.addr), addr.addrlen);

    if (!connect_result)
    {
        close(fd);
        ALOG_ERROR("Connection failed: {}", connect_result.error());
        co_return std::unexpected(connect_result.error());
    }

    ALOG_INFO("✅ Connected successfully! (fd={})", fd);
    co_return fd;
}

Task<Result<void>> echo_test(Worker& worker, std::string_view host, uint16_t port)
{
    // Connect to server
    int fd = KIO_TRY(co_await connect_to_server(worker, host, port));

    // Test message
    std::string message = "Hello from kio TCP client!\n";
    ALOG_INFO("Sending: {}", message);

    // Write message
    auto write_result = co_await worker.async_write(fd, std::span<const char>(message.data(), message.size()));
    if (!write_result)
    {
        ALOG_ERROR("Write failed: {}", write_result.error());
        co_await worker.async_close(fd);
        co_return std::unexpected(write_result.error());
    }

    ALOG_INFO("✅ Sent {} bytes", write_result.value());

    // Read response
    char buffer[8192] = {};
    ALOG_INFO("Waiting for response...");

    auto read_result = co_await worker.async_read(fd, buffer);
    if (!read_result)
    {
        ALOG_ERROR("Read failed: {}", read_result.error());
        co_await worker.async_close(fd);
        co_return std::unexpected(read_result.error());
    }

    const int bytes_read = read_result.value();
    ALOG_INFO("✅ Received {} bytes: {}", bytes_read, std::string_view(buffer, bytes_read));

    // Verify echo
    if (bytes_read == message.size() && std::string_view(buffer, bytes_read) == message)
    {
        ALOG_INFO("✅✅✅ Echo test PASSED! ✅✅✅");
    }
    else
    {
        ALOG_WARN("Echo mismatch - sent {} bytes, received {} bytes", message.size(), bytes_read);
    }

    // Close connection
    co_await worker.async_close(fd);
    ALOG_INFO("Connection closed");

    co_return {};
}

Task<Result<void>> multi_message_test(Worker& worker, std::string_view host, uint16_t port)
{
    // Connect to server
    int fd = KIO_TRY(co_await connect_to_server(worker, host, port));

    // Send multiple messages
    const char* messages[] = {"Message 1: Testing kio framework\n", "Message 2: This is awesome!\n", "Message 3: Echo server works!\n"};

    for (const char* msg: messages)
    {
        std::string_view message(msg);
        ALOG_INFO("Sending: {}", message);

        // Write
        auto write_result = co_await worker.async_write(fd, std::span<const char>(message.data(), message.size()));
        if (!write_result)
        {
            ALOG_ERROR("Write failed: {}", write_result.error());
            break;
        }

        // Read echo
        char buffer[8192] = {};
        auto read_result = co_await worker.async_read(fd, buffer);
        if (!read_result)
        {
            ALOG_ERROR("Read failed: {}", read_result.error());
            break;
        }

        ALOG_INFO("Received: {}", std::string_view(buffer, read_result.value()));
    }

    co_await worker.async_close(fd);
    ALOG_INFO("Multi-message test complete");

    co_return {};
}

Task<Result<void>> large_data_test(Worker& worker, std::string_view host, uint16_t port)
{
    // Connect to server
    int fd = KIO_TRY(co_await connect_to_server(worker, host, port));

    // Create large message (10KB)
    const size_t size = 10 * 1024;
    std::string large_message(size, 'A');
    for (size_t i = 0; i < size; i += 100)
    {
        large_message[i] = '\n';  // Add some newlines
    }

    ALOG_INFO("Sending large message ({} bytes)...", size);

    // Write
    auto write_result = co_await worker.async_write(fd, std::span<const char>(large_message.data(), large_message.size()));
    if (!write_result)
    {
        ALOG_ERROR("Write failed: {}", write_result.error());
        co_await worker.async_close(fd);
        co_return std::unexpected(write_result.error());
    }

    ALOG_INFO("✅ Sent {} bytes", write_result.value());

    // Read response (may need multiple reads)
    std::string received;
    char buffer[8192];
    size_t total_read = 0;

    while (total_read < size)
    {
        auto read_result = co_await worker.async_read(fd, buffer);
        if (!read_result || read_result.value() == 0)
        {
            ALOG_ERROR("Read failed or EOF after {} bytes", total_read);
            break;
        }

        total_read += read_result.value();
        received.append(buffer, read_result.value());
        ALOG_DEBUG("Read {} bytes (total: {} / {})", read_result.value(), total_read, size);
    }

    ALOG_INFO("✅ Received {} bytes total", total_read);

    if (received == large_message)
    {
        ALOG_INFO("✅✅✅ Large data test PASSED! ✅✅✅");
    }
    else
    {
        ALOG_WARN("Data mismatch - sent {} bytes, received {} bytes", size, total_read);
    }

    co_await worker.async_close(fd);

    co_return {};
}

int main(int argc, char* argv[])
{
    // Parse arguments
    std::string host = "127.0.0.1";
    uint16_t port = 8080;

    if (argc >= 2)
    {
        host = argv[1];
    }
    if (argc >= 3)
    {
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    }

    // Setup logging
    alog::configure(4096, LogLevel::Info);

    // Configure worker
    WorkerConfig config{};
    config.uring_queue_depth = 16800;
    config.default_op_slots = 8096;

    Worker worker(0, config);
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    ALOG_INFO("========================================");
    ALOG_INFO("KIO TCP Client Test Suite");
    ALOG_INFO("Target: {}:{}", host, port);
    ALOG_INFO("========================================");

    // Test 1: Simple echo test
    ALOG_INFO("\n[TEST 1] Simple Echo Test");
    if (auto result = SyncWait(echo_test(worker, host, port)); !result.has_value())
    {
        ALOG_ERROR("Test 1 failed: {}", result.error());
    }

    // Small delay between tests
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Test 2: Multiple messages
    ALOG_INFO("\n[TEST 2] Multiple Messages Test");
    if (auto result = SyncWait(multi_message_test(worker, host, port)); !result.has_value())
    {
        ALOG_ERROR("Test 2 failed: {}", result.error());
    }

    // Small delay between tests
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Test 3: Large data
    ALOG_INFO("\n[TEST 3] Large Data Test");
    if (auto result = SyncWait(large_data_test(worker, host, port)); !result.has_value())
    {
        ALOG_ERROR("Test 3 failed: {}", result.error());
    }

    ALOG_INFO("\n========================================");
    ALOG_INFO("All tests complete!");
    ALOG_INFO("========================================");

    (void) worker.request_stop();
    return 0;
}
