// BedrockKV — libFuzzer harness for the RESP2 parser.
//
// Fuzz contract: ANY byte sequence fed in ANY quantity must leave the
// parser in a sane state — it either extracts requests, says "need more",
// or reports a protocol error. It must never crash, hang, or grow its
// buffer unboundedly from a small input (the length limits in resp.h are
// exactly what this harness punishes).
//
// Build: cmake -DBEDROCKKV_FUZZ=ON with clang; run:
//   ./build/fuzz/resp_fuzz -max_len=4096 tests/fuzz/seeds/resp
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bedrockkv/resp.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // Bound the harness's own memory: sizes above kMaxBulkLen are rejected
  // by the parser anyway, so the interesting space is well below this.
  if (size > (1u << 20)) {
    return 0;
  }
  bedrockkv::resp::Parser p;
  p.Feed(std::string_view(reinterpret_cast<const char*>(data), size));

  std::vector<std::string> args;
  std::string error;
  size_t extracted_bytes = 0;
  for (;;) {
    const auto st = p.Next(&args, &error);
    if (st == bedrockkv::resp::NextStatus::kNeedMore) {
      break;
    }
    if (st == bedrockkv::resp::NextStatus::kError) {
      if (error.empty()) {
        __builtin_trap();  // an error report must explain itself
      }
      p.Reset();
      break;
    }
    // Every extracted command's bytes must have been in the input; a
    // runaway parser would inflate `extracted_bytes` past this bound.
    for (const auto& a : args) {
      extracted_bytes += a.size();
    }
    if (extracted_bytes > size + 64) {
      __builtin_trap();  // parser invented bytes that were never fed
    }
  }
  return 0;
}
