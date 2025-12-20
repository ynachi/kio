//
// Created by Yao ACHI on 07/12/2025.
//
#include "stream.h"

#include "context.h"
#include "kio/core/async_logger.h"

namespace kio::tls
{
// Helper coroutine: used ONLY when we have buffered OpenSSL data.
// This ensures we don't pay the coroutine frame cost for the common KTLS path.
// In another word, this is to avoid something like co_return co_await when its is not absolutely necessary.
static Task<Result<int>> buffered_read_task(int result)
{
    co_return result;
}

TlsStream::TlsStream(io::Worker& worker, net::Socket socket, TlsContext& context, const TlsRole role) :
    worker_(worker), ctx_(context), socket_(std::move(socket)), role_(role)
{
    ssl_ = SSL_new(ctx_.get());
    if (ssl_ == nullptr)
    {
        throw std::runtime_error("Failed to create SSL object: " + detail::get_openssl_error());
    }
    if (SSL_set_fd(ssl_, socket_.get()) != 1)
    {
        SSL_free(ssl_);
        throw std::runtime_error("Failed to set SSL fd: " + detail::get_openssl_error());
    }
    // SSL_MODE_ENABLE_PARTIAL_WRITE is crucial for async
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    ALOG_DEBUG("TlsStream created for fd={}", socket_.get());
}

TlsStream::TlsStream(TlsStream&& other) noexcept :
    worker_(other.worker_), ctx_(other.ctx_), socket_(std::move(other.socket_)),
    ssl_(std::exchange(other.ssl_, nullptr)), role_(other.role_), server_name_(std::exchange(other.server_name_, "")),
    handshake_done_(other.handshake_done_), ktls_active_(other.ktls_active_)
{
    other.handshake_done_ = false;
    other.ktls_active_ = false;
}

Task<Result<void>> TlsStream::do_handshake_step() const
{
    const auto st = worker_.GetStopToken();

    while (!st.stop_requested())
    {
        const int ret = (role_ == TlsRole::Client) ? SSL_connect(ssl_) : SSL_accept(ssl_);
        if (ret == 1) co_return {};

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
                co_return std::unexpected(Error::FromErrno(sys_err ? sys_err : ECONNRESET));
            }

            case SSL_ERROR_SSL:
                ALOG_ERROR("TLS handshake SSL error: {}", detail::get_openssl_error());
                co_return std::unexpected(Error{ErrorCategory::kTls, kTlsHandshakeFailed});

            default:
                ALOG_ERROR("TLS handshake unknown error: {}", err);
                co_return std::unexpected(Error{ErrorCategory::kTls, kTlsHandshakeFailed});
        }
    }

    co_return {};
}

Task<Result<void>> TlsStream::async_handshake(std::string_view hostname)
{
    if (handshake_done_) co_return {};

    if (role_ == TlsRole::Client && !hostname.empty())
    {
        server_name_ = std::string(hostname);
        SSL_set_tlsext_host_name(ssl_, server_name_.c_str());
        SSL_set1_host(ssl_, server_name_.c_str());
    }

    ALOG_DEBUG("Starting TLS handshake for fd={}", socket_.get());
    KIO_TRY(co_await do_handshake_step());
    handshake_done_ = true;
    ALOG_DEBUG("TLS handshake completed");

    if (const char* ver = SSL_get_version(ssl_))
    {
        ALOG_DEBUG("SSL version is {}", ver);
    }
    if (const char* cipher = SSL_get_cipher_name(ssl_))
    {
        ALOG_DEBUG("SSL cipher is {}", cipher);
    }

    KIO_TRY(enable_ktls());

    co_return {};
}

Result<void> TlsStream::enable_ktls()
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

Task<Result<int>> TlsStream::async_read(std::span<char> buf)
{
    // Drain OpenSSL buffer if needed (rare path)
    if (ssl_ && SSL_pending(ssl_) > 0)
    {
        // This is a synchronous memory copy from OpenSSL internal buffer
        int n = SSL_read(ssl_, buf.data(), static_cast<int>(buf.size()));
        if (n > 0)
        {
            // Return a helper task that is already "ready" with the result.
            // This allocates a frame, but only for this edge case.
            co_return n;
        }
    }

    co_return co_await worker_.AsyncRead(socket_.get(), buf);
}

Task<Result<void>> TlsStream::async_read_exact(std::span<char> buf)
{
    const auto st = worker_.GetStopToken();

    size_t total_bytes_read = 0;
    const size_t total_to_read = buf.size();

    while (!st.stop_requested() && total_bytes_read < total_to_read)
    {
        const int bytes_read = KIO_TRY(co_await this->async_read(buf.subspan(total_bytes_read)));

        if (bytes_read == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }
        total_bytes_read += static_cast<size_t>(bytes_read);
    }
    co_return {};
}

auto TlsStream::async_write(std::span<const char> buf)
{
    return worker_.AsyncWrite(socket_.get(), buf);
}

Task<Result<void>> TlsStream::async_write_exact(std::span<const char> buf)
{
    return worker_.AsyncWriteExact(socket_.get(), buf);
}

Task<Result<void>> TlsStream::do_shutdown_step()
{
    const auto st = worker_.GetStopToken();
    constexpr int kMaxShutdownIterations = 10;
    int iterations = 0;

    while (!st.stop_requested() && iterations++ < kMaxShutdownIterations)
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

Task<Result<void>> TlsStream::async_shutdown()
{
    if (!ssl_ || !handshake_done_) co_return {};
    ALOG_DEBUG("TLS shutdown initiated for fd={}", socket_.get());
    co_return co_await do_shutdown_step();
}

Task<Result<void>> TlsStream::async_close()
{
    // Best effort shutdown
    co_await async_shutdown();

    if (socket_.is_valid())
    {
        co_await worker_.AsyncClose(socket_.release());
    }
    co_return {};
}

bool TlsStream::is_ktls_active() const
{
    return ktls_active_;
}

std::string_view TlsStream::get_cipher() const
{
    if (!ssl_) return {};
    const char* cipher = SSL_get_cipher_name(ssl_);
    return cipher ? cipher : "";
}

std::string_view TlsStream::get_version() const
{
    if (!ssl_) return {};
    const char* ver = SSL_get_version(ssl_);
    return ver ? ver : "";
}
}  // namespace kio::tls
