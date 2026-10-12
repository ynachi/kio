# The ready list: ReadyRing and the epoch-capped drain loop

Status: implemented 2026-10-12, branch `rebuild-simple`.
Code: `include/kio/detail/ready_ring.h`, wiring in `include/kio/context.h`
(`run()` ~170-223, `post()`), tests in `tests/kio/ready_ring_test.cpp` and
`tests/kio/run_loop_test.cpp`.

This note explains the data structure, its usage in the run loop, and the
prefetch in Phase 1 — from the memory-layout level up.

## 0. Why it exists (decision record)

`ready_list_` was a `std::queue<std::coroutine_handle<>>` (deque-backed).
Measured against the real hot path (reap → post → Phase-1 resume, all on the
loop thread):

- Deque's cost was misjudged as "one pointer-hop per resume." In libstdc++ a
  deque node is a 512-byte block = **64 handles**; traversal inside a block is
  contiguous, the map indirection happens once per 64 pops, and the freed
  block recycles through tcache (~1ns amortized per resume). The pointer you
  chase on *every* resume is the **coroutine frame**, not the queue slot.
- The wins that justified the change: allocation-free steady state (`post()`
  is `noexcept`; the old `bad_alloc` → `std::terminate` exposure moved from
  every 64th push to only the rare grow), bulk `pop_batch` (N pops = one
  memcpy; no bulk form exists for deque), and contiguity that makes
  software prefetch of upcoming frames possible.
- Overflow policy: **grow ×2 on full.** No fixed capacity is provably
  sufficient — `spawn()` (task.h) and the reentrant reap in `get_sqe()` on
  `-EBUSY` (context.h) both push, so per-epoch demand is unbounded; a
  "return error if full" has no sound consumer (both callers would leak or
  terminate). Grow's worst case equals deque's worst case. A fence
  `CHECK(cap <= 1<<20)` (8 MiB) turns a runaway enqueue into a diagnosable
  abort — it is a crash-vs-OOM policy, not a scheduling bound.
- Bounded-CQ reasoning ("the queue can never exceed CQ size") is false:
  pre-run spawn backlogs, mid-drain spawn fan-out, and multiple nested reaps
  per epoch all break it. The bound analysis must enumerate *all* producers.

## 1. The representation: two counters and a mask

The whole design falls out of one decision: **`head_` and `tail_` never wrap
— the slot index wraps instead.**

```
cap_ = 8        mask = cap_ - 1 = 7 (0b111)

logical queue:  oldest ──► newest          size = tail_ - head_ = 3
                H3 H4 H5
                  │  │  └─ slot 5          head_ = 3   tail_ = 6
                  │  └──── slot 4
                  └─────── slot 3

physical array:  slot:  0    1    2    3    4    5    6    7
                       ┌────┬────┬────┬────┬────┬────┬────┬────┐
                 value │    │    │    │ H3 │ H4 │ H5 │    │    │
                       └────┴────┴────┴────┴────┴────┴────┴────┘
                                        ▲              ▲
                                     head_&7=3      tail_&7=6
                                                 (next push writes here)
```

- **`slot = index & (cap_ - 1)`** — one AND gate (~1 cycle) instead of `%`
  (integer division is ~20-40 cycles and a pipeline stall). This only works
  because capacity is a power of two (`8 - 1 = 0b111` masks the high bits);
  the constructor CHECKs that property — a non-pow2 cap silently computes
  garbage slots.
- **`size = tail_ - head_`** with wrap-safe monotonic counters. At 1
  push/ns, `uint64` wraps after 584 years — but the reasoning matters more
  than the lifetime: unsigned subtraction is correct modulo 2^64 even after
  wrap, while `head_ < tail_` flips false at the wrap and wedges the queue.
  Hence the rule: derive, never compare.
- Loop-thread only (the post/run/reap contract in context.h): plain
  members, zero atomics, zero locks. If that contract ever breaks, a plain
  `std::queue` was exactly as (un)thread-safe — no regression, but also no
  protection.

## 2. The three operations

### push — the hot path, allocation-free in steady state

```
push(h):  slots_[tail_ & 7] = h;  ++tail_;

before:  head_=3 tail_=6          after:  head_=3 tail_=7
           ┌───┬───┬───┬───┬───┬───┬───┬───┐      ┌───┬───┬───┬───┬───┬───┬───┬───┐
          │   │   │   │H3 │H4 │H5 │   │   │     │   │   │   │H3 │H4 │H5 │NEW│   │
           └───┴───┴───┴───┴───┴───┴───┴───┘      └───┴───┴───┴───┴───┴───┴───┴───┘
                                         ▲ write                                ▲
```

