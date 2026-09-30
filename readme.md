# URing

A C++23 coroutine interface to Linux io_uring. Each `IO` owns one ring, its
scheduled root tasks, their pending operations, and an optional registered-buffer pool.

```cpp
#include "uring/core/io.h"

URing::Task<void> read_file(URing::IO& io, std::stop_source& stop) {
    URING_TRY(auto fd, co_await io.open("input.txt", O_RDONLY));
    std::byte buffer[1024];
    URING_TRY(auto bytes, co_await io.read(fd, buffer));
    // Process buffer[0..bytes).
    stop.request_stop();
    co_return {};
}

int main() {
    URing::IO io(0);
    std::stop_source stop;
    io.schedule(read_file(io, stop));
    io.run(stop.get_token());
}
```

## Ownership and execution

`Task<T>` is lazy and completes with `Result<T>`. An awaited child remains
owned by its parent's Task object. `io.schedule(Task<void>)` transfers an
unstarted root to the reactor and returns false once shutdown begins.
Arguments used after suspension must live in the coroutine frame or outlive
the task; use named coroutine functions or retain capturing lambda closures.

Scheduling is safe from any thread. A root and its children remain on their
assigned worker. I/O against a different reactor returns `EXDEV`. Tasks must
suspend through the reactor's I/O awaiters or awaited children; arbitrary
external awaiters need their own integration with the reactor's lifetime rules.
There is no coroutine migration API.

`IO` is non-copyable and non-movable. Its ring is created disabled, then
activated by the thread that calls `run(stop_token)`. That thread exclusively
submits and processes I/O. `run` is a single lifecycle and cannot restart.
The `sync_wait(io, task)` testing helper drives tasks on its caller's thread;
repeated calls must use that same thread and precede terminal shutdown.

## Shutdown

A stop request wakes the reactor even when idle. Shutdown closes admission,
requests cancellation of pending operations, and processes their original
completions before resuming tasks. An operation that already succeeded retains
its success result; new I/O attempted during shutdown returns `ECANCELED`.
Submitted close requests drain without cancellation so descriptor ownership
is not abandoned after transfer to the kernel.
Task chains must propagate cancellation or otherwise finish. Root frames are
destroyed only after completion, so descriptors and borrowed buffers unwind
through normal C++ destruction. Work submitted before run also remains owned:
destroying an unrun reactor destroys those unstarted frames.

The optional `uring/extention/io_pool.hpp` wrapper owns stable reactor objects
and their worker threads. `pool.worker(i).schedule(task)` submits independent
work; `pool.join()` requests stop and waits for every worker to drain.

## I/O and buffers

The IO methods provide accept/connect, read/write and vectored I/O, filesystem
operations, poll, and sleep. Reads and writes may complete partially.
`Fd` owns a descriptor; socket options live in `uring/net.hpp`.

Configure registered buffers through the IO constructor:

```cpp
URing::IO io(0, nullptr, {}, {{.size = 4096, .count = 128}});
```

Borrow with `io.take_fixed_buffer(size)` inside a task. Use the typed
`read_fixed`/`write_fixed` methods; return buffers on their owning worker.
The reactor must outlive borrowed buffers. Buffer registration is owned by
the reactor and fails construction if the kernel rejects it.

`IoOptions::batch_max_size` bounds coroutine resumptions per tick.
CPU affinity and io_uring setup flags are explicit options.

## Build

The core requires C++23, liburing, and threads. Tests require GoogleTest.
Storage dependencies are discovered only when Bitcask is enabled.
Mimalloc is optional through `KIO_USE_MIMALLOC=ON`.

```sh
cmake -S . -B build -DKIO_BUILD_BITCASK=OFF -DKIO_BUILD_DEMOS=OFF \
    -DKIO_BUILD_BENCHMARK=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

The test suite covers task composition, remote submission, shutdown and resource
release, worker affinity, registered buffers, and the intrusive incoming queue.
