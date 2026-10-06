cmake_minimum_required(VERSION 3.28)
project(kio)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(GTEST_GIT_TAG v1.17.0)

# For Google CRC32
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Cmake find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

option(ENABLE_AIO_STATS "Enable AIO observability metrics" ON)
if (ENABLE_AIO_STATS)
    add_compile_definitions(AIO_STATS=1)
endif ()

# -----------------------------------------
# Optional Components
#------------------------------------------
option(KIO_BUILD_TESTS "Build the test suite" ON)
option(KIO_BUILD_DEMOS "Build the example programs" ON)
option(KIO_BUILD_BENCH "Build the HTTP benchmark server" OFF)
option(KIO_BUILD_BITCASK "Build the bitcask library (Abseil, CRC32C)" OFF)
option(KIO_USE_MIMALLOC "Link mimalloc as the global allocator" OFF)
option(KIO_FETCH_LIBURING "Build a pinned liburing instead of the system one" ON)
set(KIO_SANITIZER "none" CACHE STRING "none | address (ASan+UBSan) | thread (TSan)")
set_property(CACHE KIO_SANITIZER PROPERTY STRINGS none address thread)
option(ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)

# Enable clang-tidy during build if requested
if (ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if (CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
    else ()
        message(WARNING "clang-tidy requested but not found")
    endif ()
endif ()


#-----------------------------------------
# Dependencies
#----------------------------------------
find_program(KIO_CCACHE ccache)
if (KIO_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER "${KIO_CCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${KIO_CCACHE}")
endif ()
find_program(KIO_MOLD mold)
if (KIO_MOLD)
    add_link_options(-fuse-ld=mold)
endif ()

include(KioWarnings)
include(KioDependencies)
include(KioSanitizers)
include(KioHelpers)

kio_configure_dependencies()
kio_configure_sanitizers()
kio_configure_warnings()

include_directories(${CMAKE_SOURCE_DIR})

#-----------------------------------------------
# Aio lib, change later to kio
#----------------------------------------------
# Library definition
add_library(uring
        src/uring/logger.cpp
        src/uring/io.cc
        src/uring/net.cc
        src/uring/tcp_listener.cc
        src/uring/buffer_pool.cc
)
# Include directories
target_include_directories(uring PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# Link dependencies
target_include_directories(uring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(uring PUBLIC
        kio::liburing
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
        spdlog::spdlog
        PRIVATE kio_warnings)
if (KIO_USE_MIMALLOC)
    target_link_libraries(uring PUBLIC mimalloc-static)
endif ()

#-----------------------------------------------
# bitcask lib
#----------------------------------------------
# Library definition
if (KIO_BUILD_BITCASK)
    add_library(bitcask
            src/bitcask/cache.cc
            src/bitcask/segment.cc
    )

    # Include directories
    target_include_directories(bitcask PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    # Link dependencies
    target_link_libraries(bitcask
            PUBLIC
            uring
            absl::flat_hash_map
            absl::flat_hash_set
            absl::inlined_vector
            PRIVATE
            crc32c
    )
endif ()


if (KIO_BUILD_DEMOS)
    add_subdirectory(examples)
endif ()

# for benchmarks
if (KIO_BUILD_BENCHMARK)
    add_subdirectory(benchmarks/cpp)
endif ()

if (KIO_BUILD_TESTS)
    include(CTest)
    enable_testing()
    # Defines gtest_discover_tests(). FetchContent(googletest) only supplies the
    # GTest:: targets, so without this the call in kio_add_test is unknown.
    include(GoogleTest)
    add_subdirectory(tests)
    message(STATUS "Added targets: check, check-quick")
endif ()

