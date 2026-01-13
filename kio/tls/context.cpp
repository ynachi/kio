//
// Created by Yao ACHI on 07/12/2025.
//

#include "context.h"

#include "kio/core/async_logger.h"

#include <fstream>
#include <memory>
#include <vector>

namespace kio::tls
{
namespace detail
{
int TlsVersionToOpenssl(const TlsVersion version)
{
    switch (version)
    {
        case TlsVersion::TLS_1_2:
            return TLS1_2_VERSION;
        case TlsVersion::TLS_1_3:
            return TLS1_3_VERSION;
        default:
            return TLS1_2_VERSION;
    }
}

std::string GetOpensslError()
{
    std::string res;
    uint64_t err{0};
    while ((err = ERR_get_error()) != 0)
    {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        if (!res.empty())
        {
            res += "; ";
        }
        res += buf;
    }
    return res.empty() ? "Unknown Error" : res;
}
}  // namespace detail

// -----------------------------------------------------------------------------
// Internal Helper Functions (Refactoring)
// -----------------------------------------------------------------------------
namespace
{

Result<SSL_CTX*> CreateBaseContext(const TlsConfig& config, TlsRole role)
{
    // Initialize OpenSSL (safe to call multiple times)
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    const SSL_METHOD* method = (role == TlsRole::kServer) ? TLS_server_method() : TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (ctx == nullptr)
    {
        return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
    }

    if (!SSL_CTX_set_min_proto_version(ctx, detail::TlsVersionToOpenssl(config.min_version)))
    {
        ALOG_ERROR("Failed to set minimum TLS version: {}", detail::GetOpensslError());
        SSL_CTX_free(ctx);
        return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
    }

    return ctx;
}

Result<void> ConfigureKtls(SSL_CTX* ctx, TlsRole role)
{
#if KIO_HAVE_OPENSSL3
    SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
    ALOG_DEBUG("KTLS option enabled on SSL_CTX");

    // Disable session tickets on server to prevent post-handshake messages
    // which corrupt the stream when KTLS is active.
    if (role == TlsRole::kServer)
    {
        SSL_CTX_set_num_tickets(ctx, 0);
        ALOG_DEBUG("TLS 1.3 session tickets disabled (required for KTLS)");
    }
    return {};
#else
    ALOG_ERROR("OpenSSL 3.0+ required for KTLS support");
    return std::unexpected(Error{ErrorCategory::kTls, kTlsKtlsEnableFailed});
#endif
}

Result<void> LoadIdentity(SSL_CTX* ctx, const TlsConfig& config, TlsRole role)
{
    // Server requires cert and key. Client is optional (mTLS).
    if (role == TlsRole::kServer && (config.cert_path.empty() || config.key_path.empty()))
    {
        ALOG_ERROR("Server TLS context requires both certificate and private key");
        return std::unexpected(Error{ErrorCategory::kTls, kTlsCertificateLoadFailed});
    }

    if (!config.cert_path.empty())
    {
        if (SSL_CTX_use_certificate_chain_file(ctx, config.cert_path.c_str()) != 1)
        {
            ALOG_ERROR("Failed to load certificate from {}: {}", config.cert_path.string(), detail::GetOpensslError());
            return std::unexpected(Error{ErrorCategory::kTls, kTlsCertificateLoadFailed});
        }
        ALOG_DEBUG("Loaded certificate chain from {}", config.cert_path.string());
    }

    if (!config.key_path.empty())
    {
        if (SSL_CTX_use_PrivateKey_file(ctx, config.key_path.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            ALOG_ERROR("Failed to load private key from {}: {}", config.key_path.string(), detail::GetOpensslError());
            return std::unexpected(Error{ErrorCategory::kTls, kTlsPrivateKeyLoadFailed});
        }

        if (SSL_CTX_check_private_key(ctx) != 1)
        {
            ALOG_ERROR("Private key does not match certificate: {}", detail::GetOpensslError());
            return std::unexpected(Error{ErrorCategory::kTls, kTlsPrivateKeyLoadFailed});
        }
        ALOG_DEBUG("Loaded private key from {}", config.key_path.string());
    }

    return {};
}

Result<void> ConfigureTrust(SSL_CTX* ctx, const TlsConfig& config, TlsRole role)
{
    SSL_CTX_set_verify(ctx, config.verify_mode, nullptr);

    if (!config.ca_cert_path.empty() || !config.ca_dir_path.empty())
    {
        const char* ca_file = config.ca_cert_path.empty() ? nullptr : config.ca_cert_path.c_str();
        const char* ca_dir = config.ca_dir_path.empty() ? nullptr : config.ca_dir_path.c_str();

        if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_dir) != 1)
        {
            ALOG_ERROR("Failed to load CA certificates: {}", detail::GetOpensslError());
            return std::unexpected(Error{ErrorCategory::kTls, kTlsCertificateLoadFailed});
        }
        ALOG_DEBUG("Loaded CA certificates");
    }
    else if (role == TlsRole::kClient && config.verify_mode != SSL_VERIFY_NONE)
    {
        if (SSL_CTX_set_default_verify_paths(ctx) != 1)
        {
            ALOG_WARN("Failed to load system CA certificates: {}", detail::GetOpensslError());
        }
    }
    return {};
}

Result<void> ConfigureCiphers(SSL_CTX* ctx, const TlsConfig& config)
{
    if (!config.cipher_suites.empty())
    {
        if (SSL_CTX_set_cipher_list(ctx, config.cipher_suites.c_str()) != 1)
        {
            ALOG_ERROR("Failed to set cipher list: {}", detail::GetOpensslError());
            return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
        }
    }

    if (!config.ciphersuites_tls13.empty())
    {
        if (SSL_CTX_set_ciphersuites(ctx, config.ciphersuites_tls13.c_str()) != 1)
        {
            ALOG_ERROR("Failed to set TLS 1.3 ciphersuites: {}", detail::GetOpensslError());
            return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
        }
    }
    return {};
}

// ALPN Callback for Server
int AlpnSelectCallback(const SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                       unsigned int inlen, void* arg)
{
    (void)ssl;
    auto* protocols = static_cast<std::vector<unsigned char>*>(arg);

    if ((protocols == nullptr) || protocols->empty())
    {
        return SSL_TLSEXT_ERR_NOACK;
    }

    const int kResult = SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, protocols->data(),
                                              static_cast<unsigned int>(protocols->size()), in, inlen);

