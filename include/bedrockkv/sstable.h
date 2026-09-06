// BedrockKV — SSTable: the immutable on-disk sorted table (stage 2).
//
// File layout and encodings are specified in docs/sstable-format.md.
// Summary (all integers little-endian):
//
//   [data block 0]...[data block N] [filter block] [index block] [footer]
//
//   data block : prefix-compressed internal-key entries + restart points
//   filter     : one Bloom filter per data block (over user keys)
//   index      : (last internal key of block -> block handle), block-coded
//   footer(44B): filter handle, index handle, num_entries, whole-file
//                CRC32, magic "BRKVSST1"
//
// Stage-2 simplification (documented in the format doc): Table::Open
// reads the whole file once, verifies the CRC, and serves all block
// reads from that in-memory buffer. The block-cache step moves this to
// pread + LRU; the pruning order (index -> bloom -> data block) already
// matches the final design, so nothing above this layer changes.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "bedrockkv/block.h"
#include "bedrockkv/iterator.h"
#include "bedrockkv/memtable.h"  // kTypeValue / kTypeDeletion / Lookup
#include "bedrockkv/status.h"

namespace bedrockkv::sst {

struct TableOptions {
  size_t block_size = 4096;        // flush a data block past this estimate
  size_t restart_interval = 16;    // entries between full-key restarts
  size_t bloom_bits_per_key = 10;  // ≈0.8% false positives
};

// Metadata of one finished SST file. The DB persists this in the
// MANIFEST; Table itself only re-derives the key range and entry count.
struct FileMeta {
  uint64_t file_number = 0;
  uint32_t level = 0;  // 0 = L0 (overlapping), 1..kMaxLevels-1 = leveled
  uint64_t smallest_seq = 0;
  uint64_t largest_seq = 0;
  uint64_t entry_count = 0;
  uint64_t file_size = 0;
  std::string smallest_key;  // user keys
  std::string largest_key;
};

class Builder {
 public:
  explicit Builder(const TableOptions& options = {});

  // Entries must arrive in strictly increasing internal-key order —
  // exactly what MemTable::ForEach yields. Violations are rejected.
  Status Add(std::string_view user_key, uint64_t seq, uint8_t type,
             std::string_view value);

  // Seals the file image and writes it durably (write + fsync + dir
  // fsync). Fills *meta on success.
  Status Finish(const std::string& path, FileMeta* meta);

  uint64_t num_entries() const { return num_entries_; }
  // Current on-disk size estimate while building (sealed blocks + the
  // in-progress one) — its only consumer is compaction output splitting.
  uint64_t ApproximateFileSize() const {
    return file_.size() + data_block_.CurrentSizeEstimate();
  }

 private:
  // Seals the current data block: emits it, its Bloom filter, and its
  // index entry.
  void FlushDataBlock();

  TableOptions options_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  // Owning copies: callers may pass string_views into temporaries, and
  // the filter is built only when the block seals (possibly much later).
  std::vector<std::string> block_user_keys_;
  std::string filters_;                            // concatenated filters
  std::vector<uint32_t> filter_offsets_;           // per data block
  std::string file_;        // the file image built so far
  uint32_t crc_ = 0;        // streaming CRC over file_ before the footer
  std::string last_internal_key_;
  bool has_last_key_ = false;
  uint64_t num_entries_ = 0;
  uint64_t smallest_seq_ = 0;
  uint64_t largest_seq_ = 0;
  std::string smallest_user_key_;
  std::string largest_user_key_;
};

class Table {
 public:
  // Reads the whole file, verifies magic + whole-file CRC, parses the
  // footer, index and filter blocks. Returns nullptr on any corruption.
  static std::shared_ptr<Table> Open(uint64_t file_number,
                                     const std::string& path,
                                     Status* status = nullptr);

  // Point lookup. kFound fills *value with the newest version; kDeleted
  // means the newest version is a tombstone; kMissing means this file
  // has no version of the key (possibly a Bloom false positive checked
  // against the data). kCorruption on damaged structures.
  // `max_seq` caps visibility (snapshot reads): only versions with
  // seq <= max_seq are visible; the default reads the newest version.
  MemTable::Lookup Get(std::string_view user_key, std::string* value,
                       uint64_t max_seq = MemTable::kMaxSeq,
                       Status* status = nullptr) const;

  uint64_t file_number() const { return file_number_; }
  const std::string& smallest_user_key() const { return smallest_user_key_; }
  const std::string& largest_user_key() const { return largest_user_key_; }
  uint64_t num_entries() const { return num_entries_; }
  // Full on-disk size (the whole file lives in memory).
  size_t size() const { return file_data_.size(); }

  // Entries in internal-key order across all data blocks. The table must
  // outlive the iterator (it does: tables are immutable and shared_ptr-
  // kept alive by the Version snapshot the iterator is built from).
  std::unique_ptr<Iterator> NewIterator() const;

 private:
  struct IndexEntry {
    std::string key;  // last internal key of the block
    uint64_t offset = 0;
    uint32_t size = 0;
  };
  friend class TableIterator;
  bool KeyMayMatchInBlock(size_t block_index,
                          std::string_view user_key) const;

  uint64_t file_number_ = 0;
  std::string file_data_;  // whole file; every view below points into it
  std::vector<IndexEntry> index_;
  std::string_view filter_block_;
  std::string smallest_user_key_;
  std::string largest_user_key_;
  uint64_t num_entries_ = 0;
};

// Walks every data block of a Table in internal-key order.
class TableIterator : public Iterator {
 public:
  explicit TableIterator(const Table* table);

  bool Valid() const override { return valid_; }
  void SeekToFirst() override;
  void Seek(std::string_view target) override;
  void Next() override;
  std::string_view key() const override { return block_.key(); }
  std::string_view value() const override { return block_.value(); }

 private:
  // Positions on data block `index`; advances past any empty tail.
  void EnterBlock(size_t index, bool seek_first);

  const Table* table_;
  size_t block_index_ = 0;
  Block block_;  // re-seated on every block transition
  bool valid_ = false;
  bool corrupted_ = false;
};

}  // namespace bedrockkv::sst
