
#include "kio/core/async_logger.h"
#include "kio/fs/fs.h"
#include "kio/sync/sync_wait.h"

using namespace kio;
using namespace kio::io;

namespace
{
Task<int64_t> CountLineChar(std::span<const char> view, char c)
{
    co_return std::count(view.begin(), view.end(), c);
}

// File IO demo
// count the number of characters in a file and the occurrences of a character
using CountReturn = std::expected<std::pair<int64_t, int64_t>, Error>;

Task<CountReturn> CharCount(File &file, const char c)
{
    constexpr size_t CHUNK_SIZE = 8192;
    char buffer[CHUNK_SIZE];
    uint64_t offset = 0;
    int64_t total_bytes_read = 0;
    int64_t char_count = 0;

    while (true)
    {
        const auto res = KIO_TRY(co_await file.AsyncRead(std::span(buffer), offset));

        if (res == 0)
        {
            ALOG_INFO("Reached end of file.");
            break;
        }

        offset += res;
        total_bytes_read += static_cast<int64_t>(res);
        char_count += co_await CountLineChar(std::span(buffer, res), c);
    }

    file.Close();
    co_return std::pair{char_count, total_bytes_read};
}

static Task<void> MainCoro(FileManager &fm)
{
    const auto *const file_name = "/home/ynachi/test_data/leipzig1m.txt";
    auto file = co_await fm.AsyncOpen(file_name, O_RDONLY, 0644);

    if (!file.has_value())
    {
        ALOG_ERROR("failed to create file: {}", file.error());
        co_return;
    }

    ALOG_INFO("file opened, fd: {}", file->Fd());
    // Start timing here - after all setup is complete
    const auto start_time = std::chrono::high_resolution_clock::now();
    auto res = co_await CharCount(file.value(), 'a');
    if (!res.has_value())
    {
        ALOG_ERROR("failed to count characters: {}", res.error());
        co_return;
    }

    // End timing immediately after operation
    const auto end_time = std::chrono::high_resolution_clock::now();
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    ALOG_INFO("Total bytes read: {}\n", res.value().first);
    ALOG_INFO("Total character count: {}\n", res.value().second);
    ALOG_INFO("Processing time: {} microseconds ({}ms)", duration.count(), duration.count() / 1000.0);
    co_return;
}
}  // namespace
int main()
{
    alog::Configure(4096, LogLevel::kInfo);
    WorkerConfig config{};
    config.uring_queue_depth = 2048;

    FileManager fm(4, config);
    SyncWait(MainCoro(fm));
}
