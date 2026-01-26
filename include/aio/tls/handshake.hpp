#pragma once

#include <poll.h>

#include "aio/io.hpp"
#include "aio/tls/socket.hpp"
#include "aio/tls/tls_context.hpp"
#include <openssl/ssl.h>

namespace aio::tls
{

/// @brief Performs an async TLS handshake (KTLS-compatible).
/// @note Uses SSL_set_fd + AsyncPoll to ensure OpenSSL can configure Kernel TLS.
inline Task<Result<TlsSocket>> AsyncTlsHandshake(IoContext& ctx, net::Socket sock, TlsContext& tls_ctx, bool is_server)
{
    SSL* ssl = SSL_new(tls_ctx.NativeHandle());
    if (!ssl)
        co_return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

    // Bind SSL to the socket FD.
    // This is required for OpenSSL to enable KTLS (ULP setsockopts) on the socket.
    if (SSL_set_fd(ssl, sock.Get()) != 1)
    {
        SSL_free(ssl);
        co_return ErrorFromOpenSSL();
    }

    if (is_server)
        SSL_set_accept_state(ssl);
    else
        SSL_set_connect_state(ssl);

    while (true)
    {
        // Attempt handshake step
        // OpenSSL performs syscalls internally (read/write).
        int ret = SSL_do_handshake(ssl);

        if (ret == 1)
        {
            // Handshake Success!
            break;
        }

        if (const int err = SSL_get_error(ssl, ret); err == SSL_ERROR_WANT_READ)
        {
            // OpenSSL tried to read but socket was empty.
            // Suspend the coroutine until the socket is readable (POLLIN).
            auto poll_res = co_await AsyncPoll(ctx, sock, POLLIN);
            if (!poll_res)
            {
                SSL_free(ssl);
                co_return std::unexpected(poll_res.error());
            }
        }
        else if (err == SSL_ERROR_WANT_WRITE)
        {
            // OpenSSL tried to write but socket was full.
            // Suspend the coroutine until the socket is writable (POLLOUT).
            auto poll_res = co_await AsyncPoll(ctx, sock, POLLOUT);
            if (!poll_res)
            {
                SSL_free(ssl);
                co_return std::unexpected(poll_res.error());
            }
        }
        else
        {
            // Fatal Handshake Error
            auto final_err = ErrorFromOpenSSL();
            SSL_free(ssl);
            // If ErrorFromOpenSSL didn't find a queue error, return a generic one
            if (final_err)
                co_return final_err;
            co_return std::unexpected(std::make_error_code(std::errc::connection_aborted));
        }
    }

    co_return TlsSocket(std::move(sock), ssl);
}

}  // namespace aio::tls