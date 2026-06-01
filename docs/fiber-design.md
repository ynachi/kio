# Fiber Design

kio supports two concurrent execution models on the same `IO` event loop: **stackless coroutines** (`Task<T>`, `co_await`) and **stackful fibers** (`FiberIO`). Both share one io_uring ring and one `tick()` loop with zero cross-model overhead.

---

## Why fibers alongside coroutines?

Stackless coroutines are optimal for flat async pipelines where every suspension point is explicit. But some code is easier to write in a straight-line style — deep call chains, third-party libraries, recursive algorithms — where `co_await` at every level is invasive. Fibers let that code run synchronously from its own perspective while the OS thread never blocks.

The original ucontext-based implementation incurred ~100 ns per switch because `swapcontext` calls `sigprocmask` unconditionally. The current Boost.Context implementation (`jump_fcontext`) is pure assembly with no signal mask: ~5–15 ns per switch.

---

## Key files

| File | Role |
|---|---|
| `include/uring/core/fiber.hpp` | `FiberContext` struct — execution state, stack, intrusive links |
| `include/uring/core/fiber_io.hpp` | `FiberIO` — synchronous-looking I/O API for use inside fibers |
| `include/uring/core/fiber_sync.hpp` | `FiberMutex`, `FiberSemaphore`, `FiberChannel` — user-space sync |
| `include/uring/core/detail/fiber_queue.hpp` | `FiberQueue` — Vyukov MPSC queue for cross-thread fiber spawn |
| `src/uring/io.cc` | `fiber_entry`, `tick()` CQE dispatch, `ready_fibers_` resume batch |
| `include/uring/core/io.h` | `spawn_fiber()`, `schedule_fiber()` |

---

## Stack layout

Every fiber gets one `mmap` allocation:

```
Low address
┌─────────────────────────────────┐  ← stack_mem  (mmap base)
│  guard page  (PROT_NONE, 4 KiB) │  → SIGSEGV on overflow instead of silent heap corruption
├─────────────────────────────────┤
│                                 │
│  usable stack  (default 64 KiB) │  stack grows downward
│                                 │
└─────────────────────────────────┘  ← stack_top  (make_fcontext entry point)
High address
```

`FiberContext(size_t sz)` calls `mmap(MAP_PRIVATE|MAP_ANONYMOUS|MAP_STACK)` for the full region, then `mprotect(PROT_NONE)` on the bottom page. `~FiberContext` calls `munmap`. Non-movable, non-copyable — the raw address is stored in SQE `user_data`.

Default sizes (`fiber.hpp`):

```cpp
inline constexpr size_t kDefaultFiberStack   = 64 * 1024;  // sufficient for typical I/O handlers
inline constexpr size_t kFiberGuardPageSize  = 4096;
```

Increase `stack_size` when the handler uses large local buffers or calls deeply into unknown code.

---

## Execution model: Boost.Context

Boost.Context `jump_fcontext` is a pure-assembly cooperative switch:

```
jump_fcontext(to_ctx, data)  →  transfer_t { fctx_of_caller, data }
```

It saves the current register file onto the current stack, restores the target stack and registers, and returns in the target. No kernel involvement, no signal mask.

Two context handles are in play per fiber:

- `FiberContext::ctx` — the fiber's saved state; updated by `tick()` after each resume
- `FiberContext::scheduler_ctx` — `tick()`'s saved state; updated by the fiber after each resume

They leapfrog: `tick` jumps to fiber, fiber jumps back to `tick`, and so on.

---

## Fiber entry point

```cpp
// src/uring/io.cc
void URing::detail::fiber_entry(boost::context::detail::transfer_t t) noexcept
{
    auto* ctx = static_cast<FiberContext*>(t.data);
    ctx->scheduler_ctx = t.fctx;          // save tick's context for the first suspend

    FiberIO fio{*ctx->io, *ctx};
    ctx->result = ctx->fn(fio);            // run user code synchronously
    ctx->done   = true;

    jump_fcontext(ctx->scheduler_ctx, nullptr);  // return to tick()
}
```

`t.data` carries the `FiberContext*` passed to the first `jump_fcontext` from `tick()`. `t.fctx` is `tick()`'s context handle; storing it in `scheduler_ctx` is what allows the fiber to jump back on every suspension.

---

## CQE dispatch: tagged pointer

SQE `user_data` uses the low bit to distinguish the two models:

| Bit 0 | Meaning | Pointer type |
|---|---|---|
| `0` | stackless coroutine | `IoOps*` (cast directly) |
| `1` | stackful fiber | `FiberContext*` (XOR away the tag before casting) |

