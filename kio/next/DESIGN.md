# Design

This project provides an io_uring-backed executor that implements the
async_simple executor interface and a set of awaiters for common I/O
operations. The goals are low overhead scheduling, explicit control over
I/O submission/resume context, and structured cancellation compatible with
async_simple.

## Components

- `io_uring_executor.h` / `io_uring_executor.cpp`
  - `IoUringExecutor` implements `async_simple::Executor`.
  - Per-thread worker ring for I/O and inbound tasks.
  - Per-thread control ring for outbound cross-thread messages.
  - External control ring for scheduling from non-executor threads.
- `io_awaiters.h`
  - I/O helpers: `read`, `write`, `accept`, `recv`, `send`, `connect`, `fsync`.
  - All helpers accept an optional `async_simple::Slot*` for cancellation.
- `tcp_demo_v1_optimized.cpp`
  - Example HTTP server using the executor and awaiters.

## Threading and Execution Model

- Each worker thread owns:
  - A primary io_uring ring for I/O operations and task CQEs.
  - A control ring used to submit `IORING_OP_MSG_RING` to other threads.
- Cross-thread tasks are delivered as CQEs via `msg_ring` on the target thread.
- Tasks scheduled from external threads are funneled through an external ring.

## Scheduling Paths

- Same-thread schedule: submit a `NOP` SQE on the local ring.
- Cross-thread schedule: `msg_ring` from source control ring to target ring.
- External schedule: `msg_ring` from the external control ring.

## Awaiter Semantics

`IoUringAwaiter` supports:

- `on_context(id)` to force submission on a specific worker context.
- `resume_on(ctx)` to resume on a specific executor context via `checkin`.

Resume modes:

- Inline resume on the submit context (default).
- `checkin` resume for explicit hop back to a different executor context.

## Cancellation Integration

Awaiters integrate with async_simple cancellation:

- `await_ready` checks cancellation via `signalHelper{Terminate}.hasCanceled`.
- `await_suspend` registers a cancellation handler with `signalHelper`.
- On cancellation, the awaiter requests `io_uring` cancel for the user_data
  and resumes with a cancellation error.
- `await_resume` calls `signalHelper{Terminate}.checkHasCanceled`, which
  throws `SignalException` on cancellation.

To propagate cancellation, pass a `Slot*` from `co_await CurrentSlot{}`
or bind a signal via `Lazy::setLazyLocal`.

## Shutdown and Draining

- `stop()` requests a stop token and wakes worker threads.
- Pending I/O is tracked by a counter to allow draining in-flight operations.
- If `cancel_on_stop` is enabled, `stop()` also issues cancel requests for
  all pending I/O before draining.

## Configuration Notes

`IoUringExecutorConfig` allows:

- `num_threads`, ring sizes, and batching settings.
- `immediate_submit` for low latency at the cost of more syscalls.
- `pin_threads` for CPU affinity and optional I/O worker affinity.
- `cancel_on_stop` to cancel in-flight I/O during shutdown (higher overhead).

## Known Tradeoffs

- Cancellation requires a `Slot*`; without it, cancellation signals cannot
  interrupt I/O awaiters.
- `cancel_on_stop` adds tracking overhead; keep it off unless needed.
- `io_uring` error paths (ring full) are surfaced as `-EBUSY` or `-ECANCELED`.

