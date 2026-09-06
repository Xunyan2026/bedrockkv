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
