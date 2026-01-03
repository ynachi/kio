//
// Created by Yao ACHI on 07/12/2025.
//
#include "stream.h"

#include "context.h"
#include "kio/core/async_logger.h"

namespace kio::tls
{
TlsStream::TlsStream(io::Worker& worker, net::Socket socket, TlsContext& context, const TlsRole role) :
    worker_(worker), ctx_(context), socket_(std::move(socket)), ssl_(SSL_new(ctx_.Get())), role_(role)
{
    if (ssl_ == nullptr)
    {
        throw std::runtime_error("Failed to create SSL object: " + detail::GetOpensslError());
    }
    if (SSL_set_fd(ssl_, socket_.get()) != 1)
    {
        SSL_free(ssl_);
        throw std::runtime_error("Failed to set SSL fd: " + detail::GetOpensslError());
    }
    // SSL_MODE_ENABLE_PARTIAL_WRITE is crucial for async
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    ALOG_DEBUG("TlsStream created for fd={}", socket_.get());
}

TlsStream::TlsStream(TlsStream&& other) noexcept :
    worker_(other.worker_), ctx_(other.ctx_), socket_(std::move(other.socket_)),
    ssl_(std::exchange(other.ssl_, nullptr)), role_(other.role_), server_name_(std::exchange(other.server_name_, "")),
    handshake_done_(std::exchange(other.handshake_done_, false)), ktls_active_(std::exchange(other.ktls_active_, false))
{
}

Task<Result<void>> TlsStream::DoHandshakeStep() const
{
    const auto st = worker_.GetStopToken();

    while (!st.stop_requested())
    {
        const int ret = (role_ == TlsRole::kClient) ? SSL_connect(ssl_) : SSL_accept(ssl_);
        if (ret == 1)
        {
            co_return {};
        }

        switch (int err = SSL_get_error(ssl_, ret))
        {
            case SSL_ERROR_WANT_READ:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLIN));
                break;

            case SSL_ERROR_WANT_WRITE:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLOUT));
                break;

            case SSL_ERROR_ZERO_RETURN:
                ALOG_ERROR("TLS handshake: peer closed connection");
                co_return std::unexpected(Error{ErrorCategory::kTls, kTlsHandshakeFailed});

            case SSL_ERROR_SYSCALL:
            {
                const int sys_err = errno;
                ALOG_ERROR("TLS handshake syscall error: {}", sys_err ? strerror(sys_err) : "unexpected EOF");
                co_return std::unexpected(Error::FromErrno((sys_err != 0) ? sys_err : ECONNRESET));
            }

            case SSL_ERROR_SSL:
                ALOG_ERROR("TLS handshake SSL error: {}", detail::GetOpensslError());
                co_return std::unexpected(Error{ErrorCategory::kTls, kTlsHandshakeFailed});

            default:
                ALOG_ERROR("TLS handshake unknown error: {}", err);
                co_return std::unexpected(Error{ErrorCategory::kTls, kTlsHandshakeFailed});
        }
    }

    co_return {};
}

Task<Result<void>> TlsStream::AsyncHandshake(std::string_view hostname)
{
    if (handshake_done_)
    {
        co_return {};
    }

    if (role_ == TlsRole::kClient && !hostname.empty())
    {
        server_name_ = std::string(hostname);
        SSL_set_tlsext_host_name(ssl_, server_name_.c_str());
        SSL_set1_host(ssl_, server_name_.c_str());
    }

    ALOG_DEBUG("Starting TLS handshake for fd={}", socket_.get());
    KIO_TRY(co_await DoHandshakeStep());
    ALOG_DEBUG("TLS handshake completed");

    if (role_ == TlsRole::kClient && !server_name_.empty())
    {
        const long verify_result = SSL_get_verify_result(ssl_);
        if (verify_result != X509_V_OK)
        {
            ALOG_ERROR("Certificate verification failed: {}", X509_verify_cert_error_string(verify_result));
            co_return std::unexpected(Error{ErrorCategory::kTls, kTlsVerificationFailed});
        }
    }

    if (const char* ver = SSL_get_version(ssl_))
    {
        ALOG_DEBUG("SSL version is {}", ver);
    }
    if (const char* cipher = SSL_get_cipher_name(ssl_))
    {
        ALOG_DEBUG("SSL cipher is {}", cipher);
    }

    KIO_TRY(EnableKtls());
    handshake_done_ = true;

    co_return {};
}

