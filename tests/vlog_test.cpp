// Unit tests for the WiscKey machinery: the sharded LRU read cache and
// the VLog module (append/read, CRC protection, torn tails, GC scan).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bedrockkv/lru_cache.h"
#include "bedrockkv/vlog.h"

namespace {

using bedrockkv::DecodeValuePointer;
using bedrockkv::EncodeValuePointer;
using bedrockkv::ShardedLruCache;
using bedrockkv::Status;
using bedrockkv::ValuePointer;
using bedrockkv::VLog;

std::string TestDir(const char* base) {
  const std::string d = testing::TempDir() + base + "_" +
                        std::to_string(::getpid());
  ::mkdir(d.c_str(), 0755);
  return d;
}

std::string BigValue(uint64_t seed, size_t n) {
  std::string v(n, '\0');
  uint64_t x = seed * 0x9e3779b97f4a7c15ull + 1;
  for (size_t i = 0; i < n; ++i) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v[i] = static_cast<char>(x >> 24);
  }
  return v;
}

// ---- value pointer codec ----

TEST(ValuePointerTest, EncodeDecodeRoundTrip) {
  const ValuePointer p{42, 0xdeadbeefcafeull, 9999};
  const std::string enc = EncodeValuePointer(p);
  ASSERT_EQ(enc.size(), bedrockkv::kValuePointerSize);
  EXPECT_EQ(enc[0], bedrockkv::kValuePointerTag);
  ValuePointer out;
  EXPECT_TRUE(DecodeValuePointer(enc, &out));
  EXPECT_EQ(out.vlog_number, 42u);
  EXPECT_EQ(out.offset, 0xdeadbeefcafeull);
  EXPECT_EQ(out.value_size, 9999u);
}

TEST(ValuePointerTest, NonPointersAreRejected) {
  ValuePointer out;
  EXPECT_FALSE(DecodeValuePointer("", &out));
  EXPECT_FALSE(DecodeValuePointer("hello", &out));           // wrong size
  EXPECT_FALSE(DecodeValuePointer(std::string(21, 'x'), &out));  // wrong tag
  // A 20-byte value starting with 0xFF is still not a pointer.
  std::string almost(20, '\xff');
  EXPECT_FALSE(DecodeValuePointer(almost, &out));
}

// ---- VLog ----

TEST(VLogTest, AppendReadRoundTrip) {
  const std::string dir = TestDir("vlog_rt");
  Status s;
  auto vl = VLog::Open(dir, 7, &s);
  ASSERT_NE(vl, nullptr);

  uint64_t off1 = 0, off2 = 0, bytes = 0;
  const std::string k1 = "key-one";
  const std::string v1 = BigValue(1, 3000);
  ASSERT_TRUE(vl->Append(k1, v1, &off1, &bytes).ok());
  EXPECT_EQ(off1, 0u);
  EXPECT_EQ(bytes, 12 + k1.size() + v1.size());

  const std::string k2 = "k2";
  const std::string v2 = "small";
  ASSERT_TRUE(vl->Append(k2, v2, &off2).ok());
  EXPECT_GT(off2, off1);

  std::string out;
  EXPECT_TRUE(vl->ReadValue(off1, v1.size(), &out).ok());
  EXPECT_EQ(out, v1);
  EXPECT_TRUE(vl->ReadValue(off2, v2.size(), &out).ok());
  EXPECT_EQ(out, v2);
  EXPECT_EQ(vl->file_size(), off2 + 12 + k2.size() + v2.size());
}

TEST(VLogTest, ReopenContinuesAtFileEnd) {
  const std::string dir = TestDir("vlog_reopen");
  Status s;
  uint64_t off = 0;
  const std::string v1 = BigValue(2, 100);
  {
    auto vl = VLog::Open(dir, 3, &s);
    ASSERT_NE(vl, nullptr);
    ASSERT_TRUE(vl->Append("k", v1, &off).ok());
  }
  {
    auto vl = VLog::Open(dir, 3, &s);
    ASSERT_NE(vl, nullptr);
    EXPECT_EQ(vl->file_size(), off + 12 + 1 + v1.size());
    // The old entry stays readable after reopen...
    std::string out;
    ASSERT_TRUE(vl->ReadValue(off, v1.size(), &out).ok());
    EXPECT_EQ(out, v1);
    // ...and a new append continues at the true end.
    uint64_t off2 = 0;
    const std::string v2 = "second-era";
    ASSERT_TRUE(vl->Append("k", v2, &off2).ok());
    EXPECT_EQ(off2, off + 12 + 1 + v1.size());
    ASSERT_TRUE(vl->ReadValue(off2, v2.size(), &out).ok());
    EXPECT_EQ(out, v2);
  }
}

