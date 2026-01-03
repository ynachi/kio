//
// Created by Yao ACHI on 07/12/2025.
//
#include "stream.h"

#include "context.h"
#include "kio/core/async_logger.h"

namespace kio::tls
{

TlsStream::TlsStream(io::Worker& worker, net::Socket socket, TlsContext& context, const TlsRole role)
    : worker_(worker), ctx_(context), socket_(std::move(socket)), ssl_(SSL_new(ctx_.Get())), role_(role)
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

TlsStream::TlsStream(TlsStream&& other) noexcept
    : worker_(other.worker_),
      ctx_(other.ctx_),
      socket_(std::move(other.socket_)),
      ssl_(std::exchange(other.ssl_, nullptr)),
      role_(other.role_),
      server_name_(std::exchange(other.server_name_, "")),
      handshake_done_(std::exchange(other.handshake_done_, false)),
      ktls_active_(std::exchange(other.ktls_active_, false))
{
}

// -----------------------------------------------------------------------------
// Refactored Handshake Logic
// -----------------------------------------------------------------------------

// Helper enum to decide what the coroutine should do next
enum class AsyncSslAction : uint8_t
{
    kContinue,
    kWaitRead,
    kWaitWrite,
    kSuccess,
    kError
};

namespace
{
// Pure logic helper: Analyzes SSL return code and decides next step
std::pair<AsyncSslAction, Error> AnalyzeHandshakeResult(const int ret, const SSL* ssl)
{
    if (ret == 1)
    {
        return {AsyncSslAction::kSuccess, {}};
    }

    switch (const int kErr = SSL_get_error(ssl, ret))
    {
        case SSL_ERROR_WANT_READ:
            return {AsyncSslAction::kWaitRead, {}};

        case SSL_ERROR_WANT_WRITE:
            return {AsyncSslAction::kWaitWrite, {}};

        case SSL_ERROR_ZERO_RETURN:
            ALOG_ERROR("TLS handshake: peer closed connection");
            return {
                AsyncSslAction::kError, {ErrorCategory::kTls, kTlsHandshakeFailed}
            };

        case SSL_ERROR_SYSCALL:
        {
            const int kSysErr = errno;
            ALOG_ERROR("TLS handshake syscall error: {}", kSysErr ? strerror(kSysErr) : "unexpected EOF");
            return {AsyncSslAction::kError, Error::FromErrno((kSysErr != 0) ? kSysErr : ECONNRESET)};
        }

        case SSL_ERROR_SSL:
            ALOG_ERROR("TLS handshake SSL error: {}", detail::GetOpensslError());
            return {
                AsyncSslAction::kError, {ErrorCategory::kTls, kTlsHandshakeFailed}
            };

        default:
            ALOG_ERROR("TLS handshake unknown error: {}", kErr);
            return {
                AsyncSslAction::kError, {ErrorCategory::kTls, kTlsHandshakeFailed}
            };
    }
}
}  // namespace

Task<Result<void>> TlsStream::DoHandshakeStep() const
{
    const auto kSt = worker_.GetStopToken();

    while (!kSt.stop_requested())
    {
        const int kRet = (role_ == TlsRole::kClient) ? SSL_connect(ssl_) : SSL_accept(ssl_);

        switch (const auto [action, error] = AnalyzeHandshakeResult(kRet, ssl_); action)
        {
            case AsyncSslAction::kContinue:
                continue;

            case AsyncSslAction::kSuccess:
                co_return {};

            case AsyncSslAction::kWaitRead:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLIN));
                break;

            case AsyncSslAction::kWaitWrite:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLOUT));
                break;

            case AsyncSslAction::kError:
                co_return std::unexpected(error);
        }
    }

    co_return std::unexpected(Error{ErrorCategory::kApplication, kAppUnknown});
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

    // Post-handshake verification
    if (role_ == TlsRole::kClient && !server_name_.empty())
    {
        // Only enforce strict verification if the user explicitly requested it.
        // If the mode is SSL_VERIFY_NONE, we ignore verification errors (common for self-signed certs).
        if (SSL_get_verify_mode(ssl_) != SSL_VERIFY_NONE)
        {
            if (const auto kVerifyResult = SSL_get_verify_result(ssl_); kVerifyResult != X509_V_OK)
            {
                ALOG_ERROR("Certificate verification failed: {}", X509_verify_cert_error_string(kVerifyResult));
                co_return std::unexpected(Error{ErrorCategory::kTls, kTlsVerificationFailed});
            }
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

    return std::unexpected(Error{ErrorCategory::kTls, kTlsKtlsEnableFailed});
#else
    return std::unexpected(Error{ErrorCategory::kTls, kTlsKtlsEnableFailed});
#endif
}

Task<Result<void>> TlsStream::AsyncReadExact(std::span<char> buf)
{
    const auto kSt = worker_.GetStopToken();

    size_t total_bytes_read = 0;
    const size_t kTotalToRead = buf.size();

    while (!kSt.stop_requested() && total_bytes_read < kTotalToRead)
    {
        const int kBytesRead = KIO_TRY(co_await this->AsyncRead(buf.subspan(total_bytes_read)));

        if (kBytesRead == 0)
        {
            co_return std::unexpected(Error{ErrorCategory::kFile, kIoEof});
        }
        total_bytes_read += static_cast<size_t>(kBytesRead);
    }
    co_return {};
}

// -----------------------------------------------------------------------------
// Shutdown Logic
// -----------------------------------------------------------------------------

Task<Result<void>> TlsStream::DoShutdownStep() const
{
    const auto kSt = worker_.GetStopToken();
    constexpr int kMaxShutdownIterations = 10;
    int iterations = 0;

    while (!kSt.stop_requested() && iterations++ < kMaxShutdownIterations)
    {
        const int kRet = SSL_shutdown(ssl_);

        // Similar to Handshake, analyze result
        if (kRet == 1)
        {
            ALOG_DEBUG("TLS shutdown complete (bidirectional)");
            co_return {};
        }

        if (kRet == 0)
        {
            // The standard says: Call SSL_shutdown again to complete the bidirectional shutdown
            // BUT if we want to wait for the peer, we must poll.
            if (const auto kPollRes = co_await worker_.AsyncPoll(socket_.get(), POLLIN); !kPollRes)
            {
                ALOG_DEBUG("TLS shutdown: poll failed, treating as complete");
                co_return {};
            }
            continue;
        }

        switch (const auto [action, error] = AnalyzeHandshakeResult(kRet, ssl_); action)
        {
            case AsyncSslAction::kWaitRead:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLIN));
                break;
            case AsyncSslAction::kWaitWrite:
                KIO_TRY(co_await worker_.AsyncPoll(socket_.get(), POLLOUT));
                break;
            // For shutdown, zero return or syscall errors usually just mean "connection gone", which is fine
            case AsyncSslAction::kError:
                if (error.value == kTlsHandshakeFailed && error.category == ErrorCategory::kTls)
                {
                    // Specific SSL protocol error during shutdown -> just return error
                    ALOG_WARN("TLS shutdown protocol error");
                    co_return std::unexpected(Error{ErrorCategory::kTls, kTlsShutdownFailed});
                }
                // Otherwise assume peer closed
                co_return {};
            default:
                co_return {};
        }
    }
    co_return {};
}

Task<Result<void>> TlsStream::AsyncShutdown() const
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
        (void)co_await worker_.AsyncClose(socket_.release());
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