#include "bedrockkv/block.h"

#include <algorithm>
#include <cstring>

#include "bedrockkv/encoding.h"

namespace bedrockkv {

bool InternalKeyComparator::operator()(std::string_view a,
                                       std::string_view b) const {
  // Both are (user_key ++ tag u64 LE), at least 8 bytes by construction.
  // Defensively treat short keys as bare user keys (tag = 0).
  const size_t a_user = a.size() >= 8 ? a.size() - 8 : a.size();
  const size_t b_user = b.size() >= 8 ? b.size() - 8 : b.size();
  std::string_view a_key = a.substr(0, a_user);
  std::string_view b_key = b.substr(0, b_user);
  if (a_key != b_key) {
    return a_key < b_key;
  }
  const uint64_t a_tag = a_user == a.size() ? 0 : GetFixed64(a.data() + a_user);
  const uint64_t b_tag = b_user == b.size() ? 0 : GetFixed64(b.data() + b_user);
  return a_tag > b_tag;  // newer (larger) tag sorts first
}

// ---- BlockBuilder ----

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);  // first entry is always a restart point
  last_key_.clear();
  counter_ = 0;
}

void BlockBuilder::Add(std::string_view key, std::string_view value) {
  size_t shared = 0;
  if (counter_ < restart_interval_) {
    // Compress against the previous key, but never claim more prefix than
    // the previous key actually has.
    const size_t min_len = std::min(last_key_.size(), key.size());
    while (shared < min_len && last_key_[shared] == key[shared]) {
      ++shared;
    }
  } else {
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  PutFixed32(&buffer_, static_cast<uint32_t>(shared));
  PutFixed32(&buffer_, static_cast<uint32_t>(non_shared));
  PutFixed32(&buffer_, static_cast<uint32_t>(value.size()));
  buffer_.append(key.substr(shared));
  buffer_.append(value);

  last_key_.assign(key);
  ++counter_;
}

std::string BlockBuilder::Finish() {
  for (const uint32_t r : restarts_) {
    PutFixed32(&buffer_, r);
  }
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  counter_ = 0;
  restarts_.clear();
  restarts_.push_back(0);
  return std::move(buffer_);
}

// ---- Block ----

Block::Block(std::string_view contents) : data_(contents) {
  if (data_.size() < sizeof(uint32_t)) {
    corrupted_ = true;
    return;
  }
  num_restarts_ = GetFixed32(data_.data() + data_.size() - 4);
  const uint64_t trailer = (static_cast<uint64_t>(num_restarts_) + 1) * 4;
  if (trailer > data_.size() || num_restarts_ == 0) {
    corrupted_ = true;
    return;
  }
  restart_array_offset_ = data_.size() - static_cast<size_t>(trailer);
  // Validate every restart offset eagerly: it bounds all later seeks.
  for (uint32_t i = 0; i < num_restarts_; ++i) {
    const uint32_t off =
        GetFixed32(data_.data() + restart_array_offset_ + i * 4);
    if (off >= restart_array_offset_) {
      corrupted_ = true;
      return;
    }
  }
}

std::string_view Block::KeyAtRestart(size_t restart_index) const {
  const uint32_t off =
      GetFixed32(data_.data() + restart_array_offset_ + restart_index * 4);
  // Restart entries never share a prefix (shared == 0), so the key is
  // right there in the entry header. The constructor validated off, but
  // non_shared is attacker-controlled garbage until proven otherwise —
  // return an empty view rather than risking substr(pos) throwing.
  if (off + kHeaderSize > restart_array_offset_) {
    return {};
  }
  const uint32_t non_shared = GetFixed32(data_.data() + off + 4);
  return data_.substr(off + kHeaderSize, non_shared);
}

void Block::SeekToFirst() {
  if (corrupted_) {
    valid_ = false;
    return;
  }
  valid_ = ParseEntry(0);
}

void Block::Seek(std::string_view target) {
  if (corrupted_) {
    valid_ = false;
    return;
  }
  // Binary search over restart points: land on the last restart whose
  // key is < target (or restart 0), then linear-scan forward. Keys at
  // restarts are full keys, so no reconstruction is needed here.
  size_t left = 0;
  size_t right = num_restarts_ - 1;
  const InternalKeyComparator less;
  while (left < right) {
    const size_t mid = (left + right + 1) / 2;
    if (less(KeyAtRestart(mid), target)) {
      left = mid;
    } else {
      right = mid - 1;
    }
  }
  valid_ = false;
  size_t offset = GetFixed32(data_.data() + restart_array_offset_ + left * 4);
  while (offset < restart_array_offset_) {
    if (!ParseEntry(offset)) {
      return;  // corrupted_: ParseEntry set the flags
    }
    valid_ = true;
    if (!less(cur_key_, target)) {
      return;  // first entry >= target found
    }
    offset = entry_end_;
  }
  // Scanned past the last entry: target is beyond this block.
  valid_ = false;
}

void Block::Next() {
  if (corrupted_ || !valid_) {
    valid_ = false;
    return;
  }
  valid_ = ParseEntry(entry_end_);
}

bool Block::ParseEntry(size_t offset) {
  valid_ = false;
  if (offset == restart_array_offset_) {
    return false;  // clean end of block — not a corruption
  }
  if (offset > restart_array_offset_ ||
      offset + kHeaderSize > restart_array_offset_) {
    corrupted_ = true;
    return false;
  }
  const uint32_t shared = GetFixed32(data_.data() + offset);
  const uint32_t non_shared = GetFixed32(data_.data() + offset + 4);
  const uint32_t value_len = GetFixed32(data_.data() + offset + 8);
  // The shared prefix cannot reach back beyond the previous key.
  if (shared > key_scratch_.size()) {
    corrupted_ = true;
    return false;
  }
  const uint64_t payload = static_cast<uint64_t>(non_shared) + value_len;
  if (payload > restart_array_offset_ - offset - kHeaderSize) {
    corrupted_ = true;
    return false;
  }
  key_scratch_.resize(shared);
  key_scratch_.append(data_.substr(offset + kHeaderSize, non_shared));
  cur_key_ = key_scratch_;  // re-point: resize may have reallocated
  cur_value_ = data_.substr(offset + kHeaderSize + non_shared, value_len);
  entry_end_ = offset + kHeaderSize + non_shared + value_len;
  return true;
}

}  // namespace bedrockkv
