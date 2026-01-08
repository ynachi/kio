cmake_minimum_required(VERSION 3.30)
project(kio)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # For clang-tidy
set(GTEST_GIT_TAG v1.17.0)

# For Google CRC32
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Cmake find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# -----------------------------------------
# Common Build Functions
#------------------------------------------
function(kio_target_compile_options target_name)
    target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-gnu-statement-expression
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O3>
    )
endfunction()

function(kio_add_sanitizers target_name)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -fsanitize=thread -g)
        target_link_options(${target_name} PRIVATE -fsanitize=thread)
    endif ()
endfunction()

# -----------------------------------------
# Optional Components
#------------------------------------------
option(KIO_BUILD_TESTS "Build the kio test suite" ON)
option(KIO_BUILD_BITCASK "Build bitcask storage engine" ON)
option(KIO_BUILD_DEMOS "Build the kio demo executables" ON)
option(KIO_BUILD_BENCHMARK "Build codes used to benchmark kio against other libs" ON)
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
set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/lib64/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibUring REQUIRED IMPORTED_TARGET liburing)

#gflags
find_package(gflags REQUIRED)

# gtest
if (KIO_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif ()

# YLT
find_package(YLT REQUIRED)

# CRC32C
find_package(CRC32C REQUIRED)

# Abseil
find_package(Abseil REQUIRED)

# openssl, min 3 required
find_package(OpenSSL 3 REQUIRED)
message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
find_package(Threads REQUIRED)

# for benchmarks
if (KIO_BUILD_BENCHMARK)
    find_package(PhotonLibOS REQUIRED)
endif ()

# ============================================================================
# Development Targets
# ============================================================================

# Find clang-format
find_program(CLANG_FORMAT_EXE NAMES clang-format)

# Find clang-tidy
find_program(CLANG_TIDY_EXE NAMES clang-tidy)

# Collect all source files for formatting/linting
file(GLOB_RECURSE ALL_SOURCE_FILES
        ${CMAKE_SOURCE_DIR}/kio/*.cpp
        ${CMAKE_SOURCE_DIR}/kio/*.h
        ${CMAKE_SOURCE_DIR}/bitcask/src/*.cpp
        ${CMAKE_SOURCE_DIR}/bitcask/include/*.h
        ${CMAKE_SOURCE_DIR}/demo/*.cpp
        ${CMAKE_SOURCE_DIR}/tests/*.cpp
        ${CMAKE_SOURCE_DIR}/benchmarks/cpp/*.cpp
)

# Format target - applies clang-format
if (CLANG_FORMAT_EXE)
    add_custom_target(format
            COMMAND ${CLANG_FORMAT_EXE} -i ${ALL_SOURCE_FILES}
            COMMENT "Running clang-format on all source files"
            VERBATIM
    )

    add_custom_target(format-check
            COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${ALL_SOURCE_FILES}
            COMMENT "Checking code formatting (no changes)"
            VERBATIM
    )
    message(STATUS "Added targets: format, format-check")
else ()
    message(WARNING "clang-format not found - format targets not available")
endif ()

# Tidy target - runs clang-tidy on all sources
if (CLANG_TIDY_EXE)
    add_custom_target(tidy
            COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            ${ALL_SOURCE_FILES}
            COMMENT "Running clang-tidy on all source files"
            VERBATIM
    )

    add_custom_target(tidy-fix
            COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            --fix
            ${ALL_SOURCE_FILES}
            COMMENT "Running clang-tidy with automatic fixes"
            VERBATIM
    )
    message(STATUS "Added targets: tidy, tidy-fix")
else ()
    message(WARNING "clang-tidy not found - tidy targets not available")
endif ()

#-----------------------------------------------
# Includes
#----------------------------------------------
include_directories(${CMAKE_SOURCE_DIR})
add_subdirectory(kio)

if (KIO_BUILD_BITCASK)
    add_subdirectory(bitcask)
endif ()

if (KIO_BUILD_DEMOS)
    add_subdirectory(demo)
endif ()

# for benchmarks
if (KIO_BUILD_BENCHMARK)
    add_subdirectory(benchmarks/cpp)
endif ()

if (KIO_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(kio/tests)

    # Check target - runs all tests
    add_custom_target(check
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMENT "Running all tests"
            VERBATIM
    )

    # Quick check - only fast tests
    add_custom_target(check-quick
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L "unit"
            COMMENT "Running quick unit tests"
            VERBATIM
    )

    message(STATUS "Added targets: check, check-quick")
endif ()

add_executable(testbed main.cpp)
target_link_libraries(testbed PRIVATE kio_bitcask kio_lib)
kio_add_sanitizers(testbed)