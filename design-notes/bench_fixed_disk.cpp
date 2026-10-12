// Throwaway bench: registered-buffer disk reads vs normal reads.
// Usage: bench_fixed_disk <normal|fixed> <rand|seq> <block> <threads> <depth> <ops> <file>
// The file must exist and be dense; run order must be identical for a fair A/B.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "kio/buffer_pool.hpp"
#include "kio/context.h"
#include "kio/operations.h"
#include "kio/task.h"

using namespace kio;
using namespace std::chrono_literals;

namespace
{
    std::atomic<uint64_t> g_ops{0};
    std::atomic<uint64_t> g_bytes{0};
    std::atomic<uint64_t> g_errors{0};

    inline uint64_t next_rand(uint64_t& x)
    {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    }

    Task<> worker(io::Context& io, io::UniqueFd& fd, const uint64_t file_blocks, const unsigned block,
                  const bool fixed, const bool seq, const uint64_t nops, const unsigned worker_id,
                  const unsigned depth)
    {
        uint64_t rng = (0x9E3779B97F4A7C15ULL * (worker_id + 1)) | 1;
        // O_DIRECT demands a page-aligned buffer for the normal path.
        std::byte* heap_buf = nullptr;
        if (!fixed)
        {
            if (posix_memalign(reinterpret_cast<void**>(&heap_buf), 4096, block) != 0) co_return {};
        }
        uint64_t seq_off = static_cast<uint64_t>(worker_id) * block;

        for (uint64_t i = 0; i < nops; ++i)
        {
            const uint64_t off = seq ? seq_off = (seq_off + static_cast<uint64_t>(depth) * block)
                                           % (file_blocks * block)
                                     : (next_rand(rng) % file_blocks) * block;

            if (fixed)
            {
                auto acquired = co_await io.acquire_buffer();
                if (!acquired)
                {
                    ++g_errors;
                    break;
                }
                BufferLease lease = std::move(*acquired);
                auto r = co_await async_read_fixed(io, fd, lease, block, 0, off);
                if (!r || *r != static_cast<int>(block))
                {
                    ++g_errors;
                    if (g_errors < 4)
                    {
                        std::println(stderr, "read_fixed at {}: {} ({})", off,
                                     r ? "short" : r.error().message(), r ? *r : 0);
                    }
                }
                lease.reset();
            }
            else
            {
                auto r = co_await async_read(io, fd, std::span{heap_buf, block}, off);
                if (!r || *r != static_cast<int>(block))
                {
                    ++g_errors;
                    if (g_errors < 4)
                    {
                        std::println(stderr, "read at {}: {} ({})", off,
                                     r ? "short" : r.error().message(), r ? *r : 0);
                    }
                }
            }
            g_ops.fetch_add(1, std::memory_order_relaxed);
            g_bytes.fetch_add(block, std::memory_order_relaxed);
        }
        free(heap_buf);
        co_return {};
    }

    void serve(const unsigned id, const std::string& file, const unsigned block, const bool fixed,
               const bool seq, const uint64_t nops, const unsigned depth, const bool odirect)
    {
        // Pool slots are far above depth: acquire never parks in this bench.
        BufferPoolConfig cfg{.slot_size = 65536, .slots = 128};
        io::Context io(id, nullptr, io::IoOptions{},
                       fixed ? std::optional<BufferPoolConfig>{cfg} : std::nullopt);
        const int raw = open(file.c_str(), O_RDONLY | (odirect ? O_DIRECT : 0));
        if (raw < 0)
        {
            std::perror("open");
            std::exit(1);
        }
        io::UniqueFd fd(raw);
        const uint64_t file_bytes = static_cast<uint64_t>(lseek(raw, 0, SEEK_END));
        const uint64_t file_blocks = file_bytes / block;

        std::vector<Task<>> tasks;
        for (unsigned d = 0; d < depth; ++d)
        {
            tasks.push_back(worker(io, fd, file_blocks, block, fixed, seq, nops, d, depth));
        }
        for (auto& t : tasks) io.spawn(std::move(t));
        io.run();
    }
} // namespace

int main(int argc, char** argv)
{
    if (argc < 8)
    {
        std::println(stderr, "usage: {} <normal|fixed> <rand|seq> <block> <threads> <depth> <ops-per-worker> <file>",
                     argv[0]);
        return 1;
    }
    const bool fixed = std::string_view{argv[1]} == "fixed";
    const bool seq = std::string_view{argv[2]} == "seq";
    const bool odirect = argc > 8 && std::string_view{argv[8]} == "odirect";
    const unsigned block = static_cast<unsigned>(std::atoi(argv[3]));
    const unsigned threads = static_cast<unsigned>(std::atoi(argv[4]));
    const unsigned depth = static_cast<unsigned>(std::atoi(argv[5]));
    const uint64_t ops_per_worker = std::strtoull(argv[6], nullptr, 10);
    const std::string file = argv[7];

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> ts;
    for (unsigned i = 1; i < threads; ++i)
    {
        ts.emplace_back(serve, i, file, block, fixed, seq, ops_per_worker, depth, odirect);
    }
    serve(0, file, block, fixed, seq, ops_per_worker, depth, odirect);
    for (auto& t : ts) t.join();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();

    const uint64_t ops = g_ops.load();
    const uint64_t bytes = g_bytes.load();
    std::println("{:>6} {:>4} block={} threads={} depth={} -> {:.1f} KIOPS, {:.1f} MiB/s, "
                 "errors={}, wall={}ms{}",
                 fixed ? "fixed" : "normal", seq ? "seq" : "rand", block, threads, depth,
                 ops / 1000.0 / (ms / 1000.0), bytes / 1048576.0 / (ms / 1000.0), g_errors.load(),
                 ms, odirect ? " [O_DIRECT]" : "");
    return g_errors.load() == 0 ? 0 : 2;
}
