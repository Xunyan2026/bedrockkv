// Unit tests for the stage-1 DB: basic ops, reopen persistence, torn-tail
// recovery, and the fork + kill -9 crash test with prefix-integrity
// verification (design doc ch.7).
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <map>
#include <string>

#include <gtest/gtest.h>

#include "bedrockkv/db.h"
#include "test_util.h"

namespace {

using bedrockkv::DB;
using bedrockkv::Options;
using bedrockkv::Status;
using bedrockkv::SyncMode;
using bedrockkv::testing::TestRng;

// Unique per process and per call: ctest runs each test in its own
// process, and a leftover WAL from a previous run would poison the
// reopen-persistence assertions (learned the hard way).
std::string FreshDBDir(const char* base) {
  static int counter = 0;
  const std::string d = testing::TempDir() + base + "_" +
                        std::to_string(::getpid()) + "_" + std::to_string(counter++);
  ::mkdir(d.c_str(), 0755);
  return d;
}

std::string Key(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%020llu", static_cast<unsigned long long>(i));
  return buf;
}

bool IsNotFound(const Status& s) { return s.code() == Status::Code::kNotFound; }

TEST(DBTest, FreshOpenAndBasicOps) {
  auto db = DB::Open(FreshDBDir("db_fresh"));
  ASSERT_NE(db, nullptr);

  std::string v;
  EXPECT_TRUE(IsNotFound(db->Get("missing", &v)));

  EXPECT_TRUE(db->Put("apple", "red").ok());
  EXPECT_TRUE(db->Put("banana", "yellow").ok());
  EXPECT_TRUE(db->Get("apple", &v).ok());
  EXPECT_EQ(v, "red");

  EXPECT_TRUE(db->Delete("apple").ok());
  EXPECT_TRUE(IsNotFound(db->Get("apple", &v)));

  // Overwrite after delete: newest version wins.
  EXPECT_TRUE(db->Put("apple", "green").ok());
  EXPECT_TRUE(db->Get("apple", &v).ok());
  EXPECT_EQ(v, "green");

  // Empty key and empty value are legal.
  EXPECT_TRUE(db->Put("", "").ok());
  EXPECT_TRUE(db->Get("", &v).ok());
  EXPECT_EQ(v, "");
}

TEST(DBTest, PersistenceAcrossReopen) {
  const std::string dir = FreshDBDir("db_persist");
  {
    auto db = DB::Open(dir);
    ASSERT_NE(db, nullptr);
    for (uint64_t i = 0; i < 1000; ++i) {
      ASSERT_TRUE(db->Put(Key(i), std::string(50, 'v')).ok());
    }
  }  // destructor closes; data must survive

  {
    auto db = DB::Open(dir);
    ASSERT_NE(db, nullptr);
    std::string v;
    for (uint64_t i = 0; i < 1000; i += 37) {
      ASSERT_TRUE(db->Get(Key(i), &v).ok()) << "lost key " << i;
      EXPECT_EQ(v, std::string(50, 'v'));
    }
    EXPECT_FALSE(db->wal_truncated_on_recovery());
    // Delete the even keys; tombstones must survive reopen too.
    for (uint64_t i = 0; i < 1000; i += 2) {
      ASSERT_TRUE(db->Delete(Key(i)).ok());
    }
  }
  {
    auto db = DB::Open(dir);
    ASSERT_NE(db, nullptr);
    std::string v;
    EXPECT_TRUE(IsNotFound(db->Get(Key(0), &v)));
    EXPECT_TRUE(db->Get(Key(1), &v).ok());
    EXPECT_EQ(db->latest_seq(), 1500u);  // 1000 puts + 500 deletes
  }
}

TEST(DBTest, RecoversFromGarbageTail) {
  const std::string dir = FreshDBDir("db_garbage");
  uint64_t log_no = 0;
  {
    auto db = DB::Open(dir);
    ASSERT_NE(db, nullptr);
    log_no = db->log_number();
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Put(Key(i), std::string(50, 'v')).ok());
    }
  }

  // Append a plausible-looking header with no payload — a torn-write
  // replica. Recovery must cut it off and keep all 100 records.
  const std::string wal = dir + "/" + DB::LogFileName(log_no);
  FILE* fp = std::fopen(wal.c_str(), "ab");
  ASSERT_NE(fp, nullptr);
  const char hdr[9] = {10, 0, 0, 0, 0, 0, 0, 0, 1};  // length=10, crc=0, full
  std::fwrite(hdr, 1, sizeof(hdr), fp);
  std::fclose(fp);

  auto db = DB::Open(dir);
  ASSERT_NE(db, nullptr);
  EXPECT_TRUE(db->wal_truncated_on_recovery());
  std::string v;
  EXPECT_TRUE(db->Get(Key(99), &v).ok());
  EXPECT_TRUE(IsNotFound(db->Get(Key(100), &v)));

  // And the DB remains writable after recovery (O_APPEND lands at the
  // new end — no zero-hole punched into the log).
  EXPECT_TRUE(db->Put(Key(100), "ok").ok());
}

