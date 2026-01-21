# KIO Library - Comprehensive Code Review

## Executive Summary

**Overall Assessment**: ⭐⭐⭐⭐½ (4.5/5)

The kio library is a **well-designed, high-performance io_uring wrapper** with clean abstractions. The code demonstrates strong understanding of modern C++, io_uring mechanics, and concurrent programming. The refactored structure is excellent.

**Strengths**:
- Clean, type-safe API
- Solid io_uring integration
- Good concurrency patterns
- Excellent use of C++20/23 features
- Strong RAII throughout

**Areas for Improvement**:
- Some shutdown edge cases
- Missing backpressure in certain paths
- Documentation could be more comprehensive
- A few thread-safety concerns

---

## 1. Architecture & Design

### ✅ Strengths

**Layered Design**:
```
User API (io.hpp, net.hpp)
    ↓
ThreadContext (runtime.hpp)
    ↓
Executor (executor.hpp) 
    ↓
io_uring (liburing)
```

Clean separation of concerns:
- **Executor**: Pure io_uring wrapper
- **ThreadContext**: Thread management + work scheduling
- **Runtime**: Multi-thread orchestration
- **I/O API**: User-facing operations

**Good Abstraction Choices**:
- Operations as awaitables (not callbacks)
- `Result<T>` for error handling
- `Task<T>` for composable async
- Detail namespace for internals

### ⚠️ Concerns

**1. Runtime vs ThreadContext Coupling**

`ThreadContext` has a `Runtime*` pointer but limited use:
```cpp
// runtime.hpp
Runtime* runtime_ = nullptr;

// Only used in schedule():
if (ThreadContext* src = current(); src != nullptr && src->runtime_ == runtime_)
```

**Issue**: Tight coupling for one check. Consider:
```cpp
// Alternative: Store ring_fd of siblings for msg_ring
std::vector<int> sibling_ring_fds_;

// Or: Just use eventfd always (simpler)
```

**2. Executor Exposed in Detail Namespace**

Users can technically access `detail::Executor` directly:
```cpp
auto& exec = ctx.executor();  // Should this be public?
```

**Recommendation**: Make `ThreadContext::executor()` private, friend the io operations:
```cpp
class ThreadContext {
private:
    detail::Executor& executor() { return exec_; }
    friend detail::ReadOp;  // etc.
};
```

Or keep current design but document it clearly as "advanced/internal use only".

---

## 2. io_uring Processing Loop

### ✅ Excellent Implementation

**Main Loop** (`Executor::loop_once`):
```cpp
bool loop_once(const bool wait = true)
{
    if (stopped_) return false;
    io_uring_submit(&ring_);          // Submit pending SQEs
    if (pending_ == 0) return false;
    if (wait) io_uring_submit_and_wait(&ring_, 1);  // Block for 1 CQE
    return process_completions() > 0;
}
```

**Strengths**:
- Simple and correct
- Proper batching with `io_uring_for_each_cqe`
- Non-blocking option (wait=false) for polling mode
- Clean separation from completion processing

**Completion Processing**:
```cpp
io_uring_for_each_cqe(&ring_, head, cqe)
{
    if (auto* op = static_cast<BaseOp*>(io_uring_cqe_get_data(cqe)))
    {
        op->result = cqe->res;
        op->cqe_flags = cqe->flags;
        --pending_;
        op->handle.resume();  // Resume coroutine
    }
    ++count;
}
io_uring_cq_advance(&ring_, count);
```

**Perfect**. Textbook io_uring usage.

### ⚠️ Potential Issues

**1. Unbounded Resume Depth**

```cpp
op->handle.resume();  // Can chain deeply
```

If operation A completes → coroutine resumes → submits operation B → B completes immediately → resume again...

This creates a **deep call stack**. Could cause stack overflow with long chains.

**Mitigation**: Already handled by io_uring's batching - completions come from kernel, not inline.

**Real Risk**: Low, but worth documenting.

