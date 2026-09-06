# BedrockKV Benchmarks

All numbers in this file come from the in-repo harness (`bench/ycsb_bench.cpp`,
a faithful C++ port of the YCSB core workloads A–F). Every optimization lands
here as a before/after pair — no number without a reproducible command.

## Methodology

- **Environment**: cloud Linux host (gVisor sandbox), tmpfs-backed `/tmp`,
  Release build (`-O2`). Single benchmark process; the engine runs with its
  default threaded design (1 writer + 1 background flush/compaction thread).
- **Workloads**: YCSB core A/B/C/D/E/F (mixes in `bench/ycsb.h`), zipfian
  key distribution (θ = 0.99), workload D uses the skewed-latest variant.
- **Data size**: 100,000 records × 1 KiB values (~100 MB user data), loaded
  before each run phase; load throughput is reported separately.
- **Latency**: per-operation `steady_clock` samples, exact percentiles over
  all samples (no bucketing).
- **Write amplification** = (WAL bytes + SST bytes written) / user bytes
  written, read from the engine's own counters (`DB::wal_bytes_written` /
  `sst_bytes_written` / `user_bytes_written`). The counters are cumulative
  over load + run, so read-heavy workloads show mostly the load phase's
  amplification.
- **Command** (reproduce exactly):
  ```
  ./build/ycsb_bench --workload all --recordcount 100000 \
      --operationcount 100000 --value_size 1024
  ```

## Baseline — stage 2 engine, before WiscKey / io_uring (2026-09-06)

| Workload | Throughput (ops/s) | p50 | p95 | p99 | p99.9 | max | Write amp |
|----------|-------------------:|----:|----:|----:|------:|----:|----------:|
| A (50R/50U)   | 48,203  | 7.8µs  | 21.6µs | 57.5µs | 242.2µs | **580.8ms** | 4.88x |
| B (95R/5U)    | 164,398 | 1.7µs  | 11.0µs | 58.9µs | 144.2µs | 67.2ms  | 3.76x |
| C (100R)      | 821,276 | 945ns  | 2.2µs  | 2.9µs  | 10.4µs  | 2.4ms   | 3.82x |
| D (95R/5I)    | 274,807 | 902ns  | 7.7µs  | 15.3µs | 35.4µs  | 55.7ms  | 3.76x |
| E (95S/5I)    | 144,662 | 4.9µs  | 13.8µs | 25.5µs | 94.3µs  | 28.2ms  | 3.76x |
| F (50R/50RMW) | 24,063  | 11.3µs | 23.6µs | 41.9µs | 113.2µs | **582.4ms** | 5.03x |

Load throughput: 24k–42k inserts/s (WAL fsync every ~1 MiB, periodic mode).

### What the baseline already tells us (optimization targets)

1. **Write amplification ~4.9–5.0x on write-heavy workloads (A, F).**
   Breakdown for A: WAL 161 MB, SST 605 MB for 157 MB of user data. The SST
   side (compaction rewriting values across levels) is exactly what WiscKey
   value separation attacks: 1 KiB values dominate the bytes, and separated
   values are appended to a vLog once instead of being rewritten by every
   compaction that touches their key.
2. **Tail latency spikes of 580 ms (A, F max).** These are background
   compaction stalls — a whole-file-in-memory engine doing a 4 MiB compaction
   while the writer waits for the memtable to drain. Reducing compaction
   volume (WiscKey) and overlapping I/O (io_uring) should shrink the tail.
3. **Point reads are fast and stable (C: p99 = 2.9 µs)** thanks to the
   skiplist + Bloom-filter + in-memory index read path; the read-side risk of
   WiscKey is the extra vLog lookup, which the read cache must absorb.

## Reading the harness fairly

- Numbers on a gVisor-sandboxed tmpfs measure CPU paths and engine design,
  not device bandwidth; fsync is cheaper than on real storage. Absolute
  values will differ on bare metal — ratios (before/after, A vs C) are the
  honest comparison.
- Write amp counters include the load phase; for read-dominated workloads
  (B–E) the ratio mostly describes the load, not the run mix.
