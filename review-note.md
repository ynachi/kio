# Review note: a useful tiny I/O core

The core already has the essential shape: lazy tasks, cross-thread submission,
file and socket I/O, and a defined shutdown lifecycle. The next improvements
should make those facilities dependable, not add a larger framework.

## Priorities

1. **Surface root-task failures.** `IO::schedule(Task<void>)` owns a root task,
   but its `Result<void>` is discarded when the frame is destroyed. Add a small
   completion/error callback so applications can observe failures without a
   task-group or join-handle API.

2. **Make socket writes safe and complete.** `write()` can complete partially;
   the HTTP example currently sends a response with one write. Add a small
   `write_all` helper that loops until all bytes are written or an error occurs.
   Also expose a socket `send` operation with flags, including `MSG_NOSIGNAL`,
   so callers can avoid SIGPIPE when a peer has closed the connection. See
   [Linux `send(2)`](https://man7.org/linux/man-pages/man2/sendmsg.2.html).

3. **Report worker failures.** `IoContext` runs `IO::run()` in worker threads
   without catching exceptions. A ring or activation failure can therefore
   terminate the process. Capture worker failures and provide a way for the
   caller to observe them after joining, while keeping destructor cleanup safe.

## Later, if workloads need them

- **Per-operation deadlines.** Useful for network services, but avoid adding a
  general-purpose task-group or race framework just to support timeouts.
- **Admission/backpressure.** Submission is currently unbounded; document that
  producers must throttle, or add a small queue limit and a distinct full-queue
  result if real workloads need it.

Keep DNS, HTTP, TLS, a general scheduler, and task groups outside this core.
A custom coroutine-frame allocator is an optimization to justify with metrics,
not a prerequisite for a useful I/O API.

## Optional experiment: build messages in fixed buffers

SereneDB's useful idea here is a transactional message writer: reserve space,
serialize directly into it, then commit the complete message. If encoding fails,
discard the uncommitted bytes. Kio could try this **above** the I/O core using
its existing `FixedBuffer` leases. It would remove an application-side copy when
the serializer currently builds a separate string or vector first. The builder
should own a bounded chain of leases; a message may span several slots, while
each reservation is contiguous and no larger than one slot.

An illustrative interface (not an implemented API):

```cpp
struct BufferChunk {
    FixedBuffer storage;  // owns one registered slot
    size_t used = 0;
};

struct MessageChain {
    std::vector<BufferChunk> chunks; // transport retains until safe to reuse
};

class MessageBuilder {
public:
    Result<std::span<std::byte>> reserve(size_t contiguous_bytes);
    void advance(size_t bytes_written);  // must fit the last reservation
    MessageChain commit() &&;             // transfers the leases as one message
    ~MessageBuilder();                    // returns leases if never committed
};
```

For a frame that fits in one slot, usage could look like this:

```cpp
MessageBuilder builder{io, max_queued_bytes};
auto space = builder.reserve(encoded_size);
if (!space) co_return std::unexpected(space.error());
const size_t written = encode_response(*space); // encode in the fixed buffer
builder.advance(written);
auto message = std::move(builder).commit();      // no intermediate payload copy
co_return co_await send_all(io, socket, std::move(message));
```

`send_all` is a separate, optional transport helper: it must handle partial
sends, keep every chunk alive until the kernel is finished with it, and apply a
per-connection backlog limit. Plain `write_fixed` does **not** imply zero-copy
network transmission; registration mainly avoids repeated buffer mapping.
For a measured large-send workload, the helper could select
`io_uring_prep_send_zc_fixed` where supported, and fall back to an ordinary
send when unsupported or unprofitable. Zero-copy is still a kernel hint, not an
end-to-end guarantee. See [registered buffers](https://man7.org/linux/man-pages/man7/io_uring_registered_buffers.7.html),
[zero-copy send](https://man7.org/linux/man-pages/man3/io_uring_prep_send_zc.3.html),
and the [kernel's performance caveats](https://www.kernel.org/doc/html/latest/networking/msg_zerocopy.html).

Crucially, `send_zc` changes the operation lifecycle. The first CQE reports
the send result (which may be short); if it has `IORING_CQE_F_MORE`, a later
`IORING_CQE_F_NOTIF` CQE marks the buffer reusable. Its state must outlive the
sender coroutine if necessary. Kio's current `IO::tick()` untracks and resumes
on the first CQE, so simply adding an SQE wrapper would risk returning a buffer
too early and dereferencing an expired operation when the notification arrives.
The completion dispatcher, cancellation, and shutdown drain need explicit
two-CQE handling before this path is safe. Start with direct serialization and
ordinary sends; add `send_zc` only after profiling realistic message sizes.
