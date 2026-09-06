// BedrockKV — YCSB benchmark driver (plan ch.6, stage 3 step 1).
//
// Usage (see --help):
//   ./ycsb_bench --workload A --recordcount 100000 --operationcount 100000
//
// Phases, per the YCSB standard:
//   1. LOAD  — insert `recordcount` keys (uniform over the key space).
//              Load throughput is reported separately, not mixed into
//              the run-phase numbers.
//   2. RUN   — `operationcount` operations with the workload's mix.
//
// Every run starts from a FRESH database directory unless --reuse_dir:
// the numbers must describe "the engine after ingesting N records", and
// a reused directory would mix previous workload state into the mix.
//
// Metrics: throughput, exact latency percentiles, and the engine's own
// write-amplification counters (WAL bytes + SST bytes vs user bytes).
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <string_view>

#include "bedrockkv/db.h"
#include "ycsb.h"

namespace {

using namespace bedrockkv;
using namespace bedrockkv::bench;

struct Config {
  std::string workload_name = "A";
  uint64_t recordcount = 100000;
  uint64_t operationcount = 100000;
  size_t value_size = 1024;
  size_t scanlength = 100;
  uint64_t seed = 42;
  std::string dir = "/tmp/bedrockkv_ycsb";
  bool reuse_dir = false;
  SyncMode sync_mode = SyncMode::kSyncPeriodic;
};

void PrintUsage() {
  std::printf(
      "usage: ycsb_bench [options]\n"
      "  --workload A|B|C|D|E|F|all  (default A)\n"
      "  --recordcount N     keys loaded before the run (default 100000)\n"
      "  --operationcount N  run-phase operations (default 100000)\n"
      "  --value_size N      bytes per value (default 1024)\n"
      "  --scanlength N      keys per scan (workload E, default 100)\n"
      "  --seed N            deterministic RNG seed (default 42)\n"
      "  --dir PATH          database directory\n"
      "  --sync always|periodic|never  WAL durability mode (default periodic)\n"
      "  --reuse_dir         keep the previous directory instead of wiping\n");
}

Config ParseArgs(int argc, char** argv) {
  Config c;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> const char* {
      return i + 1 < argc ? argv[++i] : "";
    };
    if (a == "--workload") c.workload_name = next();
    else if (a == "--recordcount") c.recordcount = std::strtoull(next(), nullptr, 10);
    else if (a == "--operationcount") c.operationcount = std::strtoull(next(), nullptr, 10);
    else if (a == "--value_size") c.value_size = std::strtoull(next(), nullptr, 10);
    else if (a == "--scanlength") c.scanlength = std::strtoull(next(), nullptr, 10);
    else if (a == "--seed") c.seed = std::strtoull(next(), nullptr, 10);
    else if (a == "--dir") c.dir = next();
    else if (a == "--reuse_dir") c.reuse_dir = true;
    else if (a == "--sync") {
      const std::string m = next();
      if (m == "always") c.sync_mode = SyncMode::kSyncAlways;
      else if (m == "periodic") c.sync_mode = SyncMode::kSyncPeriodic;
      else if (m == "never") c.sync_mode = SyncMode::kSyncNever;
    } else {
      std::printf("unknown arg %s\n", a.c_str());
      PrintUsage();
      std::exit(2);
    }
  }
  return c;
}

void RemoveDirTree(const std::string& dir) {
  // Benchmarks run under /tmp; a shell-out is fine and keeps main() small.
  if (std::system(("rm -rf '" + dir + "'").c_str()) != 0) {
    std::printf("failed to remove %s\n", dir.c_str());
    std::exit(1);
  }
}

