//
// Fuzz test for DataEntry deserialization
//
#include <bitcask/entry.hpp>
#include <cstddef>
#include <cstdint>
#include <span>

using namespace bitcask;

// LibFuzzer entry point
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Skip tiny inputs that can't possibly be valid entries
    if (size < kEntryFixedHeaderSize) {
        return 0;
    }

    // Limit max size to prevent memory exhaustion (e.g., 1MB)
    if (size > 1024 * 1024) {
        return 0;
    }

    // Convert to std::span<const std::byte>
    auto byte_span = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(data),
        size
    );

    // Try to deserialize - this is what we're fuzzing
    auto result = DataEntry::Deserialize(byte_span);

    // If deserialization succeeds, verify the entry is consistent
    if (result.has_value()) {
        const auto& entry = result.value();

        // Verify internal consistency
        auto key = entry.GetKeyView();
        auto value = entry.GetValueView();

        // Re-serialize and verify it matches
        DataEntry reconstructed(key, value, entry.GetFlag(), entry.GetTimestamp());

        // The CRCs should match
        if (entry.GetCrc() != reconstructed.GetCrc()) {
            __builtin_trap(); // Signal a bug to the fuzzer
        }
    }

    return 0;
}