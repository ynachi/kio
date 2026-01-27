#include <array>
#include <filesystem>
#include <format>
#include <print>

#include <fcntl.h>

#include <sys/stat.h>

#include "aio/aio.hpp"
#include "aio/io_helpers.hpp"
#include "aio/logger.hpp"
#include "aio/tls/handshake.hpp"
#include "aio/tls/socket.hpp"
#include "aio/tls/tls_context.hpp"

// Simple CLI Parser
struct Config
{
    uint16_t port = 8443;
    std::string file_path = "/home/ynachi/benchmarks/10g.bin";
    std::string cert_path = "/home/ynachi/test_certs/server.crt";
    std::string key_path = "/home/ynachi/test_certs/server.key";
};

namespace
{
aio::Task<> HandleClient(aio::IoContext& ctx, int fd, aio::tls::TlsContext& tls_ctx, const int file_fd,
                         size_t file_size)
{
    aio::net::Socket client_sock(fd);

    // TLS Handshake
    auto handshake = co_await aio::tls::AsyncTlsHandshake(ctx, std::move(client_sock), tls_ctx, true);
    if (!handshake.has_value())
    {
        ALOG_ERROR("TLS Handshake failed: {}", handshake.error().message());
        co_return;
    }

    auto tls_sock = std::move(*handshake);

    // Read Request (Discard it - we assume GET /)
    std::array<std::byte, 1024> buf{};
    if (const auto read_res = co_await aio::AsyncRecv(ctx, tls_sock, buf); !read_res || *read_res == 0)
    {
        ALOG_ERROR("Client closed connection");
        co_return;
    }

    // Send HTTP Headers
    // We must send headers via standard writing before splashing the file
    const std::string header = std::format(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: {}\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Connection: close\r\n"
        "\r\n",
        file_size);

    if (auto send_res = co_await aio::AsyncSend(ctx, tls_sock, header); !send_res)
    {
        ALOG_ERROR("Failed to send headers: {}", send_res.error().message());
        co_return;
    }

    // ZERO-COPY SENDFILE
    // Splices file -> pipe -> socket (encrypted by kernel)
    if (const auto file_res = co_await aio::AsyncSendfile(ctx, tls_sock.Get(), aio::FDGuard(file_fd), 0, file_size);
        !file_res.has_value())
    {
        ALOG_ERROR("Failed to send file: {}", file_res.error().message());
        co_return;
        // Handle error (broken pipe, etc.)
    }

    // 5. Shutdown
    co_await tls_sock.AsyncShutdown(ctx);
}

aio::Task<> Server(aio::IoContext& ctx, Config cfg)
{
    // Open File Once
    const auto file_fd = co_await aio::AsyncOpen(ctx, cfg.file_path.c_str(), O_RDONLY);
    if (!file_fd.has_value())
    {
        ALOG_ERROR("Failed to open file: {}", cfg.file_path);
        co_return;
    }
    struct stat st{};
    fstat(*file_fd, &st);
    const size_t file_size = st.st_size;

    // TLS Setup
    aio::tls::TlsConfig tls_cfg;
    tls_cfg.cert_path = cfg.cert_path;
    tls_cfg.key_path = cfg.key_path;
    // We disable ALPN/mTLS for raw throughput testing to match standard Nginx config
    auto tls_ctx = aio::tls::TlsContext::Create(tls_cfg, true).value();

    auto listener = aio::net::TcpListener::Bind(cfg.port).value();
    ALOG_INFO("🚀 Zero-Copy File Server running on port {}", cfg.port);
    ALOG_INFO("📂 Serving: {} ({:.2f} GB)", cfg.file_path, static_cast<double>(file_size) / (1024 * 1024 * 1024));

    aio::TaskGroup tasks(1024);

    while (true)
    {
        if (auto accept = co_await aio::AsyncAccept(ctx, listener.Get()))
        {
            // Dup file_fd so each client has its own seek position reference if needed,
            // though AsyncSendfile uses pread/splice with offsets.
            // Actually, AsyncSendfile takes a raw FD. We can share the FD if we trust
            // pread behavior, but Splice usually requires offsets.
            // For safety in this demo, we'll pass the shared FD as AsyncSendfile handles offsets.
            tasks.Spawn(HandleClient(ctx, accept->fd, tls_ctx, file_fd.value(), file_size));
        }
    }
}
}  // namespace

int main()
{
    aio::IoContext ctx(4096);
    ctx.RunUntilDone(Server(ctx, Config{}));
    return 0;
}