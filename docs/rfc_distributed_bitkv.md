# RFC: Distributed BitKV with Vnode-Scoped Replication

Status: Draft

## 1. Overview

This RFC defines a Dynamo-style distributed design for BitKV that preserves the existing share-nothing, per-core worker model. The key principles are:

- Vnode is the unit of replication and ownership.
- Each vnode is stored independently on disk (data and hint files are vnode-scoped).
- Each vnode is assigned to exactly one local worker (share-nothing).
- The cluster ring maps vnodes to replica sets.

This design aligns with Riak's Bitcask backend model while keeping BitKV's local performance model intact.

## 2. Goals

- Keep local share-nothing storage and per-core workers.
- Support Dynamo-style replication and hinted handoff.
- Make rebalancing and repair vnode-granular (not per-key).
- Keep on-disk files aligned with vnode ownership to simplify handoff and compaction.

## 3. Non-Goals

- Strong consistency (Raft/Paxos) for single-key writes.
- Range scans across the full keyspace.
- Cross-vnode transactions.

## 4. Terminology

- **VNode**: Logical shard (hash range or bucket) used for ownership and replication.
- **Worker**: Local execution unit (one per core), owning a partition of vnodes.
- **Replica Set**: The list of nodes responsible for a vnode.
- **Ring**: Cluster metadata that maps vnodes to replicas.

## 5. Recommended Defaults

These defaults are conservative and align with Dynamo/Riak practice while keeping implementation simple.

- vnode_count: 4096 (1024 for very small clusters)
- replication_factor: 3
- write quorum W: 2
- read quorum R: 2
- conflict resolution: LWW (timestamp + node id)
- hinted handoff: enabled, bounded TTL (1-6 hours)
- anti-entropy: per-vnode Merkle sync, jittered schedule

## 6. Architecture

### 6.1 Data Flow (Put)

1) Client computes vnode_id = hash(key) % vnode_count.
2) Coordinator routes to the vnode replica set (RF nodes).
3) Each replica node routes to its local worker owning that vnode.
4) The worker appends to the vnode's active data file and updates vnode KeyDir.

### 6.2 Data Flow (Get)

1) Client computes vnode_id.
2) Coordinator sends read to R replicas.
3) Each replica reads from its vnode store; coordinator merges responses.

### 6.3 Node Layout (Disk)

Each worker owns a set of vnodes. Each vnode has its own files.

```
/data/
├── ring.json
├── node_id
├── worker_0/
│   ├── vnode_0000/
│   │   ├── MANIFEST
│   │   ├── data_0001.db
│   │   ├── hint_0001.ht
│   │   └── ...
│   ├── vnode_0032/
│   └── ...
├── worker_1/
│   └── ...
└── worker_31/
    └── ...
```

### 6.4 Ownership Mapping

- vnode_id = hash(key) % vnode_count
- worker_id = vnode_id % worker_count (default)

A static vnode-to-worker map can be stored to allow custom placement.

## 7. Ring Metadata (ring.json)

The ring is the source of truth for vnode ownership and replication.

Example schema:

```
{
  "ring_epoch": 42,
  "vnode_count": 4096,
  "replication_factor": 3,
  "nodes": {
    "nodeA": {"addr": "10.0.0.1:8080", "status": "up"},
    "nodeB": {"addr": "10.0.0.2:8080", "status": "up"}
  },
  "vnodes": {
    "0":  ["nodeA", "nodeB", "nodeC"],
    "1":  ["nodeB", "nodeC", "nodeD"],
    "2":  ["nodeC", "nodeD", "nodeA"]
  }
}
```

Notes:
- vnodes are fixed; ownership changes by updating replica lists.
- ring_epoch is monotonically increasing.

## 8. Worker Assignment

Each vnode is assigned to exactly one local worker. The worker owns the vnode files and in-memory KeyDir. This preserves share-nothing behavior.

Default mapping:

```
worker_id = vnode_id % worker_count
```

Optional mapping file:

```
/data/worker_0.map
/data/worker_1.map
```

These files list vnode_ids per worker, allowing targeted rebalance without changing vnode_count.

## 9. Replication Model

