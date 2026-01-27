//
// Created by Yao ACHI on 24/01/2026.
// Updated for TLS by Junie on 26/01/2026.
//
// examples/aio/215_tls_echo_server.cpp
// Demonstrates: KTLS, AsyncTlsHandshake, TaskGroup, and high-performance echo server

#include <array>
#include <print>

#include "aio/aio.hpp"
#include "aio/tls/handshake.hpp"
#include "aio/tls/socket.hpp"
#include "aio/tls/tls_context.hpp"

/**
 * To run this demo, you need:
 * 1. TLS kernel module loaded: `sudo modprobe tls`
 * 2. Certificates (server.crt, server.key) in the working directory.
 *    You can generate them using `demo/create_cert.sh`.
 */

void PrintKtlsDiagnostics()
{
    std::println("=== KTLS Diagnostics ===");
    std::println("OpenSSL version: {}", OpenSSL_version(OPENSSL_VERSION));

#ifdef SSL_OP_ENABLE_KTLS
    std::println("SSL_OP_ENABLE_KTLS: defined");
#else
    std::println("SSL_OP_ENABLE_KTLS: NOT DEFINED");
#endif

// The correct check - BIO functions, not SSL macros
#if KIO_HAVE_OPENSSL3
    std::println("BIO_get_ktls_send: available (OpenSSL 3.0+)");
#else
    std::println("BIO_get_ktls_send: NOT AVAILABLE (need OpenSSL 3.0+)");
#endif

    std::println("Kernel TLS module: {}", aio::tls::detail::HaveKtls() ? "loaded" : "NOT LOADED");
}
namespace
{

aio::Task<> HandleClient(aio::IoContext& ctx, int fd, aio::tls::TlsContext& tls_ctx)
{
    // Wrap the raw FD from AsyncAccept into a RAII Socket.
    aio::net::Socket client_sock(fd);

    // Perform the asynchronous TLS handshake.
    // This will upgrade the socket to use Kernel TLS (KTLS).
    auto handshake_res = co_await aio::tls::AsyncTlsHandshake(ctx, std::move(client_sock), tls_ctx, true);
    if (!handshake_res)
    {
        std::println(stderr, "TLS Handshake failed: {}", handshake_res.error().message());
        co_return;
    }

    // TlsSocket owns both the SSL* handle and the underlying net::Socket.
    auto tls_sock = std::move(*handshake_res);
    int ktls_fd = tls_sock.Get();

    std::println("TLS handshake successful for FD {}", ktls_fd);
    if (auto proto = tls_sock.GetNegotiatedProtocol(); proto.data())
    {
        std::println("Negotiated protocol: {}", proto);
    }

    std::array<std::byte, 1024> buffer{};

    while (true)
    {
        // For KTLS, we use standard AsyncRecv/AsyncSend on the raw FD.
        // The kernel handles encryption and decryption automatically.
        auto recv_result = co_await aio::AsyncRecv(ctx, tls_sock, buffer);
        if (!recv_result || *recv_result == 0)
            break;

        auto send_result = co_await aio::AsyncSend(ctx, tls_sock, std::span{buffer.data(), (*recv_result)});
        if (!send_result)
            break;
    }

    std::println("Client sent EOF/close, performing TLS shutdown...");
    co_await tls_sock.AsyncShutdown(ctx);
    // -----------------------------------------------------------

    // Explicitly close the socket asynchronously.
    co_await aio::AsyncClose(ctx, tls_sock);

    // Release ownership from TlsSocket to avoid double-close in its destructor
    // (since AsyncClose already handled it). Note: net::Socket doesn't have a
    // public Release() that we can easily call here through TlsSocket, but
    // AsyncClose is safe for io_uring completion.

    std::println("Client disconnected (FD {})", ktls_fd);
}

aio::Task<> Server(aio::IoContext& ctx, uint16_t port)
{
    // 1. Initialize TLS Configuration
    aio::tls::TlsConfig tls_cfg;
    // TODO: use google flags for those later
    tls_cfg.cert_path = "/home/ynachi/test_certs/server.crt";
    tls_cfg.key_path = "/home/ynachi/test_certs/server.key";
    tls_cfg.alpn_protocols = {"h2", "http/1.1"};

    tls_cfg.ca_cert_path = "/home/ynachi/test_certs/ca.crt";
    tls_cfg.verify_mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;

    // 2. Create TLS Context (enforces KTLS support)
    auto tls_ctx_res = aio::tls::TlsContext::Create(tls_cfg, true);
    if (!tls_ctx_res)
    {
        std::println(stderr, "Failed to create TLS context: {}", tls_ctx_res.error().message());
        std::println(stderr, "Ensure 'server.crt' and 'server.key' exist and 'sudo modprobe tls' was run.");
        co_return;
    }
    auto& tls_ctx = *tls_ctx_res;

    // 3. Bind TCP Listener
    auto listener = aio::net::TcpListener::Bind(port);
    if (!listener)
    {
        std::println(stderr, "Failed to bind to port {}", port);
        co_return;
    }

    std::println("TLS Echo server listening on port {}", port);
    std::println("Security: mTLS ENABLED (Client Must Present Certificate)");
    std::println("ALPN Enabled: h2, http/1.1");

    // Use TaskGroup to manage the lifetime of client tasks
    aio::TaskGroup tasks(256);

    while (true)
    {
        auto accept_result = co_await aio::AsyncAccept(ctx, listener->Get());
        if (!accept_result)
            continue;

        std::println("New client connected, starting TLS handshake...");

        // Spawn the client handler into the group.
        // We pass the TlsContext by reference; it must outlive the tasks.
        tasks.Spawn(HandleClient(ctx, accept_result->fd, tls_ctx));
    }
}
}  // namespace

int main()
{
    PrintKtlsDiagnostics();
    aio::IoContext ctx;

    // Run the server until it's done (in this case, forever)
    auto task = Server(ctx, 8443);
    ctx.RunUntilDone(task);

    return 0;
}
