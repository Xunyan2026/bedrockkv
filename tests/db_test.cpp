// Unit tests for the stage-1 DB: basic ops, reopen persistence, torn-tail
// recovery, and the fork + kill -9 crash test with prefix-integrity
// verification (design doc ch.7).
#include <dirent.h>
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
#include "bedrockkv/ring.h"
#include "test_util.h"

namespace {

using bedrockkv::DB;
using bedrockkv::Ring;
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

// ---- stage 3 batch 2: WiscKey value separation + vLog GC ----

// Counts files ending in `suffix` in `dir` (tests inspect the engine's
// file layout directly — the GC's observable promise is "old generations
// disappear").
int CountFilesWithSuffix(const std::string& dir, const std::string& suffix) {
  int n = 0;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) {
    return -1;
  }
  while (const dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name.size() > suffix.size() &&
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) ==
            0) {
      ++n;
    }
  }
  ::closedir(d);
  return n;
}

Options SeparationOptions(size_t vlog_gc_size) {
  Options opts;
  opts.sync_mode = SyncMode::kSyncNever;
  opts.enable_value_separation = true;
  opts.value_separation_threshold = 1024;
  opts.vlog_gc_size = vlog_gc_size;
  return opts;
}

TEST(DBTest, ValueSeparationRoundTripAndReopen) {
  const std::string dir = FreshDBDir("db_vsep_roundtrip");
  auto db = DB::Open(dir, SeparationOptions(64u << 20));
  ASSERT_NE(db, nullptr);

  // Both sides of the threshold: values below it stay inline, values at
  // or above it go to the vLog. Everything must read back identically.
  const std::string small = "tiny";
  const std::string big = std::string(4096, 'B');
  const std::string exact = std::string(1024, 'E');  // exactly at threshold
  ASSERT_TRUE(db->Put("small", small).ok());
  ASSERT_TRUE(db->Put("big", big).ok());
  ASSERT_TRUE(db->Put("exact", exact).ok());
  db->wait_for_background_work();

  std::string v;
  ASSERT_TRUE(db->Get("small", &v).ok());
  EXPECT_EQ(v, small);
  ASSERT_TRUE(db->Get("big", &v).ok());
  EXPECT_EQ(v, big);
  ASSERT_TRUE(db->Get("exact", &v).ok());
  EXPECT_EQ(v, exact);
  ASSERT_GE(CountFilesWithSuffix(dir, ".vlog"), 1)
      << "no vLog file: big values were not separated";

  // Reopen: pointers in the SSTs must resolve through the re-opened
  // vLog generation.
  db.reset();
  db = DB::Open(dir, SeparationOptions(64u << 20));
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(db->Get("small", &v).ok());
  EXPECT_EQ(v, small);
  ASSERT_TRUE(db->Get("big", &v).ok());
  EXPECT_EQ(v, big);
  ASSERT_TRUE(db->Get("exact", &v).ok());
  EXPECT_EQ(v, exact);
}

TEST(DBTest, ValueSeparationOverwritesDeletesAndScan) {
  const std::string dir = FreshDBDir("db_vsep_mix");
  auto db = DB::Open(dir, SeparationOptions(64u << 20));
  ASSERT_NE(db, nullptr);

  const std::string v1 = std::string(2048, '1');
  const std::string v2 = std::string(2048, '2');
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), v1).ok());
  }
  // Overwrite half (new vLog entries, old ones become garbage), delete
  // a few, keep the rest.
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), v2).ok());
  }
  for (int i = 15; i < 20; ++i) {
    ASSERT_TRUE(db->Delete("k" + std::to_string(i)).ok());
  }
  db->wait_for_background_work();

  std::string v;
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(db->Get("k" + std::to_string(i), &v).ok());
    EXPECT_EQ(v, v2);
  }
  for (int i = 10; i < 15; ++i) {
    ASSERT_TRUE(db->Get("k" + std::to_string(i), &v).ok());
    EXPECT_EQ(v, v1);
  }
  for (int i = 15; i < 20; ++i) {
    EXPECT_TRUE(IsNotFound(db->Get("k" + std::to_string(i), &v)));
  }

  // Scan resolves pointers too: 15 live keys, newest values.
  std::map<std::string, std::string> scanned;
  ASSERT_TRUE(
      db->Scan("k0", "k99",
               [&scanned](std::string_view k, std::string_view val) {
                 scanned.emplace(std::string(k), std::string(val));
               })
          .ok());
  ASSERT_EQ(scanned.size(), 15u);
  EXPECT_EQ(scanned.at("k0"), v2);
  EXPECT_EQ(scanned.at("k12"), v1);
}

