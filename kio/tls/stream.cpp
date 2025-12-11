//
// Created by Yao ACHI on 07/12/2025.
//
#include "stream.h"

#include "context.h"

namespace kio::tls
{
    TlsStream::TlsStream(io::Worker& worker, net::Socket socket, TlsContext& context, const TlsRole role) : worker_(worker), ctx_(context), socket_(std::move(socket)), role_(role)
    {
        ssl_ = SSL_new(ctx_.get());
        if (ssl_ == nullptr)
        {
            throw std::runtime_error("Failed to create SSL object: " + detail::get_openssl_error());
        }
        if (SSL_set_fd(ssl_, socket_.get()) != 1)
        {
            throw std::runtime_error("Failed to set SSL fd: " + detail::get_openssl_error());
        }
        SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

        ALOG_DEBUG("TlsStream created for fd={}", socket_.get());
    }

    TlsStream::TlsStream(TlsStream&& other) noexcept :
        worker_(other.worker_), ctx_(other.ctx_), socket_(std::move(other.socket_)), ssl_(std::exchange(other.ssl_, nullptr)), role_(other.role_), server_name_(std::exchange(other.server_name_, "")),
        handshake_done_(other.handshake_done_), ktls_active_(other.ktls_active_), cached_version_(std::exchange(other.cached_version_, "")), cached_cipher_(std::exchange(other.cached_cipher_, "")),
        cached_alpn_(std::exchange(other.cached_alpn_, ""))
    {
        // Mark the moved-from object as not having completed handshake
        // to prevent its destructor from trying to do SSL shutdown operations
        other.handshake_done_ = false;
        other.ktls_active_ = false;
    }

    Task<Result<void>> TlsStream::do_handshake_step() const
    {
        const auto st = worker_.get_stop_token();

        while (!st.stop_requested())
        {
            const int ret = (role_ == TlsRole::Client) ? SSL_connect(ssl_) : SSL_accept(ssl_);
            if (ret == 1) co_return {};

            switch (int err = SSL_get_error(ssl_, ret))
            {
                case SSL_ERROR_WANT_READ:
                    KIO_TRY(co_await worker_.async_poll(socket_.get(), POLLIN));
                    break;

                case SSL_ERROR_WANT_WRITE:
                    KIO_TRY(co_await worker_.async_poll(socket_.get(), POLLOUT));
                    break;

                case SSL_ERROR_ZERO_RETURN:
                    ALOG_ERROR("TLS handshake: peer closed connection");
                    co_return std::unexpected(Error{ErrorCategory::Tls, kTlsHandshakeFailed});

                case SSL_ERROR_SYSCALL:
                {
                    const int sys_err = errno;
                    ALOG_ERROR("TLS handshake syscall error: {}", sys_err ? strerror(sys_err) : "unexpected EOF");
                    co_return std::unexpected(Error::from_errno(sys_err ? sys_err : ECONNRESET));
                }

                case SSL_ERROR_SSL:
                    ALOG_ERROR("TLS handshake SSL error: {}", detail::get_openssl_error());
                    co_return std::unexpected(Error{ErrorCategory::Tls, kTlsHandshakeFailed});

                default:
                    ALOG_ERROR("TLS handshake unknown error: {}", err);
                    co_return std::unexpected(Error{ErrorCategory::Tls, kTlsHandshakeFailed});
            }
        }

        co_return {};
    }

    Task<Result<void>> TlsStream::async_handshake(std::string_view hostname)
    {
        if (handshake_done_) co_return {};

        if (role_ == TlsRole::Client && !hostname.empty())
        {
            SSL_set_tlsext_host_name(ssl_, std::string(hostname).c_str());
            SSL_set1_host(ssl_, std::string(hostname).c_str());
        }

        ALOG_DEBUG("Starting TLS handshake for fd={}", socket_.get());
        KIO_TRY(co_await do_handshake_step());
        handshake_done_ = true;
        ALOG_DEBUG("TLS handshake completed");

        // Cache connection info
        if (const char* ver = SSL_get_version(ssl_))
        {
            cached_version_ = ver;
            ALOG_DEBUG("SSL version is {}", cached_version_);
        }
        if (const char* cipher = SSL_get_cipher_name(ssl_))
        {
            cached_cipher_ = cipher;
            ALOG_DEBUG("SSL cipher is {}", cached_cipher_);
        }

        if (auto res = enable_ktls(); !res)
        {
            co_return std::unexpected(res.error());
        }

        co_return {};
    }