TEST(DBTest, CutMidRecordRecoversExactPrefix) {
  const std::string dir = FreshDBDir("db_cut");
  uint64_t log_no = 0;
  {
    auto db = DB::Open(dir);
    ASSERT_NE(db, nullptr);
    log_no = db->log_number();
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Put(Key(i), std::string(50, 'v')).ok());
    }
  }

  // One record: payload = 8(seq)+1(type)+4(klen)+21(key)+4(vlen)+50(value)
  // = 88 bytes; physical = 88 + 9(header) = 97. Cut 30 bytes into record
  // #99: complete header, torn payload.
  const size_t kKeyLen = 21;
  const size_t kPhysical = 9 + 8 + 1 + 4 + kKeyLen + 4 + 50;
  ASSERT_EQ(::truncate((dir + "/" + DB::LogFileName(log_no)).c_str(),
                       static_cast<off_t>(99 * kPhysical + 30)),
            0);

  auto db = DB::Open(dir);
  ASSERT_NE(db, nullptr);
  EXPECT_TRUE(db->wal_truncated_on_recovery());
  std::string v;
  EXPECT_TRUE(db->Get(Key(98), &v).ok());
  EXPECT_TRUE(IsNotFound(db->Get(Key(99), &v)));
  EXPECT_EQ(db->latest_seq(), 99u);  // only puts 1..99 were replayed intact
}

TEST(DBTest, Kill9CrashRecoversExactPrefix) {
  const std::string dir = FreshDBDir("db_kill9");
  const pid_t pid = ::fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    // Child: write forever with fsync off. kill -9 cannot lose page-cache
    // data — only a power cut could, and that is exactly what SyncMode
    // buys. The child must use _exit() (no gtest state, no destructors).
    Options opts;
    opts.sync_mode = SyncMode::kSyncNever;
    auto db = DB::Open(dir, opts);
    if (!db) {
      _exit(1);
    }
    const std::string value(128, 'v');
    for (uint64_t i = 0;; ++i) {
      if (!db->Put(Key(i), value).ok()) {
        _exit(2);
      }
    }
  }

  // Parent: wait until the WAL has ~64KB of records, then kill -9. The
  // child performs the first Open on a fresh dir, so the current log
  // generation is 1.
  const std::string wal = dir + "/" + DB::LogFileName(1);
  bool grew = false;
  for (int tries = 0; tries < 10000; ++tries) {
    struct stat st;
    if (::stat(wal.c_str(), &st) == 0 && st.st_size >= 64 * 1024) {
      grew = true;
      break;
    }
    ::usleep(1000);
  }
  ASSERT_TRUE(grew) << "child never wrote enough data";
  ASSERT_EQ(::kill(pid, SIGKILL), 0);
  int wstatus = 0;
  ASSERT_EQ(::waitpid(pid, &wstatus, 0), pid);
  ASSERT_TRUE(WIFSIGNALED(wstatus));
  ASSERT_EQ(WTERMSIG(wstatus), SIGKILL);

  // Reopen: whatever survived must form an exact prefix 0..m-1 with no
  // holes — records were appended sequentially, so recovery replays a
  // prefix by construction. This test exists to catch violations.
  auto db = DB::Open(dir);
  ASSERT_NE(db, nullptr);
  std::string v;
  size_t m = 0;
  while (db->Get(Key(m), &v).ok()) {
    ASSERT_EQ(v, std::string(128, 'v'));
    ++m;
  }
  ASSERT_GE(m, 1u) << "no records survived the crash";
  for (size_t i = m; i < m + 200; ++i) {
    EXPECT_TRUE(IsNotFound(db->Get(Key(i), &v))) << "hole at index " << i;
  }
}

