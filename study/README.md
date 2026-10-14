# Reading kio — a C++ & systems-programming curriculum

This is a study guide, not a walkthrough. The contract (tutoring mode):

1. **You write the code.** Labs below give you a goal, constraints, and what to
   observe — never a finished program. Fragments in this doc are orientation,
   not solutions.
2. **Predict before you read.** Each stage opens with questions you should try
   to answer *before* looking at the implementation. Then read the code and
   check yourself. Being wrong here is the point — that is where the learning
   is.
3. **Every claim is observable.** Whenever the code (or a comment) asserts
   "this is faster" / "this is UB" / "the kernel does X", there is a lab or a
   command that makes it visible. Don't believe it until you've seen it.
4. Bring your lab code back for review. Reviews cover: memory layout, UB,
   cache behavior, object lifetimes, and C++20/23 idioms — expect questions,
   not fixes.

The goal is **not** kio mastery. kio is the specimen; C++ and the OS are the
subject. Each stage lists the transferable concepts you should be able to
explain to someone else afterwards.

---

## Build & lab environment

kio needs Linux (`io_uring`); you are on macOS. Everything below runs through
the pinned toolchain container:

```bash
make docker-shell          # shell inside kio-dev (clang-20)
make docker-t T=baton      # run tests matching a pattern, dev build
make docker-check          # asan + tsan, what CI runs
```

Labs live in `study/labs/` and are built through the repo's own presets —
same warnings, sanitizers, ccache and container workflow as the library
itself. One registration line per lab in `study/labs/CMakeLists.txt`:

```cmake
kio_add_lab(lab01_ring lab01_ring.cpp)              # plain C++
kio_add_lab(lab04_uring lab04_uring.cpp LINK uring) # needs io_uring / kio
```

```bash
make docker-shell                                   # shell inside kio-dev (clang-20)
cmake --build --preset dev --target lab01_ring      # build one lab
./build/dev/study/labs/lab01_ring                   # run it
cmake --build --preset asan --target lab01_ring     # same lab under sanitizers
./build/asan/study/labs/lab01_ring
```

Each preset builds into `build/<preset>/study/labs/` — `build-docker/` on
the host when built in the container; run the binary where it was built.
Labs tagged **[LINUX]** (io_uring, mmap semantics) run inside the container
only; pure-language labs register the same way — one workflow, no side
doors. For cache counters on Linux:
`perf stat -e cache-misses,L1-dcache-load-misses ./lab`.

Forensic *inspection* of a snippet — dumping assembly or LLVM IR with
`clang++ -S` / `-S -emit-llvm` — stays ad hoc; that is reading code, not
building it.

---

## The map — dependency-ordered reading

```
Stage 1  detail/ready_ring.h        pure data structure, no coroutines
Stage 2  task.h                     coroutine frames, handles, lifetimes
Stage 3  baton.h, semaphore.h       the awaiter pattern, parking protocols
Stage 4  context.h                  the run loop: post / run / reap
Stage 5  detail/awaitable.h +       bridging coroutines to the kernel
         operations.h
Stage 6  buffer_pool.hpp (+.cc)     mmap, registered buffers, RAII leases
Stage 7  design-notes/sharding.md   architecture: share-nothing, benchmarks
```

Each stage below has: **Read** (files + the lines that matter),
**Concepts** (the C++/systems curriculum embedded there), **Labs**,
and **Questions** — the oral-exam list. If you can't answer a question, that
is the next thing to chase, in code or in a lab.

Companion decision records exist for most stages (`design-notes/ready-ring.md`,
`baton.md`, `buffer-pool.md`, `sharding.md`). Read them **after** the code,
as confirmation — reading them first turns prediction into recognition, which
feels like learning but isn't.

---

## Stage 1 — ReadyRing: a ring buffer is two counters and a mask

**Read:** `include/kio/detail/ready_ring.h` (all 103 lines — it is the densest
file in the repo per line). Then the Phase-1 usage in `context.h:250-269`.

### Predict first

1. A FIFO ring of capacity N: what state must it hold, minimum? Why is
   "one index + one bool for empty/full" ambiguous, and how do `head_`/`tail_`
   as **monotonic u64 counters** (`ready_ring.h:98-99`) kill the ambiguity?
2. `size()` is `tail_ - head_` on unsigned 64-bit. What happens after 2^64
   pushes? What would happen to `head_ < tail_` instead? (The comment at
   `ready_ring.h:23-25` claims one survives and the other doesn't — verify the
   arithmetic yourself on paper with 8-bit counters.)
3. Why must capacity be a power of two? What does `index & (cap_-1)` compile
   to, vs `index % cap_`?

### Concepts on display

- **Trivially copyable types & memcpy legality**: the `static_assert` at
  `ready_ring.h:14` is a lifetime-safety claim. What exactly makes
  `std::coroutine_handle<>` trivially copyable, and why does that license the
  raw `memcpy` in `pop_batch` (`ready_ring.h:65-66`)? Contrast: could you
  memcpy a `std::unique_ptr`? A `std::string`? Same bytes — different law.
- **`std::make_unique_for_overwrite` vs `make_unique`** (`ready_ring.h:39` and
  the comment in `grow()` at line 80-81): what does value-initialization of an
  array of handles cost, and when is overwriting-without-initializing both
  legal and profitable? What does the standard *guarantee* about the array's
  contents (spoiler: nothing — indeterminate values; why is that OK here)?
- **The copy-out contract**: `pop_batch` copies handles into the *caller's*
  buffer before anyone resumes them (`ready_ring.h:57-60`). The stated reason:
  resuming runs user code that can `push` → `grow` → **reallocate** the very
  ring you'd be iterating. This is a reentrancy rule, enforced by convention,
  not by the type system. Find every caller in `context.h` and check each one
  owns its pop buffer.
- **Growth policy as decision-making**: read `design-notes/ready-ring.md` §0
  *after* your prediction: the ring replaced a `std::deque`. The note is a
  masterclass in *measuring before replacing* — the deque was mispriced.
  What's the actual amortized cost model of deque? Where did the real wins
  come from (allocation-free steady state, bulk pop, prefetch)?

### Labs

**Lab 1.1 [ring]** — Write your own `ReadyRing<T>` from scratch (test-first,
no peeking): pow2-checked constructor, push/pop_batch with the two-leg memcpy
across the wrap, grow-by-doubling with head rebased to 0. Constraints: `pop_batch`
must be `noexcept`; only `push` may allocate. Then:
- Instrument `head_`/`tail_` to print `(head, tail, cap)` after each op. Push
  5, pop 3, push 5 more into cap=8 — draw the physical array and both legs of
  the memcpy. Do this on paper *before* running; the program confirms.
