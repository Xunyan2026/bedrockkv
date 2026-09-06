// BedrockKV — libFuzzer harness for the WAL replayer (log::Reader).
//
// Fuzz contract: arbitrary bytes interpreted as a WAL file must (a) parse
// without crashing, whatever the corruption pattern, and (b) obey the
// recovery invariant — truncating the file at the reported last-good-end
// and re-reading must yield ONLY intact records. That invariant is what
// DB::Open's crash recovery bets correctness on.
//
//   ./build/fuzz/wal_fuzz -max_len=8192 tests/fuzz/seeds/wal
#include <fcntl.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "bedrockkv/log.h"

namespace {

// One file path reused across iterations (this harness is single-threaded
// per libFuzzer process).
std::string Path() {
  static const std::string path =
      "/tmp/bedrockkv_wal_fuzz_" + std::to_string(::getpid()) + ".log";
  return path;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0 || size > (1u << 18)) {
    return 0;
  }
  const std::string& path = Path();
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return 0;
  }
  size_t written = 0;
  while (written < size) {
    const ssize_t n = ::write(fd, data + written, size - written);
    if (n <= 0) {
      ::close(fd);
      return 0;
    }
    written += static_cast<size_t>(n);
  }

  // Pass 1: replay as recovery would — records until EOF or corruption.
  uint64_t replayed = 0;
  {
    ::lseek(fd, 0, SEEK_SET);
    bedrockkv::log::Reader reader(fd);
    std::string payload;
    for (;;) {
      const auto r = reader.ReadRecord(&payload, nullptr);
      if (r == bedrockkv::log::Reader::Result::kOk) {
        ++replayed;
        continue;
      }
      if (r == bedrockkv::log::Reader::Result::kCorruption) {
        break;
      }
      break;  // kEof
    }
    (void)replayed;

    // The invariant: everything below last_good_end_ is intact.
    const uint64_t good_end = reader.last_good_end();
    if (::ftruncate(fd, static_cast<off_t>(good_end)) != 0) {
      ::close(fd);
      return 0;
    }
    ::lseek(fd, 0, SEEK_SET);
    bedrockkv::log::Reader verifier(fd);
    std::string record;
    for (;;) {
      const auto r = verifier.ReadRecord(&record, nullptr);
      if (r == bedrockkv::log::Reader::Result::kEof) {
        break;
      }
      if (r == bedrockkv::log::Reader::Result::kCorruption) {
        // A record inside the "safe prefix" failed CRC: the recovery
        // floor lied. This is the bug the harness exists to catch.
        __builtin_trap();
      }
      // kOk: keep verifying.
    }
  }

  ::close(fd);
  ::unlink(path.c_str());
  return 0;
}
