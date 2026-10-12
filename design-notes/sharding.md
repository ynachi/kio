# The share-nothing discipline: what kio builds, and the cross-thread ban

Status: LIVING DOCUMENT — captured 2026-10-12 after the operations-surface
port; to be updated as decisions land (open questions at the bottom).
Related: `ready-ring.md` (the loop), `buffer-pool.md` (the pager it implies).

## 0. The rule

kio v2 is **thread-per-core, share-nothing, no cross-thread data plane**:

1. **One Context per thread**, one ring per Context (`SINGLE_ISSUER` +
   `DEFER_TASKRUN`). Nothing is shared between Contexts except (optionally)
   the kernel workqueue (`IORING_SETUP_ATTACH_WQ`).
2. **A session's sockets are created by the session's Context.** A proxy
   handler opens its upstream on its own ring, so both connections of a
   session live on one thread — no handoff.
3. **Shared state is sharded by key or replicated per thread.** There is no
   cross-thread mutable memory.
4. **Cross-thread traffic, if it ever exists, is control-plane**: over
   kernel sockets (see §4) or via files — never shared memory, never
   coroutine handles crossing threads.
5. `spawn()` from a non-loop thread stays banned (contract today; a
   debug-mode thread-identity CHECK like v1's `owner_thread_` is the
   enforcement path if we ever want it enforced rather than promised).

## 1. Why the ban is right for networking (measured, not theorized)

Kernel load balancing does the distribution: `SO_REUSEPORT` hashes the
4-tuple to one listener socket, one listener per thread (spread verified on
this kernel: 150 conns → [34, 44, 38, 34] over 4 listeners).

```
flow (4-tuple) ──hash──► listener socket ──► thread ──► coroutine owns conn for life
                              │
all requests on that conn ────┘   (HTTP/1.1 keep-alive, HTTP/2, websockets)
```

Two properties make that sufficient: **per-flow affinity** (a connection's
whole life, and all its state, stays in one coroutine frame on one core) and
**stateless handling** (no cross-connection state for the LB to strand).

The payoff is our own measurement: the v2 share-nothing loop does ~143k
req/s/core vs v1's ~99k — the cross-thread machinery (MPSC CoroQueue,
heartbeat-bounded park, TransferTo migration) made every core slower. v1's
`I3` header comment (commit `48f539d`) is the post-mortem of that machinery;
git history is the reference implementation if cross-thread ever returns.

## 2. The two kinds of parallelism — what the ban costs and what it doesn't

```
one shard = one context = one ring:

  I/O parallelism  — N preads as N SQEs, await them all.  ✓ KEPT
                     (io_uring depth is not thread count)
  CPU parallelism  — decode, compare, merge, index build.  ✗ LOST per shard;
                     compensate with N shards (one per core).
```

Most "storage needs threads" intuitions are about I/O parallelism, and
io_uring already provides that on a single thread. The real losses are in
§5.

## 3. Storage engines under the ban — all three buildable

Each has an industrial existence proof on exactly this discipline.

### Bitcask — yes (v1 already did)

One active segment per engine, a long-lived fixed-buffer write lease (the
`write_buffer_` pattern — the reason lease-held-across-ops survived the
pool redesign), `read_fixed` for value-by-offset lookups. Shard by
`hash(key)` → N independent bitcasks, one Context each, one directory each.
Merge is another coroutine on the same ring, chunked with yields between
batches. (Riak ran bitcask partitioned like this.)

### LSM — yes (ScyllaDB on seastar is the proof)

The whole ladder is per-shard state:

```
core 0: Context ── memtable → WAL₀ → L0 flush → compaction → shard/00/*.sst
core 1: Context ── ...                                            shard/01/*.sst
...
routing: client hashes key → shard i (N ports, or embedded app sharding)
```

- **Group commit is better on one ring**: many pending writes coalesce into
  one WAL append (io_uring batches it). TigerBeetle saturates a disk with a
  single-threaded commit path.
- **Point reads keep I/O parallelism**: fire preads at multiple candidate
  SSTables as concurrent SQEs, await all. Needs a `when_all` combinator
  (open finding; it must self-guard the one-handle-two-ops case around the
  no-double-post invariant).