**2. SQE Starvation**

```cpp
io_uring_sqe* get_sqe()
{
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    while (!sqe)  // Spin if no SQEs available
    {
        io_uring_submit(&ring_);
        sqe = io_uring_get_sqe(&ring_);
        if (sqe) break;
        std::this_thread::yield();  // ⚠️ Unbounded spin
    }
    ++pending_;
    return sqe;
}
```

**Issue**: If SQ is full (256 entries by default), this spins indefinitely.

**Scenarios**:
- Many concurrent operations
- Slow completions (network I/O)
- Submission queue full

**Impact**: Thread spins, burns CPU, but won't deadlock (completions still process).

**Recommendation**: Add timeout or max iterations:
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
        
        if (++retries > 100) {
            // Either throw, or force wait for completion
            io_uring_submit_and_wait(&ring_, 1);
            retries = 0;
        }
        std::this_thread::yield();
    }
    ++pending_;
    return sqe;
}
```

**3. MSG_RING Untracked**

```cpp
void msg_ring_wake(const int target_ring_fd, ...)
{
    io_uring_sqe* sqe = get_sqe_untracked();  // ⚠️ Doesn't increment pending_
    io_uring_prep_msg_ring(sqe, target_ring_fd, len, data, 0);
    io_uring_sqe_set_data(sqe, nullptr);
    sqe->flags |= IOSQE_CQE_SKIP_SUCCESS;  // Skip CQE
    (void)io_uring_submit(&ring_);
}
```

**Good**: Uses `IOSQE_CQE_SKIP_SUCCESS` to avoid CQE.

**Issue**: If `IOSQE_CQE_SKIP_SUCCESS` is not supported (older kernels), CQE arrives with `nullptr` data.

**Completion processing**:
```cpp
if (auto* op = static_cast<BaseOp*>(io_uring_cqe_get_data(cqe)))
{
    // This check saves us!
}
```

**Good**: Null check protects against this. ✅

---

## 3. Thread Safety & Concurrency

### ✅ Solid Patterns

**Work Scheduling**:
```cpp
void ThreadContext::schedule(WorkItem work)
{
    incoming_.enqueue(std::move(work));  // Lock-free queue
    
    const size_t prev = pending_work_.fetch_add(1, std::memory_order_release);
    if (prev != 0) return;  // Already signaled
    
    // Wake logic...
}
```

**Excellent**:
- Lock-free queue (moodycamel::ConcurrentQueue)
- Atomic counter with proper memory ordering
- Lazy wakeup (only on 0→1 transition)

**Wake Mechanisms**:
1. **Same runtime, different thread**: MSG_RING (zero-copy, kernel-level)
2. **Foreign thread**: eventfd (portable fallback)

**Very smart**: MSG_RING is much faster than eventfd.

### ⚠️ Issues

**1. Race in wake_msg_ring_from**

```cpp
if (ThreadContext* src = current(); src != nullptr && src->runtime_ == runtime_)
{
    if (src != this)
    {
        wake_msg_ring_from(*src);  // ⚠️ Can race with shutdown
    }
}
```

**Scenario**:
1. Thread A schedules work on Thread B
2. Thread A is shutting down
3. `src->exec_.msg_ring_wake()` called on dying executor

**Risk**: Low (shutdown waits for threads), but could be cleaner.

**Fix**: Check if source executor is stopped:
```cpp
if (src != this && !src->exec_.stopped())
{
    wake_msg_ring_from(*src);
}
```

**2. Blocking Pool Shutdown Race**

```cpp
BlockingThreadPool::~BlockingThreadPool()
{
    stop_.store(true, std::memory_order_release);
    tasks_available_.release(workers_.size());  // Wake all workers
    
    for (auto& t : workers_)
        if (t.joinable())
            t.join();
}
```

**Issue**: If workers are processing jobs when `stop_` is set, jobs might schedule new work back to ThreadContext.

**Scenario**:
1. Blocking job running: `spawn_blocking([&ctx](){ ctx.schedule(...); })`
2. BlockingThreadPool destructor called
3. Job completes, tries to schedule on potentially destroyed ThreadContext

**Current Protection**: Runtime destructor calls `stop()` first, which stops ThreadContexts.

**Risk**: If someone creates BlockingThreadPool independently, this could crash.

**Fix**: Document that BlockingThreadPool must outlive associated ThreadContexts. Or add generation counter.

**3. eventfd Saturation Handling**

```cpp
void ThreadContext::wake_eventfd() const noexcept
{
    constexpr uint64_t val = 1;
    for (;;)
    {
        const ssize_t n = ::write(eventfd_, &val, sizeof(val));
        if (std::cmp_equal(n, sizeof(val))) return;
        if (n < 0 && errno == EINTR) continue;
        
        // Saturation (EAGAIN) or other errors: best-effort wake.
        return;  // ⚠️ Silently gives up
    }
}
```

**Issue**: If eventfd is saturated (counter at UINT64_MAX-1), write fails with EAGAIN.

**Impact**: Wake is lost. Thread might not wake up.

**Reality**: Extremely rare - would need 18 quintillion pending wakes.

**Mitigation**: Already handled - work is in queue, next completion will process it.

**Verdict**: Acceptable as-is, but could log the failure in debug builds.

---

## 4. Shutdown Mechanism

### ✅ Good Design

**Graceful Shutdown Flow**:
```
1. Runtime::stop()
2.   → ThreadContext::request_stop() (all threads)
3.       → stop_requested_ = true
4.       → wake_eventfd()  (wake if blocked)
5.   → ThreadContext::join() (all threads)
```

**Thread Exit Condition**:
```cpp
while (!stop_requested_.load(std::memory_order_relaxed) ||
       exec_.pending() > 0 ||
       pending_work_.load(std::memory_order_acquire) > 0)
