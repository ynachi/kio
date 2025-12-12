//
// Created by Yao ACHI on 07/12/2025.
//

//
// KTLS-Only Demo - No Fallback
//

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "kio/core/async_logger.h"
#include "kio/core/worker_pool.h"
#include "kio/net/net.h"
#include "kio/tls/listener.h"
#include "kio/tls/stream.h"

using namespace kio;
using namespace kio::tls;
using namespace kio::io;
using namespace kio::net;

// =========================================================================
// EXAMPLE 1: High-Performance Echo Server (KTLS-Only)
// =========================================================================

DetachedTask handle_client(TlsStream stream)
{
    ALOG_INFO("✅ Client ready (pure kernel TLS mode)");

    char buffer[8192];
    size_t total_bytes = 0;

    while (true)
    {
        auto read_result = co_await stream.async_read(buffer);
        if (!read_result.has_value())
        {
            ALOG_ERROR("Read error: {}", read_result.error());
            break;
        }

        const int bytes_read = read_result.value();

        // EOF: client closed the connection
        if (bytes_read == 0)
        {
            ALOG_INFO("Client closed connection cleanly");
            break;
        }

        total_bytes += bytes_read;

        // Echo back
        if (auto write_result = co_await stream.async_write_exact({buffer, static_cast<size_t>(bytes_read)}); !write_result.has_value())
        {
            ALOG_ERROR("Write error: {}", write_result.error());
            break;
        }
    }

    ALOG_INFO("Connection closed. Total bytes: {}", total_bytes);
    co_await stream.async_close();
}

// Accept loop - runs on each worker independently
DetachedTask accept_loop(Worker& worker, ListenerConfig& listener_cfg, TlsContext& ctx)
{
    ALOG_INFO("✅ Starting KTLS-only server (OpenSSL 3.0+ enforced at build)");
    const auto st = worker.get_stop_token();

    auto listener_res = TlsListener::bind(worker, listener_cfg, ctx);
    if (!listener_res.has_value())
    {
        ALOG_DEBUG("Failed to create listener, error: {}", listener_res.error());
    }

    auto listener = std::move(listener_res.value());

    while (!st.stop_requested())
    {
        // Accept connection - blocks this coroutine until a client connects
        auto accept_res = co_await listener.accept();

        if (!accept_res.has_value())
        {
            ALOG_ERROR("error: {}", accept_res.error());
            continue;
        }

        auto stream = std::move(accept_res.value());
        ALOG_DEBUG("Accepted connection on endpoint {}:{}", stream.peer_ip(), stream.peer_ip());

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        handle_client(std::move(stream)).detach();
    }
    ALOG_INFO("Worker {} stop accepting connexions", worker.get_id());
}

int main()
{
    alog::configure(4096, LogLevel::Debug);

    signal(SIGPIPE, SIG_IGN);
    // Initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();

    ALOG_INFO("OpenSSL version: {}", OpenSSL_version(OPENSSL_VERSION));

    ListenerConfig listener_cfg{};
    listener_cfg.port = 8080;

    TlsConfig tls_cfg{};
    tls_cfg.cert_path = "/home/ynachi/test_certs/server.crt";
    tls_cfg.key_path = "/home/ynachi/test_certs/server.key";

    // Create SSL context (reuse for all connections!)
    auto ctx_res = TlsContext::make_server(tls_cfg);
    if (!ctx_res.has_value())
    {
        ALOG_ERROR("Failed to create Tls Context");
        ALOG_DEBUG("{}", ctx_res.error());
    }

    ALOG_INFO("🚀 KTLS-Only Server listening");
    ALOG_INFO("⚡ Zero-copy mode - maximum performance!");
    ALOG_INFO("📝 TLS 1.3 only, no fallback");

    auto ctx = std::move(ctx_res.value());

    // Configure workers
    WorkerConfig config{};
    config.uring_queue_depth = 16800;
    config.default_op_slots = 8096;

    // Create a worker pool
    IOPool pool(4, config, [&listener_cfg, &ctx](Worker& worker) { accept_loop(worker, listener_cfg, ctx).detach(); });
    Worker* worker = pool.get_worker(0);

    if (!worker)
    {
        ALOG_ERROR("Failed to get worker");
        return 1;
    }

    // Main thread waits (or handles signals)
    std::cout << "Server running. Press Enter to stop..." << std::endl;
    std::cin.get();  // Blocks until user presses Enter
    pool.stop();

    ALOG_INFO("Server stopped from main");

    // Pool destructor stops all workers gracefully
    return 0;
}
