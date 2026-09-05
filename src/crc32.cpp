#include "bedrockkv/crc32.h"

namespace bedrockkv {
namespace {

// Compile-time-generated lookup table for the reflected IEEE polynomial.
struct Crc32Table {
  uint32_t values[256];
  constexpr Crc32Table() : values{} {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; ++bit) {
        c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      values[i] = c;
    }
  }
};

constexpr Crc32Table kTable{};

}  // namespace

uint32_t Crc32(const void* data, size_t n, uint32_t crc) {
  const auto* p = static_cast<const unsigned char*>(data);
  uint32_t c = crc ^ 0xFFFFFFFFu;  // pre-inversion (zlib convention)
  for (size_t i = 0; i < n; ++i) {
    c = kTable.values[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;  // post-inversion
}

}  // namespace bedrockkv