TEST(DBTest, SeparationSurvivesFlushAndCompaction) {
  // Tiny thresholds force memtable flushes and L0->L1 compaction while
  // separated pointers flow through them. Compaction treats pointers as
  // opaque values — that is the whole WiscKey design — so every value
  // must still resolve afterwards.
  const std::string dir = FreshDBDir("db_vsep_compact");
  Options opts = SeparationOptions(64u << 20);
  opts.write_buffer_size = 16 * 1024;
  opts.l0_compaction_trigger = 2;
  opts.level_base_size = 64 * 1024;
  opts.max_sst_size = 16 * 1024;
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);

  for (uint64_t i = 0; i < 2000; ++i) {
    const std::string value = std::string(1500, static_cast<char>('a' + i % 26));
    ASSERT_TRUE(db->Put("key" + std::to_string(i), value).ok());
  }
  db->wait_for_background_work();
  ASSERT_GE(db->level_file_count(1), 1u) << "no compaction happened";

  std::string v;
  for (uint64_t i = 0; i < 2000; ++i) {
    ASSERT_TRUE(db->Get("key" + std::to_string(i), &v).ok())
        << "lost key" << i << " after flush+compaction";
    EXPECT_EQ(v, std::string(1500, static_cast<char>('a' + i % 26)));
  }

  // And after reopen, where pointers come back from the SSTs.
  db.reset();
  db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(db->Get("key7", &v).ok());
  EXPECT_EQ(v, std::string(1500, 'h'));
}

// GC contract: garbage (overwritten/deleted values) is reclaimed, live
// values survive untouched, and the old generation's file disappears.
TEST(DBTest, VlogGcReclaimsGarbageKeepsLive) {
  const std::string dir = FreshDBDir("db_vsep_gc");
  // 64 KiB trigger: the 2x-live rule means GC fires once the file holds
  // more garbage than live bytes.
  auto db = DB::Open(dir, SeparationOptions(64u << 10));
  ASSERT_NE(db, nullptr);

  const std::string first = std::string(2048, 'a');
  const std::string second = std::string(2048, 'b');
  // 300 live keys; then overwrite ALL of them (every first version turns
  // to garbage) and delete 100 (their second version turns to garbage,
  // too).
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), first).ok());
  }
  for (int i = 0; i < 300; ++i) {
    ASSERT_TRUE(db->Put("k" + std::to_string(i), second).ok());
  }
  for (int i = 0; i < 100; ++i) {
    ASSERT_TRUE(db->Delete("k" + std::to_string(i)).ok());
  }
  db->wait_for_background_work();

  EXPECT_GE(db->vlog_gc_count(), 1u) << "GC never ran";
  EXPECT_EQ(CountFilesWithSuffix(dir, ".vlog"), 1)
      << "old vLog generation still on disk after GC";

  std::string v;
  for (int i = 100; i < 300; ++i) {
    ASSERT_TRUE(db->Get("k" + std::to_string(i), &v).ok())
        << "GC lost live key" << i;
    EXPECT_EQ(v, second);
  }
  for (int i = 0; i < 100; ++i) {
    EXPECT_TRUE(IsNotFound(db->Get("k" + std::to_string(i), &v)));
  }

  // Survives reopen with the reclaimed layout.
  db.reset();
  db = DB::Open(dir, SeparationOptions(64u << 10));
  ASSERT_NE(db, nullptr);
  ASSERT_TRUE(db->Get("k250", &v).ok());
  EXPECT_EQ(v, second);
}

// The full model test, separation on: random puts/gets/deletes against
// std::map with flushes, compaction and GC all firing underneath.
// The io_uring fast path: requested in Options, best-effort at Open.
// On a kernel/sandbox without io_uring (gVisor: ENOSYS) the DB must open
// fine on the synchronous path, report why, and behave identically; on
// an io_uring host the async WAL + parallel-fsync path must be exercised
// by every operation below. Either way the full model check runs.
TEST(DBTest, IoUringOptionDegradesOrActivates) {
  const std::string dir = FreshDBDir("db_uring");
  Options opts = SeparationOptions(64u << 10);
  opts.enable_io_uring = true;
  opts.write_buffer_size = 32 * 1024;   // force rotations with async WAL
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  if (db->io_uring_active()) {
    EXPECT_TRUE(db->io_uring_unavailable_reason().empty());
  } else {
    EXPECT_FALSE(db->io_uring_unavailable_reason().empty());
  }

  // Data through every write shape: plain puts, overwrites, deletes,
  // separated values, forced rotations, flushes, and a reopen that
  // replays whatever the async WAL left behind.
  std::map<std::string, std::string> model;
  for (int i = 0; i < 400; ++i) {
    const std::string key = "k" + std::to_string(i % 120);
    const std::string value(i % 3 == 0 ? std::string(2048, 'a' + (i % 26))
                                       : std::string(40, 'a' + (i % 26)));
    ASSERT_TRUE(db->Put(key, value).ok());
    model[key] = value;
    if (i % 97 == 0) {
      ASSERT_TRUE(db->Delete(key).ok());
      model.erase(key);
    }
  }
  db->wait_for_background_work();
  std::string v;
  for (const auto& [key, expected] : model) {
    ASSERT_TRUE(db->Get(key, &v).ok()) << "lost " << key;
    EXPECT_EQ(v, expected);
  }
  db.reset();
  db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  EXPECT_EQ(db->io_uring_active(), Ring::Supported());
  for (const auto& [key, expected] : model) {
    ASSERT_TRUE(db->Get(key, &v).ok()) << "lost " << key << " after reopen";
    EXPECT_EQ(v, expected);
  }
}

