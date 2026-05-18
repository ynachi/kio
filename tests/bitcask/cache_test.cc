#include "bitcask/cache.hpp"

#include <string>

#include <gtest/gtest.h>

using namespace bitcask;

namespace
{
// ============================================================================
// Test Fixture with shared setup
// ============================================================================
class CacheTest : public ::testing::Test
{
protected:
    using StrCache = Cache<std::string, int>;

    void SetUp() override { pmr_ = std::make_unique<TrackingResource>(); }

    void TearDown() override
    {
        // Verify no leaks: all allocated nodes should be deallocated
        EXPECT_EQ(pmr_->net_allocations(), 0)
            << "Memory leak detected: " << pmr_->net_allocations() << " allocations not freed";
    }

    std::unique_ptr<TrackingResource> pmr_;
};
}  // namespace

// ============================================================================
// Basic Operations Tests
// ============================================================================

TEST_F(CacheTest, ConstructWithZeroCapacityThrows)
{
    EXPECT_THROW(StrCache(0, pmr_.get()), std::invalid_argument);
}

TEST_F(CacheTest, EmptyCacheInitialState) {
    StrCache cache(10, pmr_.get());
    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.contains("key"));
    EXPECT_EQ(cache.get("key"), std::nullopt);
}

TEST_F(CacheTest, PutAndGetSingleItem) {
    StrCache cache(1, pmr_.get());

    cache.put("key1", 100);

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.empty());
    EXPECT_TRUE(cache.contains("key1"));
    EXPECT_EQ(cache.get("key1"), 100);
    EXPECT_EQ(cache.get("missing"), std::nullopt);
}

TEST_F(CacheTest, UpdateExistingKey) {
    StrCache cache(3, pmr_.get());

    cache.put("key", 1);
    EXPECT_EQ(cache.get("key"), 1);

    cache.put("key", 2);  // Update
    EXPECT_EQ(cache.size(), 1u);  // Size unchanged
    EXPECT_EQ(cache.get("key"), 2);  // New value
}

TEST_F(CacheTest, ContainsDoesNotMarkVisited) {
    StrCache cache(2, pmr_.get());

    cache.put("a", 1);
    cache.put("b", 2);

    // Touch "a" via contains (should NOT mark visited)
    EXPECT_TRUE(cache.contains("a"));

    // Insert "c": should evict "a" (oldest, unvisited) not "b"
    cache.put("c", 3);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
}

// ============================================================================
// Sieve Eviction Semantics Tests
// ============================================================================

TEST_F(CacheTest, EvictUnvisitedOldest) {
    StrCache cache(3, pmr_.get());

    // Insert 3 items: a (oldest) -> b -> c (newest)
    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);

    // Access "b" to mark visited
    (void)cache.get("b");

    // Insert "d": should evict "a" (oldest, unvisited)
    cache.put("d", 4);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_TRUE(cache.contains("d"));
    EXPECT_EQ(cache.size(), 3u);
}

TEST_F(CacheTest, SecondChanceThenEvict) {
    StrCache cache(3, pmr_.get());

    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);

    // Mark all as visited
    (void)cache.get("a");
    (void)cache.get("b");
    (void)cache.get("c");

    // Insert "d": Sieve scan resets visited flags, then evicts oldest
    cache.put("d", 4);

    // "a" had visited=true, so it got reset to false, then evicted
    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));  // visited reset but survived this round
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_TRUE(cache.contains("d"));

    // Insert "e": now "b" is oldest unvisited
    cache.put("e", 5);
    EXPECT_FALSE(cache.contains("b"));
}

TEST_F(CacheTest, HandPointerWrapAround) {
    StrCache cache(4, pmr_.get());

    // Fill cache and mark all visited
    for (int i = 0; i < 4; ++i) {
        cache.put(std::to_string(i), i);
        (void)cache.get(std::to_string(i));  // Mark visited
    }

    // Insert 4 more items: each should evict one after resetting visited
    for (int i = 4; i < 8; ++i) {
        cache.put(std::to_string(i), i);
        EXPECT_EQ(cache.size(), 4u);
    }

    // Verify final state: should have items 4,5,6,7
    for (int i = 0; i < 4; ++i) {
        EXPECT_FALSE(cache.contains(std::to_string(i)));
    }
    for (int i = 4; i < 8; ++i) {
        EXPECT_TRUE(cache.contains(std::to_string(i)));
        EXPECT_EQ(cache.get(std::to_string(i)), i);
    }
}

TEST_F(CacheTest, CapacityOneEviction) {
    StrCache cache(1, pmr_.get());

    cache.put("a", 1);
    EXPECT_EQ(cache.get("a"), 1);

    cache.put("b", 2);  // Should evict "a"
    EXPECT_EQ(cache.get("a"), std::nullopt);
    EXPECT_EQ(cache.get("b"), 2);

    // Update "b" then insert "c"
    cache.put("b", 20);
    cache.put("c", 3);  // Should evict "b"
    EXPECT_EQ(cache.get("b"), std::nullopt);
    EXPECT_EQ(cache.get("c"), 3);
}

// ============================================================================
// Clear and Reuse Tests
// ============================================================================

TEST_F(CacheTest, ClearEmptiesCache) {
    StrCache cache(5, pmr_.get());

    for (int i = 0; i < 5; ++i) {
        cache.put(std::to_string(i), i);
    }
    EXPECT_EQ(cache.size(), 5u);

    cache.clear();

    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.size(), 0u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_FALSE(cache.contains(std::to_string(i)));
    }
}

