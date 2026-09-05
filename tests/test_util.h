// BedrockKV — shared test utilities.
//
// Every oracle test in this project (skiplist today, MemTable, SSTable,
// the whole engine later) uses the same pattern: drive random operations
// from a deterministic RNG and mirror them into an std::map/set oracle.
// Determinism matters: a failing seed must reproduce the failure exactly.
#pragma once

#include <cstdint>
#include <random>

namespace bedrockkv::testing {

// Deterministic 64-bit RNG wrapper. Same seed => same operation sequence.
class TestRng {
 public:
  explicit TestRng(uint64_t seed) : gen_(seed) {}

  // Uniform integer in [0, upper).
  uint64_t Uniform(uint64_t upper) {
    return dist_(gen_) % upper;
  }

  // True with probability `percent` (0-100).
  bool Percent(int percent) { return Uniform(100) < static_cast<uint64_t>(percent); }

 private:
  std::mt19937_64 gen_;
  std::uniform_int_distribution<uint64_t> dist_{0, UINT64_MAX};
};

}  // namespace bedrockkv::testing