- Break it deliberately: use `% cap` instead of `& (cap_-1)` in a copy of your
  code, and use `clang++ -O2` — look at the difference in the generated
  assembly (`-S`). Count instructions. Is the modulo a `div` instruction or
  did the compiler save you? When *does* the compiler optimize `%` to `&`,
  and why doesn't that apply here?

**Lab 1.2 [false sharing, the concept kio *doesn't* need here]** — kio's ring
is single-threaded by contract, so `head_`/`tail_` sit adjacent on one cache
line (`ready_ring.h:98-101`) — that's *good* for the one thread that touches
both. To understand what the contract buys you, write the counterfactual:
two threads each incrementing their own counter 10^8 times, in a struct with
and without `alignas(64)` separation. Time both. Then explain:
- What is a cache line, what does coherence traffic (MESI) do when two cores
  write the same line, and why is the penalty called *false* sharing when the
  program has no logical sharing at all?
- `std::hardware_destructive_interference_size` — what does it promise, why
  64, and why do some standard libraries refuse to define it?

**Lab 1.3 [prefetch]** — `context.h:261-266` prefetches the next coroutine
frame's header line with `__builtin_prefetch(..., 0, 3)`. What do the two
trailing arguments mean? Lab: array of 64B structs, random vs sequential
walk, with/without a manual prefetch a few iterations ahead — measure.
What is "two bodies of cover for one DRAM miss" (see `ready-ring.md` §4)?
This lab builds the mental model for why `pop_batch` copying N handles
contiguously matters at all.

### Questions

1. Why is `push` *not* `noexcept` while `pop_batch` is? Trace the failure
   mode if push's `grow()` throws inside `Context::post` (read
   `context.h:435-446` — what does the comment say happens, and why is that
   deemed correct)?
2. `kMaxCapacity = 1<<20` is called "a crash-vs-OOM fence, not a scheduling
   bound". Explain the difference. What would a "reject when full" policy
   require from *every producer* of the ring? Enumerate the producers
   (hint: `spawn`, `reap`, `post` — where does each run from?).
3. In `grow()`, `head_` is rebased to 0 but `tail_` is set to `n`. Why must
   the *logical* contents be "unwrapped" — what would be wrong with memcpy'ing
   slot-by-slot preserving physical positions?

---

## Stage 2 — Task: coroutine frames, handles, and lifetime law

**Read:** `include/kio/task.h` in full. It is ~245 lines containing an entire
coroutines course.

### Predict first

1. When you `co_await someTask`, who resumes whom, and *where does the stack
   go*? What does `Task::await_suspend` returning a handle (vs `void` vs
   `bool`, `task.h:139-147`) mean for control flow?
2. A coroutine frame — where does it live (stack? heap? neither?), what's in
   it, and who calls `destroy()` on it? What is a *dangling* coroutine?
3. Why does every kio promise use `initial_suspend = suspend_always`
   (`task.h:35`)? What breaks with `suspend_never` — think about the
   `Task` handle returned from `get_return_object`.

### Concepts on display

- **The promise ↔ frame ↔ handle triangle**: `get_return_object` /
   `from_promise` (`task.h:98-101`), `handle.address()`, `handle.destroy()`,
   `handle.done()`, `handle.resume()`. A `std::coroutine_handle<P>` is a
   typed, trivially-copyable pointer into the frame that knows how to find
   its promise. This is *the* representation Stage 1's ring stores.
- **Symmetric transfer** (`task.h:37-55`): `Final::await_suspend` returns the
   continuation handle instead of resuming it. What does it prevent?
   (Answer to *verify*, not read: stack growth across a long chain of
   completing coroutines — each resume is a nested C++ call otherwise.)
   And why does it return `std::noop_coroutine()` when `cont` is null
   instead of `nullptr` — what is UB about returning null?
- **The sentinel-comment at `task.h:24-26`**: kio compares `cont` against a
   default-constructed handle, *not* against `noop_coroutine()`, because
   "the standard promises nothing about handle equality with noop". Lesson:
   reading the standard's exact wording matters; comment says v1 "learned
   this the hard way".
- **`std::expected` as error channel** (`ResultMixin`, `task.h:61-93`): why
   is the promise's `result` seeded with `std::unexpect` + an error rather
   than default-constructed? What constraint would default-construction
   silently impose on every `T`? (This is `std::expected` API fluency:
   `std::unexpect_t`, converting constructors, `has_value`/`error()`.)
- **Deliberate omission of `return_void`** (`task.h:86-88`): a promise with
   both `return_value` and `return_void` is ill-formed; kio makes bare
   `co_return;` a *compile error* so every exit names its outcome. Design
   taste: use the type system to delete the sloppy spelling.
- **Move-only RAII wrapper around a raw frame** (`Task` itself): the big four
   deleted/moved, `std::exchange` idiom in the move ops, `reset()` pattern.
   Also `[[nodiscard]]` on the class — why is discarding a `Task` (a handle
   to a frame you own!) worth a warning?
- **Detached coroutines** (`task.h:197-243`): `suspend_never` final suspend
   means the frame destroys itself at completion — the coroutine commits
   suicide and the wrapper holds a handle it must never touch again. Who
   guarantees the detached task runs to completion in kio? (Trace
   `Context::spawn` → `post` → Phase 1.)

### Labs

**Lab 2.1 [minimal task]** — Before trusting kio's, write the smallest
correct `Task<T>` you can: lazy start, one-shot co_await, symmetric transfer,
no error channel. Use it in a 3-deep `co_await` chain and print
`__PRETTY_FUNCTION__`/thread id at each resume. Then:
- Make `Final::await_suspend` `void` and `h.resume()` inline instead — now
   chain 1'000'000 awaits (a loop calling a leaf task). Does it crash? Why
   exactly — what does each nested resume push onto the machine stack?
   This *is* the reason symmetric transfer exists; watch it fail.
