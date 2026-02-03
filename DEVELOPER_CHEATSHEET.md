# Developer Cheat Sheet - kio

## Quick Start

```bash
# Initial setup (one time)
cmake --preset dev
cmake --build build -j$(nproc)

# Run tests
ctest --preset dev
```

## Build Configurations

### Configure Presets

```bash
# Development build (Debug + ThreadSanitizer + all features)
cmake --preset dev

# Development with clang-tidy (strict checking)
cmake --preset dev-tidy

# Release build (optimized, minimal features)
cmake --preset release

# CI build (for continuous integration)
cmake --preset ci
```

### Build Presets

```bash
# Build everything (parallel)
cmake --build --preset dev

# Build with clang-tidy checks
cmake --build --preset dev-tidy

# Build release
cmake --build --preset release

# Format all code
cmake --build --preset format

# Run clang-tidy checks
cmake --build --preset tidy
```

### Manual Build Commands

```bash
# Build everything
cmake --build build -j$(nproc)

# Build specific target
cmake --build build --target 03_http_services -j$(nproc)

# Build all examples
cmake --build build --target examples -j$(nproc)

# Clean build
rm -rf build && cmake --preset dev && cmake --build build -j$(nproc)
```

## Testing

```bash
# Run all tests
ctest --preset dev

# Run tests with verbose output
ctest --preset dev-verbose

# Run specific test
cd build && ctest -R 203_aio_net_tests

# Run tests matching pattern
cd build && ctest -R "aio.*"

# Quick test check (unit tests only)
cd build && make check-quick

# All tests
cd build && make check
```

## Code Quality

```bash
# Format code (clang-format)
cmake --build --preset format
# OR
cd build && make format

# Check formatting without changes
cd build && make format-check

# Run clang-tidy
cmake --build --preset tidy
# OR
cd build && make tidy

# Auto-fix with clang-tidy
cd build && make tidy-fix
```

## Running Examples

```bash
# After building, examples are in build/examples/
./build/examples/03_http_services --help
./build/examples/02_echo_server --port=8080
./build/examples/01_core_concepts
```

## Compiler Configuration

**Current compiler:** clang-22 (set in presets)

To use a different compiler (temporary):
```bash
CC=clang-18 CXX=clang++-18 cmake --preset dev
```

## Common Workflows

### Daily Development
```bash
# Edit code...
cmake --build build -j$(nproc)          # Build
ctest --preset dev                      # Test
./build/examples/my_example             # Run
```

### Before Committing
```bash
cmake --build --preset format           # Format code
cmake --build --preset tidy             # Check with clang-tidy
ctest --preset dev                      # Run all tests
```

### Full Clean Build
```bash
rm -rf build
cmake --preset dev
cmake --build build -j$(nproc)
ctest --preset dev
```

### Debugging Build Issues
```bash
# Verbose build
cmake --build build --verbose

# Reconfigure without cache
rm -rf build/CMakeCache.txt
cmake --preset dev
```

## Build Toggles

Control what gets built via presets or manual:

```bash
# Disable tests
cmake --preset dev -DKIO_BUILD_TESTS=OFF

# Disable examples
cmake --preset dev -DKIO_BUILD_DEMOS=OFF

# Disable benchmarks
cmake --preset dev -DKIO_BUILD_BENCHMARK=OFF

# Disable bitcask
cmake --preset dev -DKIO_BUILD_BITCASK=OFF

# Disable clang-tidy
cmake --preset dev -DENABLE_CLANG_TIDY=OFF
```

## Useful Aliases

Add to `~/.zshrc` or `~/.bashrc`:

```bash
# kio project shortcuts
alias kb='cmake --build build -j$(nproc)'
alias kt='ctest --preset dev'
alias kf='cmake --build --preset format'
alias kc='rm -rf build && cmake --preset dev && kb'
alias ke='cd build/examples && ls -lh'
```

## Directory Structure

```
kio/
├── build/                  # Build output
│   ├── examples/          # Example executables
│   ├── bin/              # Library binaries
│   └── lib/              # Compiled libraries
├── include/kio/          # Public headers
├── src/kio/              # Implementation
├── examples/             # Example programs
├── tests/                # Test files
├── bitcask/              # Bitcask storage engine
└── demo/                 # Demo programs
```

## Troubleshooting

### Compiler Issues
```bash
# Check compiler version
clang++-22 --version

# Force rebuild everything
rm -rf build && cmake --preset dev && kb
```

### clang-tidy Errors
```bash
# Disable for quick builds
cmake --preset dev -DENABLE_CLANG_TIDY=OFF

# Only check your code (not dependencies) - already configured in .clang-tidy
```

### Test Failures
```bash
# Run specific failing test with verbose output
cd build && ctest -R failing_test_name -VV

# Run with ThreadSanitizer report
TSAN_OPTIONS="verbosity=1" ./build/tests/failing_test
```
