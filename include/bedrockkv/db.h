// BedrockKV — DB: the stage-2 complete LSM: WAL + MemTable + leveled
// SSTs with background compaction, Scan, and iterators.
//
// Concurrency contract (leveldb-style, our first threaded stage):
//   * ONE writer thread calls Put/Delete (enforced by the caller);
//   * any number of readers may call Get/Scan concurrently;
//   * ONE background thread performs flushes and compactions.
// Synchronization: a single mutex guards the mutable bookkeeping
// (memtable rotation, version install, sequence counter, log writer).
// Readers hold the mutex only to copy a snapshot — {memtable, immutable
// memtable, Version} — and then work lock-free:
//   * MemTable::Get is the skiplist's lock-free reader protocol;
//   * SSTables are immutable; a Version is immutable once published.
//
// Version lifetime IS the "version reference counting" story: readers
// copy shared_ptrs, so a compaction can publish a new Version and drop
// the old one while readers are still inside it. Because a Table has
// already read its file fully into memory at Open (no fd held), input
// files can be unlinked as soon as the MANIFEST no longer names them —
// in-flight readers depend on the in-memory Table object, not the file.
//
// Flush: when the memtable crosses Options::write_buffer_size, Put
// rotates to a NEW log generation and moves the old memtable to imm_
// (under the mutex), then the background thread exports imm_ into an
// L0 SST, atomically rewrites the MANIFEST (tmp + rename + dir fsync)
// and deletes the retired log. Put waits for the previous flush to
// drain, so at most one immutable memtable exists.
//
// Compaction (background, one step at a time):
//   L0 -> L1  when L0 has l0_compaction_trigger files (size-tiered:
//             all L0 files are inputs);
//   Ln -> Ln+1 when level Ln exceeds level_base_size * 10^(n-1)
//             (leveled: one victim file + all overlapping next-level
//             files, chosen by the same range-expansion fixpoint
//             leveldb uses so output ranges stay disjoint);
// merge rules: per user key keep only the newest version (no snapshot
// reads yet); tombstones are dropped only when compacting INTO the
// bottom level — elsewhere they must survive to shadow older levels.
//
// Crash consistency is unchanged from the single-log design: the
// MANIFEST is the sole source of truth (SST list + current log
// generation), published atomically. Its log number is a REPLAY FLOOR:
// recovery replays every log generation >= the floor (a shutdown/crash
// can leave a memtable pending flush, with its records in a retired log
// the MANIFEST does not yet name — that log must survive and be
// replayed). A generation is deleted only once the floor moves past it.
//
// WiscKey value separation (stage 3): with
// Options::enable_value_separation, values >= value_separation_threshold
// bypass the LSM entirely — they are appended to a value log (VLog) and
// the LSM carries a 21-byte pointer in the value slot. Pointers are
// opaque below the DB layer: WAL records, memtables, SSTs and
// compaction move them like any other value and never rewrite them, so
// compaction traffic shrinks from "every byte ever written, several
// times" to "one pointer per key". The DB layer resolves pointers on
// Get/Scan through a sharded LRU read cache, and a threshold-triggered
// background GC rewrites live entries into a fresh vLog generation
// (docs/vlog-format.md for the format and crash story). The MANIFEST
// additionally names every live vLog generation, because until GC
// finishes, pointers may reference the old one.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bedrockkv/iterator.h"
#include "bedrockkv/log.h"
#include "bedrockkv/lru_cache.h"
#include "bedrockkv/memtable.h"
#include "bedrockkv/status.h"
#include "bedrockkv/sstable.h"
#include "bedrockkv/vlog.h"

