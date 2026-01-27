//
// Created by Yao ACHI on 11/12/2025.

//
// KTLS Client Demo - Tests TLS client functionality with the kio framework
//

/**
 * Here is how you can create a cert to test
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 365 -key ca.key -out ca.crt \
    -subj "/CN=Test CA"

# 2. Create server certificate request
openssl genrsa -out server.key 4096
openssl req -new -key server.key -out server.csr \
    -subj "/CN=localhost"

# 3. Sign with CA
openssl x509 -req -days 365 -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt
 */

#include "kio/core/async_logger.h"
#include "kio/core/worker.h"
#include "kio/net/net.h"
#include "kio/sync/sync_wait.h"
#include "kio/tls/context.h"
#include "kio/tls/listener.h"

#include <format>
#include <iostream>

#include <gflags/gflags.h>

// CLI flags
DEFINE_string(client_cert, "/home/ynachi/test_certs/client.crt", "Path to client certificate (for mTLS)");
DEFINE_string(client_key, "/home/ynachi/test_certs/client.key", "Path to client private key (for mTLS)");
DEFINE_string(host, "127.0.0.1", "Server host");
DEFINE_uint32(port, 8443, "Server port");
DEFINE_string(mode, "echo", "Test mode: raw, simple, echo, or perf");
DEFINE_string(path, "/", "HTTP path for http mode");
DEFINE_uint64(bytes, 10 * 1024 * 1024, "Bytes to transfer for perf mode");
DEFINE_string(ca, "/home/ynachi/test_certs/ca.crt",
              "CA certificate path, optional but must be provided if verify is true");
DEFINE_bool(verify, true, "Enable certificate verification");
DEFINE_string(alpn, "h2,http/1.1", "Comma-separated list of ALPN protocols to advertise (e.g. 'h2,http/1.1')");

using namespace kio;
using namespace kio::io;
using namespace kio::tls;

// =========================================================================
// Simple Demo - Just connect and send one message
// =========================================================================

Task<Result<void>> SimpleTest(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Simple test: connecting to {}:{}", host, port);

    TlsConnector connector(worker, ctx);
    ALOG_DEBUG("Created connector, calling Connect...");
    auto connect_result = co_await connector.Connect(host, port);
    if (!connect_result.has_value())
    {
        ALOG_ERROR("Connect failed: {}", connect_result.error());
        co_return std::unexpected(connect_result.error());
    }

    TlsStream stream = std::move(connect_result.value());
    ALOG_INFO("✅ Connected! fd={}, KTLS={}", stream.Fd(), stream.IsKtlsActive());

    // Check ALPN result
    if (auto proto = stream.GetNegotiatedProtocol(); !proto.empty())
    {
        ALOG_INFO("🌐 Negotiated ALPN Protocol: '{}'", proto);
    }
    else
    {
        ALOG_INFO("🌐 No ALPN protocol negotiated");
    }

    // Simple message
    auto msg = "PING\n";
    constexpr size_t msg_len = 5;

    ALOG_INFO("Sending {} bytes...", msg_len);
    auto write_result = co_await stream.AsyncWrite({msg, msg_len});
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
    auto read_result = co_await stream.AsyncRead(buffer);
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
    co_await stream.AsyncClose();
    ALOG_INFO("✅ Simple test passed!");

    co_return {};
}

// =========================================================================
// Echo Test - Send messages and verify they come back
// =========================================================================

Task<Result<void>> EchoTest(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Starting echo test to {}:{}", host, port);

    TlsConnector connector(worker, ctx);
    TlsStream stream = KIO_TRY(co_await connector.Connect(host, port));

    ALOG_INFO("✅ Connected!");
    ALOG_INFO("   Version: {}", stream.GetVersion());
    ALOG_INFO("   Cipher:  {}", stream.GetCipher());
    ALOG_INFO("   KTLS:    {}", stream.IsKtlsActive() ? "active" : "NOT active");

    if (auto proto = stream.GetNegotiatedProtocol(); !proto.empty())
    {
        ALOG_INFO("   ALPN:    '{}'", proto);
    }
    else
    {
        ALOG_INFO("   ALPN:    <none>");
    }

    const std::vector<std::string> messages = {
        "Hello from kio client!\n",
        "Testing KTLS echo...\n",
        "Message number 3\n",
        "Final message\n",
    };

    char buffer[1024];

    for (const auto& msg : messages)
    {
        std::string_view msg_trimmed(msg.data(), !msg.empty() && msg.back() == '\n' ? msg.size() - 1 : msg.size());
        ALOG_INFO("Sending: '{}'", msg_trimmed);

        KIO_TRY(co_await stream.AsyncWriteExact({msg.data(), msg.size()}));

        auto read_result = co_await stream.AsyncRead(buffer);
        if (!read_result.has_value())
        {
            ALOG_ERROR("Read error: {}", read_result.error());
            co_return std::unexpected(read_result.error());
        }

        const int bytes_read = read_result.value();
        if (bytes_read == 0)
        {
            ALOG_ERROR("Unexpected EOF from server");
            co_return std::unexpected(Error{ErrorCategory::kNetwork, ECONNRESET});
        }

        std::string_view response(buffer, bytes_read);
        std::string_view response_trimmed(
            response.data(), !response.empty() && response.back() == '\n' ? response.size() - 1 : response.size());
        ALOG_INFO("Received: '{}'", response_trimmed);

        if (response != msg)
        {
            ALOG_ERROR("Echo mismatch! Expected '{}', got '{}'", msg, response);
            co_return std::unexpected(Error{ErrorCategory::kApplication, kAppUnknown});
        }
    }

    ALOG_INFO("✅ Echo test passed! All {} messages verified.", messages.size());
    co_await stream.AsyncClose();

    co_return {};
}