std::string FormatNs(int64_t ns) {
  char buf[32];
  if (ns >= 1000000) {
    std::snprintf(buf, sizeof(buf), "%.1fms", static_cast<double>(ns) / 1e6);
  } else if (ns >= 1000) {
    std::snprintf(buf, sizeof(buf), "%.1fus", static_cast<double>(ns) / 1e3);
  } else {
    std::snprintf(buf, sizeof(buf), "%lldns", static_cast<long long>(ns));
  }
  return buf;
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// LOAD phase: insert recordcount keys, uniform order (YCSB "ordered"
// load), timing excluded from run-phase metrics but reported.
void RunLoad(const Config& c, DB* db) {
  const int64_t t0 = NowNs();
  for (uint64_t i = 0; i < c.recordcount; ++i) {
    const Status s = db->Put(MakeKey(i), MakeValue(i, c.value_size));
    if (!s.ok()) {
      std::printf("LOAD failed at %llu: %s\n", (unsigned long long)i,
                  s.message().c_str());
      std::exit(1);
    }
  }
  const int64_t elapsed = NowNs() - t0;
  const double secs = static_cast<double>(elapsed) / 1e9;
  std::printf("LOAD  %llu keys in %.2fs (%.0f ops/s)\n\n",
              (unsigned long long)c.recordcount, secs,
              static_cast<double>(c.recordcount) / secs);
}

// RUN phase: one timing sample per operation, per-operation-type counters.
void RunWorkload(const Config& c, const WorkloadSpec& w, DB* db) {
  // Read skew: zipfian over the whole key space, or "latest" (workload D):
  // zipfian over the most recent 10% of inserted keys, plus inserts that
  // push the frontier forward — approximating YCSB's skewed-latest.
  ZipfianGenerator zip(c.recordcount, 0.99, c.seed);
  ZipfianGenerator latest_zip(c.recordcount / 10 + 1, 0.99, c.seed + 1);
  std::mt19937_64 coin(c.seed + 2);
  std::uniform_int_distribution<uint64_t> pct(0, 99);
  std::uniform_int_distribution<uint64_t> scan_len(1, c.scanlength * 2 - 1);

  uint64_t next_insert = c.recordcount;  // workload D's frontier
  uint64_t ops[5] = {0, 0, 0, 0, 0};
  uint64_t total_scanned = 0;
  LatencyHistogram hist;

  const auto latest_key = [&]() -> std::string {
    // "Latest" (YCSB skewed-latest, simplified): zipfian over the newest
    // 10% of the key space, where inserts keep pushing the frontier.
    const uint64_t window = std::max<uint64_t>(next_insert / 10, 1);
    const uint64_t base = next_insert - window;
    return MakeKey(base + latest_zip.Next() % window);
  };

  const int64_t t0 = NowNs();
  for (uint64_t i = 0; i < c.operationcount; ++i) {
    const uint64_t roll = pct(coin);
    OpType op;
    if (roll < static_cast<uint64_t>(w.read_pct)) {
      op = OpType::kRead;
    } else if (roll < static_cast<uint64_t>(w.read_pct + w.update_pct)) {
      op = OpType::kUpdate;
    } else if (roll < static_cast<uint64_t>(w.read_pct + w.update_pct +
                                           w.insert_pct)) {
      op = OpType::kInsert;
    } else if (roll < static_cast<uint64_t>(w.read_pct + w.update_pct +
                                            w.insert_pct + w.scan_pct)) {
      op = OpType::kScan;
    } else {
      op = OpType::kRmw;
    }

    const auto key_for = [&]() -> std::string {
      if (w.latest_reads && op == OpType::kRead) {
        return latest_key();
      }
      return MakeKey(zip.Next() % c.recordcount);
    };

    const int64_t t_start = NowNs();
    switch (op) {
      case OpType::kRead: {
        std::string value;
        const Status s = db->Get(key_for(), &value);
        if (!s.ok() && s.code() != Status::Code::kNotFound) {
          std::printf("READ failed: %s\n", s.message().c_str());
          std::exit(1);
        }
        break;
      }
      case OpType::kUpdate: {
        const std::string key = key_for();
        const Status s = db->Put(key, MakeValue(next_insert + i, c.value_size));
        if (!s.ok()) {
          std::printf("UPDATE failed: %s\n", s.message().c_str());
          std::exit(1);
        }
        break;
      }
      case OpType::kInsert: {
        const std::string key = MakeKey(next_insert++);
        const Status s = db->Put(key, MakeValue(next_insert + i, c.value_size));
        if (!s.ok()) {
          std::printf("INSERT failed: %s\n", s.message().c_str());
          std::exit(1);
        }
        break;
      }
      case OpType::kScan: {
        // Scan by RECORD count (YCSB semantics): keys are zero-padded so
        // [begin, begin+scanlength) in key space is [begin, +N records).
        const uint64_t begin_idx = zip.Next() % c.recordcount;
        const uint64_t length = 1 + scan_len(coin) % c.scanlength;
        const std::string begin = MakeKey(begin_idx);
        const std::string end = MakeKey(begin_idx + length);
        uint64_t scanned = 0;
        const Status s = db->Scan(begin, end,
                                  [&scanned](std::string_view, std::string_view) {
                                    ++scanned;
                                  });
        if (!s.ok()) {
          std::printf("SCAN failed: %s\n", s.message().c_str());
          std::exit(1);
        }
        total_scanned += scanned;
        break;
      }
      case OpType::kRmw: {
        const std::string key = key_for();
        std::string value;
        const Status got = db->Get(key, &value);
        if (!got.ok() && got.code() != Status::Code::kNotFound) {
          std::printf("RMW read failed: %s\n", got.message().c_str());
          std::exit(1);
        }
        const Status s = db->Put(key, MakeValue(next_insert + i, c.value_size));
        if (!s.ok()) {
          std::printf("RMW write failed: %s\n", s.message().c_str());
          std::exit(1);
        }
        break;
      }
    }
    hist.Record(NowNs() - t_start);
    ops[static_cast<size_t>(op)]++;
  }
  const int64_t elapsed = NowNs() - t0;
  db->wait_for_background_work();
  const int64_t bg_elapsed = NowNs() - t0;

  const double secs = static_cast<double>(elapsed) / 1e9;
  const uint64_t disk_bytes = db->wal_bytes_written() + db->sst_bytes_written();
  const uint64_t user_bytes = db->user_bytes_written();
  std::printf("RUN  [%s] %s\n", w.name, w.description);
  std::printf("     throughput: %.0f ops/s (%llu ops in %.2fs)\n",
              static_cast<double>(c.operationcount) / secs,
              (unsigned long long)c.operationcount, secs);
  std::printf("     latency   : p50=%s p95=%s p99=%s p99.9=%s max=%s\n",
              FormatNs(hist.Percentile(50)).c_str(),
              FormatNs(hist.Percentile(95)).c_str(),
              FormatNs(hist.Percentile(99)).c_str(),
              FormatNs(hist.Percentile(99.9)).c_str(),
              FormatNs(hist.max()).c_str());
  std::printf("     ops       : read=%llu update=%llu insert=%llu scan=%llu rmw=%llu"
              "%s\n",
              (unsigned long long)ops[0], (unsigned long long)ops[1],
              (unsigned long long)ops[2], (unsigned long long)ops[3],
              (unsigned long long)ops[4],
              ops[3] > 0
                  ? (" scanned_total=" +
                     std::to_string(total_scanned))
                        .c_str()
                  : "");
  std::printf("     write amp : %.2fx  (wal=%.1fMB sst=%.1fMB user=%.1fMB, "
              "%llu flushes, %llu compactions)\n",
              disk_bytes > 0 ? static_cast<double>(disk_bytes) /
                                   static_cast<double>(user_bytes)
                             : 0.0,
              static_cast<double>(db->wal_bytes_written()) / 1e6,
              static_cast<double>(db->sst_bytes_written()) / 1e6,
              static_cast<double>(user_bytes) / 1e6,
              (unsigned long long)db->flush_count(),
              (unsigned long long)db->compaction_count());
  std::printf("     (incl. bg drain: %.2fs total)\n\n",
              static_cast<double>(bg_elapsed) / 1e9);
}

}  // namespace