```

**Perfect**: Drains all pending work before exiting.

### ⚠️ Edge Cases

**1. Long-Running Blocking Jobs**

```cpp
auto result = co_await ctx.spawn_blocking([]() {
    // Runs for 10 minutes
    std::this_thread::sleep_for(10min);
});
```

**Shutdown behavior**: Runtime destructor blocks until job completes.

**Issue**: No timeout or cancellation mechanism.

**Impact**: Shutdown can hang indefinitely.

**Recommendation**: Add shutdown timeout:
```cpp
class Runtime {
    std::chrono::seconds shutdown_timeout_ = 30s;
    
    void stop() {
        // ... request stops ...
        
        auto deadline = std::chrono::steady_clock::now() + shutdown_timeout_;
        for (const auto& ctx : threads_) {
            auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= 0s || !join_with_timeout(ctx, remaining)) {
                // Log warning, force terminate?
            }
        }
    }
};
```

**2. Eventfd Read in park_task()**

```cpp
Task<> park_task()
{
    while (!stop_requested_.load(std::memory_order_relaxed))
    {
        EventfdReadOp op(*this);
        const auto r = co_await op;  // Blocks here
        (void)r;
    }
}
```

**Issue**: If `request_stop()` is called while blocked in eventfd read:
1. `stop_requested_` set to true
2. `wake_eventfd()` called → eventfd readable
3. Read completes
4. Loop checks `stop_requested_` → exits ✅

**Verdict**: Correct! The wake ensures we check the flag.

**3. In-Flight Operations During Shutdown**

```cpp
// Thread running this:
auto result = co_await read(ctx, fd, buffer);
// What if stop() called here?
```

**Current behavior**:
```cpp
while (!stop_requested_ || exec_.pending() > 0 || pending_work_ > 0)
```

**Drains `exec_.pending()`**, so in-flight I/O completes. ✅

**But**: No cancellation of new operations after stop requested.

**Recommendation**: Make operations check stopped flag:
```cpp
void await_suspend(std::coroutine_handle<> h)
{
    if (exec.stopped()) {
        // Resume immediately with ECANCELED
        result = -ECANCELED;
        h.resume();
        return;
    }
    handle = h;
    io_uring_sqe* sqe = exec.get_sqe();
    // ...
}
```

---

## 5. Error Handling

### ✅ Excellent Patterns

**Result<T> Type**:
```cpp
template <typename T = void>
using Result = std::expected<T, std::error_code>;
```

**Perfect**:
- Modern C++23 `std::expected`
- No exceptions in hot path
- Composable with `co_await`

**Consistent Error Conversion**:
```cpp
Result<int> await_resume()
{
    if (result < 0) return error_from_errno(-result);
    return result;
}
```

**Good**: io_uring returns negative errno, converted to `std::error_code`.

**Exception Handling in Tasks**:
```cpp
void unhandled_exception() { 
    exception = std::current_exception(); 
}