Result<void> TlsStream::EnableKtls()
{
#if KIO_HAVE_OPENSSL3
    // Check if OpenSSL successfully negotiated KTLS
    bool tx = BIO_get_ktls_send(SSL_get_wbio(ssl_));
    bool rx = BIO_get_ktls_recv(SSL_get_rbio(ssl_));

    if (tx && rx)
    {
        ktls_active_ = true;
        ALOG_DEBUG("KTLS enabled: TX={}, RX={}", tx, rx);
        return {};
    }

    // Detailed error logging to help debugging
    ALOG_ERROR("KTLS failed to negotiate. TX={}, RX={}", tx, rx);
    ALOG_ERROR("  - Kernel TLS module loaded? (modprobe tls)");
    ALOG_ERROR("  - Cipher: {} (need AES-GCM or ChaCha20-Poly1305)", SSL_get_cipher_name(ssl_));

    // This is a strict requirement for this implementation
    return std::unexpected(Error{ErrorCategory::kTls, kTlsKtlsEnableFailed});
#else
    return std::unexpected(Error{ErrorCategory::Tls, kTlsKtlsEnableFailed});
#endif
}

Task<Result<void>> TlsStream::AsyncReadExact(std::span<char> buf)
{
    const auto st = worker_.GetStopToken();

    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (!st.stop_requested() && total_bytes_read < total_to_read)
    {
        const int bytes_read = KIO_TRY(co_await this->AsyncRead(buf.subspan(total_bytes_read)));

        if (bytes_read == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }
        total_bytes_read += static_cast<size_t>(bytes_read);
    }
    co_return {};
}

Task<Result<void>> TlsStream::DoShutdownStep()
{
    const auto st = worker_.GetStopToken();
    constexpr int k_max_shutdown_iterations = 10;
    int iterations = 0;

    while (!st.stop_requested() && iterations++ < k_max_shutdown_iterations)
    {
        const int ret = SSL_shutdown(ssl_);

        if (ret == 1)
        {
            ALOG_DEBUG("TLS shutdown complete (bidirectional)");
            co_return {};
        }

        if (ret == 0)
        {
            // Wait for peer's close_notify
            if (const auto poll_res = co_await worker_.AsyncPoll(socket_.get(), POLLIN); !poll_res)
            {
                ALOG_DEBUG("TLS shutdown: poll failed, treating as complete");
                co_return {};
            }
            continue;
        }

        switch (const int err = SSL_get_error(ssl_, ret))
        {
            case SSL_ERROR_WANT_READ:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLIN));
                break;
            case SSL_ERROR_WANT_WRITE:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLOUT));
                break;
            case SSL_ERROR_ZERO_RETURN:
            case SSL_ERROR_SYSCALL:
                // Peer closed, fine.
                co_return {};
            default:
                ALOG_WARN("TLS shutdown error: {}", err);
                co_return std::unexpected(Error{ErrorCategory::kTls, kTlsShutdownFailed});
        }
    }
    co_return {};
}

Task<Result<void>> TlsStream::AsyncShutdown()
{
    if ((ssl_ == nullptr) || !handshake_done_)
    {
        co_return {};
    }
    ALOG_DEBUG("TLS shutdown initiated for fd={}", socket_.get());
    co_return co_await DoShutdownStep();
}

Task<Result<void>> TlsStream::AsyncClose()
{
    // Best effort shutdown
    co_await AsyncShutdown();

    if (socket_.is_valid())
    {
        co_await worker_.AsyncClose(socket_.release());
    }
    co_return {};
}

bool TlsStream::IsKtlsActive() const
{
    return ktls_active_;
}

std::string_view TlsStream::GetCipher() const
{
    if (ssl_ == nullptr)
    {
        return {};
    }
    const char* cipher = SSL_get_cipher_name(ssl_);
    return (cipher != nullptr) ? cipher : "";
}

std::string_view TlsStream::GetVersion() const
{
    if (ssl_ == nullptr)
    {
        return {};
    }
    const char* ver = SSL_get_version(ssl_);
    return (ver != nullptr) ? ver : "";
}
}  // namespace kio::tls
