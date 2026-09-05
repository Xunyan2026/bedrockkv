// Unit tests for the stage-1 DB: basic ops, reopen persistence, torn-tail
// recovery, and the fork + kill -9 crash test with prefix-integrity
// verification (design doc ch.7).
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <string>

#include <gtest/gtest.h>

#include "bedrockkv/db.h"

namespace {

using bedrockkv::DB;
using bedrockkv::Options;
using bedrockkv::Status;
using bedrockkv::SyncMode;

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
  {
    auto db = DB::Open(dir);
    ASSERT_NE(db, nullptr);
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Put(Key(i), std::string(50, 'v')).ok());
    }
  }

  // Append a plausible-looking header with no payload — a torn-write
  // replica. Recovery must cut it off and keep all 100 records.
  const std::string wal = dir + "/" + DB::kWalFileName;
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
  {
    auto db = DB::Open(dir);
    ASSERT_NE(db, nullptr);
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Put(Key(i), std::string(50, 'v')).ok());
    }
  }

  // One record: payload = 8(seq)+1(type)+4(klen)+21(key)+4(vlen)+50(value)
  // = 88 bytes; physical = 88 + 9(header) = 97. Cut 30 bytes into record
  // #99: complete header, torn payload.
  const size_t kKeyLen = 21;
  const size_t kPhysical = 9 + 8 + 1 + 4 + kKeyLen + 4 + 50;
  ASSERT_EQ(::truncate((dir + "/" + DB::kWalFileName).c_str(),
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

  // Parent: wait until the WAL has ~64KB of records, then kill -9.
  const std::string wal = dir + "/" + DB::kWalFileName;
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

}  // namespace
