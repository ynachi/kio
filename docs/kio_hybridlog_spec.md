# kio HybridLog Spec (Single-Worker, Share-Nothing)

This spec adapts the FASTER HybridLog concept to kio's per-worker, share-nothing model. It removes the concurrency machinery required for multi-threaded shared-memory access, while keeping the performance benefits of in-place updates for hot data.

## 1. Goals

- Preserve the append-only log as the source of truth.
- Allow in-place updates for hot records in memory.
- Keep the design single-threaded per worker (no lock-free index or epochs).
- Provide clear regions for flushing and eviction.

## 2. Core Concepts

Each worker owns an independent HybridLog.

Logical address space is a circular in-memory buffer plus on-disk segments.

Regions:
- **Mutable region**: newest portion of log; records here can be updated in-place.
- **Read-only region**: older portion still in memory; updates are copy-on-write (append new record).
- **Stable region**: on disk; updates require disk read then append new record.

Offsets:
- `head_offset`: oldest in-memory address (eviction boundary).
- `read_only_offset`: boundary between read-only and mutable in memory.
- `tail_offset`: next append location.

## 3. Data Structures

Per worker:
- `HybridLog` (segments + in-memory buffer)
- `KeyDir` (hash map: key -> {segment_id, offset, size, timestamp})
- `SegmentManifest` (list of sealed segments)

Record format:
- header: {key_len, value_len, timestamp, flags}
- key bytes
- value bytes

Flags:
- tombstone
- invalid (optional, for failed CAS-free append handling)

## 4. Read Path

1) Lookup key in KeyDir.
2) If not found, return not found.
3) If address >= head_offset: read from in-memory buffer.
4) Else: read record from disk segment.

## 5. Update Path (Upsert)

Given key, value:

- If key not found: append new record at tail; update KeyDir.
- If key found at logical address `addr`:
  - If `addr >= read_only_offset`: in-place update (mutable region).
  - Else: append new record at tail; update KeyDir to new address.

## 6. Delete Path

- Write tombstone record at tail.
- Remove key from KeyDir.

## 7. Region Movement and Flushing

- `read_only_offset` lags `tail_offset` by a fixed size (mutable window).
- When `tail_offset` moves into a new page/segment:
  - If a page transitions from mutable -> read-only, it becomes eligible for flush.
- When `head_offset` advances:
  - Pages before head are evicted from memory (must be flushed first).

Because each worker is single-threaded, these transitions can occur safely without epochs.

## 8. Segmenting

- Use fixed-size segments (e.g., 128MB).
- Active segment is writable; sealed segments are immutable.
- Sealed segments are recorded in the manifest for replication.

## 9. Compaction

- Scan sealed segments for live records (KeyDir points to latest).
- Copy live records to a new segment.
- Update KeyDir and delete old segments.

## 10. Recovery

- Load manifest and KeyDir checkpoint if present.
- Replay segments newer than checkpoint to rebuild KeyDir.

## 11. Why No FASTER Concurrency Features

These are intentionally omitted:
- Epoch protection and trigger actions.
- Fuzzy region and safe read-only offset.
- Latch-free hash index with CAS inserts.

Reason: each worker is single-threaded; there is no shared-memory contention.

## 12. Optional Extensions

- Add per-record version/timestamp for LWW conflict resolution.
- Add a WAL if stronger crash consistency is needed.
- Add a read cache log for read-hot workloads.

## 13. Mapping FASTER Mechanisms to kio

This maps key FASTER concurrency mechanisms to the kio share-nothing worker model.

1) **Epoch protection + trigger actions**
   - FASTER: global coordination for safe reclamation, flush, and region transitions.
   - kio: not needed; a single worker can perform transitions directly at safe points.

2) **Latch-free hash index (CAS, tentative bits)**
   - FASTER: required for concurrent inserts/updates by many threads.
   - kio: replace with a normal hash map per worker; no CAS required.

3) **HybridLog fuzzy region + safe read-only offset**
   - FASTER: prevents lost-update anomalies under concurrent updates.
   - kio: not needed in single-threaded worker; no fuzzy region.

4) **CompletePending / pending queues**
   - FASTER: continuations for async IO per thread.
   - kio: use coroutine suspension and resume instead.

5) **In-place updates in mutable region**
   - FASTER: requires careful coordination across threads.
   - kio: safe to update in-place within worker-owned memory.