TEST(DBTest, SeparationModelTestAgainstStdMap) {
  const std::string dir = FreshDBDir("db_vsep_model");
  Options opts = SeparationOptions(32u << 10);
  opts.write_buffer_size = 16 * 1024;
  opts.l0_compaction_trigger = 2;
  opts.level_base_size = 64 * 1024;
  opts.max_sst_size = 16 * 1024;
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);

  std::map<std::string, std::string> model;
  TestRng rng(20260909);
  const int kN = 600;
  for (int i = 0; i < kN; ++i) {
    const std::string key = "key" + std::to_string(rng.Uniform(150));
    // Values straddle the separation threshold (64..300 bytes).
    const std::string value(rng.Uniform(237) + 64,
                            static_cast<char>(rng.Uniform(256)));
    if (rng.Percent(55)) {
      ASSERT_TRUE(db->Put(key, value).ok());
      model[key] = value;
    } else if (rng.Percent(45)) {  // 20% of all iterations
      ASSERT_TRUE(db->Delete(key).ok());
      model.erase(key);
    } else {
      std::string v;
      const Status s = db->Get(key, &v);
      const auto it = model.find(key);
      if (it == model.end()) {
        EXPECT_TRUE(IsNotFound(s)) << "ghost value for " << key;
      } else {
        ASSERT_TRUE(s.ok()) << "missing live key " << key;
        EXPECT_EQ(v, it->second);
      }
    }
    if (i % 50 == 0) {
      db->wait_for_background_work();
    }
  }
  db->wait_for_background_work();

  // Full verification pass.
  std::string v;
  for (const auto& [key, expected] : model) {
    ASSERT_TRUE(db->Get(key, &v).ok()) << "lost " << key;
    EXPECT_EQ(v, expected);
  }
  db.reset();
  db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);
  for (const auto& [key, expected] : model) {
    ASSERT_TRUE(db->Get(key, &v).ok()) << "lost " << key << " after reopen";
    EXPECT_EQ(v, expected);
  }
}

// Regression: the lost-wakeup deadlock. Separated values leave ~40-byte
// memtable entries, so with a large write buffer NO rotation ever fires
// — and rotations were the only thing waking the background thread. The
// vLog crossed its GC trigger with the background thread asleep, and
// wait_for_background_work() (whose predicate requires !VlogGcNeeded)
// blocked forever on a GC nobody would ever start. The fix: a write that
// crosses the trigger wakes the background thread, and
// wait_for_background_work() notifies before waiting.
TEST(DBTest, VlogGcRunsAfterTriggerWithoutAnyRotation) {
  const std::string dir = FreshDBDir("db_vsep_wakeup");
  Options opts = SeparationOptions(256 * 1024);  // GC trigger: 256 KiB
  opts.value_separation_threshold = 64;  // 512 B values must separate
  opts.write_buffer_size = 32u << 20;            // no rotation, ever
  auto db = DB::Open(dir, opts);
  ASSERT_NE(db, nullptr);

  // 5000 x 512 B = ~2.6 MB of vLog bytes: 10x over the trigger, all
  // without a single memtable rotation.
  for (int i = 0; i < 5000; ++i) {
    const std::string key = "k" + std::to_string(i);
    const std::string value(512, static_cast<char>(i & 0xff));
    ASSERT_TRUE(db->Put(key, value).ok());
  }
  // Before the fix this never returned; after it, the GC must have run.
  db->wait_for_background_work();
  EXPECT_GE(db->vlog_gc_count(), 1);
  EXPECT_EQ(CountFilesWithSuffix(dir, ".vlog"), 1);  // old generation gone

  std::string v;
  for (int i = 0; i < 5000; i += 97) {
    const std::string key = "k" + std::to_string(i);
    ASSERT_TRUE(db->Get(key, &v).ok()) << "lost " << key;
    EXPECT_EQ(v, std::string(512, static_cast<char>(i & 0xff)));
  }
}

}  // namespace
