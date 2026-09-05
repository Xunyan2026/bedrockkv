// BedrockKV — Write-Ahead Log: a block-based, CRC-protected record stream.
//
// Physical layout (leveldb's log format, header layout per the design doc):
//
//   block:  32768 bytes
//   record: [ length: 4B LE ][ crc32: 4B LE ][ type: 1B ][ payload ]
//   crc covers the type byte + payload; records never span block
//   boundaries — a logical record that does not fit in the remaining
//   block space is split into FIRST/MIDDLE/.../LAST fragments; a block
//   tail shorter than a header is zero-padded (kZeroType, skipped).
//
// Why blocks + fragmentation (interview gold):
//   * a torn or corrupted write damages at most one physical record — the
//     length field can never silently shift parsing of all later records
//     across a block boundary;
//   * readers work on one bounded buffer (one block) at a time;
//   * crash recovery can truncate the tail exactly at the end of the last
//     fully intact logical record (see Reader::last_good_end).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "bedrockkv/status.h"

namespace bedrockkv::log {

constexpr size_t kBlockSize = 32768;
constexpr size_t kHeaderSize = 9;  // length(4) + crc32(4) + type(1)

enum RecordType : uint8_t {
  kZeroType = 0,   // zero padding / torn-header remnants — skipped by reader
  kFullType = 1,
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4,
};

// Appends records to a file descriptor. Single-writer only. Does NOT
// fsync — the caller (the future DB layer) owns the durability policy.
class Writer {
 public:
  explicit Writer(int fd) : fd_(fd) {}

  Status AddRecord(std::string_view payload);
  // Offset within the current block (diagnostics only).
  size_t block_offset() const { return block_offset_; }

 private:
  Status WriteAll(const char* data, size_t n);
  Status EmitPhysicalRecord(RecordType type, const char* payload, size_t len);

  int fd_;
  size_t block_offset_ = 0;
};

// Reads records back sequentially. Tolerates a torn tail (crash mid-write):
// reports it as kCorruption together with the end offset of the last fully
// intact logical record, so recovery can truncate exactly there.
class Reader {
 public:
  enum class Result { kOk, kEof, kCorruption };

  explicit Reader(int fd) : fd_(fd) {}

  // Reads the next logical record into *payload. On kCorruption,
  // *corruption_offset (if given) receives the end offset of the last
  // complete logical record — the safe truncation point.
  Result ReadRecord(std::string* payload, uint64_t* corruption_offset = nullptr);
  uint64_t last_good_end() const { return last_good_end_; }

 private:
  Result ReadPhysicalRecord(std::string_view* fragment, uint8_t* type,
                            uint64_t* end_offset);

  int fd_;
  std::string buffer_;        // current block
  size_t pos_ = 0;            // parse position within buffer_
  bool eof_ = false;
  uint64_t bytes_read_ = 0;   // total bytes pulled from fd_
  uint64_t last_good_end_ = 0;  // end offset of last complete logical record
  std::string scratch_;       // fragment assembly area
};

}  // namespace bedrockkv::log
