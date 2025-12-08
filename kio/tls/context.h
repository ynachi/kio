//
// Created by Yao ACHI on 07/12/2025.
//
#ifndef KIO_TLS_CONTEXT_H
#define KIO_TLS_CONTEXT_H

#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <string>
#include <string_view>

#include "kio/core/coro.h"
#include "kio/core/errors.h"
#include "kio/core/worker.h"

namespace kio::tls
{
    // -------------------------------------------------------------------------
    // Error Codes
    // -------------------------------------------------------------------------

    constexpr int kTlsKtlsNotSupported = 5000;
    constexpr int kTlsKtlsNotEnabled = 5001;
    constexpr int kTlsHandshakeFailed = 5002;


    /**
     * @brief RAII wrapper for SSL_CTX with automatic KTLS enablement
     *
     * This class configures OpenSSL to automatically enable KTLS when possible.
     * No manual key extraction or kernel calls are needed!
     */
    class SslContext
    {
        SSL_CTX* ctx_{nullptr};
        bool is_server_{false};

    public:
        /**
         * @brief Create SSL context for server or client
         *
         * @param is_server true for a server, false for a client
         * @param cert_path Path to a certificate file (server only)
         * @param key_path Path to a private key file (server only)
         */
        explicit SslContext(bool is_server, const std::string& cert_path = "", const std::string& key_path = "");

        ~SslContext()
        {
            if (ctx_)
            {
                SSL_CTX_free(ctx_);
            }
        }

        // Move-only
        SslContext(SslContext&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

        SslContext& operator=(SslContext&& other) noexcept
        {
            if (this != &other)
            {
                if (ctx_) SSL_CTX_free(ctx_);
                ctx_ = std::exchange(other.ctx_, nullptr);
            }
            return *this;
        }

        SslContext(const SslContext&) = delete;
        SslContext& operator=(const SslContext&) = delete;

        [[nodiscard]]
        SSL_CTX* get() const noexcept
        {
            return ctx_;
        }

        [[nodiscard]]
        bool is_server() const noexcept
        {
            return is_server_;
        }

        /**
         * @brief Enable/disable certificate verification
         */
        void set_verify_peer(const bool verify) const { SSL_CTX_set_verify(ctx_, verify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr); }
    };


    /**
     * @brief Create SSL context for servers
     */
    inline SslContext create_server_context(const std::string& cert_path, const std::string& key_path) { return SslContext(true, cert_path, key_path); }

    /**
     * @brief Create SSL context for clients
     * @param ca_path Optional path to CA certificate file for verification
     */
    SslContext create_client_context(const std::string& ca_path = "");

}  // namespace kio::tls

#endif  // KIO_TLS_CONTEXT_H
