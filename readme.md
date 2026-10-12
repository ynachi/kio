# kio — a share-nothing C++23 coroutine runtime on io_uring

kio is a low-latency asynchronous I/O library for Linux built on `io_uring`
and C++23 coroutines. Its design discipline is **thread-per-core,
share-nothing**: one `Context` (one ring, one ready list, one optional buffer
pool) per thread, no cross-thread data plane, no atomics on any hot path.

> **Status**: v2 rebuild, branch `rebuild-simple`. The core runtime (run loop,
> Task, operations, buffer pool, Baton/Semaphore) is complete and tested under
> ASan/UBSan/TSan. `examples/kio/03_http_services` builds against v2; example
> 02 is disabled pending its port; the stale v1 test files are
> salvage-then-delete (see `tests/CMakeLists.txt`). Design decision records
> live in `design-notes/`.

## Architecture

```
one shard = one thread = one Context
  ┌────────────────────────────────────────────────┐
  │ ReadyRing (growing pow2 ring of coroutine      │
  │            handles — the runnable queue)       │
  │ io_uring ring (SINGLE_ISSUER + DEFER_TASKRUN)  │
  │ RegisteredBufferPool (optional, mmap'd, THP)   │
  └────────────────────────────────────────────────┘

  run() = Phase 1: resume ≤ batch_max_size ready coroutines (FIFO)
          Phase 2: submit — block for a CQE only when nothing is runnable
          Phase 3: reap CQEs → post handles to the ready list (never resume)
```

