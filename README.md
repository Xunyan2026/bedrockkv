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

## License

MIT (to be finalized at v1.0).