- Destroy a coroutine while it is suspended at an internal `co_await`. Under
   ASAN: what dies? Is it UB, and does the sanitizer even catch it? (This
   motivates kio's CHECK-abort in `~Task`, `task.h:176`.)

**Lab 2.2 [frame forensics]** — Compile a 20-line coroutine with
`clang++ -std=c++23 -S -emit-llvm` and find `llvm.coro.begin`,
`llvm.coro.suspend`, `llvm.coro.destroy` in the output. Then add one local
array of 1000 bytes and watch the frame size change. Questions to answer from
the IR: where does the frame allocation call come from (`operator new`?
global `new`?), and what does `coro.free` do? This lab permanently changes
how you see `co_await`.

**Lab 2.3 [kill kio's Task on purpose]** — Write death tests against the
real `kio::Task` (copy the style from `tests/kio/baton_semaphore_test.cpp`):
(a) destroy a started-but-suspended Task, (b) co_await a Task twice,
(c) co_await a moved-from Task. `EXPECT_DEATH` each. For each abort, point
at the exact CHECK that fired and explain what *silent corruption* would
have happened in a library without these checks.

### Questions

1. `Task::await_ready` (`task.h:137`) returns true for a *completed* task.
   Trace what happens mechanically when you await a completed task: which
   functions run, in what order, and why is no suspension involved?
2. Why is `PromiseBase::cont` written by the *awaiter's* `await_suspend`
   rather than somewhere in the promise? What race does
   "initial_suspend is suspend_always + written exactly once before the body
   runs" prevent? (The comment at `task.h:20-23` asserts no race — for a
   single-threaded loop. What thread would you need for a race?)
3. `run_detached` (`task.h:220-232`) logs errors instead of propagating.
   Why can't it propagate — who would receive it?
4. `unhandled_exception` calls `std::terminate()` (`task.h:58`). What are
   the alternatives (`std::exception_ptr` capture) and what do they cost in
   the frame? What is kio's stated philosophy about exceptions?

---

## Stage 3 — Baton & Semaphore: the awaiter pattern and parking protocols

**Read:** `include/kio/baton.h`, then `include/kio/semaphore.h`, then the
park counter wiring in `context.h:448-462` and the exit CHECK at
`context.h:276-290`. Companion: `design-notes/baton.md` (after your own
predictions).

### Predict first

1. An awaitable is any type with `await_ready` / `await_suspend` /
   `await_resume`. For `Baton::WaitAwaiter` (`baton.h:66-92`): who calls
   `await_ready`, and if it returns false, exactly what happens between
   suspension and a later `await_resume`? Draw the timeline of a
   post-then-wait and a wait-then-post.
2. `Baton::post()` (`baton.h:48-59`) never resumes a handle — it only pushes
   them into the Context's ready list. Why not `h.resume()` right there?
   Think reentrancy: whose stack would the waiter's body run on?
3. Semaphore adds `reserved_` on top of Baton's shape. Before reading: with
   a counting semaphore that parks waiters FIFO and wakes one per
   `release()`, construct (as a sequence of interleaved steps, on paper) a
   scenario where a *newly arriving* coroutine steals the permit that was
   meant for the *woken waiter*. If you can build that interleaving, you've
   derived `reserved_` yourself.

### Concepts on display

- **The awaiter protocol as an interface that doesn't exist**: no base class,
   no concept required — the compiler *finds* `await_ready`/`await_suspend`/
   `await_resume` via overload resolution (`co_await expr` →
   `operator co_await` or the member forms). kio's awaiters are plain nested
   structs: `WaitAwaiter`, `AcquireAwaiter` in both semaphore.h and
   buffer_pool.hpp. Note the pattern: the awaiter is a *temporary state
   machine* living in the awaiting coroutine's frame — that's why
   `parked_`/`fast_` flags (`baton.h:69`, `semaphore.h:69`) work at all.
   What is the lifetime of an awaiter object? Where does it live?
- **Sticky/broadcast vs resource semantics** (`baton.h:12-26`): a posted
   Baton can't be stolen — `ready_` is monotone — so no accounting is
   needed. A released semaphore permit *can* be stolen — hence
   `reserved_` (`semaphore.h:22-27`). This distinction (event vs resource)
   is the whole stage. The comment at `baton.h:23-26` states it as a
   design rule: accounting exists only when a released resource can be
   stolen.
- **External parking & the quiescence CHECK**: parked coroutines are
   invisible to `ready_list_` and `inflight_`, so `note_park`/`note_unpark`
   (`context.h:454-462`) teach `run()` they exist. The exit CHECK
   (`context.h:285-287`) then turns "parked with no one left to wake you"
   into an abort. Why is a parked-but-unwakeable waiter necessarily a bug,
   not just slowness? (Argument sketch is in the comment — formalize it:
   the releaser must be runnable, in flight, or parked; what does an
   all-parked configuration imply?)
- **No atomics anywhere** — and why that's not reckless: the loop-thread
   contract. `Baton` holds `io::Context&` and every mutation happens on the
   loop thread. Contrast with `folly::coro::Baton`'s atomic state machine
   (baton.md §0 compares them). What does folly pay for multi-thread
   semantics, line by line? This is your first real taste of "concurrency
   is a *choice of* sharing, not a sprinkle of atomics".
- **Lazy allocation** (`baton.h:99-107`): waiters queue allocated on first
   park, capacity 16. When is eager allocation the right call instead?
   What's the actual cost model (the comment prices an eager 1024-slot ring:
   8 KiB + one malloc *per Baton*)?

### Labs

**Lab 3.1 [toy scheduler + baton]** — Build the minimal harness you'll reuse
for the rest of the stages: a `struct Loop` with a
`std::vector<std::coroutine_handle<>>`, a `post(h)` that pushes, and a
`run_until_drained()` that pops and resumes — i.e., a degenerate kio Context
with no io_uring. Implement your own `Baton` on top of it (single-threaded,
plain bool, wake-via-post). Verify: stickiness (late waiter never parks —
assert your awaiter's `parked_` stayed false), broadcast (3 waiters, one
post, all three resume, FIFO or not?).
- Then break the wake discipline: make your `post()` resume inline. Write a
   `release()`-style function that runs *inside* a coroutine, and construct
   a reentrancy failure (e.g., a waiter whose body destroys the very object
   currently being iterated). Observing the crash (or the ASAN report)
   teaches why `context.h` routes every wake through Phase 1.

**Lab 3.2 [steal the permit]** — On your toy scheduler, implement the
   counting semaphore *without* `reserved_` first. You control the loop, so
   you control interleavings: engineer a run where, between the wake (handle
   pushed to ready list) and the resume, a *new* coroutine runs
   `try_acquire()` and takes the last permit. Instrument (print) both
   acquisitions. What does the woken waiter observe when it finally resumes?
   Then add your own `reserved_` (surplus rule in try_acquire, consume in
   await_resume) and watch the steal become impossible. You will have
   re-derived `semaphore.h:37-101` — and you'll never forget why the
   invariant `count_ >= reserved_` must hold.

