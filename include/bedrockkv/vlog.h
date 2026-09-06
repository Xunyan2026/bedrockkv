// BedrockKV — VLog: the append-only value log of the WiscKey design.
//
// When value separation is on, the LSM stores only a 21-byte pointer and
// the actual bytes live here. File format (docs/vlog-format.md):
//
//   entry: [crc32 u32][klen u32][vsize u32][key][value]
//   crc covers klen + key + vsize + value — everything after the crc
//   field — so a torn tail (crash mid-append) is detectable on read.
//
// The DB owns the durability policy: appends land in the page cache and
// Sync() is called by DB::MaybeSync BEFORE the WAL's fsync, so a durable
// WAL record can never reference vLog bytes that were not durable first.
//
// Threading: Append and Sync are called with the DB mutex held (one
// appender by contract); ReadValue and ScanEntries are const and
// thread-safe (pread on an fd is atomic with respect to appends).
// Readers reach a VLog through a snapshot taken under the DB mutex, so
// every byte they read was appended before that snapshot (happens-before
// via the mutex); `end_` is still an atomic because lock-free readers
// sample it outside any lock (TSan-clean, and a stale sample at worst
// yields kNotFound — CRC guards the data itself).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "bedrockkv/status.h"

namespace bedrockkv {

// The fixed-size pointer the LSM stores in place of a separated value.
struct ValuePointer {
  uint64_t vlog_number = 0;
  uint64_t offset = 0;
  uint32_t value_size = 0;
};

// [tag u8 = 0xFF][vlog_number u64][offset u64][value_size u32] — 21 bytes.
inline constexpr size_t kValuePointerSize = 1 + 8 + 8 + 4;
// First byte of an encoded pointer. Inline values can never collide with
// it because Open clamps the separation threshold to >= 64 bytes.
inline constexpr char kValuePointerTag = static_cast<char>(0xff);

std::string EncodeValuePointer(const ValuePointer& p);
// Recognizes exactly the 21-byte, 0xFF-tagged shape; everything else is
// an inline value and returns false.
bool DecodeValuePointer(std::string_view stored, ValuePointer* p);

class VLog {
 public:
  static std::string FileName(uint64_t number);  // e.g. 000003.vlog

  // Opens `number`.vlog, creating it if missing; append position starts
  // at the file's real end (a vLog is never truncated on Open — its torn
  // tail is inert garbage that GC reclaims).
  static std::unique_ptr<VLog> Open(const std::string& dir, uint64_t number,
                                    Status* status);

  ~VLog();

  VLog(const VLog&) = delete;
  VLog& operator=(const VLog&) = delete;

  // Appends one entry; *offset receives the entry's start offset (what
  // goes into the value pointer) and *entry_bytes (if non-null) the
  // physical bytes written, for the write-amplification counters.
  // Returns an error only on I/O failure.
  Status Append(std::string_view key, std::string_view value,
                uint64_t* offset, uint64_t* entry_bytes = nullptr);

  // Reads the value of the entry at `offset`. `expected_size` is the
  // size recorded in the pointer and must match the entry — a mismatch
  // means the pointer and the log disagree, which is corruption.
  // kNotFound: offset points past the file end or into a torn tail
  // (bounded loss, same contract as the WAL's sync mode). kCorruption:
  // CRC mismatch or size mismatch.
  Status ReadValue(uint64_t offset, uint32_t expected_size,
                   std::string* value) const;

  Status Sync() const;

  // Invokes fn(key, value, offset) for every CRC-valid entry starting at
  // the beginning of the file, up to `limit` bytes (the GC scans only
  // the pre-rotation portion). Stops at the first invalid entry (torn
  // tail): everything after it is unreachable garbage by construction.
  Status ScanEntries(
      uint64_t limit,
      const std::function<void(std::string_view key, std::string_view value,
                               uint64_t offset)>& fn) const;

  uint64_t file_number() const { return number_; }
  uint64_t file_size() const { return end_.load(std::memory_order_relaxed); }
  // Raw fd, exposed for the DB's parallel vLog+WAL fsync SQE pair —
  // never written or closed by callers.
  int fd() const { return fd_; }

 private:
  VLog(int fd, uint64_t number, uint64_t end)
      : fd_(fd), number_(number), end_(end) {}

  int fd_ = -1;
  uint64_t number_ = 0;
  // Next append offset == current file size. Written by the (mutex-held)
  // appender, sampled by lock-free readers: relaxed is enough — the
  // mutex chain in the read path already orders every append a reader
  // can legitimately reference.
  std::atomic<uint64_t> end_ = 0;
};

}  // namespace bedrockkv
