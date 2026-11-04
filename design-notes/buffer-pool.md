# The registered buffer pool: slab + reserved-wake wait queue

Status: implemented 2026-10-12, branch `rebuild-simple` (uncommitted at time of
writing). Code: `include/kio/buffer_pool.hpp`, `src/kio/buffer_pool.cc`,
wiring in `include/kio/context.h` (ctor `pool_cfg`, registration in
`activate()`, run() exit CHECK, `acquire_buffer()`/`try_acquire_buffer()`),
ops in `include/kio/operations.h` (`async_read_fixed`/`async_write_fixed`),
tests in `tests/kio/buffer_pool_test.cpp`.

This note records why the pool looks the way it does — including two
derivations a future reader cannot reconstruct from the code: why *someone*
must park coroutines (and why the pool is only minimally the one doing it),
and how the reserved-wake protocol buys strict FIFO with plain handles.

## 0. What this replaced

The v1 pool entangled three separate concerns in one awaiter:

```
old pool knew:
  1. parking          — intrusive doubly-linked Waiter nodes in parked frames
  2. result delivery  — Result<Lease> written INTO the parked frame (direct handoff)
  3. resume policy    — resume_waiter: inline h.resume() vs a reschedule hook
                        that was never wired (the Phase-1 invariant violation)
```

Direct handoff is an *optimization* (avoids wake-and-retry). It is also what
forces the intrusive nodes (the result slot must live in the parked frame),
the `~AcquireAwaiter` unlink, the move-constructor hazard (parked awaiters
hold `pool&` to the source), and the resume-policy hook. Dropping it removes
all four — at the price of the protocol in §3.

## 1. Why parking must exist somewhere (and where it lives)

When the pool is exhausted, the waiter must suspend *its coroutine* and let
the loop service everything else. Thread-level blocking (`std::counting_semaphore`,
condvars) is fatal in a single-threaded loop. And "no parking, caller
retries" has a livelock: a `co_await yield()` retry loop keeps the ready list
perpetually non-empty, so Phase 2 never blocks in `io_uring_enter` — and with
`DEFER_TASKRUN` some completions only materialize *at* enter. A spinner can
starve the very release it is waiting for.

So the choice is only *where* the parking lives and how thin it is:

- **Dumb slab, waiting in Context** — purest pool, but `~BufferLease` →
  `release()` must wake waiters, and if the queue lives in Context the pool
  needs a back-pointer to reach it: the reschedule hook, reborn, split across
  two objects.
- **Slab + one generic wait-queue member** (chosen) — the queue lives next to
  the free list, so release→wake is one object away. The pool's entire
  coroutine awareness is: a FIFO of `coroutine_handle<>` (a
  `detail::ReadyRing` — the same primitive as the ready list) plus a wake
  callback Context installs (`Context::post` → Phase 1).

Why plain handles cannot dangle: kio Tasks CHECK-abort when destroyed while
suspended, and detached tasks run to completion — a coroutine parked on
`acquire()` cannot legally cease to exist. This is a *documented dependency*
on the Task model, not an assumption about user code.

## 2. What stayed (the good bones, kept verbatim)

```
base_ ──► one contiguous mmap, BASE and length 2 MiB-aligned, MADV_HUGEPAGE
 ┌─────────────────────┬─────────────────────┬──────────────┐
 │     Chunk 0         │     Chunk 1         │   Chunk 2    │
 │      ≤ 1 GiB        │      ≤ 1 GiB        │    short     │
 └─────────────────────┴─────────────────────┴──────────────┘
   iovec[0]              iovec[1]              iovec[2]        ← registered table

 slot_size: pow2, ≥ 4 KiB, divides 1 GiB ⟹ every 1 GiB chunk boundary
                                       lands exactly on a slot boundary
 index(ptr) = first_index_ + ((ptr - base_) >> 30)   ← no interval trees
```

- The kernel caps each registered iovec at 1 GiB (`io_uring_register_buffers`);
  the pow2-divides-1GiB invariant guarantees no slot straddles a registration
  boundary, so any sub-slice of a lease lives in exactly one registered buffer.
- LIFO free list (cache warmth); FIFO waiter service.
- Per-Context pool: registered buffers are per-ring, matching single-issuer /
  thread-per-core.
- Registration timing: the table is built in `init()`, the
  `io_uring_register_buffers` call happens in `activate()` after
  `ENABLE_RINGS` (nothing can submit before activate returns).
- Fixed en route: the mmap *base* is now genuinely 2 MiB-aligned (map one
  guard 2 MiB, align up inside, `munmap` head/tail slivers) — previously only
  the length was rounded and the THP claim was aspirational; and the `uint16_t`
  index space is CHECKed (`table.size() + regions <= 65535`; SQE buf_index is
  u16).
