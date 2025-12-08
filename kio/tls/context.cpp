//
// Created by Yao ACHI on 07/12/2025.
//

#include "context.h"

namespace kio::tls
{
    SslContext::SslContext(const bool is_server, const std::string& cert_path, const std::string& key_path) : is_server_(is_server)
    {
        ctx_ = SSL_CTX_new(is_server ? TLS_server_method() : TLS_client_method());
        if (!ctx_)
        {
            throw std::runtime_error("Failed to create SSL_CTX");
        }

        // OpenSSL will automatically call setsockopt() to enable KTLS after handshake
        SSL_CTX_set_options(ctx_, SSL_OP_ENABLE_KTLS);
        SSL_CTX_set_options(ctx_, SSL_OP_NO_TICKET);

        // Only allow TLS 1.3 for best performance and security
        SSL_CTX_set_min_proto_version(ctx_, TLS1_3_VERSION);

        // Disable internal buffering slightly to reduce latency with io_uring
        SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE);
        SSL_CTX_set_mode(ctx_, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

        // Prefer KTLS-compatible ciphers (AES-GCM, ChaCha20-Poly1305)
        // These are the default in modern OpenSSL, but being explicit is good
        SSL_CTX_set_ciphersuites(ctx_, "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");

        // Server-specific setup
        if (is_server)
        {
            if (!cert_path.empty() && !key_path.empty())
            {
                if (SSL_CTX_use_certificate_chain_file(ctx_, cert_path.c_str()) <= 0)
                {
                    SSL_CTX_free(ctx_);
                    throw std::runtime_error("Failed to load certificate chain");
                }

                if (SSL_CTX_use_PrivateKey_file(ctx_, key_path.c_str(), SSL_FILETYPE_PEM) <= 0)
                {
                    SSL_CTX_free(ctx_);
                    throw std::runtime_error("Failed to load private key");
                }

                if (SSL_CTX_check_private_key(ctx_) <= 0)
                {
                    SSL_CTX_free(ctx_);
                    throw std::runtime_error("Private key does not match certificate");
                }
            }
        }
        // Client-specific setup
        else
        {
            // Load system CA certificates for verification
            SSL_CTX_set_default_verify_paths(ctx_);
        }
    }

    SslContext create_client_context(const std::string& ca_path)
    {
        SslContext ctx(false);

        if (!ca_path.empty())
        {
            // Load specific CA certificate
            if (SSL_CTX_load_verify_locations(ctx.get(), ca_path.c_str(), nullptr) != 1)
            {
                throw std::runtime_error("Failed to load CA certificate: " + ca_path);
            }
            // Enable verification
            ctx.set_verify_peer(true);
        }

        return ctx;
    }
}  // namespace kio::tls
