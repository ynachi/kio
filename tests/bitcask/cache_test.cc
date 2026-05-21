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
    (void)cache.put("a", 1);
    (void)cache.put("b", 2);
    (void)cache.put("c", 3);

    // Access "b" to mark visited
    (void)cache.get("b");

    // Insert "d": should evict "a" (oldest, unvisited)
    auto evicted = cache.put("d", 4);
    EXPECT_TRUE(evicted.has_value());
    EXPECT_EQ(*evicted, 1);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_TRUE(cache.contains("d"));
    EXPECT_EQ(cache.size(), 3u);
}

TEST_F(CacheTest, SecondChanceThenEvict) {
    StrCache cache(3, pmr_.get());

    (void)cache.put("a", 1);
    (void)cache.put("b", 2);
    (void)cache.put("c", 3);

    // Mark all as visited
    (void)cache.get("a");
    (void)cache.get("b");
    (void)cache.get("c");

    // Insert "d": Sieve scan resets visited flags, then evicts oldest
    auto evicted = cache.put("d", 4);
    EXPECT_EQ(evicted, 1);

    // "a" had visited=true, so it got reset to false, then evicted
    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));  // visited reset but survived this round
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_TRUE(cache.contains("d"));

    // Insert "e": now "b" is oldest unvisited
    evicted = cache.put("e", 5);
    EXPECT_EQ(evicted, 2);
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

    MoveCounter(const MoveCounter& other) : value(other.value) {
        copy_count++;
    }
    MoveCounter& operator=(const MoveCounter& other) {
        value = other.value;
        copy_count++;
        return *this;
    }

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

    int value = 0;
    bool moved_from = false;
    static inline int move_count = 0;
    static inline int copy_count = 0;

    bool operator==(const MoveCounter& other) const {
        return value == other.value;
    }
};

class CacheMoveTest : public ::testing::Test {
protected:
    void SetUp() override { pmr_ = std::make_unique<TrackingResource>(); }
    void TearDown() override {
        EXPECT_EQ(pmr_->net_allocations(), 0);
    }
    std::unique_ptr<TrackingResource> pmr_;
};

TEST_F(CacheMoveTest, ValuesAreMovedNotCopied) {
    using MoveCache = Cache<std::string, MoveCounter>;
    MoveCache cache(3, pmr_.get());

    MoveCounter::move_count = 0;
    MoveCounter::copy_count = 0;

    cache.put("key1", MoveCounter(100));
    cache.put("key2", MoveCounter(200));

    // Each put should move the value into the Node. 
    // Depending on compiler optimizations, it might be more than 1 move per put,
    // but should be 0 copies.
    EXPECT_EQ(MoveCounter::copy_count, 0);
    EXPECT_GE(MoveCounter::move_count, 2);

    auto val = cache.get("key1");
    EXPECT_TRUE(val.has_value());
    EXPECT_EQ(val->value, 100);
    // get() returns by value, so it WILL copy (or move if compiler optimizes)
}

// ============================================================================
// Exception Safety Tests
// ============================================================================

struct ThrowOnConstruct {
    bool should_throw = false;
    ThrowOnConstruct() = default;
    explicit ThrowOnConstruct(bool t) : should_throw(t) {
        if (should_throw) throw std::runtime_error("construction failed");
    }
    ThrowOnConstruct(ThrowOnConstruct&& other) noexcept : should_throw(other.should_throw) {}
    ThrowOnConstruct& operator=(ThrowOnConstruct&& other) noexcept {
        should_throw = other.should_throw;
        return *this;
    }
};

class CacheExceptionTest : public ::testing::Test {
protected:
    void SetUp() override { pmr_ = std::make_unique<TrackingResource>(); }
    void TearDown() override {
        EXPECT_EQ(pmr_->net_allocations(), 0);
    }
    std::unique_ptr<TrackingResource> pmr_;
};

TEST_F(CacheExceptionTest, PutWithThrowingValueIsSafe) {
    using ExCache = Cache<std::string, ThrowOnConstruct>;
    ExCache cache(3, pmr_.get());

    // Insert one valid item first
    cache.put("valid", ThrowOnConstruct(false));

    struct ThrowOnMove {
        bool should_throw = false;
        ThrowOnMove() = default;
        ThrowOnMove(bool t) : should_throw(t) {}
        ThrowOnMove(const ThrowOnMove&) = default;
        ThrowOnMove(ThrowOnMove&& other) {
            if (other.should_throw) throw std::runtime_error("move failed");
        }
        ThrowOnMove& operator=(ThrowOnMove&&) = default;
    };
    
    using ThrowCache = Cache<std::string, ThrowOnMove>;
    ThrowCache tcache(3, pmr_.get());
    tcache.put("valid", ThrowOnMove(false));

    std::size_t allocs_before = pmr_->allocations();
    std::size_t deallocs_before = pmr_->deallocations();

    EXPECT_THROW(tcache.put("throwing", ThrowOnMove(true)), std::runtime_error);

    EXPECT_EQ(tcache.size(), 1u);
    EXPECT_TRUE(tcache.contains("valid"));
    
    // Any partial allocation for "throwing" should have been rolled back
    EXPECT_EQ(pmr_->allocations() - allocs_before, pmr_->deallocations() - deallocs_before);
}

// ============================================================================
// PMR Integration Tests
// ============================================================================

TEST_F(CacheTest, PMRResourceIsUsedForAllocations) {
    StrCache cache(3, pmr_.get());
    
    // Each put allocates one Node. Total: 2 (sentinels) + 2 (data) = 4
    cache.put("a", 1);
    cache.put("b", 2);

    EXPECT_EQ(pmr_->net_allocations(), 4);
}

TEST_F(CacheTest, PMRDeallocatesOnEviction) {
    StrCache cache(2, pmr_.get());
    cache.put("a", 1);
    cache.put("b", 2);

    std::size_t allocs_before = pmr_->allocations();
    std::size_t deallocs_before = pmr_->deallocations();

    // Insert "c": evicts "a", deallocating its Node
    cache.put("c", 3);

    EXPECT_EQ(pmr_->allocations() - allocs_before, 1u);    // "c" allocated
    EXPECT_EQ(pmr_->deallocations() - deallocs_before, 1u); // "a" freed
}

TEST_F(CacheTest, PMRDeallocatesOnClear) {
    StrCache cache(3, pmr_.get());
    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);

    std::size_t deallocs_before = pmr_->deallocations();

    cache.clear();

    EXPECT_EQ(pmr_->deallocations() - deallocs_before, 3u);  // All 3 nodes freed
}

TEST_F(CacheTest, DestructorFreesAllMemory) {
    {
        StrCache cache(5, pmr_.get());
        for (int i = 0; i < 5; ++i) {
            cache.put(std::to_string(i), i);
        }
        EXPECT_EQ(pmr_->net_allocations(), 7); // 5 nodes + 2 sentinels
    }  // cache destroyed here

    // All nodes should be deallocated
    EXPECT_EQ(pmr_->net_allocations(), 0);
}

// ============================================================================
// Edge Cases and Stress Tests
// ============================================================================

TEST_F(CacheTest, RapidPutGetPattern) {
    StrCache cache(10, pmr_.get());

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
    StrCache cache(5, pmr_.get());

    // Fill and mark all visited
    for (int i = 0; i < 5; ++i) {
        cache.put(std::to_string(i), i);
        (void)cache.get(std::to_string(i));
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