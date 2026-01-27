#pragma once

#include <chrono>
#include <string>
#include <utility>

#include <poll.h>

#include "aio/io.hpp"
#include "aio/net.hpp"
#include <openssl/ssl.h>

namespace aio::tls
{

class TlsSocket
{
public:
    TlsSocket(net::Socket&& sock, SSL* ssl) : sock_(std::move(sock)), ssl_(ssl)
    {
        if (ssl_)
        {
            const unsigned char* data = nullptr;
            unsigned int len = 0;
            SSL_get0_alpn_selected(ssl_, &data, &len);
            if (len > 0)
            {
                negotiated_protocol_.assign(reinterpret_cast<const char*>(data), len);
            }
        }
    }

    ~TlsSocket()
    {
        if (ssl_)
        {
            SSL_free(ssl_);
        }
    }

    TlsSocket(TlsSocket&& other) noexcept
        : sock_(std::move(other.sock_)),
          ssl_(std::exchange(other.ssl_, nullptr)),
          negotiated_protocol_(std::move(other.negotiated_protocol_))
    {
    }

    TlsSocket& operator=(TlsSocket&& other) noexcept
    {
        if (this != &other)
        {
            if (ssl_)
                SSL_free(ssl_);
            sock_ = std::move(other.sock_);
            ssl_ = std::exchange(other.ssl_, nullptr);
            negotiated_protocol_ = std::move(other.negotiated_protocol_);
        }
        return *this;
    }

    TlsSocket(const TlsSocket&) = delete;
    TlsSocket& operator=(const TlsSocket&) = delete;

    /// Raw FD for async I/O (KTLS-enabled after handshake)
    [[nodiscard]] int Get() const { return sock_.Get(); }

    /// ALPN negotiated protocol (empty string if none)
    [[nodiscard]] std::string_view GetNegotiatedProtocol() const { return negotiated_protocol_; }

    /// Check if HTTP/2 was negotiated
    [[nodiscard]] bool IsHttp2() const { return negotiated_protocol_ == "h2"; }

    /// Performs graceful TLS shutdown (sends close_notify)
    /// @param ctx IoContext for async operations
    /// @param timeout Maximum time to wait for peer's close_notify
    /// @return Result<void> - success even if peer doesn't respond cleanly
    template <typename Rep = int64_t, typename Period = std::ratio<1>>
    Task<Result<void>> AsyncShutdown(IoContext& ctx,
                                     std::chrono::duration<Rep, Period> timeout = std::chrono::seconds{5})
    {
        if (!ssl_)
            co_return Result<void>{};

        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (true)
        {
            int ret = SSL_shutdown(ssl_);

            if (ret == 1)
                co_return Result<void>{};  // Bidirectional shutdown complete

            if (ret == 0)
            {
                // Sent our close_notify, waiting for peer's
                // Call SSL_shutdown again to complete
                continue;
            }

            // ret < 0: need to wait for I/O
            int err = SSL_get_error(ssl_, ret);

            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            {
                auto remaining = deadline - std::chrono::steady_clock::now();
                if (remaining <= std::chrono::nanoseconds{0})
                {
                    // Timeout waiting for peer - not fatal
                    co_return Result<void>{};
                }

                unsigned poll_mask = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
                auto poll_res = co_await AsyncPoll(ctx, sock_, poll_mask).WithTimeout(remaining);

                if (!poll_res)
                {
                    // Poll failed or timed out - acceptable for shutdown
                    co_return Result<void>{};
                }
            }
            else
            {
                // Other error - still return success, shutdown is best-effort
                co_return Result<void>{};
            }
        }
    }

private:
    net::Socket sock_;
    SSL* ssl_ = nullptr;
    std::string negotiated_protocol_;
};

}  // namespace aio::tls