If `size() == cap_` on entry, grow first (below). Handles are trivially
copyable 8-byte values — the store is one instruction, guarded by a
null-handle CHECK (same contract as `Context::post`).

### pop_batch — N pops collapse into one memcpy

The subtlety is the *two legs* across the wrap:

```
pop_batch(out, 7):   head_=6, tail_=13, size=7, k=min(7, size)=7
head slot = 6&7 = 6;  first = min(k, cap - 6) = min(7, 2) = 2

  slot:   0    1    2    3    4    5    6    7
        ┌────┬────┬────┬────┬────┬────┬────┬────┐
        │ D  │ E  │ F  │ G  │ H  │    │ A  │ B  │     logical order: A B | D E F G H
        └────┴────┴────┴────┴────┴────┴────┴────┘     (C was popped earlier)
          ▲    ▲              ▲    ▲
          └────┼──────────────┴────┼── head_=6
               └── leg 2 ─────────┘    └─ leg 1

  memcpy leg 1: slots 6,7   → out[0..2)      (A, B)
  memcpy leg 2: slots 0..4  → out[2..7)      (D..H)
  head_ += 7   → out holds the true FIFO order A B D E F G H
```

`first = min(k, cap_ - (head_ & mask))` is "how many elements until the
physical array ends." If the batch doesn't straddle the wrap, leg 2 is a
zero-byte memcpy — free.

### grow — double, unwrap, rebase

```
trigger: push when size == cap_ (8).   head_=6, tail_=14 → 8 elements at 6,7,0,1,2,3,4,5

old (cap=8):   [D][E][F][G][H][ ][A][B]        logical: A B D E F G H (+the new push)
                       wrap ─────┘

new (cap=16):  [A][B][D][E][F][G][H][ ][ ] ...  head_=0, tail_=7
                ▲ leg2 copies slots 0..5      leg1 copies slots 6..7
                (new[2..7])                   (new[0..2])

then the triggering push writes slot 7.  Old array freed (unique_ptr move).
```

Rebasing `head_ = 0` "unwraps" the logical order into the new array. The
queue empties at every phase boundary, so in steady state capacity converges
to the max epoch batch (≤ CQ capacity in practice) and then never moves —
grow is a warm-up artifact plus the rare-backlog path.

## 3. Usage in the run loop

```
 ┌──────────────────────────── run() ─────────────────────────────┐
 │                                                                │
 │  epoch ────────────────────────────────────────────────┐       │
 │   Phase 1: remaining = batch_max_size (128)             │       │
 │     while remaining:                                   │       │
 │       n = pop_batch(chunk[256], min(256, remaining))   │       │
 │       for i < n: prefetch(i+2); chunk[i].resume()      │       │
 │                    │                                   │       │
 │                    ├─ spawn()  ──► push  ─┐            │       │
 │                    └─ get_sqe() EBUSY ─► nested reap    │       │
 │                           ──► push ───────┴─ NEXT epoch │       │
 │   exit? ready empty && inflight==0 → submit; return     │       │
 │   Phase 2: empty ? submit_and_wait(1) : submit_and_get_events│      │
 │   Phase 3: reap walks CQ ring, post()s each CQE ─► push ┘       │
 └────────────────────────────────────────────────────────────────┘
```

Design points:

- **Copy-out is the safety contract.** `pop_batch` copies handles into the
  stack `chunk[]` *before* anything resumes. `chunk[i].resume()` runs
  arbitrary user code; that code can `push`, which can `grow`, which
  **reallocates and rebases the slot array**. Iterating ring slots in place
  would hold references across that. The stack array holds values, not
  references — same discipline as the old copy-then-pop loop, batched.
- **The epoch cap and the non-blocking Phase 2 are one decision.** At most
  `batch_max_size` resumes between kernel enters bounds fairness under
  spawn storms; and Phase 2 must then not block while coroutines are
  runnable — parking a non-empty ready list behind `submit_and_wait` would
  invert the point of the cap.
- **The busy branch must enter WITH GETEVENTS** (2026-10-12 fix, external
  review). `DEFER_TASKRUN` posts completions (deferred task work) only when
  the loop enters the kernel asking for events. Plain `io_uring_submit`
  never does — and with an empty SQ it does not enter the kernel at all —
  so a busy, self-refilling ready list would starve completions
  unboundedly. Verified on kernel 7.0.0-38 with a bare-liburing canary
  (`.zcode/taskrun_canary.c`): after a socket write completes an armed
  recv, `submit`+nop posts nothing; `io_uring_submit_and_get_events` posts
  the deferred CQE, non-blocking. The loop therefore uses
  `submit_and_get_events` on the busy path — one syscall per epoch, same as
  before, but with task work run. Regression test:
  `RunLoopTest.BusyReadyListDoesNotStarveCompletions` (busy self-respawning
  chain on a wall clock — io_uring timers starve identically — with the
  verdict taken *during* the storm; fails in 500 ms with the bug, no hang).
  Note this hazard was introduced by the epoch cap itself: the pre-cap loop
  always called `submit_and_wait(1)`, which always passes GETEVENTS.
