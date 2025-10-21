#include <iostream>

#include "core/include/fs.h"
#include "core/include/sync_wait.h"
#include "spdlog/spdlog.h"

using namespace kio;
using namespace kio::io;

Task<int64_t> CountLineChar(std::span<const char> view, char c) { co_return std::count(view.begin(), view.end(), c); }

// File IO demo
// count the number of characters in a file and the occurrences of a character
using CountReturn = std::expected<std::pair<int64_t, int64_t>, Error>;
Task<CountReturn> char_count(File& file, const char c)
{
    constexpr size_t CHUNK_SIZE = 8192;
    char buffer[CHUNK_SIZE];
    uint64_t offset = 0;
    int64_t total_bytes_read = 0;
    int64_t char_count = 0;

    while (true)
    {
        const auto res = KIO_TRY(co_await file.async_read(std::span(buffer), offset));

        if (res == 0)
        {
            spdlog::info("Reached end of file.");
            break;
        }

        offset += res;
        total_bytes_read += static_cast<int64_t>(res);
        char_count += co_await CountLineChar(std::span(buffer, res), c);
    }

    file.close();
    co_return std::pair{char_count, total_bytes_read};
}

Task<void> main_coro(FileManager& fm)
{
    const auto file_name = "/home/ynachi/test_data/leipzig1m.txt";
    auto file = co_await fm.async_open(file_name, O_RDONLY, 0644);

    if (!file.has_value())
    {
        spdlog::error("failed to create file: {}", file.error().message());
        co_return;
    }

    spdlog::info("file opened, fd: {}", file->fd());
    // Start timing here - after all setup is complete
    const auto start_time = std::chrono::high_resolution_clock::now();
    auto result = co_await char_count(file.value(), 'a');
    if (!result)
    {
        spdlog::error(result.error().message());
    }

    // End timing immediately after operation
    const auto end_time = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    auto value = result.value();

    std::cout << "Total bytes read: " << value.second << "\n";
    std::cout << "Total character count: " << value.first << "\n";
    std::cout << "Processing time: " << duration.count() << " microseconds (" << duration.count() / 1000.0 << " ms)\n";
    co_return;
}

int main()
{
    spdlog::set_level(spdlog::level::info);
    WorkerConfig config{};
    config.uring_queue_depth = 2048;
    config.default_op_slots = 4096;

    FileManager fm(4, config);
    SyncWait(main_coro(fm));
}
