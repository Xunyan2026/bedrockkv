// Unit tests for the SSTable layer: roundtrips, prefix compression and
// restart points at scale (tiny blocks), Bloom pruning correctness, and
// corruption detection (bit flip / truncation / out-of-order adds).
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bedrockkv/bloom.h"
#include "bedrockkv/sstable.h"
#include "test_util.h"

namespace {

using bedrockkv::Status;
using bedrockkv::kTypeDeletion;
using bedrockkv::kTypeValue;
using bedrockkv::MemTable;
using bedrockkv::sst::Builder;
using bedrockkv::sst::Table;

std::string SstPath(const char* name) {
  return testing::TempDir() + name;
}

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%019llu", static_cast<unsigned long long>(i));
  return buf;
}

// Builds a table with many versions of the same keys (prefix-compression
// stress), tombstones, and enough entries at a tiny block size to span
// many blocks and restart points.
std::shared_ptr<Table> BuildRoundTripTable(const std::string& path,
                                           size_t block_size) {
  bedrockkv::sst::TableOptions opts;
  opts.block_size = block_size;
  Builder builder(opts);
  uint64_t seq = 0;
  // MemTable order: for one user key the newest (largest seq) version
  // comes first, then progressively older ones — strictly descending
  // tags. Keys 0..99 have a tombstone on top of three value versions.
  for (uint64_t i = 0; i < 500; ++i) {
    if (i < 100) {
      EXPECT_TRUE(builder.Add(Key(i), seq + 3, kTypeDeletion, "").ok());
    }
    EXPECT_TRUE(builder.Add(Key(i), seq + 2, kTypeValue, "new" + std::to_string(i)).ok());
    EXPECT_TRUE(builder.Add(Key(i), seq + 1, kTypeValue, "mid" + std::to_string(i)).ok());
    EXPECT_TRUE(builder.Add(Key(i), seq, kTypeValue, "old" + std::to_string(i)).ok());
    seq += (i < 100) ? 4 : 3;
  }
  if (::testing::Test::HasFailure()) {
    return nullptr;
  }
  bedrockkv::sst::FileMeta meta;
  EXPECT_TRUE(builder.Finish(path, &meta).ok());
  EXPECT_EQ(meta.entry_count, 1600u);
  EXPECT_EQ(meta.largest_seq, seq - 1);
  Status s = Status::Ok();
  auto table = Table::Open(42, path, &s);
  EXPECT_TRUE(s.ok()) << s.message();
  return table;
}

TEST(SSTableTest, RoundTripNewestVersionWins) {
  auto table = BuildRoundTripTable(SstPath("sst_roundtrip.sst"), 4096);
  ASSERT_NE(table, nullptr);
  std::string v;

  // Newest version of every key, tombstoned keys report kDeleted.
  for (uint64_t i = 0; i < 500; ++i) {
    const auto r = table->Get(Key(i), &v);
    if (i < 100) {
      EXPECT_EQ(r, MemTable::Lookup::kDeleted) << "key " << i;
    } else {
      ASSERT_EQ(r, MemTable::Lookup::kFound) << "key " << i;
      EXPECT_EQ(v, "new" + std::to_string(i));
    }
  }
  // Absent keys: kMissing.
  EXPECT_EQ(table->Get(Key(500), &v), MemTable::Lookup::kMissing);
  EXPECT_EQ(table->Get("", &v), MemTable::Lookup::kMissing);
  EXPECT_EQ(table->Get("key000000000000000000x", &v), MemTable::Lookup::kMissing);
}

TEST(SSTableTest, RoundTripTinyBlocksAndRestarts) {
  // 128-byte blocks → ~700 blocks, thousands of restart points: any
  // seek/restart-boundary bug shows up here immediately.
  auto table = BuildRoundTripTable(SstPath("sst_tiny.sst"), 128);
  ASSERT_NE(table, nullptr);
  std::string v;
  for (uint64_t i = 0; i < 500; ++i) {
    const auto r = table->Get(Key(i), &v);
    if (i < 100) {
      EXPECT_EQ(r, MemTable::Lookup::kDeleted) << "key " << i;
    } else {
      ASSERT_EQ(r, MemTable::Lookup::kFound) << "key " << i;
      EXPECT_EQ(v, "new" + std::to_string(i));
    }
  }
}

