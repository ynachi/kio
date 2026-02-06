# Fuzzing Guide for Bitcask

This directory contains fuzz tests for the Bitcask storage engine.

## Prerequisites

- Clang compiler (clang-22 or later)
- libFuzzer (included with Clang)

## Building Fuzz Targets

### 1. Enable fuzzing in CMake

```bash
cmake --preset dev -DKIO_ENABLE_FUZZING=ON
cmake --build build --target fuzz_entry
```

Or create a dedicated fuzzing preset in `CMakePresets.json`:

```json
{
  "name": "fuzz",
  "displayName": "Fuzzing Build",
  "inherits": "base",
  "cacheVariables": {
    "CMAKE_BUILD_TYPE": "Debug",
    "KIO_BUILD_TESTS": "OFF",
    "KIO_BUILD_DEMOS": "OFF",
    "KIO_BUILD_BITCASK": "ON",
    "KIO_BUILD_BENCHMARK": "OFF",
    "KIO_ENABLE_FUZZING": "ON"
  }
}
```

Then:
```bash
cmake --preset fuzz
cmake --build build --target fuzz_entry
```

## Running Fuzz Tests

### Basic run (runs indefinitely until crash found)

```bash
./build/fuzz_entry
```

### Run with time limit (60 seconds)

```bash
./build/fuzz_entry -max_total_time=60
```

### Run with corpus directory

Create a corpus directory to save interesting test cases:

```bash
mkdir -p corpus/entry
./build/fuzz_entry corpus/entry -max_total_time=60
```

### Run with seed inputs

Create some valid DataEntry files to seed the fuzzer:

```bash
mkdir -p seeds/entry
# Add some valid entry files to seeds/entry/
./build/fuzz_entry corpus/entry seeds/entry -max_total_time=60
```

## Interpreting Results

### No crash
If the fuzzer runs successfully, you'll see output like:
```
#1000000 REDUCE cov: 52 ft: 234 corp: 45/1234b exec/s: 100000 rss: 64Mb
```

This means:
- Executed 1M inputs
- Coverage: 52 code blocks
- Corpus: 45 interesting inputs totaling 1234 bytes
- Speed: 100k executions per second

### Crash found
If a bug is found, you'll see:
```
==12345==ERROR: AddressSanitizer: heap-buffer-overflow
...
Test unit written to ./crash-abc123
```

The crash input is saved to a file. You can replay it:
```bash
./build/fuzz_entry crash-abc123
```

## Integration with CI

You can run fuzzing for a fixed duration in CI:

```bash
# Run for 5 minutes, exit 0 even if no crashes found
timeout 300 ./build/fuzz_entry corpus/entry || ([ $? -eq 124 ] && exit 0)
```

## Fuzz Targets

### `fuzz_entry`
Fuzzes `DataEntry::Deserialize()` with random byte sequences.

**What it tests:**
- CRC validation
- Buffer bounds checking
- Integer overflow in length fields
- Malformed headers
- Edge cases in key/value parsing

## Tips

1. **Start with a corpus**: Provide valid entries as seeds to help the fuzzer explore deeper
2. **Monitor coverage**: Check that coverage increases over time
3. **Run long sessions**: Fuzzing works best over hours/days
4. **Use sanitizers**: The build enables AddressSanitizer and UBSan by default
5. **Minimize crashes**: Use `-minimize_crash=1` to reduce crash inputs to minimal reproducers

## Advanced Options

```bash
# Parallel fuzzing (8 workers)
./build/fuzz_entry corpus/entry -jobs=8

# Minimize a crash case
./build/fuzz_entry -minimize_crash=1 crash-abc123

# Generate coverage report
./build/fuzz_entry corpus/entry -runs=0 -print_coverage=1

# Dictionary-based fuzzing (define common patterns)
echo -e '"key"\n"value"\n"tombstone"' > dict.txt
./build/fuzz_entry corpus/entry -dict=dict.txt
```

## References

- [libFuzzer Documentation](https://llvm.org/docs/LibFuzzer.html)
- [Efficient Fuzzing Guide](https://chromium.googlesource.com/chromium/src/+/master/testing/libfuzzer/efficient_fuzzing.md)