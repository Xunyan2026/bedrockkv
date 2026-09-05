// BedrockKV — DB: the stage-1 engine: WAL + MemTable.
//
// Write path: encode (seq, type, key, value) → append one CRC-protected
// WAL record → apply into the MemTable. The WAL record is durable before
// the MemTable ever sees it, so a crash never loses an acknowledged write
// (with SyncMode::kSyncAlways; see SyncMode for the trade-offs).
//
// Recovery: on Open, replay the WAL into a fresh MemTable, restore the
// sequence counter, and truncate any torn tail exactly at the end of the
// last intact record.
//
// Concurrency contract (stage 1): single-threaded. The RESP server layer
// (stage 4) will add external synchronization around this class.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "bedrockkv/log.h"
#include "bedrockkv/memtable.h"
#include "bedrockkv/status.h"

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
};

class DB {
 public:
  static constexpr const char* kWalFileName = "bedrockkv.wal";

  // Opens (creating if necessary) a database rooted at `dir`. Returns
  // nullptr on failure with *status (if provided) describing the error.
  // Recovery is part of Open; a torn WAL tail is truncated automatically
  // and reported via wal_truncated_on_recovery().
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

 private:
  DB() = default;

  Status WriteRecord(uint8_t type, std::string_view key,
                     std::string_view value);
  Status MaybeSync();

  int wal_fd_ = -1;
  std::unique_ptr<log::Writer> log_writer_;
  MemTable memtable_;
  uint64_t next_seq_ = 1;
  SyncMode sync_mode_ = SyncMode::kSyncAlways;
  uint64_t unsynced_bytes_ = 0;
  bool wal_truncated_ = false;
};

}  // namespace bedrockkv
