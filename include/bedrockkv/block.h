// BedrockKV — Block encoding: prefix-compressed entries with restart
// points. One encoding, three uses: data blocks, the index block, and
// (in later stages) metaindex blocks — same as leveldb.
//
//   entry := [shared u32][non_shared u32][value_len u32][key_delta][value]
//   block := entry* [restart_offset u32]* [num_restarts u32]
//
// See docs/sstable-format.md §2. The decoder is defensive: every field
// is bounds-checked against the block extent; a violation marks the
// block corrupted rather than reading out of bounds.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bedrockkv/status.h"

namespace bedrockkv {

// Orders (user_key ++ tag u64 LE) internal keys: user key ascending, tag
// descending — the newest version of a key sorts first. Comparing only
// the last 8 bytes as the tag keeps this independent of key length.
struct InternalKeyComparator {
  bool operator()(std::string_view a, std::string_view b) const;
};

class BlockBuilder {
 public:
  explicit BlockBuilder(size_t restart_interval = 16)
      : restart_interval_(restart_interval) {
    Reset();
  }

  void Reset();
  // Keys MUST be added in strictly increasing order under
  // InternalKeyComparator — the caller (sst::Builder) enforces this.
  void Add(std::string_view key, std::string_view value);
  size_t CurrentSizeEstimate() const {
    return buffer_.size() + (restarts_.size() + 1) * sizeof(uint32_t);
  }
  bool empty() const { return counter_ == 0; }
  // Appends the restart array and hands over the finished block.
  std::string Finish();

 private:
  size_t restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  std::string last_key_;
  size_t counter_ = 0;
};

// Decoded view over a finished block. Lightweight (no copies of entry
// payloads): keys are reconstructed into a scratch string, values are
// string_views into the caller's buffer, which must outlive the Block.
class Block {
 public:
  // `contents` includes the restart array. Validates the trailer up front.
  explicit Block(std::string_view contents);

  bool valid() const { return valid_; }
  bool corrupted() const { return corrupted_; }

  // Positions on the first entry with key >= target (invalid at end).
  void Seek(std::string_view target);
  void SeekToFirst();
  // Precondition: valid().
  bool Valid() const { return valid_; }
  std::string_view key() const { return cur_key_; }
  std::string_view value() const { return cur_value_; }
  void Next();

 private:
  static constexpr size_t kHeaderSize = 12;
  // Decodes the entry at `offset` into cur_key_/cur_value_/entry_end_.
  // Returns false (and sets corrupted_) on any bounds violation.
  bool ParseEntry(size_t offset);
  std::string_view KeyAtRestart(size_t restart_index) const;

  std::string_view data_;
  size_t restart_array_offset_ = 0;  // where the u32 offsets start
  uint32_t num_restarts_ = 0;
  bool valid_ = false;
  bool corrupted_ = false;

  std::string key_scratch_;  // shared-prefix reconstruction area
  std::string_view cur_key_;
  std::string_view cur_value_;
  size_t entry_end_ = 0;
};

}  // namespace bedrockkv