TEST(SSTableTest, EmptyKeyRoundTrip) {
  bedrockkv::sst::TableOptions opts;
  opts.block_size = 256;
  Builder builder(opts);
  ASSERT_TRUE(builder.Add("", 1, kTypeValue, "empty-key").ok());
  ASSERT_TRUE(builder.Add("a", 2, kTypeValue, "a-value").ok());
  bedrockkv::sst::FileMeta meta;
  ASSERT_TRUE(builder.Finish(SstPath("sst_empty_key.sst"), &meta).ok());
  auto table = Table::Open(7, SstPath("sst_empty_key.sst"));
  ASSERT_NE(table, nullptr);
  std::string v;
  EXPECT_EQ(table->Get("", &v), MemTable::Lookup::kFound);
  EXPECT_EQ(v, "empty-key");
  EXPECT_EQ(table->Get("a", &v), MemTable::Lookup::kFound);
  EXPECT_EQ(v, "a-value");
}

TEST(SSTableTest, RejectsOutOfOrderAdds) {
  Builder builder;
  ASSERT_TRUE(builder.Add("b", 1, kTypeValue, "1").ok());
  // Same key again without a new seq is NOT increasing.
  auto s = builder.Add("b", 1, kTypeValue, "2");
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
  // Older seq for the same user key is also out of order.
  s = builder.Add("a", 2, kTypeValue, "3");
  EXPECT_EQ(s.code(), Status::Code::kInvalidArgument);
}

TEST(SSTableTest, DetectsBitFlipAnywhere) {
  const std::string path = SstPath("sst_flip.sst");
  auto table = BuildRoundTripTable(path, 4096);
  ASSERT_NE(table, nullptr);
  table.reset();

  // Flip one bit in the data region and one in the footer region; each
  // must fail Open() (whole-file CRC).
  for (const size_t offset : {size_t{100}, size_t{250}}) {
    FILE* fp = std::fopen(path.c_str(), "r+b");
    ASSERT_NE(fp, nullptr);
    std::fseek(fp, static_cast<long>(offset), SEEK_SET);
    int c = std::fgetc(fp);
    std::fseek(fp, static_cast<long>(offset), SEEK_SET);
    std::fputc(c ^ 0x01, fp);
    std::fclose(fp);

    Status s = Status::Ok();
    EXPECT_EQ(Table::Open(1, path, &s), nullptr) << "offset " << offset;
    EXPECT_EQ(s.code(), Status::Code::kCorruption);

    // Undo the flip for the next iteration.
    fp = std::fopen(path.c_str(), "r+b");
    std::fseek(fp, static_cast<long>(offset), SEEK_SET);
    std::fputc(c, fp);
    std::fclose(fp);
  }
}

TEST(SSTableTest, DetectsTruncatedTail) {
  const std::string path = SstPath("sst_trunc.sst");
  auto table = BuildRoundTripTable(path, 4096);
  ASSERT_NE(table, nullptr);
  table.reset();

  // Cut the file short: the footer (or part of it) is gone.
  struct stat st;
  ASSERT_EQ(::stat(path.c_str(), &st), 0);
  ASSERT_EQ(::truncate(path.c_str(), st.st_size - 10), 0);
  Status s = Status::Ok();
  EXPECT_EQ(Table::Open(1, path, &s), nullptr);
  EXPECT_EQ(s.code(), Status::Code::kCorruption);
}

TEST(SSTableTest, BloomNeverFalseNegative) {
  bedrockkv::testing::TestRng rng(12345);
  std::vector<std::string> keys;
  for (int i = 0; i < 2000; ++i) {
    keys.push_back("k" + std::to_string(rng.Uniform(1000000)));
  }
  std::vector<std::string_view> views(keys.begin(), keys.end());
  const std::string filter = bedrockkv::bloom::BuildFilter(views, 10);
  for (const std::string& k : keys) {
    ASSERT_TRUE(bedrockkv::bloom::KeyMayMatch(filter, k)) << k;
  }
  // False-positive sanity: expect ~0.8%, allow a wide margin (5%).
  int fp = 0;
  for (int i = 0; i < 2000; ++i) {
    const std::string absent = "absent" + std::to_string(rng.Uniform(1000000));
    if (bedrockkv::bloom::KeyMayMatch(filter, absent)) {
      ++fp;
    }
  }
  EXPECT_LT(fp, 100) << "bloom false-positive rate exploded: " << fp << "/2000";
}

}  // namespace
