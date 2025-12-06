cmake_minimum_required(VERSION 3.30)
project(kio)

set(CMAKE_CXX_STANDARD 23)
set(GTEST_GIT_TAG v1.17.0)

# For Google CRC32
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Cmake find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")


# -----------------------------------------
# Optional Components
#------------------------------------------
option(KIO_BUILD_TESTS "Build the kio test suite" ON)
option(KIO_BUILD_BITCASK "Build bitcask storage engine" ON)
option(KIO_BUILD_DEMOS " Build the kio demo executables" ON)

# Enable Thread Sanitizer for debug builds
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=thread -g")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -fsanitize=thread -g")
    set(CMAKE_EXE_LINKER_FLAGS_DEBUG "${CMAKE_EXE_LINKER_FLAGS_DEBUG} -fsanitize=thread")
endif ()

#-----------------------------------------
# Dependencies
#----------------------------------------
set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/lib64/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibUring REQUIRED IMPORTED_TARGET liburing)

# gtest
if (KIO_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif ()

# YLT
find_package(YLT REQUIRED)

# CRC32C
find_package(CRC32C REQUIRED)

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

if (KIO_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif ()


add_executable(testbed main.cpp)
target_link_libraries(testbed PRIVATE kio_bitcask kio_lib)