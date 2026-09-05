// Unit tests for bedrockkv::Crc32 — pinned to well-known IEEE CRC-32
// vectors so any implementation drift (wrong polynomial, missing
// inversion) fails loudly.
#include <gtest/gtest.h>

#include "bedrockkv/crc32.h"

namespace {

using bedrockkv::Crc32;

TEST(Crc32Test, KnownVectors) {
  EXPECT_EQ(Crc32("", 0), 0x00000000u);
  EXPECT_EQ(Crc32("a", 1), 0xE8B7BE43u);
  EXPECT_EQ(Crc32("123456789", 9), 0xCBF43926u);
}

TEST(Crc32Test, IncrementalMatchesWhole) {
  const std::string s = "The quick brown fox jumps over the lazy dog";
  const size_t cut = 10;
  const uint32_t whole = Crc32(s.data(), s.size());
  const uint32_t split = Crc32(s.data() + cut, s.size() - cut,
                               Crc32(s.data(), cut));
  EXPECT_EQ(whole, split);
}

}  // namespace
