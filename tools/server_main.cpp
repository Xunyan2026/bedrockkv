// BedrockKV — the server binary. A Redis-compatible endpoint in front of
// the LSM engine:
//
//   ./build/bedrockkv-server --port 7379 --dir /tmp/bedrockkv-demo
//   redis-cli -p 7379 set greeting hello
//   redis-cli -p 7379 get greeting
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

#include "bedrockkv/db.h"
#include "bedrockkv/server.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;  // async-signal-safe by type

void OnSignal(int) { g_stop = 1; }

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = 7379;
  std::string dir = "/tmp/bedrockkv-server";
  bool value_separation = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value after %s\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--port") {
      port = static_cast<uint16_t>(std::stoi(next("--port")));
    } else if (arg == "--dir") {
      dir = next("--dir");
    } else if (arg == "--value-separation") {
      value_separation = true;
    } else {
      std::fprintf(stderr,
                   "usage: bedrockkv-server [--port N] [--dir PATH] "
                   "[--value-separation]\n");
      return 2;
    }
  }

  bedrockkv::Options options;
  options.enable_value_separation = value_separation;
  bedrockkv::Status s = bedrockkv::Status::Ok();
  auto db = bedrockkv::DB::Open(dir, options, &s);
  if (db == nullptr) {
    std::fprintf(stderr, "cannot open database at %s: %s\n", dir.c_str(),
                 s.message().c_str());
    return 1;
  }

  auto server = bedrockkv::RedisServer::Open(db.get(), "0.0.0.0", port, &s);
  if (server == nullptr) {
    std::fprintf(stderr, "cannot start server: %s\n", s.message().c_str());
    return 1;
  }
  std::signal(SIGINT, OnSignal);
  std::signal(SIGTERM, OnSignal);

  server->Start();
  std::printf("bedrockkv-server listening on port %u (dir: %s%s)\n",
              server->port(), dir.c_str(),
              value_separation ? ", value separation on" : "");
  std::fflush(stdout);

  // The event loop runs on its own thread; main parks until a signal
  // asks for a clean shutdown (handler only sets a flag; all teardown
  // happens here on the main thread).
  while (g_stop == 0) {
    ::pause();
  }
  server->Stop();
  std::printf("bye\n");
  return 0;
}