    if (kResult == OPENSSL_NPN_NEGOTIATED)
    {
        ALOG_DEBUG("ALPN negotiated: {}", std::string_view(reinterpret_cast<const char*>(*out), *outlen));
        return SSL_TLSEXT_ERR_OK;
    }

    ALOG_WARN("ALPN negotiation failed: no matching protocol");
    return SSL_TLSEXT_ERR_NOACK;
}

Result<std::unique_ptr<std::vector<unsigned char>>> ConfigureAlpn(SSL_CTX* ctx, const TlsConfig& config, TlsRole role)
{
    if (config.alpn_protocols.empty())
    {
        return nullptr;
    }

    auto protos = std::make_unique<std::vector<unsigned char>>();
    for (const auto& protocol : config.alpn_protocols)
    {
        if (protocol.empty() || protocol.size() > 255)
        {
            ALOG_ERROR("Invalid ALPN protocol: '{}' (length must be 1-255 bytes)", protocol);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
        }
        protos->push_back(static_cast<unsigned char>(protocol.length()));
        protos->insert(protos->end(), protocol.begin(), protocol.end());
    }

    if (role == TlsRole::kServer)
    {
        // The callback receives raw pointer to the vector.
        // The unique_ptr will be stored in TlsContext, ensuring the address stays valid.
        SSL_CTX_set_alpn_select_cb(ctx, reinterpret_cast<SSL_CTX_alpn_select_cb_func>(AlpnSelectCallback),
                                   protos.get());
        ALOG_DEBUG("ALPN server callback configured with {} protocols", config.alpn_protocols.size());
    }
    else
    {
        // For client, we just set the protos directly (OpenSSL copies them)
        if (SSL_CTX_set_alpn_protos(ctx, protos->data(), static_cast<unsigned int>(protos->size())) != 0)
        {
            ALOG_ERROR("Failed to set ALPN protocols: {}", detail::GetOpensslError());
            return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
        }
        ALOG_DEBUG("ALPN client protocols configured: {} protocols", config.alpn_protocols.size());
    }

    return protos;
}

}  // namespace

