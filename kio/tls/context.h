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

#include "kio/core/coro.h"
#include "kio/core/errors.h"
#include "kio/core/worker.h"

// OpenSSL 3.0+ KTLS support
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#define KIO_HAVE_OPENSSL3 1
#else
#define KIO_HAVE_OPENSSL3 0
#endif

namespace kio::tls
{
// -------------------------------------------------------------------------
// Enums & Config
// -------------------------------------------------------------------------
enum class TlsVersion
{
    TLS_1_2,
    TLS_1_3
};
enum class TlsRole
{
    Client,
    Server
};

struct TlsConfig
{
    // Identity required for Server
    std::filesystem::path cert_path{};
    std::filesystem::path key_path{};

    // Trust required for Client
    std::filesystem::path ca_cert_path{};
    std::filesystem::path ca_dir_path{};

    TlsVersion min_version{TlsVersion::TLS_1_2};
    bool verify_hostname{true};
    int verify_mode{SSL_VERIFY_PEER};
    std::vector<std::string> alpn_protocols{};
    // Cipher suites (empty = use OpenSSL defaults)
    // Only KTLS-compatible ciphers will work: AES-GCM, ChaCha20-Poly1305
    std::string cipher_suites{};
    std::string ciphersuites_tls13{};
    bool enable_session_cache{true};
    // SNI
    std::string server_name{};
};

/**
 * @brief Provides a TLS Context for client and servers automatic KTLS enablement
 *
 * This class configures OpenSSL to automatically enable KTLS when possible.
 */
class TlsContext
{
    SSL_CTX* ctx_{nullptr};
    bool is_server_{true};
    explicit TlsContext(SSL_CTX* ctx, const bool is_server = true) : ctx_(ctx), is_server_(is_server) {}
    static Result<TlsContext> make(const TlsConfig& config, TlsRole role);

public:
    ~TlsContext()
    {
        if (ctx_)
        {
            SSL_CTX_free(ctx_);
        }
    }

    // Move-only
    TlsContext(TlsContext&& other) noexcept : ctx_(std::exchange(other.ctx_, nullptr)) {}

    TlsContext& operator=(TlsContext&& other) noexcept
    {
        if (this != &other)
        {
            if (ctx_) SSL_CTX_free(ctx_);
            ctx_ = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    [[nodiscard]]
    SSL_CTX* get() const noexcept
    {
        return ctx_;
    }

    /**
     * Creates a TLS context for clients
     * @param config TLS configurations
     * @return returns a KIO results containing a tls context
     */
    static Result<TlsContext> make_client(const TlsConfig& config) { return make(config, TlsRole::Client); }

    /**
     * Create a TLS context for servers (or listeners).
     * @param config TLS configurations
     * @return returns a KIO results containing a tls context
     */
    static Result<TlsContext> make_server(const TlsConfig& config) { return make(config, TlsRole::Server); }
};

//
// KTLS helpers
//
bool is_ktls_available();
Result<void> require_ktls();
std::string get_ktls_info();

namespace detail
{
int tls_version_to_openssl(TlsVersion version);
std::string get_openssl_error();
}  // namespace detail

}  // namespace kio::tls

#endif  // KIO_TLS_CONTEXT_H
