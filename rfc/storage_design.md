# RFC: Distributed Bitcask Storage Engine

**Status:** Draft  
**Author:** [TBD]  
**Created:** January 2026

---

## 1. Overview

This document describes the design of a distributed key-value storage engine built on Bitcask principles, using a
share-nothing architecture with io_uring for async I/O.

### Goals

- Simple, predictable performance
- Linear horizontal scaling
- Cache-friendly execution
- Clean replication boundaries
- Minimal operational complexity

### Non-Goals

- Cross-partition transactions
- Dynamic partition splitting
- Work-stealing between cores

---

## 2. Architecture Summary

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              Cluster                                    │
│                                                                         │
│   ┌─────────────┐     ┌─────────────┐     ┌─────────────┐              │
│   │   Node A    │     │   Node B    │     │   Node C    │              │
│   │             │     │             │     │             │              │
│   │  P0  P1  P2 │     │  P3  P4  P5 │     │  P6  P7  P8 │  ...         │
│   │             │     │             │     │             │              │
│   └─────────────┘     └─────────────┘     └─────────────┘              │
│                                                                         │
│   Partitions distributed across nodes                                   │
│   Each partition replicated RF times                                    │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Partitioning

### 3.1 Fixed Partition Count

The system uses a fixed number of partitions determined at cluster creation time. This number never changes.

```cpp
constexpr uint32_t NUM_PARTITIONS = 256;  // or 512, 1024 based on scale
```

**Rationale:** Fixed partition count avoids the complexity of partition splitting, key remapping, and associated data
migration.

### 3.2 Key to Partition Mapping

Keys map to partitions via simple modulo hashing:

```cpp
uint32_t partition_for_key(std::string_view key) {
    return xxhash(key) % NUM_PARTITIONS;
}
```

**Rationale:** With a good hash function and fixed partition count, distribution is perfectly even. Consistent hashing (
Ketama) provides no benefit here and adds complexity.

### 3.3 Partition to Node Mapping

Partitions are assigned to nodes using consistent hashing with virtual nodes:

```cpp
NodeId node_for_partition(PartitionId p) {
    return ketama_ring.lookup(p);
}
```

**Rationale:** Consistent hashing at this level enables smooth rebalancing when nodes join or leave. Virtual nodes
ensure even distribution across physical nodes.

### 3.4 Two-Level Routing Summary

| Level            | Mapping             | Method          | When It Changes |
|------------------|---------------------|-----------------|-----------------|
| Key → Partition  | `hash(key) % N`     | Simple modulo   | Never           |
| Partition → Node | `ketama(partition)` | Consistent hash | Node join/leave |

---

## 4. Node Architecture

### 4.1 Components

Each node contains:

- **Workers:** One per CPU core, each with its own io_uring instance
- **Partitions:** Data and state for assigned partitions
- **Affinity Map:** Partition to worker assignment

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              Node                                       │
│                                                                         │
│   ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐        │
│   │    Worker 0     │  │    Worker 1     │  │    Worker 2     │        │
│   │   (io_uring)    │  │   (io_uring)    │  │   (io_uring)    │        │
│   │                 │  │                 │  │                 │        │
│   │   P0, P3, P6    │  │   P1, P4, P7    │  │   P2, P5, P8    │        │
│   │   (affinity)    │  │   (affinity)    │  │   (affinity)    │        │
│   └─────────────────┘  └─────────────────┘  └─────────────────┘        │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

### 4.2 Ownership vs Affinity

| Concept       | Scope           | Purpose                                      |
|---------------|-----------------|----------------------------------------------|
| **Ownership** | Cluster-visible | Node owns partitions for replication/routing |
| **Affinity**  | Node-internal   | Worker affinity for cache locality           |

The cluster only knows "Node A owns partition P47." Which worker handles it is an internal optimization invisible to
other nodes.

### 4.3 Worker Affinity Assignment

Partitions are assigned to workers using hash-based affinity:

```cpp
uint32_t worker_for_partition(PartitionId p, uint32_t num_workers) {
    return xxhash(p) % num_workers;
}
```

**Rationale:** Hash-based assignment guarantees even distribution regardless of which partition IDs the node receives.
Simple round-robin (`p % num_workers`) can produce pathological imbalance if partition IDs follow patterns.

