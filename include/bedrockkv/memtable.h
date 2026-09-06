// BedrockKV — MemTable: the in-memory write buffer on top of the skiplist.
//
// Entries are self-delimiting composite keys:
//
//   [klen u32 LE][user_key][tag u64 LE][value]
//
// where tag = (seq << 8) | value_type. The length prefix is what lets the
// comparator treat the entry as (user_key, tag) and IGNORE the value tail —
// values must never leak into key comparisons.
//
// Ordering: user key ascending, then tag DESCENDING — so for one user key
// the newest version sorts first and a point lookup is a single Seek.
// Multiple versions of the same key coexist (unique seq each), which later
// generalizes to MVCC snapshot reads (stage 4).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "bedrockkv/encoding.h"
#include "bedrockkv/skiplist.h"

namespace bedrockkv {

constexpr uint8_t kTypeDeletion = 0x0;  // tombstone
constexpr uint8_t kTypeValue = 0x1;

class MemTable {
 public:
  MemTable() = default;

  // seq must be strictly increasing across all writes (the DB owns the
  // counter). Both methods are single-writer; Get is lock-free.
  void Put(uint64_t seq, std::string_view key, std::string_view value);
  void Delete(uint64_t seq, std::string_view key);

  enum class Lookup {
    kFound,    // *value set to the newest visible value
    kDeleted,  // newest version is a tombstone
    kMissing,  // no version of this key at all
  };
  Lookup Get(std::string_view key, std::string* value) const;

  // Rough in-memory footprint estimate — its only consumer is the future
  // flush trigger ("memtable is full, freeze it"), so precision is a
  // non-goal.
  size_t ApproximateSize() const { return approximate_size_; }
  size_t Count() const { return list_.ApproximateSize(); }

  // Invokes fn(seq, type, key, value) for every entry in memtable order:
  // user key ascending, newest version first — exactly the order an SST
  // requires. Decoding is the inverse of Put/Delete's encoding.
  template <typename Fn>
  void ForEach(Fn&& fn) const {
    List::Iterator it(&list_);
    for (it.SeekToFirst(); it.Valid(); it.Next()) {
      const std::string& entry = it.key();
      const uint32_t klen = GetFixed32(entry.data());
      const uint64_t tag = GetFixed64(entry.data() + kLenPrefixSize + klen);
      std::string_view key(entry.data() + kLenPrefixSize, klen);
      std::string_view value(entry.data() + kLenPrefixSize + klen + kTagSize,
                             entry.size() - kLenPrefixSize - klen - kTagSize);
      fn(tag >> 8, static_cast<uint8_t>(tag & 0xff), key, value);
    }
  }

 private:
  static constexpr size_t kLenPrefixSize = 4;
  static constexpr size_t kTagSize = 8;

  struct Comparator {
    // a < b  <=>  (user key of a) < (user key of b), or equal user keys
    // and a's tag is NEWER (numerically larger). Ignores the value tail.
    bool operator()(const std::string& a, const std::string& b) const;
  };
  using List = SkipListT<Comparator>;

  List list_;
  size_t approximate_size_ = 0;
};

}  // namespace bedrockkv
