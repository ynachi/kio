# Getting Started

This guide shows how to build and run the executor and sample code, and how
to use cancellation with async_simple.

## Requirements

- Linux with io_uring support
- liburing headers and library
- C++20 compiler

## Build Example

Build the optimized HTTP demo:

```sh
c++ -std=c++20 -O2 -pthread tcp_demo_v1_optimized.cpp io_uring_executor.cpp -luring -I./async_simple -o tcp_demo_v1_optimized
```

Run:

```sh
./tcp_demo_v1_optimized
```

## Basic Usage

Use the executor and I/O helpers:

```cpp
IoUringExecutor executor;
int fd = /* ... */;
char buf[4096];

auto n = co_await kio::io::next::recv(&executor, fd, buf, sizeof(buf));
```

Force submission on a specific context and resume on another:

```cpp
auto home = executor.checkout();
auto n = co_await kio::io::next::read(&executor, fd, buf, sizeof(buf))
    .on_context(2)
    .resume_on(home);
```

## Cancellation With async_simple

To allow cancellation, pass the `Slot*` from `CurrentSlot` into the I/O awaiter:

```cpp
Lazy<void> task(IoUringExecutor* executor, int fd) {
    auto slot = co_await async_simple::coro::CurrentSlot{};
    char buf[1024];
    co_await kio::io::next::recv(executor, fd, buf, sizeof(buf), 0, slot);
}
```

Attach a cancellation signal to a coroutine:

```cpp
auto signal = async_simple::Signal::create();
task(&executor, fd)
    .setLazyLocal(signal.get())
    .via(&executor)
    .start([](auto&&){});

signal->emits(async_simple::Terminate);
```

When canceled, awaiters throw `async_simple::SignalException` on resume.

## Shutdown Behavior

- `stop()` requests all worker threads to stop and drains pending I/O.
- If you want `stop()` to actively cancel in-flight I/O, set:

```cpp
IoUringExecutorConfig cfg;
cfg.cancel_on_stop = true;
IoUringExecutor executor(cfg);
```

## Stress Test

Build and run the cancellation + shutdown stress test:

```sh
c++ -std=c++20 -O2 -pthread cancel_shutdown_stress.cpp io_uring_executor.cpp -luring -I./async_simple -o cancel_shutdown_stress
./cancel_shutdown_stress
```

