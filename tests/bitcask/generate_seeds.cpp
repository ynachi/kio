//
// Generate seed corpus for fuzzing
// Compile: clang++ -std=c++23 -I../../include generate_seeds.cpp ../../src/bitcask/entry.cpp -o generate_seeds -lcrc32c
//

#include <bitcask/entry.hpp>
#include <fstream>
#include <iostream>
#include <filesystem>

using namespace bitcask;
namespace fs = std::filesystem;

void WriteSeed(const std::string& filename, const DataEntry& entry) {
    std::ofstream file(filename, std::ios::binary);
    auto span = entry.GetPayloadSpan();
    file.write(reinterpret_cast<const char*>(span.data()), span.size());
    std::cout << "Generated: " << filename << " (" << span.size() << " bytes)\n";
}

int main() {
    fs::create_directories("seeds/entry");

    // Seed 1: Minimal valid entry
    {
        DataEntry entry("k", std::as_bytes(std::span(std::string_view("v"))));
        WriteSeed("seeds/entry/minimal.bin", entry);
    }

    // Seed 2: Empty value
    {
        DataEntry entry("empty_val", {});
        WriteSeed("seeds/entry/empty_value.bin", entry);
    }

    // Seed 3: Long key
    {
        std::string long_key(256, 'k');
        std::string val = "value";
        DataEntry entry(long_key, std::as_bytes(std::span(val)));
        WriteSeed("seeds/entry/long_key.bin", entry);
    }

    // Seed 4: Long value
    {
        std::string key = "key";
        std::string long_val(1024, 'v');
        DataEntry entry(key, std::as_bytes(std::span(long_val)));
        WriteSeed("seeds/entry/long_value.bin", entry);
    }

    // Seed 5: JSON value
    {
        std::string key = "user:123";
        std::string json = R"({"name":"alice","age":30,"active":true})";
        DataEntry entry(key, std::as_bytes(std::span(json)));
        WriteSeed("seeds/entry/json_value.bin", entry);
    }

    // Seed 6: Binary value (null bytes)
    {
        std::vector<std::byte> binary_val = {
            std::byte{0x00}, std::byte{0xFF}, std::byte{0xAA}, std::byte{0x55}
        };
        DataEntry entry("binary_key", binary_val);
        WriteSeed("seeds/entry/binary_value.bin", entry);
    }

    // Seed 7: Tombstone
    {
        DataEntry entry("deleted", {}, kFlagTombstone);
        WriteSeed("seeds/entry/tombstone.bin", entry);
    }

    // Seed 8: Old timestamp
    {
        DataEntry entry("old_key", std::as_bytes(std::span(std::string_view("val"))),
                       kFlagNone, 1000000000ULL);
        WriteSeed("seeds/entry/old_timestamp.bin", entry);
    }

    std::cout << "\nSeed corpus generated in seeds/entry/\n";
    std::cout << "Use: ./build/fuzz_entry corpus/entry seeds/entry\n";

    return 0;
}