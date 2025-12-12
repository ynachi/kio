//
// Created by Yao ACHI on 07/12/2025.
//

#ifndef KIO_TLS_STREAM_H
#define KIO_TLS_STREAM_H
#include <openssl/ssl.h>
#include <string_view>

#include "context.h"
#include "kio/core/worker.h"
#include "kio/net/socket.h"

namespace kio::tls
{
    /**
     * @brief Async TLS connection with mandatory KTLS offload
     *
     * Features:
     * - Automatic KTLS enablement (OpenSSL 3.0 handles everything)
     * - KTLS is mandatory - connection fails if KTLS cannot be enabled
     * - Zero-copy sendfile when KTLS is active
     * - Async handshake and shutdown using io_uring poll
     *
     * Usage:
     * @code
     * auto ctx_res = TlsContext::make_server(config);
     * TlsStream stream(worker, std::move(socket), *ctx_res, TlsRole::Server);
     *
     * co_await stream.async_handshake();
     * co_await stream.async_write("Hello!");
     * auto n = co_await stream.async_read(buffer);
     * co_await stream.async_close();  // Clean shutdown
     * @endcode
     *
     * @note KTLS Requirement: This stream requires KTLS to be successfully
     * negotiated. Ensure:
     * - OpenSSL 3.0+ is installed
     * - Kernel TLS module is loaded (`sudo modprobe tls`)
     * - A KTLS-compatible cipher is used (AES-GCM or ChaCha20-Poly1305)
     */
    class TlsStream
    {
        io::Worker& worker_;
        TlsContext& ctx_;
        net::Socket socket_;
        SSL* ssl_{nullptr};
        TlsRole role_;
        std::string server_name_;  // For SNI
        net::SocketAddress peer_addr_{};

        bool handshake_done_{false};
        bool ktls_active_{false};

        Result<void> enable_ktls();
        [[nodiscard]] Task<Result<void>> do_handshake_step() const;
        [[nodiscard]] Task<Result<void>> do_shutdown_step();


    public:
        // Takes ownership of the socket
        TlsStream(io::Worker& worker, net::Socket socket, TlsContext& context, TlsRole role);

        ~TlsStream()
        {
            if (ssl_)
            {
                if (handshake_done_)
                {
                    // Mark as already shut down to prevent SSL_free from trying
                    // to send close_notify (which would block)
                    SSL_set_shutdown(ssl_, SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
                }
                SSL_free(ssl_);
                ssl_ = nullptr;
            }
            ALOG_DEBUG("TlsStream destroyed for fd={}", socket_.get());
        }

        // Move-only, no move assignable because Worker& and TlsContext& are references
        TlsStream(TlsStream&&) noexcept;
        TlsStream& operator=(TlsStream&&) = delete;


        /**
         * @brief Perform async TLS handshake
         * @param hostname Optional hostname for SNI (client-side)
         * @return Result<void> - fails if handshake or KTLS negotiation fails
         */
        Task<Result<void>> async_handshake(std::string_view hostname = {});

        /**
         * @brief Async read using kernel TLS
         * @param buf Buffer to read into
         * @return Number of bytes read, or error
         */
        Task<Result<int>> async_read(std::span<char> buf);

        /**
         * @brief Async write using kernel TLS
         * @param buf Buffer to write from
         * @return Number of bytes written, or error
         */
        Task<Result<int>> async_write(std::span<const char> buf);

        /**
         * @brief Async write entire buffer using kernel TLS
         * @param buf Buffer to write completely
         * @return void on success, error on failure
         */
        Task<Result<void>> async_write_exact(std::span<const char> buf);

        /**
         * @brief Async sendfile using kernel TLS
         * @param in_fd Source file descriptor
         * @param offset Offset in a source file
         * @param count Number of bytes to send
         * @return void on success, error on failure
         */
        Task<Result<void>> async_sendfile(const int in_fd, const off_t offset, const size_t count) { return worker_.async_sendfile(socket_.get(), in_fd, offset, count); }

        /**
         * @brief Perform clean TLS shutdown (sends close_notify)
         *
         * This performs a proper bidirectional TLS shutdown:
         * 1. Sends close_notify to peer
         * 2. Waits for peer's close_notify (with timeout protection)
         *
         * @return void on success (including if peer already closed)
         */
        [[nodiscard]] Task<Result<void>> async_shutdown();

        /**
         * @brief Shutdown TLS and close the underlying socket
         *
         * This is the recommended way to close a TlsStream. It:
         * 1. Performs TLS shutdown (best effort)
         * 2. Closes the underlying socket
         *
         * @return void (shutdown errors are logged but don't fail the close)
         */
        Task<Result<void>> async_close();

        // Connection info
        [[nodiscard]] bool is_ktls_active() const;
        [[nodiscard]] std::string_view get_cipher() const;
        [[nodiscard]] std::string_view get_version() const;
        [[nodiscard]] bool is_handshake_done() const { return handshake_done_; }
        [[nodiscard]] int fd() const { return socket_.get(); }

        // Peer address
        void set_peer_addr(const net::SocketAddress&& addr) { peer_addr_ = addr; }
        [[nodiscard]] std::string peer_ip() const { return peer_addr_.ip; }
        [[nodiscard]] uint16_t peer_port() const { return peer_addr_.port; }
        [[nodiscard]] const net::SocketAddress& peer_addr() const { return peer_addr_; }
    };

}  // namespace kio::tls

#endif  // KIO_TLS_STREAM_H