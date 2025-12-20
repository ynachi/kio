//
// Quick sendfile test - Server sends a file immediately on connect
//

#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include "kio/core/async_logger.h"
#include "kio/core/worker.h"
#include "kio/tls/context.h"
#include "kio/tls/listener.h"
#include <thread>

using namespace kio;
using namespace kio::tls;
using namespace kio::io;

// Create a test file if it doesn't exist
void create_test_file(const char* path, const size_t size_mb)
{
    if (access(path, F_OK) == 0) return;  // Already exists

    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("create");
        return;
    }
    net::FDGuard guard(fd);

    const std::vector buf(1024 * 1024, 'X');  // 1MB of 'X'
    for (size_t i = 0; i < size_mb; i++)
    {
        write(fd, buf.data(), buf.size());
    }
    std::printf("Created test file: %s (%zu MB)\n", path, size_mb);
}

DetachedTask handle_client(TlsStream stream, const char* filepath)
{
    ALOG_INFO("Client connected, sending file via sendfile()...");

    struct stat st{};
    if (stat(filepath, &st) < 0)
    {
        ALOG_ERROR("Cannot stat file");
        co_return;
    }

    const int fd = open(filepath, O_RDONLY);
    if (fd < 0)
    {
        ALOG_ERROR("Cannot open file");
        co_return;
    }
    // ensure always closed
    net::FDGuard guard(fd);

    const auto start = std::chrono::steady_clock::now();

    // Send file size first (8 bytes)
    uint64_t size = st.st_size;
    co_await stream.async_write_exact({reinterpret_cast<char*>(&size), sizeof(size)});

    // Send file using sendfile (zero-copy with KTLS!)
    auto res = co_await stream.async_sendfile(fd, 0, size);

    const auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    if (res.has_value())
    {
        double mbps = ms > 0 ? (static_cast<double>(size) / (1024 * 1024)) / (static_cast<double>(ms) / 1000) : 0;
        ALOG_INFO("✅ Sent {} bytes in {} ms ({:.2f} MB/s)", size, ms, mbps);
    }
    else
    {
        ALOG_ERROR("sendfile failed: {}", res.error());
    }

    co_await stream.async_close();
}

DetachedTask accept_loop(Worker& worker, TlsContext& ctx, const char* filepath)
{
    ListenerConfig cfg{};
    cfg.port = 8080;
    const auto st = worker.get_stop_token();

    const auto listener = TlsListener::bind(worker, cfg, ctx);
    if (!listener)
    {
        ALOG_ERROR("Bind failed");
        co_return;
    }

    ALOG_INFO("Listening on :8080, will send: {}", filepath);

    while (!st.stop_requested())
    {
        if (auto conn = co_await listener->accept(); conn.has_value())
        {
            handle_client(std::move(*conn), filepath);
        }
        else
        {
            ALOG_WARN("Connexion failed with error: {}", conn.error());
        }
    }
}

int main(int argc, char* argv[])
{
    alog::configure(4096, LogLevel::Info);
    signal(SIGPIPE, SIG_IGN);

    auto filepath = "/tmp/testfile.bin";
    size_t size_mb = 1000;

    if (argc > 1) size_mb = std::stoull(argv[1]);

    create_test_file(filepath, size_mb);

    TlsConfig tls{};
    tls.cert_path = "/home/ynachi/test_certs/server.crt";
    tls.key_path = "/home/ynachi/test_certs/server.key";

    auto ctx_res = TlsContext::make_server(tls);
    if (!ctx_res)
    {
        ALOG_ERROR("TLS context failed");
        return 1;
    }
    auto ctx = std::move(ctx_res.value());

    constexpr WorkerConfig wcfg{};
    Worker worker(0, wcfg, [&ctx, filepath](Worker& worker) { accept_loop(worker, ctx, filepath); });
    std::jthread t([&] { worker.loop_forever(); });
    worker.wait_ready();

    ALOG_INFO("Press Enter to stop...");
    std::cin.get();
    (void) worker.request_stop();
}
