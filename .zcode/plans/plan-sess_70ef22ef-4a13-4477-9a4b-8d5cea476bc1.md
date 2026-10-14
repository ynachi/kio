# Buffer pool v2: coroutine-thin pool, lease-first fixed I/O

## Decision record
- **API: lease-first** (user decision). `co_await pool.acquire()` → `BufferLease`; user passes the lease to fixed ops and may use it like a normal buffer (span-like surface). Fused ops (`pool.read(...)`) are future sugar on the same primitives — NOT in scope. Raw `span+buf_index` disappears from the public API entirely.
- **Waiting: one generic wait-queue member in the pool** (user decision). Plain handle FIFO — implemented as a `detail::ReadyRing` — woken via a callback Context installs (== `Context::post` → Phase-1 resume). Wake-and-retry + no-bypass rule ⇒ strict FIFO with no result channel, no intrusive nodes, no unlink-on-destroy, no reschedule hook.
- **Pool core stays coroutine-unaware:** slab + LIFO free list + `closed_`. Move constructor DELETED (pinned memory; moving is meaningless and was unsound with parked awaiters).
- **Misuse policy: CHECK fail-fast** for empty/foreign leases in fixed ops (default; kernel-memory-corruption class, matches "abort at the bug site" philosophy). v1's submit-on-fd−1→EBADF trick retired.
- **buf_ring: out of scope**, nothing precludes adding IOSQE_BUFFER_SELECT recv later.
- **Keep verbatim:** contiguous THP mapping, pow2 slot_size dividing 1 GiB, ≤1 GiB iovec chunking (kernel cap), `>> 30` branchless index, per-Context pool, LIFO freelist for cache warmth.

## Changes

### 1. `include/kio/buffer_pool.hpp` + `src/kio/buffer_pool.cc` — pool rewrite
- **Delete:** `Waiter` struct, nested `AcquireAwaiter` (with its Result channel), `enqueue_waiter`/`pop_waiter`/`remove_waiter`, `resume_waiter` + `set_reschedule_hook`, move ctor/assign, `FixedBuffer` alias.
- **Add `WaitQueue`** (~20 lines): wraps `detail::ReadyRing` + `using WakeFn = void(*)(void*, std::coroutine_handle<>) noexcept;` + ctx — members: `push(handle)`, `pop_one() → handle/nullopt`, `wake_all()` (for close), `empty()/size()`. No fence needed (bounded by parked tasks).
- **`acquire()`** becomes a ~15-line awaiter (free of pool internals):
  ```cpp
  bool await_ready()    { return pool.closed_ || pool.try_acquire_into(lease); }
  void await_suspend(h) { pool.waiters_.push(h); }
  Result<BufferLease> await_resume()
  {
      if (pool.closed_) return Error::fail_errc(operation_canceled, "pool closed");
      auto lease = pool.try_acquire();      // retry; no-bypass guarantees hit
      CHECK(lease) << "woken waiter found no slot (pool invariant broken)";
      return lease;
  }
  ```
  Documented dependency: kio tasks cannot be destroyed while suspended (`~Task` CHECK) — that is why a plain handle FIFO cannot dangle.
