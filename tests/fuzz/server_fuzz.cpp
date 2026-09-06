// BedrockKV — libFuzzer harness for the full server stack: RESP2 parser →
// command dispatch → DB engine.
//
// Fuzz contract: any byte sequence addressed at the server parses into
// commands, executes against a real database, and returns well-formed
// replies. This catches cross-layer bugs the parser-only harness cannot
// see (e.g. a parsed arg that later breaks the engine, or a dispatch path
// that mishandles binary-safe keys).
//
//   ./build/fuzz/server_fuzz -max_len=4096 tests/fuzz/seeds/resp
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bedrockkv/db.h"
#include "bedrockkv/resp.h"
#include "bedrockkv/server.h"

namespace {

bedrockkv::DB* Db() {
  // One real database per fuzz process (Open is far too expensive per
  // input); /tmp on these hosts is tmpfs.
  static bedrockkv::DB* db = [] {
    const std::string dir =
        "/tmp/bedrockkv_server_fuzz_" + std::to_string(::getpid());
    return bedrockkv::DB::Open(dir).release();
  }();
  return db;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0 || size > (1u << 18)) {
    return 0;
  }
  bedrockkv::resp::Parser p;
  p.Feed(std::string_view(reinterpret_cast<const char*>(data), size));

  std::vector<std::string> args;
  std::string error;
  bedrockkv::DB* db = Db();
  size_t reply_bytes = 0;
  for (;;) {
    const auto st = p.Next(&args, &error);
    if (st == bedrockkv::resp::NextStatus::kNeedMore) {
      break;
    }
    if (st == bedrockkv::resp::NextStatus::kError) {
      p.Reset();
      break;
    }
    const std::string reply = bedrockkv::RedisServer::Execute(args, db);
    // Replies must be complete RESP2 messages, whatever the command did.
    if (reply.size() < 3 || reply.substr(reply.size() - 2) != "\r\n") {
      __builtin_trap();
    }
    reply_bytes += reply.size();
    if (reply_bytes > (1u << 20)) {
      break;  // bound the harness's own work on echo-heavy inputs
    }
  }
  return 0;
}