Set in `FiberIO::submit_and_wait`:

```cpp
io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&ctx_) | 1u);
```

Dispatched in `tick()`:

```cpp
if (user_data & 1u)                                    // fiber CQE
{
    auto* fiber     = reinterpret_cast<FiberContext*>(user_data ^ 1u);
    fiber->last_res = cqe->res;                        // make result available before resume
    auto t = jump_fcontext(fiber->ctx, fiber);
    if (fiber->done)
        unlink_fiber(fiber);                           // O(1) via intrusive links
    else
        fiber->ctx = t.fctx;                           // save fiber's new suspended state
}
```

Fiber CQEs are resumed **inline** inside the CQE loop — the fiber acts on its result immediately without waiting until the end of `tick()`.

---

## Suspension: FiberIO::submit_and_wait

Every `FiberIO` method (read, write, open, …) calls this template:

```cpp
template <typename Setup>
int32_t FiberIO::submit_and_wait(Setup&& setup) noexcept
{
    io_uring_sqe* sqe = io_.get_sqe();
    std::forward<Setup>(setup)(sqe);                           // populate the SQE
    io_uring_sqe_set_data64(sqe, reinterpret_cast<uint64_t>(&ctx_) | 1u);

    // Suspend: jump back to tick(). t.fctx is tick's saved state.
    auto t = jump_fcontext(ctx_.scheduler_ctx, nullptr);
    ctx_.scheduler_ctx = t.fctx;                               // tick may have moved call sites

    return ctx_.last_res;                                      // CQE result written by tick() before jump
}
```

From the fiber's point of view this is a blocking call. From `tick()`'s point of view the fiber disappears and other work runs while the kernel processes the I/O.

---

## Fiber lifecycle in tick()

```
spawn_fiber()
  │  allocates FiberContext (mmap + make_fcontext)
  │  links into IO's intrusive owned-fiber list
  └► pushes raw ptr to ready_fibers_

tick() — end of loop, ready_fibers_ batch (new spawns + sync wakeups):
  for fiber in ready_fibers_:
    jump_fcontext(fiber->ctx, fiber)   ← first-time: calls fiber_entry; re-resume: returns from suspend()
    if done: unlink_fiber(fiber)
    else:    fiber->ctx = t.fctx

tick() — inside CQE loop, resuming suspended fibers:
  fiber->last_res = cqe->res
  jump_fcontext(fiber->ctx, fiber)
  if done: unlink_fiber(fiber)
  else:    fiber->ctx = t.fctx
```

The owned-fiber list is intrusive: `FiberContext::prev_owned` and `next_owned`
link each live fiber into `IO`. This keeps the fiber address stable and allows
O(1) removal without storing a nullable standard-library iterator in the fiber.

---

## Ownership and lifetime

- `IO`'s intrusive owned-fiber list — sole owner of all fiber stacks
- `IO::ready_fibers_` (`std::vector<FiberContext*>`) — raw pointers to fibers pending their next resume: newly spawned fibers waiting for their first run, and fibers re-enqueued by sync primitives via `FiberIO::wakeup()`
- SQE `user_data` — raw pointer tag for fibers suspended on an I/O op; valid because the fiber is alive in the owned-fiber list until it completes

`FiberContext` is **non-movable** and **non-copyable** — its address is stored in multiple places and must be stable.

---

## Spawning fibers

### Same-thread (local): `spawn_fiber`

```cpp
io.spawn_fiber([](FiberIO& fio) -> Result<void> {
    FIBER_TRY(auto buf, fio.take_fixed_buffer(4096));
    FIBER_TRY(auto fd,  fio.open("/tmp/data", O_RDONLY));
    FIBER_TRY(auto n,   fio.read_fixed(fd, buf));
    // ...
    return {};
}, kDefaultFiberStack);
```

Must be called from the thread that owns the `IO`. The fiber starts at the end of the current `tick()` call.

### Cross-thread: `schedule_fiber`

```cpp
other_io.schedule_fiber([](FiberIO& fio) -> Result<void> {
    // runs on other_io's thread
    return {};
});
```

Constructs the `FiberContext` fully on the caller (allocates the stack, calls `make_fcontext`), then enqueues the raw pointer into `IO::fiber_queue_` (a `FiberQueue` Vyukov MPSC queue) and calls `wake()`. The target thread's `tick()` drains `fiber_queue_` at the top of every call, links each pointer into its owned-fiber list, and queues it for its first resume. The fiber is born on the target thread and never migrates.