    Result<void> TlsStream::enable_ktls()
    {
#if KIO_HAVE_OPENSSL3
        bool tx = BIO_get_ktls_send(SSL_get_wbio(ssl_));
        bool rx = BIO_get_ktls_recv(SSL_get_rbio(ssl_));
        if (tx && rx)
        {
            ktls_active_ = true;
            return {};
        }
        ALOG_ERROR("KTLS Failed to Negotiate. TX={}, RX={}", tx, rx);
        ALOG_ERROR("  - Kernel TLS module loaded? (modprobe tls)");
        ALOG_ERROR("  - Cipher: {} (need AES-GCM or ChaCha20-Poly1305)", SSL_get_cipher_name(ssl_));
        return std::unexpected(Error{ErrorCategory::Tls, kTlsKtlsEnableFailed});
#else
        return std::unexpected(Error{ErrorCategory::Tls, kTlsKtlsEnableFailed});
#endif
    }

    Task<Result<void>> TlsStream::async_shutdown()
    {
        ALOG_DEBUG("TLS shutdown initiated for fd={}", socket_.get());

        if (!ssl_)
        {
            co_return {};
        }

        if (!handshake_done_)
        {
            // Never completed handshake, nothing to shut down
            ALOG_DEBUG("TLS shutdown: handshake was never completed, skipping");
            co_return {};
        }

        co_return co_await do_shutdown_step();
    }

    Task<Result<void>> TlsStream::do_shutdown_step()
    {
        const auto st = worker_.get_stop_token();

        // Maximum iterations to prevent infinite loops on misbehaving peers
        constexpr int kMaxShutdownIterations = 10;
        int iterations = 0;

        while (!st.stop_requested() && iterations++ < kMaxShutdownIterations)
        {
            // Note: SSL_shutdown returns:
            //   0 = shutdown initiated, need to call again to complete (wait for peer's close_notify)
            //   1 = shutdown complete (both sides have sent close_notify)
            //  <0 = error
            const int ret = SSL_shutdown(ssl_);

            if (ret == 1)
            {
                // Clean bidirectional shutdown complete
                ALOG_DEBUG("TLS shutdown complete (bidirectional)");
                co_return {};
            }

            if (ret == 0)
            {
                // We've sent our close_notify, now wait for peer's
                // Need to call SSL_shutdown again after receiving peer's close_notify
                // First, wait for data to arrive
                if (auto poll_res = co_await worker_.async_poll(socket_.get(), POLLIN); !poll_res)
                {
                    // Poll failed, but we've already sent our close_notify
                    // This is acceptable - peer may have already closed
                    ALOG_DEBUG("TLS shutdown: poll failed after sending close_notify, treating as complete");
                    co_return {};
                }
                continue;
            }

            // ret < 0: check the error
            switch (const int err = SSL_get_error(ssl_, ret))
            {
                case SSL_ERROR_WANT_READ:
                    KIO_TRY(co_await worker_.async_poll(socket_.get(), POLLIN));
                    break;

                case SSL_ERROR_WANT_WRITE:
                    KIO_TRY(co_await worker_.async_poll(socket_.get(), POLLOUT));
                    break;

                case SSL_ERROR_ZERO_RETURN:
                    // Peer already closed - this is fine during shutdown
                    ALOG_DEBUG("TLS shutdown: peer already closed");
                    co_return {};

                case SSL_ERROR_SYSCALL:
                {
                    const int sys_err = errno;
                    if (sys_err == 0 || sys_err == EPIPE || sys_err == ECONNRESET)
                    {
                        // Connection already closed by peer - acceptable during shutdown
                        ALOG_DEBUG("TLS shutdown: connection reset by peer");
                        co_return {};
                    }
                    ALOG_WARN("TLS shutdown syscall error: {}", strerror(sys_err));
                    co_return std::unexpected(Error::from_errno(sys_err));
                }

                case SSL_ERROR_SSL:
                    ALOG_WARN("TLS shutdown SSL error: {}", detail::get_openssl_error());
                    co_return std::unexpected(Error{ErrorCategory::Tls, kTlsShutdownFailed});

                default:
                    ALOG_WARN("TLS shutdown unknown error: {}", err);
                    co_return std::unexpected(Error{ErrorCategory::Tls, kTlsShutdownFailed});
            }
        }

        if (iterations >= kMaxShutdownIterations)
        {
            ALOG_WARN("TLS shutdown: max iterations reached, forcing close");
        }

        co_return {};
    }

    Task<Result<void>> TlsStream::async_close()
    {
        if (auto shutdown_res = co_await async_shutdown(); !shutdown_res)
        {
            // Log but don't fail - we still want to close the socket
            ALOG_WARN("TLS shutdown failed during close: {}", shutdown_res.error());
        }

        if (socket_.is_valid())
        {
            co_await worker_.async_close(socket_.release());
        }

        co_return {};
    }

    bool TlsStream::is_ktls_active() const { return ktls_active_; }
    std::string_view TlsStream::get_cipher() const { return SSL_get_cipher_name(ssl_); }
}  // namespace kio::tls
