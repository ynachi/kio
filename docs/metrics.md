# KIO Metrics System

A lightweight metrics system for KIO-based applications.

## Architecture Overview

The metrics system follows a **collector pattern** with these key parts:

```
┌─────────────────────────────────────────────────────────────┐
│                     HTTP /metrics endpoint                  │
│                    (any thread, blocks)                     │
└────────────────────────────┬────────────────────────────────┘
                             │ calls Scrape()
                             ▼
                    ┌─────────────────┐
                    │ MetricsRegistry │ (singleton, thread-safe)
                    │  - holds all    │
                    │    collectors   │
                    └────────┬────────┘
                             │ calls Collect() on each
                ┌────────────┴────────────┐
                ▼                         ▼
    ┌──────────────────────┐  ┌──────────────────────┐
    │ WorkerMetricsCollec  │  │ ComponentCollector   │
    │ - hops to worker     │  │                      │
    │ - reads stats        │  │                      │
    └──────────────────────┘  └──────────────────────┘
                │                         │
                ▼                         ▼
          ┌──────────┐            ┌──────────────┐
          │  Worker  │    ...     │  Component   │
          │  stats   │            │   stats      │
          └──────────┘            └──────────────┘
```

### Key Design Principles

1. **Share-Nothing Hot Path**: Workers maintain local, non-atomic stats for zero-overhead tracking
2. **Cold Path Coordination**: Only during scraping do we coordinate between threads
3. **Safe Cross-Thread Access**: Uses `SyncWait(SwitchToWorker())` to safely read worker-local data
4. **Zero External Dependencies**: Pure C++23, works with your existing coroutine infrastructure

## Core Components

### 1. `MetricSnapshot` - Transient Data Container

Created fresh on every scrape. NOT a singleton. Holds metric families and samples.

```cpp
MetricSnapshot snapshot;
auto& family = snapshot.BuildFamily(
    "kio_worker_bytes_read_total",  // metric name
    "Total bytes read by workers",   // help text
    MetricType::Counter              // type
);
family.Add({{"worker_id", "0"}}, 12345.0);
```

### 2. `IMetricsCollector` - Component Interface

Any component that wants to expose metrics implements this:

```cpp
class MyCollector : public IMetricsCollector {
public:
    void Collect(MetricSnapshot& snapshot) override {
        // Safely gather your data and add to snapshot
    }
};
```

### 3. `MetricsRegistry` - Central Orchestrator

Thread-safe singleton that holds all collectors:

```cpp
// At startup, register your collectors
auto collector = std::make_shared<WorkerMetricsCollector>(worker);
MetricsRegistry<>::Instance().Register(collector);

// In your HTTP endpoint
std::string metrics = MetricsRegistry<>::Instance().Scrape();
```

### 4. `IMetricSerializer` - Output Format

Currently, it supports a Prometheus text format. Extensible to JSON, etc.

## How to Implement Metrics for a New Component

### Step 1: Define Your Stats Structure

Keep it simple, local to the component. To keep benefiting from the kio shared-nothing architecture, avoid atomics and
any other locking primitives. Bellow we share a pattern for still getting the metrics safely across threads.

```cpp
// In your component's header
struct MyComponentStats {
    size_t requests_total{0};
    size_t errors_total{0};
    size_t active_connections{0};  // gauge
    
    // Add whatever you need to track
};

class MyComponent {
    MyComponentStats stats_;
    
public:
    void handle_request() {
        stats_.requests_total++;
        // ... do work
    }
    
    const MyComponentStats& get_stats() const { return stats_; }
};
```

### Step 2: Create a Collector

The collector knows how to safely read your component's stats:

```cpp
// my_component_metrics.h
#include "core/include/metrics/collector.h"
#include "my_component.h"

class MyComponentMetricsCollector : public IMetricsCollector {
    MyComponent& component_;
    
public:
    explicit MyComponentMetricsCollector(MyComponent& comp) 
        : component_(comp) {}
    
    void Collect(MetricSnapshot& snapshot) override {
        // If your component is single-threaded, just read:
        const auto& stats = component_.get_stats();
        
        // OR if it runs on a Worker, hop to its thread:
        // auto stats = SyncWait([&]() -> Task<MyComponentStats> {
        //     co_await SwitchToWorker(component_.worker());
        //     co_return component_.get_stats();
        // }());
        
        // Build metric families
        auto& requests = snapshot.BuildFamily(
            "mycomponent_requests_total",
            "Total requests processed",
            MetricType::Counter
        );
        requests.Add({{"component_id", "1"}}, stats.requests_total);
        
        auto& errors = snapshot.BuildFamily(
            "mycomponent_errors_total",
            "Total errors encountered",
            MetricType::Counter
        );
        errors.Add({{"component_id", "1"}}, stats.errors_total);
        
        auto& active = snapshot.BuildFamily(
            "mycomponent_active_connections",
            "Current active connections",
            MetricType::Gauge
        );
        active.Add({{"component_id", "1"}}, stats.active_connections);
    }
};
```

### Step 3: Register at Startup

```cpp
// In your application startup code
void setup_metrics(MyComponent& component) {
    auto collector = std::make_shared<MyComponentMetricsCollector>(component);
    MetricsRegistry<>::Instance().Register(collector);
}
```

