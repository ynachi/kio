#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "aio/result.hpp"
#include <openssl/ssl.h>

// OpenSSL 3.0+ KTLS only support
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    #define KIO_HAVE_OPENSSL3 1
#else
    #define KIO_HAVE_OPENSSL3 0
#endif

namespace aio::tls
{

namespace detail
{
bool HaveKtls();
std::string GetKtlsInfo();
}  // namespace detail

static const std::string kDefaultTls1_2Ciphers =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

static const std::string kDefaultTls1_3Ciphers =
    "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256";

struct TlsConfig
{
    // Identity required for Server (and Client if mTLS)
    std::filesystem::path cert_path;
    std::filesystem::path key_path;

    // Trust required for Client (and Server if mTLS)
    std::filesystem::path ca_cert_path;
    std::filesystem::path ca_dir_path;

    bool verify_hostname{true};
    int verify_mode{SSL_VERIFY_PEER};

    // ALPN protocols in preference order
    // Example: {"h2", "http/1.1"} prefers HTTP/2, falls back to HTTP/1.1
    std::vector<std::string> alpn_protocols;

    // Cipher suites (empty = use OpenSSL defaults)
    // Only KTLS-compatible ciphers will work: AES-GCM, ChaCha20-Poly1305
    std::string cipher_suites = kDefaultTls1_2Ciphers;
    std::string ciphersuites_tls13 = kDefaultTls1_3Ciphers;
};

class TlsContext
{
public:
    struct SSLCtxDeleter
    {
        void operator()(SSL_CTX* p) const noexcept { SSL_CTX_free(p); }
    };
    using SSLCtxPtr = std::unique_ptr<SSL_CTX, SSLCtxDeleter>;

private:
    SSLCtxPtr ctx_;

    // Stable storage for Server ALPN data.
    // Must be unique_ptr so the address of the vector on the heap    // remains valid even if TlsContext is moved after
    // creation.
    std::unique_ptr<std::vector<unsigned char>> alpn_store_;

    // Static ALPN Callback for Server mode
    static int AlpnSelectCb(SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                            unsigned int inlen, void* arg);

    // Constructor taking ownership of both Context and ALPN storage
    explicit TlsContext(SSLCtxPtr&& ctx, std::unique_ptr<std::vector<unsigned char>>&& alpn_store = nullptr)
        : ctx_(std::move(ctx)), alpn_store_(std::move(alpn_store))
    {
    }

public:
    // Main factory function
    static Result<TlsContext> Create(const TlsConfig& config, bool server_mode = true);

    // Accessor
    [[nodiscard]] SSL_CTX* NativeHandle() const { return ctx_.get(); }
};

}  // namespace aio::tls