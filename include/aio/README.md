# aio — Minimal io_uring + C++23 coroutine I/O (Linux)

`aio` is a focused async I/O library built on **Linux io_uring** and **C++23 coroutines**, designed as a low-level foundation for:
- storage engines (file I/O, registered files)
- network proxies (accept/connect/recv/send)
- event loops with predictable control flow and minimal abstraction

Core principles:
- **one `io_context` per thread** (thread-per-core)
- **direct coroutine resumption** from io_uring completions
- **caller-owned buffers** (no hidden allocations)
- **fail-fast safety** for coroutine/op lifetime issues (detection, not prevention)

---

## Requirements

- Linux kernel **≥ 6.x**
- `liburing`
- C++23 compiler
- `-pthread`

Build example:

```bash
c++ -O3 -std=c++23 -pthread your_app.cpp -luring -o your_app
```

---

## High-level design

### Threading model

- Each worker thread owns its own `aio::io_context` + `io_uring`.
- No cross-thread submission of regular I/O ops.
- Cross-thread coordination is done via:
  - a data queue (SPSC/MPSC depending on your topology)
  - a wake mechanism: **`IORING_OP_MSG_RING`** with a reserved `WAKE_TAG`

> If you enable io_uring flags that assume single-issuer or single-thread access, enforce that in your program (or just leave them off).

### Coroutine model

- Each I/O op stores a `std::coroutine_handle<>` to resume.
- The op’s address is stored in io_uring `user_data`.
- `io_context` drains CQEs into a vector of handles and resumes **after** CQ iteration:
  - avoids deep resume recursion / stack growth
  - gives a flat, predictable “process then resume” loop

### Operation lifetime model (critical)

Operations are embedded in coroutine frames (awaitable objects live inside the coroutine). The kernel keeps `user_data` pointing to those awaitables.

To avoid silent use-after-free:
- Each submitted op is **tracked** in an intrusive pending list inside `io_context`.
- On completion, the op is **untracked**.
- If an op is destroyed while still tracked, the program **terminates**.

This is **detection**, not prevention. The current implementation chooses “crash loudly” rather than “corrupt memory silently”.

---

## Features

### I/O operations

- File I/O:
  - `async_read`, `async_write`
  - `async_read_fixed`, `async_write_fixed` (requires `register_files`)
- Networking:
  - `async_accept`, `async_connect`
  - `async_recv`, `async_send`
  - `async_close`
- Timing:
  - `async_sleep(duration)`
- Cross-thread wake/message:
  - `async_msg_ring` (posts a CQE to another ring)
  - reserved `WAKE_TAG` for “wake only” CQEs on the target ring

### Timeouts

`.with_timeout(dur)` wraps an op using `IOSQE_IO_LINK` + `IORING_OP_LINK_TIMEOUT`.

Example:

```cpp
auto n = co_await aio::async_recv(&ctx, fd, buf, len, 0).with_timeout(500ms);
if (!n && n.error() == std::make_error_code(std::errc::timed_out)) {
  // timeout
}
```

Timeout mechanics:
- the timer SQE is linked to the op
- if the timeout fires, the linked op often completes with `-ECANCELED`
- `aio` maps that to `std::errc::timed_out` for clarity

### Fixed file table

```cpp
ctx.register_files(std::span<const int>{fds});
auto r = co_await aio::async_read_fixed(&ctx, file_index, buf, off);
```

This uses `IOSQE_FIXED_FILE` (registered file index). (This is not fixed buffers.)

### Blocking offload (DNS, etc.)

Some things are inherently blocking (e.g., `getaddrinfo`). Offload those to a separate pool and resume the coroutine back on the owning `io_context` thread.

The recommended pattern:
- pool threads run blocking work
- completion is posted to the target `io_context` via an external queue
- pool thread wakes the target ring with `async_msg_ring(..., WAKE_TAG)`

---

## Quickstart

### Single-thread example

```cpp
#include "aio.hpp"

aio::task<void> example(aio::io_context& ctx, int fd) {
  std::byte buf[4096];
  auto r = co_await aio::async_read(&ctx, fd, std::span{buf}, 0);
  if (!r) co_return;
  co_return;
}

int main() {
  aio::io_context ctx;
  auto t = example(ctx, 0);
  ctx.run_until_done(t);
}
```

### Minimal server accept loop pattern

```cpp
aio::task<void> handle_client(aio::io_context& ctx, int fd) {
  char buf[4096];
  static constexpr std::string_view resp =
    "HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: keep-alive\r\n\r\nHello, World!";

  while (true) {
    auto r = co_await aio::async_recv(&ctx, fd, buf, sizeof(buf), 0);
    if (!r || *r == 0) break;

    size_t sent = 0;
    while (sent < resp.size()) {
      auto s = co_await aio::async_send(&ctx, fd, resp.data() + sent, resp.size() - sent, 0);
      if (!s) co_return;
      sent += *s;
    }
  }

  (void) co_await aio::async_close(&ctx, fd);
  co_return;
}

aio::task<void> accept_loop(aio::io_context& ctx, int listen_fd, std::vector<aio::task<void>>& tasks) {
  while (true) {
    auto fd = co_await aio::async_accept(&ctx, listen_fd);
    if (!fd) co_return; // handle shutdown / errors as you prefer

    tasks.push_back(handle_client(ctx, *fd));
    tasks.back().resume();
  }
}
```

