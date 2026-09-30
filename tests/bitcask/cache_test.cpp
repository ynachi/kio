#include "bitcask/cache.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

using namespace bitcask;

namespace
{
class CacheTest : public ::testing::Test
{
protected:
    using StrCache = Cache<std::string, int>;

    void SetUp() override { pmr_ = std::make_unique<TrackingResource>(); }

    void TearDown() override
    {
        EXPECT_EQ(pmr_->net_allocations(), 0)
            << "Memory leak detected: " << pmr_->net_allocations() << " allocations not freed";
    }

    std::unique_ptr<TrackingResource> pmr_;
};

struct MoveCounter
{
    MoveCounter() = default;
    explicit MoveCounter(int v) : value(v) {}

    MoveCounter(MoveCounter&& other) noexcept : value(other.value)
    {
        other.moved_from = true;
        ++move_count;
    }

    MoveCounter& operator=(MoveCounter&& other) noexcept
    {
        value = other.value;
        other.moved_from = true;
        ++move_count;
        return *this;
    }

    MoveCounter(const MoveCounter&) = delete;
    MoveCounter& operator=(const MoveCounter&) = delete;

    int value = 0;
    bool moved_from = false;
    static inline int move_count = 0;
};

struct ThrowOnSecondMove
{
    explicit ThrowOnSecondMove(int v) : value(v) {}

    ThrowOnSecondMove(ThrowOnSecondMove&& other)
    {
        if (++move_count == throw_on_move)
        {
            throw std::runtime_error("move failed");
        }
        value = other.value;
    }

    ThrowOnSecondMove& operator=(ThrowOnSecondMove&& other)
    {
        if (++move_count == throw_on_move)
        {
            throw std::runtime_error("move failed");
        }
        value = other.value;
        return *this;
    }

    ThrowOnSecondMove(const ThrowOnSecondMove&) = delete;
    ThrowOnSecondMove& operator=(const ThrowOnSecondMove&) = delete;

    int value = 0;
    static inline int move_count = 0;
    static inline int throw_on_move = 0;
};
}  // namespace

TEST_F(CacheTest, ConstructWithZeroCapacityThrows)
{
    EXPECT_THROW(StrCache(0, pmr_.get()), std::invalid_argument);
}

TEST_F(CacheTest, EmptyCacheInitialState)
{
    StrCache cache(10, pmr_.get());

    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_FALSE(cache.contains("key"));
    EXPECT_EQ(cache.get("key"), std::nullopt);
}

TEST_F(CacheTest, PutAndGetSingleItem)
{
    StrCache cache(1, pmr_.get());

    cache.put("key1", 100);

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_FALSE(cache.empty());
    EXPECT_TRUE(cache.contains("key1"));
    EXPECT_EQ(cache.get("key1"), 100);
    EXPECT_EQ(cache.get("missing"), std::nullopt);
}

TEST_F(CacheTest, UpdateExistingKey)
{
    StrCache cache(3, pmr_.get());

    cache.put("key", 1);
    EXPECT_EQ(cache.get("key"), 1);

    cache.put("key", 2);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.get("key"), 2);
}

TEST_F(CacheTest, ContainsDoesNotMarkVisited)
{
    StrCache cache(2, pmr_.get());

    cache.put("a", 1);
    cache.put("b", 2);

    EXPECT_TRUE(cache.contains("a"));

    cache.put("c", 3);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
}

TEST_F(CacheTest, EvictUnvisitedOldest)
{
    StrCache cache(3, pmr_.get());

    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);

    (void)cache.get("b");

    cache.put("d", 4);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_TRUE(cache.contains("d"));
    EXPECT_EQ(cache.size(), 3u);
}

TEST_F(CacheTest, SecondChanceThenEvict)
{
    StrCache cache(3, pmr_.get());

    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);

    (void)cache.get("a");
    (void)cache.get("b");
    (void)cache.get("c");

    cache.put("d", 4);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_TRUE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_TRUE(cache.contains("d"));

    cache.put("e", 5);
    EXPECT_FALSE(cache.contains("b"));
}

TEST_F(CacheTest, HandPointerWrapAround)
{
    StrCache cache(4, pmr_.get());

    for (int i = 0; i < 4; ++i)
    {
        cache.put(std::to_string(i), i);
        (void)cache.get(std::to_string(i));
    }

    for (int i = 4; i < 8; ++i)
    {
        cache.put(std::to_string(i), i);
        EXPECT_EQ(cache.size(), 4u);
    }

    for (int i = 0; i < 4; ++i)
    {
        EXPECT_FALSE(cache.contains(std::to_string(i)));
    }
    for (int i = 4; i < 8; ++i)
    {
        EXPECT_TRUE(cache.contains(std::to_string(i)));
        EXPECT_EQ(cache.get(std::to_string(i)), i);
    }
}

