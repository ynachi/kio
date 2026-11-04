# Baton: the coroutine event primitive (kio's folly::coro::Baton)

Status: **IMPLEMENTED & verified 2026-10-12.** Code: `include/kio/baton.h`,
`include/kio/semaphore.h`, Context park counter in `include/kio/context.h`
(external_parks_ + the composed exit CHECK). Tests:
`tests/kio/baton_semaphore_test.cpp` (10 cases incl. two death tests);
37/37 on docker-dev, docker-check (asan+ubsan, tsan), and the remote
second toolchain (clang-21/libc++, kernel 7.0). The buffer pool is
untouched by this change (its 8 tests green unchanged). Related:
`ready-ring.md` (the queue it reuses), `buffer-pool.md` (the specialized
sibling), `sharding.md` (the thread contract).

## 0. What folly's Baton is, and what of it kio refuses to copy

`folly::coro::Baton` is a one-shot coroutine event: `co_await baton` parks
until `post()`; `post()` is sticky (once posted, always posted) and
broadcast (all current waiters wake); late waiters complete inline; no
payload. It is the building block for folly's async `Mutex`, `Semaphore`,
and one-shot Promise bridges.

Its internals pay a tax kio does not owe: folly coroutines live in a
multi-threaded world, so the state is an atomic state machine
(EMPTY → WAITING → NOTIFIED) with CAS transitions, waiter nodes
intrusive-linked inside parked frames, and — the big semantic choice —
`post()` resumes waiters INLINE on the posting thread (callers re-hop with
`viaIfAsync`). kio's loop-thread contract deletes all of it: a plain
`bool` instead of the state machine, and wakes route through
`Context::post` → Phase 1, never on the poster's stack.

## 1. Semantics (decided)

- **Sticky**: once posted, stays posted; waiters arriving later complete
  via `await_ready` without suspending.
- **Broadcast**: `post()` wakes ALL parked waiters (an event, not a
  resource). Wake-one is a different primitive — a Semaphore — and needs
  the reserved-wake accounting the pool has (a released slot can be
  stolen; a posted flag cannot).
- **One-shot, no reset**: a resettable flag is a different primitive
  (EventCount); reset+wait races are where these designs rot.
- **No cancellation**: a parked waiter cannot be destroyed (kio's
  Task-lifetime invariant — same documented dependency as the pool). When
  `when_any` arrives, IT owns the double-resume guard, not the Baton.
- **Loop-thread only**; cross-thread `post()` is the socketpair control
  plane (`sharding.md` §4).

## 2. The design

```cpp
class Baton   // namespace kio; holds io::Context&
{
public:
    explicit Baton(io::Context& io) noexcept;

    /// Latch and wake every parked waiter (via Context::post -> Phase 1).
    /// Idempotent: a second post() when already ready is a no-op.
    void post() noexcept;

    [[nodiscard]] bool try_wait() const noexcept;   // ready_

    struct WaitAwaiter;                              // returned by wait()
    [[nodiscard]] WaitAwaiter wait() noexcept;

private:
    io::Context& io_;
    bool ready_ = false;
    detail::ReadyRing waiters_{};   // parked handles, FIFO — the pool's shape
};

struct Baton::WaitAwaiter
{
    Baton& baton;
    bool parked_ = false;           // guards note_unpark on the fast path

    bool await_ready() noexcept;                         // baton.ready_
    void await_suspend(std::coroutine_handle<> h) noexcept;
    void await_resume() noexcept;                        // consume park count
};
```

Flows:

```
fast path  waiter after post:  ready_ == true -> never suspends (0 allocs, 0 syscalls)
park       await_suspend:      ctx.note_park(); waiters_.push(h)
post       ready_ = true;      while (pop h) ctx.post(h);   // broadcast, never inline
resume     await_resume:       if (parked_) ctx.note_unpark();
```

