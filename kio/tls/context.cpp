//
// Created by Yao ACHI on 07/12/2025.
//

#include "context.h"

#include <fstream>

namespace kio::tls
{
    namespace detail
    {
        int tls_version_to_openssl(const TlsVersion version)
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

        std::string get_openssl_error()
        {
            std::string res;
            unsigned long err;
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


    Result<TlsContext> TlsContext::make(const TlsConfig& config, const TlsRole role)
    {
        // Initialize OpenSSL
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);

        const SSL_METHOD* method = (role == TlsRole::Server) ? TLS_server_method() : TLS_client_method();
        SSL_CTX* ctx = SSL_CTX_new(method);
        if (ctx == nullptr) return std::unexpected(Error{ErrorCategory::Tls, kTlsContextCreationFailed});

        // Set minimum TLS version
        if (!SSL_CTX_set_min_proto_version(ctx, detail::tls_version_to_openssl(config.min_version)))
        {
            ALOG_ERROR("Failed to set minimum TLS version: {}", detail::get_openssl_error());
            SSL_CTX_free(ctx);
            return std::unexpected(Error{ErrorCategory::Tls, kTlsContextCreationFailed});
        }

        // KTLS Enable
        // Enable KTLS - mandatory for kio
#if KIO_HAVE_OPENSSL3
        SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
        ALOG_DEBUG("KTLS option enabled on SSL_CTX");
#else
        ALOG_ERROR("OpenSSL 3.0+ required for KTLS support");
        SSL_CTX_free(ctx);
        return std::unexpected(Error{ErrorCategory::Tls, kTlsKtlsEnableFailed});
#endif

        // Load certificate chain (required for server, optional for client mutual TLS)
        if (!config.cert_path.empty())
        {
            if (SSL_CTX_use_certificate_chain_file(ctx, config.cert_path.c_str()) != 1)
            {
                ALOG_ERROR("Failed to load certificate from {}: {}", config.cert_path.string(), detail::get_openssl_error());
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::Tls, kTlsCertificateLoadFailed});
            }
            ALOG_DEBUG("Loaded certificate chain from {}", config.cert_path.string());
        }

        // Load private key
        if (!config.key_path.empty())
        {
            if (SSL_CTX_use_PrivateKey_file(ctx, config.key_path.c_str(), SSL_FILETYPE_PEM) != 1)
            {
                ALOG_ERROR("Failed to load private key from {}: {}", config.key_path.string(), detail::get_openssl_error());
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::Tls, kTlsPrivateKeyLoadFailed});
            }

            if (SSL_CTX_check_private_key(ctx) != 1)
            {
                ALOG_ERROR("Private key does not match certificate: {}", detail::get_openssl_error());
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::Tls, kTlsPrivateKeyLoadFailed});
            }
            ALOG_DEBUG("Loaded private key from {}", config.key_path.string());
        }

        // Validate server has both cert and key
        if (role == TlsRole::Server)
        {
            if (config.cert_path.empty() || config.key_path.empty())
            {
                ALOG_ERROR("Server TLS context requires both certificate and private key");
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::Tls, kTlsCertificateLoadFailed});
            }
        }

        // Load CA certificates
        if (!config.ca_cert_path.empty() || !config.ca_dir_path.empty())
        {
            const char* ca_file = config.ca_cert_path.empty() ? nullptr : config.ca_cert_path.c_str();
            const char* ca_dir = config.ca_dir_path.empty() ? nullptr : config.ca_dir_path.c_str();

            if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_dir) != 1)
            {
                ALOG_ERROR("Failed to load CA certificates: {}", detail::get_openssl_error());
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::Tls, kTlsCertificateLoadFailed});
            }
            ALOG_DEBUG("Loaded CA certificates");
        }
        else if (role == TlsRole::Client && config.verify_mode != SSL_VERIFY_NONE)
        {
            // Use system CA store for clients
            if (SSL_CTX_set_default_verify_paths(ctx) != 1)
            {
                ALOG_WARN("Failed to load system CA certificates: {}", detail::get_openssl_error());
            }
        }

        // Verification
        SSL_CTX_set_verify(ctx, config.verify_mode, nullptr);

        // Cipher suites
        if (!config.cipher_suites.empty())
        {
            if (SSL_CTX_set_cipher_list(ctx, config.cipher_suites.c_str()) != 1)
            {
                ALOG_ERROR("Failed to set cipher list: {}", detail::get_openssl_error());
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::Tls, kTlsContextCreationFailed});
            }
        }

        if (!config.ciphersuites_tls13.empty())
        {
            if (SSL_CTX_set_ciphersuites(ctx, config.ciphersuites_tls13.c_str()) != 1)
            {
                ALOG_ERROR("Failed to set TLS 1.3 ciphersuites: {}", detail::get_openssl_error());
                SSL_CTX_free(ctx);
                return std::unexpected(Error{ErrorCategory::Tls, kTlsContextCreationFailed});
            }
        }

        // ALPN
        if (!config.alpn_protocols.empty())
        {
            std::vector<unsigned char> protos;
            for (const auto& p: config.alpn_protocols)
            {
                protos.push_back(static_cast<unsigned char>(p.length()));
                protos.insert(protos.end(), p.begin(), p.end());
            }
            SSL_CTX_set_alpn_protos(ctx, protos.data(), protos.size());
        }

        return TlsContext{ctx, role == TlsRole::Server};
    }

    bool is_ktls_available()
    {
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

        if (std::ifstream tls_module("/sys/module/tls/initstate"); tls_module.is_open())
        {
            std::string state;
            tls_module >> state;
            return state == "live";
        }

        return false;
    }

    Result<void> require_ktls()
    {
#if !KIO_HAVE_OPENSSL3
        ALOG_ERROR("KTLS requires OpenSSL 3.0+, found: {}", OpenSSL_version(OPENSSL_VERSION));
        return std::unexpected(Error{ErrorCategory::Tls, kTlsKtlsEnableFailed});
#endif

        if (!is_ktls_available())
        {
            ALOG_ERROR("Kernel TLS module not loaded. Run: sudo modprobe tls");
            return std::unexpected(Error{ErrorCategory::Tls, kTlsKtlsEnableFailed});
        }

        return {};
    }

    std::string get_ktls_info()
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

        if (is_ktls_available())
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