- Write quorums: W replicas must acknowledge.
- Read quorums: R replicas must respond.
- W + R > RF ensures read-your-write under normal conditions.

Failure handling:
- Coordinator uses hinted handoff for unavailable replicas.
- A hint is stored locally and replayed later.

Conflict resolution:
- LWW by default using (timestamp, node_id).
- Vector clocks can be added later for multi-writer conflicts.

## 10. Anti-Entropy and Repair

Each vnode maintains a Merkle tree summary of its keyspace.

Repair protocol:
1) Replica A compares vnode root hash with Replica B.
2) If different, descend into the tree to locate differing subranges.
3) Exchange key/version for only those subranges.

This avoids per-key scanning when vnodes are in sync.

## 11. Compaction and Handoff

Compaction is vnode-local:
- Only data files for that vnode are rewritten.
- Hint files rebuilt for that vnode only.

Handoff is vnode-local:
- Stream vnode data files and hint files to new owner.
- New owner replays and switches active vnode.

Operational note:
- Pause or throttle compaction during vnode handoff.

## 12. Failure and Recovery

- On startup, each worker scans its vnode directories and rebuilds KeyDir.
- If ring.json changes, workers load new ownership mapping.
- Workers only open vnodes they own locally.

## 13. Observability

Suggested per-vnode metrics:
- puts_total, gets_total, deletes_total
- compactions_total
- fragmentation
- repair_events
- handoff_bytes

Suggested per-worker metrics:
- vnode_count
- io_queue_depth
- fd_open_count

## 14. Implementation Notes

- Each vnode can reuse the existing BitKV partition code path.
- KeyDir is vnode-local, not shared.
- Worker owns the vnode's IO context and file descriptors.
- File descriptor caching is recommended due to vnode count.
- Use atomic ring updates (write ring.json.tmp then rename).

## 15. Single-Node Refactor Impact

To keep the current single-node behavior while enabling distribution later:

Minimal required changes:
- Introduce vnode routing even in single-node mode (hash -> vnode_id).
- Wrap the current partition logic as a vnode store (vnode-scoped data and hint files).
- Add a local ring.json where all vnodes map to the single node.
- Route requests through a local coordinator that still uses the ring.

What stays the same:
- Append-only file format and hint files.
- KeyDir structure and compaction logic.
- Per-core worker model and IO paths.

This lets a single node behave the same, while exercising the distributed path end-to-end.

## 16. Diagrams

### 16.1 Key Routing

```
Client
  |
  | hash(key) -> vnode_id
  v
Coordinator
  |
  | ring.json -> replica set
  v
Replica Nodes (RF)
  |
  | vnode_id -> worker_id
  v
Worker -> Vnode store -> data_*.db + hint_*.ht
```

### 16.2 Ownership and Storage

```
Cluster Ring
  vnode_0000 -> [nodeA, nodeB, nodeC]
  vnode_0001 -> [nodeB, nodeC, nodeD]

NodeA
  worker_0: vnode_0000, vnode_0032, vnode_0064, ...
  worker_1: vnode_0001, vnode_0033, vnode_0065, ...
```

### 16.3 Handoff

```
Old Owner (nodeA)
  vnode_0000 files
     |
     | stream files
     v
New Owner (nodeE)
  vnode_0000 files
```

## 17. Implementation Phases

1) Vnode routing and local vnode storage layout.
2) Replication and quorum reads/writes.
3) Hinted handoff.
4) Merkle-based anti-entropy repair.

## 18. Message Flows

### 18.1 Write Path

1) Client sends PUT(key, value) to coordinator.
2) Coordinator computes vnode_id = hash(key) % vnode_count.
3) Coordinator reads ring.json and selects the vnode replica set.
4) Coordinator sends write to all RF replicas (or parallel until W acks).
5) Replica node routes to local worker owning vnode_id.
6) Worker appends to vnode data file and updates vnode KeyDir.
7) Replica responds to coordinator; coordinator returns success after W acks.

### 18.2 Read Path and Read Repair

1) Client sends GET(key) to coordinator.
2) Coordinator computes vnode_id and selects replica set.
3) Coordinator sends read to R replicas.
4) Coordinator merges responses (selects newest LWW timestamp).
5) If stale replicas are detected, coordinator issues read repair writes to them.

