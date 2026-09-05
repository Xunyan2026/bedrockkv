// BedrockKV — CRC-32 (IEEE 802.3, zlib-compatible).
//
// Used to protect every WAL record from silent corruption. Table-driven
// implementation (reflected polynomial 0xEDB88320) — no external deps,
// and the table is computed at compile time.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrockkv {

// Streaming API: Crc32(b, len, Crc32(a, len1)) == Crc32(ab). Start with
// the default crc = 0 for a fresh computation.
uint32_t Crc32(const void* data, size_t n, uint32_t crc = 0);

}  // namespace bedrockkv