---

## Safety rules (must-follow)

These are required today for correctness.

### 1) Never destroy a task while it is suspended on I/O
If a coroutine is destroyed while an op is still in-flight, the kernel may later complete into freed memory.
`aio` detects this and **terminates**.

**Rule:** keep tasks alive until completion, or ensure their in-flight ops complete first.

Common pattern:
- store tasks in a container (`std::vector<aio::task<void>>`)
- periodically remove completed tasks

### 2) `io_context` must outlive all operations submitted to it
Ops store `io_context*`. The ring owns in-flight submissions.

**Rule:** stop/drain tasks before destroying the context.

### 3) Buffers must remain valid until completion
No buffer copying is done.

**Rule:** any buffer passed to read/write/recv/send must outlive the awaited op.

### 4) Resume only on the owning thread
Do not resume coroutine handles from foreign threads.
If you offload blocking work, schedule resumption back onto the owning `io_context`.

### 5) MSG_RING WAKE_TAG is a “kick”, not an I/O completion
Treat `WAKE_TAG` CQEs as “drain external queue” events, not as op completions.

### 6) Shutdown strategy matters
- `cancel_all_pending()` is a *destructive* shutdown tool.
- During destruction, `aio` should **not resume** coroutines (results are dropped).
- If you need graceful shutdown, stop accepts/new work and let tasks drain.

---

## Recommended patterns

### Task lifetime management (container + sweep)

If `task` supports move assignment, you can use `std::erase_if`:

```cpp
std::erase_if(tasks, [](aio::task<void>& t){ return t.done(); });
```

If you want to avoid move assignment requirements, do a move-filter compaction:

```cpp
std::vector<aio::task<void>> alive;
alive.reserve(tasks.size());
for (auto& t : tasks) if (!t.done()) alive.push_back(std::move(t));
tasks.swap(alive);
```

### External completions (offload pool → io_context)

If you have offloaded work completing on other threads, keep an external-ready queue per `io_context`:

- pool thread pushes completed coroutine handles into `ctx.enqueue_external(h)`
- pool thread wakes the ring via MSG_RING with `WAKE_TAG`
- `io_context::step()` drains CQEs, sees `WAKE_TAG`, then drains external-ready handles and resumes them

This keeps all coroutine resumption on the correct thread.

### Graceful shutdown (server)

A typical shutdown sequence:

1) Stop accepting new connections (close listen fd, or set a flag)
2) Wake workers (MSG_RING WAKE_TAG if they might be sleeping)
3) Let in-flight tasks complete (optionally with timeouts)
4) Stop workers (`ctx.stop()`), join threads
5) Destroy contexts

If you destroy `io_context` while tasks still exist, you will trip the fail-fast safety (terminate).

---

## FAQ / Troubleshooting

### “Program terminates during shutdown”
Most likely:
- a task/coroutine was destroyed while it had a tracked op in flight

Fix:
- keep tasks alive until `done()`
- stop accepting new work, drain tasks, then destroy context
- avoid “detach and forget” unless you keep the task object alive somewhere

### “I see lots of ECANCELED”
Common sources:
- linked timeout fired (expected)
- you closed an FD to cancel I/O (expected)
- you issued cancel requests on shutdown

If you use `.with_timeout()`, map `ECANCELED` to timed out (as `aio` does).

### “accept loop doesn’t stop when I call stop()”
`stop()` only flips a flag. If the worker is blocked in `submit_and_wait`, you need a wake:
- close the listen FD (accept should complete)
- or send MSG_RING WAKE_TAG to break the wait and let the loop observe the stop flag

### “recv returns error under normal disconnects”
For proxies, disconnects like `ECONNRESET` / `EPIPE` can be normal.
Treat them as expected connection teardown, not necessarily “errors”.

### “Logging slows everything down”
Formatting + synchronous writes can dominate hot paths.
Best practice:
- use debug logs only in development
- use counters + periodic summaries for hot paths
- consider an async logger thread if you instrument heavily

---

## Roadmap ideas (optional)

- Op-state pool/slab allocator (prevention of UAF instead of detection)
- Structured cancellation tokens
- Buffer rings / registered buffers for networking hot paths
- Multishot accept/recv for fewer syscalls (where useful)
- Async DNS integration (socket-driven resolver) + caching

---

## License / status

This is a minimal foundation library. Expect to evolve APIs and enforcement as invariants become clear in benchmarks and real workloads.