**Lab 3.3 [quiescence detective]** — Write a toy run loop with kio's exit
   CHECK (parked waiter + empty ready list + no inflight ⇒ abort). Produce
   the abort three different ways: a Baton never posted, a Semaphore never
   released, a baton posted *from a coroutine that itself parked*. The third
   one is the interesting one — why does posting-while-parked count as a
   cycle? (This mirrors `context.h:276-290` exactly.)

### Questions

1. `Semaphore::try_acquire` serves only surplus (`semaphore.h:37-45`). Why
   does the invariant say `count_ == reserved_` whenever waiters are parked
   (comment, lines 23-24) — where would a surplus permit *come from* while
   anyone is parked?
2. Both awaiters are copy-deleted (`baton.h:73-74`, `semaphore.h` same).
   Awaiters live in the awaiting frame — what could go wrong if an awaiter
   were copyable and got copied while suspended?
3. `release()` may be called "when count_ was at its initial maximum"
   (`semaphore.h:48-50` — count is a floor, not ceiling). How does POSIX
   `sem_post` behave here, and why does allowing over-release simplify
   the proof of the wake protocol?
4. `Baton::post` is idempotent; `Semaphore::release` is NOT (it injects a
   permit every call). Find a use site where idempotence would be load-
   bearing, and one where it would mask a bug.

---

## Stage 4 — Context: the run loop, three phases, and the kernel boundary

**Read:** `include/kio/context.h`, focusing on `run()` (226-313), `reap()`
(464-489), `post()` (435-446), `get_sqe()` (379-419), `activate()`
(504-540), and the hot/cold member layout (348-374).

### Predict first

1. The loop is Phase 1 (resume ready coroutines) → Phase 2 (submit + wait)
   → Phase 3 (reap CQEs into the ready list). Why must Phase 3 *never*
   resume a coroutine inline, only queue? (You answered a version of this
   in Stage 3 — same discipline, one level up.)
2. What does `io_uring` actually mmap into your process? Draw SQ/CQ rings,
   the tail/head indices, and who writes which side. Why can submission be
   entirely userspace (no syscall) until it can't?
3. `DEFER_TASKRUN` (flag at `context.h:37-38`): what task work is deferred,
   and why does the comment at `context.h:294-304` claim a busy
   self-refilling ready list would *starve completions* under plain submit?
   This is subtle and empirical — the comment says it was verified on a
   specific kernel.

### Concepts on display

- **io_uring from first principles**: SQ/CQ as shared-memory rings (yes —
   the kernel uses the *same* ring-buffer idea as ReadyRing, Stage 1),
   `user_data` as the completion→coroutine correlation key (kio packs the
   `IoAwaitableBase*` pointer there, `context.h:472-474`), SQE batching —
   why building SQEs in userspace and submitting N-at-once amortizes
   syscalls.
- **Pointer tagging** (`IGNORE_TAG = 1`, `awaitable.h:15,68`): the low bit
   of the op pointer is free because `alignof(IoAwaitableBase) >= 2`.
   CQE user_data either points at an op or is the tag for link-timeout/
   cancel completions that must be skipped (`context.h:473`). Classic
   bit-stealing — grounded in an alignment guarantee.
- **Epoch cap and starvation policy** (`context.h:246-275`): at most
   `batch_max_size` resumes per epoch so Phase 2 (kernel submit) happens
   regularly. FIFO across epochs falls out of ring order (comment at
   270-275) with zero bookkeeping. This is *latency vs throughput* policy
   expressed as a loop shape.
- **Error-path discipline with errno-style returns**: `-EINTR` retry,
   `-EBUSY` drain-then-retry in `get_sqe()` (379-419). Which errno values
   mean "nothing happened, safe to retry" vs "the ring is broken"? And why
   can `reap()` inside `get_sqe` never reap *the calling coroutine's own
   op*? (The comment at 386-389 argues it — check the argument.)
- **Hot/cold field layout** (`context.h:348-374`): members ordered by access
   frequency so the loop's working set spans few cache lines. This is
   data-oriented design in miniature. What would a profiler say about the
   two `mutable`/cold members? Why is `owner_tid_` `mutable` at all?
- **`IORING_SETUP_R_DISABLED` + late activation** (`context.h:84-97`):
   construct-on-one-thread, run-on-another, via a ring that records its
   issuer at enable time (SINGLE_ISSUER semantics). Read the comment —
   it's a portable lesson in API identity/ownership traps.
- **Fail-fast as a philosophy**: CHECK (not assert — survives release,
   `context.h:174-176`), LOG(FATAL) → abort on broken rings, destructor
   CHECKs for leaked inflight ops/unstarted work. Compare: errno returns
   for *expected* failures vs aborts for *invariant* violations. Where's
   the line, and who drew it?

### Labs

**Lab 4.1 [LINUX] uring by hand** — Before trusting kio's wrapper, open a
ring yourself with raw `io_uring_setup` + `mmap` (or liburing's
`io_uring_queue_init`), submit one NOP with `user_data = 0xdeadbeef`, reap
it. Then:
- `strace -f -e trace=io_uring_setup,io_uring_enter,mmap ./yourprog` —
   count the syscalls for: submit-then-wait per op vs batched submit of 64
   then one wait. Compute the amortized syscall cost per op. This is the
   entire economic argument for io_uring, measured by you.
- Look at `/proc/<pid>/maps` while parked: find the SQ/CQ mappings. Their
   size tells you the ring geometry (entries × SQE size + CQEs).

**Lab 4.2 [LINUX] DEFER_TASKRUN, verify the comment** —
   `context.h:294-304` claims: with DEFER_TASKRUN, plain `submit` (empty SQ)
   never enters the kernel, and completions only post when the loop asks
   for events; a busy self-refilling ready list starves completions.
   Build the counterexample harness: a kio Context where spawned tasks
   keep spawning tasks (ready list never empties), one op in flight.
   Instrument (or `strace`) to show what happens with
   `submit_and_get_events` vs plain submit. You are reproducing an
   empirical finding documented in the code — this is real kernel-behavior
   research methodology.

**Lab 4.3 [user_data forensics]** — Read `reap()` until you can answer:
   what exact 64-bit value does a canceled link-timeout CQE carry, and
   which branch skips it? Then write a kio test that uses `.timeout()` on
   an op that *succeeds* first — how many CQEs come back, with which
   user_data and which `res` codes? (Answer via instrumentation you add
   locally, e.g. a print in reap — do not commit it.)

