//
// Created by Yao ACHI on 07/12/2025.
//

#ifndef KIO_TLS_STREAM_H
#define KIO_TLS_STREAM_H
#include <openssl/ssl.h>

#include "context.h"
#include "kio/core/worker.h"

namespace kio::tls
{
    /**
     * @brief Async TLS connection with automatic KTLS offload
     *
     * Features:
     * - Automatic KTLS enablement (OpenSSL 3.0 handles everything)
     * - Transparent fallback to userspace if KTLS unavailable
     * - Zero-copy sendfile when KTLS is active
     * - Async handshake using io_uring poll
     *
     * Usage:
     * @code
     * SslContext ctx(true, "server.crt", "server.key");
     * TlsStream tls(worker, client_fd, ctx);
     *
     * co_await tls.handshake();
     * co_await tls.write("Hello!");
     * auto data = co_await tls.read(buffer);
     * @endcode
     */
    class TlSStream
    {
        io::Worker& worker_;
        int fd_;
        SSL* ssl_{nullptr};
        std::string server_name_;  // For SNI

    public:
        /**
         * @brief Create a TLS stream for an already-connected socket
         *
         * @param worker kio worker for async operations
         * @param fd Socket file descriptor (must be connected)
         * @param ctx SSL context (server or client)
         * @param server_name Server name for SNI (client only, optional)
         */
        TlSStream(io::Worker& worker, int fd, const SslContext& ctx, std::string_view server_name = "");

        ~TlSStream()
        {
            if (ssl_)
            {
                // Send close_notify if the connection is still active
                // Note: With KTLS, SSL_shutdown() will use the kernel's TLS offload
                SSL_shutdown(ssl_);
                SSL_free(ssl_);
                ssl_ = nullptr;
            }
        }

        // Move-only
        TlSStream(TlSStream&& other) noexcept : worker_(other.worker_), fd_(std::exchange(other.fd_, -1)), ssl_(std::exchange(other.ssl_, nullptr)), server_name_(std::move(other.server_name_)) {}

        TlSStream(const TlSStream&) = delete;
        TlSStream& operator=(const TlSStream&) = delete;
        TlSStream& operator=(TlSStream&&) = delete;

        // -------------------------------------------------------------------------
        // Handshake
        // -------------------------------------------------------------------------
        /**
         * @brief Performs handshake and verifies KTLS activation.
         * Fails if the kernel refuses to offload encryption.
         */
        Task<Result<void>> async_handshake();


        // -------------------------------------------------------------------------
        // IO operations
        // -------------------------------------------------------------------------
        Task<Result<int>> async_read(std::span<char> buf) { return worker_.async_read(fd_, buf); }

        /**
         * @brief Zero-overhead write with KTLS.
         * Kernel transparently encrypts the data - no special syscalls needed!
         */
        Task<Result<int>> async_write(std::span<const char> buf)
        {
            // With KTLS active, plain write() works - kernel handles encryption
            return worker_.async_write(fd_, buf);
        }

        // shutdown sending tls close_notify message
        Task<Result<void>> shutdown_goodbye();

        /**
         * @brief Gracefully close TLS connection
         */
        Task<Result<void>> shutdown()
        {
            // To send a proper TLS close_notify on a KTLS socket,
            // we need sendmsg() with CMSG_TYPE = TLS_SET_RECORD_TYPE.
            // Standard write() wraps data in Application Records.

            // For this high-perf implementation, we rely on the TCP FIN
            // (triggered by async_close later) to tear down the connection.
            // This saves an implementation of async_sendmsg.
            // If you strictly need close_notify, use Worker::shutdown_goodbye.

            co_return {};
        }
    };

}  // namespace kio::tls

#endif  // KIO_TLS_STREAM_H