TEST(DBTest, FlushWritesSstAndServesReadsAfterReopen) {
  const std::string dir = FreshDBDir("db_flush");
  Options opts;
  opts.write_buffer_size = 64 * 1024;  // flush every ~64KB of entries
  // This test pins the OLD flush-only semantics (L0 grows, nothing
  // compacted); the compaction model test covers the leveled path.
  opts.l0_compaction_trigger = 1000;
  uint64_t flushed_seq = 0;
  {
    auto db = DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    for (uint64_t i = 0; i < 2000; ++i) {
      ASSERT_TRUE(db->Put(Key(i), std::string(60, 'v')).ok());
    }
    db->wait_for_background_work();
    EXPECT_GE(db->level_file_count(0), 2u) << "flush never triggered";
    EXPECT_GT(db->log_number(), 1u) << "log never rotated";
    flushed_seq = db->latest_seq();
    std::string v;
    EXPECT_TRUE(db->Get(Key(1999), &v).ok());
  }

  // The old logs must be gone; only SSTs + the current log remain.
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  EXPECT_GE(db->level_file_count(0), 2u);
  std::string v;
  for (uint64_t i = 0; i < 2000; i += 17) {
    ASSERT_TRUE(db->Get(Key(i), &v).ok()) << "lost key " << i;
    EXPECT_EQ(v, std::string(60, 'v'));
  }
  EXPECT_EQ(db->latest_seq(), flushed_seq);
  EXPECT_FALSE(db->wal_truncated_on_recovery());
}

TEST(DBTest, VersionsAcrossFlushBoundary) {
  const std::string dir = FreshDBDir("db_versions");
  Options opts;
  opts.write_buffer_size = 16 * 1024;
  opts.l0_compaction_trigger = 1000;  // L0 must accumulate: pin flush-only
  opts.level_base_size = 1u << 30;    // semantics for this test
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  std::string v;

  // v1 lands in an SST, v2 in the memtable: the memtable must win.
  EXPECT_TRUE(db->Put("k", "v1").ok());
  uint64_t filler = 0;
  while (db->level_file_count(0) == 0) {
    ASSERT_TRUE(db->Put(Key(filler++), std::string(100, 'f')).ok());
  }
  EXPECT_TRUE(db->Put("k", "v2").ok());
  EXPECT_TRUE(db->Get("k", &v).ok());
  EXPECT_EQ(v, "v2");

  // Tombstone on top of an SST value: must stay deleted, including
  // across a reopen (tombstone replayed from the log).
  EXPECT_TRUE(db->Delete("k").ok());
  EXPECT_TRUE(IsNotFound(db->Get("k", &v)));

  db.reset();
  db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  EXPECT_TRUE(IsNotFound(db->Get("k", &v))) << "tombstone lost on reopen";

  // And an SST tombstone must shadow an older SST value even after both
  // are flushed and the process restarted.
  EXPECT_TRUE(db->Put("k", "v3").ok());
  while (db->level_file_count(0) < 3) {
    ASSERT_TRUE(db->Put(Key(filler++), std::string(100, 'f')).ok());
  }
  EXPECT_TRUE(db->Delete("k").ok());
  while (db->level_file_count(0) < 4) {
    ASSERT_TRUE(db->Put(Key(filler++), std::string(100, 'f')).ok());
  }
  db.reset();
  db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  EXPECT_TRUE(IsNotFound(db->Get("k", &v)));
}

