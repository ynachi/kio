# Critical Fixes - Action Items

## Priority 1: Must Fix Before Production

### 1. Add noexcept to Destructors ⚠️

**Issue**: Destructors can cause program termination if they throw during stack unwinding.

**Files to fix**:
- `executor.hpp`: `~Task()`, `~SplicePipePool::Lease()`
- `runtime.hpp`: `~ThreadContext()`, `~Runtime()`  
- `net.hpp`: `~Socket()`

**Changes**:
```cpp
// executor.hpp
~Task() noexcept
{
    if (handle_)
        handle_.destroy();
}

// runtime.hpp
~ThreadContext() noexcept
{
    if (eventfd_ >= 0)
        ::close(eventfd_);
}

~Runtime() noexcept
{
    stop();
}

// net.hpp
~Socket() noexcept
{
    if (fd_ >= 0)
        ::close(fd_);
}
```

**Effort**: 5 minutes
**Risk**: None - pure safety improvement

---

### 2. Add SQE Backpressure ⚠️

**Issue**: `get_sqe()` spins indefinitely if submission queue is full.

**File**: `executor.hpp`

**Current**:
```cpp
io_uring_sqe* get_sqe()
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    while (!sqe)  // ⚠️ Unbounded spin
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (sqe) break;
        std::this_thread::yield();
    }
    ++pending_;
    return sqe;
}
```

**Fixed**:
```cpp
io_uring_sqe* get_sqe()
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    int retries = 0;
    while (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (sqe) break;
        
        // If we've spun too long, force wait for a completion
        if (++retries > 100)
        {
            io_uring_submit_and_wait(&ring_, 1);
            retries = 0;
        }
        std::this_thread::yield();
    }
    ++pending_;
    return sqe;
}
```

**Effort**: 10 minutes
**Risk**: None - only adds escape hatch

---

### 3. Make Operations Non-Movable After Construction ⚠️

**Issue**: If operation is moved after `await_suspend`, the `this` pointer in io_uring is invalid.

**File**: `executor.hpp`

**Add to all operation structs**:
```cpp
struct ReadOp : BaseOp
{
    // ... existing members ...
    
    // Prevent moves (operations are meant to be temporaries)
    ReadOp(ReadOp&&) = delete;
    ReadOp& operator=(ReadOp&&) = delete;
    
    // ... existing methods ...
};
```

**Apply to**: All operation types (ReadOp, WriteOp, AcceptOp, etc.)

**Effort**: 15 minutes (20+ operation types)
**Risk**: None - operations are already used as temporaries

---

## Priority 2: Should Fix Soon

### 4. Document Buffer Lifetime Requirements 📝

**Issue**: Users might not know buffers must outlive operations.

**File**: `io.hpp`

**Add documentation**:
```cpp
/// @brief Reads data from a file descriptor.
/// @param ctx The thread context to run on
/// @param fd File descriptor to read from
/// @param buf Buffer to read into. MUST remain valid until operation completes.
/// @param off Offset to read from (default: current position)
/// @return Awaitable that yields Result<int> with bytes read
/// 
/// @warning The buffer must remain valid until co_await returns!
/// @example
///   std::vector<char> buffer(1024);
///   auto result = co_await read(ctx, fd, buffer);  // OK
///   
///   // WRONG - buffer destroyed before read completes:
///   auto op = read(ctx, fd, temp_buffer);
///   // temp_buffer destroyed here
///   co_await op;  // ⚠️ Undefined behavior
[[nodiscard]] inline auto read(ThreadContext& ctx, int fd, std::span<char> buf, 
                                uint64_t off = static_cast<uint64_t>(-1))
{
    return detail::ReadOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}
```

**Effort**: 30 minutes (document all I/O operations)
**Risk**: None

---

### 5. Add Runtime Shutdown Timeout ⚠️

**Issue**: If blocking jobs run forever, shutdown hangs.

**File**: `runtime.hpp` and `runtime.cpp`

**Add to RuntimeConfig**:
```cpp
struct RuntimeConfig
{
    // ... existing fields ...
    
    /// @brief Maximum time to wait for graceful shutdown
    std::chrono::seconds shutdown_timeout = std::chrono::seconds(30);
};
```

**Update Runtime::stop()**:
```cpp
void Runtime::stop()
{
    if (!started_)
        return;

    for (const auto& ctx : threads_)
        ctx->request_stop();

    // Wait with timeout
    auto deadline = std::chrono::steady_clock::now() + 
                    std::chrono::seconds(30);  // TODO: use config
    
    for (const auto& ctx : threads_)
    {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            // Log warning: forced shutdown
            break;
        }
        
        // Try to join with remaining time
        // (Requires implementing join_with_timeout)
        ctx->join();
    }

    started_ = false;
}
```

