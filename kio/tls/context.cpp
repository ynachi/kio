//
// Created by Yao ACHI on 07/12/2025.
//

#include "context.h"

#include <fstream>
#include <memory>

#include "kio/core/async_logger.h"

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
    unsigned long err{0};
    while ((err = ERR_get_error()) != 0)
    {
        char buf[256];
        ERR_error_string_n(err, buf, sizeof(buf));
        if (!res.empty()) res += "; ";
        res += buf;
    }
    return res.empty() ? "Unknown Error" : res;
}
}  // namespace detail

Result<TlsContext> TlsContext::Make(const TlsConfig& config, const TlsRole role)
{
    // Initialize OpenSSL
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

    const SSL_METHOD* method = (role == TlsRole::kServer) ? TLS_server_method() : TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (ctx == nullptr)
    {
        return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
    }

    // Set minimum TLS version
    if (!SSL_CTX_set_min_proto_version(ctx, detail::TlsVersionToOpenssl(config.min_version)))
    {
        ALOG_ERROR("Failed to set minimum TLS version: {}", detail::GetOpensslError());
        SSL_CTX_free(ctx);
        return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
    }

    // KTLS Enable - mandatory for kio
#if KIO_HAVE_OPENSSL3
    SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
    ALOG_DEBUG("KTLS option enabled on SSL_CTX");
#else
    ALOG_ERROR("OpenSSL 3.0+ required for KTLS support");
    SSL_CTX_free(ctx);
    return std::unexpected(Error{ErrorCategory::kTls, kTlsKtlsEnableFailed});
#endif

    // =========================================================================
    // FOR KTLS + TLS 1.3:
    // Disable session tickets to prevent post-handshake messages.
    //
    // In TLS 1.3, servers send NewSessionTicket messages AFTER the handshake
    // completes. With KTLS, after we enable kernel TLS offload, any raw read
    // will receive these tickets as corrupted data (EIO error).
    //
    // Disabling tickets prevents this issue. Session resumption will still
    // work via session IDs (TLS 1.2) or other mechanisms if configured.
    // =========================================================================
    if (role == TlsRole::kServer)
    {
        SSL_CTX_set_num_tickets(ctx, 0);
        ALOG_DEBUG("TLS 1.3 session tickets disabled (required for KTLS)");
    }

    // Load certificate chain (required for server, optional for client mutual TLS)
    if (!config.cert_path.empty())
    {
        if (SSL_CTX_use_certificate_chain_file(ctx, config.cert_path.c_str()) != 1)
        {
            ALOG_ERROR("Failed to load certificate from {}: {}", config.cert_path.string(), detail::GetOpensslError());
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsCertificateLoadFailed});
        }
        ALOG_DEBUG("Loaded certificate chain from {}", config.cert_path.string());
    }

    // Load private key
    if (!config.key_path.empty())
    {
        if (SSL_CTX_use_PrivateKey_file(ctx, config.key_path.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            ALOG_ERROR("Failed to load private key from {}: {}", config.key_path.string(), detail::GetOpensslError());
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsPrivateKeyLoadFailed});
        }

        if (SSL_CTX_check_private_key(ctx) != 1)
        {
            ALOG_ERROR("Private key does not match certificate: {}", detail::GetOpensslError());
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsPrivateKeyLoadFailed});
        }
        ALOG_DEBUG("Loaded private key from {}", config.key_path.string());
    }

    // Validate server has both cert and key
    if (role == TlsRole::kServer)
    {
        if (config.cert_path.empty() || config.key_path.empty())
        {
            ALOG_ERROR("Server TLS context requires both certificate and private key");
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsCertificateLoadFailed});
        }
    }

    // Load CA certificates
    if (!config.ca_cert_path.empty() || !config.ca_dir_path.empty())
    {
        const char* ca_file = config.ca_cert_path.empty() ? nullptr : config.ca_cert_path.c_str();
        const char* ca_dir = config.ca_dir_path.empty() ? nullptr : config.ca_dir_path.c_str();

        if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_dir) != 1)
        {
            ALOG_ERROR("Failed to load CA certificates: {}", detail::GetOpensslError());
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsCertificateLoadFailed});
        }
        ALOG_DEBUG("Loaded CA certificates");
    }
    else if (role == TlsRole::kClient && config.verify_mode != SSL_VERIFY_NONE)
    {
        // Use system CA store for clients
        if (SSL_CTX_set_default_verify_paths(ctx) != 1)
        {
            ALOG_WARN("Failed to load system CA certificates: {}", detail::GetOpensslError());
        }
    }

    // Verification
    SSL_CTX_set_verify(ctx, config.verify_mode, nullptr);

    // Cipher suites
    if (!config.cipher_suites.empty())
    {
        if (SSL_CTX_set_cipher_list(ctx, config.cipher_suites.c_str()) != 1)
        {
            ALOG_ERROR("Failed to set cipher list: {}", detail::GetOpensslError());
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
        }
    }

    if (!config.ciphersuites_tls13.empty())
    {
        if (SSL_CTX_set_ciphersuites(ctx, config.ciphersuites_tls13.c_str()) != 1)
        {
            ALOG_ERROR("Failed to set TLS 1.3 ciphersuites: {}", detail::GetOpensslError());
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
        }
    }

    // =========================================================================
    // ALPN Configuration
    // Handles both client (advertise protocols) and server (select protocol)
    // =========================================================================
    std::unique_ptr<std::vector<unsigned char>> alpn_storage = nullptr;

    if (!config.alpn_protocols.empty())
    {
        auto protos = std::make_unique<std::vector<unsigned char>>();

        // Build wire format: each protocol is length-prefixed
        // Example: ["h2", "http/1.1"] becomes:
        // [0x02, 'h', '2', 0x08, 'h', 't', 't', 'p', '/', '1', '.', '1']
        for (const auto& protocol: config.alpn_protocols)
        {
            // Validate protocol length (ALPN protocol names must be 1-255 bytes)
            if (protocol.empty() || protocol.size() > 255)
            {
                ALOG_ERROR("Invalid ALPN protocol: '{}' (length must be 1-255 bytes)", protocol);
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
            }

            // Add length prefix
            protos->push_back(static_cast<unsigned char>(protocol.length()));
            // Add protocol string
            protos->insert(protos->end(), protocol.begin(), protocol.end());
        }

        if (role == TlsRole::kServer)
        {
            // Server: Install callback to select protocol during handshake
            // The callback receives a pointer to our protocols vector (protos.get())
            // This pointer stays valid because we move the unique_ptr into TlsContext below,
            // and the heap allocation address never changes even when TlsContext is moved.
            SSL_CTX_set_alpn_select_cb(
                    ctx,
                    [](SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                       unsigned int inlen, void* arg) -> int
                    {
                        (void) ssl;  // Unused but required by OpenSSL API

                        auto* protocols = static_cast<std::vector<unsigned char>*>(arg);

                        // Sanity check (should never happen if constructed properly)
                        if (!protocols || protocols->empty())
                        {
                            ALOG_WARN("ALPN callback invoked but no server protocols configured");
                            return SSL_TLSEXT_ERR_NOACK;
                        }

                        // Use OpenSSL's helper function to find the first match
                        // It iterates through the client's protocol list and finds the first
                        // protocol that also appears in the server's list (RFC 7301 compliant)
                        const int result = SSL_select_next_proto(
                                const_cast<unsigned char**>(out),  // Output: selected protocol
                                outlen,  // Output: selected protocol length
                                protocols->data(),  // Server's supported protocols
                                static_cast<unsigned int>(protocols->size()),  // Server list length
                                in,  // Client's advertised protocols
                                inlen  // Client list length
                        );

                        if (result == OPENSSL_NPN_NEGOTIATED)
                        {
                            // Successfully negotiated a protocol
                            std::string_view selected(reinterpret_cast<const char*>(*out), *outlen);
                            ALOG_DEBUG("ALPN negotiated: {}", selected);
                            return SSL_TLSEXT_ERR_OK;
                        }

                        // No matching protocol found between client and server
                        ALOG_WARN("ALPN negotiation failed: no matching protocol");

                        // Return NOACK to continue connection without ALPN (lenient mode)
                        // Use SSL_TLSEXT_ERR_ALERT_FATAL for strict mode (abort handshake)
                        return SSL_TLSEXT_ERR_NOACK;
                    },
                    protos.get()  // Pass raw pointer - remains valid after move below
            );
            ALOG_DEBUG("ALPN server callback configured with {} protocols", config.alpn_protocols.size());
        }
        else  // Client
        {
            // Client: Advertise supported protocols in ClientHello
            // OpenSSL copies this data internally, so we don't strictly need to persist it,
            // but we do for consistency with the server-side storage approach.
            //
            // NOTE: SSL_CTX_set_alpn_protos returns 0 on SUCCESS (opposite of most OpenSSL functions!)
            if (SSL_CTX_set_alpn_protos(ctx, protos->data(), static_cast<unsigned int>(protos->size())) != 0)
            {
                ALOG_ERROR("Failed to set ALPN protocols: {}", detail::GetOpensslError());
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::kTls, kTlsContextCreationFailed});
            }

            ALOG_DEBUG("ALPN client protocols configured: {} protocols", config.alpn_protocols.size());
        }

        // Move the heap-allocated vector into storage
        // The pointer passed to the server callback (protos.get()) remains valid
        // because the heap allocation address is stable - only the unique_ptr moves
        alpn_storage = std::move(protos);
    }

    return TlsContext{ctx, role == TlsRole::kServer, std::move(alpn_storage)};
}

bool IsKtlsAvailable()
{
    // Check if TLS kernel module is loaded
    // Method 1: Check /proc/modules
    if (std::ifstream modules("/proc/modules"); modules.is_open())
    {
        std::string line;
        while (std::getline(modules, line))
        {
            if (line.find("tls") != std::string::npos)
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

    if (IsKtlsAvailable())
    {
        info += "Kernel TLS module: Loaded\n";
    }
    else
    {
        info += "Kernel TLS module: NOT LOADED (run: sudo modprobe tls)\n";
    }

    return info;
}
}  // namespace kio::tls