// =========================================================================
// Throughput Test - Send large amount of data to measure performance
// =========================================================================

Task<Result<void>> ThroughputTest(Worker& worker, TlsContext& ctx, std::string_view host, uint16_t port,
                                  size_t total_bytes)
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Starting throughput test to {}:{} ({} bytes)", host, port, total_bytes);

    TlsConnector connector(worker, ctx);
    TlsStream stream = KIO_TRY(co_await connector.Connect(host, port));

    ALOG_INFO("✅ Connected! KTLS: {}", stream.IsKtlsActive() ? "active" : "NOT active");
    if (auto proto = stream.GetNegotiatedProtocol(); !proto.empty())
    {
        ALOG_INFO("   ALPN:    '{}'", proto);
    }

    constexpr size_t kChunkSize = 16384;
    std::vector send_buf(kChunkSize, 'X');
    std::vector<char> recv_buf(kChunkSize);

    size_t bytes_sent = 0;
    size_t bytes_received = 0;

    auto start = std::chrono::steady_clock::now();

    while (bytes_sent < total_bytes)
    {
        size_t to_send = std::min(kChunkSize, total_bytes - bytes_sent);

        KIO_TRY(co_await stream.AsyncWriteExact({send_buf.data(), to_send}));
        bytes_sent += to_send;

        size_t chunk_received = 0;
        while (chunk_received < to_send)
        {
            auto read_result = co_await stream.AsyncRead({recv_buf.data(), recv_buf.size()});
            if (!read_result.has_value())
            {
                ALOG_ERROR("Read error: {}", read_result.error());
                co_return std::unexpected(read_result.error());
            }

            const int n = read_result.value();
            if (n == 0)
            {
                ALOG_ERROR("Unexpected EOF");
                co_return std::unexpected(Error{ErrorCategory::kNetwork, ECONNRESET});
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
        throughput_mbps = (static_cast<double>(bytes_sent + bytes_received) / (1024 * 1024)) /
                          (static_cast<double>(duration_ms) / 1000);
    }

    ALOG_INFO("✅ Throughput test complete!");
    ALOG_INFO("   Sent:       {} bytes", bytes_sent);
    ALOG_INFO("   Received:   {} bytes", bytes_received);
    ALOG_INFO("   Duration:   {} ms", duration_ms);
    ALOG_INFO("   Throughput: {:.2f} MB/s", throughput_mbps);

    co_await stream.AsyncClose();
    co_return {};
}

// =========================================================================
// Main
// =========================================================================

int main(int argc, char* argv[])
{
    alog::Configure(4096, LogLevel::kDebug);

    gflags::SetUsageMessage(
        "KTLS Client Demo - Test TLS client with kio framework\n\n"
        "Modes:\n"
        "  simple - Minimal test (connect, send, receive, close)\n"
        "  echo   - Send messages and verify echo response\n"
        "  perf   - Throughput benchmark\n\n"
        "Examples:\n"
        "  ktls_client_demo --mode=simple --host=127.0.0.1 --port=8080\n"
        "  ktls_client_demo --mode=echo --host=127.0.0.1 --port=8080\n"
        "  ktls_client_demo --mode=perf --bytes=104857600\n"
        "  ktls_client_demo --mode=simple --alpn=h2,http/1.1 --host=127.0.0.1");

    gflags::ParseCommandLineFlags(&argc, &argv, true);

    ALOG_INFO("=== KTLS Client Demo ===");
    ALOG_INFO("Mode: {}, Target: {}:{}", FLAGS_mode, FLAGS_host, FLAGS_port);
    ALOG_INFO("OpenSSL: {}", OpenSSL_version(OPENSSL_VERSION));

    // Check KTLS availability
    ALOG_INFO("KTLS Info:\n{}", GetKtlsInfo());
    if (auto res = RequireKtls(); !res)
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

    if (!FLAGS_client_cert.empty() && !FLAGS_client_key.empty())
    {
        tls_cfg.cert_path = FLAGS_client_cert;
        tls_cfg.key_path = FLAGS_client_key;
        ALOG_INFO("mTLS: Enabled (Sending client cert)");
    }

    // Parse ALPN flags
    if (!FLAGS_alpn.empty())
    {
        std::stringstream ss(FLAGS_alpn);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            tls_cfg.alpn_protocols.push_back(item);
        }
        ALOG_INFO("ALPN configured: {}", FLAGS_alpn);
    }

    auto ctx_res = TlsContext::MakeClient(tls_cfg);
    if (!ctx_res.has_value())
    {
        ALOG_ERROR("Failed to create TLS context: {}", ctx_res.error());
        return 1;
    }
    auto ctx = std::move(ctx_res.value());

    // Create worker
    WorkerConfig worker_cfg{};
    worker_cfg.uring_queue_depth = 1024;

    Worker worker(0, worker_cfg);
    std::jthread worker_thread([&] { worker.LoopForever(); });
    worker.WaitReady();

    // Run the test
    Result<void> result;

    if (FLAGS_mode == "simple")
    {
        result = SyncWait(SimpleTest(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
    }
    else if (FLAGS_mode == "echo")
    {
        result = SyncWait(EchoTest(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port)));
    }
    else if (FLAGS_mode == "perf")
    {
        result = SyncWait(ThroughputTest(worker, ctx, FLAGS_host, static_cast<uint16_t>(FLAGS_port), FLAGS_bytes));
    }
    else
    {
        ALOG_ERROR("Unknown mode: '{}'. Use 'raw', 'simple', 'echo', 'http', or 'perf'.", FLAGS_mode);
        (void)worker.RequestStop();
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

    (void)worker.RequestStop();
    return result.has_value() ? 0 : 1;
}