### Questions

1. Why does `run()` return only when `ready_list_.empty() && inflight_ == 0`
   *and* the parks check passes — why isn't empty+inflight enough? What
   user bug does the parks CHECK catch that would otherwise hang forever?
2. `post()` is `noexcept` but calls `ready_list_.push` which can allocate.
   Read `context.h:435-446` fully: what does the standard say happens when
   an exception escapes a `noexcept` function, and why does kio call that
   "matching the library's error handling philosophy"?
3. The destructor CHECKs `inflight_ == 0` and `ready_list_.empty()`
   (`context.h:177-181`). Give a concrete user-code sequence for each
   CHECK firing.
4. `cancel_all()` uses `IORING_ASYNC_CANCEL_ALL|ANY` with a tagged SQE
   (`context.h:335-345`). Why does the cancel's *own* CQE need
   `IGNORE_TAG`, and what does `canceling_all_` (the latch) fix in
   `await_resume` (see `context.h:579-591`) — what bogus error would the
   user see without it?

---

## Stage 5 — IoAwaitable & operations: type-safe kernel bridging

**Read:** `include/kio/detail/awaitable.h` in full, then
`include/kio/operations.h:1-100` (the header comment is the curriculum),
then skim the rest of operations.h for the shapes.

### Predict first

1. `IoAwaitable` takes a `Prep` callable invoked later, inside
   `await_suspend` — long after the call site's full-expression ended.
   What lifetime rule does that impose on everything the lambda captured?
   (The header comment at `operations.h:9-25` states the rule — derive it
   yourself from first principles first: when does `co_await async_read(...)`
   actually execute the lambda?)
2. The awaitable stores `__kernel_timespec scratch_{}` and `link_ts_` as
   members, with the comment "Both must live in the frame: the kernel reads
   them at submit time, after await_suspend has returned" (`awaitable.h:110-116`).
   Explain the dangling alternative: why isn't a timespec passed by value to
   `.timeout()` enough? Where exactly does the kernel's read happen relative
   to your C++ scopes?
3. `state` packs lifecycle (3 bits) + 3 flag bits into one byte
   (`awaitable.h:29-38`). What did the previous layout cost, and what does
   packing save — one byte per op, or more? When is bit-packing worth it?

### Concepts on display

- **`std::invocable` concept as a template parameter**
  (`awaitable.h:70`): the Prep constraint. This is C++20 concepts doing
   real interface documentation. What error message do you get if you pass
   a wrong-signature lambda? Compare mentally to the SFINE-era alternative.
- **`[[no_unique_address]]` on a lambda member** (`awaitable.h:109`): empty
   (capture-less) lambdas are empty types — the member can overlap other
   subobjects. Verify: `sizeof` an `IoAwaitable` with a capture-heavy vs
   capture-less Prep. Which ops in operations.h have the heavy ones?
- **Template bloat vs type erasure trade**: every distinct Prep lambda =
   distinct `IoAwaitable` instantiation. What's the compile-time/code-size
   cost? What would `std::function` cost at runtime instead? Why did kio
   choose the template?
- **Expected/Result plumbing in `await_resume`**
  (`context.h:567-595`): the ETIMEDOUT-vs-ECANCELED disambiguation is a
   masterclass in "the same errno means different things depending on
   which of *three* sources requested the cancel". Enumerate them.
- **`[[nodiscard]]` discipline** (`operations.h:29-31`): discarding the
   awaitable silently drops an I/O error *and* leaves a suspended
   coroutine. Why is the awaiter (not the result) where the error lives?
   Trace it: where does `res` get written, where read?

### Labs

**Lab 5.1 [build your own op]** — Add a new operation to kio by yourself:
`async_nop(Context&)` (prep with `io_uring_prep_nop`)... then something
real: `async_fsync` with a `timeout()`. Rules: capture-by-value only,
follow the one-result-type convention, `[[nodiscard]]`. Write a test that
runs it. This is your first *contribution-grade* understanding: you can
extend the library correctly without copying an existing op verbatim.
- Then write the op *wrong* on purpose: capture a `std::string_view` to a
   temporary path. Predict what happens (when? at submit, at completion,
   never?), run under ASAN, and compare with your prediction. If nothing
   blows up, explain *why* it got lucky — luck is the most instructive
   outcome here.

