# Critical Fixes Patch

This patch addresses the highest-priority issues found in code review.

## Fix 1: Add noexcept to All Destructors

### executor.hpp

```cpp
// Line ~190
~Task() noexcept
{
    if (handle_)
    {
        handle_.destroy();
    }
}

// Line ~62
~Lease() noexcept { recycle(); }
```

### runtime.hpp

```cpp
// Line ~151
~ThreadContext() noexcept;

// Line ~328
~Runtime() noexcept;
```

### runtime.cpp

```cpp
// Line ~145
ThreadContext::~ThreadContext() noexcept
{
    if (eventfd_ >= 0)
    {
        ::close(eventfd_);
    }
}

// Line ~348
Runtime::~Runtime() noexcept
{
    stop();
}
```

### net.hpp

```cpp
// Line ~27
~Socket() noexcept
{
    if (fd_ >= 0)
        ::close(fd_);
}
```

---

## Fix 2: Add SQE Backpressure Protection

### executor.hpp

```cpp
// Replace get_sqe() around line 287
io_uring_sqe* get_sqe()
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    int retries = 0;
    constexpr int MAX_SPIN_RETRIES = 100;
    
    while (!sqe)
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (sqe) break;
        
        // If we've spun too many times, force wait for a completion
        // This prevents infinite spin if SQ is saturated
        if (++retries > MAX_SPIN_RETRIES)
        {
            // Wait for at least one completion to free up SQE slots
            io_uring_submit_and_wait(&ring_, 1);
            retries = 0;
        }
        
        std::this_thread::yield();
    }
    ++pending_;
    return sqe;
}
```

---

## Fix 3: Make Operations Non-Movable

Add to each operation struct in executor.hpp:

```cpp
struct ReadOp : BaseOp
{
    // ... existing members ...
    
    ReadOp(ReadOp&&) = delete;
    ReadOp& operator=(ReadOp&&) = delete;
    
    // ... existing methods ...
};

// Apply same pattern to all operations:
// WriteOp, FsyncOp, AcceptOp, ConnectOp, RecvOp, SendOp, CloseOp,
// TimeoutOp, CancelOp, CancelFdOp, ReadvOp, WritevOp, PollOp,
// SendmsgOp, RecvmsgOp, OpenAtOp, UnlinkAtOp, FallocateOp,
// FtruncateOp, SpliceOp
```

---

## Fix 4: Add [[nodiscard]] to I/O Operations

### io.hpp

Add to all operation-returning functions:

```cpp
[[nodiscard]] inline auto read(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t off = 0)
{
    return detail::ReadOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}

[[nodiscard]] inline auto write(ThreadContext& ctx, int fd, std::span<const char> buf, uint64_t off = 0)
{
    return detail::WriteOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}

// ... etc for all operations
```

---

## Verification

After applying patches:

```bash
# Verify compilation
cmake -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror -Wnoexcept" ..
cmake --build .

# Run with sanitizers
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined" ..
./tcp_demo

# Test under load
wrk -t4 -c1000 -d60s http://localhost:8080/
```

All three fixes should have:
- ✅ Zero compilation warnings
- ✅ Zero runtime errors
- ✅ Same performance
- ✅ Improved safety

---

## Estimated Time to Apply

- Fix 1 (noexcept): 5 minutes
- Fix 2 (SQE backpressure): 5 minutes
- Fix 3 (non-movable ops): 15 minutes (copy-paste to ~20 structs)
- Fix 4 (nodiscard): 10 minutes

**Total**: ~35 minutes

---

## Breaking Changes

**None**. All fixes are backwards compatible.

---

## Next Steps

After these critical fixes:
1. Add comprehensive documentation (see CRITICAL_FIXES.md)
2. Build test suite
3. Add shutdown timeout
4. Fix offset semantics inconsistency
