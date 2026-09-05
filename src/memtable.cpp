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

MemTable::Lookup MemTable::Get(std::string_view key, std::string* value) const {
  // Seek with the largest possible tag: under "tag descending" ordering
  // this lands on the newest version of `key` (or the first user key
  // greater than `key`).
  std::string target;
  PutFixed32(&target, static_cast<uint32_t>(key.size()));
  target.append(key);
  PutFixed64(&target, ~static_cast<uint64_t>(0));

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

}  // namespace bedrockkv
