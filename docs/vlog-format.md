# vLog format (WiscKey value separation) — spec v1

When `Options::enable_value_separation` is on, values >= `value_separation_threshold`
bytes stop flowing through the LSM. The LSM (WAL, MemTable, SST, compaction)
carries only a fixed-size **value pointer**; the bytes themselves live in
append-only **value log** files. Compaction rewrites pointers (21 bytes) and
never touches value bytes — that is the entire write-amplification win.

## Value pointer (what the LSM stores as "the value")

    [tag u8 = 0xFF][vlog_number u64 LE][offset u64 LE][value_size u32 LE]   (21 B)

`offset` points at the vLog entry start. The 0xFF tag makes a pointer
recognizable: a slot is a pointer iff it is exactly 21 bytes and starts
with 0xFF. Known limitation (documented, accepted for this project): a
user value that is itself 21 bytes and starts with 0xFF would be
misresolved while separation is enabled. The Open-time clamp of the
threshold to >= 64 keeps *newly written* small values from being
separated, but cannot distinguish such a value in already-stored data.
RocksDB's approach (a per-value metadata byte) costs a byte on every
inline value and is the production-grade fix if this ever matters. Pointers are opaque bytes everywhere
below the DB layer — memtables, SSTs, iterators, compaction treat them like
any other value and must never rewrite them.

## Value log file `NNNNNN.vlog`

    entry: [crc32 u32 LE][klen u32 LE][vsize u32 LE][key][value]

    crc covers klen + key + vsize + value (everything after the crc field).
    vsize == the size in the pointer (checked on every read).

- Appends only, generation-numbered like WAL logs; `DB` owns the fsync policy.
- **Write order per record: vLog append first, then the WAL record that
  references it; fsync order: vLog, then WAL.** A durable WAL record can
  therefore never reference vLog bytes that were not already durable.
- Orphan entries (vLog bytes whose key never reached the LSM, e.g. crash
  between the two appends) are harmless and are reclaimed by GC.
- A torn tail (crash mid-append) fails its CRC on read; the engine reports
  such reads as missing values — the same bounded-loss contract as the WAL's
  sync mode. The tail is not truncated on Open; GC reclaims it.

## GC (simplified, threshold-triggered full rewrite)

Triggered by the background thread when the current vLog exceeds
`Options::vlog_gc_size`:

1. Rotate to a fresh vLog generation (all new appends — GC rewrites and
   concurrent user writes — land there).
2. Scan the old file up to its size at GC start: for each CRC-valid entry,
   under the mutex, check whether the LSM's current pointer for that key
   still selects exactly this entry; if yes, re-Put `(key, value)` through
   the normal write path (which re-separates into the new generation).
   The mutex hold across check + write makes the liveness decision atomic
   against concurrent user overwrites.
3. Rewrite the memtable whenever it fills (background thread flushes it
   inline, so user writers keep flowing).
4. Drop the old generation from the MANIFEST's vLog list, rewrite the
   MANIFEST, then unlink the old file.

Crash at any point: the MANIFEST names every generation that live pointers
may still reference, so recovery replays correctly; a crashed GC leaves
garbage that the next GC pass reclaims.

Scheduling contract (this bit bit us once): with separation on, memtable
entries shrink to ~40 bytes, so user rotations — the engine's usual
background wakeup source — become rare. A write that pushes the vLog past
the GC trigger must itself wake the background thread, and
`wait_for_background_work()` must notify before waiting; otherwise the
trigger is crossed with the background thread asleep and no future event
ever evaluates the predicate, and every caller of
`wait_for_background_work()` blocks forever. Condition-variable protocols
need a notify on BOTH sides of every cross-thread dependency.

## Known limitations (audited, deliberately deferred)

Found during a full-codebase bug sweep; each is real but bounded, and the
fix for the first two changes core recovery semantics, so they are
documented rather than patched on a quiet weekend:

1. **Replay floor vs. pending immutable memtable.** Compaction/GC step 3
   can publish a MANIFEST whose log number (replay floor) advances past a
   log generation that still holds the records of an immutable memtable
   not yet flushed (leveldb avoids this with an explicit
   `min(log_number_, imm_log_number_)` floor). Window: a crash between
   that MANIFEST write and the flush of the pending immutable memtable
   loses its records. Same structural tradeoff leveldb made; fixing it
   means threading `imm_log_number_` through every MANIFEST publication.
2. **GC vs. pre-GC snapshot readers.** A `Scan`/iterator that copied its
   version before a GC pass can hit a pointer into a generation the GC
   has already unpublished; the read then reports Corruption instead of
   the old value (the memtable-liveness check above makes *user
   overwrites* safe; *GC rewrites* of the very entry being read are the
   residual race). A graveyard list of retired generations until no
   reader can hold them closes this.
3. **Failed GC pass busy-retries.** A GC that fails early (e.g. a vLog
   open error) re-enters every ~200 ms background wakeup and rotates an
   empty generation each time, until it succeeds.
4. **Memtable growth during GC.** While the background thread runs GC
   (which never drains the memtable), an active writer can grow the
   memtable past `write_buffer_size`; it flushes when the GC finishes.
5. **Background errors are recorded, not raised.** A failed flush or
   compaction sets `last_error_`/disables compaction but `Put` keeps
   succeeding (WAL is still durable; reads go stale). Surfacing this as
   a `Put` error, leveldb-style, is a one-line API decision away.
6. **`RotateForFlush` leaves a stale `wal_ring_error_`.** The error is
   surfaced at the next durability point, so nothing is lost — but the
   rotation itself does not clear it.
