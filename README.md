# BedrockKV

**A persistent LSM-Tree key-value storage engine in C++20 — WAL, leveled
compaction, WiscKey value separation, io_uring, MVCC snapshots, and a
Redis-compatible wire server. Zero third-party dependencies.**

[![CI](https://github.com/Xunyan2026/bedrockkv/actions/workflows/ci.yml/badge.svg)](https://github.com/Xunyan2026/bedrockkv/actions/workflows/ci.yml)

Built from scratch: the WAL format, skiplist, SSTable layout, Bloom
filters, compaction picker, MVCC visibility rules, the io_uring ring
(raw syscalls), and the RESP2 protocol parser are all hand-written —
the only dependency is GoogleTest, for the 102 tests that pin it all
down (Release + ASan/UBSan + TSan, plus libFuzzer targets in CI).

## The headline numbers

| What | Result | How |
|---|---|---|
| Write amplification (YCSB-A, 1 KiB values) | **4.88x → 2.26x** | WiscKey value separation: the LSM moves 21-byte pointers, values append once to a vLog |
| Tail latency (YCSB-A max) | **580 ms → 40 ms** | Compaction volume collapses (37 → 0 compactions/run) — nothing left to stall the writer |
| redis-benchmark GET, pipeline=16 | **278k req/s = 87% of real Redis 7.2**, same host | RESP2 over a hand-written epoll event loop; read path is memory-only |
| libFuzzer, 3 targets (RESP / WAL / full server) | **26M+ execs, zero findings** | Invariant-driven harnesses; WAL recovery invariant fuzzed: truncate at last-good-end ⇒ clean replay |

Details, methodology, and honest caveats (gVisor sandbox, tmpfs fsync,
the scan tax value separation pays): [docs/benchmarks.md](docs/benchmarks.md).

## 30-second demo

```console
$ ./build/bedrockkv-server --port 7379 --dir /tmp/bedrockkv-demo --sync always
bedrockkv-server listening on port 7379 (dir: /tmp/bedrockkv-demo)

$ redis-cli -p 7379 set greeting "hello, bedrockkv"
OK
$ redis-cli -p 7379 get greeting
"hello, bedrockkv"
$ redis-cli -p 7379 del greeting
(integer) 1
$ redis-cli -p 7379 ping
PONG
^C   # kill the server — restart on the same dir

$ ./build/bedrockkv-server --port 7379 --dir /tmp/bedrockkv-demo --sync always
$ redis-cli -p 7379 get greeting          # survived the restart
"hello, bedrockkv"
```

## Architecture

```mermaid
flowchart TB
    subgraph clients [" "]
        RC[redis-cli / redis-benchmark]
    end
    subgraph server ["bedrockkv-server — 1 event-loop thread (epoll LT)"]
        RESP["RESP2 parser<br/>SET GET DEL EXISTS PING ECHO"]
    end
    subgraph engine ["DB — 1 writer + N lock-free readers + 1 background thread"]
        direction TB
        W[Put / Delete] --> WAL[WAL<br/>32 KiB chunks · CRC · torn-tail truncation]
        WAL --> MEM[MemTable<br/>skiplist · tag = seq&#8858;8 + type]
        MEM -- "rotate at 4 MiB" --> IMM[immutable memtable]
        IMM -- "background flush" --> L0[L0 SSTs<br/>(overlapping, newest first)]
        L0 -- "size-tiered" --> L1[L1..L6 SSTs<br/>leveled compaction]
        VLOG[vLog<br/>append-only values ≥ 1 KiB<br/>+ full-rewrite GC]
        MAN[("MANIFEST<br/>SST list · live vLogs · replay floor")]
        SNAP["MVCC snapshots<br/>read at any past sequence"]
    end
    RC --> RESP --> W
    RESP --> R[Get / Scan] --> MEM & L0 & L1
    R -- "21-byte pointer" --> VLOG
```

- **Internal key** `[klen][user_key][tag=(seq<<8)|type][value]`, sorted
  by user key then tag *descending* — the newest version of a key is
  one `Seek` away, and "everything visible at snapshot S" is one
  contiguous run.
- **Crash safety**: the MANIFEST is the sole on-disk truth, atomically
  rewritten (tmp + rename + dir fsync); recovery replays every log
  generation ≥ the published floor and truncates a torn WAL tail at the
  last intact record. kill -9 tested at every ordering boundary.
- **Snapshots**: `GetSnapshot()` pins a sequence; reads through it see
  exactly the writes up to that point across flushes and compactions
  (leveldb's retention rule); vLog GC defers while snapshots live.
- **Value separation**: compaction traffic drops from "every byte ever
  written, several times" to "one pointer per key" (format + crash
  story: [docs/vlog-format.md](docs/vlog-format.md)).
- **io_uring**: a vendored ~200-line ring (raw syscalls + mmap). Where
  the kernel lacks it (gVisor: ENOSYS), the engine degrades honestly —
  `io_uring_active() == false` with the reason, control run byte-identical.

## Build & test

```bash
git clone git@github.com:Xunyan2026/bedrockkv.git
cd bedrockkv
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure      # 102 tests
```

Requirements: Linux, CMake ≥ 3.16, a C++20 compiler (GCC ≥ 11 /
Clang ≥ 13). No other dependencies.

Run the YCSB harness (reproduces every number in docs/benchmarks.md):

```bash
./build/ycsb_bench --workload all --recordcount 100000 \
    --operationcount 100000 --value_size 1024 [--vsep] [--io_uring]
```

## Documentation

| Doc | Contents |
|---|---|
| [docs/design.md](docs/design.md) | **Why it's shaped this way** — each module's decision, rejected alternatives, and the bugs that taught something |
| [docs/benchmarks.md](docs/benchmarks.md) | Every number with its reproducible command; before/after for each optimization; redis-benchmark vs Redis 7.2; fuzzing |
| [docs/sstable-format.md](docs/sstable-format.md) | On-disk SSTable layout spec |
| [docs/vlog-format.md](docs/vlog-format.md) | vLog format, GC algorithm, crash story, known limitations |

## Quality posture

- **102 gtest tests** across three sanitizer builds (ASan/UBSan, TSan),
  `-Werror`, zero warnings; randomized model tests against `std::map`
  shadows that deliberately cross flush/compaction boundaries; crash
  tests with real `kill -9`.
- **libFuzzer targets** for the RESP parser, WAL replay, and the full
  server path — CI runs a 60-second smoke of each on every push.
- **Honest benchmarking**: fixed-seed YCSB harness in-repo; known
  sandbox artifacts (tmpfs fsync, gVisor loopback RTT) called out
  rather than hidden; control runs proving opt-in paths change nothing
  when disabled.

## Roadmap

Done: WAL + MemTable → SSTable + leveled compaction → YCSB harness →
WiscKey value separation + vLog GC → io_uring → RESP2 server →
MVCC snapshots. Next: async flush (kill the 40 ms rotation stall),
group commit (unlocks io_uring write batching), block cache +
pread-backed tables.

## License

MIT (finalized at v1.0).
