# Golden Rules for Async C++ (io_uring + async_simple)

## 1. The Lifetime Rule (The "Fire-and-Forget" Trap)

**Rule:** Any coroutine launched as a "root" task (via `start()`, `schedule()`, or `detach`) must own its arguments.

* **NEVER** pass references (`const std::string&`, `int&`) to local variables of the caller.
* **ALWAYS** pass by Value (copy/move) or use `std::shared_ptr`.

**Why:** The caller function (e.g., `acceptLoop`) continues running and may exit (or loop) immediately after launching
the coroutine. Any reference to the caller's stack becomes a dangling pointer instantly.

### Bad

```cpp
// Caller passes a reference to a local variable
Lazy<void> handle(const std::string& data) { ... }
executor->schedule([&]() { handle(local_str).start(...); }); // CRASH
```

### Good

```cpp
// Caller passes ownership (copy or move)
Lazy<void> handle(std::string data) { ... }
executor->schedule([data = std::move(local_str)]() mutable {
    handle(std::move(data)).start(...);
});
```

## 2. The Lambda Capture Rule

**Rule:** Lambdas passed to `executor->schedule()` are temporary objects. They are destroyed immediately after the
coroutine suspends for the first time, not when it finishes.

* Capture by Value (`[=]`) or explicit move.
* Avoid Capture by Reference (`[&]`) unless referring to global constants or objects guaranteed to outlive the entire
  application (like the `Executor` itself).

## 3. The I/O Buffer Rule

**Rule:** Any buffer passed to `io_uring` (via `net::recv`, `io::read`, etc.) must remain valid until the `co_await`
returns.

* **Safe:** Local variables inside the coroutine itself. (Because coroutine frames are allocated on the heap and persist
  while suspended).
* **Safe:** Heap-allocated buffers (`std::vector`, `std::unique_ptr`) owned by the coroutine.
* **Unsafe:** Pointers to buffers on the caller's stack.

### Example

```cpp
Lazy<void> myTask() {
    char buf[1024]; // Safe: Lives in coroutine frame
    co_await net::recv(..., buf, ...);
}
```

## 4. The "Sticky" Thread Rule (Performance)

**Rule:** Do not manually reschedule tasks unless you explicitly want to load-balance.

**Concept:** Once a task starts on a thread (e.g., via `schedule` in `acceptLoop`), the `io_uring` integration ensures
all subsequent I/O in that task "sticks" to that same thread.

**Benefit:** Zero lock contention, maximum CPU cache locality.

**Pattern:** Use `executor->schedule()` only for new independent tasks (like a new client connection). Within a
connection handling loop, just use `co_await`.

## 5. Helper Pattern for Safe Scheduling

To avoid mistakes, you can define a helper macro or template that enforces move semantics when scheduling.

```cpp
template <typename Func, typename... Args>
void spawn_task(IoUringExecutor* exec, Func&& func, Args&&... args) {
    // Capture arguments by value/move into the lambda
    exec->schedule([func = std::forward<Func>(func), 
                   ...args = std::forward<Args>(args)]() mutable {
        // Start the coroutine
        func(std::move(args)...).start([](auto&&){});
    });
}

// Usage:
// spawn_task(executor, handleClient, client_fd, std::move(addr_str));
```
