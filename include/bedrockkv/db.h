// BedrockKV — DB: the stage-2 engine: WAL + MemTable + L0 SSTables.
//
// Write path: encode (seq, type, key, value) → append one CRC-protected
// record to the CURRENT log file → apply into the MemTable. When the
// MemTable crosses Options::write_buffer_size, it is flushed: its entries
// are exported in order into an immutable SSTable file, the MANIFEST is
// atomically rewritten (tmp + rename + dir fsync) to name the SST and the
// NEW log generation, and the old log file is deleted. Flush is
// synchronous inside Put (stage 2); the background-flush + immutable-
// memtable machinery arrives with compaction.
//
// Crash consistency of flush (why the order is what it is):
//   1. the new (empty) log file is created and fsynced BEFORE the
//      MANIFEST switches — a crash right after the switch must find it;
//   2. the SST is fsynced BEFORE the MANIFEST names it — a crash before
//      the switch leaves the SST an orphan, which the next Open deletes;
//   3. the MANIFEST is replaced atomically (rename), so it is never
//      observed torn;
//   4. the old log is deleted only AFTER the switch — its records live
//      in the SST now.
//
// Read path: MemTable → L0 SSTables newest-file-first. The first file
// that CONTAINS the user key decides the answer (its newest version may
// be a tombstone: a key deleted on top of an older file must stay
// deleted) — inside a file, index seek + Bloom filter prune the read.
//
// Recovery: parse the MANIFEST (SST list + current log number), delete
// orphan files, open every SST, then replay the current log into a fresh
// MemTable and continue the sequence where the records left off.
//
// Concurrency contract (stage 2): single-threaded. The RESP server layer
// (stage 4) will add external synchronization around this class.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bedrockkv/log.h"
#include "bedrockkv/memtable.h"
#include "bedrockkv/status.h"
#include "bedrockkv/sstable.h"

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
};

class DB {
 public:
  static constexpr const char* kManifestFileName = "MANIFEST";
  static constexpr const char* kManifestTmpFileName = "MANIFEST.tmp";

  static std::string SstFileName(uint64_t number);  // e.g. 000003.sst
  static std::string LogFileName(uint64_t number);  // e.g. 000003.log

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

  bool wal_truncated_on_recovery() const { return wal_truncated_; }
  uint64_t latest_seq() const { return next_seq_ - 1; }
  // Number of L0 SST files (test/observability accessor).
  size_t l0_file_count() const { return l0_.size(); }
  // File number of the current log generation (test accessor).
  uint64_t log_number() const { return log_number_; }

 private:
  DB() = default;

  struct SstFile {
    sst::FileMeta meta;
    std::shared_ptr<sst::Table> table;
  };

  std::string SstPath(uint64_t number) const { return dir_ + "/" + SstFileName(number); }
  std::string LogPath(uint64_t number) const { return dir_ + "/" + LogFileName(number); }
  std::string ManifestPath() const { return dir_ + "/" + kManifestFileName; }

  Status WriteRecord(uint8_t type, std::string_view key,
                     std::string_view value);
  Status MaybeSync();
  Status FlushMemTable();
  Status WriteManifest();  // full atomic rewrite of the current state
  void RemoveOrphanFiles(
      const std::vector<sst::FileMeta>& files) const;  // at Open

  std::string dir_;
  int log_fd_ = -1;
  std::unique_ptr<log::Writer> log_writer_;
  // Heap-held: a flushed MemTable is discarded wholesale and replaced
  // with a fresh one; SkipList is deliberately non-movable (atomic
  // member), so swap-by-pointer is the clean way.
  std::unique_ptr<MemTable> memtable_;
  uint64_t next_seq_ = 1;
  SyncMode sync_mode_ = SyncMode::kSyncAlways;
  size_t write_buffer_size_ = 4u << 20;
  uint64_t unsynced_bytes_ = 0;
  bool wal_truncated_ = false;

  uint64_t next_file_number_ = 1;
  uint64_t log_number_ = 0;
  std::vector<SstFile> l0_;  // ascending file number = old → new
};

}  // namespace bedrockkv