- **Compaction is the real CPU cost** — mitigations in order: more shards
  (N compactions on N cores), cooperative chunking between awaits, and only
  if desperate a separate compaction Context whose input/output interface is
  FILES (the filesystem is a legitimate cross-thread bulk-data handoff).

### B+-tree — yes, with the most caveats

Single-writer ownership deletes the hardest part: **latching**. Lock
crabbing, page latches, lock-free reads — all exist to coordinate concurrent
access; one writer thread needs none of it (the LMDB/SQLite single-writer
insight). Splits propagate up the same thread.

Convergence worth noting: **the buffer pool is a pager.** `slot_size =
4096` already satisfies the pool's pow2-divides-1GiB invariant; a
`BufferLease` is a pinned page handle; `async_read_fixed` at a page offset
is `read_page()`. Range scans hold several leases at once — size the pool
for max-cursors × tree-height.

What sharding costs a tree: no natural disjoint unit until the key range is
cut into tablets (one tree per tablet, one thread per tablet — how every
distributed B+-tree works: TiKV regions, Spanner tablets). Tablet
split/merge migrates a key range between threads: rare, control-plane,
moved via files or serialized pages, never shared memory.

## 4. Escape hatches (zero new runtime machinery)

- **Control plane over sockets**: a `socketpair` between Contexts is two
  fds in two rings — wakes come free through the existing loop, no atomics,
  ~1-2 µs per hop. Fine for routing, metrics, shutdown coordination. Too
  slow for a data plane — which is precisely the signal for when real
  cross-thread would be justified.
- **Files as bulk handoff**: compaction input/output, tablet migration,
  snapshot transfer. The page cache is the shared medium.

## 5. What the ban prices in — the honest list

1. **Per-shard CPU serialization** (compaction merge, index build) — offset
   by shard count; bites when one shard's maintenance competes with its own
   foreground.
2. **Cross-shard transactions** — multi-key ACID across shards is a
   distributed-transaction problem regardless of the ban (2PC/Paxos;
   Scylla's LWT exists for it). The ban is not the hard part.
3. **No global buffer pool / LRU / admission control** — N pools, N×
   memory, per-shard eviction. Usually better for cache locality,
   occasionally wasteful.
4. **Skew** — one whale flow or hot key range pins one core (REUSEPORT
   hashes 4-tuples, not user IDs). No rebalance without handoff.

## 6. Architectural fact to keep recorded

v2 is not cross-thread-free merely by preference — it is **unwakeable
cross-thread by construction**: Phase 2 blocks in `submit_and_wait` with no
deadline, and `SINGLE_ISSUER` + `DEFER_TASKRUN` forbid a foreign thread
entering the ring. A cross-thread post would sit until the next unrelated
completion. If cross-thread ever becomes necessary, the known price is: an
eventfd armed in the ring as the wake channel + a mailbox + optional
TransferTo-style migration — all of which existed in v1 and all of whose
pitfalls are documented in v1's `I3` comment.

## 7. Current build inventory against this discipline

| Domain | Fits | Notes |
|---|---|---|
| HTTP/1.1 services | ✓ (example 03) | conn = coroutine |
| Proxies/gateways | ✓ | handler creates upstream on its own ring |
| UDP services | ~ | `recvfrom`/`sendto` wrappers are a mechanical add |
| TLS services | ~ | kTLS path: handshake then kernel — `send`/`recv` work on the ring as-is; userspace async TLS is a project, not an op |
| Bitcask / LSM / B+-tree | ✓ | §3; storage op surface is complete (below) |
| Clients, timers, per-thread rate limiters | ✓ | |

The operations surface now carries the full storage syscall set:
`async_fsync`, `async_fallocate`, `async_ftruncate`, `async_rename`
(atomic manifest swap — the crash-recovery primitive), `async_open/close`,
plus the buffer pool and `when_all` (pending) for concurrent reads.

## Open questions (to update this note with)

- Workload shape for the first engine (write-heavy → LSM; read-mostly point
  lookups → bitcask; range scans → B+-tree)?
- Embedded vs server? Embedded ⇒ the application must shard identically to
  the engine (session-affinity discipline); server ⇒ N listener ports and
  token-aware clients. This fixes the routing design.
- Is the few-heavy-flows skew case in scope, or is the workload
  many-small-connections?
- Enforcement of rule 5: keep as contract, or add the debug-mode
  thread-identity CHECK?
