// BedrockKV — YCSB harness primitives (plan ch.6, stage 3).
//
// A faithful-but-small port of the Yahoo! YCSB core workloads (A-F) for
// C++ KV engines: zipfian + uniform key distributions, the standard
// operation mixes, and the metrics that matter (throughput, latency
// percentiles, write amplification).
//
// Design decisions:
//   * Deterministic: a seeded RNG reproduces a run exactly, so a failed
//     or surprising measurement can be replayed.
//   * Single-writer: BedrockKV's contract is ONE writer thread, so the
//     harness interleaves reads and writes in one thread — the same
//     shape YCSB uses for threads=1.
//   * Zero dependencies: no Java YCSB, no google benchmark. The harness
//     itself is a deliverable (plan: "自写 C++ YCSB harness 本身就是考点").
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace bedrockkv::bench {

// Zipf-distributed sampler over [0, num_items). Builds the CDF once
// (O(N) time/memory for the fixed N of a benchmark run) and answers each
// sample with a binary search — exact, deterministic given the seed, and
// O(log N) per op. theta = 0 (uniform) .. ~1 (extremely skewed); YCSB's
// standard is 0.99.
class ZipfianGenerator {
 public:
  ZipfianGenerator(uint64_t num_items, double theta, uint64_t seed)
      : cdf_(num_items), gen_(seed), uniform_(0.0, 1.0) {
    // p(i) = (i+1)^-theta / H; cumulative sum normalized to 1.0.
    double sum = 0.0;
    for (uint64_t i = 0; i < num_items; ++i) {
      sum += 1.0 / std::pow(static_cast<double>(i + 1), theta);
      cdf_[i] = sum;
    }
    for (double& v : cdf_) {
      v /= sum;
    }
    cdf_.back() = 1.0;  // guard fp rounding: the last key must be reachable
  }

  uint64_t Next() {
    const double u = uniform_(gen_);
    return static_cast<uint64_t>(
        std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
  }

 private:
  std::vector<double> cdf_;
  std::mt19937_64 gen_;
  std::uniform_real_distribution<double> uniform_;
};

// Latency histogram with ns resolution, storing every sample (op counts
// here are ~10^5, so a full vector is fine and gives exact percentiles —
// no bucketing artifacts).
class LatencyHistogram {
 public:
  void Record(int64_t ns) { samples_.push_back(ns); }

  size_t count() const { return samples_.size(); }
  int64_t total_ns() const {
    int64_t sum = 0;
    for (int64_t s : samples_) {
      sum += s;
    }
    return sum;
  }

  // Percentile in [0, 100]. p = 99.9 goes through the double overload.
  int64_t Percentile(double p) const {
    if (samples_.empty()) {
      return 0;
    }
    std::vector<int64_t> sorted(samples_);
    std::sort(sorted.begin(), sorted.end());
    const size_t idx = std::min(
        sorted.size() - 1,
        static_cast<size_t>(p / 100.0 * static_cast<double>(sorted.size())));
    return sorted[idx];
  }

  int64_t max() const {
    int64_t m = 0;
    for (int64_t s : samples_) {
      m = std::max(m, s);
    }
    return m;
  }

 private:
  std::vector<int64_t> samples_;
};

// The YCSB core workloads (Yahoo paper table 2):
//   A: 50% read / 50% update        — session store
//   B: 95% read / 5% update         — photo tagging
//   C: 100% read                    — user profile cache
//   D: 95% read / 5% insert, latest — user status (reads skew to newest)
//   E: 95% scan / 5% insert         — thread lists
//   F: 50% read / 50% read-modify-write — user records
enum class OpType { kRead, kUpdate, kInsert, kScan, kRmw };

struct WorkloadSpec {
  const char* name;
  int read_pct;
  int update_pct;
  int insert_pct;
  int scan_pct;
  int rmw_pct;      // read-modify-write (read then put, both inside one op)
  bool latest_reads;  // true: reads skew to the most recently inserted keys
  const char* description;
};

inline const WorkloadSpec kWorkloads[] = {
    {"A", 50, 50, 0, 0, 0, false, "50R/50U zipfian — session store"},
    {"B", 95, 5, 0, 0, 0, false, "95R/5U zipfian — photo tagging"},
    {"C", 100, 0, 0, 0, 0, false, "100R zipfian — profile cache"},
    {"D", 95, 0, 5, 0, 0, true, "95R/5I latest — user status"},
    {"E", 0, 0, 5, 95, 0, false, "95S/5I zipfian — thread lists"},
    {"F", 0, 0, 0, 0, 100, false, "50R/50RMW zipfian — user records"},
};

inline const WorkloadSpec* FindWorkload(const std::string& name) {
  for (const WorkloadSpec& w : kWorkloads) {
    if (name == w.name) {
      return &w;
    }
  }
  return nullptr;
}

// Zero-padded keys: lexicographic order == numeric order (the engine's
// leveled search relies on sensible lex ranges, and db_bench uses the
// same convention).
inline std::string MakeKey(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "user%019llu",
                static_cast<unsigned long long>(i));
  return buf;
}

// Deterministic pseudo-random value of `size` bytes — real-looking bytes
// (not all-zeros), so compression proxies and cache behavior stay honest.
inline std::string MakeValue(uint64_t i, size_t size) {
  std::string v(size, '\0');
  uint64_t x = 0x9e3779b97f4a7c15ULL ^ (i * 0xff51afd7ed558ccdULL);
  for (size_t k = 0; k < size; ++k) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    v[k] = static_cast<char>(x & 0xff);
  }
  return v;
}

}  // namespace bedrockkv::bench
