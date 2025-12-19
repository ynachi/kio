# Benchmark Results: KIO vs Photon

**Test Configuration:**

- Workers: 4
- Backend: io_uring
- Tool: oha
- Hardware: AMD Ryzen 9 5900HX with Radeon Graphics
- Server and workers on the same machine

## Summary Table

| Metric         | Photon        | kio           | Difference   |
|----------------|---------------|---------------|--------------|
| Throughput     | 272,499 req/s | 263,198 req/s | Photon +3.5% |
| P50 Latency    | 5.32 ms       | 5.70 ms       | Photon -7%   |
| P99 Latency    | 11.40 ms      | 7.33 ms       | **kio -36%** |
| P99.9 Latency  | 15.77 ms      | 8.75 ms       | **kio -44%** |
| P99.99 Latency | 50.07 ms      | 13.57 ms      | **kio -73%** |
| Max Latency    | 173.95 ms     | 99.31 ms      | **kio -43%** |

### Summary

kio trades ~3.5% throughput for significantly better tail latency. At P99.99, kio is 73% faster than Photon, making it
more suitable for latency-sensitive workloads.