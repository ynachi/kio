cmake_minimum_required(VERSION 3.30)
project(kio)

set(CMAKE_CXX_STANDARD 23)
set(GTEST_GIT_TAG v1.17.0)

# -----------------------------------------
# Optional Components
#------------------------------------------
option(KIO_BUILD_TESTS "Build the kio test suite" ON)
option(KIO_BUILD_DEMOS "Build the kio demo executables" ON)
option(KIO_BUILD_BENCHMARKS "Build the kio benchmark executables" ON)

# Enable Thread Sanitizer for debug builds
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=thread -g")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=thread -g")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=thread")
endif ()

#-----------------------------------------
# Dependencies
#----------------------------------------
find_package(PkgConfig REQUIRED)

# gtest
if(KIO_BUILD_TESTS)
    include(cmake/FindGtest.cmake)
endif()

# benchmark
if(KIO_BUILD_BENCHMARKS)
    include(cmake/FindBenchmark.cmake)
endif()

# ylt alibaba yalantislib
include(cmake/FindYLT.cmake)

# crc32
include(cmake/FindCRC32C.cmake)

#-----------------------------------------------
# Includes
#----------------------------------------------
include_directories(${CMAKE_SOURCE_DIR})
add_subdirectory(core)
add_subdirectory(bitcask)

if(KIO_BUILD_DEMOS)
    add_subdirectory(demo)
endif()

if(KIO_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

if(KIO_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