---

## 5. Local Partition Storage

### 5.1 Sparse Array

Each node uses a sparse array indexed by partition ID. Most slots are empty (nullptr), only owned partitions are
populated.

```cpp
class Node {
    std::array<std::unique_ptr<Partition>, NUM_PARTITIONS> partitions_;
    std::array<WorkerId, NUM_PARTITIONS> affinity_;
    uint32_t num_workers_;
    
public:
    void own_partition(PartitionId pid) {
        partitions_[pid] = std::make_unique<Partition>(pid);
        affinity_[pid] = hash(pid) % num_workers_;
    }
    
    Partition* partition(PartitionId pid) {
        return partitions_[pid].get();  // O(1), nullptr if not owned
    }
    
    WorkerId affinity(PartitionId pid) {
        return affinity_[pid];
    }
};
```

Example for Node B owning {P12, P33, P47, P89, P102}:

```
partitions_ array:

Index:  [0]    [1]   ... [12]   ... [33]   ... [47]   ... [89]   ... [102]  ... [255]
Value:  null   null      P12*       P33*       P47*       P89*       P102*      null
```

**Rationale:**

- O(1) direct indexing by partition ID
- Memory overhead: 256 pointers = 2KB (negligible)
- No hash map overhead or indirection
- Simple to implement

The sparse array answers: "Do I own partition X locally?"

### 5.2 Worker Affinity Calculation

Affinity is computed by hashing the partition ID:

```cpp
WorkerId worker_for_partition(PartitionId pid, uint32_t num_workers) {
    return hash(pid) % num_workers;
}
```

Example for Node B with 4 workers:

```
hash(12)  % 4 = 3  →  P12  → Worker 3
hash(33)  % 4 = 1  →  P33  → Worker 1
hash(47)  % 4 = 2  →  P47  → Worker 2
hash(89)  % 4 = 0  →  P89  → Worker 0
hash(102) % 4 = 1  →  P102 → Worker 1
```

Result:

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Node B                                                                 │
│                                                                         │
│  Worker 0:  {P89}                                                       │
│  Worker 1:  {P33, P102}                                                 │
│  Worker 2:  {P47}                                                       │
│  Worker 3:  {P12}                                                       │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

**Rationale:**

- Deterministic: same partition always maps to same worker
- Even distribution: hash spreads partitions across workers
- Pattern-resistant: non-contiguous partition IDs don't cluster

---

## 6. Storage Model

### 6.1 Bitcask Per Partition

Each partition is a self-contained Bitcask instance:

```
/data/partitions/
├── p0/
│   ├── 00001.log
│   ├── 00002.log
│   ├── 00003.log    (active)
│   └── hints/
│       ├── 00001.hint
│       └── 00002.hint
├── p1/
│   └── ...
└── p2/
    └── ...
```

### 6.2 Log Entry Format

```
┌──────────┬───────────┬───────────┬─────────┬───────┬───────┐
│   CRC    │ Timestamp │  Key Len  │ Val Len │  Key  │ Value │
│  4 bytes │  8 bytes  │  4 bytes  │ 4 bytes │  var  │  var  │
└──────────┴───────────┴───────────┴─────────┴───────┴───────┘
```

### 6.3 KeyDir (In-Memory Index)

Each partition maintains an in-memory hash table mapping keys to file positions:

```cpp
struct KeyDirEntry {
    uint32_t file_id;
    uint32_t offset;
    uint32_t size;
    uint64_t timestamp;
};

using KeyDir = std::unordered_map<std::string, KeyDirEntry>;
```

---

## 7. Replication

### 7.1 Replication Unit

The partition is the unit of replication.

```
┌─────────────┐      log entries      ┌─────────────┐
│   Node A    │ ────────────────────► │   Node B    │
│  P47 Leader │                       │ P47 Follower│
│             │      log entries      ├─────────────┤
│             │ ────────────────────► │   Node C    │
│             │                       │ P47 Follower│
└─────────────┘                       └─────────────┘
```

**Rationale:** One partition = one folder = one replication stream. Simple to reason about, simple to implement.

### 7.2 Replication Factor

Fixed at cluster configuration:

```cpp
constexpr uint32_t REPLICATION_FACTOR = 3;
```

### 7.3 What Gets Replicated

