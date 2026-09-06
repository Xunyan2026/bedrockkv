# BedrockKV Design

This document records *why* the engine is shaped the way it is: the
decision behind each module, the alternatives that were rejected, and
the bugs that taught us something non-obvious. Format specs live
separately ([sstable-format.md](sstable-format.md),
[vlog-format.md](vlog-format.md)); numbers live in
[benchmarks.md](benchmarks.md).

```
                         ┌──────────────────────────────┐
                         │   RESP2 server (epoll, 1 th) │  redis-cli /
                         │  SET GET DEL EXISTS PING ECHO│  redis-benchmark
                         └──────────────┬───────────────┘
                                        │ DB calls
  ┌─────────────────────────────────────┼─────────────────────────────────────┐
  │ DB (mutex-guarded bookkeeping, lock-free readers)                         │
  │                                     ▼                                     │
  │   writer ──► WAL ──► MemTable ──(rotate)──► imm ──(flush)──► L0 SST       │
  │                  (vLog first          │                │                   │
  │                   when separated)     │    background: L0→L1 size-tiered,  │
  │                                       │    Ln→Ln+1 leveled compaction      │
  │   reader ◄── mem ─ imm ─ L0 (newest first) ─ L1..L6 (binary search)       │
  │                            └── 21-byte pointer ──► vLog ──► LRU cache     │
  │                                                                           │
  │   MANIFEST = sole on-disk truth: SST list + live vLogs + replay floor     │
  └───────────────────────────────────────────────────────────────────────────┘
```

## Guiding constraints

1. **Zero third-party dependencies.** No asio, no liburing, no absl —
   only the C++20 standard library and POSIX. Every dependency in a
   resume project invites the question "did you build this, or glue
   it?"; removing the question removes the doubt. It also made the
   io_uring work possible at all: vendoring a ~200-line ring (raw
   syscalls + three mmaps) is feasible, wiring liburing into CMake
   FetchContent sandboxes is not.
2. **Every claim reproducible.** Benchmarks come from an in-repo YCSB
   harness with fixed seeds; every optimization lands as a before/after
   pair in docs/benchmarks.md; degradation (io_uring on gVisor) is
   reported by the engine itself, not marketing copy.
3. **Crash safety is a first-class test axis.** kill -9 mid-write,
   garbage tails, mid-record truncation, restart-with-pending-flush —
   each has a regression test that once failed.

## Storage hierarchy and key encoding

Every version of every key is addressed by an *internal key*:

```
[klen u32 LE][user_key][tag u64 LE][value]      tag = (seq << 8) | type
```

Ordering: user key ascending, **tag descending**. Three decisions hide
in this one line:

- **The length prefix keeps values out of comparisons.** The first
  draft encoded `[key][tag][value]` with a comparator that stopped at
  the value — and leaked value bytes into key ordering (a self-caught
  design flaw; caught before it ever landed in a file, but it forced a
  redesign). A prefix length makes the (key, tag) projection exact.
- **Tag descending, not ascending**, puts the newest version of a key
  first, so a point lookup is a single `Seek` instead of a scan-to-end
  of the key's version run.
- **Seq in the high 56 bits, type in the low 8** means "all versions of
  key K with seq ≤ S" is one contiguous, ordered run — the property
  snapshot reads (below) are built on. Tombstone vs value is one byte.

Sequence numbers are the global time line: assigned under the DB mutex
immediately before a write is applied, so "seq ≤ S" and "happened
before GetSnapshot() returned" are the same statement.

## WAL

A size-chunked, CRC'd log: 32 KiB blocks, records fragmented into
FIRST/MIDDLE/LAST pieces, zero-padded block tails, per-record CRC-32
(own compile-time table implementation). The Reader walks records and
reports both "corrupt at offset X" and "last good record ends at Y" —
recovery is then an exact `ftruncate(Y)`, not a conservative rewind.

Design choice worth stating: **recovery truncates to the last intact
record, and O_APPEND guarantees post-truncation writes land at the new
EOF.** The subtle bug class this avoids: reopening a WAL requires
telling the Writer its true starting offset (`lseek(fd, 0, SEEK_END)`),
because fragmentation and padding decisions depend on the position
*within the current 32 KiB block*. Passing 0 silently produced valid
-looking records that became unreadable at real block boundaries —
found only by byte-level WAL parsing after a flaky persistence test.

## MemTable

