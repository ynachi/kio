# Build and integrate kio

The core requires Linux with io_uring, a C++23 compiler and standard library,
CMake 3.28+, pkg-config, liburing, and threads. Tests additionally use GoogleTest.
The CMake find modules fetch dependency sources as needed.

## Build the coroutine core

```sh
cmake -S . -B build -DKIO_BUILD_BITCASK=OFF -DKIO_BUILD_BENCHMARK=OFF \
    -DKIO_BUILD_TESTS=ON -DKIO_BUILD_DEMOS=ON \
    -DCMAKE_BUILD_TYPE=Debug -DKIO_SANITIZER=none
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Tests need permission to create io_uring rings. Restricted containers may deny
this even when compilation succeeds.

The `dev` preset selects Clang 22, core tests and examples, and thread sanitizer.
`dev-tidy` adds clang-tidy; `release` enables storage and disables tests/examples;
`ci` builds core tests with clang-tidy. List presets with `cmake --list-presets`.
Benchmarks are opt-in.

Debug sanitizer builds use `KIO_SANITIZER=address,undefined`, `thread`, or `none`.
Formatting and analysis targets are `format`, `format-check`, `tidy`, and `tidy-fix`.

## Optional dependencies

- `KIO_BUILD_BITCASK=ON`: CRC32C and Abseil for storage.
- `KIO_USE_MIMALLOC=ON`: mimalloc; otherwise use the normal allocator.
- `KIO_BUILD_BENCHMARK=ON`: gflags and PhotonLibOS comparison benchmarks.
- `KIO_BUILD_HTTP_BENCH=ON`: HTTP/disk comparisons, including Boost.Asio headers.

Boost and OpenSSL are not core dependencies.

## Embed with CMake

Set the desired options before adding this repository, then link the `uring`
target. Its include directory and liburing/thread dependencies propagate.

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_app LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)

set(KIO_BUILD_BITCASK OFF CACHE BOOL "" FORCE)
set(KIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KIO_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(KIO_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(KIO_SANITIZER none CACHE STRING "" FORCE)
add_subdirectory(path/to/kio)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE uring)
```
