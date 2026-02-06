#!/bin/bash
# Quick script to create seed corpus from the unit test

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR/../.."
BUILD_DIR="$PROJECT_ROOT/build"

# Create directories
mkdir -p seeds/entry
mkdir -p corpus/entry

echo "Building test to generate seeds..."
cd "$PROJECT_ROOT"

# Create a simple seed generator using the test
cat > /tmp/gen_seed.cpp << 'EOF'
#include <bitcask/entry.hpp>
#include <fstream>
#include <iostream>

using namespace bitcask;

int main() {
    // Generate several valid seeds
    struct Seed {
        const char* filename;
        const char* key;
        const char* value;
        uint8_t flag;
    } seeds[] = {
        {"seed_minimal.bin", "k", "v", kFlagNone},
        {"seed_empty_value.bin", "key", "", kFlagNone},
        {"seed_tombstone.bin", "deleted_key", "", kFlagTombstone},
        {"seed_long_key.bin", "this_is_a_very_long_key_name_that_tests_key_length", "value", kFlagNone},
        {"seed_long_value.bin", "key", "this is a very long value string that tests value length handling in the deserializer", kFlagNone},
        {"seed_json.bin", "user:123", "{\"name\":\"alice\",\"age\":30}", kFlagNone},
    };

    for (const auto& seed : seeds) {
        DataEntry entry(seed.key,
                       std::as_bytes(std::span(std::string_view(seed.value))),
                       seed.flag);

        std::ofstream file(std::string("seeds/entry/") + seed.filename, std::ios::binary);
        auto span = entry.GetPayloadSpan();
        file.write(reinterpret_cast<const char*>(span.data()), span.size());

        std::cout << "Created: " << seed.filename << " (" << span.size() << " bytes)\n";
    }

    std::cout << "\nSeeds created in seeds/entry/\n";
    return 0;
}
EOF

# Compile and run seed generator
echo "Compiling seed generator..."
clang++-22 -std=c++23 \
    -I"$PROJECT_ROOT/include" \
    /tmp/gen_seed.cpp \
    "$PROJECT_ROOT/build/CMakeFiles/bitcask.dir/src/bitcask/entry.cpp.o" \
    "$PROJECT_ROOT/build/libbitcask.a" \
    "$PROJECT_ROOT/build/libkio.a" \
    "$PROJECT_ROOT/build/_deps/crc32c-build/libcrc32c.a" \
    -luring -lssl -lcrypto \
    -o /tmp/gen_seed

echo "Generating seeds..."
cd "$PROJECT_ROOT/tests/bitcask"
/tmp/gen_seed

echo ""
echo "Seed corpus created! Now run:"
echo "  ./build/tests/fuzz_entry corpus/entry seeds/entry -max_total_time=300"
