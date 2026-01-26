#pragma once

#include <utility>

#include "aio/net.hpp"
#include <openssl/ssl.h>

namespace aio::tls
{

class TlsSocket
{
public:
    // Move-only
    TlsSocket(net::Socket&& sock, SSL* ssl) : sock_(std::move(sock)), ssl_(ssl) {}

    ~TlsSocket()
    {
        if (ssl_)
        {
            // Note: SSL_free here frees the SSL structure.
            // It does NOT close the fd because we own sock_, which closes the fd.
            SSL_free(ssl_);
        }
    }

    TlsSocket(TlsSocket&& other) noexcept : sock_(std::move(other.sock_)), ssl_(std::exchange(other.ssl_, nullptr)) {}

    TlsSocket& operator=(TlsSocket&& other) noexcept
    {
        if (this != &other)
        {
            if (ssl_)
                SSL_free(ssl_);
            sock_ = std::move(other.sock_);
            ssl_ = std::exchange(other.ssl_, nullptr);
        }
        return *this;
    }

    // CONCEPT SATISFACTION: This allows AsyncRead/AsyncWrite/AsyncSendfile to work.
    // Returns the raw FD, which is now "Upgraded" to KTLS.
    [[nodiscard]] int Get() const { return sock_.Get(); }

    // Metadata access
    const char* GetNegotiatedProtocol() const
    {
        const unsigned char* data = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(ssl_, &data, &len);
        return len > 0 ? reinterpret_cast<const char*>(data) : "http/1.1";
    }

private:
    net::Socket sock_;
    SSL* ssl_ = nullptr;
};

}  // namespace aio::tls