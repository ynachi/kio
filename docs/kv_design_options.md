# KV Design Options for kio Workers

This document compares modern KV storage designs and how each maps to the kio per-core, share-nothing worker model. It also highlights segment-based replication patterns and a concrete recommendation.

## 1. WiscKey-Style (LSM Keys + Value Log)

**Core idea**
- Keep keys/metadata in an LSM tree.
- Store large values in an append-only value log (segmented).

**kio mapping**
- One LSM + one value log per worker.
- Routing by hash to a worker preserves share-nothing.

**Segments**
- Value log segments are the natural replication unit.
- SST files from the LSM are also segment-like units.

**Replication**
- Replicate sealed value log segments.
- Replicate SSTs (or WAL) to keep index consistent.

**Pros**
- Lower write amplification for large values.
- Value log segments fit handoff and replication well.

**Cons**
- Two systems to keep consistent (LSM + value log).
- Requires garbage collection of old values.

## 2. FASTER-Style (Append Log + In-Memory Index)

**Core idea**
- Append-only log for all writes.
- In-memory hash index points to log offsets.
- Periodic compaction to remove stale records.

**kio mapping**
- Each worker owns its own log and hash index.
- Fits the current BitKV design closely.

**Segments**
- Log segments are sealed and replicated as units.
- Index can be rebuilt by replaying segments.

**Replication**
- Replicate sealed segments and a compacted checkpoint.
- Followers rebuild index from segment stream.

**Pros**
- Very close to BitKV today; minimal redesign.
- High throughput and simple replication.

**Cons**
- Keydir must fit in RAM.
- Compaction needed to reclaim space.

## 3. Partitioned LSM (Per-Worker LSM)

**Core idea**
- Each worker runs a full LSM tree.
- Compaction is local, no cross-worker coordination.

**kio mapping**
- Hash routing to workers; each worker owns its own LSM.

**Segments**
- SST files are the replication unit.
- WAL segments can be used for fast catch-up.

**Replication**
- Replicate WAL + SSTs or only SSTs with periodic checkpoints.

**Pros**
- Better for range scans and mixed workloads.
- LSM reduces memory footprint of large keyspaces.

**Cons**
- More complex compaction scheduling.
- Higher write amplification than log-only systems.

## 4. Bw-Tree / Latch-Free B-Tree Variants

**Core idea**
- In-memory delta chains per page.
- Background consolidation and logging.

**kio mapping**
- Per-worker tree and log to keep isolation.

**Segments**
- Log segments are replicated.
- Checkpoints represent tree snapshots.

**Replication**
- Replicate logs and occasional checkpoints.

**Pros**
- Good for read-heavy workloads with frequent updates.

**Cons**
- Much more complex than log or LSM designs.
- Harder to combine with value log unless split.

## 5. Learned Index + Log (Experimental)

**Core idea**
- Use a model to predict key positions.
- Store data in sorted segments or a log.

**kio mapping**
- Per-worker model and segment sets.

**Segments**
- Sorted segments act as replication units.

**Replication**
- Replicate segments; rebuild model on followers.

**Pros**
- Potentially better cache behavior for skewed keys.

**Cons**
- High R&D risk; not production-proven for general KV.

## 6. Segment-Centric Replication Pattern

This pattern applies to all the options above.

**Unit**
- Fixed-size segments (64-256MB).

**Lifecycle**
- Append -> seal -> replicate -> compact/merge -> delete.

**Benefits**
- Handoff and replication are streaming-friendly.
- Recovery can replay segments or ship compacted segments.

## 7. Recommendation

Given your current BitKV code and share-nothing workers, the best path is a **FASTER-style log store** with segment-level replication.

Why:
- Minimal changes to your existing Bitcask design.
- Natural fit for per-worker isolation.
- Segments map cleanly to replication and handoff.

If you expect very large values or memory pressure, a **WiscKey-style split** (LSM for keys + value log) is a strong second option.

## 8. Decision Table

Use this to pick a design based on workload and constraints.

| Workload / Constraint | Best Fit |
| --- | --- |
| Write-heavy, point lookups, fits in RAM | FASTER-style log |
| Very large values, want lower write amp | WiscKey-style |
| Needs range scans, mixed reads/writes | Partitioned LSM |
| Heavy update rate, complex read patterns | Bw-tree (advanced) |
| Experimental, skewed hot keys | Learned index |