```cpp
// tick() — top of every call
fiber_queue_.drain([this](FiberContext* fiber) {
    link_fiber(fiber);
    ready_fibers_.push_back(fiber);
});
```

`fn` is moved once — into `FiberContext::fn` on the caller. No coroutine frame, no extra allocation.

---

## Cross-thread fiber queue: FiberQueue

`FiberQueue` (`include/uring/core/detail/fiber_queue.hpp`) is a Vyukov MPSC queue for `FiberContext*`, mirroring `CoroQueue` for `TaskPromiseBase*`. It is the transport used by `schedule_fiber`.

Key details:
- Intrusive link: `std::atomic<FiberContext*> next_queued` stored directly in `FiberContext`
- Sentinel node: `FiberContext stub_{FiberContext::StubTag{}}` — a no-mmap constructor; the destructor's `if (stack_mem != nullptr)` guard handles cleanup safely
- `enqueue()` is safe from any thread; `drain()` is single-consumer (called only from `tick()`)
- `IO::fiber_queue_` is a private member; `IO::submit_or_wait_for()` checks `fiber_queue_.empty()` in both the fast-path and the sleep double-check, ensuring a cross-thread enqueue + `wake()` never leaves the target thread sleeping

---

## Error handling

All `FiberIO` methods return `Result<T>` (`std::expected<T, std::error_code>`). The recommended pattern:

```cpp
FIBER_TRY(auto n, fio.read_fixed(fd, buf));   // declares 'n', propagates error
FIBER_TRY_VOID(fio.fsync(fd));                // propagates error, no value
```

`FIBER_TRY(decl, expr)` takes two arguments: a variable declaration and the expression. On error it expands to an early `return std::unexpected(err)` — structurally identical to `co_return std::unexpected(err)` in coroutines but without the coroutine machinery.

---

## Fiber synchronization primitives

Defined in `include/uring/core/fiber_sync.hpp`. All primitives require all participating fibers to run on the **same `IO`** instance — there are no atomics internally, the single-thread cooperative invariant is the guarantee.

Suspended fibers are re-enqueued via `FiberIO::wakeup()`, which pushes to `IO::ready_fibers_`. They run in the `ready_fibers_` batch at the end of `tick()`, either in the same tick (if woken during CQE processing) or the next tick (if woken during the batch itself).

### FiberMutex

Non-reentrant mutual exclusion. Waiters are queued in FIFO order. `unlock()` transfers ownership directly to the next waiter without clearing `locked_`, so the woken fiber holds the lock on return from `lock()` with no extra round-trip.

```cpp
FiberMutex mu;

io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
    FiberLockGuard g{mu, fio};        // acquires (suspends if held), releases on scope exit
    FIBER_TRY(auto n, fio.read_fixed(fd, buf));
    process(buf, n);
    return {};
});
```

`FiberLockGuard` is the RAII counterpart — it releases the mutex on any exit path, including early returns via `FIBER_TRY`.

### FiberSemaphore

Counting semaphore. Useful for signalling: one fiber waits until another posts.

```cpp
FiberSemaphore ready{0};

io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
    FIBER_TRY_VOID(fio.fsync(fd));
    ready.post(fio);           // unblocks the waiter
    return {};
});

io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
    ready.wait(fio);           // suspends until post()
    return {};
});
```

### FiberChannel\<T, N\>

Bounded FIFO channel. `send()` suspends when full; `recv()` suspends when empty. Blocked senders and receivers are woken in FIFO order.

```cpp
FiberChannel<int, 8> ch;

io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
    for (int i = 0; i < 16; ++i)
        ch.send(fio, i);       // suspends when buffer full
    return {};
});

io.spawn_fiber([&](FiberIO& fio) -> Result<void> {
    for (int i = 0; i < 16; ++i)
        process(ch.recv(fio)); // suspends when buffer empty
    return {};
});
```

Note: `channel.empty()` and `channel.full()` reflect only the internal buffer, not values held by suspended senders. Use a known item count or a sentinel value to determine when to stop receiving.

---

## Migration from coroutines

| Coroutine | Fiber equivalent |
|---|---|
| `Task<void> handler(IO& io)` | `Result<void> handler(FiberIO& fio)` |
| `co_await io.read(fd, buf)` | `fio.read(fd, buf)` |
| `URING_TRY(co_await io.X())` | `FIBER_TRY(fio.X())` |
| `co_return {};` | `return {};` |
| `io.schedule(task)` | `io.schedule_fiber(fn)` |

No coroutine keywords, no `co_await` noise in call chains. The function is a plain synchronous function that happens to run on a fiber stack.