namespace bedrockkv {

// Durability policy for WAL writes.
//   kSyncAlways   — fsync after every write: acknowledged writes survive
//                   even a power cut. Safest, slowest.
//   kSyncPeriodic — fsync once ~1 MiB has accumulated: bounded loss on
//                   power cut, much faster. (kill -9 loses nothing either
//                   way: the OS page cache survives process death.)
//   kSyncNever    — let the OS decide: fastest, for benchmarks/crash tests.
enum class SyncMode { kSyncAlways, kSyncPeriodic, kSyncNever };

struct Options {
  SyncMode sync_mode = SyncMode::kSyncAlways;
  // Flush the MemTable into a new L0 SSTable once its approximate size
  // crosses this. 4 MiB mirrors leveldb's default.
  size_t write_buffer_size = 4u << 20;
  // Compaction tuning (leveldb names).
  size_t l0_compaction_trigger = 4;    // L0 files before L0 -> L1
  size_t level_base_size = 10u << 20;  // L1 target; Ln = base * 10^(n-1)
  size_t max_sst_size = 4u << 20;      // compaction output file split size
  // WiscKey value separation (stage 3). When enabled, values at least
  // value_separation_threshold bytes go to the vLog; smaller values stay
  // inline in the LSM (the pointer is 21 bytes — separating tiny values
  // would cost more than it saves). vlog_gc_size triggers a full-rewrite
  // GC when the current vLog generation crosses it. Note: once a database
  // has separated values, keep the switch on — pointers left in the LSM
  // cannot be resolved with separation disabled.
  bool enable_value_separation = false;
  size_t value_separation_threshold = 1024;
  size_t vlog_gc_size = 64u << 20;
};

// L0..L6. levels_[i] below holds L(i+1); L0 is separate because its
// files overlap and carry their own read order.
constexpr size_t kMaxLevels = 7;

// One immutable SST as seen by a Version: the opened table plus the
// metadata the MANIFEST needs. Copying a TableRef copies ownership of
// the underlying table — that copy IS the reader's version pin.
struct TableRef {
  std::shared_ptr<sst::Table> table;
  sst::FileMeta meta;
};

// An immutable set of levels. Publishing a compaction/flush result means
// building a new Version and swapping the shared_ptr.
struct Version {
  std::vector<TableRef> l0;                    // newest file first
  std::vector<std::vector<TableRef>> levels;   // [i] = L(i+1), key-sorted
};

class DB {
 public:
  static constexpr const char* kManifestFileName = "MANIFEST";
  static constexpr const char* kManifestTmpFileName = "MANIFEST.tmp";

  static std::string SstFileName(uint64_t number);  // e.g. 000003.sst
  static std::string LogFileName(uint64_t number);  // e.g. 000003.log
  static std::string VlogFileName(uint64_t number);  // e.g. 000003.vlog

  // Opens (creating if necessary) a database rooted at `dir`. Returns
  // nullptr on failure with *status (if provided) describing the error.
  // Recovery is part of Open: orphan files are removed, the current
  // log's torn tail is truncated (reported via
  // wal_truncated_on_recovery()).
  static std::unique_ptr<DB> Open(const std::string& dir,
                                  const Options& options = Options{},
                                  Status* status = nullptr);

  ~DB();

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  Status Put(std::string_view key, std::string_view value);
  Status Delete(std::string_view key);
  // kNotFound when the key is absent OR deleted (tombstone).
  Status Get(std::string_view key, std::string* value) const;
  // Invokes fn(user_key, value) for every live key in [begin, end),
  // newest version first, tombstones skipped. fn receives views that are
  // valid for the duration of the call.
  Status Scan(std::string_view begin, std::string_view end,
              const std::function<void(std::string_view, std::string_view)>& fn)
      const;

  bool wal_truncated_on_recovery() const { return wal_truncated_; }
  uint64_t latest_seq() const { return next_seq_ - 1; }
  uint64_t log_number() const { return log_number_; }
  // Files per level (0 = L0) and in total — observability/tests.
  size_t level_file_count(size_t level) const;
  size_t total_file_count() const;
  // Blocks until the background thread has drained the immutable
  // memtable and satisfied all compaction triggers (tests/benchmarks).
  void wait_for_background_work();

  // ---- benchmark instrumentation (relaxed atomics; diagnostics only) ----
  // Write amplification = (wal_bytes_written + sst_bytes_written) /
  // user_bytes_written: every logical byte the user asked us to persist
  // vs. every physical byte we sent to the filesystem for it. The split
  // (WAL vs SST) makes flush/compaction contributions visible separately.
  uint64_t wal_bytes_written() const { return stats_wal_bytes_.load(std::memory_order_relaxed); }
  uint64_t sst_bytes_written() const { return stats_sst_bytes_.load(std::memory_order_relaxed); }
  uint64_t vlog_bytes_written() const { return stats_vlog_bytes_.load(std::memory_order_relaxed); }
  uint64_t user_bytes_written() const { return stats_user_bytes_.load(std::memory_order_relaxed); }
  uint64_t flush_count() const { return stats_flushes_.load(std::memory_order_relaxed); }
  uint64_t compaction_count() const { return stats_compactions_.load(std::memory_order_relaxed); }
  uint64_t vlog_gc_count() const { return stats_vlog_gcs_.load(std::memory_order_relaxed); }

 private:
  DB();

  Status WriteEntry(uint8_t type, std::string_view key, std::string_view value,
                    bool count_user_bytes = true);  // mutex held
  Status MaybeSync();                         // mutex held
  Status RotateForFlush();                    // mutex held
  void BackgroundLoop();
  Status FlushImmMemTable(const std::shared_ptr<MemTable>& imm,
                          uint64_t retired_log, uint64_t sst_number);
  bool CompactionNeeded() const;  // mutex held
  size_t LevelSize(size_t level_index) const;  // mutex held
  void PickCompaction(std::vector<TableRef>* inputs_a,
                      std::vector<TableRef>* inputs_b,
                      size_t* source_level_index,
                      size_t* output_level_index) const;  // mutex held
  Status RunCompaction(std::vector<TableRef> inputs_a,
                       std::vector<TableRef> inputs_b,
                       size_t source_level_index,
                       size_t output_level_index);
  Status WriteManifest();  // mutex held
  void RemoveOrphanFiles(const std::vector<sst::FileMeta>& files,
                         uint64_t log_floor,
                         const std::set<uint64_t>& live_vlogs) const;