TEST_F(CacheTest, ReuseAfterClear) {
    StrCache cache(2, pmr_.get());

    cache.put("a", 1);
    cache.clear();

    // Should work normally after clear
    cache.put("b", 2);
    cache.put("c", 3);
    cache.put("d", 4); // evicts one item

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_FALSE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_EQ(cache.get("c"), 3);
}

// ============================================================================
// Move Semantics Tests
// ============================================================================

struct MoveCounter {
    MoveCounter() = default;
    explicit MoveCounter(int v) : value(v) {}

    MoveCounter(MoveCounter&& other) noexcept : value(other.value) {
        other.moved_from = true;
        move_count++;
    }

    MoveCounter& operator=(MoveCounter&& other) noexcept {
        value = other.value;
        other.moved_from = true;
        move_count++;
        return *this;
    }

    // Delete copy to enforce move-only
    MoveCounter(const MoveCounter&) = delete;
    MoveCounter& operator=(const MoveCounter&) = delete;

    int value = 0;
    bool moved_from = false;
    static inline int move_count = 0;

    bool operator==(const MoveCounter& other) const {
        return value == other.value;
    }
};

TEST_F(CacheTest<std::string, MoveCounter>, ValuesAreMovedNotCopied) {
    using Cache = Cache<std::string, MoveCounter>;
    Cache cache(3, pmr_.get());

    MoveCounter::move_count = 0;

    cache.put("key1", MoveCounter(100));
    cache.put("key2", MoveCounter(200));

    // Each put should move the value once (into Node)
    EXPECT_EQ(MoveCounter::move_count, 2);

    auto val = cache.get("key1");
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(val->value, 100);
}

// ============================================================================
// Exception Safety Tests
// ============================================================================

struct ThrowOnConstruct {
    explicit ThrowOnConstruct(int) {
        throw std::runtime_error("construction failed");
    }
};

TEST_F(CacheTest<std::string, ThrowOnConstruct>, PutWithThrowingValueIsSafe) {
    using Cache = SieveCache<std::string, ThrowOnConstruct>;
    Cache cache(3, mock_resource_.get());

    // Insert one valid item first
    cache.put("valid", ThrowOnConstruct(0));  // Assume this doesn't throw for test

    // Reset mock counts after successful insert
    mock_resource_->reset_counts();

    // Attempt to insert throwing value
    EXPECT_THROW(
        cache.put("throwing", ThrowOnConstruct(1)),
        std::runtime_error
    );

    // Cache should still be consistent
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_TRUE(cache.contains("valid"));

    // No memory leaks: any allocated node for "throwing" should be cleaned up
    EXPECT_EQ(mock_resource_->net_allocations(), 0);
}

// ============================================================================
// PMR Integration Tests
// ============================================================================

TEST_F(CacheTest, PMRResourceIsUsedForAllocations) {
    Cache cache(3, mock_resource_.get());

    mock_resource_->reset_counts();

    cache.put("a", 1);
    cache.put("b", 2);

    // Each put allocates one Node
    EXPECT_GE(mock_resource_->allocations(), 2u);
    EXPECT_EQ(mock_resource_->net_allocations(), 2);  // 2 nodes alive
}

TEST_F(CacheTest, PMRDeallocatesOnEviction) {
    Cache cache(2, mock_resource_.get());

    cache.put("a", 1);
    cache.put("b", 2);

    mock_resource_->reset_counts();

    // Insert "c": evicts "a", deallocating its Node
    cache.put("c", 3);

    EXPECT_EQ(mock_resource_->deallocations(), 1u);  // "a" freed
    EXPECT_EQ(mock_resource_->net_allocations(), 2);  // Still 2 nodes alive
}

TEST_F(CacheTest, PMRDeallocatesOnClear) {
    Cache cache(3, mock_resource_.get());

    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);

    mock_resource_->reset_counts();

    cache.clear();

    EXPECT_EQ(mock_resource_->deallocations(), 3u);  // All 3 nodes freed
    EXPECT_EQ(mock_resource_->net_allocations(), 0);
}

TEST_F(CacheTest, DestructorFreesAllMemory) {
    std::size_t allocs_before = mock_resource_->allocations();

    {
        Cache cache(5, mock_resource_.get());
        for (int i = 0; i < 5; ++i) {
            cache.put(std::to_string(i), i);
        }
    }  // cache destroyed here

    // All nodes should be deallocated
    EXPECT_EQ(mock_resource_->net_allocations(), 0);
    EXPECT_GE(mock_resource_->deallocations(), 5u);
}

// ============================================================================
// Edge Cases and Stress Tests
// ============================================================================

TEST_F(CacheTest, RapidPutGetPattern) {
    Cache cache(10, mock_resource_.get());

    // Simulate workload: 80% reads, 20% writes
    for (int iter = 0; iter < 100; ++iter) {
        if (iter % 5 == 0) {
            // Write
            cache.put(std::to_string(iter), iter);
        } else {
            // Read (may miss)
            auto val = cache.get(std::to_string(iter % 20));
            (void)val;  // Suppress unused
        }
    }

    EXPECT_LE(cache.size(), 10u);
}

TEST_F(CacheTest, AllItemsVisitedThenEvict) {
    Cache cache(5, pmr_.get());

    // Fill and mark all visited
    for (int i = 0; i < 5; ++i) {
        cache.put(std::to_string(i), i);
        cache.get(std::to_string(i));
    }

    // Insert new item: should reset all visited flags, then evict oldest
    cache.put("new", 999);

    EXPECT_FALSE(cache.contains("0"));  // Oldest, evicted after reset
    EXPECT_TRUE(cache.contains("new"));
    EXPECT_EQ(cache.size(), 5u);
}

// ============================================================================
// Main Entry Point
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}