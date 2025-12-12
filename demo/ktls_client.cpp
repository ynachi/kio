//
// Created by Yao ACHI on 11/12/2025.
//
// KTLS Client Demo - Tests TLS client functionality with the kio framework
//

#include <format>
#include <gflags/gflags.h>
#include <iostream>

#include "kio/core/async_logger.h"
#include "kio/core/worker.h"
#include "kio/net/net.h"
#include "kio/sync/sync_wait.h"
#include "kio/tls/context.h"
#include "kio/tls/listener.h"  // For TlsConnector

// CLI flags
DEFINE_string(host, "127.0.0.1", "Server host");
DEFINE_uint32(port, 8080, "Server port");
DEFINE_string(mode, "echo", "Test mode: raw, simple, echo, http, or perf");
DEFINE_string(path, "/", "HTTP path for http mode");
DEFINE_uint64(bytes, 10 * 1024 * 1024, "Bytes to transfer for perf mode");
DEFINE_string(ca, "", "CA certificate path (optional)");
DEFINE_bool(verify, false, "Enable certificate verification");

using namespace kio;
using namespace kio::io;
using namespace kio::tls;

// =========================================================================
// Raw Test - Bypass TlsConnector to isolate issues
// =========================================================================

Task<Result<void>> raw_test(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Raw test: connecting to {}:{}", host, port);

    // Step 1: Resolve address
    auto addr_res = net::resolve_address(host, port);
    if (!addr_res.has_value())
    {
        ALOG_ERROR("Address resolution failed: {}", addr_res.error());
        co_return std::unexpected(addr_res.error());
    }
    auto addr = addr_res.value();
    ALOG_INFO("Address resolved");

    // Step 2: Create socket
    auto fd_res = net::create_tcp_fd(addr.family);
    if (!fd_res.has_value())
    {
        ALOG_ERROR("Socket creation failed: {}", fd_res.error());
        co_return std::unexpected(fd_res.error());
    }
    int fd = fd_res.value();
    ALOG_INFO("Socket created: fd={}", fd);

    // Step 3: Connect
    auto connect_res = co_await worker.async_connect(fd, addr.as_sockaddr(), addr.addrlen);
    if (!connect_res.has_value())
    {
        ALOG_ERROR("Connect failed: {}", connect_res.error());
        ::close(fd);
        co_return std::unexpected(connect_res.error());
    }
    ALOG_INFO("TCP connected");

    // Step 4: Create TlsStream (this does SSL_new, SSL_set_fd)
    net::Socket socket(fd);
    TlsStream stream(worker, std::move(socket), ctx, TlsRole::Client);
    ALOG_INFO("TlsStream created, fd in stream: {}", stream.fd());

    // Step 5: Handshake
    auto hs_res = co_await stream.async_handshake(host);
    if (!hs_res.has_value())
    {
        ALOG_ERROR("Handshake failed: {}", hs_res.error());
        co_return std::unexpected(hs_res.error());
    }
    ALOG_INFO("Handshake complete, KTLS={}, cipher={}", stream.is_ktls_active(), stream.get_cipher());

    // Step 6: Write (using raw worker call to bypass any TlsStream issues)
    const char* msg = "TEST\n";
    ALOG_INFO("Writing 5 bytes via stream.async_write...");
    auto write_res = co_await stream.async_write({msg, 5});
    if (!write_res.has_value())
    {
        ALOG_ERROR("Write failed: {}", write_res.error());
        co_return std::unexpected(write_res.error());
    }
    ALOG_INFO("Write completed: {} bytes", write_res.value());

    // Step 7: Small delay to ensure data is sent
    ALOG_INFO("Sleeping 100ms...");
    co_await worker.async_sleep(std::chrono::milliseconds(100));

    // Step 8: Read
    char buffer[256];
    ALOG_INFO("Reading via stream.async_read...");
    auto read_res = co_await stream.async_read(buffer);
    if (!read_res.has_value())
    {
        ALOG_ERROR("Read failed: {}", read_res.error());

        // Try reading again with raw fd to see if it's a TlsStream issue
        ALOG_INFO("Trying raw read on fd {}...", stream.fd());
        auto raw_read_res = co_await worker.async_read(stream.fd(), std::span<char>(buffer, sizeof(buffer)));
        if (!raw_read_res.has_value())
        {
            ALOG_ERROR("Raw read also failed: {}", raw_read_res.error());
        }
        else
        {
            ALOG_INFO("Raw read succeeded: {} bytes", raw_read_res.value());
        }

        co_return std::unexpected(read_res.error());
    }
    ALOG_INFO("Read completed: {} bytes", read_res.value());

    std::string_view response(buffer, read_res.value());
    ALOG_INFO("Response: '{}'", response);

    co_await stream.async_close();
    ALOG_INFO("✅ Raw test passed!");

    co_return {};
}

