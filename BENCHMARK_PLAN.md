# KIO vs. Alibaba Photon Benchmark Report & Integration Plan

## Executive Summary

KIO and Alibaba Photon were benchmarked across identical file and network I/O workloads on an 8-core / 16-thread AMD Ryzen 9 5900HX machine running Linux 7.0 (Ubuntu).

- **Network I/O**: KIO outperforms Photon by **+32% to +34%** at high concurrency (500–1000 connections), reaching **463,523 req/s** vs. **345,269 req/s** with tighter tail latencies (**2.64 ms** vs. **3.78 ms** at p99.9).
- **File I/O**: KIO delivers higher throughput in concurrent random storage workloads with queue depth scaling (**+38.6%** random read at depth 64, **+80% to +87%** random write at depth 16/64 reaching **3,376 MiB/s**).

---

## 1. Network I/O Benchmarks (HTTP Server, 4 Workers)

Both servers ran an HTTP/1.1 echo server returning an identical plaintext response (`HTTP/1.1 200 OK`, 13 bytes body, `Connection: keep-alive`) across 4 worker threads.

### A. Throughput & Latency (`wrk`)

| Architecture | Concurrency | Metric | KIO | Alibaba Photon | Δ (KIO vs Photon) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **SO_REUSEPORT** | 100 conns (4 threads) | Requests/sec<br>Avg Latency | **320,613 req/s**<br>**172.69 µs** | 314,712 req/s<br>178.19 µs | **+1.9%**<br>-3.1% (faster) |
| **SO_REUSEPORT** | 500 conns (8 threads) | Requests/sec<br>Avg Latency | **463,523 req/s**<br>**0.98 ms** | 345,269 req/s<br>1.41 ms | **+34.2%**<br>-30.5% (faster) |
| **SO_REUSEPORT** | 1000 conns (8 threads) | Requests/sec<br>Avg Latency | **427,052 req/s**<br>**2.26 ms** | 323,297 req/s<br>3.05 ms | **+32.1%**<br>-25.9% (faster) |
| **Dispatch** | 100 conns (4 threads) | Requests/sec<br>Avg Latency | **283,540 req/s**<br>**195.21 µs** | 276,406 req/s<br>209.55 µs | **+2.6%**<br>-6.8% (faster) |
| **Dispatch** | 500 conns (8 threads) | Requests/sec<br>Avg Latency | **387,887 req/s**<br>**1.21 ms** | 300,484 req/s<br>1.63 ms | **+29.1%**<br>-25.8% (faster) |

### B. Latency Percentiles (`oha`)

| Scenario | Engine | Throughput | p50 | p90 | p99 | p99.9 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **100 conns, SO_REUSEPORT** | **KIO** | **353,135 req/s** | **0.27 ms** | **0.39 ms** | **0.45 ms** | **0.51 ms** |
| | Photon | 306,849 req/s | 0.31 ms | 0.47 ms | 0.53 ms | 0.63 ms |
| **500 conns, SO_REUSEPORT** | **KIO** | **361,106 req/s** | **1.37 ms** | **1.76 ms** | **2.11 ms** | **2.64 ms** |
| | Photon | 302,843 req/s | 1.55 ms | 1.99 ms | 2.16 ms | 3.78 ms |
| **100 conns, Dispatch** | **KIO** | **328,445 req/s** | **0.30 ms** | **0.38 ms** | **0.43 ms** | **0.58 ms** |
| | Photon | 287,260 req/s | 0.35 ms | 0.41 ms | 0.46 ms | 0.63 ms |

---

## 2. File I/O Benchmarks (NVMe SSD, `/var/tmp`)

Page caches were flushed (`echo 3 > /proc/sys/vm/drop_caches`) before read runs. Median of 3 iterations reported.

### A. Sequential File I/O (512 MiB total transfer)

| Test Operation | Block Size | KIO Throughput | Photon Throughput | Δ (KIO vs Photon) |
| :--- | :--- | :--- | :--- | :--- |
| **Sequential Write** | 4 KiB | **281.4 MiB/s** (72,047 IOPS) | 241.3 MiB/s (61,775 IOPS) | **+16.6%** |
| **Sequential Write** | 64 KiB | **2,591.8 MiB/s** | 2,495.3 MiB/s | **+3.9%** |
| **Sequential Write** | 1 MiB | 5,147.4 MiB/s | **5,791.0 MiB/s** | -11.1% |
| **Sequential Read** | 4 KiB | **1,326.8 MiB/s** (339,658 IOPS) | 1,014.8 MiB/s (259,796 IOPS) | **+30.7%** |
| **Sequential Read** | 64 KiB | **1,579.9 MiB/s** | 1,479.7 MiB/s | **+6.8%** |
| **Sequential Read** | 1 MiB | **1,662.4 MiB/s** | 1,570.0 MiB/s | **+5.9%** |

### B. Concurrent Random File I/O (Queue Depth Scaling, 256 MiB, 4 KiB blocks)

| Test Operation | Queue Depth | KIO Throughput | Photon Throughput | Δ (KIO vs Photon) |
| :--- | :--- | :--- | :--- | :--- |
| **Random Read** | Depth = 1 | **84.4 MiB/s** (21,603 IOPS) | 80.8 MiB/s (20,681 IOPS) | **+4.5%** |
| **Random Read** | Depth = 16 | **792.0 MiB/s** (202,754 IOPS) | 636.1 MiB/s (162,854 IOPS) | **+24.5%** |
| **Random Read** | Depth = 64 | **1,016.9 MiB/s** (260,319 IOPS) | 733.6 MiB/s (187,792 IOPS) | **+38.6%** |
| **Random Write** | Depth = 1 | **268.2 MiB/s** (68,661 IOPS) | 237.5 MiB/s (60,790 IOPS) | **+12.9%** |
| **Random Write** | Depth = 16 | **3,376.2 MiB/s** (864,299 IOPS) | 1,799.0 MiB/s (460,544 IOPS) | **+87.7%** |
| **Random Write** | Depth = 64 | **3,336.7 MiB/s** (854,190 IOPS) | 1,850.0 MiB/s (473,587 IOPS) | **+80.4%** |

### C. Direct I/O (`O_DIRECT`, Unbuffered 4 KiB)

| Test Operation | Block Size | KIO Throughput | Photon Throughput | Δ (KIO vs Photon) |
| :--- | :--- | :--- | :--- | :--- |
| **Direct Write** | 4 KiB (aligned) | **195.4 MiB/s** (50,032 IOPS) | 171.4 MiB/s (43,877 IOPS) | **+14.0%** |
| **Direct Read** | 4 KiB (aligned) | 139.9 MiB/s (35,818 IOPS) | **149.6 MiB/s** (38,306 IOPS) | -6.5% |

---

## 3. Integration Plan & Package Contents

The benchmark source code is packaged in `kio_photon_benchmarks.zip` at the repository root.

Contents:
- `kio_disk_bench.cpp`: C++23 disk I/O benchmark target using KIO (`URing::IO`).
- `photon_disk_bench.cpp`: Equivalent disk I/O benchmark target using Alibaba Photon (`ioengine_iouring`).
- `photon_http_bench.cpp`: Equivalent HTTP benchmark server for Photon with SO_REUSEPORT and WorkPool dispatch modes.
- `run_benchmarks.py`: Automated Python benchmark runner orchestrating disk and network test suites (`wrk` and `oha`).
- `benchmark_results.json`: Full machine-readable test results data.
- `test_concurrent_kio.cpp`, `test_kio_file.cpp`, `test_photon_file.cpp`: Helper test fixtures.