  // ---- value separation internals ----
  // The read-path traversal shared by Get and by the GC's liveness check.
  // No locking: callers either hold the mutex (GC) or pass snapshot
  // copies they took under it (Get). `stored` receives the raw slot
  // contents: an inline value, a 21-byte value pointer, or empty.
  MemTable::Lookup LookupIn(const std::shared_ptr<MemTable>& mem,
                            const std::shared_ptr<MemTable>& imm,
                            const std::shared_ptr<Version>& v,
                            std::string_view key, std::string* stored,
                            Status* error) const;
  // Replaces a 21-byte value pointer in *value with the bytes it selects
  // (via the LRU cache + vLog pread). Inline values pass through.
  Status MaybeResolvePointer(std::string* value) const;
  // Copies the shared_ptr for `number` out of vlogs_ under the mutex so
  // the caller can pread lock-free; nullptr if the generation is gone.
  std::shared_ptr<VLog> VLogFor(uint64_t number) const;
  bool VlogGcNeeded() const;  // mutex held
  Status RunVlogGC();         // background thread, called without the lock

  std::string SstPath(uint64_t number) const { return dir_ + "/" + SstFileName(number); }
  std::string LogPath(uint64_t number) const { return dir_ + "/" + LogFileName(number); }
  std::string VlogPath(uint64_t number) const { return dir_ + "/" + VlogFileName(number); }
  std::string ManifestPath() const { return dir_ + "/" + kManifestFileName; }

  std::string dir_;
  int log_fd_ = -1;
  std::unique_ptr<log::Writer> log_writer_;
  std::shared_ptr<MemTable> mem_;
  std::shared_ptr<MemTable> imm_;       // draining to L0 in the background
  uint64_t imm_log_number_ = 0;         // log holding imm_'s records
  uint64_t imm_sst_number_ = 0;         // pre-allocated output file number
  std::shared_ptr<Version> current_;

  uint64_t next_seq_ = 1;
  SyncMode sync_mode_ = SyncMode::kSyncAlways;
  Options options_;
  uint64_t unsynced_bytes_ = 0;
  bool wal_truncated_ = false;

  uint64_t next_file_number_ = 1;
  uint64_t log_number_ = 0;

  // WiscKey value-separation state (mutex_-guarded unless noted).
  bool vsep_enabled_ = false;
  size_t vsep_threshold_ = 0;   // clamped to >= 64 at Open (pointer collision)
  size_t vlog_gc_size_ = 0;
  std::map<uint64_t, std::shared_ptr<VLog>> vlogs_;  // every live generation
  std::shared_ptr<VLog> vlog_current_;               // append target, in vlogs_
  // GC-trigger floor, measured by the previous pass: twice the bytes it
  // had to rewrite (i.e. the live data volume it saw). The first GC
  // triggers on file size alone; afterwards the floor keeps GC from
  // rewriting mostly-live files forever (see VlogGcNeeded).
  uint64_t vlog_gc_floor_ = 0;
  // True while the background thread is inside RunVlogGC. The last step
  // of a pass first unpublishes the old generations and only then bumps
  // the GC counter, so between those points VlogGcNeeded() is false
  // while the GC is still finishing — wait_for_background_work would
  // return early and observers could see a stale gc count / old file.
  // The flag closes that window (mutex-held, like everything it guards).
  bool vlog_gc_active_ = false;
  // Read cache for separated values: key = encoded 21-byte pointer,
  // weight = value bytes. Sharded + internally locked, so Get/Scan may
  // touch it after releasing mutex_ — hence mutable under const methods.
  mutable ShardedLruCache<std::string, std::string, std::hash<std::string>>
      vlog_cache_;

  // Diagnostics counters (see the accessors above). Relaxed is enough:
  // they are observability, never read back to make decisions.
  std::atomic<uint64_t> stats_wal_bytes_{0};
  std::atomic<uint64_t> stats_sst_bytes_{0};
  std::atomic<uint64_t> stats_vlog_bytes_{0};
  std::atomic<uint64_t> stats_user_bytes_{0};
  std::atomic<uint64_t> stats_flushes_{0};
  std::atomic<uint64_t> stats_compactions_{0};
  std::atomic<uint64_t> stats_vlog_gcs_{0};

  // Background machinery.
  mutable std::mutex mutex_;
  std::condition_variable signal_;  // wakes bg; Put waits on it too
  std::thread bg_;
  bool exit_ = false;
  bool compaction_disabled_ = false;  // set after a compaction error
  Status last_error_;
};

}  // namespace bedrockkv