// =========================================================================
// Simple Test - Just connect and send one message (for debugging)
// =========================================================================

Task<Result<void>> simple_test(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Simple test: connecting to {}:{}", host, port);

    TlsConnector connector(worker, ctx);
    ALOG_DEBUG("Created connector, calling connect...");
    auto connect_result = co_await connector.connect(host, port);
    if (!connect_result.has_value())
    {
        ALOG_ERROR("Connect failed: {}", connect_result.error());
        co_return std::unexpected(connect_result.error());
    }

    TlsStream stream = std::move(connect_result.value());
    ALOG_INFO("✅ Connected! fd={}, KTLS={}", stream.fd(), stream.is_ktls_active());

    // Simple message
    const char* msg = "PING\n";
    const size_t msg_len = 5;

    ALOG_INFO("Sending {} bytes...", msg_len);
    auto write_result = co_await stream.async_write({msg, msg_len});
    if (!write_result.has_value())
    {
        ALOG_ERROR("Write failed: {}", write_result.error());
        co_return std::unexpected(write_result.error());
    }
    ALOG_INFO("Write returned: {} bytes", write_result.value());

    // Small delay to let server process (debugging only)
    // co_await worker.async_sleep(std::chrono::milliseconds(100));

    ALOG_INFO("Reading response...");
    char buffer[256];
    auto read_result = co_await stream.async_read(buffer);
    if (!read_result.has_value())
    {
        ALOG_ERROR("Read failed: {}", read_result.error());
        co_return std::unexpected(read_result.error());
    }
    ALOG_INFO("Read returned: {} bytes", read_result.value());

    if (read_result.value() > 0)
    {
        std::string_view response(buffer, read_result.value());
        ALOG_INFO("Response: '{}'", response);
    }

    ALOG_INFO("Closing connection...");
    co_await stream.async_close();
    ALOG_INFO("✅ Simple test passed!");

    co_return {};
}

// =========================================================================
// Echo Test - Send messages and verify they come back
// =========================================================================

Task<Result<void>> echo_test(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Starting echo test to {}:{}", host, port);

    TlsConnector connector(worker, ctx);
    TlsStream stream = KIO_TRY(co_await connector.connect(host, port));

    ALOG_INFO("✅ Connected!");
    ALOG_INFO("   Version: {}", stream.get_version());
    ALOG_INFO("   Cipher:  {}", stream.get_cipher());
    ALOG_INFO("   KTLS:    {}", stream.is_ktls_active() ? "active" : "NOT active");

    const std::vector<std::string> messages = {
            "Hello from kio client!\n",
            "Testing KTLS echo...\n",
            "Message number 3\n",
            "Final message\n",
    };

    char buffer[1024];

    for (const auto& msg: messages)
    {
        std::string_view msg_trimmed(msg.data(), msg.size() > 0 && msg.back() == '\n' ? msg.size() - 1 : msg.size());
        ALOG_INFO("Sending: '{}'", msg_trimmed);

        KIO_TRY(co_await stream.async_write_exact({msg.data(), msg.size()}));

        auto read_result = co_await stream.async_read(buffer);
        if (!read_result.has_value())
        {
            ALOG_ERROR("Read error: {}", read_result.error());
            co_return std::unexpected(read_result.error());
        }

        const int bytes_read = read_result.value();
        if (bytes_read == 0)
        {
            ALOG_ERROR("Unexpected EOF from server");
            co_return std::unexpected(Error{ErrorCategory::Network, ECONNRESET});
        }

        std::string_view response(buffer, bytes_read);
        std::string_view response_trimmed(response.data(), response.size() > 0 && response.back() == '\n' ? response.size() - 1 : response.size());
        ALOG_INFO("Received: '{}'", response_trimmed);

        if (response != msg)
        {
            ALOG_ERROR("Echo mismatch! Expected '{}', got '{}'", msg, response);
            co_return std::unexpected(Error{ErrorCategory::Application, kAppUnknown});
        }
    }

    ALOG_INFO("✅ Echo test passed! All {} messages verified.", messages.size());
    co_await stream.async_close();

    co_return {};
}