Log entries (the encoded key-value-timestamp tuples) are shipped to followers. Followers:

1. Append entry to their own log file
2. Update their in-memory KeyDir

No complex state reconciliation required.

### 7.4 Replication Modes

| Mode   | Behavior                            | Trade-off               |
|--------|-------------------------------------|-------------------------|
| Sync   | Wait for all followers before ack   | Durable, higher latency |
| Async  | Ack immediately, ship in background | Fast, risk of data loss |
| Quorum | Wait for (RF/2)+1 acks              | Balanced                |

**Recommendation:** Start with sync replication for simplicity. Optimize later.

### 7.5 Leader Distribution

Leaders are distributed across nodes to spread write load:

```
Partition P0: Leader=Node A, Followers={Node B, Node C}
Partition P1: Leader=Node B, Followers={Node C, Node A}
Partition P2: Leader=Node C, Followers={Node A, Node B}
```

---

## 8. API Design

### 8.1 User-Facing API

Users see a simple interface:

```cpp
co_await db.put("user:123", data);
auto value = co_await db.get("user:123");
co_await db.remove("user:123");
```

### 8.2 Internal Layering

```
┌─────────────────────────────────────────────────────────────────────────┐
│  User Code                                                              │
│      co_await db.put(key, value);                                       │
├─────────────────────────────────────────────────────────────────────────┤
│  Worker (accepts connection via SO_REUSEPORT)                           │
│      - Parses request                                                   │
│      - Routes to correct worker if needed                               │
│      - Provides IoContext                                               │
├─────────────────────────────────────────────────────────────────────────┤
│  DB (Facade)                                                            │
│      - Hashes key to partition                                          │
│      - Forwards to partition                                            │
├─────────────────────────────────────────────────────────────────────────┤
│  Partition                                                              │
│      - Pure state + logic                                               │
│      - put(IoContext&, key, value)                                      │
└─────────────────────────────────────────────────────────────────────────┘
```

### 8.3 IoContext Passing

Partition methods receive IoContext as an argument, not as a member:

```cpp
class Partition {
    KeyDir keydir_;
    std::string path_;
    
public:
    Task<void> put(IoContext& io, Key k, Value v);
    Task<std::optional<Value>> get(IoContext& io, Key k);
    Task<void> remove(IoContext& io, Key k);
};
```

**Rationale:**

- Partitions are pure state + logic, decoupled from execution context
- Enables testing with mock IoContext
- Allows affinity reassignment without reconstructing partitions
- No reference lifetime concerns

### 8.4 Request Routing

Routing is handled by the worker that accepts the connection. If the target partition has affinity with a different
worker, cross-post occurs. See Section 8 for details.

The DB class contains no routing logic—it simply hashes and forwards to the partition:

```cpp
class DB {
    Node& node_;
    
public:
    Task<void> put(IoContext& io, Key k, Value v) {
        auto partition_id = hash(k) % NUM_PARTITIONS;
        co_await node_.partition(partition_id).put(io, k, v);
    }
};
```

---

## 9. Networking Model

### 9.1 SO_REUSEPORT Per Worker

Each worker binds to the same port with `SO_REUSEPORT`. The kernel distributes incoming connections across workers.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              Node                                       │
│                                                                         │
│   ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐        │
│   │    Worker 0     │  │    Worker 1     │  │    Worker 2     │        │
│   │   (io_uring)    │  │   (io_uring)    │  │   (io_uring)    │        │
│   │                 │  │                 │  │                 │        │
│   │  listen :9000   │  │  listen :9000   │  │  listen :9000   │        │
│   │  (SO_REUSEPORT) │  │  (SO_REUSEPORT) │  │  (SO_REUSEPORT) │        │
│   │                 │  │                 │  │                 │        │
│   │   P0, P3, P6    │  │   P1, P4, P7    │  │   P2, P5, P8    │        │
│   └─────────────────┘  └─────────────────┘  └─────────────────┘        │
│                                                                         │
│   Kernel distributes connections across workers                         │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

**Rationale:**

- No single-core accept bottleneck
- True share-nothing: each worker handles its own connections
- Fast path when request lands on correct worker (no cross-post)

### 9.2 Request Routing Within Node