### Step 4: Expose via HTTP Endpoint

```cpp
// In your HTTP server
Task<void> handle_metrics_request(/* ... */) {
    std::string metrics = MetricsRegistry<>::Instance().Scrape();
    co_await send_response(200, "text/plain", metrics);
}
```

## Complete Example: BufferPool Metrics

```cpp
// ============================================================
// Step 1: Define stats in your component
// ============================================================
struct BufferPoolStats {
    size_t acquires_total{0};
    size_t cache_hits{0};
    size_t cache_misses{0};
    size_t pooled_memory_bytes{0};
};

class BufferPool {
    BufferPoolStats stats_;
    size_t worker_id_;
    
public:
    void acquire() { stats_.acquires_total++; }
    const BufferPoolStats& get_stats() const { return stats_; }
    size_t worker_id() const { return worker_id_; }
};

// ============================================================
// Step 2: Create collector
// ============================================================
class BufferPoolMetricsCollector : public IMetricsCollector {
    BufferPool& pool_;
    Worker& worker_;  // The worker that owns this pool
    
public:
    BufferPoolMetricsCollector(BufferPool& pool, Worker& worker)
        : pool_(pool), worker_(worker) {}
    
    void Collect(MetricSnapshot& snapshot) override {
        // Safely hop to the worker's thread to read stats
        auto get_stats_task = [&]() -> Task<BufferPoolStats> {
            co_await SwitchToWorker(worker_);
            co_return pool_.get_stats();
        };
        
        BufferPoolStats stats = SyncWait(get_stats_task());
        std::string wid = std::to_string(pool_.worker_id());
        
        // Add metrics
        auto& acquires = snapshot.BuildFamily(
            "bufferpool_acquires_total",
            "Total buffer acquisitions",
            MetricType::Counter
        );
        acquires.Add({{"worker_id", wid}}, stats.acquires_total);
        
        auto& hits = snapshot.BuildFamily(
            "bufferpool_cache_hits_total",
            "Buffer pool cache hits",
            MetricType::Counter
        );
        hits.Add({{"worker_id", wid}}, stats.cache_hits);
        
        auto& memory = snapshot.BuildFamily(
            "bufferpool_pooled_memory_bytes",
            "Total pooled memory in bytes",
            MetricType::Gauge
        );
        memory.Add({{"worker_id", wid}}, stats.pooled_memory_bytes);
    }
};

// ============================================================
// Step 3: Register at startup
// ============================================================
void init_worker_with_metrics(Worker& worker) {
    // Create buffer pool for this worker
    auto pool = std::make_unique<BufferPool>(worker.get_id());
    
    // Register its metrics collector
    auto collector = std::make_shared<BufferPoolMetricsCollector>(
        *pool, worker
    );
    MetricsRegistry<>::Instance().Register(collector);
    
    // Store pool in worker or wherever it belongs
    worker.set_buffer_pool(std::move(pool));
}
```

## Thread Safety Model

- **Component stats**: No locking needed, single-threaded access
- **Collectors**: Read-only during scrape, safe. No locking is needed during instrumentation. So low overhead.
- **Registry**: Mutex-protected, only accessed during registration and scraping.
- **Cross-thread access**: `SyncWait(SwitchToWorker())` ensures safety

## Advanced: Custom Serializers

Want JSON output instead of Prometheus text?

```cpp
class MyJsonSerializer : public IMetricSerializer {
public:
    std::string Serialize(const MetricSnapshot& snapshot) override {
        // Your JSON implementation
        return "{ ... }";
    }
};

// Use it:
using MyRegistry = MetricsRegistry<MyJsonSerializer>;
MyRegistry::Instance().Register(collector);
```

## Future Enhancements

- [ ] Histogram support (e.g., request latency distributions)
- [ ] Summary support (e.g., quantiles)
- [ ] Metric metadata (unit, exemplars)
- [ ] OpenTelemetry exporter
- [ ] Metric cardinality limits (prevent label explosion)

## Example Output

```
# HELP kio_worker_bytes_read_total Total bytes read by kio workers
# TYPE kio_worker_bytes_read_total counter
kio_worker_bytes_read_total{worker_id="0"} 1048576
kio_worker_bytes_read_total{worker_id="1"} 2097152

# HELP bufferpool_acquires_total Total buffer acquisitions
# TYPE bufferpool_acquires_total counter
bufferpool_acquires_total{worker_id="0"} 1523
bufferpool_acquires_total{worker_id="1"} 2847

# HELP bufferpool_pooled_memory_bytes Total pooled memory in bytes
# TYPE bufferpool_pooled_memory_bytes gauge
bufferpool_pooled_memory_bytes{worker_id="0"} 262144
bufferpool_pooled_memory_bytes{worker_id="1"} 524288
```

## Summary

1. **Define simple stats struct** in your component (non-atomic!)
2. **Create a collector** that implements `IMetricsCollector`
3. **Use SyncWait + SwitchToWorker** for cross-thread safety
4. **Register at startup** with `MetricsRegistry::Instance().Register()`
5. **Expose via HTTP** by calling `Scrape()` in your endpoint

