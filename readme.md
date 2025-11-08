# kio: A C++23 io_uring Library for High-Performance I/O

`kio` is a modern C++23 library for building ultra-fast, scalable network and file I/O applications on Linux.
It is built on the following principles:
- **Asynchronous I/O**: `kio` uses the Linux `io_uring` kernel feature to provide a high-performance, asynchronous I/O API.
- **Share-Nothing Architecture**: `kio` uses a thread-per-core architecture to eliminate lock contention on the hot path,
- **C++ 20 stackless coroutines**: `kio` uses C++ 20 stackless coroutines to provide a familiar, easy-to-use API. 
The asynchronous code looks like synchronous code and is easy to reason about.

```textmate
User App                    kio Library                          Linux Kernel
┌─────────┐    ┌──────────────────────────────────────┐        ┌────────────┐
│ async   │───▶│ IOPool                               │───────▶│ io_uring   │
│ /await  │    │  ├─ Worker 0 (Core 0, io_uring)      │ SQE    │            │
│  code   │    │  ├─ Worker 1 (Core 1, io_uring)      │─────── │  Async I/O │
│         │◀───│  └─ Worker N (Core N, io_uring)      │◀───────│            │
└─────────┘    │     Share-Nothing Architecture       │  CQE   └────────────┘
               └──────────────────────────────────────┘

Task<T>         : Lazy coroutines which starts only when awaited.
DetachedTask<T> : Fires and forgets a coroutines, for background tasks. They can also contain Lazy coroutines.
```

`kio` is a specialized library for **Linux only** systems and requires a Kernel version from 6.0. 
We tried to only implement a minimal set of features to provide a high-performance, asynchronous I/O API.  
Kio is good for building high-performance, scalable network and file I/O applications.  

Internally, `kio` uses the `liburing` library to provide the `io_uring` kernel feature. You can find below the main 
abstractions exposed by `kio` to the end user. 

## IO Worker
A worker is a self-contained IO loop that can be used to perform asynchronous I/O operations. Each worker internally   
uses a dedicated io_uring instance. Workers are single-threaded and share-nothing. So within a worker, all the I/O 
operations are performed in a single thread. That's why we don't need to worry about lock contention. Scaling is 
done by creating multiple workers. Each IO operation MUST be issued from the worker thread. Submitting an I/O operation 
from a different thread will result in data races and undefined behaviors. We provide the `SwitchToWorker` mechanism to help you to 
avoid this issue. We provide some demos to show how to use the `SwitchToWorker` mechanism.  A worker can be initialized 
with a callback coroutine which will be called upon start. Its useful for cases like network servers where your entry point 
is a single, well-defined function.  I heard you cry when you saw the word `callback`. Rest assured, this is not a 
callback asynchronous model. Keep reading.  
You can refer to the [io-trace](./docs/io_traces.md) document to see the flow of an I/O operation within a worker.  
Look at the following demos to see some examples of how to use `Worker`.  
- [Worker with callback](./demo/tcp_worker_callback.cpp)
- [Worker without callback, use SwitchToWorker](./demo/tcp_worker_no_callback_v2.cpp)

## IOPool
An `IOPool` is a collection of workers. It is used to manage the lifecycle of the workers. It is also used to 
distribute the I/O operations across the workers. The number of workers is fixed. Each worker is pined to a CPU core. 
You can provide a coroutine that would run on each worker or implement some load distribution logic. 
- [IOPool demo](./demo/tcp.cpp)

## Task
A `Task` is a coroutine that can be awaited. It is a lazy coroutine. It is not started until it is awaited.
```c++
TEST(SyncWaitTest, NestedTasks)
{
   // inner_task not started yet.
    auto inner_task = []() -> Task<int> {
        co_return 10;
    };

    auto outer_task = [&inner_task]() -> Task<int> {
       // inner_task is started here when the parent task is awaited.
        const int value = co_await inner_task();
        co_return value * 2;
    };

    const int result = SyncWait(outer_task());
    EXPECT_EQ(result, 20);
}
```

## DetachedTask
A `DetachedTask` is a coroutine that can be fired and forgotten. Its eagerly started. It can contain lazy coroutines. 
A `DetachedTask` is useful for background tasks. It does not need to be awaited and does not return a value.   
See [Worker without callback, use SwitchToWorker](./demo/tcp_worker_no_callback_v2.cpp) for an example of how to use 
`DetachedTask`.  

## SwitchToWorker
A `SwitchToWorker` is a coroutine that can be used to switch to another worker.  As stated above, all the I/O operations 
MUST be issued from the worker thread. Submitting an I/O operation from a different thread will result in data races and 
undefined behaviors. `SwitchToWorker` post any io operation that happens after it to run on the specified worker.  
See [Worker without callback, use SwitchToWorker](./demo/tcp_worker_no_callback_v2.cpp) for an example of how to use
`SwitchToWorker`.  

## How to build Kio

See [build.md](./docs/build.md) for instructions on how to build Kio.

## Benchmarks against Tokio

see [benchmarks](./docs/benchmarks.md) for benchmarks against Tokio.