A skiplist (p = 1/4, max level 12, insert-only) templated on a
comparator, so the same structure orders WAL-encoded entries by
(user key, tag desc). The single-writer / lock-free-reader protocol
(release on publish, acquire on read) was proven under TSan in the
stage-0 build and carried through unchanged.

An entries-in-memtable size estimate (key + value + tag + ~24 B
overhead per node) drives the flush trigger at 4 MiB by default. The
estimate is deliberately rough: its only consumer decides "freeze this
memtable", not "bill memory".

## SSTable

Format: prefix-compressed data blocks (restart every 16 keys) + one
Bloom filter per block (10 bits/key, ~0.8% FP) over user keys + a
block-coded index (last internal key of each block → handle) + a 44 B
footer with the whole-file CRC and magic.

Two deliberate simplifications, both recorded in the format doc:

- **The whole file is read into memory at open** (CRC verified once).
  No fd is held; all reads are memcpy+decode. This makes Version
  lifetime trivial — an in-flight reader pins the in-memory Table, so
  compaction can unlink input files the moment the MANIFEST stops
  naming them, with no reference counting on open descriptors. The
  block-cache/pread step would slot in behind the same index→bloom→block
  pruning order.
- **The bloom filter is built over owning copies** of user keys, not
  the caller's views. An early version stored views into temporaries;
  by the time the block sealed and the filter was built, the bytes were
  gone and *every* lookup became a false negative — invisible except
  as mysteriously slow reads.

A subtle correctness detail: `smallest_user_key` is derived by decoding
the first entry of the first data block, because the index's first
entry is the *last key of the first block* — under lexicographic
ordering that can be far from the file's true smallest key (key0 vs
key10026), and L1+ binary search silently skipped files.

## Concurrency model: one writer, lock-free readers, one background thread

The leveldb shape, deliberately:

- **One writer thread** calls Put/Delete (the RESP server's single
  event loop satisfies this for free — see below).
- **Any number of readers** call Get/Scan. Each takes the mutex once to
  copy `{memtable, imm, Version}` shared_ptrs, then works entirely
  lock-free: memtable reads are the skiplist's reader protocol; SSTs
  and Versions are immutable once published.
- **One background thread** drains the immutable memtable and runs
  compaction.

**Version lifetime IS the reference-counting story.** Publishing a
flush or compaction means building a new Version and swapping one
shared_ptr; readers keep the old one alive as long as they need it.
Because tables are in-memory and immutable, "old version still being
read" never blocks file deletion.

Two lost-wakeup bugs taught the operational lesson the hard way: every
cross-thread condition needs a notify on *both* sides. A vLog GC
trigger that fires without a write (so without the usual rotation
notify), and a `wait_for_background_work()` predicate that becomes true
while the background thread is mid-step, each deadlocked exactly once
before `notify_all` was added at every predicate-touching point.

## Crash consistency: the MANIFEST is the sole truth

On-disk state = SST files + vLog files + per-generation WALs +
MANIFEST. The MANIFEST (atomically rewritten: tmp file + rename + dir
fsync) names every live SST, every live vLog generation, and the
**current log generation as a replay floor**. Recovery replays every
log generation ≥ the floor, then deletes nothing above the floor and
removes only true orphans.

Why a *floor* and not "the current log": a shutdown or crash can leave
a memtable pending flush, its records in a retired log the MANIFEST
does not yet name. Deleting "logs the MANIFEST doesn't name" lost 33
keys in one memorable test run; replaying everything ≥ floor loses
nothing and only ever replays idempotent duplicates.

The orderings around the MANIFEST are the crash story, and each is
unit-tested with kill -9:

- publish new log file (created + fsynced) → name it → *then* retire
  the old one (a MANIFEST pointing at a log that doesn't exist yet
  cannot be survived);
- flush: install in memory → MANIFEST → unlink retired log. Crash
  between the last two: orphans, removed on next open;
- vLog GC: durability barrier → unpublish old generations in MANIFEST →
  unlink. Crash either side leaves a consistent world.

One negative guard deserves its paragraph: opening a database that has
vLog files with separation *disabled* is refused, because the LSM is
full of pointers nothing can resolve. The first version checked the
MANIFEST's vLog list — which misses a database that wrote values but
never flushed (the MANIFEST was never rewritten), and the orphan
cleanup then *deleted the only copy of the data*. The fix scans the
disk for `.vlog` files before any cleanup, and the regression test
wrote itself the same day: **every new rejection path must immediately
be probed by a test that tries to defeat it.**

## Compaction

