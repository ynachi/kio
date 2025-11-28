# Build and integrate kio

## Requirements

- Compiler: A C++23 compatible compiler (e.g., Clang 16+ or GCC 14+).
- CMake: Version 3.28 or higher.
- liburing (install the latest version, tested with 2.9).
```bash
# On Debian/Ubuntu
git clone https://github.com/axboe/liburing.git
cd liburing
./configure --prefix=/usr/local
make
sudo make install
```

## Build

:warning: **Note:** `gcc` is not working, we get a "compiler error (ICE) in GCC". Use `clang` instead.
`clang 20 and 22` are working fine.

1. Building as a Standalone Project

Optional Build Flags

You can disable parts of the build by passing options to CMake:

- `-DKIO_BUILD_TESTS=OFF`: Disables building the test suite.
- `-DKIO_BUILD_DEMOS=OFF`: Disables building the demo executables.
- `-DKIO_BUILD_BENCHMARKS=OFF`: Disables building the benchmarks.

This method is best to test `kio` features by running the tests and demos. 
```bash
git clone https://github.com/ynachi/kio.git

# Optional, set the compiler, here we use clang/clang++, build type, debug
CC=clang CXX=clang++ cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug

# or just build in release mode
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# Or set some flags if you want
CC=clang CXX=clang++  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DKIO_BUILD_TESTS=ON \
    -DKIO_BUILD_DEMOS=ON \
    -DKIO_BUILD_BENCHMARKS=OFF
    
# Build
cmake --build build -j 

# Run tests
cmake --build build --target test

# Run one demo demos
cd build/demo && ./timer_demo
```

2. Integrate using CMake
```cmake
cmake_minimum_required(VERSION 3.28)
project(my_awesome_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# 1. Include FetchContent
include(FetchContent)

# 2. Set options for kio *before* making it available.
#    This ensures we don't build kio's tests in our project.
set(KIO_BUILD_TESTS OFF)
set(KIO_BUILD_DEMOS OFF)
set(KIO_BUILD_BENCHMARKS OFF)

# 3. Declare the kio dependency
FetchContent_Declare(
  kio
  GIT_REPOSITORY https://github.com/ynachi/kio.git
  GIT_TAG main # Or a specific release tag, e.g., v1.0.0
)

# 4. This command downloads and configures kio
FetchContent_MakeAvailable(kio)

# 5. Define your application's executable
add_executable(my_awesome_app main.cpp)

# 6. Link against kio's core library
#    kio's dependencies (spdlog, liburing) are handled automatically.
target_link_libraries(my_awesome_app PRIVATE kio_lib)
```