// Stage-2 model test: random ops against an std::map oracle with a tiny
// write buffer so flushes and log rotations interleave with every kind
// of operation, then a full verification pass after a reopen.
TEST(DBTest, ModelTestWithFlushesAgainstStdMap) {
  const std::string dir = FreshDBDir("db_model");
  Options opts;
  opts.write_buffer_size = 32 * 1024;
  TestRng rng(20260907);
  std::map<std::string, std::string> oracle;

  {
    auto db = DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    const int kOps = 20000;
    const uint64_t kKeySpace = 800;
    for (int i = 0; i < kOps; ++i) {
      const std::string key = "k" + std::to_string(rng.Uniform(kKeySpace));
      const int roll = static_cast<int>(rng.Uniform(100));
      if (roll < 50) {
        const std::string value = "v" + std::to_string(rng.Uniform(100000));
        ASSERT_TRUE(db->Put(key, value).ok());
        oracle[key] = value;
      } else if (roll < 65) {
        ASSERT_TRUE(db->Delete(key).ok());
        oracle.erase(key);
      } else {
        std::string v;
        const Status s = db->Get(key, &v);
        const auto it = oracle.find(key);
        if (it == oracle.end()) {
          EXPECT_TRUE(IsNotFound(s)) << "op " << i << " key " << key;
        } else {
          ASSERT_TRUE(s.ok()) << "op " << i << " key " << key;
          EXPECT_EQ(v, it->second) << "op " << i << " key " << key;
        }
      }
    }
    // Compaction collapses the flush output (correctly), so instead of
    // counting files assert the mechanics ran: the log rotated many times
    // and compaction moved data into L1.
    db->wait_for_background_work();
    EXPECT_GT(db->log_number(), 1u) << "log never rotated";
    EXPECT_GE(db->level_file_count(1), 1u)
        << "nothing was ever compacted into L1";
  }

  // Reopen: the oracle must survive exactly, served from SSTs + log.
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  std::string v;
  for (const auto& [key, expected] : oracle) {
    ASSERT_TRUE(db->Get(key, &v).ok()) << "lost key " << key;
    EXPECT_EQ(v, expected) << "wrong value for " << key;
  }
  for (int i = 0; i < 500; ++i) {
    const std::string key = "zzz" + std::to_string(rng.Uniform(1000000));
    if (oracle.count(key) == 0) {
      EXPECT_TRUE(IsNotFound(db->Get(key, &v)));
    }
  }
}

// ---- stage-2 second batch: Scan, compaction, regressions ----

TEST(DBTest, ScanRangesTombstonesAndBoundaries) {
  auto db = DB::Open(FreshDBDir("db_scan"));
  ASSERT_NE(db, nullptr);

  EXPECT_TRUE(db->Put("a", "1").ok());
  EXPECT_TRUE(db->Put("b", "2").ok());
  EXPECT_TRUE(db->Put("c", "3").ok());
  EXPECT_TRUE(db->Put("d", "4").ok());
  EXPECT_TRUE(db->Delete("c").ok());
  EXPECT_TRUE(db->Put("b", "2b").ok());  // overwrite: scan must see 2b

  // Collect [begin, end) results.
  const auto scan = [&](const char* begin, const char* end) {
    std::vector<std::pair<std::string, std::string>> out;
    EXPECT_TRUE(db->Scan(begin, end,
                         [&](std::string_view k, std::string_view v) {
                           out.emplace_back(k, v);
                         }).ok());
    return out;
  };

  // Full range: tombstoned c skipped, newest b wins.
  const auto all = scan("a", "z");
  ASSERT_EQ(all.size(), 3u);
  EXPECT_EQ(all[0].first, "a");
  EXPECT_EQ(all[1].first, "b");
  EXPECT_EQ(all[1].second, "2b");
  EXPECT_EQ(all[2].first, "d");

  // Half-open boundaries: [b, d) excludes d, includes b.
  const auto mid = scan("b", "d");
  ASSERT_EQ(mid.size(), 1u);
  EXPECT_EQ(mid[0].first, "b");

  // begin == end is empty; begin past everything is empty.
  EXPECT_TRUE(scan("c", "c").empty());
  EXPECT_TRUE(scan("x", "z").empty());
}

