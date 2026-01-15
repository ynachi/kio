# IoUringExecutor + async_simple integration (architecture)

## What this component is

We expose **`kio::next::v1::IoUringExecutor`** as an `async_simple::Executor` implementation, and we run *both*:
- coroutine scheduling work (resume/continuations), and
- kernel I/O completions

…through **io_uring CQEs**, so a single per-thread event-loop drives everything.

### Design goals
- **Single-issuer io_uring** per worker thread (safe + fast): only the owning worker thread touches `io_uring_get_sqe()` / submission for that ring.
- **No shared work-queues** on the hot path: “schedule” == enqueue a **NOP SQE** (task) into the target ring.
- **Cross-thread scheduling without locks** between workers: use `IORING_OP_MSG_RING` and a per-thread *control ring*.
- **Predictable thread-affinity** when needed: `Executor::checkout()/checkin()` + `IoUringAwaiter::resume_on()`.

## Key building blocks

### 1) Per-thread context (`PerThreadContext`)
Each worker thread owns:
- `ring` — the main io_uring instance (I/O + scheduled tasks)
- `control_ring` — used to send `MSG_RING` messages to other rings
- `task_pool` — an object pool for `TaskOp` (reduces allocs)

The worker thread sets a TLS pointer:
- `IoUringExecutor::current_context_` → indicates “we are currently on a worker thread”.

### 2) Task scheduling path (`TaskOp`)
When async_simple asks us to run some function (`Executor::schedule()` / `checkin()`), we:
1. Wrap it in `TaskOp`
2. Push a **NOP SQE** (same-thread) or send it via **MSG_RING** (cross-thread/external)
3. When the NOP “completes”, we execute `TaskOp::complete()` which runs the function and returns the object to the right pool.

#### Same-thread schedule
- Allocate `TaskOp` from this thread’s pool
- Put a `NOP` into this thread’s `ring`
- CQE arrives → `TaskOp::complete()` runs inline in the event loop

#### Cross-thread schedule (worker → worker)
- Allocate `TaskOp` from the *source* thread pool
- Send pointer to target via `IORING_OP_MSG_RING` using **source’s `control_ring`**
- Target ring receives CQE with `user_data = TaskOp*`
- Target executes it; returning the pool object back uses a *second* MSG_RING back to the source (marked as `PoolReturn`)

#### External schedule (non-worker thread → worker)
- Allocate `TaskOp` with `new` (no per-thread pool)
- Use `external_control_ring_` guarded by a mutex
- Send pointer to target with `MSG_RING`
- Target executes and then `delete`s the `TaskOp`

### 3) I/O awaiters (`IoUringAwaiter`)
`make_io_awaiter()` builds an awaiter with a `prepare_func(sqe)` callback.
Key behaviors:
- Picks a submission context automatically:
  - if already on a worker: submit to `currentContextId()`
  - else: `pickContextId()` (round-robin)
- Guarantees **single-issuer**: if the target context is not current, it schedules a small lambda *onto the target context* to call `io_uring_get_sqe()` and `prepare_func()` there.
- Default resumption: **inline on the submit-context thread**
- Optional resumption: `.resume_on(home_ctx)` → uses `Executor::checkin()` to hop back

### 4) Event loop
Each worker runs `runEventLoop(ctx)`:
- consumes CQEs in batches
- for each CQE:
  - decode `IoOp*` from `user_data`
  - dispatch:
    - `Task` / `PoolReturn` → run function / reclaim pool object
    - `Io` → call `IoUringAwaiter::complete(res)` → resumes the coroutine (inline or via checkin)

## Threading + affinity model (how to reason about “where code runs”)

### Three “places” code can execute
1. **External threads** (your main thread or app threads)
2. **Worker context N** (event loop thread N)
3. **Worker context M** (event loop thread M)

### Typical flows

#### A) Task started with `.via(&executor)`
```
external thread
  └─ via(&exec).start()
       └─ Executor::schedule(...)  → enqueue NOP on some worker ring
worker thread (ctx K)
  └─ TaskOp CQE → runs coroutine resume → (now coroutine is “on executor”)
```

#### B) Task started with `.start()` (no via)
```
external thread
  └─ Lazy::start() runs synchronously until first suspension point
      └─ co_await io_awaiter(&exec, ...)
            await_suspend → schedules submission on a worker ctx
worker thread (ctx K)
  └─ I/O CQE → resumes coroutine on worker thread
```
So: **the coroutine may “migrate” to a worker thread at the first I/O await.**

#### C) Explicit lane + hop back (`on_context()` + `resume_on()`)
```
worker thread (ctx A)
  home = exec.checkout()
  co_await read(...).on_context(B).resume_on(home)

worker thread (ctx B)
  I/O CQE arrives → completion calls exec.checkin(...)
worker thread (ctx A)
  continuation resumes here
```

## Integration points with async_simple

We implement the core `Executor` hooks used by `Lazy` and structured concurrency:
- `schedule(Func)` → schedule runnable onto the executor (NOP/MSG_RING)
- `currentThreadInExecutor()` → TLS check (`current_context_ != nullptr`)
- `currentContextId()` → which worker lane we’re on
- `checkout()` → return the lane identity (`PerThreadContext*`)
- `checkin(Func, Context)` → schedule runnable back onto that lane

That is enough for:
- `.via(&executor)` (RescheduleLazy scheduling)
- `directlyStart(cb, &executor)` (bind executor without immediate scheduling)
- `collectAny/collectAll` (child tasks can use the bound/current executor)
- `IoUringAwaiter::resume_on()` (hop back to a saved context)

## Known limitations / gotchas
- **Cancellation**: async_simple’s cancellation signals don’t currently cancel in-flight io_uring operations created by our awaiters. The coroutine can observe cancellation at its own checkpoints, but the kernel I/O will still complete.
- **schedule_info / priority**: we don’t currently use `schedule_info` or `ScheduleOptions` to influence lane choice or priority.
- **Backpressure**: if rings are full (`io_uring_get_sqe()` fails), we “trampoline” by resuming with an error path in some cases; correctness is preserved but it may degrade under extreme load.
