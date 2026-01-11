# Build and integrate kio

## Requirements

- Compiler: A C++23 compatible compiler (e.g., Clang 16+ or GCC 14+).
- CMake: Version 3.25 or higher.
- liburing (install the latest version, tested with 2.9).
```bash
# On Debian/Ubuntu/RHEL based
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

**Quick Start**

```bash
git clone https://github.com/ynachi/kio.git
cd kio

# Configure for development (Debug build with all features)
cmake --preset dev

# you can set the compiler if needed
CC=clang CXX=clang++ cmake --preset dev

# Build
cd build && make -j$(nproc)

# Run tests
make check
```

**Available Presets**

List all available presets:

```bash
cmake --list-presets
```

- `dev`      :  Development build (Debug, all features enabled, thread sanitizer)
- `dev-tidy` : Development with clang-tidy enabled during compilation
- `release`  : Optimized release build (-O3)
- `ci`       : CI environment (Debug + clang-tidy, minimal features)

**Configure Examples:**

```bash
# Development
cmake --preset dev

# Development with clang-tidy checking during build (slower)
cmake --preset dev-tidy

# Release build
cmake --preset release
```

**Build Commands**

From project root:

```bash
# Using cmake --build (works from anywhere)
cmake --build build -j$(nproc)

# Or change to build directory and use make
cd build
make -j$(nproc)
```

**Code Formatting:**

```bash
cd build
make format              # Apply clang-format to all sources
make format-check        # Check formatting without modifying (CI-friendly)
```

**Static Analysis:**

```bash
cd build
make tidy                # Run clang-tidy on all sources
make tidy-fix            # Run clang-tidy with automatic fixes
```

**Testing:**

```bash
cd build
make check               # Run all tests
make check-quick         # Run only fast unit tests
ctest --output-on-failure  # Alternative test runner
```

**Using Presets for Build Targets:**

```bash
# From project root
cmake --build --preset dev           # Build all
cmake --build build --target format  # Run format
cmake --build build --target tidy    # Run tidy
ctest --preset dev                   # Run tests
ctest --preset dev-verbose           # Verbose test output
```

**Typical dev workflow:**

```bash
# Quick iteration
cmake --preset dev
cd build && make -j$(nproc) && make check

# Before committing
cd build
make format && make tidy && make check

# Build with tidy checking (thorough but slower):
cmake --preset dev-tidy
cmake --build build -j$(nproc)

# Clean rebuild:
rm -rf build
cmake --preset dev
cd build && make -j$(nproc)

# Run specific demos:
cd build/demo
./timer_demo
./tcp_demo
```

2. Integrate using CMake
```cmake
cmake_minimum_required(VERSION 3.25)
project(my_awesome_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)

# 1. Include FetchContent
include(FetchContent)

# 2. Set options for kio *before* making it available.
#    This ensures we don't build kio's tests in our project.
set(KIO_BUILD_TESTS OFF)
set(KIO_BUILD_DEMOS OFF)
set(KIO_BUILD_BENCHMARK OFF)

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
#    kio's dependencies (liburing, openssl, etc.) are handled automatically.
target_link_libraries(my_awesome_app PRIVATE kio_lib)
```