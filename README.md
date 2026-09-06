# BedrockKV

A high-performance, persistent LSM-Tree based key-value storage engine written in C++20.

> 🚧 Work in progress. This README will grow with architecture diagrams and
> benchmark data as the project reaches its milestones. Roadmap: WAL +
> MemTable → SSTable + Leveled Compaction → WiscKey value separation + io_uring
> → Redis RESP2-compatible server → benchmarks vs. LevelDB.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requirements: Linux, CMake ≥ 3.16, a C++20 compiler (GCC ≥ 11 / Clang ≥ 13).

## Value separation (WiscKey)

`Options::enable_value_separation` moves values ≥
`value_separation_threshold` (default 1 KiB) out of the LSM tree into an
append-only value log: the LSM stores a fixed 21-byte pointer, compactions
rewrite pointers instead of payload, and a threshold-triggered full-rewrite
GC reclaims garbage. On YCSB-A this cuts write amplification 4.88x → 2.26x
and tail latency 572 ms → 40 ms (numbers and method:
`docs/benchmarks.md`; on-disk format and crash story:
`docs/vlog-format.md`). Known simplification: a user value that happens to
be exactly 21 bytes starting with `0xFF` is indistinguishable from a
pointer — documented in the format spec, removable with a metadata byte.

## License

MIT (to be finalized at v1.0).