TEST_F(CacheTest, CapacityOneEviction)
{
    StrCache cache(1, pmr_.get());

    cache.put("a", 1);
    EXPECT_EQ(cache.get("a"), 1);

    cache.put("b", 2);
    EXPECT_EQ(cache.get("a"), std::nullopt);
    EXPECT_EQ(cache.get("b"), 2);

    cache.put("b", 20);
    cache.put("c", 3);
    EXPECT_EQ(cache.get("b"), std::nullopt);
    EXPECT_EQ(cache.get("c"), 3);
}

TEST_F(CacheTest, ClearEmptiesCache)
{
    StrCache cache(5, pmr_.get());

    for (int i = 0; i < 5; ++i)
    {
        cache.put(std::to_string(i), i);
    }
    EXPECT_EQ(cache.size(), 5u);

    cache.clear();

    EXPECT_TRUE(cache.empty());
    EXPECT_EQ(cache.size(), 0u);
    for (int i = 0; i < 5; ++i)
    {
        EXPECT_FALSE(cache.contains(std::to_string(i)));
    }
}

TEST_F(CacheTest, ReuseAfterClear)
{
    StrCache cache(2, pmr_.get());

    cache.put("a", 1);
    cache.clear();

    cache.put("b", 2);
    cache.put("c", 3);
    cache.put("d", 4);

    EXPECT_FALSE(cache.contains("a"));
    EXPECT_FALSE(cache.contains("b"));
    EXPECT_TRUE(cache.contains("c"));
    EXPECT_EQ(cache.get("c"), 3);
}

TEST_F(CacheTest, ValuesAreMovedNotCopied)
{
    Cache<std::string, MoveCounter> cache(3, pmr_.get());

    MoveCounter::move_count = 0;

    cache.put("key1", MoveCounter(100));
    cache.put("key2", MoveCounter(200));

    EXPECT_EQ(MoveCounter::move_count, 2);
    EXPECT_TRUE(cache.contains("key1"));
    EXPECT_TRUE(cache.contains("key2"));
}

TEST_F(CacheTest, PutWithThrowingValueIsSafe)
{
    Cache<std::string, ThrowOnSecondMove> cache(3, pmr_.get());

    ThrowOnSecondMove::move_count = 0;
    ThrowOnSecondMove::throw_on_move = 0;
    cache.put("valid", ThrowOnSecondMove(0));

    const auto live_allocations = pmr_->net_allocations();
    ThrowOnSecondMove::move_count = 0;
    ThrowOnSecondMove::throw_on_move = 1;

    EXPECT_THROW(cache.put("throwing", ThrowOnSecondMove(1)), std::runtime_error);

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_TRUE(cache.contains("valid"));
    EXPECT_FALSE(cache.contains("throwing"));
    EXPECT_EQ(pmr_->net_allocations(), live_allocations);
}

TEST_F(CacheTest, PMRResourceIsUsedForAllocations)
{
    StrCache cache(3, pmr_.get());

    pmr_->reset_counts();

    cache.put("a", 1);
    cache.put("b", 2);

    EXPECT_GE(pmr_->allocations(), 2u);
    EXPECT_EQ(pmr_->net_allocations(), 2u);
}

TEST_F(CacheTest, PMRDeallocatesOnEviction)
{
    StrCache cache(2, pmr_.get());

    cache.put("a", 1);
    cache.put("b", 2);

    pmr_->reset_counts();

    cache.put("c", 3);

    EXPECT_EQ(pmr_->deallocations(), 1u);
    EXPECT_EQ(pmr_->net_allocations(), 2u);
}

TEST_F(CacheTest, PMRDeallocatesOnClear)
{
    StrCache cache(3, pmr_.get());

    cache.put("a", 1);
    cache.put("b", 2);
    cache.put("c", 3);

    pmr_->reset_counts();

    cache.clear();

    EXPECT_EQ(pmr_->deallocations(), 3u);
    EXPECT_EQ(pmr_->net_allocations(), 0u);
}

TEST_F(CacheTest, DestructorFreesAllMemory)
{
    {
        StrCache cache(5, pmr_.get());
        for (int i = 0; i < 5; ++i)
        {
            cache.put(std::to_string(i), i);
        }
    }

    EXPECT_EQ(pmr_->net_allocations(), 0u);
    EXPECT_GE(pmr_->deallocations(), 5u);
}

TEST_F(CacheTest, RapidPutGetPattern)
{
    StrCache cache(10, pmr_.get());

    for (int iter = 0; iter < 100; ++iter)
    {
        if (iter % 5 == 0)
        {
            cache.put(std::to_string(iter), iter);
        }
        else
        {
            (void)cache.get(std::to_string(iter % 20));
        }
    }

    EXPECT_LE(cache.size(), 10u);
}

TEST_F(CacheTest, AllItemsVisitedThenEvict)
{
    StrCache cache(5, pmr_.get());

    for (int i = 0; i < 5; ++i)
    {
        cache.put(std::to_string(i), i);
        (void)cache.get(std::to_string(i));
    }

    cache.put("new", 999);

    EXPECT_FALSE(cache.contains("0"));
    EXPECT_TRUE(cache.contains("new"));
    EXPECT_EQ(cache.size(), 5u);
}
