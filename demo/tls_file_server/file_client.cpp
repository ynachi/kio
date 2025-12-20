//
// Quick sendfile test - Client receives file from server
//

#include "kio/core/async_logger.h"
#include "kio/core/worker.h"
#include "kio/sync/sync_wait.h"
#include "kio/tls/context.h"
#include "kio/tls/listener.h"

using namespace kio;
using namespace kio::tls;
using namespace kio::io;

Task<Result<void>> run(Worker& worker, TlsContext& ctx)
{
    co_await SwitchToWorker(worker);

    TlsConnector connector(worker, ctx);
    auto stream = KIO_TRY(co_await connector.connect("127.0.0.1", 8080));

    ALOG_INFO("Connected! KTLS: {}", stream.is_ktls_active() ? "YES" : "no");

    // Read file size (8 bytes)
    uint64_t size = 0;
    auto p = reinterpret_cast<char*>(&size);
    size_t got = 0;
    while (got < 8)
    {
        auto r = co_await stream.async_read({p + got, 8 - got});
        if (!r || *r == 0) co_return std::unexpected(Error{ErrorCategory::kNetwork, EIO});
        got += *r;
    }

    ALOG_INFO("Receiving {} bytes ({} MB)...", size, size / (1024 * 1024));

    std::vector<char> buf(65536);
    size_t remaining = size;
    size_t total = 0;

    auto start = std::chrono::steady_clock::now();

    while (remaining > 0)
    {
        auto r = co_await stream.async_read({buf.data(), std::min(buf.size(), remaining)});
        if (!r.has_value())
        {
            ALOG_ERROR("Read error: {}", r.error());
            break;
        }
        if (*r == 0) break;
        remaining -= *r;
        total += *r;
    }

    auto end = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    double mbps = ms > 0 ? (static_cast<double>(total) / (1024 * 1024)) / (static_cast<double>(ms) / 1000) : 0;

    ALOG_INFO("✅ Received {} bytes in {} ms ({:.2f} MB/s)", total, ms, mbps);

    co_await stream.async_close();
    co_return {};
}

int main()
{
    alog::configure(4096, LogLevel::Info);

    TlsConfig tls{};
    tls.verify_mode = SSL_VERIFY_NONE;

    auto ctx = TlsContext::make_client(tls);
    if (!ctx)
    {
        ALOG_ERROR("TLS failed");
        return 1;
    }

    constexpr WorkerConfig wcfg{};
    Worker worker(0, wcfg);
    std::jthread t([&] { worker.LoopForever(); });
    worker.WaitReady();

    auto res = SyncWait(run(worker, *ctx));
    if (!res) ALOG_ERROR("Failed: {}", res.error());

    (void) worker.RequestStop();
    return res ? 0 : 1;
}