### 18.3 Hinted Handoff Replay

1) Replica is down during write; coordinator stores a hint locally.
2) Hints are keyed by vnode_id and target node.
3) When target node is reachable, coordinator streams hinted writes.
4) Target node applies writes via normal vnode path.
5) Hint entries are deleted after successful replay.

## 19. Implementation Specs (Concrete)

### 19.1 Hashing and Routing

- Hash input: raw bytes of key (or bucket+key if buckets are added later).
- Hash function: XXH3-128 or SHA-1 (choose one and keep it stable).
- vnode_id = hash % vnode_count.
- Preference list: walk ring clockwise from vnode_id, pick N distinct nodes where possible.

### 19.2 Wire Message Formats

Define simple binary or JSON frames with request_id and vnode_id:

- PUT: {request_id, key, value, timestamp, durability_mode}
- GET: {request_id, key}
- DEL: {request_id, key, timestamp}
- REPAIR_PUT: {request_id, key, value, timestamp}
- HANDOFF_STREAM: {request_id, vnode_id, file_id, offset, bytes}
- HINT_PUT: {request_id, vnode_id, target_node, key, value, timestamp}

Responses:

- OK: {request_id}
- VALUE: {request_id, value, timestamp}
- NOT_FOUND: {request_id}
- ERROR: {request_id, code}

### 19.3 Local Vnode Store Contract

Coordinator -> worker calls must be idempotent:

- put(key, value, timestamp, durability_mode)
- get(key)
- del(key, timestamp, durability_mode)

On duplicate request_id, return the same result without reapplying.

### 19.4 Quorum and Durability Behavior

- W: number of replicas that must ack for success.
- R: number of replicas to read before returning.
- DW: optional durable ack (fsync) count. If set, wait for DW fsyncs.
- Timeouts: coordinator returns error if W/R not met before deadline.

### 19.5 Conflict Strategy (Default LWW)

- Timestamp source: coordinator assigns monotonic timestamp; ties broken by node_id.
- On read, highest (timestamp, node_id) wins.
- If switching to vector clocks later, store clock in value metadata.

### 19.6 Hinted Handoff Format

Hints are stored locally in an append-only log:

{target_node, vnode_id, request_id, key, value, timestamp, expiry_ms}

Replay is ordered by hint log offset; remove entry after successful apply.

### 19.7 Merkle Tree Layout

- Build per vnode with fixed fanout (e.g., 16-way).
- Leaf nodes cover hash ranges within the vnode.
- Hash input per leaf: sorted list of (key_hash, timestamp).
- Compare roots, descend into mismatched ranges only.

### 19.8 Backpressure Limits

- Max inflight ops per worker and per vnode (configurable).
- Reject or queue when limits exceed.
- Apply circuit breaker if a vnode falls behind.

## 20. Ring Change Protocol (Minimal)

1) Operator or controller computes new ring.json with updated vnode ownership.
2) Increment ring_epoch and write ring.json.tmp.
3) Atomically rename ring.json.tmp to ring.json.
4) Nodes detect ring_epoch change and update local routing.
5) For newly owned vnodes, start handoff (stream files from old owner).
6) For vnodes no longer owned, stop serving after handoff completes.

## 21. Membership Changes (Add/Remove Node)

This design allows node add/remove without changing local partition_count. Only vnode ownership changes.

Vnode_count guidance:
- Choose a vnode_count that yields 50-200 vnodes per node at expected cluster size.
- More vnodes gives smoother rebalancing but increases files and metadata.
- Keep vnode_count fixed cluster-wide to avoid reshaping the hash space.

Add node:
1) Update ring to move a subset of vnodes to the new node.
2) Stream vnode files (data + hint) from old owners.
3) New node starts serving those vnodes after handoff completes.

Remove node:
1) Update ring to reassign its vnodes to remaining nodes.
2) Handoff from old owners or rebuild from replicas if node is gone.
3) Remaining nodes take over serving those vnodes.

## 22. Open Questions

- What vnode_count and worker_count produce an optimal file descriptor footprint?
- Should compaction be paused during vnode handoff?
- Should hint files include per-key version metadata for conflict resolution?