```cpp
void Worker::on_request(Connection& conn, Request req) {
    auto partition_id = hash(req.key) % NUM_PARTITIONS;
    auto target_worker = affinity[partition_id];
    
    if (target_worker == this->id) {
        // Fast path: handle locally
        co_await handle_locally(conn, req);
    } else {
        // Cross-post to correct worker
        co_await workers[target_worker].post([&]() -> Task<void> {
            co_await handle_locally(conn, req);
        });
    }
}
```

Statistically, `1/num_workers` requests hit the fast path (no cross-post).

### 9.3 Write Path

```
1. Client connects to Node:9000
2. Kernel assigns connection to Worker N (SO_REUSEPORT)
3. Worker N accepts, parses: PUT("user:123", data)
4. Compute: partition = hash("user:123") % 256 → P47
5. Check: Is this node the leader for P47?
   - No  → Forward to leader node
   - Yes → Continue
6. Compute: target_worker = affinity[P47] → Worker 2
7. If N == 2: handle locally (fast path)
   Else: post to Worker 2
8. Worker 2 executes:
   a. Append entry to P47 log file (io_uring pwrite)
   b. Update P47 KeyDir
   c. Ship entry to followers (io_uring send)
   d. Wait for follower acks
9. Response back through original connection
```

### 9.4 Read Path

```
1. Client connects to Node:9000
2. Kernel assigns connection to Worker N (SO_REUSEPORT)
3. Worker N accepts, parses: GET("user:123")
4. Compute: partition = hash("user:123") % 256 → P47
5. Check: Does this node have a replica of P47?
   - No  → Forward to a replica node
   - Yes → Continue
6. Compute: target_worker = affinity[P47] → Worker 2
7. If N == 2: handle locally (fast path)
   Else: post to Worker 2
8. Worker 2 executes:
   a. Lookup key in P47 KeyDir
   b. Read value from log file (io_uring pread)
9. Response back through original connection
```

### 9.5 Detailed Request Flow Example

This section traces `PUT("hello", "world")` end to end.

**Setup:**

```
Cluster:
  - 256 partitions (fixed)
  - RF = 3
  - 3 nodes, 4 workers each

Partition P47:
  - Leader: Node B
  - Replicas: Node A, Node C

Node B owns: {P12, P33, P47(leader), P89, P102, ...}
Node B workers: 4
Node B affinity: P47 → Worker 2
```

**Full Trace:**