- The pool's move constructor is deleted: pinned registered memory plus parked
  waiters make it address-fixed (and the old move was unsound with waiters).

## 3. The reserved-wake protocol (strict FIFO with plain handles)

Naive wake-and-retry has a hole. If `release()` pops-and-posts the oldest
waiter, that waiter *leaves the queue* before it resumes — and anything
running before its Phase-1 turn can call `try_acquire()`, see an empty
waiter queue, and **steal the slot meant for the woken waiter**. The woken
waiter then retries into an empty free list. (An awaiter cannot re-suspend
from `await_resume`, so "retry and re-park" is not an option.)

The fix is four lines of accounting — a `reserved_` counter:

```
release(ordinal):                     try_acquire():
  free_slots_.push(ordinal)             deny if closed_
  if waiters_ nonempty:                 deny if free_slots_.size() <= reserved_
    h = waiters_.pop_one()                (the SURPLUS rule: new arrivals
    ++reserved_                            may only take slots nobody was
    wake_fn_(ctx, h)   ──► Context::post   woken for)
                                        ──► Phase 1 resume

woken waiter (await_resume, suspended path):
  CHECK(reserved_ > 0); --reserved_
  lease = try_acquire(); CHECK(lease)   ← invariant, not error path
```

Invariants: `free_slots_.size() >= reserved_` always (each reservation is
backed by a pushed slot; both increment on wake, both decrement on
consumption). After `--reserved_` the woken waiter's retry is guaranteed
`size > reserved_`, regardless of who parked meanwhile — parked newcomers
have no reservation and wait for their own wake. Wakes are issued strictly
FIFO, each backed by a distinct slot, so service order is FIFO. `close()`
latches `closed_` and wakes still-parked handles; already-woken ones (popped
at wake time) resume into the same `closed_` branch on their own — no
double-post is possible because a handle is popped exactly once.

Test `NoBypassSurplusRule` pins the sharpest edge: the releaser itself calls
`try_acquire()` immediately after `reset()` and must get an empty lease —
its own just-released slot is spoken for.

## 4. Usage

```cpp
io::Context ctx(0, nullptr, opts, io::BufferPoolConfig{.slot_size = 65536, .slots = 1024});

Task<> work(io::Context& io, io::UniqueFd& fd)
{
    auto lease = co_await io.acquire_buffer();     // parks FIFO when exhausted
    // lease is span-like: bytes()/first()/subspan(), explicit span conversions
    auto n = co_await async_read_fixed(io, fd, *lease, 1000, 0, /*off=*/0);
    // ... operate on lease->first(*n) ...
    co_return {};                                   // ~Lease returns the slot
}
```

- `buf_index` is never a parameter — derived via the branchless region math.
- A foreign or empty lease in a fixed op is a CHECK (abort at the bug site),
  not an EBADF submission: the failure class is kernel-memory corruption.
- Deliberately no whole-lease overload: a 4th integer argument would be
  ambiguous between `len` and `off` (the v1 lesson — v1 callers had to write
  `std::size_t{16}`). Pass `buf.size()` for `len`.
- `run()` quiescence now includes `buffer_pool_->waiting() == 0`; the
  complementary state — waiters parked, nothing runnable, nothing in flight —
  is a leaked-lease deadlock and CHECK-fails with "leases leaked?" rather
  than spinning.
- Lease-first was chosen over fused ops (`pool.read(...)` returning a filled
  lease) for flexibility: the lease is usable as a normal buffer and can be
  held across ops (the bitcask `write_buffer_` pattern). Fused ops remain
  possible later as sugar on these primitives.

## 5. Decision record

- Lease-first API (user decision); fused ops out of scope; raw
  span+buf_index gone from the public surface.
- Wait-queue lives in the pool as a generic member (user decision);
  wake-and-retry with the reserved-wake protocol instead of direct handoff.
- Pool core coroutine-unaware: slab + LIFO free list + `closed_`.
- Misuse policy: CHECK fail-fast (matches `~Context`'s abort-at-bug-site
  philosophy); v1's submit-on-fd−1→EBADF trick retired.
- `buf_ring` (IORING_REGISTER_BUF_RING + IOSQE_BUFFER_SELECT) explicitly out
  of scope for the network recv path; nothing here precludes it.
- Out of scope: size classes / multiple pools, shared mmap across rings,
  per-ring index tables.

Verified: `make docker-dev` (full suite), `make docker-check` (asan+ubsan,
tsan), remote clang-21/libc++ on kernel 7.0 — 26/26 tests, including the
death tests for the foreign-lease CHECK and the leaked-lease deadlock CHECK.
