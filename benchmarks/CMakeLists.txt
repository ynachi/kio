add_executable(worker_startup_benchmark worker_startup_benchmark.cpp)

target_link_libraries(worker_startup_benchmark
        PRIVATE
        core_lib
        spdlog::spdlog
        benchmark::benchmark
        benchmark::benchmark_main
)

target_include_directories(worker_startup_benchmark
        PRIVATE
        ${CMAKE_SOURCE_DIR}
)

add_executable(char_count_benchmark char_count_benchmark.cpp)

target_link_libraries(char_count_benchmark
        PRIVATE
        core_lib
        spdlog::spdlog
        benchmark::benchmark
        benchmark::benchmark_main
)

target_include_directories(char_count_benchmark
        PRIVATE
        ${CMAKE_SOURCE_DIR}
)