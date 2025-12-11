//
// Created by Yao ACHI on 07/12/2025.
//

#include <format>
#include <iostream>
#include <kio/sync/sync_wait.h>
#include <kio/tls/context.h>
#include <kio/tls/stream.h>

#include "kio/core/async_logger.h"
#include "kio/net/net.h"

using namespace kio;
using namespace kio::io;
using namespace kio::tls;

Task<Result<int>> connect_with_retries(Worker& worker, std::string_view host, uint16_t port)
{
    const net::SocketAddress addr = KIO_TRY(net::resolve_address(host, port));

    int max_retries = 5;
    auto delay = std::chrono::milliseconds(200);

    for (int i = 0; i < max_retries; ++i)
    {
        // Use the 2-argument KIO_TRY for assignment
        int fd = KIO_TRY(net::create_raw_socket(addr.family));

        ALOG_INFO("Attempt {}/{} to connect...", i + 1, max_retries);
        // Add reinterpret_cast to match const sockaddr*
        auto connect_result = co_await worker.async_connect(fd, reinterpret_cast<const sockaddr*>(&addr), addr.addrlen);

        if (connect_result)
        {
            ALOG_INFO("Connection successful on attempt {}!", i + 1);
            co_return fd;  // Success!
        }

        // Connection failed. Log it, close the failed fd, and wait.
        ALOG_WARN("Connect failed: {}. Retrying in {:.1f}s.", connect_result.error(), std::chrono::duration<double>(delay).count());

        close(fd);

        // Use the 1-argument KIO_TRY for a void expression
        KIO_TRY(co_await worker.async_sleep(delay));

        // Double the delay for the next attempt (exponential backoff)
        delay *= 2;
    }

    ALOG_ERROR("All {} connection attempts failed.", max_retries);
    co_return std::unexpected(Error::from_errno(ETIMEDOUT));
}

Task<Result<void>> https_get_ktls(Worker& worker, std::string_view host, uint16_t port, std::string_view path, std::string_view ca_path = "")
{
    co_await SwitchToWorker(worker);
    ALOG_INFO("Connecting to {}:{}...", host, port);

    auto client_fd = KIO_TRY(co_await connect_with_retries(worker, host, port));

    ALOG_INFO("Connected! Starting KTLS handshake...");

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    // 1. Create SSL context with CA path (Fixes verification error)
    // REPLACE with your actual CA path
    TlsContext ctx = ca_path.empty() ? create_client_context() : create_client_context(std::string(ca_path));

    ALOG_INFO("CA certificate: {}", ca_path.empty() ? "not set (verification disabled)" : std::string(ca_path));

    // 2. Enable verification
    // ctx.set_verify_peer(false);

    // Create a TLS stream with SNI
    TlsStream tls(worker, client_fd, ctx, host);

    // KTLS handshake
    KIO_TRY(co_await tls.async_handshake());

    ALOG_INFO("✅ KTLS handshake complete!");

    std::string request = std::format(
            "GET {} HTTP/1.1\r\n"
            "Host: {}\r\n"
            "Connection: close\r\n"
            "\r\n",
            path, host);

    auto write_count = KIO_TRY(co_await tls.async_write(std::span<const char>(request.data(), request.size())));
    ALOG_INFO("Request sent, wrote {} bytes. Waiting for response...", write_count);

    char buffer[8192];
    auto read_count = co_await tls.async_read(buffer);

    if (!read_count.has_value())
    {
        ALOG_ERROR("Failed to read response: {}", read_count.error());
        co_return std::unexpected(read_count.error());
    }

    ALOG_INFO("Response received ({} bytes)", read_count.value());

    co_await tls.shutdown();
    co_await worker.async_close(client_fd);

    co_return {};
}

int main()
{
    alog::configure(4096, LogLevel::Info);
    WorkerConfig config{};
    config.uring_queue_depth = 16800;
    config.default_op_slots = 8096;

    // IOPool pool(4, config, [server_fd, &ctx](Worker& worker) { accept_loop(worker, server_fd, ctx).detach(); });

    Worker worker(0, config);
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    const char* ca_path = "/home/ynachi/test_certs/ca.crt";

    if (auto result = SyncWait(https_get_ktls(worker, "127.0.0.1", 8080, "ok", ca_path)); !result.has_value())
    {
        ALOG_ERROR("Client code failed failed: {}", result.error());
    }

    // Cast to (void) to suppress the nodiscard warning
    (void) worker.request_stop();
    return 0;
}
