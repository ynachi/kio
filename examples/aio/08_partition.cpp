#include "kio/kio.hpp"
#include "bitcask/partition.hpp"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace
{
    kio::Task<> MainTask(kio::IoContext& ctx)
    {
        using namespace std::chrono_literals;
        bitcask::BitcaskConfig cfg;
        cfg.directory = "/tmp/bitcask_demo";
        cfg.Validate();
        cfg.compaction_interval_s = 50ms;

        // Partition expects: <directory>/partition_<id> to exist.
        std::filesystem::create_directories(cfg.directory / "partition_0");

        auto open_res = co_await bitcask::Partition::AsyncOpen(ctx, cfg, 0);
        if (!open_res)
        {
            // handle open_res.error()
            co_return;
        }
        auto& part = open_res.value();

        std::string value = "hello";
        auto put_res = co_await part->Put(ctx, "k1", std::as_bytes(std::span(value)));
        if (!put_res)
        {
            // handle put_res.error()
            co_await part->AsyncClose(ctx);
            co_return;
        }

        std::string value2 = "hello";
        auto put_res2 = co_await part->Put(ctx, "k2", std::as_bytes(std::span(value)));
        if (!put_res2)
        {
            // handle put_res.error()
            co_await part->AsyncClose(ctx);
            co_return;
        }

        auto get_res = co_await part->Get(ctx, "k1");
        if (get_res && get_res.value())
        {
            auto bytes = get_res.value().value();
            std::string out(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            // use out
        }

        co_await part->Del(ctx, "k1");
        co_await part->AsyncClose(ctx);
    }
} // namespace

int main()
{
    kio::alog::g_level = kio::alog::Level::Debug;
    kio::IoContext ctx;
    ctx.RunUntilDone(MainTask(ctx));
    return 0;
}