## 9. Mapping to Current Codebase

This maps each option to likely changes in the current BitKV code structure.

### 9.1 FASTER-Style (Closest to BitKV)

- Keep `Partition` and `DataFile` logic.
- Add segment metadata and a segment manifest per partition.
- Add checkpointing for KeyDir (periodic snapshot or log replay).
- Replication ships sealed segment files and optional checkpoints.

### 9.2 WiscKey-Style

- Replace KeyDir with an LSM for keys and value pointers.
- Keep `DataFile` as a value log (segment it explicitly).
- Add value-log garbage collection (rewrite live values).
- Replication ships value-log segments + LSM SSTs or WAL.

### 9.3 Partitioned LSM

- Replace Bitcask log + KeyDir with a per-partition LSM tree.
- Add WAL + memtable flush + SST compaction per worker.
- Replication ships SSTs (or WAL for near-real-time).

### 9.4 Bw-Tree

- Add per-worker tree with delta chains + logging.
- Add checkpointing and page consolidation threads.
- Replication ships log segments + periodic tree checkpoints.

### 9.5 Learned Index

- Add per-worker model and segment builder.
- Store data in sorted segments with periodic rebuild.
- Replication ships segments; model can be rebuilt.

## 10. FASTER-Style Deep Dive

FASTER (Fast and Scalable Threaded/Concurrent Key-Value Store) is a log-structured KV design that combines an append-only log with a hash index in memory. The key idea is that every update appends to a log; the index points to the latest record. Compaction or log truncation removes stale data over time.

Key components:
- **Hybrid log**: append-only record log, segmented by size. New writes go to the tail.
- **In-memory hash index**: maps key -> log address. This is the KeyDir in BitKV terms.
- **Read path**: lookup in index, then one read from log at the recorded offset.
- **Write path**: append to log, update index to point to new address.
- **Compaction/checkpoint**: periodically scan older segments, copy live records to new segments, then retire old segments.

Replication-friendly structure:
- Sealed segments are immutable and can be shipped to followers.
- Followers rebuild their in-memory index by replaying segments or applying a checkpoint.

Relevant references:
- FASTER project: https://www.microsoft.com/en-us/research/project/faster/
- FASTER paper (overview): https://www.microsoft.com/en-us/research/publication/faster-a-concurrent-key-value-store-with-in-place-updates/

Notes for kio alignment:
- Your `Partition` already behaves like a FASTER hybrid log, with KeyDir as the in-memory index.
- Make segments explicit and add a segment manifest so replication can ship sealed segments.
- Add checkpointing of KeyDir to avoid full log replay on startup.

## 11. Minimal Prototype Plan (FASTER-Style)

This is the smallest path to a working prototype with segment replication.

1) **Segment definition**
   - Choose a segment size (e.g., 128MB).
   - When active file reaches segment size, seal it.
   - Track sealed segments in a manifest (per worker).

2) **Segment manifest**
   - Add a manifest file per worker recording: segment_id, file path, min/max offsets.
   - Write manifest updates atomically (tmp + rename).

3) **Checkpoint KeyDir**
   - Periodically serialize KeyDir to a checkpoint file.
   - On startup, load checkpoint then replay only segments newer than checkpoint.

4) **Replication v0**
   - Add a simple segment shipper: copy sealed segments to a peer.
   - Peer loads segment, rebuilds index from it.

5) **Compaction v0**
   - Scan oldest sealed segments, copy live records to a new segment.
   - Update KeyDir to new offsets and delete old segments.

## 12. Suggested Defaults

These values are safe starting points for a prototype and can be tuned later.

- Segment size: 128MB
- Checkpoint interval: 30-120 seconds
- Compaction trigger: >50% dead bytes in a segment
- Max open segments per worker: 16
- Replication batch size: 4-16MB per stream chunk

## 13. Minimal Manifest Sketch

Segment manifest (per worker) with append-only records:

```
{segment_id, path, min_offset, max_offset, sealed, created_ts}
```

Checkpoint manifest:

```
{checkpoint_id, path, segment_id, created_ts}
```

Both manifests should be updated atomically using a temp file + rename.

## 14. Next Steps

- Decide on segment size and sealing policy.
- Define a segment manifest and replication log format.
- Add a checkpoint mechanism to rebuild the in-memory index.