// =========================================================================
// HTTP GET Test - Send an HTTP request and read the response
// =========================================================================

Task<Result<void>> http_get_test(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port, std::string_view path)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("HTTP GET https://{}:{}{}", host, port, path);

    TlsConnector connector(worker, ctx);
    TlsStream stream = KIO_TRY(co_await connector.connect(host, port));

    ALOG_INFO("✅ TLS handshake complete!");
    ALOG_INFO("   Version: {}", stream.get_version());
    ALOG_INFO("   Cipher:  {}", stream.get_cipher());
    ALOG_INFO("   KTLS:    {}", stream.is_ktls_active() ? "active" : "NOT active");

    std::string request = std::format(
            "GET {} HTTP/1.1\r\n"
            "Host: {}\r\n"
            "User-Agent: kio-ktls-client/1.0\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host);

    KIO_TRY(co_await stream.async_write_exact({request.data(), request.size()}));
    ALOG_INFO("Request sent ({} bytes)", request.size());

    char buffer[8192];
    std::string response;
    size_t total_read = 0;

    while (true)
    {
        auto read_result = co_await stream.async_read(buffer);
        if (!read_result.has_value())
        {
            ALOG_ERROR("Read error: {}", read_result.error());
            break;
        }

        const int bytes_read = read_result.value();
        if (bytes_read == 0)
        {
            ALOG_DEBUG("Server closed connection (EOF)");
            break;
        }

        total_read += bytes_read;
        response.append(buffer, bytes_read);
    }

    ALOG_INFO("Response received ({} bytes total)", total_read);
    std::cout << "\n--- Response Start ---\n" << response << "\n--- Response End ---\n\n";

    co_await stream.async_close();
    co_return {};
}

// =========================================================================
// Throughput Test - Send large amount of data to measure performance
// =========================================================================

