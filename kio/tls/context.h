//
// Created by Yao ACHI on 07/12/2025.
//
#ifndef KIO_TLS_CONTEXT_H
#define KIO_TLS_CONTEXT_H

#include <filesystem>
#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <string>
#include <string_view>
#include <utility>
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

    // ALPN protocols in preference order
    // Example: {"h2", "http/1.1"} prefers HTTP/2, falls back to HTTP/1.1
    std::vector<std::string> alpn_protocols;

    // Cipher suites (empty = use OpenSSL defaults)
    // Only KTLS-compatible ciphers will work: AES-GCM, ChaCha20-Poly1305
    std::string cipher_suites;
    std::string ciphersuites_tls13;
};

/**
 * @brief Provides a TLS Context for clients and servers with automatic KTLS enablement
 *
 * KTLS REQUIREMENTS:
 * 1. OpenSSL 3.0+ (checked at compile time via KIO_HAVE_OPENSSL3)
 * 2. Kernel TLS module loaded: `sudo modprobe tls`
 * 3. KTLS-compatible cipher negotiated:
 *    - TLS 1.2: AES-128-GCM, AES-256-GCM
 *    - TLS 1.3: AES-128-GCM, AES-256-GCM, CHACHA20-POLY1305
 * 4. Kernel 6.0+ (verified at runtime)
 *
 * KTLS LIMITATIONS:
 * - Session tickets disabled on TLS 1.3 servers to prevent post-handshake messages
 * - No renegotiation support
 * - Certificate changes require new connection
 *
 * This class configures OpenSSL to automatically enable KTLS when possible.
 * KTLS enablement is mandatory - connections fail if KTLS cannot be negotiated.
 */
class TlsContext
{
    SSL_CTX* ctx_{nullptr};
    bool is_server_{true};

    // Store ALPN protocols on the heap so the address passed to OpenSSL callback
    // remains valid even if TlsContext is moved. The unique_ptr ensures the heap
    // allocation address is stable across moves.
    std::unique_ptr<std::vector<unsigned char>> alpn_protos_{nullptr};

    explicit TlsContext(SSL_CTX* ctx, bool is_server,
                        std::unique_ptr<std::vector<unsigned char>> alpn_protos = nullptr) :
        ctx_(ctx), is_server_(is_server), alpn_protos_(std::move(alpn_protos))
    {
    }

    static Result<TlsContext> Make(const TlsConfig& config, TlsRole role);

public:
    ~TlsContext()
    {
        if (ctx_ != nullptr)
        {
            SSL_CTX_free(ctx_);
        }
    }

    // Move-only semantics
    TlsContext(TlsContext&& other) noexcept :
        ctx_(std::exchange(other.ctx_, nullptr)), is_server_(other.is_server_),
        alpn_protos_(std::move(other.alpn_protos_))
    {
    }

    TlsContext& operator=(TlsContext&& other) noexcept
    {
        if (this != &other)
        {
            if (ctx_ != nullptr)
            {
                SSL_CTX_free(ctx_);
            }
            ctx_ = std::exchange(other.ctx_, nullptr);
            is_server_ = other.is_server_;
            alpn_protos_ = std::move(other.alpn_protos_);
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

    [[nodiscard]]
    bool IsServer() const noexcept
    {
        return is_server_;
    }

    /**
     * @brief Get the negotiated ALPN protocol from an SSL connection
     * @param ssl The SSL connection (must have completed handshake)
     * @return Protocol name (e.g., "h2", "http/1.1") or empty if none negotiated
     */
    static std::string_view GetNegotiatedProtocol(SSL* ssl)
    {
        if (ssl == nullptr)
        {
            return {};
        }

        const unsigned char* data = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(ssl, &data, &len);

        if (data && len > 0)
        {
            return std::string_view(reinterpret_cast<const char*>(data), len);
        }
        return {};
    }

    /**
     * @brief Creates a TLS context for clients
     * @param config TLS configurations
     * @return Result containing TlsContext or Error
     */
    static Result<TlsContext> MakeClient(const TlsConfig& config) { return Make(config, TlsRole::kClient); }

    /**
     * @brief Create a TLS context for servers (or listeners)
     * @param config TLS configurations
     * @return Result containing TlsContext or Error
     */
    static Result<TlsContext> MakeServer(const TlsConfig& config) { return Make(config, TlsRole::kServer); }
};

//
// KTLS helpers
//

/**
 * @brief Check if kernel TLS module is loaded
 * @return true if TLS kernel module is available
 */
bool IsKtlsAvailable();

/**
 * @brief Verify all KTLS requirements are met
 * @return Success or error describing what's missing
 */
Result<void> RequireKtls();

/**
 * @brief Get detailed information about KTLS availability
 * @return Multi-line string with OpenSSL version and kernel module status
 */
std::string GetKtlsInfo();

namespace detail
{
/**
 * @brief Convert TlsVersion enum to OpenSSL constant
 */
int TlsVersionToOpenssl(TlsVersion version);

/**
 * @brief Get human-readable OpenSSL error string
 * @return Error description or "Unknown Error" if no errors queued
 */
std::string GetOpensslError();

}  // namespace detail

}  // namespace kio::tls

#endif  // KIO_TLS_CONTEXT_H
