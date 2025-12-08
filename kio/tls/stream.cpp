//
// Created by Yao ACHI on 07/12/2025.
//
#include "stream.h"

#include <linux/tls.h>

namespace kio::tls
{
    TlSStream::TlSStream(io::Worker& worker, const int fd, const SslContext& ctx, std::string_view server_name) : worker_(worker), fd_(fd), server_name_(server_name)
    {
        ssl_ = SSL_new(ctx.get());
        if (!ssl_)
        {
            throw std::runtime_error("Failed to create SSL object");
        }

        // Bind socket to SSL
        if (SSL_set_fd(ssl_, fd_) == 0)
        {
            throw std::runtime_error("Failed to bind FD to SSL");
        }

        // Explicitly set the handshake state based on context
        if (ctx.is_server())
        {
            SSL_set_accept_state(ssl_);
        }
        else
        {
            SSL_set_connect_state(ssl_);

            // Set SNI hostname (Only valid for clients)
            if (!server_name_.empty())
            {
                SSL_set_tlsext_host_name(ssl_, server_name_.c_str());
            }
        }
    }

    Task<Result<void>> TlSStream::async_handshake()
    {
        // Perform handshake
        const auto st = worker_.get_stop_token();
        while (!st.stop_requested())
        {
            const int ret = SSL_do_handshake(ssl_);

            if (ret == 1)
            {
                // Handshake complete!
                break;
            }

            if (const int err = SSL_get_error(ssl_, ret); err == SSL_ERROR_WANT_READ)
            {
                if (auto poll_result = co_await worker_.async_poll(fd_, POLLIN); !poll_result)
                {
                    co_return std::unexpected(poll_result.error());
                }
            }
            else if (err == SSL_ERROR_WANT_WRITE)
            {
                if (auto poll_result = co_await worker_.async_poll(fd_, POLLOUT); !poll_result)
                {
                    co_return std::unexpected(poll_result.error());
                }
            }
            else
            {
                // Fatal handshake error
                char err_buf[256];
                ERR_error_string_n(ERR_get_error(), err_buf, sizeof(err_buf));
                ALOG_ERROR("TLS handshake failed: {}", err_buf);
                co_return std::unexpected(Error{ErrorCategory::Application, kTlsHandshakeFailed});
            }
        }

        // CRITICAL: Verify KTLS was enabled
        BIO* wbio = SSL_get_wbio(ssl_);
        BIO* rbio = SSL_get_rbio(ssl_);

        bool tx_ktls = wbio ? BIO_get_ktls_send(wbio) : false;
        bool rx_ktls = rbio ? BIO_get_ktls_recv(rbio) : false;

        if (!tx_ktls || !rx_ktls)
        {
            // KTLS not available - FAIL FAST
            ALOG_ERROR("KTLS not enabled after handshake!");
            ALOG_ERROR("  TX KTLS: {}", tx_ktls ? "YES" : "NO");
            ALOG_ERROR("  RX KTLS: {}", rx_ktls ? "YES" : "NO");
            ALOG_ERROR("  Cipher: {}", SSL_CIPHER_get_name(SSL_get_current_cipher(ssl_)));
            ALOG_ERROR("This library requires KTLS. Check:");
            ALOG_ERROR("  1. Kernel version >= 5.1 (for TLS 1.3)");
            ALOG_ERROR("  2. 'lsmod | grep tls' shows tls module loaded");
            ALOG_ERROR("  3. OpenSSL 3.0+ is being used");

            co_return std::unexpected(Error{ErrorCategory::Application, kTlsKtlsNotEnabled});
        }

        ALOG_INFO("✅ KTLS enabled successfully!");
        ALOG_INFO("  Version: TLS 1.3");
        ALOG_INFO("  Cipher: {}", SSL_CIPHER_get_name(SSL_get_current_cipher(ssl_)));
        ALOG_INFO("  All encryption now handled by kernel (zero-copy mode)");

        // Ensure socket remains non-blocking for io_uring operations
        if (fcntl(fd_, F_SETFL, O_NONBLOCK) < 0)
        {
            ALOG_ERROR("Failed to set socket non-blocking after KTLS handshake");
            co_return std::unexpected(Error::from_errno(errno));
        }

        // CRITICAL: Do NOT free the SSL object!
        // OpenSSL 3.x has a known bug where freeing the SSL object after KTLS
        // enablement causes recv() to return EIO instead of proper error codes.
        // See: https://github.com/nginx/nginx/issues/651
        // The SSL object must remain alive for the entire connection lifetime.

        co_return {};
    }

    Task<Result<void>> TlSStream::shutdown_goodbye()
    {
        ALOG_INFO("shutting down TLS");

        // 1. Construct the TLS Alert Payload {Warning, CloseNotify}
        char alert_buffer[2] = {0x01, 0x00};

        iovec iov{};
        iov.iov_base = alert_buffer;
        iov.iov_len = sizeof(alert_buffer);

        // 2. Prepare CMSG buffer
        char cmsg_buf[CMSG_SPACE(sizeof(unsigned char))] = {};

        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        // Use the full buffer size, relying on the zero-padding to terminate parsing
        msg.msg_controllen = sizeof(cmsg_buf);

        // 3. Fill the CMSG
        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        if (cmsg == nullptr)
        {
            ALOG_ERROR("CMSG is null");
            co_return {};
        }
        cmsg->cmsg_level = SOL_TLS;
        cmsg->cmsg_type = TLS_SET_RECORD_TYPE;
        cmsg->cmsg_len = CMSG_LEN(sizeof(unsigned char));
        *CMSG_DATA(cmsg) = 21;  // TLS_RECORD_TYPE_ALERT

        // 4. Send asynchronously

        if (auto res = co_await worker_.async_sendmsg(fd_, &msg, 0); !res)
        {
            ALOG_WARN("Failed to send TLS close_notify: {}", res.error());
        }

        co_return {};
    }
}  // namespace kio::tls