The awaiter is a prvalue slotted into the awaiting frame (guaranteed copy
elision — same pattern as the pool's `AcquireAwaiter`), so a wait costs
zero allocations. The fast path costs one load.

**The waiter ring is lazy** (2026-10-12, review finding): both Baton and
Semaphore allocate their `ReadyRing` only on the first park, at initial
capacity 16. An eager ring cost 8 KiB + one malloc per instance — fatal
for the per-locked-resource / per-bridge use cases this primitive exists
for (folly's Baton is 8 bytes). The fast path stays allocation-free by
construction; a genuine broadcast-many still grows. `waiting()` reads 0
through the null pointer, so quiescence accounting is unchanged. The pool
keeps its eager ring: one instance, waiters genuinely reach thousands.

No reserved-wake accounting is needed (contrast `buffer-pool.md` §3):
accounting exists because a released *slot* can be stolen by a new
arrival; a posted Baton's `ready_` is sticky, so an arrival during the
wake drain sees `await_ready() == true` and never parks.

## 3. The park counter — finding 1.4's general rule, paid once

A Baton is a user-owned object, so the pool's exit-time poll
(`buffer_pool_->waiting()`) does not scale to N primitives. The design adds
ONE mechanism to Context:

- `Context::external_parks_` (plain `size_t`, hot section next to
  `inflight_`), with private `note_park()` / `note_unpark()` (friended to
  Baton and Semaphore).
- `run()`'s exit CHECK composes both inputs:
  `CHECK((!buffer_pool_ || buffer_pool_->waiting() == 0) && external_parks_ == 0)`
  — the pool keeps its own `waiting()` poll; the counter covers everything
  else. Full unification (pool counting through `note_park` too) is
  deliberately deferred: it forces out-of-line pool methods to break the
  buffer_pool.hpp ↔ context.h include cycle, for zero behavioral gain.

Soundness of the exit CHECK (same argument as the pool's, verbatim): at
`ready_list_.empty() && inflight_ == 0`, a still-parked waiter can never be
posted — its poster must be runnable (contradiction), in flight
(contradiction), or itself parked on another primitive (a cycle = genuine
deadlock). There is no gap between the counter and the ready list: a
woken handle is popped at wake time but sits in `ready_list_` until it
resumes, so `ready` stays non-empty across the window, and the counter is
consumed exactly at that resume (`await_resume`, guarded by the awaiter's
`parked_`/`fast_` flag so the fast path never uncounts what it never
counted).

## 3b. Semaphore — the resource counterpart (added to scope 2026-10-12)

Counting async semaphore, wake-one FIFO, built on the same
ReadyRing + `Context::post` shape. Where Baton needs **no** accounting
(a sticky flag cannot be stolen), a released **permit** can be — so the
Semaphore carries the reserved-wake protocol verbatim from the pool:

```
state: count_ (permits), reserved_, waiters_
try_acquire: ok iff count_ > reserved_          (surplus rule)
release:     ++count_; if waiters: ++reserved_; post(oldest)
woken:       --reserved_; --count_              (take the backed permit)
invariants:  count_ >= reserved_; waiters nonempty ⟹ count_ == reserved_
```

v1 has no `close()`/cancellation on either primitive: with no cancellation
in kio, `wait()`/`acquire()` return plain `void` — no Result machinery, no
error channel — and the exit CHECK is the backstop for never-posted/
never-released cases. A RAII acquire-guard and close-time drain are
follow-ups if teardown patterns want them.

## 4. What it unlocks

- `async Mutex` (`locked_` + Baton), a general `Semaphore` (the pool minus
  the slab; the pool re-expressible as slab + semaphore).
- A one-shot channel / Promise bridge: `Baton` + a `std::optional<T>` slot
  — hand a result from a completion callback into a `co_await`.
- Condition-style loops: `while (!pred()) co_await baton;`
- The fused pool ops deferred in the pool redesign are compositions of
  these primitives, should they ever be wanted.

## 5. Test plan — all green as implemented

1. ✓ Fast path: post-then-wait never parks (`PostThenWaitIsFast`).
2. ✓ Broadcast FIFO: 3 parked waiters wake on one post, in park order
   (`BroadcastWakesAllInParkOrder`).
3. ✓ Sticky: late waiter after post completes without suspension
   (`StickyLateWaiterCompletes`, including a second `run()` session).
4. ✓ Quiescence: a parked waiter with a sleeping releaser keeps `run()`
   alive (`QuiescenceWaitsForParkedWaiter`,
   `QuiescenceWaitsForParkedAcquirer`).
5. ✓ Never-posted baton at quiescence: death tests on the composed exit
   CHECK (`NeverPostedAborts`, `NeverReleasedAborts`).
6. ✓ Semaphore surplus rule: `TryAcquireSurplusOnly` (two of two, third
   denied, release-without-waiters re-enables),
   `ReleaserCannotStealReservedPermit` (the releaser cannot re-take the
   permit reserved for the woken waiter), `WakeOneFifoChained` (chained
   handoff in spawn order, capacity 1).

## Decision record (2026-10-12)

- Semantics: broadcast + sticky, no reset, no cancellation — folly-
  compatible, with kio's wake-through-Phase-1 replacing inline resume.
- Scope: Baton + Context park counter + pool migration in one change (the
  recommendation, picked); one quiescence mechanism, 1.4's rule enforced
  generically.
- Wake-one/resource semantics are explicitly NOT this primitive.
- Deferred: timed wait (needs `when_any`, which owns its double-resume
  guard), EventCount-style reset.
