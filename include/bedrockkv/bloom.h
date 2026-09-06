// BedrockKV — Bloom filter (leveldb bloom.cc layout).
//
// Property contract: KeyMayMatch(filter, key) may return true for a key
// that was never added (false positive, ~0.8% at the default 10 bits/key)
// but NEVER false for a key that was added (no false negatives). The read
// path is allowed to skip a data block solely because the filter said no.
//
// Filter layout: [bits array][k u8]  — k is the number of hash probes,
// stored so that reader and writer agree without out-of-band data.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "bedrockkv/crc32.h"

namespace bedrockkv::bloom {

// Arbitrary odd seed, same role as leveldb's 0xbc9f1d34: decorrelate the
// hash from trivial key patterns (sequential keys differ in low bits).
inline uint32_t Hash(std::string_view key) {
  return Crc32(key.data(), key.size(), 0xbc9f1d34);
}

// Builds a filter over `keys` with the given bits per key.
// k = round(ln2 * bits_per_key), clamped to [1, 30] — the classic
// optimal-k formula for a target false-positive rate of (1 - e^{-k/m*n})^k.
inline std::string BuildFilter(const std::vector<std::string_view>& keys,
                               size_t bits_per_key = 10) {
  const size_t n = keys.size();
  if (n == 0) {
    return {};  // empty filter: reader treats it as "maybe"
  }
  size_t bits = n * bits_per_key;
  if (bits < 64) {
    bits = 64;  // tiny filters otherwise saturate at high FP rates
  }
  const size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;  // the array bounds every probe
  int k = static_cast<int>(bits_per_key * 0.69);  // ≈ ln 2
  if (k < 1) k = 1;
  if (k > 30) k = 30;

  std::string filter(bytes, '\0');
  for (const std::string_view key : keys) {
    uint32_t h = Hash(key);
    // Rotate right by 17 == rotate left by 15 (leveldb's delta trick):
    // each probe's hash differs by a full rotation-add, emulating
    // independent hash functions without a second hash computation.
    const uint32_t delta = (h >> 17) | (h << 15);
    for (int i = 0; i < k; ++i) {
      const size_t bitpos = h % bits;
      filter[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
      h += delta;
    }
  }
  filter.push_back(static_cast<char>(k));
  return filter;
}

// Conservative by construction: an unparsable/empty filter returns true
// ("maybe present") — a Bloom miss must never cost us a real key.
inline bool KeyMayMatch(std::string_view filter, std::string_view key) {
  if (filter.size() < 2) {
    return true;
  }
  const int k = static_cast<uint8_t>(filter.back());
  const size_t bits = (filter.size() - 1) * 8;
  uint32_t h = Hash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  for (int i = 0; i < k; ++i) {
    const size_t bitpos = h % bits;
    if ((static_cast<uint8_t>(filter[bitpos / 8]) & (1 << (bitpos % 8))) == 0) {
      return false;
    }
    h += delta;
  }
  return true;
}

}  // namespace bedrockkv::bloom