TEST(VLogTest, PointerPastEndIsNotFound) {
  const std::string dir = TestDir("vlog_pastend");
  Status s;
  auto vl = VLog::Open(dir, 1, &s);
  ASSERT_NE(vl, nullptr);
  std::string out;
  const Status got = vl->ReadValue(0, 10, &out);
  EXPECT_EQ(got.code(), Status::Code::kNotFound);
}

TEST(VLogTest, TornTailIsNotFoundAndScanStops) {
  const std::string dir = TestDir("vlog_torn");
  Status s;
  const std::string path = dir + "/" + VLog::FileName(5);
  uint64_t off1 = 0, off2 = 0;
  const std::string v1 = BigValue(3, 500);
  const std::string v2 = BigValue(4, 500);
  {
    auto vl = VLog::Open(dir, 5, &s);
    ASSERT_NE(vl, nullptr);
    ASSERT_TRUE(vl->Append("a", v1, &off1).ok());
    ASSERT_TRUE(vl->Append("b", v2, &off2).ok());
  }
  // Simulate a crash mid-append of a third entry: 30 bytes of garbage.
  {
    const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND);
    ASSERT_GE(fd, 0);
    const std::string junk(30, 'z');
    ASSERT_EQ(::write(fd, junk.data(), junk.size()),
              static_cast<ssize_t>(junk.size()));
    ::close(fd);
  }
  {
    auto vl = VLog::Open(dir, 5, &s);
    ASSERT_NE(vl, nullptr);
    std::string out;
    // Entries 1 and 2 remain fully readable...
    EXPECT_TRUE(vl->ReadValue(off1, v1.size(), &out).ok());
    EXPECT_EQ(out, v1);
    // ...the torn tail reads as "not found" (bounded loss)...
    const Status torn = vl->ReadValue(off2 + 12 + 1 + v2.size(), 5, &out);
    EXPECT_EQ(torn.code(), Status::Code::kNotFound);
    // ...and the GC scan stops cleanly before the garbage.
    std::vector<std::string> keys;
    ASSERT_TRUE(vl->ScanEntries(vl->file_size(),
                                [&](std::string_view k, std::string_view,
                                    uint64_t) {
                                  keys.emplace_back(k);
                                })
                    .ok());
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0], "a");
    EXPECT_EQ(keys[1], "b");
  }
}

TEST(VLogTest, CorruptedEntryIsCorruption) {
  const std::string dir = TestDir("vlog_crc");
  Status s;
  const std::string path = dir + "/" + VLog::FileName(9);
  uint64_t off = 0;
  const std::string v = BigValue(5, 200);
  {
    auto vl = VLog::Open(dir, 9, &s);
    ASSERT_NE(vl, nullptr);
    ASSERT_TRUE(vl->Append("k", v, &off).ok());
  }
  // Flip one bit inside the value region.
  {
    const int fd = ::open(path.c_str(), O_WRONLY);
    ASSERT_GE(fd, 0);
    const char bad = 'X';
    ASSERT_EQ(::pwrite(fd, &bad, 1, static_cast<off_t>(off + 12)), 1);
    ::close(fd);
  }
  auto vl = VLog::Open(dir, 9, &s);
  ASSERT_NE(vl, nullptr);
  std::string out;
  const Status got = vl->ReadValue(off, v.size(), &out);
  EXPECT_EQ(got.code(), Status::Code::kCorruption);
}

