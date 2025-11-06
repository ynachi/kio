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