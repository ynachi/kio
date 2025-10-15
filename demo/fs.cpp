#include "coro/include/fs.h"

#include "coro/include/sync_wait.h"
#include "spdlog/spdlog.h"

using namespace kio;

Task<void> main_coro()
{
    IoWorkerConfig config{};
    config.uring_queue_depth = 2048;
    config.default_op_slots = 4096;

    FileManager fm(4, config);

    const auto file_name = "/home/ynachi/test_data/leipzig1m.txt";
    auto file = co_await fm.async_open(file_name, O_RDONLY, 0644);

    if (!file.has_value())
    {
        spdlog::error("failed to create file: {}" ,kio_error_to_string(file.error()));
        co_return;
    }

    spdlog::info("file opened, fd: {}", file->fd());
    // Start timing here - after all setup is complete
    auto start_time = std::chrono::high_resolution_clock::now();
    // auto read_file_coro = char_count(file.value(), 'a');
    // auto result = co_await read_file_coro;
    //
    // // End timing immediately after operation
    // auto end_time = std::chrono::high_resolution_clock::now();
    // auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
    //
    // std::cout << "Total bytes read: " << result.second << "\n";
    // std::cout << "Total character count: " << result.first << "\n";
    // std::cout << "Processing time: " << duration.count() << " microseconds (" << duration.count() / 1000.0 << " ms)\n";
    co_return;
}

int main()
{
    spdlog::set_level(spdlog::level::info);
    sync_wait(main_coro());
    return 0;
}