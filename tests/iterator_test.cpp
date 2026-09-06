// Unit tests for the iterator layer: MemTableIterator, TableIterator,
// and the MergingIterator that Scan and compaction are built on.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bedrockkv/iterator.h"
#include "bedrockkv/memtable.h"
#include "bedrockkv/sstable.h"
#include "test_util.h"

namespace {

using namespace bedrockkv;
using bedrockkv::testing::TestRng;

// Internal key from (user, seq, type) — the encoding every iterator yields.
std::string Ik(std::string_view user, uint64_t seq, uint8_t type) {
  std::string ik(user);
  PutFixed64(&ik, (seq << 8) | type);
  return ik;
}

// Builds an SST over the given (user, seq, type, value) entries, which
// MUST already be in strictly increasing internal-key order.
std::shared_ptr<sst::Table> BuildTable(
    const std::vector<std::tuple<std::string, uint64_t, uint8_t,
                                 std::string>>& entries,
    const sst::TableOptions& opts = {}) {
  sst::Builder builder(opts);
  sst::FileMeta meta;
  meta.file_number = 999;
  for (const auto& [user, seq, type, value] : entries) {
    EXPECT_TRUE(builder.Add(user, seq, type, value).ok());
  }
  static int counter = 0;
  const std::string path =
      ::testing::TempDir() + "iter_sst_" + std::to_string(::getpid()) + "_" +
      std::to_string(counter++);
  EXPECT_TRUE(builder.Finish(path, &meta).ok());
  Status s;
  auto t = sst::Table::Open(999, path, &s);
  EXPECT_NE(t, nullptr);
  return t;
}

// ---- MemTableIterator ----

TEST(IteratorTest, MemTableIteratesNewestFirstAndSkipsNothing) {
  MemTable mem;
  mem.Put(1, "a", "a1");
  mem.Put(2, "a", "a2");   // newest version of "a"
  mem.Put(3, "b", "b1");
  mem.Delete(4, "b");      // tombstone on top of b1
  mem.Put(5, "c", "c1");

  auto it = mem.NewIterator();
  it->SeekToFirst();
  std::vector<std::string> seen;
  while (it->Valid()) {
    seen.push_back(std::string(it->key()));
    it->Next();
  }
  // User key ascending; within "a" and "b" the newest seq first.
  ASSERT_EQ(seen.size(), 5u);
  EXPECT_EQ(seen[0], Ik("a", 2, kTypeValue));
  EXPECT_EQ(seen[1], Ik("a", 1, kTypeValue));
  EXPECT_EQ(seen[2], Ik("b", 4, kTypeDeletion));
  EXPECT_EQ(seen[3], Ik("b", 3, kTypeValue));
  EXPECT_EQ(seen[4], Ik("c", 5, kTypeValue));
}

TEST(IteratorTest, MemTableSeekLandsOnInternalKeyTarget) {
  MemTable mem;
  mem.Put(1, "a", "v");
  mem.Put(3, "b", "v");
  mem.Put(5, "c", "v");

  auto it = mem.NewIterator();
  // Tag ordering is DESCENDING within a user key, so seeking with a huge
  // tag is the "land on the newest version of this user key" pattern —
  // exactly what MemTable::Get does.
  it->Seek(Ik("b", 999, kTypeValue));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Ik("b", 3, kTypeValue));
  EXPECT_EQ(it->value(), "v");
  it->Next();
  EXPECT_EQ(it->key(), Ik("c", 5, kTypeValue));
  // Seek past everything.
  it->Seek(Ik("zz", 999, kTypeValue));
  EXPECT_FALSE(it->Valid());
}

// Zero-padded keys: lexicographic order must equal numeric add order.
std::string PaddedKey(uint64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "k%06llu", static_cast<unsigned long long>(i));
  return buf;
}

// ---- TableIterator ----

