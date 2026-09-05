// Unit tests for MemTable: model test against an std::map oracle with
// latest-wins semantics, over random operation sequences (deterministic
// seed). This is the only place the composite-key comparator gets
// exercised at scale — tower heights differ on every run, so repeated
// executions shake out order/seek bugs.
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "bedrockkv/memtable.h"
#include "test_util.h"

namespace {

using bedrockkv::MemTable;
using bedrockkv::testing::TestRng;

TEST(MemTableTest, ModelTestLatestWinsAgainstStdMap) {
  TestRng rng(20260906);
  MemTable mem;
  std::map<std::string, std::string> oracle;

  const int kOps = 100000;
  const uint64_t kKeySpace = 500;  // small: forces overwrites and deletes
  uint64_t writes = 0;             // entries actually inserted into the memtable

  for (int i = 0; i < kOps; ++i) {
    const uint64_t seq = static_cast<uint64_t>(i) + 1;
    const std::string key = "k" + std::to_string(rng.Uniform(kKeySpace));
    const int roll = static_cast<int>(rng.Uniform(100));

    if (roll < 55) {
      const std::string value = "v" + std::to_string(rng.Uniform(100000));
      mem.Put(seq, key, value);
      ++writes;
      oracle[key] = value;  // latest wins
    } else if (roll < 75) {
      mem.Delete(seq, key);
      ++writes;
      oracle.erase(key);
    } else {
      std::string v;
      const auto r = mem.Get(key, &v);
      const auto it = oracle.find(key);
      if (it == oracle.end()) {
        // Absent: either never written or newest version is a tombstone.
        EXPECT_TRUE(r == MemTable::Lookup::kMissing ||
                    r == MemTable::Lookup::kDeleted)
            << "op " << i << " key " << key;
      } else {
        EXPECT_EQ(r, MemTable::Lookup::kFound) << "op " << i << " key " << key;
        ASSERT_EQ(v, it->second) << "op " << i << " key " << key;
      }
    }
    ASSERT_EQ(mem.Count(), writes) << "op " << i;  // one entry per write
  }
}

TEST(MemTableTest, EmptyKeyAndMultiVersion) {
  MemTable mem;
  std::string v;
  EXPECT_EQ(mem.Get("", &v), MemTable::Lookup::kMissing);

  mem.Put(1, "", "empty-key-value");
  EXPECT_EQ(mem.Get("", &v), MemTable::Lookup::kFound);
  EXPECT_EQ(v, "empty-key-value");

  // Two versions of one key: newest (highest seq) must win.
  mem.Put(2, "k", "old");
  mem.Put(3, "k", "new");
  EXPECT_EQ(mem.Get("k", &v), MemTable::Lookup::kFound);
  EXPECT_EQ(v, "new");

  // Tombstone on top: deleted.
  mem.Delete(4, "k");
  EXPECT_EQ(mem.Get("k", &v), MemTable::Lookup::kDeleted);

  // Newer put resurrects it.
  mem.Put(5, "k", "reborn");
  EXPECT_EQ(mem.Get("k", &v), MemTable::Lookup::kFound);
  EXPECT_EQ(v, "reborn");
}

}  // namespace