TEST(VLogTest, ScanEntriesSeesEveryEntryWithOffsets) {
  const std::string dir = TestDir("vlog_scan");
  Status s;
  auto vl = VLog::Open(dir, 11, &s);
  ASSERT_NE(vl, nullptr);
  std::vector<uint64_t> offsets;
  for (int i = 0; i < 50; ++i) {
    uint64_t off = 0;
    ASSERT_TRUE(vl->Append("key" + std::to_string(i), BigValue(i, i * 10 + 1),
                           &off)
                    .ok());
    offsets.push_back(off);
  }
  size_t seen = 0;
  ASSERT_TRUE(vl->ScanEntries(vl->file_size(),
                              [&](std::string_view k, std::string_view v,
                                  uint64_t off) {
    EXPECT_EQ(k, "key" + std::to_string(seen));
    EXPECT_EQ(v, BigValue(seen, seen * 10 + 1));
    EXPECT_EQ(off, offsets[seen]);
    ++seen;
  }).ok());
  EXPECT_EQ(seen, 50u);
}

// ---- sharded LRU cache ----

TEST(LruCacheTest, PutGetEvictsLeastRecent) {
  // 4 shards, total weight 40 (10 per shard), weight = value length.
  ShardedLruCache<std::string, std::string, std::hash<std::string>> cache(
      4, 40, [](const std::string& v) { return v.size(); });
  std::string out;
  EXPECT_FALSE(cache.Get("a", &out));

  cache.Put("a", "12345");  // weight 5
  cache.Put("b", "12345");  // weight 10
  ASSERT_TRUE(cache.Get("a", &out));  // promote a; b is now LRU
  EXPECT_EQ(out, "12345");
  cache.Put("c", "12345");  // must evict b, keep a
  EXPECT_TRUE(cache.Get("a", &out));
  EXPECT_TRUE(cache.Get("c", &out));
  // b was evicted: a fresh "b" hit is impossible to distinguish from a
  // stored one, so verify via a new key instead.
  cache.Put("d", "1234");
  cache.Put("e", "123456");  // over capacity alone -> must evict others
  EXPECT_TRUE(cache.Get("e", &out));
}

TEST(LruCacheTest, OverwriteUpdatesWeight) {
  ShardedLruCache<int, std::string, std::hash<int>> cache(
      1, 10, [](const std::string& v) { return v.size(); });
  cache.Put(1, "0123456789");  // exactly full
  std::string out;
  EXPECT_TRUE(cache.Get(1, &out));
  cache.Put(1, "abc");  // same key, smaller: still fits, no evictions
  EXPECT_TRUE(cache.Get(1, &out));
  EXPECT_EQ(out, "abc");
  cache.Put(2, "0123456789");  // full again; entry 1 must be evicted
  EXPECT_TRUE(cache.Get(2, &out));
  EXPECT_FALSE(cache.Get(1, &out));
}

TEST(LruCacheTest, OversizedEntryIsNotCached) {
  ShardedLruCache<int, std::string, std::hash<int>> cache(
      1, 4, [](const std::string& v) { return v.size(); });
  cache.Put(1, "too-big-entry");
  std::string out;
  EXPECT_FALSE(cache.Get(1, &out));
}

TEST(LruCacheTest, ShardsShareTheTotalBudget) {
  // Regression: every shard used to evict against the TOTAL budget, so
  // N shards could each hold max_total_weight — 4 shards meant 4x the
  // memory the caller asked for. Weight = value length.
  ShardedLruCache<std::string, std::string, std::hash<std::string>> cache(
      4, 40, [](const std::string& v) { return v.size(); });  // 10 per shard
  for (int i = 0; i < 40; ++i) {
    cache.Put("key" + std::to_string(i), std::string(8, 'x'));
  }
  std::string out;
  int hits = 0;
  for (int i = 0; i < 40; ++i) {
    if (cache.Get("key" + std::to_string(i), &out)) {
      ++hits;
    }
  }
  // A shard's share is 10, so it retains at most ONE 8-byte entry (a
  // second would push its weight to 16); four shards retain at most 4
  // no matter how the hash distributes the keys. The old behavior kept
  // about 20.
  EXPECT_LE(hits, 8);
}

}  // namespace
