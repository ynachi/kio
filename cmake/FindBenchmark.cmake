# kio/cmake/FindBenchmark.cmake
include(FetchContent)

message(STATUS "Setting up Google Benchmark")

FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.4
)

FetchContent_MakeAvailable(benchmark)

# Allow benchmark to be built as a subproject
list(APPEND CMAKE_MODULE_PATH "${benchmark_SOURCE_DIR}/cmake")

# Disable testing for the benchmark library itself
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
