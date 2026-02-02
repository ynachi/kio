#include "aio/tls/tls_context.hpp"

#include <fstream>
#include <numeric>

namespace aio::tls
{

namespace detail
{

bool HaveKtls()
{
    // Method 1: Check /proc/modules
    if (std::ifstream modules("/proc/modules"); modules.is_open())
    {
        std::string line;
        while (std::getline(modules, line))
        {
            if (const std::string_view view = line; view.starts_with("tls "))
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

    info += HaveKtls() ? "Kernel TLS module: Loaded\n" : "Kernel TLS module: NOT LOADED (run: sudo modprobe tls)\n";
    return info;
}

// Helper: Convert {"h2", "http/1.1"} -> \x02h2\x08http/1.1 (Wire format)
static std::vector<unsigned char> SerializeAlpn(const std::vector<std::string>& protos)
{
    if (protos.empty())
        return {};

    size_t total_size = 0;
    for (const auto& p : protos)
    {
        if (p.length() > 255)
            continue;
        total_size += 1 + p.length();
    }

    std::vector<unsigned char> wire;
    wire.reserve(total_size);

    for (const auto& p : protos)
    {
        if (p.length() > 255)
            continue;
        wire.push_back(static_cast<unsigned char>(p.length()));
        wire.insert(wire.end(), p.begin(), p.end());
    }
    return wire;
}

static Result<> SetCiphers(SSL_CTX* ctx, const TlsConfig& config)
{
    // Cipher Suites (TLS 1.2)
    if (!config.cipher_suites.empty())
    {
        if (SSL_CTX_set_cipher_list(ctx, config.cipher_suites.c_str()) != 1)
        {
            return ErrorFromOpenSSL();
        }
    }

    // Cipher Suites (TLS 1.3)
    const std::string& tls13_ciphers =
        config.ciphersuites_tls13.empty() ? kDefaultTls1_3Ciphers : config.ciphersuites_tls13;

    if (SSL_CTX_set_ciphersuites(ctx, tls13_ciphers.c_str()) != 1)
    {
        return ErrorFromOpenSSL();
    }

    return {};
}

static Result<> LoadCerts(SSL_CTX* ctx, const TlsConfig& config, const bool server_mode)
{
    if (!config.cert_path.empty() && !config.key_path.empty())
    {
        // Load Certificate Chain
        if (SSL_CTX_use_certificate_chain_file(ctx, config.cert_path.string().c_str()) != 1)
        {
            return ErrorFromOpenSSL();
        }

        // Load Private Key
        if (SSL_CTX_use_PrivateKey_file(ctx, config.key_path.string().c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return ErrorFromOpenSSL();
        }

        // Verify Key Matches Cert
        if (SSL_CTX_check_private_key(ctx) != 1)
        {
            return ErrorFromOpenSSL();
        }
    }
    else if (server_mode)
    {
        // For servers, identity is mandatory
        return ErrorFromErrno(EINVAL);
    }

    return {};
}

// Updated to take server_mode: mTLS requires sending the CA names list to the client
static Result<> TrustStore(SSL_CTX* ctx, const TlsConfig& config, const bool server_mode)
{
    if (!config.ca_cert_path.empty() || !config.ca_dir_path.empty())
    {
        std::string ca_file_str = config.ca_cert_path.string();
        std::string ca_dir_str = config.ca_dir_path.string();
        const char* ca_file = config.ca_cert_path.empty() ? nullptr : ca_file_str.c_str();
        const char* ca_dir = config.ca_dir_path.empty() ? nullptr : ca_dir_str.c_str();

        // 1. Load for Verification (Check signature)
        if (SSL_CTX_load_verify_locations(ctx, ca_file, ca_dir) != 1)
        {
            return ErrorFromOpenSSL();
        }

        // 2. Load for Advertisement (Server mTLS only)
        // If we are a server requesting a client cert, we must send the list of
        // Acceptable CAs (Distinguished Names) in the ServerHello.
        // Otherwise, "picky" clients (browsers, Java, Go) won't send a cert.
        if (server_mode && !config.ca_cert_path.empty())
        {
            STACK_OF(X509_NAME)* list = SSL_load_client_CA_file(ca_file);
            if (list == nullptr)
            {
                return ErrorFromOpenSSL();
            }
            // Takes ownership of the stack
            SSL_CTX_set_client_CA_list(ctx, list);
        }
    }
    return {};
}

using AlpnCallback = int (*)(SSL*, const unsigned char**, unsigned char*, const unsigned char*, unsigned int, void*);

static Result<std::unique_ptr<std::vector<unsigned char>>> SetupAlpn(SSL_CTX* ctx, const TlsConfig& config,
                                                                     const bool server_mode, AlpnCallback alpn_cb)
{
    std::unique_ptr<std::vector<unsigned char>> alpn_storage = nullptr;

    if (!config.alpn_protocols.empty())
    {
        auto wire_data = SerializeAlpn(config.alpn_protocols);
        if (!wire_data.empty())
        {
            if (server_mode)
            {
                // SERVER: Create stable storage on heap
                alpn_storage = std::make_unique<std::vector<unsigned char>>(std::move(wire_data));

                // Pass the raw pointer of the vector to OpenSSL.
                // Because 'alpn_storage' is a unique_ptr, moving it into the TlsContext
                // later will NOT change the address of the vector in the heap.
                SSL_CTX_set_alpn_select_cb(ctx, alpn_cb, alpn_storage.get());
            }
            else
            {
                // CLIENT: OpenSSL copies the data internally immediately.
                if (SSL_CTX_set_alpn_protos(ctx, wire_data.data(), wire_data.size()) != 0)
                {
                    return ErrorFromOpenSSL();
                }
            }
        }
    }

    return alpn_storage;
}

}  // namespace detail

int TlsContext::AlpnSelectCb(SSL* /*ssl*/, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                             unsigned int inlen, void* arg)
{
    // 'arg' is the raw pointer to the std::vector inside the unique_ptr
    auto* server_protos = static_cast<std::vector<unsigned char>*>(arg);

    if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen, server_protos->data(), server_protos->size(),
                              in, inlen) != OPENSSL_NPN_NEGOTIATED)
    {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

Result<TlsContext> TlsContext::Create(const TlsConfig& config, const bool server_mode)
{
    // Ensure KTLS compile-time support
#ifndef SSL_OP_ENABLE_KTLS
    return ErrorFromErrno(EINVAL);
#endif

    // Ensure KTLS runtime support
    if (!detail::HaveKtls())
    {
        return ErrorFromErrno(EINVAL);
    }

    // 1. Create Context
    const SSL_METHOD* method = server_mode ? TLS_server_method() : TLS_client_method();
    if (method == nullptr)
        return ErrorFromOpenSSL();

    SSLCtxPtr ctx{SSL_CTX_new(method)};
    if (ctx == nullptr)
        return ErrorFromOpenSSL();

    // 2. Protocols & Ciphers
    if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION) != 1)
        return ErrorFromOpenSSL();

    if (auto err = detail::SetCiphers(ctx.get(), config); !err.has_value())
    {
        return std::unexpected(err.error());
    }

    // 3. Set Options (Enforce KTLS)
    SSL_CTX_set_options(ctx.get(), SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION | SSL_OP_ENABLE_KTLS |
                                       SSL_OP_NO_RENEGOTIATION);

    // TLS 1.3 post-handshake tickets break KTLS; disable for servers.
    if (server_mode)
    {
        SSL_CTX_set_num_tickets(ctx.get(), 0);
        SSL_CTX_set_options(ctx.get(), SSL_OP_NO_TICKET);
    }

    // 4. Certificates & Trust
    if (auto err = detail::LoadCerts(ctx.get(), config, server_mode); !err.has_value())
    {
        return std::unexpected(err.error());
    }

    // Pass server_mode to TrustStore to enable CA Advertisement for mTLS
    if (auto err = detail::TrustStore(ctx.get(), config, server_mode); !err.has_value())
    {
        return std::unexpected(err.error());
    }

    SSL_CTX_set_verify(ctx.get(), config.verify_mode, nullptr);

    // 5. ALPN Setup
    auto alpn_res = detail::SetupAlpn(ctx.get(), config, server_mode, AlpnSelectCb);
    if (!alpn_res.has_value())
    {
        return std::unexpected(alpn_res.error());
    }
    std::unique_ptr<std::vector<unsigned char>> alpn_storage = std::move(alpn_res.value());

    return TlsContext(std::move(ctx), std::move(alpn_storage), config.verify_hostname);
}

}  // namespace aio::tls
