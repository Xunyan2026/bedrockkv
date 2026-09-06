# BedrockKV

A high-performance, persistent LSM-Tree based key-value storage engine written in C++20.

> 🚧 Work in progress. This README will grow with architecture diagrams and
> benchmark data as the project reaches its milestones. Roadmap: WAL +
> MemTable → SSTable + Leveled Compaction → YCSB harness → WiscKey value
> separation (done) + io_uring async I/O → Redis RESP2-compatible server →
> benchmarks vs. LevelDB.

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

## Async I/O (io_uring)

`Options::enable_io_uring` activates a hand-rolled io_uring ring (raw
syscalls + mmap, no liburing). Where the kernel supports it: WAL records
go out as explicit-offset pwrite SQEs (shared fragmentation code with the
sync path, byte-identical output) and the vLog + WAL fsyncs run as one
parallel SQE pair. Where it doesn't — this project's gVisor sandbox
returns ENOSYS — the engine opens identically on the synchronous path,
`io_uring_active()` reports false with the kernel's reason, and a control
run proves the fallback is byte-for-byte the baseline. Design analysis of
what io_uring can and cannot buy a single-writer engine:
`docs/benchmarks.md` (stage 3 batch 3 section).

## Redis-compatible server (stage 4)

The engine speaks RESP2 over TCP — redis-cli and redis-benchmark connect
out of the box:

```sh
./build/bedrockkv-server --port 7379 --dir /tmp/bedrockkv-demo
redis-cli -p 7379 set greeting hello     # +OK
redis-cli -p 7379 get greeting           # "hello"
```

Commands: `SET / GET / DEL / EXISTS / PING / ECHO`, binary-safe keys and
values, inline commands for telnet sessions, pipelining (many commands
per packet, one reply batch). Transport is a hand-written epoll
(level-triggered) + non-blocking-fd event loop: one thread accepts,
parses, executes and replies — Redis' own single-threaded model, which
also satisfies the engine's single-writer `Put` contract with zero
locks. Protocol errors close the connection with `-ERR`, exactly like
real Redis; clients that stop reading are evicted once their reply
backlog crosses 64 MiB.

Stock `redis-benchmark` drives it out of the box (same-host numbers,
gVisor sandbox): with pipelining, 278k GET and 91k SET req/s — within
85% of real Redis 7.2 on the read path. The protocol layer and WAL
replayer are coverage-guided fuzz targets (libFuzzer, 23M+ execs, zero
findings; short fuzz smokes run in CI): numbers and analysis in
`docs/benchmarks.md`.

## License

MIT (to be finalized at v1.0).
