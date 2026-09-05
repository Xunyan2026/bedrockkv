// BedrockKV — little-endian fixed-width integer encoding helpers.
//
// Shared by the WAL record payload, the MemTable internal-key tags and
// (later) SSTable blocks. Little-endian everywhere: files stay
// byte-identical across architectures.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace bedrockkv {

inline void PutFixed32(char* dst, uint32_t v) {
  dst[0] = static_cast<char>(v & 0xffu);
  dst[1] = static_cast<char>((v >> 8) & 0xffu);
  dst[2] = static_cast<char>((v >> 16) & 0xffu);
  dst[3] = static_cast<char>((v >> 24) & 0xffu);
}

inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  PutFixed32(buf, v);
  dst->append(buf, 4);
}

inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  for (int i = 0; i < 8; ++i) {
    buf[i] = static_cast<char>((v >> (8 * i)) & 0xffu);
  }
  dst->append(buf, 8);
}

inline uint32_t GetFixed32(const char* src) {
  return static_cast<uint32_t>(static_cast<unsigned char>(src[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(src[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(src[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(src[3])) << 24);
}

inline uint64_t GetFixed64(const char* src) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) {
    v = (v << 8) | static_cast<unsigned char>(src[i]);
  }
  return v;
}

}  // namespace bedrockkv