T await_resume()
{
    if (handle.promise().exception)
        std::rethrow_exception(handle.promise().exception);
    // ...
}
```

**Perfect**: Exceptions propagate through coroutine chains.

### ⚠️ Issues

**1. spawn_blocking Exception Handling**

```cpp
catch (...)
{
    st->value = std::unexpected(std::make_error_code(std::errc::io_error));
}
```

**Issue**: All exceptions map to generic `io_error`. Loses information.

**Better**:
```cpp
catch (const std::system_error& e)
{
    st->value = std::unexpected(e.code());
}
catch (...)
{
    st->value = std::unexpected(std::make_error_code(std::errc::io_error));
}
```

**2. No Error Context**

```cpp
auto res = co_await read(ctx, fd, buffer);
if (!res) {
    // res.error() is just an error code - no context
    // Which file? What offset? What operation?
}
```

**Impact**: Debugging is harder.

**Recommendation**: Add error context type:
```cpp
struct Error {
    std::error_code code;
    std::string context;  // "read(fd=5, offset=1024)"
    std::source_location loc;
};

template <typename T = void>
using Result = std::expected<T, Error>;
```

But this adds overhead. Current design is valid for high-performance code.

**3. Silent Failures in Helpers**

```cpp
Socket::set_nodelay(bool enable) const
{
    int opt = enable ? 1 : 0;
    if (::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0)
        return error_from_errno(errno);
    return {};
}

// Usage in demo:
(void)client_sock.set_nodelay(true);  // Ignores error!
```

**Issue**: Easy to ignore errors with `(void)` cast.

**Reality**: In practice, `setsockopt` failures are rare and often non-fatal.

**Verdict**: Acceptable for perf-critical paths. Document that errors can be ignored.

---

## 6. Memory Safety & Lifetime Management

### ✅ Strong RAII

**Task Ownership**:
```cpp
~Task()
{
    if (handle_)
        handle_.destroy();  // Always cleanup
}

Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
```

**Perfect**: Move-only, automatic cleanup.

**Socket RAII**:
```cpp
~Socket()
{
    if (fd_ >= 0)
        ::close(fd_);
}
```

**Good**: No leaks possible.

### ⚠️ Concerns

**1. Dangling References in Operations**

```cpp
inline auto read(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t off = 0)
{
    return detail::ReadOp(get_exec(ctx), fd, buf.data(), buf.size_bytes(), off);
}

