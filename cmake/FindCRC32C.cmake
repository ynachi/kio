include(FetchContent)

message(STATUS "Setting up google/crc32c")

set(CRC32C_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CRC32C_INSTALL OFF CACHE BOOL "" FORCE)
set(CRC32C_USE_GLOG OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
        crc32c
        GIT_REPOSITORY https://github.com/google/crc32c.git
        GIT_TAG        1.1.2
)

FetchContent_MakeAvailable(crc32c)