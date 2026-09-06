// BedrockKV — ring tests: the io_uring wrapper must either work end to
// end (kernel with io_uring) or fail cleanly (gVisor hides it: ENOSYS).
// Both outcomes are legitimate; what is NOT acceptable is a half-open
// ring or a crash — so both worlds assert via the same test suite and
// CI covers whichever environment it runs in.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "bedrockkv/ring.h"

using bedrockkv::Ring;
using bedrockkv::Status;

namespace {

std::string TempPath(const char* name) {
  return std::string("/tmp/bedrockkv_ring_") + name + "_" +
         std::to_string(::getpid());
}

TEST(RingTest, OpenEitherWorksOrFailsCleanly) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    // Degraded environment (e.g. gVisor with io_uring disabled): the
    // failure must be reported, not thrown or half-constructed.
    EXPECT_FALSE(s.ok());
    EXPECT_FALSE(s.message().empty());
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  EXPECT_TRUE(s.ok());
}

TEST(RingTest, WriteAndReapRoundTrip) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  const std::string path = TempPath("rw");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);

  const std::string first = "hello-uring";
  const std::string second = "second-record";
  const uint64_t off_second = first.size();
  ASSERT_TRUE(ring->QueueWrite(fd, first.data(), first.size(), 0, 1));
  ASSERT_TRUE(ring->QueueWrite(fd, second.data(), second.size(), off_second,
                               2));
  // Disjoint offsets submitted in one enter(): completion order is the
  // kernel's choice — the whole point of explicit-offset writes.
  ASSERT_TRUE(ring->Flush(true));
  int seen = 0;
  ASSERT_TRUE(ring->Reap(
      [&](uint64_t token, int res) {
        EXPECT_GE(res, 0) << "write failed: " << std::strerror(-res);
        EXPECT_EQ(res, token == 1 ? static_cast<int>(first.size())
                                  : static_cast<int>(second.size()));
        ++seen;
      },
      /*wait=*/false));
  EXPECT_EQ(seen, 2);
  EXPECT_EQ(ring->outstanding(), 0u);

  std::string buf(first.size() + second.size(), '\0');
  ASSERT_EQ(::pread(fd, buf.data(), buf.size(), 0),
            static_cast<ssize_t>(buf.size()));
  EXPECT_EQ(buf, first + second);
  ::close(fd);
  ::unlink(path.c_str());
}

TEST(RingTest, ReapWithoutWaitDrainsWhateverArrived) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  const std::string path = TempPath("nowait");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);
  const std::string data = "x";
  ASSERT_TRUE(ring->QueueWrite(fd, data.data(), data.size(), 0, 7));
  ASSERT_TRUE(ring->Flush(false));
  // Poll-style reap: may or may not have the CQE yet, but must be safe.
  ring->Reap([](uint64_t, int) {}, /*wait=*/false);
  // A waiting drain must finish the job regardless.
  ASSERT_TRUE(ring->Flush(true));
  ring->Reap([](uint64_t, int) {}, /*wait=*/false);
  EXPECT_EQ(ring->outstanding(), 0u);
  ::close(fd);
  ::unlink(path.c_str());
}

TEST(RingTest, QueueFsyncCompletes) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  const std::string path = TempPath("fsync");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);
  const std::string data = "durable-bytes";
  ASSERT_EQ(::pwrite(fd, data.data(), data.size(), 0),
            static_cast<ssize_t>(data.size()));
  ASSERT_TRUE(ring->QueueFsync(fd, 42));
  ASSERT_TRUE(ring->Flush(true));
  int errors = 0;
  ring->Reap(
      [&](uint64_t token, int res) {
        EXPECT_EQ(token, 42u);
        if (res < 0) {
          ++errors;
        }
      },
      /*wait=*/false);
  EXPECT_EQ(errors, 0);
  ::close(fd);
  ::unlink(path.c_str());
}

}  // namespace