**Effort**: 1 hour (need to implement timed join)
**Risk**: Low

---

### 6. Fix Inconsistent Offset Semantics 🔧

**Issue**: `read()` defaults to offset 0, `readv()` defaults to -1.

**File**: `io.hpp`

**Option 1 - Make consistent (use -1 for current position)**:
```cpp
inline auto read(ThreadContext& ctx, int fd, std::span<char> buf, 
                 uint64_t off = static_cast<uint64_t>(-1))  // Changed
{
    return detail::ReadOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}
```

**Option 2 - Provide both variants**:
```cpp
// Read at current position
inline auto read(ThreadContext& ctx, int fd, std::span<char> buf)
{
    return detail::ReadOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), -1);
}

// Read at specific offset
inline auto pread(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t off)
{
    return detail::ReadOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}
```

**Recommendation**: Option 2 (clearer intent)

**Effort**: 30 minutes
**Risk**: Breaking change for existing users

---

## Priority 3: Nice to Have

### 7. Add Class-Level Documentation 📝

Add comprehensive docs to main classes:
- `ThreadContext`
- `Runtime`
- `Task<T>`
- `Executor`

**Example**:
```cpp
/// @brief Single-threaded io_uring executor with work queue.
///
/// Each ThreadContext owns:
/// - One io_uring instance
/// - One event loop thread
/// - One work queue for cross-thread scheduling
///
/// @thread_safety All public methods are thread-safe and can be called
///                from any thread. Operations submitted to this context
///                execute on its dedicated thread.
///
/// @lifetime Must outlive all operations submitted to it.
///           Typically owned by Runtime.
///
/// @example
///   ThreadContext ctx(0, config);
///   ctx.start(false);
///   ctx.schedule(my_task());
///   ctx.request_stop();
///   ctx.join();
class ThreadContext { ... };
```

**Effort**: 2 hours
**Risk**: None

---

### 8. Add Test Suite 🧪

**Critical test cases**:

```cpp
// Test 1: SQE exhaustion
TEST(Executor, SQEExhaustion) {
    // Submit > ring size operations
    // Verify no hang
}

// Test 2: Graceful shutdown
TEST(Runtime, ShutdownUnderLoad) {
    // Submit 1000s of operations
    // Call stop()
    // Verify all complete or cancel
}

// Test 3: Cross-thread scheduling
TEST(Runtime, CrossThreadScheduling) {
    // Thread A → B → A
    // Verify no deadlock
}

// Test 4: Exception propagation
TEST(Task, ExceptionPropagation) {
    // Task throws
    // Verify exception propagates through co_await
}

// Test 5: Blocking pool saturation
TEST(Runtime, BlockingPoolSaturation) {
    // Fill blocking queue
    // Verify spawn_blocking fails gracefully
}
```

**Effort**: 8-16 hours
**Risk**: None

---

## Quick Wins (< 30 min each)

✅ Add [[nodiscard]] to I/O operations
✅ Add noexcept to move constructors
✅ Add static_assert for io_uring version requirements
✅ Add debug logging for error cases (if EFD_EAGAIN, etc.)

---

## Implementation Order

**Week 1**:
1. Add noexcept to destructors (Priority 1.1)
2. Add SQE backpressure (Priority 1.2)
3. Make operations non-movable (Priority 1.3)

**Week 2**:
4. Document buffer lifetime (Priority 2.4)
5. Add shutdown timeout (Priority 2.5)
6. Fix offset semantics (Priority 2.6)

**Week 3-4**:
7. Add comprehensive documentation (Priority 3.7)
8. Build test suite (Priority 3.8)

---

## Testing Plan

After each fix:
```bash
# Build with all warnings
cmake -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror" ..

# Build with sanitizers
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined,thread" ..

# Run existing demos
./tcp_demo
wrk -t4 -c100 -d30s http://localhost:8080/

# Verify no crashes, no leaks, no warnings
```

---

## Success Criteria

✅ All destructors are noexcept
✅ No infinite spins under any load
✅ Operations cannot be misused (move after suspend)
✅ All APIs documented with examples
✅ Shutdown completes within timeout
✅ All tests pass with ASAN/TSAN/UBSAN
✅ Zero crashes under stress testing

---

## Estimated Total Effort

- **Must fix**: 30 minutes
- **Should fix**: 2 hours
- **Nice to have**: 10 hours
- **Total**: ~12.5 hours for complete hardening

**Recommended**: Focus on Priority 1 items (30 min) before any production use.
