#include <gtest/gtest.h>
#include <coroutine>
#include "uring/operation.hpp"

using namespace URing;

// ---------------------------------------------------------
// Token Tests
// ---------------------------------------------------------
TEST(TokenTest, PackUnpack) {
    Token original{42, 1337};
    uint64_t packed = original.pack();
    
    Token unpacked = Token::unpack(packed);
    
    EXPECT_EQ(unpacked.idx, 42);
    EXPECT_EQ(unpacked.gen, 1337);
}

TEST(TokenTest, MaxValues) {
    Token original{0xFFFFFFFF, 0xFFFFFFFF};
    uint64_t packed = original.pack();
    
    EXPECT_EQ(packed, 0xFFFFFFFFFFFFFFFF);
    
    Token unpacked = Token::unpack(packed);
    EXPECT_EQ(unpacked.idx, 0xFFFFFFFF);
    EXPECT_EQ(unpacked.gen, 0xFFFFFFFF);
}

// ---------------------------------------------------------
// OpPool Tests
// ---------------------------------------------------------

TEST(OpPoolTest, InitialPreallocationWorks) {
    OpPool pool(10);
    auto h = std::noop_coroutine();
    
    // Because we pre-allocated and threaded the free list in reverse (LIFO),
    // the first allocated index should be the last one we threaded (index 9).
    Token t1 = pool.allocate(h);
    EXPECT_EQ(t1.idx, 9);
    EXPECT_EQ(t1.gen, 1);
    
    PendingOp* op = pool.try_get(t1);
    ASSERT_NE(op, nullptr);
    EXPECT_EQ(op->status, SlotStatus::active);
    EXPECT_EQ(op->handle, h);
}

TEST(OpPoolTest, DynamicExpansionWorks) {
    OpPool pool(0); // Start empty
    auto h = std::noop_coroutine();
    
    Token t1 = pool.allocate(h);
    Token t2 = pool.allocate(h);
    
    EXPECT_EQ(t1.idx, 0);
    EXPECT_EQ(t2.idx, 1);
}

TEST(OpPoolTest, IntrusiveFreeListReuse) {
    OpPool pool(0);
    auto h = std::noop_coroutine();
    
    Token t1 = pool.allocate(h); // index 0, gen 1
    pool.deallocate(t1);
    
    // Allocate again. It should reuse index 0, but generation must be 2.
    Token t2 = pool.allocate(h);
    
    EXPECT_EQ(t2.idx, 0);
    EXPECT_EQ(t2.gen, 2);
    
    // Ensure the original token is now invalid (ABA protection)
    EXPECT_EQ(pool.try_get(t1), nullptr);
    
    // Ensure the new token is valid
    EXPECT_NE(pool.try_get(t2), nullptr);
}

TEST(OpPoolTest, ABAPrevention_StaleGeneration) {
    OpPool pool(0);
    auto h = std::noop_coroutine();
    
    Token t1 = pool.allocate(h); // index 0, gen 1
    pool.deallocate(t1);
    
    Token t2 = pool.allocate(h); // index 0, gen 2
    
    // Simulate kernel returning a CQE with the old, stale user_data (t1)
    PendingOp* op = pool.try_get(t1);
    
    // Pool must reject it, even though the slot is active for a NEW operation
    EXPECT_EQ(op, nullptr); 
}

TEST(OpPoolTest, ABAPrevention_FreedSlot) {
    OpPool pool(0);
    auto h = std::noop_coroutine();
    
    Token t1 = pool.allocate(h);
    pool.deallocate(t1);
    
    // Simulate kernel returning a CQE for an operation we already cancelled/freed
    PendingOp* op = pool.try_get(t1);
    
    EXPECT_EQ(op, nullptr); 
}

TEST(OpPoolTest, OutOfBoundsAccess) {
    OpPool pool(5);
    
    // Try to access an index that doesn't exist in the deque
    Token bad_token{100, 1};
    PendingOp* op = pool.try_get(bad_token);
    
    EXPECT_EQ(op, nullptr);
}

TEST(OpPoolTest, DeallocationCleansState) {
    OpPool pool(0);
    auto h = std::noop_coroutine();
    
    Token t1 = pool.allocate(h);
    pool.get(t1.idx).status = SlotStatus::active;
    pool.get(t1.idx).original_ud = 0xDEADBEEF;
    
    pool.deallocate(t1);
    
    const PendingOp& op = pool.get(t1.idx);
    
    // Handle must be nullified
    EXPECT_EQ(op.handle, nullptr);
    // Status must be free
    EXPECT_EQ(op.status, SlotStatus::free);
    // The original_ud field is now the next_free_idx. Because this is the 
    // only thing in the free list, it should point to END_OF_LIST.
    EXPECT_EQ(op.next_free_idx, std::numeric_limits<uint32_t>::max());
}