- **`try_acquire` no-bypass rule:** returns empty not only when closed/empty but also when `waiters_` nonempty (new arrivals park behind; a woken waiter's retry always finds its slot).
- **`release(ordinal)`:** `free_slots_.push_back(ordinal); if (waiters_ nonempty) wake_one via callback;` (push-then-wake; no direct handoff).
- **`close()`:** latch `closed_`, `wake_all()` — each waiter resumes into the canceled-error branch. Outstanding leases stay valid (release under closed just recycles to the freelist — v1 behavior).
- **Alignment FIX:** mmap `aligned_total + 2 MiB`, align the base pointer up to a 2 MiB boundary inside the mapping, `munmap` the head and tail slivers, then `MADV_HUGEPAGE` (today only the length is rounded — the THP claim is currently false for the base).
- **Index-space FIX:** `append_regions` and `registration_index` CHECK (not assert) the `uint16_t` ceiling: `first_index_ + region_count <= 65535` (SQE buf_index is u16).
- **`BufferLease`:** keep 32 B layout + RAII `reset()`; extend the span-like surface (`bytes()`, `first(n)`, `subspan()`, explicit `operator std::span<std::byte>()`) so ordinary buffer code works on it.
- **Quiescence accessor:** `waiting()` → O(1) waiter count (feeds run()'s exit condition — replaces the previously-envisioned external-pending counter with a poll).

### 2. `include/kio/context.h` — wiring (resurrects v1's init path)
- Ctor gains `const std::optional<BufferPoolConfig>& pool_cfg = std::nullopt` (v1 shape; `BufferPoolConfig{slot_size=65536, slots=1024}` stays).
- `init()`: when configured — construct pool, `pool->append_regions(registered_iovecs_)`, `io_uring_register_buffers(&ring_, iovecs.data(), n)`; on failure `throw std::runtime_error` with strerror + config (v1 pattern, fills the `// TODO: register buffer`).
- `run()` exit condition becomes `ready_list_.empty() && inflight_ == 0 && (!buffer_pool_ || buffer_pool_->waiting() == 0)`.
- Phase 2 unchanged (blocks on `ready_list_.empty()` only): waiters + inflight > 0 correctly block for the CQE that will release.
- **Deadlock CHECK:** at the exit check, if `inflight_ == 0 && ready empty && waiting() > 0` → CHECK-fail with "pool exhausted with waiters and no in-flight ops (leases leaked?)" instead of spinning forever.
- Member-order note: `buffer_pool_` is declared after `ring_`/`ready_list_` → destroyed before them; its close/wake callback (`post`) only touches still-alive members. Verified against current declaration order.
- Wake callback installed at pool construction: a small static trampoline calling `ctx.post(h)` on the owning Context.

### 3. `include/kio/operations.h` — lease-only fixed ops
- `async_read_fixed(Context&, Fd&, BufferLease&, size_t len, size_t buf_offset = 0, off_t = -1)` + whole-lease overload; `async_write_fixed` mirroring (const lease). `buf_index` derived internally via `registration_index` — never a parameter.
- At awaitable construction: `CHECK(lease && lease.pool() == &*io.buffer_pool())` "lease not registered with this Context" (the fail-fast policy; replaces v1's EBADF submission).
- No public raw span+index form.

### 4. Tests
- **Pool unit** (Linux — remote/docker): slot/index math across regions, LIFO order, no-bypass FIFO fairness (park via real Tasks), release-wakes-one, close cancels all waiters with `operation_canceled`, lease RAII release, lease move semantics, `registration_index` u16 CHECK, foreign-lease death test (EXPECT_DEATH on async_read_fixed with foreign pool).
- **Integration** (real io_uring, kernel 7.0 via docker/remote): registered fixed read/write round-trip on a temp file (write via lease → fsync-free read-back via lease); exhaustion path — pool of 2 slots, 5 coroutines acquire, releaser timer wakes them FIFO; quiescence — a coroutine parked on `acquire()` keeps `run()` alive (waits for a delayed release), then the deadlock CHECK path (parked waiter, no inflight, no releaser) aborts with the message.
- Existing 18 tests stay green (03_http example untouched — no pool by default).

### 5. Docs
- `design-notes/buffer-pool.md`: the analysis from this discussion (complexity anatomy, coroutine-awareness decomposition, why someone must park coroutines and why the wait-queue won), the settled design with diagrams, and the decision record.

### 6. Verification pipeline (same as ready-ring)
- Implementation executed by a GLM-5.3-Flash workflow (user's established cost split), spec'd from this plan; deterministic gates: `make docker-dev` (fix loop), new-files clang-format check, `make docker-check` (asan+ubsan+tsan); I review the diff against this plan's invariants and re-verify on srv401 (clang-21/libc++) before handing over. `.zcode/kio-remote.sh` and `.zcode/kio-fmt-check.sh` reused (extend the fmt list with the touched files).

### Out of scope
- Fused ops, buf_ring/kernel-side selection, size classes / multiple pools, shared-mmap-across-rings, readme v1 snippets.