// User code:
auto result = co_await read(ctx, fd, temp_buffer);  // ⚠️ temp_buffer must stay alive!
```

**Issue**: `ReadOp` stores `void* buf` - pointer to user's buffer.

**Risk**: If buffer is temporary, it's destroyed before I/O completes.

**Example**:
```cpp
Task<> bad_example(ThreadContext& ctx, int fd)
{
    {
        std::vector<char> buffer(1024);
        auto op = read(ctx, fd, buffer);
        // buffer destroyed here!
        co_await op;  // ⚠️ Use-after-free
    }
}
```

**Reality**: Users must keep buffers alive. This is standard for async I/O.

**Mitigation**: Document clearly. Consider adding `[[nodiscard]]`:
```cpp
[[nodiscard]] inline auto read(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t off = 0)
```

**2. Operation Lifetime**

Operations are move-only awaitables:
```cpp
struct ReadOp {
    // No copy constructor
    // Move is implicit
};
```

**Question**: What if moved after await_suspend?

```cpp
auto op = read(ctx, fd, buffer);
auto op2 = std::move(op);  // ⚠️ What happens?
co_await op2;
```

**Analysis**: 
- `await_suspend` stores `this` pointer in SQE
- If moved, `this` pointer is wrong
- **Crash** when completion arrives

**Protection**: Operations are temporaries, immediately co_awaited:
```cpp
co_await read(...);  // Can't move a temporary
```

**Verdict**: Safe in practice, but could add:
```cpp
struct ReadOp {
    ReadOp(ReadOp&&) = delete;  // Prevent moves
};
```

**3. ThreadContext Lifetime vs Operations**

```cpp
auto op = read(ctx, fd, buffer);
// ctx destroyed here
co_await op;  // ⚠️ Executor reference is dangling
```

**Protection**: Runtime owns ThreadContexts, keeps them alive.

**User responsibility**: Don't destroy Runtime while operations are pending.

**Recommendation**: Add usage documentation.

---

## 7. API Design

### ✅ Excellent User API

**Intuitive**:
```cpp
auto bytes = co_await read(ctx, fd, buffer);
if (!bytes) {
    // Error handling
}
```

**Type-safe**:
```cpp
std::span<char> buf;  // Can't accidentally use wrong size
auto result = co_await read(ctx, fd, buf);
```

**Modern C++**:
- Concepts (`SocketAddressable`)
- Ranges (`std::span`)
- Coroutines (natural async)

### ⚠️ API Inconsistencies

**1. Mixed Offset Semantics**

```cpp
auto read(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t off = 0);
auto readv(ThreadContext& ctx, int fd, const iovec* iov, int iovcnt, 
           uint64_t offset = static_cast<uint64_t>(-1));
```

**Issue**: `read` defaults to offset 0, `readv` defaults to -1 (current position).

**Inconsistent**: User might expect both to use current position or both to use 0.

**Fix**: Make them consistent:
```cpp
// Option 1: Both use -1 for current position
auto read(ThreadContext& ctx, int fd, std::span<char> buf, 
          uint64_t off = static_cast<uint64_t>(-1));

// Option 2: Separate functions
auto pread(ThreadContext& ctx, int fd, std::span<char> buf, uint64_t off);
auto read(ThreadContext& ctx, int fd, std::span<char> buf);  // Current position
```

**2. Raw Pointers in API**

```cpp
auto accept(ThreadContext& ctx, int fd, sockaddr* addr = nullptr, socklen_t* len = nullptr);
```

**Observation**: Uses raw pointers for optional output parameters.

**Alternative**: Could use optional references or output struct:
```cpp
struct AcceptResult {
    int fd;
    std::optional<SocketAddress> addr;
};