**Lab 5.2 [state packing]** — Reproduce the layout math in a standalone
   program: a struct with `enum class` + 3 `bool`s vs the packed byte.
   `static_assert(sizeof(...))` both, and use `__builtin_offsetof` /
   `-fdump-record-layouts` to see the real layout. What is the alignment
   rule that made the bools free anyway (they'd fit in padding)? When does
   packing *not* save anything? (Careful reading of the comment at
   `awaitable.h:29-32` — what was actually saved was the 24-byte optional.)

**Lab 5.3 [TRY macros]** — `include/kio/try.hpp`: why can't `KIO_CO_TRY` be
   a function? (Two reasons: `co_return` inside, and declaring a variable in
   the caller's scope.) Write your own minimal version; then break it:
   what happens without `__COUNTER__` when the macro is used twice on one
   line — and why is `auto&&` + `std::move` at the end the right pattern
   (what does moving from an `expected` leave behind, and is that state
   valid)? Also explain the void-form split (`KIO_CO_TRY_VOID`) via
   `kio_try_unwrap_impl`'s `if constexpr`.

### Questions

1. `await_ready` returns false unconditionally (`awaitable.h:100`) — even a
   cached/zero-timeout op suspends. What would an eager-completion path
   require, and why did v2 refuse it? (Hint: where would the op object have
   to live for `co_await` to see a result without suspending?)
2. The TODOs in the file (`awaitable.h:85-87`, `574`, `592-594`) are real
   design debts. Pick the `etime_is_success`-per-op-branch one: sketch (not
   implement) the compile-time split that removes the branch — what does it
   cost in instantiations?
3. `.timeout()` returns `IoAwaitable&` for chaining. Why is the timespec
   split into `link_ts_` while sleeps reuse `scratch_` — what invariant
   about "a sleep cannot also carry a timeout" makes sharing sound?

---

## Stage 6 — RegisteredBufferPool: mmap, DMA registration, and RAII leases

**Read:** `include/kio/buffer_pool.hpp` — the layout comment block (131-204)
first, then `AcquireAwaiter` (216-264), `registration_index` (304-312),
`BufferLease` (28-117) and its `reset` (383-392). Then
`src/kio/buffer_pool.cc`. Companion: `design-notes/buffer-pool.md`.

### Predict first

1. Why round the mapping's base *and length* to 2 MiB and `madvise`
   MADV_HUGEPAGE? What does a 2 MiB compound page change for (a) page-table
   walks, (b) TLB reach, (c) page faults at first touch?
2. `registration_index` is `first_index_ + (offset >> 30)` — one shift,
   because slot_size divides 1 GiB and chunks are 1 GiB. Derive why the
   divisibility constraint guarantees no slot straddles an iovec boundary,
   and what breaks (which kernel check, which op) if a slot did.
3. The free list is LIFO but waiters are FIFO (Invariant 5). State the
   cache-warmth argument for LIFO, and the starvation argument for FIFO,
   and why kio can afford *both* at once.

### Concepts on display

- **mmap as an allocator**: `MAP_ANONYMOUS|MAP_NORESERVE`, alignment by
   over-mapping and unmapping the head/tail slivers (read the .cc — the
   technique of "map guard, round up inside, return slivers" is a
   transfers-anywhere pattern for guaranteed alignments), `madvise`
   policies. What is `MAP_NORESERVE` promising, and what happens under
   overcommit when memory actually runs out?
- **Registered/pinned buffers & DMA**: why does zero-copy I/O want
   kernel-registered, page-pinned memory (`io_uring_register_buffers`)?
   What does pinning cost the rest of the system? Why is the iovec table
   registered *after* ring activation (`context.h:524-537`)?
- **RAII with identity — the BufferLease** (`buffer_pool.hpp:28-117`):
   move-only, `std::exchange` in moves, idempotent `reset()`, explicit
   `operator bool`, span conversions. But the deep part is the *release
   path is private, friend-accessed only through the lease*
   (`buffer_pool.hpp:337-346`): the comment explains what a public
   raw-ordinal release would enable — double release → two leases on one
   slot → silent corruption, and a vector push past reserve() inside
   noexcept → terminate. Access control as memory-safety policy.
- **The reserved-wake protocol, round two** (Invariant 4): identical shape
   to Stage 3's semaphore, but now the resource is a memory slot and the
   wake callback is installed by the Context (function pointer + void*,
   `buffer_pool.hpp:211`, installed at `context.h:133-135`). Why a plain
   function pointer instead of `std::function`? (noexcept, no allocation,
   no SSO games — and the callback is set once.)
- **Wake-via-callback, never inline** (Invariant 6): the releaser's frame
   must never host the waiter's resume. Same law as Stage 3/4, third
   appearance. By now you should be able to state *the general theorem*
   and where each layer enforces it.

### Labs

**Lab 6.1 [LINUX] aligned mapping from scratch** — Write your own
   `map_aligned(size, alignment)` using the guard-and-trim technique. Verify
   the returned pointer's alignment with `std::bit_cast<uintptr_t>` and a
   CHECK. Then unmap only the head/tail slivers — prove nothing leaked by
   diffing `/proc/self/maps` before/after (parse it in-program with
   std::ifstream — 20 lines, write them).
- `madvise(MADV_HUGEPAGE)` a touched region, sleep, and read
   `AnonHugePages:` from `/proc/self/smaps`. THP is best-effort — what
   makes a collapse fail? (Try sizes just under/over 2 MiB.)

**Lab 6.2 [branchless index]** — Microbench the branchless lookup vs the
   naive loop-over-iovecs on a table of 4 regions with random pointers
   inside. Is the branchless form actually faster at N=4? At what N would
   it matter? Lesson: "fast" constructs need a workload before they're
   fast. Then verify the divisibility invariant by construction: generate
   all slot boundaries for slot_size = 64 KiB and 128 KiB over a 2.5 GiB
   mapping, assert each lands exactly on a 1 GiB multiple or not — where's
   the first slot_size that *straddles*? (You'll have to relax the
   constraint to find one — that's the point: the config CHECK at
   `context.h:157-160` exists because straddling is easy to walk into.)

**Lab 6.3 [lease discipline]** — Using the real pool + a real Context (copy
   test setup from `tests/kio/buffer_pool_test.cpp`):
   (a) move-assign one lease over another and verify the old slot returned
   (pool `available()` before/after), (b) let a lease die at scope exit —
   same check, (c) construct the exhaustion interleaving from Lab 3.2 with
   real buffers: N slots, N+2 concurrent acquires, watch FIFO wake order
   in the logs. Then try to double-release by taking a `BufferLease*` and
   calling `reset()` twice — idempotent by contract; where in the code is
   that guaranteed?

### Questions

1. The pool "RELIES on the Task-lifetime invariant" (`buffer_pool.hpp:182-184`)
   — parked handles cannot dangle because Tasks CHECK-abort when destroyed
   suspended. This is a *cross-component* invariant. What would it take to
   make it local (e.g., intrusive list nodes that unlink on destroy), and
   why did kio refuse to pay it? (baton.md's folly comparison has the
   multi-thread version of the same trade.)
2. `close()` wakes every waiter with `operation_canceled`
   (Invariant 7). Walk the interleaving where a waiter was *already woken*
   (popped, counted in `reserved_`, sitting in ready_list_) when `close()`
   runs — what does its `await_resume` hit: the closed_ branch or the
   reservation branch? Is a woken-then-closed waiter's retry guaranteed to
   succeed? Read `AcquireAwaiter::await_resume` (245-263) carefully and
   find your answer in the CHECKs.
3. Why is `available()` documented as "including those reserved for woken
   waiters" (`buffer_pool.hpp:322-324`)? Who is the intended consumer of
   that number, and what would they conclude wrongly from it?

---

## Stage 7 — Architecture: share-nothing, sharding, and the benchmark habit

**Read:** `design-notes/sharding.md` (the decided stance), `readme.md`,
then *skim* `benchmarks/cpp/` and `examples/kio/` (03_http_services.cpp is
the showpiece). Revisit `context_remote_test.cpp` in `tests/` for how the
multi-Context stance is tested.

### The systems questions (no labs — these are essay/oral questions)

1. Share-nothing: one Context (ring, ready list, pool) per thread, no
   cross-thread wakes. What does this delete from the design (count the
   atomics and memory-ordering arguments that would otherwise appear in
   Baton/Semaphore/pool)? What does it make *hard* (cross-thread
   coordination, work stealing, load imbalance)? Where's the seam —
   how *would* two shards exchange a buffer without sharing? (The
   leader/WQ-attach option in `IoOptions.share_work_queues`
   (`context.h:50`) shares one thing deliberately — which thing, and why
   is it safe to share?)
2. `IORING_SETUP_ATTACH_WQ` (`context.h:100-104`): what is the shared
   work-queue (async workers for hashed ops), and why is sharing the *pool
   of kernel workers* compatible with share-nothing *userspace*?
3. The benchmark discipline in this repo (never benchmark sanitizer
   builds — Makefile comment; seastar/asio/photon baselines in
   benchmarks/): what makes a coroutine-IO benchmark honest? What are the
   standard lies (warmup, core pinning, IRQ affinity, small-object
   allocators, TCP loopback artifacts)?
4. End-to-end: trace one `async_read` on a shard from `co_await` to resume,
   naming every data structure it touches (Task frame → IoAwaitable in
   frame → SQE in mmap'd SQ → CQE → reap → ReadyRing → Phase-1 resume) and
   every thread it runs on. If you can do this cold, on a whiteboard, you
   have the whole library.

---

## Stage 8 — The testing-gap lab: tests kio owes you

Method: for every documented invariant in a header, a test must exist that
fails if the invariant breaks. A gap is an invariant with no pin. Below is
the full inventory from cross-referencing the six live test files against
the headers — organized as labs. **You write the tests** (that's the point:
designing the failing scenario is harder and more instructive than writing
the implementation was). New tests go in the existing per-component files,
registered like the others in `tests/CMakeLists.txt` (`kio_add_test`),
run with `make docker-t T=<suite>`.

### Finding 0 — the suite is six files, not nine

`shutdown_test.cpp`, `io_regression_test.cpp`, `context_remote_test.cpp`
are 100% commented v1 code (old `IO`/`sync_wait`/cross-thread API). The
CMake comment says "restore each one as its v2 counterpart lands" — but
which counterparts survived the share-nothing decision? Verdict to argue:
the cross-thread, `sync_wait`, and grace-period tests are **obsolete by
design** (sharding.md §6); four scenarios are worth resurrecting in v2
form and appear in the tiers below (empty-lease death #5, malformed-IP
#22, completion burst #21, and nothing else). Decide: salvage then delete,
or keep as reference.

### Tier 1 — quick wins: pinned contracts (death tests + trivial semantics)

Each is an afternoon or less. For EXPECT_DEATH, copy the established idiom:
spawn/run **inside** the death statement so the parent's `~Context` never
sees unstarted work (see `baton_semaphore_test.cpp:22-26`).

1. **`~Task` destroyed-while-suspended** (`task.h:176`) — the most
   important missing death test; the CHECK that the whole pool's
   handles-can't-dangle argument leans on, and it has never fired in a
   test. The lab is the *construction*: a started Task, suspended at an
   internal await, must be destroyed while its park is still counted —
   without `run()`'s exit CHECK aborting first. Hint: the destruction has
   to happen *during* Phase 1, while some other coroutine is running.
2. **`ReadyRing` constructor rejects non-pow2** (`ready_ring.h:41-42`)
   and **push of null handle** (`ready_ring.h:48`) — two death tests,
   zero lines of setup.
3. **`~Context` with unstarted work** (`context.h:181`): `spawn()` then
   destroy, never `run()`.
4. **Empty-lease fixed op dies** — v1 had this
   (`FixedReadRejectsEmptyAndForeignLeases`); v2 only covers the
   *foreign* half (`ForeignLeaseDies`). A default `BufferLease{}` into
   `async_read_fixed` must hit the same CHECK.
5. **`close()` idempotence + post-close acquire**: `close(); close();`
   cancels each waiter exactly once; an `acquire()` arriving *after*
   close returns `operation_canceled` via `await_ready` without parking
   (buffer_pool.hpp:235-237 — the parked-waiter test never exercises the
   late-arriver branch).
6. **Errno propagation is a Result, not an abort**: `async_read` on a
   closed fd → `EBADF` in the Result. The v2 suite asserts exactly one
   *failure* class (timeout/cancel); no op has ever failed a plain errno
   test. One socketpair or temp file covers the pattern for the matrix in
   Tier 3.
7. **`UniqueFd` contract** (`fd.hpp`) — zero coverage today, and it hides
   one subtle line: `reset()` early-returns when `fd_ == fd && closer_ ==
   closer` (fd.hpp:64) — self-reset must not close. Pin: move-source is
   invalid, `release()` hands off without closing, custom closer runs
   exactly once, self-reset no-ops, `set_options` fold short-circuits on
   first error. This is the RAII-101 lab.
8. **Baton double-post no-op** (`baton.h:47`), and
   **`waiting()`/`available()` accessors** report truth — three trivial
   assertions that catch future refactors quietly changing semantics.
9. **Semaphore over-release** (`semaphore.h:48-50`): release above the
   initial count is legal (floor, not ceiling) — release twice on
   initial=1, two acquires must succeed.

### Tier 2 — protocol depth: interleavings the suite never drives

10. **Wrapped-state grow** — `ReadyRing::grow()`'s two-leg memcpy path
    (`ready_ring.h:89-91`) has *never executed under test*: every grow in
    the suite happens with `head_ == 0` (push-only until full). Fill 8,
    pop 3 (head mid-ring), push until grow — the copy now wraps. This is
    the kind of gap that sits quietly until a production grow at 3 a.m.
11. **Broadcast-many forces waiter-ring growth** (`baton.h:28-32`): the
    lazy 16-slot waiter ring's `grow()` only runs when >16 waiters park —
    park 40 recorders on one Baton, post, assert all 40 wake in park
    order. Every broadcast test today has 3.
12. **Newcomer steal at the ready-list level** — distinct from the
    covered releaser-steals (`ReleaserCannotStealReservedPermit`): after
    `release()` wakes waiter W (W sits in `ready_list_`, permit counted in
    `reserved_`), a *third* coroutine running in the same Phase-1 epoch
    must fail `try_acquire`. Deterministic because Phase 1 resumes FIFO —
    engineer the order, assert the surplus rule holds at epoch scope.
13. **FIFO across the epoch cap**: `context.h:270-275` promises the
    capped backlog stays at the ring's head — every run-loop test asserts
    only *counts*. `batch_max_size = 4`, spawn 10 recorders, completion
    order must be 1..10 across three epochs.
14. **`cancel_all` latch resets per run session** (`context.h:228-231,
    340`): session 1 cancels an op (reports ECANCELED); session 2's fresh
    linked-timeout must report ETIMEDOUT again — never a disguised
    cancellation. No test runs `run()` twice after `cancel_all()`.
15. **Woken-then-close race** — Stage 6's question 2, as a test: waiter
    popped and woken (in `ready_list_`, counted in `reserved_`), `close()`
    fires before its resume. Which branch of `AcquireAwaiter::
    await_resume` runs, and does the reservation CHECK hold? This is the
    pool's least-traveled interleaving.
16. **Park without a wake callback dies**: a standalone
    `RegisteredBufferPool` (no Context), `acquire()` parks → CHECK "no
    wake callback installed" (buffer_pool.hpp:353-357). Guards the
    invariant that parking is only legal under an owning loop.
17. **Lease move-assignment returns the old slot**: the v2 plan listed
    "lease move semantics" as a test; `LifoWarmthOrder` uses `reset()`
    only. Move-assign lease B over live lease A → A's slot back on the
    free list (watch `available()`), B's slot survives.
18. **`timeout()` splits seconds/nanoseconds** (`awaitable.h:89-98`):
    every timeout in the suite is a round number. Arm 1500 ms on an op
    that completes at ~100 ms — and separately, verify a 2500 ms timeout
    fires at ~2.5 s, not at 2 s (the `duration_cast` truncation bug
    class).

### Tier 3 — structural coverage: the two biggest holes and the rest

19. **Op-surface smoke matrix** — *the* gap. sharding.md §7 claims "the
    full storage syscall set," but the live suite exercises exactly four
    awaitables: `recv`, `sleep`, `read_fixed`, `write_fixed`. Untested:
    `async_read`, `async_write`, `readv`, `writev`, `open`, `close`,
    `fsync`, `ftruncate`, `fallocate`, `rename`, `accept`, `connect`,
    `poll`, `send`. One table-driven suite: per op, one success assertion
    plus one errno failure (from #6's pattern). This is also where the
    lambda-capture-lifetime rules from operations.h get exercised for
    real — write at least one op *wrong* on purpose and watch which
    sanitizer catches it.
20. **Concurrency burst → SQ backpressure**: >100 ops in flight on an
    `entries = 64` ring forces `get_sqe()`'s space-left path
    (mid-epoch submit, context.h:379-419) and multi-batch reaps — the
    v1 `CompletionBurstLargerThanHarvestCap` is the direct counterpart to
    resurrect. Pre-fill a socketpair/pipe within its 64 KiB capacity (the
    v1 file's `static_assert` trick) so every read completes immediately.
    Assert all N complete, none resume twice (a counter that reaches
    exactly N).
21. **Net layer: zero tests.** `SocketAddress` parse rejection is the
    direct salvage of v1's `MalformedIpLiteralIsRejected...` (accepts
    "127.0.0.1"/"::1", rejects "127.0.0.l"/"::zz", bind on "not-an-ip"
    fails EINVAL); then a loopback listener + echo round-trip to pin
    `accept`/`connect`/`send`/`recv` end-to-end.
22. **Multi-region fixed I/O at a 1 GiB boundary** (optional, deep):
    `slot_size` 1 GiB × 2, sparse temp file, read/write only pages
    straddling the GiB boundary — proves the no-slot-straddles-an-iovec
    invariant with real DMA rather than arithmetic (SlotMathTwoRegions
    proves the math, not the kernel). Needs overcommit headroom; lazy
    pages keep it cheap.

### Accepted gaps — write down *why*, then stop (judgment is the skill)

- `registration_index`'s u16 ceiling CHECK: unreachable under ~64 TiB of
  pool. Belt-and-suspenders only.
- `~Context`'s inflight CHECK: single-threaded `run()` cannot return
  with `inflight_ > 0`; the CHECK guards future reentrancy, not a
  reachable state.
- The `-EINTR` retry loops: deterministic testing needs signal injection;
  kernel-behavior coverage, not unit-testable.
- Timeout firing exactly as data arrives: racy by nature; never assert
  timing races.
- `head_`/`tail_` 2^64 wrap: arithmetic argument (Stage 1's paper
  exercise), not executable.

---

## Concept index — C++ feature → where kio exercises it

| Concept | Where |
|---|---|
| Trivially copyable + memcpy legality | `ready_ring.h:14,65-66` |
| `make_unique_for_overwrite` | `ready_ring.h:39,87` |
| pow2 masking vs modulo | `ready_ring.h:53`, lab 1.1 |
| `alignas` / false sharing / interference size | lab 1.2, `context.h:348` hot/cold |
| `__builtin_prefetch` | `context.h:261-266`, lab 1.3 |
| Coroutine handle/promise/frame | `task.h` all, lab 2.2 |
| `suspend_always`/`suspend_never`, symmetric transfer | `task.h:35,37-55,207-208` |
| `noop_coroutine` vs null-handle sentinel | `task.h:24-26,49` |
| `std::expected` (unexpect, converting ctors) | `task.h:74,89-92`, `try.hpp` |
| Move-only RAII, `std::exchange` | `Task`, `BufferLease` |
| `[[nodiscard]]` as safety | `task.h:106`, `operations.h:29-31` |
| Awaiter protocol (3 hooks) | `baton.h:66-92`, `semaphore.h:66-102` |
| Concepts (`std::invocable`) | `awaitable.h:70` |
| `[[no_unique_address]]` | `awaitable.h:109` |
| Bit-packing state + alignment tag guarantee | `awaitable.h:29-68` |
| Lambdas: capture lifetimes across suspension | `operations.h:9-25`, lab 5.1 |
| `std::span` everywhere (views, no ownership) | `BufferLease`, `operations.h` |
| `std::format` | `context.h:200-202` |
| `if constexpr` | `try.hpp:13-17` |
| `[[likely]]`/`[[unlikely]]` | `task.h:181`, `try.hpp:35` |
| errno-style error returns vs aborts (CHECK) | `context.h` throughout |
| mmap/madvise/THP | `buffer_pool.cc`, lab 6.1 |
| io_uring SQ/CQ, user_data correlation | `context.h:464-489` |

---

## Working rhythm with your tutor

Per stage: **predict → read → lab → questions → review**. Bring me:
- your predictions (wrong ones especially — we'll dig into *why* they were
  wrong, which is where the compiler/OS mental model gets fixed), and
- your lab code for layout/UB/cache review.

When you get stuck on a lab, describe what you *expected* to observe vs
what you observed. I will not hand you the working program; I will point
at the concept you're missing and ask you a question about it. That's the
deal.

Suggested order and rough effort per stage: 1 (2 sessions), 2 (3), 3 (2),
4 (3), 5 (2), 6 (3), 7 (1). One session ≈ one focused sitting with code
and labs open.
