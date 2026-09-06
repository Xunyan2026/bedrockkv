#include "bedrockkv/memtable.h"

#include "bedrockkv/encoding.h"

namespace bedrockkv {

void MemTable::Put(uint64_t seq, std::string_view key,
                   std::string_view value) {
  std::string entry;
  PutFixed32(&entry, static_cast<uint32_t>(key.size()));
  entry.append(key);
  PutFixed64(&entry, (seq << 8) | kTypeValue);
  entry.append(value);
  list_.Insert(entry);
  approximate_size_ += key.size() + value.size() + kTagSize + kLenPrefixSize + 24;
}

void MemTable::Delete(uint64_t seq, std::string_view key) {
  std::string entry;
  PutFixed32(&entry, static_cast<uint32_t>(key.size()));
  entry.append(key);
  PutFixed64(&entry, (seq << 8) | kTypeDeletion);
  list_.Insert(entry);
  approximate_size_ += key.size() + kTagSize + kLenPrefixSize + 24;
}

MemTable::Lookup MemTable::Get(std::string_view key, std::string* value,
                               uint64_t max_seq) const {
  // Seek just past the newest version with seq <= max_seq: under "tag
  // descending" ordering this lands on the newest VISIBLE version of
  // `key` (or the first user key greater than `key`). max_seq = kMaxSeq
  // (the latest-read default) overflows into tag ~0 — the same "newest"
  // seek the table always did.
  const uint64_t target_tag =
      max_seq >= kMaxSeq ? ~static_cast<uint64_t>(0) : (max_seq << 8) | 0xff;
  std::string target;
  PutFixed32(&target, static_cast<uint32_t>(key.size()));
  target.append(key);
  PutFixed64(&target, target_tag);

  List::Iterator it(&list_);
  it.Seek(target);
  if (!it.Valid()) {
    return Lookup::kMissing;
  }
  const std::string& ik = it.key();
  const uint32_t klen = GetFixed32(ik.data());
  if (klen != key.size() ||
      ik.compare(kLenPrefixSize, klen, key.data(), key.size()) != 0) {
    return Lookup::kMissing;  // landed past `key`: it simply isn't here
  }

  const size_t tag_offset = kLenPrefixSize + klen;
  const uint64_t tag = GetFixed64(ik.data() + tag_offset);
  if ((tag & 0xffu) == kTypeValue) {
    value->assign(ik, tag_offset + kTagSize, std::string::npos);
    return Lookup::kFound;
  }
  return Lookup::kDeleted;
}

bool MemTable::Comparator::operator()(const std::string& a,
                                      const std::string& b) const {
  // Entries are [klen][user_key][tag][value]; the length prefix tells us
  // exactly where the user key ends, so value bytes stay out of the
  // comparison entirely.
  const uint32_t alen = GetFixed32(a.data());
  const uint32_t blen = GetFixed32(b.data());
  const int c = a.compare(kLenPrefixSize, alen, b, kLenPrefixSize, blen);
  if (c != 0) {
    return c < 0;
  }
  return GetFixed64(a.data() + kLenPrefixSize + alen) >
         GetFixed64(b.data() + kLenPrefixSize + blen);  // tag desc: newest first
}

std::unique_ptr<Iterator> MemTable::NewIterator() const {
  return std::make_unique<MemTableIterator>(this);
}

// ---- MemTableIterator ----

MemTableIterator::MemTableIterator(const MemTable* mem)
    : it_(std::make_unique<ListIterator>(&mem->list_)) {}

void MemTableIterator::Decode() {
  if (!it_->Valid()) {
    valid_ = false;
    return;
  }
  const std::string& entry = it_->key();
  const uint32_t klen = GetFixed32(entry.data());
  const size_t tag_off = MemTable::kLenPrefixSize + klen;
  const uint64_t tag = GetFixed64(entry.data() + tag_off);
  key_buf_.assign(entry, MemTable::kLenPrefixSize, klen);
  PutFixed64(&key_buf_, tag);
  value_ = std::string_view(entry.data() + tag_off + MemTable::kTagSize,
                            entry.size() - tag_off - MemTable::kTagSize);
  valid_ = true;
}

void MemTableIterator::SeekToFirst() {
  it_->SeekToFirst();
  Decode();
}

void MemTableIterator::Seek(std::string_view target) {
  // Target is an internal key (user ++ tag). Rebuild the skiplist entry
  // encoding around it — the comparator ignores the value tail, so a
  // value-less entry seeks correctly.
  constexpr size_t kTag = 8;
  std::string entry;
  PutFixed32(&entry, static_cast<uint32_t>(target.size() - kTag));
  entry.append(target.substr(0, target.size() - kTag));
  entry.append(target.substr(target.size() - kTag));
  it_->Seek(entry);
  Decode();
}

void MemTableIterator::Next() {
  it_->Next();
  Decode();
}

}  // namespace bedrockkv
