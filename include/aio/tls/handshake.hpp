#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <poll.h>

#include "aio/io.hpp"
#include "aio/tls/socket.hpp"
#include "aio/tls/tls_context.hpp"
#include <openssl/ssl.h>

namespace aio::tls
{

/// @brief Performs an async TLS handshake (KTLS-compatible).
/// @param ctx IoContext for async operations
/// @param sock Socket to upgrade (ownership transferred)
/// @param tls_ctx TLS context with certificates/config
/// @param is_server true for server-side, false for client-side
/// @param hostname Server hostname for SNI and verification (client only)
/// @param timeout Maximum time for handshake completion
/// @return TlsSocket on success, error on failure
template <typename Rep = int64_t, typename Period = std::ratio<1>>
Task<Result<TlsSocket>> AsyncTlsHandshake(IoContext& ctx, net::Socket sock, TlsContext& tls_ctx, bool is_server,
                                          std::string_view hostname = {},
                                          std::chrono::duration<Rep, Period> timeout = std::chrono::seconds{30})
{
    if (auto nb = sock.SetNonBlocking(); !nb)
    {
        co_return std::unexpected(nb.error());
    }

    SSL* ssl = SSL_new(tls_ctx.NativeHandle());
    if (!ssl)
        co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    if (SSL_set_fd(ssl, sock.Get()) != 1)
    {
        SSL_free(ssl);
        co_return ErrorFromOpenSSL();
    }

    if (is_server)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);

    SSL_set_mode(ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    std::string server_name;
    if (!is_server && !hostname.empty())
    {
        server_name = std::string(hostname);
        if (SSL_set_tlsext_host_name(ssl, server_name.c_str()) != 1)
        {
            SSL_free(ssl);
            co_return ErrorFromOpenSSL();
        }
        if (tls_ctx.VerifyHostname())
        {
            if (SSL_set1_host(ssl, server_name.c_str()) != 1)
            {
                SSL_free(ssl);
                co_return ErrorFromOpenSSL();
            }
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (true)
    {
        int ret = SSL_do_handshake(ssl);

        if (ret == 1)
            break;

        const int err = SSL_get_error(ssl, ret);

        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::nanoseconds{0})
            {
                SSL_free(ssl);
                co_return std::unexpected(std::make_error_code(std::errc::timed_out));
            }

            const unsigned poll_mask = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;
            auto poll_res = co_await AsyncPoll(ctx, sock, poll_mask).WithTimeout(remaining);

            if (!poll_res)
            {
                SSL_free(ssl);
                co_return std::unexpected(poll_res.error());
            }
        }
        else
        {
            auto final_err = ErrorFromOpenSSL();
            SSL_free(ssl);
            co_return final_err;
        }
    }

    // Post-handshake verification (defense in depth)
    if (!is_server && tls_ctx.VerifyHostname() && !server_name.empty())
    {
        if (SSL_get_verify_mode(ssl) != SSL_VERIFY_NONE)
        {
            if (const auto verify_res = SSL_get_verify_result(ssl); verify_res != X509_V_OK)
            {
                SSL_free(ssl);
                co_return std::unexpected(std::make_error_code(std::errc::permission_denied));
            }
        }
    }

// Enforce KTLS
#if KIO_HAVE_OPENSSL3
    bool ktls_tx = BIO_get_ktls_send(SSL_get_wbio(ssl));
    bool ktls_rx = BIO_get_ktls_recv(SSL_get_rbio(ssl));

    if (!ktls_tx || !ktls_rx)
    {
        std::fprintf(stderr, "KTLS failed: TX=%d, RX=%d\n", ktls_tx, ktls_rx);
        std::fprintf(stderr, "  Cipher: %s\n", SSL_get_cipher_name(ssl));
        SSL_free(ssl);
        co_return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }
#else
    SSL_free(ssl);
    co_return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif

    co_return TlsSocket(std::move(sock), ssl);
}

}  // namespace aio::tls