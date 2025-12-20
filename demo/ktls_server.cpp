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

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "kio/core/async_logger.h"
#include "kio/core/worker_pool.h"
#include "kio/tls/listener.h"
#include "kio/tls/stream.h"


namespace io = kio::io;
namespace tls = kio::tls;
namespace net = kio::net;

static auto handle_client(tls::TlsStream stream) -> kio::DetachedTask
{
    ALOG_INFO("✅ Client ready to start sending traffic");

    char buffer[8192];
    size_t total_bytes = 0;

    while (true)
    {
        auto read_result = co_await stream.async_read(buffer);
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
        if (auto write_result = co_await stream.async_write_exact({buffer, static_cast<size_t>(bytes_read)});
            !write_result.has_value())
        {
            ALOG_ERROR("Write error: {}", write_result.error());
            break;
        }
    }

    ALOG_INFO("Connection closed. Total bytes echoed: {}", total_bytes);
    co_await stream.async_close();
}

// Accept loop - runs on each worker independently
DetachedTask accept_loop(Worker& worker, const ListenerConfig& listener_cfg, TlsContext& ctx)
{
    ALOG_INFO("✅ Starting KTLS-only server");
    const auto st = worker.get_stop_token();

    auto listener_res = TlsListener::bind(worker, listener_cfg, ctx);
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
        auto accept_res = co_await listener.accept();

        if (!accept_res.has_value())
        {
            ALOG_ERROR("Accept error: {}", accept_res.error());
            continue;
        }

        auto stream = std::move(accept_res.value());
        ALOG_DEBUG("Accepted connection from {}:{}", stream.peer_ip(), stream.peer_port());

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        handle_client(std::move(stream));
    }
    ALOG_INFO("Worker {} stopped accepting connections", worker.get_id());
}

int main()
{
    alog::configure(4096, LogLevel::Debug);

    // do not kill our server on a broken pipe
    signal(SIGPIPE, SIG_IGN);

    ALOG_INFO("OpenSSL version: {}", OpenSSL_version(OPENSSL_VERSION));

    ListenerConfig listener_cfg{};
    listener_cfg.port = 8080;

    TlsConfig tls_cfg{};
    // TODO: make it configurable
    tls_cfg.cert_path = "/home/ynachi/test_certs/server.crt";
    tls_cfg.key_path = "/home/ynachi/test_certs/server.key";

    // Create SSL context (reuse for all connections!)
    auto ctx_res = TlsContext::make_server(tls_cfg);
    if (!ctx_res.has_value())
    {
        ALOG_ERROR("Failed to create TLS Context: {}", ctx_res.error());
        return 1;
    }

    ALOG_INFO("🚀 KTLS-Only Server listening on port {}", listener_cfg.port);
    ALOG_INFO("⚡ Zero-copy mode - maximum performance!");
    ALOG_INFO("📝 TLS 1.3 only, no fallback");

    auto ctx = std::move(ctx_res.value());

    // Configure workers
    WorkerConfig config{};
    config.uring_queue_depth = 16800;
    config.default_op_slots = 8096;

    // Create a worker pool
    // You may have noticed references are passed to coroutines. You may wonder about lifetime and you are right!
    // But the design makes it safe to use references here.
    // The ownership model is clear.
    // Main own objects that are shared with the non owning struct and the main thread is the last to go
    // out of scope. See below, it does after the pool is stopped.
    // As a user, you can still create shared ptrs for worker, TLS context, ... but that is not necessary normally.
    IOPool pool(4, config, [&listener_cfg, &ctx](Worker& worker) { accept_loop(worker, listener_cfg, ctx); });

    // Main thread waits
    std::cout << "Server running. Press Enter to stop..." << std::endl;
    // Blocks until user presses Enter
    std::cin.get();
    pool.stop();

    ALOG_INFO("Server stopped");

    return 0;
}