TEST(IteratorTest, TableIteratorWalksAllBlocksInOrder) {
  // Tiny blocks force many block transitions; 100 keys with 128-byte
  // blocks means a block holds only a handful of entries.
  sst::TableOptions opts;
  opts.block_size = 128;
  std::vector<std::tuple<std::string, uint64_t, uint8_t, std::string>> entries;
  for (uint64_t i = 0; i < 100; ++i) {
    entries.emplace_back(PaddedKey(i), i + 1, kTypeValue,
                         std::string(20, 'v'));
  }
  auto t = BuildTable(entries);

  auto it = t->NewIterator();
  it->SeekToFirst();
  size_t n = 0;
  while (it->Valid()) {
    EXPECT_EQ(it->key(), Ik(PaddedKey(n), n + 1, kTypeValue));
    EXPECT_EQ(it->value(), std::string(20, 'v'));
    it->Next();
    ++n;
  }
  EXPECT_EQ(n, 100u);
}

TEST(IteratorTest, TableIteratorSeekAcrossBlocks) {
  sst::TableOptions opts;
  opts.block_size = 128;
  std::vector<std::tuple<std::string, uint64_t, uint8_t, std::string>> entries;
  for (uint64_t i = 0; i < 100; ++i) {
    entries.emplace_back(PaddedKey(i), i + 1, kTypeValue,
                         std::string(20, 'v'));
  }
  auto t = BuildTable(entries);

  auto it = t->NewIterator();
  // Huge tag: land on the newest version of k000055 (seq 56).
  it->Seek(Ik(PaddedKey(55), 999, kTypeValue));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Ik(PaddedKey(55), 56, kTypeValue));

  it->Seek(Ik(PaddedKey(98), 999, kTypeValue));
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), Ik(PaddedKey(98), 99, kTypeValue));

  it->Seek(Ik("zzz", 999, kTypeValue));
  EXPECT_FALSE(it->Valid());
}

// ---- MergingIterator ----

TEST(IteratorTest, MergingIteratorInterleavesChildrenInKeyOrder) {
  // Three children with interleaved ranges; ties (same internal key in
  // two children) are fine for the merger — the consumer (Scan/compaction)
  // dedupes by user key.
  MemTable m1, m2;
  m1.Put(10, "a", "a@10");
  m1.Put(30, "c", "c@30");
  m2.Put(20, "b", "b@20");
  m2.Put(40, "c", "c@40");

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(m1.NewIterator());
  children.push_back(m2.NewIterator());
  MergingIterator it(std::move(children));

  it.SeekToFirst();
  std::vector<std::string> order;
  while (it.Valid()) {
    order.push_back(std::string(it.key()));
    it.Next();
  }
  ASSERT_EQ(order.size(), 4u);
  EXPECT_EQ(order[0], Ik("a", 10, kTypeValue));
  EXPECT_EQ(order[1], Ik("b", 20, kTypeValue));
  // Same user key in two children: tag descending puts the newer first.
  EXPECT_EQ(order[2], Ik("c", 40, kTypeValue));
  EXPECT_EQ(order[3], Ik("c", 30, kTypeValue));
}

TEST(IteratorTest, MergingIteratorSeekPositionsAllChildren) {
  MemTable m1, m2;
  m1.Put(1, "a", "v");
  m1.Put(5, "e", "v");
  m2.Put(3, "c", "v");
  m2.Put(7, "g", "v");

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(m1.NewIterator());
  children.push_back(m2.NewIterator());
  MergingIterator it(std::move(children));

  // Huge tag = land on the newest version of "c" (the Get pattern).
  it.Seek(Ik("c", 999, kTypeValue));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), Ik("c", 3, kTypeValue));
  it.Next();
  EXPECT_EQ(it.key(), Ik("e", 5, kTypeValue));
  it.Next();
  EXPECT_EQ(it.key(), Ik("g", 7, kTypeValue));
  it.Next();
  EXPECT_FALSE(it.Valid());
}

TEST(IteratorTest, MergingIteratorEmptyAndSingleChild) {
  {
    MergingIterator it{std::vector<std::unique_ptr<Iterator>>()};
    it.SeekToFirst();
    EXPECT_FALSE(it.Valid());
  }
  MemTable m;
  m.Put(1, "only", "v");
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(m.NewIterator());
  MergingIterator it(std::move(children));
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), Ik("only", 1, kTypeValue));
  it.Next();
  EXPECT_FALSE(it.Valid());
}

}  // namespace