Everything a coroutine touches — the ready list, the ring, the pool, the
primitives — is plain (non-atomic) memory guarded by a loop-thread contract,
CHECK-enforced in debug builds. Cross-thread coordination, if ever needed, is
control-plane over kernel sockets; the stance and its measured costs are
recorded in `design-notes/sharding.md` (~143k req/s/core on the v2 loop vs
~99k for v1's cross-thread machinery).

## Features

- **`Task<T>`** — lazy, move-only coroutine task with symmetric transfer
  (flat stacks), a `Result<T>` (`std::expected<T, Error>`) error channel,
  and fail-fast CHECKs on every lifetime misuse (double-await, abandoned
  in-flight tasks, result double-consumption).
- **Operations** — the full storage + network syscall surface as free
  functions over a `Context&`: `async_read/write/readv/writev`,
  `async_recv/send/accept/connect`, `async_open/close/remove/rename`,
  `async_fsync/fallocate/ftruncate`, `async_poll`, `async_sleep`, and
  registered-buffer `async_read_fixed/write_fixed`. Every op arms a
  linked-timeout via `.timeout(ns)`; per-op `cancel()` and teardown-wide
  `cancel_all()` classify their `-ECANCELED`s correctly (timeout vs explicit
  cancel vs teardown).
- **`Baton` / `Semaphore`** — loop-thread coroutine event and counting
  primitives with a FIFO reserved-wake protocol (kio's folly::coro::Baton,
  minus the multi-threaded atomics tax). See `design-notes/baton.md`.
- **`RegisteredBufferPool`** — a single contiguous 2 MiB-aligned THP mapping,
  sliced into power-of-two slots, registered with the ring for fixed-buffer
  zero-copy I/O. `BufferLease` is the move-only RAII handle (span-like
  surface); exhausted pools park acquirers FIFO. See
  `design-notes/buffer-pool.md`.
- **Leader/follower rings** — multiple `Context`s may share one kernel
  async-worker pool (`IORING_SETUP_ATTACH_WQ`) without sharing anything in
  userspace.
- **Fail-fast by design** — invariant violations (leaked leases, parked
  waiters with no releaser, foreign buffer leases) abort at the bug site
  with a named CHECK instead of corrupting or hanging.

## Requirements

- **OS**: Linux 5.19+ (`IORING_SETUP_SINGLE_ISSUER` + `DEFER_TASKRUN`);
  tested on kernels 6.8 and 7.0.
- **Compiler**: C++23. Clang 20/22 verified. GCC currently ICEs — use
  clang.
- **Libraries**: liburing (2.9 tested), CMake 3.25+.

## Quick start

```cpp
#include "kio/context.h"
#include "kio/fd.hpp"
#include "kio/operations.h"
#include "kio/task.h"
#include "kio/try.hpp"

#include <fcntl.h>
#include <iostream>
#include <span>

using namespace kio;

Task<> read_file(io::Context& io)
{
    KIO_CO_TRY(fd_raw, co_await async_open(io, "/etc/hostname", O_RDONLY));
    io::UniqueFd file{fd_raw};  // ops return the raw fd; ownership is explicit

    std::byte buf[256];
    KIO_CO_TRY(n, co_await async_read(io, file, std::span{buf}));
    std::cout << "read " << n << " bytes\n";
    co_return {};
}

int main()
{
    io::Context ctx{0};        // ring created disabled; nothing submitted yet
    ctx.spawn(read_file(ctx)); // detach a task onto the loop
    ctx.run();                 // drives until quiescent, then returns
}
```

Timeouts chain onto any op. The awaiter may be a temporary inside the
`co_await` expression — its lifetime spans the suspension — or a named
object. What dangles is binding a reference *before* the await
(`auto&& op = f(...).timeout(t); co_await op;` — no lifetime extension
through the returned reference; the SQE would tag dead memory):

```cpp
auto n = co_await async_recv(io, sock, std::span{buf})
                      .timeout(std::chrono::seconds(1));
// Result<int>: ETIMEDOUT on expiry, not ECANCELED
```

Buffers for fixed ops come from the pool, never from raw pointers:

```cpp
io::Context ctx{0, nullptr, io::IoOptions{},
                BufferPoolConfig{.slot_size = 4096, .slots = 1024}};
// inside a task:
auto lease = co_await io.acquire_buffer();   // parks FIFO when exhausted
KIO_CO_TRY(w, co_await async_write_fixed(io, fd, *lease, len));
// lease returns its slot to the pool at scope exit (RAII)
```

## Building

```bash
cmake --preset dev          # configure (Debug, all features)
cmake --build build -j      # build
make check                  # ASan+UBSan and TSan — what CI runs before a merge
make t T=baton              # run tests matching a pattern in the dev build
```

A pinned toolchain container is available for non-Linux hosts:

```bash
make docker-shell           # clang-20 container, repo mounted at /src
make docker-check           # sanitizer pair inside the container
```

Details, cross-toolchain notes, and dependency setup: `docs/build.md`.

## Testing

GoogleTest suites per component: ready ring, run loop, Task/TRY, Baton +
Semaphore, buffer pool, timeout/cancel classification. Sanitizers are the
merge gate — `make check` — and the suite doubles as executable
documentation of the invariants (death tests pin every CHECK).

## Benchmarks

`benchmarks/cpp/` carries kio against asio, seastar, and photon baselines
(HTTP and disk). Never benchmark a sanitizer build; use the release preset.
Methodology notes and the share-nothing measurements live in
`design-notes/sharding.md`.

## Documentation

| Path | What it is |
|---|---|
| `design-notes/ready-ring.md` | The ready list and the epoch-capped drain loop |
| `design-notes/baton.md` | Baton/Semaphore vs folly's multi-threaded versions |
| `design-notes/buffer-pool.md` | Pool invariants, THP layout, reserved-wake protocol |
| `design-notes/sharding.md` | The share-nothing stance — what the ban costs and buys |
| `docs/build.md` | Toolchains, presets, container workflow |
| `study/README.md` | Guided reading curriculum for the codebase (C++ + systems) |

## Contributing

kio is built for high-performance networking and storage engines (bitcask /
LSM / B+-tree on shard-local state — see `design-notes/sharding.md` §3).
Every invariant a comment claims is worth a test that pins it; CHECK (not
assert) for invariant violations, errno-in-`Result` for expected failures.

---
**Author**: ynachi
**License**: MIT
