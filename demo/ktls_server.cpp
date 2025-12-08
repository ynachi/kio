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
#include "kio/tls/stream.h"

using namespace kio;
using namespace kio::tls;
using namespace kio::io;

// =========================================================================
// EXAMPLE 1: High-Performance Echo Server (KTLS-Only)
// =========================================================================

DetachedTask handle_client(Worker& worker, int client_fd, SslContext& ctx)
{
    ALOG_INFO("New client connected (fd={})", client_fd);

    TlSStream tls(worker, client_fd, ctx);

    // Handshake - KTLS or fail
    if (auto handshake_result = co_await tls.async_handshake(); !handshake_result)
    {
        ALOG_ERROR("KTLS handshake failed, closing connection, error {}", handshake_result.error());
        co_await worker.async_close(client_fd);
        co_return;
    }

    ALOG_INFO("✅ Client ready (pure kernel TLS mode)");

    // From here on, it's pure kernel I/O!
    char buffer[8192];

    size_t total_bytes = 0;
    while (true)
    {
        auto read_result = co_await tls.async_read(buffer);
        if (!read_result)
        {
            if (read_result.error().value == kIoEof)
            {
                ALOG_INFO("Client closed connection cleanly");
            }
            else
            {
                ALOG_ERROR("Read error: {}", read_result.error());
            }
            break;
        }

        const int bytes_read = read_result.value();
        total_bytes += bytes_read;

        // Echo back (pure kernel write!)
        // TODO use write exact later
        auto write_result = co_await tls.async_write(std::span<const char>(buffer, bytes_read));

        if (!write_result)
        {
            ALOG_ERROR("Write error: {}", write_result.error());
            break;
        }
    }

    ALOG_INFO("Connection closed. Total bytes: {}", total_bytes);
    co_await tls.shutdown();
    co_await worker.async_close(client_fd);
}

// Accept loop - runs on each worker independently
DetachedTask accept_loop(Worker& worker, const int listen_fd, SslContext& ctx)
{
    ALOG_INFO("✅ Starting KTLS-only server (OpenSSL 3.0+ enforced at build)");
    const auto st = worker.get_stop_token();

    while (!st.stop_requested())
    {
        sockaddr_storage client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        // Accept connection - blocks this coroutine until a client connects
        auto client_fd = co_await worker.async_accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);

        if (!client_fd.has_value())
        {
            ALOG_ERROR("error: {}", client_fd.error());
            continue;
        }

        ALOG_DEBUG("Accepted connection on fd {}", client_fd.value());

        // Spawn coroutine to handle this client
        // Each connection runs independently on this worker
        handle_client(worker, client_fd.value(), ctx).detach();
    }
    ALOG_INFO("Worker {} stop accepting connexions", worker.get_id());
}

int main()
{
    // Initialize OpenSSL
    SSL_library_init();
    SSL_load_error_strings();

    ALOG_INFO("OpenSSL version: {}", OpenSSL_version(OPENSSL_VERSION));

    auto server_fd_exp = net::create_tcp_socket("0.0.0.0", 8080, 4096);
    if (!server_fd_exp.has_value())
    {
        ALOG_ERROR("Failed to create server socket: {}", server_fd_exp.error());
        return 1;
    }

    auto server_fd = *server_fd_exp;

    ALOG_INFO("Listening on port 8080");

    // Create SSL context (reuse for all connections!)
    SslContext ctx = create_server_context("/home/ynachi/test_certs/server.crt", "/home/ynachi/test_certs/server.key");

    ALOG_INFO("🚀 KTLS-Only Server listening");
    ALOG_INFO("⚡ Zero-copy mode - maximum performance!");
    ALOG_INFO("📝 TLS 1.3 only, no fallback");

    // Configure workers
    WorkerConfig config{};
    config.uring_queue_depth = 16800;
    config.default_op_slots = 8096;

    // Create a worker pool
    IOPool pool(4, config, [server_fd, &ctx](Worker& worker) { accept_loop(worker, server_fd, ctx).detach(); });
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