- **L0 → L1 size-tiered**: at 4 L0 files, all of L0 merges into the
  overlapping range of L1. L0 files overlap, so they must merge
  together to produce disjoint output.
- **Ln → Ln+1 leveled**: one victim file plus *all* overlapping
  next-level files, the overlap expanded by the same fixpoint leveldb
  uses (adding a file can widen the range, which can pull in more
  files) — guaranteeing outputs stay disjoint from surviving
  next-level files, which is what keeps "at most one file per level
  can contain a key" true for the binary-search read path.
- Output splits at user-key boundaries only, never inside a key's
  version run.
- **Tombstones survive every level except the bottom.** A tombstone
  dropped at L3 would let L4..L6 values resurrect. At the bottom level
  nothing lies deeper, so it (and everything it shadows) can finally
  go.

Per user key, the merge keeps the newest version and drops the rest —
*except* when snapshots exist (next section).

## MVCC snapshots

`GetSnapshot()` returns an opaque handle pinned to a sequence number S;
`Get`/`Scan` overloads read "newest version with seq ≤ S". Nothing new
is stored — snapshots are a read-side interpretation of the existing
ordering:

- **Visibility**: seek to internal key `(key, tag=(S<<8)|0xFF)`. The
  first entry at-or-below is the newest version S can see (kMaxSeq
  overflows to the plain newest-version seek, so latest reads share the
  code path unchanged). Compaction, flush, memtable — all use the same
  arithmetic, because all versions of a key sort together.
- **Snapshot-aware compaction retention** (the leveldb rule): per user
  key, newest first — keep everything with seq > F (F = smallest live
  snapshot seq; some snapshot may read it), keep the *first* version
  with seq ≤ F (what every snapshot at-or-below F sees), drop the rest.
  With no snapshots, F = UINT64_MAX and the rule collapses to
  keep-only-newest: the pre-snapshot behavior is a special case, not a
  branch.
- **Scan** walks the merge in internal-key order and, per user key,
  skips versions above S and emits the first visible one. Latest reads
  take the first occurrence, exactly as before.

The expensive lesson of this feature is its interaction with **vLog
GC**. GC's liveness check reads *latest* state, between mutex holds,
while scanning a generation it intends to unlink. A snapshot created
mid-pass can need exactly the entry the scanner just classified dead:
its pointer was current when the snapshot captured S, a later write
made the entry look dead, and rewriting cannot help — the rewrite gets
a fresh sequence the snapshot cannot see. The fix is not a smarter
check but a **decision point moved next to the irreversible action**:
immediately before retiring old generations, under the same mutex
`GetSnapshot` holds, the GC aborts the retirement if any snapshot is
live (earlier checks — the trigger, pass start — had already proven
insufficient). A snapshot created *and released* mid-pass is safe
either way: it could only read while the old files were still intact.
While snapshots live, GC is simply deferred — a documented trade-off
that costs reclamation, never correctness.

Snapshots are in-memory only: after a restart every read is a latest
read and compaction reclaims what they pinned.

## WiscKey value separation and vLog GC

With `enable_value_separation`, values ≥ threshold (default 1 KiB)
bypass the LSM: appended to an append-only value log as
`[crc][klen][vsize][key][value]`, with the LSM slot holding a 21-byte
pointer `[0xFF tag][vlog# u64][offset u64][size u32]`. Pointers are
opaque below the DB layer — WAL records, memtables, SSTs and
compactions move them like any value. The vLog is written *before* the
WAL in every write, so a surviving WAL record can never reference bytes
that were never appended.

The measured effect (docs/benchmarks.md): YCSB-A write amplification
4.88x → 2.26x, compactions per run 37 → 0, max latency 580 ms → 40 ms.
The honest cost: scan-heavy E dropped 114k → 18.5k ops/s, because every
scanned record now resolves a pointer through the LRU read cache into
a pread.

GC is a full-rewrite: rotate to a fresh generation (published in the
MANIFEST *before* anything references it), scan every older
generation, re-append live entries through the normal write path, then
unpublish + unlink the old ones. The three races worth writing down:

- **Liveness check and rewrite hold the mutex together**, so a
  concurrent overwrite can never slip between "this entry is the key's
  current value" and writing it back (which would resurrect a stale
  value over a fresh user write).
- The **anti-livelock floor**: a pass that finds the file mostly live
  sets `floor = 2×rewritten`, so GC re-triggers only after enough new
  garbage accumulates — otherwise a live data volume above the trigger
  is rewritten forever.