```
┌─────────────────────────────────────────────────────────────────────────┐
│  STEP 1: Client                                                         │
│  ─────────────────                                                      │
│                                                                         │
│  client.put("hello", "world")                                           │
│                                                                         │
│  Client knows cluster nodes: [A, B, C]                                  │
│  Client picks one (any): Node A                                         │
│  Sends request to Node A:9000                                           │
│                                                                         │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  STEP 2: Node A receives request (Worker 3 via SO_REUSEPORT)            │
│  ────────────────────────────────────────────────────────────────────   │
│                                                                         │
│  // Parse request                                                       │
│  key = "hello"                                                          │
│                                                                         │
│  // Global hash → partition                                             │
│  partition_id = hash("hello") % 256 = 47                                │
│                                                                         │
│  // Do I own P47?                                                       │
│  partitions_[47] == nullptr?  → YES, I don't own it                     │
│                                                                         │
│  // Who owns P47? (Ketama lookup)                                       │
│  leader = ketama_.leader(47) = Node B                                   │
│                                                                         │
│  // Forward to Node B                                                   │
│  forward(NodeB, request)                                                │
│                                                                         │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  STEP 3: Node B receives request (Worker 0 via SO_REUSEPORT)            │
│  ────────────────────────────────────────────────────────────────────   │
│                                                                         │
│  // Same global hash                                                    │
│  partition_id = hash("hello") % 256 = 47                                │
│                                                                         │
│  // Do I own P47?                                                       │
│  partitions_[47] != nullptr?  → YES, I own it                           │
│                                                                         │
│  // Which worker has affinity?                                          │
│  target_worker = affinity_[47] = Worker 2                               │
│                                                                         │
│  // Am I Worker 2?                                                      │
│  current_worker == 0, target == 2  → NO, cross-post needed              │
│                                                                         │
│  // Post to Worker 2                                                    │
│  workers_[2].post(handle_put(47, "hello", "world"))                     │
│                                                                         │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  STEP 4: Node B, Worker 2 executes                                      │
│  ─────────────────────────────────                                      │
│                                                                         │
│  // Now on correct worker with correct IoContext                        │
│  Partition& p = *partitions_[47];                                       │
│                                                                         │
│  // Bitcask write                                                       │
│  entry = encode(key="hello", value="world", timestamp=now)              │
│  co_await io_.pwrite(p.active_log_fd, entry, p.write_offset);           │
│  p.keydir["hello"] = {file_id, offset, size, timestamp};                │
│  p.write_offset += entry.size();                                        │
│                                                                         │
│  // Replication (parallel via io_uring)                                 │
│  co_await when_all(                                                     │
│      replica_send(NodeA, partition=47, entry),                          │
│      replica_send(NodeC, partition=47, entry)                           │
│  );                                                                     │
│                                                                         │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  STEP 5: Replicas (Node A, Node C) apply entry                          │
│  ─────────────────────────────────────────────                          │
│                                                                         │
│  // Receive replication entry for P47                                   │
│  // Route to worker with affinity for P47                               │
│  // Append to local log, update keydir                                  │
│  // Ack back to Node B                                                  │
│                                                                         │
└────────────────────────────────────┬────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────┐
│  STEP 6: Response                                                       │
│  ───────────────                                                        │
│                                                                         │
│  Node B Worker 2 → Node B Worker 0 (response)                           │
│  Node B Worker 0 → Node A Worker 3 (response)                           │
│  Node A Worker 3 → Client (OK)                                          │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

**Summary:**

| Step | Where            | What                                             |
|------|------------------|--------------------------------------------------|
| 1    | Client           | Pick any node, send request                      |
| 2    | Node A           | Hash key → P47, don't own it → forward to leader |
| 3    | Node B, Worker 0 | Own P47, but affinity is Worker 2 → cross-post   |
| 4    | Node B, Worker 2 | Write to Bitcask, replicate                      |
| 5    | Replicas         | Apply entry                                      |
| 6    | All              | Response chain back to client                    |

The hash `hash(key) % 256` is computed at every hop, but always gives the same answer. The sparse array answers "do I
have it locally?"

---

## 10. Configuration

### 10.1 Cluster Configuration

```yaml
cluster:
  num_partitions: 256          # Fixed at creation, never changes
  replication_factor: 3

nodes:
  - id: node-a
    address: 10.0.0.1:9000
  - id: node-b
    address: 10.0.0.2:9000
  - id: node-c
    address: 10.0.0.3:9000
```

### 10.2 Node Configuration

```yaml
node:
  id: node-a
  data_dir: /data/partitions
  num_workers: 8               # Typically = CPU cores
  listen_port: 9000            # All workers bind with SO_REUSEPORT

io_uring:
  queue_depth: 256
  flags:
    - IORING_SETUP_SINGLE_ISSUER
    - IORING_SETUP_COOP_TASKRUN
```

---

## 11. Future Considerations

The following are explicitly deferred:

| Feature                      | Notes                                                |
|------------------------------|------------------------------------------------------|
| Dynamic partition count      | Would require key remapping, significant complexity  |
| Work stealing                | Conflicts with io_uring-per-worker model             |
| Cross-partition transactions | Out of scope for Bitcask model                       |
| Affinity rebalancing         | Start with static affinity, optimize later if needed |

---

## 12. Summary of Decisions

| Decision                | Choice                               | Rationale                                |
|-------------------------|--------------------------------------|------------------------------------------|
| Partition count         | Fixed (e.g., 256)                    | Simplicity, no remapping                 |
| Key → Partition         | `hash % N`                           | Perfect distribution, simple             |
| Partition → Node        | Consistent hash (Ketama)             | Smooth rebalancing                       |
| Local partition storage | Sparse array indexed by partition ID | O(1) lookup, 2KB overhead, simple        |
| Worker affinity         | `hash(partition) % workers`          | Even distribution, pattern-resistant     |
| Replication unit        | Partition                            | Clean boundary                           |
| IoContext               | Passed as argument                   | Decoupling, testability                  |
| Networking              | SO_REUSEPORT per worker              | No accept bottleneck, true share-nothing |
| Routing location        | Worker (post if affinity mismatch)   | Fast path for local partitions           |