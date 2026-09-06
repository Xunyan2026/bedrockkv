#include "bedrockkv/sstable.h"

#include <algorithm>
#include <cstring>

#include "bedrockkv/bloom.h"
#include "bedrockkv/crc32.h"
#include "bedrockkv/encoding.h"
#include "bedrockkv/fs_util.h"

namespace bedrockkv::sst {
namespace {

constexpr char kMagic[8] = {'B', 'R', 'K', 'V', 'S', 'S', 'T', '1'};
constexpr size_t kFooterSize = 8 + 4 + 8 + 4 + 8 + 4 + 8;  // 44
constexpr size_t kTagSize = 8;

std::string EncodeInternalKey(std::string_view user_key, uint64_t seq,
                              uint8_t type) {
  std::string ik;
  ik.reserve(user_key.size() + kTagSize);
  ik.append(user_key);
  PutFixed64(&ik, (seq << 8) | type);
  return ik;
}

std::string_view UserKeyOf(std::string_view internal_key) {
  return internal_key.size() >= kTagSize
             ? internal_key.substr(0, internal_key.size() - kTagSize)
             : internal_key;
}

}  // namespace

// ---- Builder ----

Builder::Builder(const TableOptions& options)
    : options_(options),
      data_block_(options.restart_interval),
      index_block_(options.restart_interval) {}

void Builder::FlushDataBlock() {
  if (data_block_.empty()) {
    return;
  }
  std::string block = data_block_.Finish();
  const uint64_t offset = file_.size();
  crc_ = Crc32(block.data(), block.size(), crc_);
  file_.append(block);

  filter_offsets_.push_back(static_cast<uint32_t>(filters_.size()));
  const std::vector<std::string_view> filter_views(block_user_keys_.begin(),
                                                   block_user_keys_.end());
  filters_ += bloom::BuildFilter(filter_views, options_.bloom_bits_per_key);
  block_user_keys_.clear();

  std::string handle;
  PutFixed64(&handle, offset);
  PutFixed32(&handle, static_cast<uint32_t>(block.size()));
  index_block_.Add(last_internal_key_, handle);
  data_block_.Reset();
}

Status Builder::Add(std::string_view user_key, uint64_t seq, uint8_t type,
                    std::string_view value) {
  std::string ik = EncodeInternalKey(user_key, seq, type);
  const InternalKeyComparator less;
  if (has_last_key_ && !less(last_internal_key_, ik)) {
    return Status::InvalidArgument(
        "sstable entries must be strictly increasing");
  }

  if (!has_last_key_) {
    smallest_user_key_.assign(user_key);
    smallest_seq_ = seq;
    largest_seq_ = seq;
  }
  smallest_seq_ = std::min(smallest_seq_, seq);
  largest_seq_ = std::max(largest_seq_, seq);
  largest_user_key_.assign(user_key);
  last_internal_key_ = std::move(ik);
  has_last_key_ = true;

  data_block_.Add(last_internal_key_, value);
  block_user_keys_.emplace_back(user_key);
  ++num_entries_;

  if (data_block_.CurrentSizeEstimate() >= options_.block_size) {
    FlushDataBlock();
  }
  return Status::Ok();
}

Status Builder::Finish(const std::string& path, FileMeta* meta) {
  FlushDataBlock();

  // Filter block: [filters][offset array][array_start]
  const uint32_t array_start = static_cast<uint32_t>(filters_.size());
  std::string filter_block = filters_;
  for (const uint32_t off : filter_offsets_) {
    PutFixed32(&filter_block, off);
  }
  PutFixed32(&filter_block, array_start);
  const uint64_t filter_offset = file_.size();
  crc_ = Crc32(filter_block.data(), filter_block.size(), crc_);
  file_.append(filter_block);

  std::string index_block = index_block_.Finish();
  const uint64_t index_offset = file_.size();
  crc_ = Crc32(index_block.data(), index_block.size(), crc_);
  file_.append(index_block);

  // Footer. The CRC covers everything before it — one bit flipped
  // anywhere in the file fails Open().
  std::string footer;
  PutFixed64(&footer, filter_offset);
  PutFixed32(&footer, static_cast<uint32_t>(filter_block.size()));
  PutFixed64(&footer, index_offset);
  PutFixed32(&footer, static_cast<uint32_t>(index_block.size()));
  PutFixed64(&footer, num_entries_);
  PutFixed32(&footer, crc_);
  footer.append(kMagic, sizeof(kMagic));
  file_ += footer;

  if (meta != nullptr) {
    // file_number / level are assigned by the caller (DB / MANIFEST) —
    // Finish must not touch them.
    meta->smallest_seq = smallest_seq_;
    meta->largest_seq = largest_seq_;
    meta->entry_count = num_entries_;
    meta->file_size = file_.size();
    meta->smallest_key = smallest_user_key_;
    meta->largest_key = largest_user_key_;
  }
  return fs::WriteFileDurable(path, file_);
}

// ---- Table ----

std::shared_ptr<Table> Table::Open(uint64_t file_number,
                                   const std::string& path,
                                   Status* status) {
  const auto fail = [status, &path](const std::string& msg) {
    if (status != nullptr) {
      *status = Status::Corruption("sstable " + path + ": " + msg);
    }
    return std::shared_ptr<Table>();
  };

  std::string data;
  const Status s = fs::ReadFileToString(path, &data);
  if (!s.ok()) {
    if (status != nullptr) {
      *status = s;
    }
    return nullptr;
  }
  if (data.size() < kFooterSize) {
    return fail("file smaller than a footer");
  }
  const size_t footer_start = data.size() - kFooterSize;
  const char* f = data.data() + footer_start;
  if (std::memcmp(f + 36, kMagic, sizeof(kMagic)) != 0) {
    return fail("bad magic");
  }
  const uint64_t filter_offset = GetFixed64(f);
  const uint32_t filter_size = GetFixed32(f + 8);
  const uint64_t index_offset = GetFixed64(f + 12);
  const uint32_t index_size = GetFixed32(f + 20);
  const uint64_t num_entries = GetFixed64(f + 24);
  const uint32_t stored_crc = GetFixed32(f + 32);

  // Handles must stay inside the file and below the footer.
  if (filter_offset > footer_start ||
      filter_offset + filter_size > footer_start ||
      index_offset > footer_start || index_offset + index_size > footer_start) {
    return fail("block handle out of range");
  }
  if (Crc32(data.data(), footer_start) != stored_crc) {
    return fail("whole-file CRC mismatch");
  }

  auto table = std::shared_ptr<Table>(new Table());
  table->file_number_ = file_number;
  table->num_entries_ = num_entries;
  table->file_data_ = std::move(data);

  Block index_block(
      std::string_view(table->file_data_).substr(index_offset, index_size));
  if (index_block.corrupted()) {
    return fail("index block corrupt");
  }
  index_block.SeekToFirst();
  while (index_block.Valid()) {
    IndexEntry entry;
    entry.key = index_block.key();
    const std::string_view handle = index_block.value();
    if (handle.size() != 12) {
      return fail("index handle has wrong size");
    }
    entry.offset = GetFixed64(handle.data());
    entry.size = GetFixed32(handle.data() + 8);
    if (entry.offset > footer_start ||
        entry.offset + entry.size > footer_start) {
      return fail("data block handle out of range");
    }
    table->index_.push_back(std::move(entry));
    index_block.Next();
  }
  if (index_block.corrupted()) {
    return fail("index block corrupt mid-scan");
  }
  if (table->index_.empty()) {
    return fail("no data blocks");
  }
  table->largest_user_key_ = std::string(UserKeyOf(table->index_.back().key));
  // The smallest key is NOT the index's first entry — index entries hold
  // the LAST key of each block. Decode the first data block and take its
  // first entry (leveldb does the same). Skipping this made every file's
  // smallest key look like "the key ~one block into the file", which
  // leveled-compaction binary search then misread as an empty left side.
  {
    Block first_block(std::string_view(table->file_data_)
                          .substr(table->index_.front().offset,
                                  table->index_.front().size));
    if (first_block.corrupted()) {
      return fail("first data block corrupt");
    }
    first_block.SeekToFirst();
    if (!first_block.Valid()) {
      return fail("first data block empty");
    }
    table->smallest_user_key_ = std::string(UserKeyOf(first_block.key()));
  }
  table->filter_block_ =
      std::string_view(table->file_data_).substr(filter_offset, filter_size);

  if (status != nullptr) {
    *status = Status::Ok();
  }
  return table;
}

bool Table::KeyMayMatchInBlock(size_t block_index,
                               std::string_view user_key) const {
  if (filter_block_.size() < 4) {
    return true;  // no filter data: be conservative
  }
  const uint32_t array_start =
      GetFixed32(filter_block_.data() + filter_block_.size() - 4);
  if (array_start + 4 > filter_block_.size()) {
    return true;
  }
  const size_t n = (filter_block_.size() - 4 - array_start) / 4;
  if (block_index >= n) {
    return true;
  }
  const uint32_t start =
      GetFixed32(filter_block_.data() + array_start + block_index * 4);
  const uint32_t end = block_index + 1 < n
                           ? GetFixed32(filter_block_.data() +
                                        array_start + (block_index + 1) * 4)
                           : array_start;
  if (start > end || end > array_start) {
    return true;
  }
  return bloom::KeyMayMatch(filter_block_.substr(start, end - start), user_key);
}

MemTable::Lookup Table::Get(std::string_view user_key, std::string* value,
                            Status* status) const {
  const auto corrupt = [status, this](const std::string& what) {
    if (status != nullptr) {
      *status = Status::Corruption("sstable " + std::to_string(file_number_) +
                                   ": " + what);
    }
    return MemTable::Lookup::kMissing;
  };

  // Search key: (user_key, tag = max) — under the internal comparator the
  // first entry >= this target is exactly the newest version of user_key,
  // if the file contains it at all.
  std::string target;
  target.reserve(user_key.size() + kTagSize);
  target.append(user_key);
  PutFixed64(&target, ~static_cast<uint64_t>(0));

  const InternalKeyComparator less;
  const auto it = std::lower_bound(
      index_.begin(), index_.end(), target,
      [&](const IndexEntry& entry, const std::string& t) {
        return less(entry.key, t);
      });
  if (it == index_.end()) {
    return MemTable::Lookup::kMissing;
  }
  const size_t block_index = static_cast<size_t>(it - index_.begin());
  if (!KeyMayMatchInBlock(block_index, user_key)) {
    return MemTable::Lookup::kMissing;  // Bloom says no: skip the block
  }

  Block block(std::string_view(file_data_).substr(it->offset, it->size));
  if (block.corrupted()) {
    return corrupt("data block corrupt");
  }
  block.Seek(target);
  if (block.corrupted()) {
    return corrupt("data block corrupt at seek");
  }
  if (!block.Valid()) {
    return MemTable::Lookup::kMissing;
  }
  const std::string_view found = block.key();
  if (found.size() >= kTagSize &&
      found.substr(0, found.size() - kTagSize) == user_key) {
    const uint64_t tag = GetFixed64(found.data() + found.size() - kTagSize);
    if ((tag & 0xff) == kTypeValue) {
      value->assign(block.value());
      return MemTable::Lookup::kFound;
    }
    return MemTable::Lookup::kDeleted;
  }
  return MemTable::Lookup::kMissing;
}

// ---- TableIterator ----

TableIterator::TableIterator(const Table* table)
    : table_(table), block_(std::string_view()) {}

std::unique_ptr<Iterator> Table::NewIterator() const {
  return std::make_unique<TableIterator>(this);
}

void TableIterator::EnterBlock(size_t index, bool seek_first) {
  block_index_ = index;
  block_ = Block(std::string_view(table_->file_data_)
                     .substr(table_->index_[index].offset,
                             table_->index_[index].size));
  if (block_.corrupted()) {
    corrupted_ = true;
    valid_ = false;
    return;
  }
  if (seek_first) {
    block_.SeekToFirst();
  }
  valid_ = block_.Valid();
}

void TableIterator::SeekToFirst() {
  corrupted_ = false;
  if (table_->index_.empty()) {
    valid_ = false;
    return;
  }
  EnterBlock(0, /*seek_first=*/true);
}

void TableIterator::Seek(std::string_view target) {
  corrupted_ = false;
  // First block whose last key >= target: the only one that can hold it.
  const InternalKeyComparator less;
  auto it = std::lower_bound(
      table_->index_.begin(), table_->index_.end(), target,
      [&less](const Table::IndexEntry& e, std::string_view t) {
        return less(e.key, t);
      });
  if (it == table_->index_.end()) {
    valid_ = false;
    return;
  }
  EnterBlock(static_cast<size_t>(it - table_->index_.begin()),
             /*seek_first=*/false);
  if (corrupted_) {
    return;
  }
  // A freshly seated block is UNPOSITIONED (Valid() is false until the
  // first positioning call), so Seek must run before reading valid_ —
  // checking Valid() first silently disabled every non-initial Seek.
  block_.Seek(target);
  valid_ = block_.Valid();
}

void TableIterator::Next() {
  if (!valid_) {
    return;
  }
  block_.Next();
  if (block_.corrupted()) {
    corrupted_ = true;
    valid_ = false;
    return;
  }
  if (block_.Valid()) {
    return;  // same block, next entry
  }
  // Move to the next data block (every sealed block holds >= 1 entry).
  if (block_index_ + 1 >= table_->index_.size()) {
    valid_ = false;
    return;
  }
  EnterBlock(block_index_ + 1, /*seek_first=*/true);
}

}  // namespace bedrockkv::sst