- The **snapshot retirement guard** above.

The pointer/inline ambiguity is closed on both paths: while separation
is enabled, a 21-byte `0xFF`-led value is separated into the vLog even
below the threshold, so an inline slot is always unambiguously a real
value or a real pointer; with separation disabled the read path never
decodes pointers at all (details in the format spec; RocksDB's
per-value metadata byte remains the production-grade scheme).

## io_uring

`Options::enable_io_uring` opens a hand-rolled ring (raw syscalls 425 /
426 + three mmaps, no liburing). Where supported: WAL writes go out as
explicit-offset pwrite SQEs (no O_APPEND on the fd then — the kernel
ignores explicit offsets on append fds and *reorders* batched writes;
found only on the CI runner's real kernel, never locally), and the
vLog+WAL fsync pair is submitted as parallel SQEs — one round trip
instead of two, which is the real win on storage where fsync costs a
millisecond.

Where unsupported (this project's gVisor sandbox returns ENOSYS), the
engine opens identically on the synchronous path,
`io_uring_active()` reports false with the kernel's reason verbatim,
and a control run proves the fallback byte-identical to the baseline.
A unit test passes in both worlds by construction.

The design analysis in benchmarks.md is the honest part: a
single-writer engine with synchronous Put cannot benefit from write
batching (the batching horizon is one record, and deferring
submissions would break the WAL contract). What the ring buys: the
fsync pair, scan-range pread batching, and multi-file compaction reads
— the first of which is wired, the rest queued.

The bug that took a week off our life: io_uring's `sq_off.*` /
`cq_off.*` struct fields are **byte offsets into the shared mappings**,
not the values of the indices/masks. Reading the mask field's *offset*
as the mask collapsed every ring index to slot 0 — single operations
all passed, batched writes overwrote each other, and the sandbox's
ENOSYS meant it could only be debugged on CI, via self-contained
diagnostic tests that hexdumped the ring memory.

## RESP2 server

A hand-written epoll (level-triggered) + non-blocking-fd + eventfd
event loop, one thread: accept, parse, execute, reply. This is Redis'
own model, and it satisfies the engine's single-writer contract with
zero locks. Commands: SET/GET/DEL/EXISTS/PING/ECHO, binary-safe,
pipelined (many commands per packet, one reply batch), inline commands
for telnet, protocol errors answered `-ERR` and connection closed like
real Redis, slow clients evicted at a 64 MiB reply backlog.

The parser is incremental (any TCP fragmentation pattern is legal) and
bounds everything before allocating (length limits first). Its most
instructive bug was an interface-semantics conflict: arguments were
initially zero-copy views into the parse buffer, but the buffer is
compacted (prefix erased) after every completed command to keep long
connections from growing — so views read overwritten bytes
(`"SET"` → `"\0ET"`). Random-split feeding and pipelined tests caught
it immediately; the fix copies arguments by value (~3 bytes per
command). **Zero-copy and buffer compaction are mutually exclusive;
pick one or defer materialization with offsets.**

## Fuzzing and testing posture

- **106 gtest tests** across three builds (Release, ASan/UBSan, TSan),
  `-Wall -Wextra -Wpedantic -Werror`, zero warnings. Concurrency tests
  re-run under TSan repeatedly (flaky interleave hunting).
- **Model tests**: engine vs `std::map` shadow over randomized op
  streams that deliberately cross flush and compaction boundaries;
  the snapshot model test freezes a shadow per snapshot.
- **Crash tests**: kill -9 in forked children at WAL-write boundaries,
  garbage tails, mid-record cuts, restart-with-pending-flush.
- **Three libFuzzer harnesses** (RESP parser, WAL replay — invariant:
  truncate at last-good-end ⇒ clean replay — and the full
  parser→dispatch→DB path); 60-second smokes per target in CI,
  26M+ execs locally, zero findings.

## Known limitations (audited, deliberately deferred)

Recorded so they are findings, not surprises: the replay floor can
still skip a log holding an unflushed immutable memtable (leveldb
shares the structural flaw); vLog GC with a mid-pass snapshot reader
is guarded by deferral, not by a graveyard; GC retries on failure
without backoff; the memtable grows unboundedly during a GC pass;
`last_error_` never propagates to Put's caller; a 21-byte `0xFF`-led
value collides with the pointer encoding. The next architectural
steps, in value order: async flush to kill the 40 ms rotation stall,
group commit (which would also unlock io_uring write batching), and a
block cache with pread-backed tables to bound memory.
