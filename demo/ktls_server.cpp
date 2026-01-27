//
// Created by Yao ACHI on 07/12/2025.
//

/**
 * This is a demo on how to build a TLS server using KIO.
 * As a reminder, KIO relies on Kernel TLS, and there is no fallback
 * to user space TLS. In other words, if your system does not support
 * KTLS, you won't be able to use that feature.
 *
 */

#include "kio/core/async_logger.h"
#include "kio/core/worker_pool.h"
#include "kio/tls/listener.h"
#include "kio/tls/stream.h"

#include <fcntl.h>

namespace io = kio::io;
namespace tls = kio::tls;
namespace net = kio::net;

static auto HandleClient(tls::TlsStream stream) -> kio::DetachedTask
{
    // Log negotiated protocol
    if (auto proto = stream.GetNegotiatedProtocol(); !proto.empty())
    {
        ALOG_INFO("✅ Client ready (ALPN: '{}')", proto);
    }
    else
    {
        ALOG_INFO("✅ Client ready (No ALPN)");
    }

    char buffer[8192];
    size_t total_bytes = 0;

    while (true)
    {
        auto read_result = co_await stream.AsyncRead(buffer);
        if (!read_result.has_value())
        {
            // With KTLS, EIO (errno 5) is returned when the peer closes the TLS connection.
            // This is the expected way KTLS signals "TLS close_notify received".
            // Treat it as a clean disconnect, not an error.
            if (const auto& err = read_result.error(); err.value == EIO)
            {
                ALOG_DEBUG("Client closed TLS connection (KTLS EIO - normal close)");
            }
            else
            {
                ALOG_ERROR("Read error: {}", err);
            }
            break;
        }

        const int bytes_read = read_result.value();

        // EOF: a client closed the connection
        // shouldn't happen with KTLS - we get EIO instead, but let's take it into account anyway
        if (bytes_read == 0)
        {
            ALOG_INFO("Client closed connection (EOF)");
            break;
        }

        total_bytes += bytes_read;

        // Echo back
        if (auto write_result = co_await stream.AsyncWriteExact({buffer, static_cast<size_t>(bytes_read)});
            !write_result.has_value())
        {
            ALOG_ERROR("Write error: {}", write_result.error());
            break;
        }
    }

    ALOG_INFO("Connection closed. Total bytes echoed: {}", total_bytes);
    co_await stream.AsyncClose();
}

// Accept loop - runs on each worker independently
kio::DetachedTask AcceptLoop(io::Worker& worker, const tls::ListenerConfig& listener_cfg, tls::TlsContext& ctx)
{
    ALOG_INFO("✅ Starting KTLS-only server");
    const auto st = worker.GetStopToken();

    auto listener_res = tls::TlsListener::Bind(worker, listener_cfg, ctx);
    if (!listener_res.has_value())
    {
        ALOG_ERROR("Failed to create listener: {}", listener_res.error());
        co_return;
    }

    // Listener is move only
    // This makes sense as it enforce a single ownership
    const auto listener = std::move(listener_res.value());

    while (!st.stop_requested())
    {
        // Accept connection - blocks this coroutine until a client connects
        auto accept_res = co_await listener.Accept();

        if (!accept_res.has_value())
        {
            ALOG_ERROR("Accept error: {}", accept_res.error());
            continue;
        }

        auto stream = std::move(accept_res.value());
        ALOG_DEBUG("Accepted connection from {}:{}", stream.PeerIp(), stream.PeerPort());

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        HandleClient(std::move(stream));
    }
    ALOG_INFO("Worker {} stopped accepting connections", worker.GetId());
}

int main()
{
    kio::alog::Configure(4096, kio::LogLevel::kDebug);

    // do not kill our server on a broken pipe
    signal(SIGPIPE, SIG_IGN);

    ALOG_INFO("OpenSSL version: {}", OpenSSL_version(OPENSSL_VERSION));

    tls::ListenerConfig listener_cfg{};
    listener_cfg.port = 8443;

    tls::TlsConfig tls_cfg{};
    // TODO: make it configurable
    tls_cfg.cert_path = "/home/ynachi/test_certs/server.crt";
    tls_cfg.key_path = "/home/ynachi/test_certs/server.key";

    // Add ALPN support to the server (preferred protocols)
    tls_cfg.alpn_protocols = {"h2", "http/1.1"};

    // Create SSL context (reuse for all connections!)
    auto ctx_res = tls::TlsContext::MakeServer(tls_cfg);
    if (!ctx_res.has_value())
    {
        ALOG_ERROR("Failed to create TLS Context: {}", ctx_res.error());
        return 1;
    }

    ALOG_INFO("🚀 KTLS-Only Server listening on port {}", listener_cfg.port);
    ALOG_INFO("⚡ Zero-copy mode - maximum performance!");
    ALOG_INFO("📝 TLS 1.3 only, no fallback");
    ALOG_INFO("🌐 ALPN enabled: h2, http/1.1");

    auto ctx = std::move(ctx_res.value());

    // Configure workers
    io::WorkerConfig config{};
    config.uring_queue_depth = 16800;

    // Create a worker pool
    // You may have noticed references are passed to coroutines. You may wonder about lifetime and you are right!
    // But the design makes it safe to use references here.
    // The ownership model is clear.
    // Main own objects that are shared with the non owning struct and the main thread is the last to go
    // out of scope. See below, it does after the pool is stopped.
    // As a user, you can still create shared ptrs for worker, TLS context, ... but that is not necessary normally.
    io::IOPool pool(4, config, [&listener_cfg, &ctx](io::Worker& worker) { AcceptLoop(worker, listener_cfg, ctx); });

    // Main thread waits
    std::cout << "Server running. Press Enter to stop..." << std::endl;
    // Blocks until user presses Enter
    std::cin.get();
    pool.Stop();

    ALOG_INFO("Server stopped");

    return 0;
}