// Scan against an std::map oracle while flushes and compactions run
// underneath (tiny thresholds so L0 -> L1 -> L2 all get exercised).
TEST(DBTest, ScanMatchesStdMapAcrossFlushesAndCompactions) {
  const std::string dir = FreshDBDir("db_scan_model");
  Options opts;
  opts.write_buffer_size = 16 * 1024;
  opts.l0_compaction_trigger = 2;
  opts.level_base_size = 64 * 1024;
  opts.max_sst_size = 16 * 1024;
  TestRng rng(20260908);
  std::map<std::string, std::string> oracle;

  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  const int kOps = 12000;
  const uint64_t kKeySpace = 600;
  for (int i = 0; i < kOps; ++i) {
    const std::string key = "k" + std::to_string(rng.Uniform(kKeySpace));
    const int roll = static_cast<int>(rng.Uniform(100));
    if (roll < 55) {
      const std::string value = "v" + std::to_string(rng.Uniform(100000));
      ASSERT_TRUE(db->Put(key, value).ok());
      oracle[key] = value;
    } else if (roll < 70) {
      ASSERT_TRUE(db->Delete(key).ok());
      oracle.erase(key);
    } else if (roll < 90) {
      // Random range scan, verified against the oracle.
      const uint64_t lo = rng.Uniform(kKeySpace);
      const uint64_t hi = lo + rng.Uniform(80);
      std::map<std::string, std::string> got;
      ASSERT_TRUE(
          db->Scan("k" + std::to_string(lo), "k" + std::to_string(hi),
                   [&](std::string_view k, std::string_view v) {
                     got.emplace(k, v);
                   }).ok());
      std::map<std::string, std::string> want;
      for (auto it = oracle.lower_bound("k" + std::to_string(lo));
           it != oracle.end() && it->first < "k" + std::to_string(hi); ++it) {
        want.emplace(it->first, it->second);
      }
      EXPECT_EQ(got, want) << "op " << i << " scan [" << lo << ", " << hi << ")";
    } else {
      std::string v;
      const Status s = db->Get(key, &v);
      const auto it = oracle.find(key);
      if (it == oracle.end()) {
        EXPECT_TRUE(IsNotFound(s)) << "op " << i;
      } else {
        ASSERT_TRUE(s.ok()) << "op " << i;
        EXPECT_EQ(v, it->second) << "op " << i;
      }
    }
  }
  db->wait_for_background_work();
  EXPECT_GE(db->level_file_count(1), 1u)
      << "compaction never moved anything into L1";

  // Full-range scan after everything settled.
  std::map<std::string, std::string> got;
  ASSERT_TRUE(db->Scan("", "\xff", [&](std::string_view k, std::string_view v) {
    got.emplace(k, v);
  }).ok());
  EXPECT_EQ(got, oracle);

  // And the whole oracle survives a reopen.
  db.reset();
  db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  std::string v;
  for (const auto& [key, expected] : oracle) {
    ASSERT_TRUE(db->Get(key, &v).ok()) << "lost key " << key;
    EXPECT_EQ(v, expected) << "wrong value for " << key;
  }
}

// REGRESSION (stage-2 bug): DB::Open deleted every log the MANIFEST did
// not name. With a memtable still pending flush, its records lived only
// in the retired log — the reopen lost them. The MANIFEST's log number
// is now a replay floor and recovery replays every log >= the floor.
TEST(DBTest, ReopenWithPendingFlushKeepsRetiredLogs) {
  const std::string dir = FreshDBDir("db_pending_flush");
  Options opts;
  opts.write_buffer_size = 8 * 1024;
  opts.l0_compaction_trigger = 1000;  // keep SSTs in L0: pure flush test
  {
    auto db = DB::Open(dir, opts);
    ASSERT_NE(db, nullptr);
    for (int i = 0; i < 20000; ++i) {
      ASSERT_TRUE(db->Put("key" + std::to_string(i), std::string(100, 'v'))
                      .ok());
    }
    // No wait_for_background_work: destroy while a flush may still be
    // pending. The retired log holding the immutable memtable's records
    // must survive and be replayed.
  }
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  std::string v;
  for (int i = 0; i < 20000; ++i) {
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &v).ok())
        << "lost key" << i;
  }
}

// REGRESSION (stage-2 bug): Table::Open derived smallest_user_key_ from
// the index's first entry — the LAST key of the first data block. Leveled
// compaction then binary-searched with a wrong left boundary and skipped
// files whose real first key sorted before that. Any model test with
// compaction catches it; this one uses an adversarial key layout where
// the first block's last key is lexicographically FAR from the file's
// first key ("key0" vs "key10026"-style gaps).
TEST(DBTest, LeveledLookupAfterCompactionFindsSmallKeys) {
  const std::string dir = FreshDBDir("db_leveled_smallkeys");
  Options opts;
  opts.write_buffer_size = 8 * 1024;
  opts.l0_compaction_trigger = 2;
  opts.level_base_size = 32 * 1024;
  opts.max_sst_size = 32 * 1024;
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);

  // Lexicographic order clusters these so one data block spans a huge
  // lexicographic range: key0, key1, key10, key100, ...
  for (uint64_t i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db->Put("key" + std::to_string(i), std::string(80, 'v')).ok());
  }
  db->wait_for_background_work();
  ASSERT_GE(db->level_file_count(1), 1u) << "no L1 file after compaction";

  std::string v;
  for (uint64_t i = 0; i < 5000; ++i) {
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &v).ok())
        << "lost key" << i << " after compaction";
    EXPECT_EQ(v, std::string(80, 'v'));
  }
}

}  // namespace
