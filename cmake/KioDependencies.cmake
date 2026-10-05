include(FetchContent)
include(ExternalProject)

function(kio_configure_dependencies)
    # google/crc32c 1.1.2 still declares a pre-3.5 CMake policy baseline.
    # CMake 4 removed that compatibility mode, so provide the supported floor
    # without modifying the dependency's source tree.
    if (CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND
            NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    endif ()

    #
    # MIMALLOC
    #
    if (KIO_USE_MIMALLOC)
        set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(mimalloc
                GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
                GIT_TAG v3.3.2 GIT_SHALLOW ON)
        FetchContent_MakeAvailable(mimalloc)
    endif ()

    # openssl, min 3 required
    find_package(OpenSSL 3 REQUIRED)
    message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
    find_package(Threads REQUIRED)

    #
    # LIBURING
    #
    # kio needs liburing >= 2.9 (io_uring_prep_ftruncate). Distributions lag, so the
    # default is to build a pinned tag as a static library inside the build tree.
    set(KIO_LIBURING_TAG liburing-2.9)
    if (KIO_FETCH_LIBURING)
        set(_lu_src ${CMAKE_BINARY_DIR}/_deps/liburing)
        set(_lu_lib ${_lu_src}/src/liburing.a)
        ExternalProject_Add(liburing_ext
                GIT_REPOSITORY https://github.com/axboe/liburing.git
                GIT_TAG ${KIO_LIBURING_TAG}
                GIT_SHALLOW ON
                SOURCE_DIR ${_lu_src}
                BUILD_IN_SOURCE ON
                UPDATE_DISCONNECTED ON
                CONFIGURE_COMMAND ./configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
                BUILD_COMMAND make -C src -j4 liburing.a
                INSTALL_COMMAND ""
                BUILD_BYPRODUCTS ${_lu_lib})
        file(MAKE_DIRECTORY ${_lu_src}/src/include)
        add_library(kio_liburing STATIC IMPORTED GLOBAL)
        set_target_properties(kio_liburing PROPERTIES
                IMPORTED_LOCATION ${_lu_lib}
                INTERFACE_INCLUDE_DIRECTORIES ${_lu_src}/src/include)
        add_dependencies(kio_liburing liburing_ext)
        add_library(kio::liburing ALIAS kio_liburing)
    else ()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LibUring REQUIRED IMPORTED_TARGET liburing>=2.9)
        add_library(kio::liburing ALIAS PkgConfig::LibUring)
    endif ()

    #
    # SPDLOG
    #
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG v1.17.0
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(spdlog)

    #
    # CRC32
    #
    if (KIO_BUILD_BITCASK)
        set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(CRC32C_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(CRC32C_USE_GLOG OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                crc32c
                GIT_REPOSITORY https://github.com/google/crc32c.git
                GIT_TAG 1.1.2
                GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(crc32c)

        if (TARGET crc32c AND NOT TARGET crc32c::crc32c)
            add_library(crc32c::crc32c ALIAS crc32c)
        endif ()
    endif ()

    #
    # GTEST
    #
    if (KIO_BUILD_TESTS)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                googletest
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG v1.17.0
                GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(googletest)
    endif ()

    #
    # ABSEIL
    #
    if (KIO_BUILD_BITCASK)
        message(STATUS "Setting up Google Abseil")

        FetchContent_Declare(
                abseil-cpp
                GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
                GIT_TAG 20260107.1
                EXCLUDE_FROM_ALL                  # Don't build Abseil's tests/docs by default
        )

        # Configure Abseil build options BEFORE MakeAvailable
        set(ABSL_BUILD_TESTING OFF CACHE BOOL "Build Abseil tests" FORCE)
        set(ABSL_BUILD_DOCS OFF CACHE BOOL "Build Abseil docs" FORCE)
        set(ABSL_USE_EXTERNAL_GOOGLETEST ON CACHE BOOL "Use system googletest if needed" FORCE)
        set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "Don't install Abseil" FORCE)  # Optional

        FetchContent_MakeAvailable(abseil-cpp)
    endif ()

    #
    # BENCHMARK
    #
    if (KIO_BUILD_BENCHMARKS)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                benchmark
                GIT_REPOSITORY https://github.com/google/benchmark.git
                GIT_TAG v1.9.5
                GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(benchmark)

        FetchContent_Declare(
                photon
                GIT_REPOSITORY https://github.com/ynachi/PhotonLibOS.git
                GIT_TAG main
        )
        FetchContent_MakeAvailable(photon)
    endif ()
endfunction()