auto accept(ThreadContext& ctx, int fd) -> Task<Result<AcceptResult>>;
```

**Verdict**: Current design is fine - matches POSIX semantics. Changing would be over-engineering.

**3. ThreadContext Passed Everywhere**

```cpp
co_await read(ctx, fd, buf);
co_await write(ctx, fd, buf);
co_await timeout(ctx, 1s);
```

**Observation**: Verbose, ctx passed to every call.

**Alternative**: Could use TLS or implicit context:
```cpp
// Inside a coroutine running on ctx
co_await read(fd, buf);  // Implicit ctx from TLS
```

**Pros**: Less verbose
**Cons**: Hidden dependency, harder to test, less explicit

**Verdict**: Current explicit design is better for clarity and testability.

---

## 8. Performance Considerations

### ✅ Optimizations

**1. Lazy Wakeups**:
```cpp
const size_t prev = pending_work_.fetch_add(1, std::memory_order_release);
if (prev != 0) return;  // Don't wake if already signaled
```

**Excellent**: Avoids syscalls when unnecessary.

**2. Batched Completions**:
```cpp
io_uring_for_each_cqe(&ring_, head, cqe) {
    // Process all available CQEs
}
io_uring_cq_advance(&ring_, count);  // Single kernel update
```

**Perfect**: Minimal syscall overhead.

**3. MSG_RING for Cross-Thread Wake**:
```cpp
wake_msg_ring_from(*src);  // Zero-copy, kernel-level
```

**Brilliant**: Much faster than eventfd for same-runtime threads.

**4. Lock-Free Queue**:
```cpp
moodycamel::ConcurrentQueue<WorkItem> incoming_;
```

**Good**: No mutex contention on hot path.

### ⚠️ Potential Bottlenecks

**1. Single Executor Per Thread**

Each ThreadContext has one executor with one io_uring instance (256 SQEs default).

**Limit**: ~256 concurrent operations per thread.

**Impact**: For high-concurrency servers (1000s of connections), might need more threads or larger rings.

**Recommendation**: Make `entries` configurable (already is via RuntimeConfig ✅).

**2. Shared Listen Socket**

Demo code:
```cpp
int listen_fd = listener->get();
// All threads accept from same FD
```

**Issue**: Thundering herd - kernel wakes all threads on new connection.

**Modern kernels**: Have `SO_REUSEPORT` which distributes accepts.

**Recommendation**: Document usage of `SO_REUSEPORT` for multi-threaded servers.

**3. No SQPOLL Support**

```cpp
RuntimeConfig config;
config.uring_flags = 0;  // Could set IORING_SETUP_SQPOLL
```

**Observation**: SQPOLL is supported in config but not tested/documented.

**SQPOLL**: Kernel polls submission queue in separate thread - lower latency.

**Recommendation**: Add documentation and test SQPOLL mode.

**4. Work Stealing**

If one thread is overloaded:
```cpp
// Thread 1: 1000 pending tasks
// Thread 2: idle
// No work stealing!
```

**Current**: Round-robin scheduling via `Runtime::next_thread()`.

**Impact**: Can have load imbalance.

**Mitigation**: User can manually load-balance, or implement work stealing.

**Verdict**: Acceptable for most use cases. Work stealing adds complexity.

---

## 9. Documentation & Code Quality

### ✅ Strengths

**Well-Commented**:
```cpp
// Increment first so the consumer sees "work exists" before any wake decisions.
const size_t prev = pending_work_.fetch_add(1, std::memory_order_release);
```

**Clear Names**:
```cpp
spawn_blocking()  // Obvious what it does
schedule_on()     // Clear intent
```

**Consistent Style**:
- Uniform naming conventions
- RAII everywhere
- Modern C++ idioms

### ⚠️ Missing Documentation

**1. No Header Comments on Key Classes**

```cpp
class ThreadContext {
    // No doc comment explaining:
    // - Thread safety guarantees
    // - Lifetime requirements
    // - Usage examples
};
```

**Recommendation**: Add comprehensive class-level documentation.

**2. No Usage Examples in Headers**

**Add**:
```cpp
/// @example
/// Runtime rt;
/// rt.loop_forever();
/// rt.thread(0).schedule([](){ 
///     co_await read(...);
/// });
```

**3. No Performance Characteristics**

Document Big-O complexity and scalability:
```cpp
/// @performance O(1) submission, O(n) completion processing where n = ready CQEs
/// @scalability Up to RuntimeConfig::entries concurrent operations per thread
```

---

## 10. Testing Recommendations

### Critical Test Cases

**1. Shutdown Under Load**:
```cpp
// Submit 10,000 operations
// Call stop() immediately
// Verify no crashes, no leaks
```

**2. SQE Exhaustion**:
```cpp
// Submit 300 operations (> 256 ring size)
// Verify get_sqe() doesn't hang
```

**3. Blocking Pool Saturation**:
```cpp
// Fill blocking queue
// Verify spawn_blocking returns EAGAIN
```

**4. Cross-Thread Scheduling**:
```cpp
// Thread A schedules work on Thread B
// Thread B schedules back on A
// Verify no deadlocks
```

**5. Exception Propagation**:
```cpp
Task<> thrower() {
    throw std::runtime_error("test");
}