Task<Result<void>> throughput_test(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port, size_t total_bytes)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Starting throughput test to {}:{} ({} bytes)", host, port, total_bytes);

    TlsConnector connector(worker, ctx);
    TlsStream stream = KIO_TRY(co_await connector.connect(host, port));

    ALOG_INFO("✅ Connected! KTLS: {}", stream.is_ktls_active() ? "active" : "NOT active");

    constexpr size_t kChunkSize = 16384;
    std::vector<char> send_buf(kChunkSize, 'X');
    std::vector<char> recv_buf(kChunkSize);

    size_t bytes_sent = 0;
    size_t bytes_received = 0;

    auto start = std::chrono::steady_clock::now();

    while (bytes_sent < total_bytes)
    {
        size_t to_send = std::min(kChunkSize, total_bytes - bytes_sent);

        KIO_TRY(co_await stream.async_write_exact({send_buf.data(), to_send}));
        bytes_sent += to_send;

        size_t chunk_received = 0;
        while (chunk_received < to_send)
        {
            auto read_result = co_await stream.async_read({recv_buf.data(), recv_buf.size()});
            if (!read_result.has_value())
            {
                ALOG_ERROR("Read error: {}", read_result.error());
                co_return std::unexpected(read_result.error());
            }

            const int n = read_result.value();
            if (n == 0)
            {
                ALOG_ERROR("Unexpected EOF");
                co_return std::unexpected(Error{ErrorCategory::Network, ECONNRESET});
            }

            chunk_received += n;
            bytes_received += n;
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double throughput_mbps = 0;
    if (duration_ms > 0)
    {
        throughput_mbps = (static_cast<double>(bytes_sent + bytes_received) / (1024 * 1024)) / (static_cast<double>(duration_ms) / 1000);
    }

    ALOG_INFO("✅ Throughput test complete!");
    ALOG_INFO("   Sent:       {} bytes", bytes_sent);
    ALOG_INFO("   Received:   {} bytes", bytes_received);
    ALOG_INFO("   Duration:   {} ms", duration_ms);
    ALOG_INFO("   Throughput: {:.2f} MB/s", throughput_mbps);

    co_await stream.async_close();
    co_return {};
}

// =========================================================================
// Main
// =========================================================================

int main(int argc, char* argv[])
{
    alog::configure(4096, LogLevel::Debug);

    gflags::SetUsageMessage(
            "KTLS Client Demo - Test TLS client with kio framework\n\n"
            "Modes:\n"
            "  simple - Minimal test (connect, send, receive, close)\n"
            "  echo   - Send messages and verify echo response\n"
            "  http   - Send HTTP GET request\n"
            "  perf   - Throughput benchmark\n\n"
            "Examples:\n"
            "  ktls_client_demo --mode=simple --host=127.0.0.1 --port=8080\n"
            "  ktls_client_demo --mode=echo --host=127.0.0.1 --port=8080\n"
            "  ktls_client_demo --mode=http --host=example.com --port=443 --verify\n"
            "  ktls_client_demo --mode=perf --bytes=104857600");

    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ALOG_INFO("=== KTLS Client Demo ===");
    ALOG_INFO("Mode: {}, Target: {}:{}", FLAGS_mode, FLAGS_host, FLAGS_port);
    ALOG_INFO("OpenSSL: {}", OpenSSL_version(OPENSSL_VERSION));

    // Check KTLS availability
    ALOG_INFO("KTLS Info:\n{}", get_ktls_info());
    if (auto res = require_ktls(); !res)
    {
        ALOG_ERROR("KTLS not available: {}", res.error());
        return 1;
    }

    // Create TLS client context
    TlsConfig tls_cfg{};
    if (FLAGS_verify)
    {
        tls_cfg.verify_mode = SSL_VERIFY_PEER;
        if (!FLAGS_ca.empty())
        {
            tls_cfg.ca_cert_path = FLAGS_ca;
        }
        ALOG_INFO("Certificate verification: ENABLED");
    }
    else
    {
        tls_cfg.verify_mode = SSL_VERIFY_NONE;
        ALOG_INFO("Certificate verification: DISABLED (testing mode)");
    }

    auto ctx_res = TlsContext::make_client(tls_cfg);
    if (!ctx_res.has_value())
    {
        ALOG_ERROR("Failed to create TLS context: {}", ctx_res.error());
        return 1;
    }
    auto ctx = std::move(ctx_res.value());

    // Create worker
    WorkerConfig worker_cfg{};
    worker_cfg.uring_queue_depth = 1024;
    worker_cfg.default_op_slots = 256;

    Worker worker(0, worker_cfg);
    std::jthread worker_thread([&] { worker.loop_forever(); });
    worker.wait_ready();

    // Run the test
    Result<void> result;

    if (FLAGS_mode == "raw")
    {
        result = SyncWait(raw_test(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
    }
    else if (FLAGS_mode == "simple")
    {
        result = SyncWait(simple_test(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
    }
    else if (FLAGS_mode == "echo")
    {
        result = SyncWait(echo_test(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
    }
    else if (FLAGS_mode == "http")
    {
        result = SyncWait(http_get_test(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port), FLAGS_path));
    }
    else if (FLAGS_mode == "perf")
    {
        result = SyncWait(throughput_test(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port), FLAGS_bytes));
    }
    else
    {
        ALOG_ERROR("Unknown mode: '{}'. Use 'raw', 'simple', 'echo', 'http', or 'perf'.", FLAGS_mode);
        worker.request_stop();
        return 1;
    }

    if (!result.has_value())
    {
        ALOG_ERROR("❌ Test failed: {}", result.error());
    }
    else
    {
        ALOG_INFO("✅ Test completed successfully!");
    }

    worker.request_stop();
    return result.has_value() ? 0 : 1;
}