int main(int argc, char** argv) {
  Config c = ParseArgs(argc, argv);

  if (c.workload_name == "all") {
    for (const WorkloadSpec& w : kWorkloads) {
      Config one = c;
      one.workload_name = w.name;
      RemoveDirTree(one.dir);
      Status s;
      auto db = DB::Open(one.dir,
                         Options{one.sync_mode, 4u << 20, 4, 10u << 20,
                                 4u << 20},
                         &s);
      if (db == nullptr) {
        std::printf("Open failed: %s\n", s.message().c_str());
        return 1;
      }
      RunLoad(one, db.get());
      RunWorkload(one, w, db.get());
    }
    return 0;
  }

  const WorkloadSpec* w = FindWorkload(c.workload_name);
  if (w == nullptr) {
    std::printf("unknown workload %s\n", c.workload_name.c_str());
    PrintUsage();
    return 2;
  }
  if (!c.reuse_dir) {
    RemoveDirTree(c.dir);
  }
  Status s;
  auto db = DB::Open(c.dir, Options{c.sync_mode, 4u << 20, 4, 10u << 20,
                                    4u << 20},
                     &s);
  if (db == nullptr) {
    std::printf("Open failed: %s\n", s.message().c_str());
    return 1;
  }
  RunLoad(c, db.get());
  RunWorkload(c, *w, db.get());
  return 0;
}