Task<> catcher() {
    try {
        co_await thrower();
    } catch (const std::runtime_error& e) {
        // Verify exception caught
    }
}
```

---

## 11. Critical Bugs Found

### 🔴 HIGH: Potential UAF in spawn_blocking

```cpp
auto job = [st = s]() mutable  // Captures shared_ptr
{
    // ... execute job ...
    st->ctx->schedule([st]() {  // ⚠️ Schedules back
        st->h.resume();
    });
};
```

**Scenario**:
1. Blocking job completes
2. Schedules resume on ThreadContext
3. ThreadContext is shutting down
4. `schedule()` enqueues work
5. Work never executes (thread exited)
6. Coroutine never resumes
7. **Leak**

**Fix**: Check if thread is stopping:
```cpp
if (st->ctx->is_stopping()) {
    // Don't schedule, complete with error
    return;
}
st->ctx->schedule(...);
```

Or accept the leak as edge case in shutdown (documented).

### 🟡 MEDIUM: Missing Noexcept on Destructors

```cpp
~Task()  // Should be noexcept
{
    if (handle_)
        handle_.destroy();
}
```

**Issue**: If destructor throws during stack unwinding, program terminates.

**Fix**: Mark noexcept:
```cpp
~Task() noexcept
```

Same for `~Socket`, `~Runtime`, etc.

### 🟡 MEDIUM: Unchecked eventfd() Failure

```cpp
eventfd_(eventfd(0, EFD_CLOEXEC))  // What if this returns -1?
// ...
if (eventfd_ < 0)  // Checked later ✅
    throw std::system_error(errno, std::system_category(), "eventfd");
```

**Actually OK**: Error is checked. False alarm. ✅

---

## 12. Recommendations Summary

### High Priority

1. **Add noexcept to destructors**
2. **Document buffer lifetime requirements**
3. **Add shutdown timeout to Runtime**
4. **Fix/document spawn_blocking shutdown behavior**
5. **Make operations non-movable after await_suspend**

### Medium Priority

6. **Add backpressure in get_sqe() spin**
7. **Consistent offset semantics in read/readv**
8. **Add class-level documentation**
9. **Add usage examples**
10. **Test suite for edge cases**

### Low Priority (Nice to Have)

11. **Error context in Result<T>**
12. **Work stealing between threads**
13. **SQPOLL documentation and testing**
14. **Debug logging for edge cases**

---

## 13. Overall Verdict

### Production Readiness: ✅ Ready with Minor Fixes

**This is high-quality code**. With the recommended fixes, it's production-ready for:
- High-performance servers
- Storage engines
- Network proxies
- Real-time systems

**Strengths**:
- Solid architecture
- Correct io_uring usage
- Good concurrency design
- Clean modern C++

**Before Production**:
- Add comprehensive tests
- Add noexcept to destructors
- Document lifetime requirements
- Handle shutdown edge cases

**Rating**: ⭐⭐⭐⭐½ (4.5/5)

---

## Appendix: Benchmarking Recommendations

```cpp
// Benchmark these scenarios:
1. Single-threaded throughput (ops/sec)
2. Multi-threaded scaling (1, 2, 4, 8 threads)
3. Latency (p50, p99, p99.9)
4. Memory usage under load
5. CPU usage (should be near 100% under load)
6. Context switch rate (should be low)

// Compare against:
- Raw io_uring
- libuv
- Boost.Asio
- Tokio (Rust)
```

Expect performance within 5-10% of raw io_uring.