// -----------------------------------------------------------------------------
// TlsContext Implementation
// -----------------------------------------------------------------------------

Result<TlsContext> TlsContext::Make(const TlsConfig& config, const TlsRole role)
{
    // 1. Create Base Context
    auto ctx_res = CreateBaseContext(config, role);
    if (!ctx_res)
    {
        return std::unexpected(ctx_res.error());
    }
    SSL_CTX* ctx = ctx_res.value();

    // RAII cleaner in case of early return failure inside Make
    auto ctx_guard = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>(ctx, SSL_CTX_free);

    // 2. Configure KTLS
    if (auto res = ConfigureKtls(ctx, role); !res)
    {
        return std::unexpected(res.error());
    }

    // 3. Configure Identity (Cert/Key)
    if (auto res = LoadIdentity(ctx, config, role); !res)
    {
        return std::unexpected(res.error());
    }

    // 4. Configure Trust (CA)
    if (auto res = ConfigureTrust(ctx, config, role); !res)
    {
        return std::unexpected(res.error());
    }

    // 5. Configure Ciphers
    if (auto res = ConfigureCiphers(ctx, config); !res)
    {
        return std::unexpected(res.error());
    }

    // 6. Configure ALPN
    auto alpn_res = ConfigureAlpn(ctx, config, role);
    if (!alpn_res)
    {
        return std::unexpected(alpn_res.error());
    }

    // Success - Release guard and return object
    ctx_guard.release();
    return TlsContext{ctx, role == TlsRole::kServer, std::move(alpn_res.value())};
}

bool IsKtlsAvailable()
{
    // Method 1: Check /proc/modules
    if (std::ifstream modules("/proc/modules"); modules.is_open())
    {
        std::string line;
        while (std::getline(modules, line))
        {
            if (line.contains("tls"))
            {
                return true;
            }
        }
    }

    // Method 2: Check module state directly
    if (std::ifstream tls_module("/sys/module/tls/initstate"); tls_module.is_open())
    {
        std::string state;
        tls_module >> state;
        return state == "live";
    }

    return false;
}

Result<void> RequireKtls()
{
#if !KIO_HAVE_OPENSSL3
    ALOG_ERROR("KTLS requires OpenSSL 3.0+, found: {}", OpenSSL_version(OPENSSL_VERSION));
    return std::unexpected(Error{ErrorCategory::kTls, kTlsKtlsEnableFailed});
#endif

    if (!IsKtlsAvailable())
    {
        ALOG_ERROR("Kernel TLS module not loaded. Run: sudo modprobe tls");
        return std::unexpected(Error{ErrorCategory::kTls, kTlsKtlsEnableFailed});
    }

    return {};
}

std::string GetKtlsInfo()
{
    std::string info;
    info += "OpenSSL: ";
    info += OpenSSL_version(OPENSSL_VERSION);
    info += "\n";

#if KIO_HAVE_OPENSSL3
    info += "OpenSSL 3.0+ KTLS support: Yes\n";
#else
    info += "OpenSSL 3.0+ KTLS support: No (FATAL: version too old)\n";
#endif

    info +=
        IsKtlsAvailable() ? "Kernel TLS module: Loaded\n" : "Kernel TLS module: NOT LOADED (run: sudo modprobe tls)\n";
    return info;
}

}  // namespace kio::tls