- **Exit condition** is `ready_list_.empty() && inflight_ == 0` — both
  quiescence dimensions.
- The 256-handle chunk decouples the stack buffer (fixed 2 KiB) from the
  configurable epoch cap: a huge `batch_max_size` drains in chunks without
  a variable-size stack frame.

## 4. The prefetch line

```cpp
__builtin_prefetch(chunk[i + 2].address(), 0, 3);
```

**What the machine does without it.** `chunk[i + 2]` is a
`coroutine_handle<>` — an 8-byte wrapper around a `void*` to the
heap-allocated coroutine frame. Resuming compiles to roughly:

```asm
mov  rdi, <frame>        ; the handle's address()
mov  rax, [rdi]          ; load frame+0: the resume fn pointer   ← cache miss (~100ns)
jmp  rax                 ; indirect call into the coroutine body
```

A coroutine frame begins with its function pointers:

```
frame(i+2) on the heap:
 ┌──────────────────────────┐
 │ 0x00  resume    fn ptr   │ ◄── all in the FIRST 64B cache line
 │ 0x08  destroy   fn ptr   │      = exactly what address() names
 │ 0x10  promise            │
 │ ...   resume point, refs, locals                       │
 └──────────────────────────┘
```

Frames are scattered across the heap — each `resume()` is a pointer chase,
and **hardware prefetchers cannot chase pointers** (they detect stride
patterns; the chunk array itself they stream for free, the frames never).
Without help, every iteration stalls ~100ns on DRAM before the body starts.
That is the dominant memory cost of Phase 1: the queue slot was never the
miss; the frame always was.

**The three arguments.**

| arg | value | meaning |
|---|---|---|
| address | `chunk[i+2].address()` | effective address of that frame's **first cache line** — the one holding `resume`/`destroy`. `address()` is a constexpr accessor returning the stored pointer; the handle itself sits in the chunk array just memcpy'd, so it's L1-hot. |
| rw | `0` | anticipate a **read** (the fn-pointer load), not a store |
| locality | `3` | "keep it, I'll reuse it" (scale 0..3). On x86-64 clang emits `prefetcht0` (fill all levels incl. L1d) for 3; 0 would emit `prefetchnta` (stream, don't pollute). 3 because after the header load the *whole frame* becomes the coroutine's working set — resident, not streamed. |

**Why distance 2, and why now.** The instruction issues right before
`chunk[i].resume()`:

```
time ──►
├─ prefetch(frame i+2) ─┤
        ├── body(i) runs ──┼── body(i+1) runs ──┤ frame i+2 needed
        └───────── ~2 bodies of cover for one DRAM miss ─────────┘
```

Distance 1 gives the fetch only `body(i)` of cover; a short body (complete
synchronously, post a completion) may not cover the miss and the loop
stalls anyway. Distance 2 buys slack for the worst case and costs nothing
when frames are already cached. The guard `i + 2 < n` keeps it inside the
batch — beyond `n` the stack slots hold stale handles from previous
batches. The guard is purely an *efficiency* guard: `prefetch` is
architecturally a hint — it never faults, never commits state, and the
compiler doesn't order anything around it, so correctness can never depend
on it. A wasted prefetch is one retired µop; a missing one is ~100ns of
stall. Asymmetric bet — take it every time.

This is the standard memory-level-parallelism trick for hiding
pointer-chase latency behind batch compute (RCU/hazard-pointer reclamation
batches, epoll ready lists, tokio's local run queue all lean on the same
shape), and it is exactly what the ring's contiguity purchases: you cannot
cheaply know "`i + 2`" in a deque's segmented blocks.

## 5. Exercises

1. In `grow()`, the old array is freed while a resume from the current
   batch is live on the stack. Walk through exactly why that's safe —
   which object holds the surviving value, and when did it get there?
2. `pop_batch` returns `k = min(max, size)`; Phase 1 pops
   `min(kChunk, remaining)`. With `batch_max_size = 4096`, how many memcpys
   and how many grow-rebases can happen across the epochs that drain a
   5000-handle backlog — and why is the rebase count not one per chunk?
3. To also warm the *promise* (frame offset 0x10, second cache line) rather
   than just the fn-pointer line, what would change — and what property of
   `body(i)` decides whether it's worth it?
