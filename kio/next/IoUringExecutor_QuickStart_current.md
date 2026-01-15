# IoUringExecutor + async_simple (quick start)

## 1) Create an executor

```cpp
#include "io_uring_executor.h"

kio::next::v1::IoUringExecutorConfig cfg;
cfg.num_threads = 4;
cfg.io_uring_entries = 32768;
cfg.control_ring_entries = 2048;
cfg.immediate_submit = false;  // batch submits (often higher throughput)

kio::next::v1::IoUringExecutor exec(cfg);
```

## 2) Write a Lazy coroutine that uses io_uring awaiters

```cpp
#include <async_simple/coro/Lazy.h>
#include "io_awaiters.h"

using async_simple::coro::Lazy;
namespace io = kio::io::next;

Lazy<ssize_t> echo_once(kio::next::v1::IoUringExecutor* exec, int fd) {
    char buf[4096];

    // read from socket
    auto n = co_await io::recv(exec, fd, buf, sizeof(buf));
    if (n <= 0) co_return n;

    // write back
    co_await io::send(exec, fd, buf, static_cast<size_t>(n));
    co_return n;
}
```

### Controlling which worker lane handles the I/O
```cpp
auto n = co_await io::recv(exec, fd, buf, sizeof(buf))
    .on_context(lane_id);
```

### Doing I/O on lane B but resuming on lane A
```cpp
auto home = exec->checkout();
auto n = co_await io::read(exec, fd, buf, len, off)
    .on_context(storage_lane)
    .resume_on(home);
```

## 3) Start work (choose one)

### Option A: Run on the executor from the beginning (recommended)
Use `.via(&exec)` so the root starts on a worker thread.

```cpp
#include <async_simple/coro/SyncAwait.h>

auto result = async_simple::coro::syncAwait(echo_once(&exec, fd).via(&exec));
```

Or non-blocking:

```cpp
echo_once(&exec, fd).via(&exec).start([](async_simple::Try<ssize_t> t) {
    if (t.hasError()) {
        // handle error
    } else {
        // t.value()
    }
});
```

### Option B: Bind an executor without scheduling immediately
`directlyStart(cb, exec)` binds the executor but the coroutine begins on the caller thread and only moves onto the executor when it hits a scheduling point. (This is async_simple’s documented behavior.)

```cpp
echo_once(&exec, fd).directlyStart([](async_simple::Try<ssize_t>) {}, &exec);
```

### Option C: Plain `.start()` (no executor binding)
This runs inline on the caller thread until the coroutine suspends.
```cpp
echo_once(&exec, fd).start([](async_simple::Try<ssize_t>) {});
```

## 4) Structured concurrency (`collectAny`, `collectAll`)
From async_simple docs, `collectAny` returns when the first finishes; `collectAll` waits for all. You usually want these tasks to run on an executor (either by calling them inside a coroutine already running on the executor, or using `.via(&exec)`).

```cpp
#include <async_simple/coro/Collect.h>
#include <async_simple/Signal.h>

using async_simple::coro::collectAny;
using async_simple::coro::collectAll;
using async_simple::SignalType;

// Example: timeout pattern (cancel others when one finishes)
auto res = co_await collectAny<SignalType::terminate>(
    task().via(&exec),
    async_simple::coro::Sleep(1s)   // needs a bound/current executor
);

// Example: wait all
auto [a, b] = co_await collectAll(taskA().via(&exec), taskB().via(&exec));
```

## 5) Shutdown
```cpp
exec.stop();
exec.join();
```
