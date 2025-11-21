```text
bitcask/
├── include/
│   └── bitcask/
│       ├── bitcask.h              # Main public API
│       ├── entry.h                # Entry format (CRC, timestamp, key, value)
│       ├── datafile.h             # Data file management
│       ├── hint_file.h            # Hint file for faster startup
│       ├── keydir.h               # In-memory hash index
│       ├── compaction.h           # Merge/compaction logic
│       ├── config.h               # Configuration options
│       └── errors.h               # Error types
├── src/
│   ├── bitcask.cpp
│   ├── entry.cpp
│   ├── datafile.cpp
│   ├── hint_file.cpp
│   ├── keydir.cpp
│   └── compaction.cpp
├── tests/
│   ├── bitcask_test.cpp
│   ├── entry_test.cpp
│   ├── compaction_test.cpp
│   └── benchmark.cpp
└── examples/
    └── basic_usage.cpp
```

Main design 1.

```mermaid
sequenceDiagram
    autonumber
    participant U as User Thread
    participant K as Shared KeyDir
    participant W as Writer Worker (Core 0)
    participant R as Reader Worker (Core 1)
    participant D as Disk (io_uring)
    Note over U, D: SCENARIO 1: WRITE (PUT)
    U ->> U: db.put("key", "val")
    U ->> W: SwitchToWorker(Writer)
    Note right of U: Context Switch 1

    rect rgb(240, 240, 240)
        Note right of W: Now on Writer Thread
        W ->> D: async_write(active_file)
        D -->> W: Result(offset=100)
        W ->> K: Lock Shard -> Update Index
        Note right of K: "key" -> {file: 1, offset: 100}
    end
    W -->> U: return void
    Note left of W: Context Switch 2 (Resume User)
    Note over U, D: SCENARIO 2: READ (GET)
    U ->> U: db.get("key")

    rect rgb(230, 240, 255)
        Note right of U: Step 1: Index Lookup (Local)
        U ->> K: Shared Lock Shard -> Get Location
        K -->> U: {file_id: 5, offset: 2000}
    end

    Note right of U: Step 2: Routing
    U ->> U: Find DataFile(5)
    Note right of U: DataFile 5 is bound to Reader Worker
    U ->> R: SwitchToWorker(Reader)
    Note right of U: Context Switch 3

    rect rgb(240, 240, 240)
        Note right of R: Now on Reader Thread
        R ->> D: async_read(file_5, offset=2000)
        D -->> R: Result("value")
    end

    R -->> U: return "value"
    Note left of R: Context Switch 4 (Resume User)
```

The Step-by-Step Analysis

Yes, SwitchToWorker is absolutely involved, and it is the primary mechanism for thread safety in this design.

1. The Write Path (put)

The active_file (the mutable file we are appending to) is owned by the Writer Worker. To write to it safely without
mutexes on the file descriptor, we must be on that thread.

Main Thread: Calls db.put(key, value).

Main Thread: Checks active_file_->worker(). It sees it is the Writer Worker.

Transition: Calls co_await SwitchToWorker(writer_worker). The Main Thread coroutine suspends.

Writer Worker: Resumes the coroutine.

Writer Worker: Calls active_file_->async_write(). This submits an SQE to the Writer's io_uring.

Writer Worker: Updates KeyDir. Since KeyDir is shared, it takes a shard-granular lock.

Optimistic: This is fast.

Pessimistic: If a reader holds this shard's lock, the Writer Worker stalls here.

Writer Worker: co_return.

Transition: Control returns to the Main Thread (or stays on Writer depending on your Task implementation).

2. The Read Path (get)

This is where the "Granular Locking" shines. We want to avoid sending the request to a worker if we don't know which
file contains the data yet.

Main Thread: Calls db.get(key).

Main Thread (Local lookup): It queries KeyDir immediately.

It takes a shared_lock on the specific shard for key.

It retrieves {file_id, offset, size}.

Benefit: We haven't context-switched yet! If the key doesn't exist, we return NotFound instantly. Cost is near zero.

Main Thread (Routing): It looks up file_id in the older_files_ map to find the DataFile object.

Let's say DataFile 100 was assigned to Reader Worker 1 when it was opened/rotated.

Transition: The DataFile's async_read method internally calls co_await SwitchToWorker(this->worker_).

Reader Worker 1: Resumes execution.

Reader Worker 1: Submits pread to its io_uring.

Reader Worker 1: Returns data.

Summary of "SwitchToWorker" Logic

Write: Always switches to the single Writer Worker.

Read (Index): Never switches. Runs on the calling thread (using locks).

Read (IO): Always switches to the specific Worker that owns that file.

Why this is good (The "Happy Path")

This design minimizes "blind" context switches. You only switch threads when you know you need to perform I/O on a
specific ring. You don't switch threads just to look up a hash map.

Main design 2:

```mermaid
sequenceDiagram
    autonumber
    participant U as User Thread (e.g., HTTP Handler)
    participant DB as Database (Router)
    participant W1 as Worker 1 (Shard 1 Owner)
    participant K1 as KeyDir (Shard 1 Local)
    participant D1 as DataFile (Shard 1 Local)
    Note over U, D1: User calls db.put("user:123", "value")
    U ->> DB: get_target_shard("user:123")
    DB ->> DB: hash("user:123") % 4 = 1
    Note right of U: Target is Shard 1. Am I on Worker 1? No.
    U ->> W1: SwitchToWorker(Worker 1)
    Note right of U: CONTEXT SWITCH (Task moves to Core 1)

    rect rgb(240, 255, 240)
        Note right of W1: Now executing on Core 1
        W1 ->> D1: async_write(entry)
        Note right of D1: No locks needed (Single Writer)
        D1 -->> W1: offset = 500
        W1 ->> K1: index.put("user:123", 500)
        Note right of K1: No locks needed (Thread Local)
    end

    W1 -->> U: return void
    Note left of W1: CONTEXT SWITCH (Result moves back to User Thread)
```