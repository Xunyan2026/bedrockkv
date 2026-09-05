// Unit tests for bedrockkv::SkipList.
//
// Layers of defense, in order of importance:
//   1. Model test vs std::map oracle (random Insert/Contains interleavings);
//   2. Iterator order + Seek boundary tests (first/last/between/exact);
//   3. Concurrency smoke test (3 lock-free readers racing one writer).
// Keys are zero-padded so that lexicographic string order == numeric order.
#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "bedrockkv/skiplist.h"
#include "test_util.h"

namespace {

using bedrockkv::SkipList;
using bedrockkv::testing::TestRng;

// %010u keeps lexicographic order equal to numeric order (a pitfall worth
// remembering: with raw numbers, "9" > "30" as strings).
std::string MakeKey(uint64_t n) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%010llu", static_cast<unsigned long long>(n));
  return buf;
}

TEST(SkipListTest, ModelTestAgainstStdMap) {
  TestRng rng(20260905);
  SkipList list;
  std::map<std::string, int> oracle;

  const int kOps = 50000;
  const uint64_t kKeySpace = 2000;  // small on purpose: forces duplicates

  for (int i = 0; i < kOps; ++i) {
    const std::string key = MakeKey(rng.Uniform(kKeySpace));
    if (rng.Percent(60) || oracle.empty()) {
      const bool inserted = list.Insert(key);
      const bool expected = oracle.emplace(key, i).second;  // false if existed
      ASSERT_EQ(inserted, expected) << "op " << i << ", key " << key;
    } else {
      ASSERT_EQ(list.Contains(key), oracle.count(key) > 0)
          << "op " << i << ", key " << key;
    }
    // Invariant checked mid-stream: sizes must always agree.
    ASSERT_EQ(list.ApproximateSize(), oracle.size()) << "op " << i;
  }
}

TEST(SkipListTest, IteratorMatchesMapOrder) {
  TestRng rng(42);
  SkipList list;
  std::map<std::string, int> oracle;

  const int kInserts = 1000;
  const uint64_t kKeySpace = 500;
  for (int i = 0; i < kInserts; ++i) {
    const std::string key = MakeKey(rng.Uniform(kKeySpace));
    list.Insert(key);
    oracle.emplace(key, i);
  }

  SkipList::Iterator it(&list);
  it.SeekToFirst();
  for (const auto& [key, _] : oracle) {
    ASSERT_TRUE(it.Valid()) << "iterator ended early, expected key " << key;
    EXPECT_EQ(it.key(), key);
    it.Next();
  }
  EXPECT_FALSE(it.Valid()) << "iterator has more keys than the oracle";
}

TEST(SkipListTest, SeekBoundaries) {
  SkipList list;
  for (uint64_t n : {10, 20, 30, 40}) {
    ASSERT_TRUE(list.Insert(MakeKey(n)));
  }

  SkipList::Iterator it(&list);

  // Exact match seeks to itself.
  it.Seek(MakeKey(20));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), MakeKey(20));

  // Between keys seeks to the next greater one.
  it.Seek(MakeKey(25));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), MakeKey(30));

  // Below everything seeks to the first key.
  it.Seek(MakeKey(0));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), MakeKey(10));

  // Above everything is invalid.
  it.Seek(MakeKey(41));
  EXPECT_FALSE(it.Valid());

  // SeekToFirst after a dead-end Seek revives the iterator.
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), MakeKey(10));
}

TEST(SkipListTest, DuplicateAndEmptyKeySemantics) {
  SkipList list;
  EXPECT_TRUE(list.Insert("banana"));
  EXPECT_FALSE(list.Insert("banana"));  // duplicate rejected
  EXPECT_TRUE(list.Insert(""));         // empty key is a legal key
  EXPECT_TRUE(list.Contains(""));
  EXPECT_TRUE(list.Contains("banana"));
  EXPECT_FALSE(list.Contains("apple"));

  SkipList::Iterator it(&list);
  it.SeekToFirst();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "");  // "" sorts before "banana"
  it.Next();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "banana");
  it.Next();
  EXPECT_FALSE(it.Valid());
}

TEST(SkipListTest, ConcurrentReadersDuringWrites) {
  SkipList list;

  const int kPre = 1000;
  for (int i = 0; i < kPre; ++i) {
    ASSERT_TRUE(list.Insert(MakeKey(i)));
  }

  constexpr uint64_t kAbsentSentinel = ~static_cast<uint64_t>(0);
  std::atomic<bool> stop{false};
  std::atomic<int> violations{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r) {
    readers.emplace_back([&list, &stop, &violations, r] {
      TestRng rng(9000 + r);
      while (!stop.load(std::memory_order_relaxed)) {
        // (a) A pre-inserted key must ALWAYS be found, any time, from any
        // reader — the linchpin of lock-free reading.
        if (!list.Contains(MakeKey(rng.Uniform(kPre)))) {
          violations.fetch_add(1);
        }
        // (b) A key that will never be inserted must NEVER be found.
        if (list.Contains(MakeKey(kAbsentSentinel))) {
          violations.fetch_add(1);
        }
      }
    });
  }

  // Single writer (the contract) inserts 200k fresh keys while readers spin.
  const int kWrites = 200000;
  for (int i = 1000000; i < 1000000 + kWrites; ++i) {
    list.Insert(MakeKey(i));
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(violations.load(), 0);
  EXPECT_EQ(list.ApproximateSize(),
            static_cast<size_t>(kPre) + static_cast<size_t>(kWrites));
  // Spot-check the freshly written keys now that the writer is done.
  for (int i = 1000000; i < 1000000 + kWrites; i += 997) {
    ASSERT_TRUE(list.Contains(MakeKey(i))) << "missing key " << i;
  }
}

TEST(SkipListTest, CustomComparatorDescendingOrder) {
  // The template must work with any strict weak ordering. A reversed
  // comparator makes the iterator walk keys from largest to smallest —
  // exactly the pattern the MemTable will use (user key asc, tag desc).
  struct ReverseComp {
    bool operator()(const std::string& a, const std::string& b) const {
      return a > b;
    }
  };
  bedrockkv::SkipListT<ReverseComp> list;
  for (uint64_t n : {5, 1, 3}) {
    ASSERT_TRUE(list.Insert(MakeKey(n)));
  }
  EXPECT_FALSE(list.Insert(MakeKey(3)));  // duplicate under the comparator

  bedrockkv::SkipListT<ReverseComp>::Iterator it(&list);
  it.SeekToFirst();
  for (uint64_t expected : {5, 3, 1}) {
    ASSERT_TRUE(it.Valid());
    EXPECT_EQ(it.key(), MakeKey(expected));
    it.Next();
  }
  EXPECT_FALSE(it.Valid());

  // Seek targets the first key >= target under the reversed ordering,
  // i.e. the largest key <= the target numerically.
  it.Seek(MakeKey(4));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), MakeKey(3));
}

}  // namespace
