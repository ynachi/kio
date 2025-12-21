//
// Created by Yao ACHI on 07/12/2025.
//
#ifndef KIO_TLS_CONTEXT_H
#define KIO_TLS_CONTEXT_H

#include <filesystem>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <string>
#include <vector>

#include "kio/core/errors.h"

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
enum class TlsVersion : uint8_t
{
    TLS_1_2,  // NOLINT
    TLS_1_3  // NOLINT
};
enum class TlsRole : uint8_t
{
    kClient,
    kServer
};

struct TlsConfig
{
    // Identity required for Server
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    // Trust required for Client
    std::filesystem::path ca_cert_path;
    std::filesystem::path ca_dir_path;

    TlsVersion min_version{TlsVersion::TLS_1_2};
    bool verify_hostname{true};
    int verify_mode{SSL_VERIFY_PEER};
    std::vector<std::string> alpn_protocols;
    // Cipher suites (empty = use OpenSSL defaults)
    // Only KTLS-compatible ciphers will work: AES-GCM, ChaCha20-Poly1305
    std::string cipher_suites;
    std::string ciphersuites_tls13;
    bool enable_session_cache{true};
    // SNI
    std::string server_name;
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
    static Result<TlsContext> Make(const TlsConfig& config, TlsRole role);

public:
    ~TlsContext()
    {
        if (ctx_ != nullptr)
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
            if (ctx_ != nullptr)
            {
                SSL_CTX_free(ctx_);
            }
            ctx_ = std::exchange(other.ctx_, nullptr);
        }
        return *this;
    }

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    [[nodiscard]]
    SSL_CTX* Get() const noexcept
    {
        return ctx_;
    }

    /**
     * Creates a TLS context for clients
     * @param config TLS configurations
     * @return returns a KIO results containing a tls context
     */
    static Result<TlsContext> MakeClient(const TlsConfig& config) { return Make(config, TlsRole::kClient); }

    /**
     * Create a TLS context for servers (or listeners).
     * @param config TLS configurations
     * @return returns a KIO results containing a tls context
     */
    static Result<TlsContext> MakeServer(const TlsConfig& config) { return Make(config, TlsRole::kServer); }
};

//
// KTLS helpers
//
bool IsKtlsAvailable();
Result<void> RequireKtls();
std::string GetKtlsInfo();

namespace detail
{
int TlsVersionToOpenssl(TlsVersion version);
std::string GetOpensslError();
}  // namespace detail

}  // namespace kio::tls

#endif  // KIO_TLS_CONTEXT_H
