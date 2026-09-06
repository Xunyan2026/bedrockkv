// BedrockKV — ring tests: the io_uring wrapper must either work end to
// end (kernel with io_uring) or fail cleanly (gVisor hides it: ENOSYS).
// Both outcomes are legitimate; what is NOT acceptable is a half-open
// ring or a crash — so both worlds assert via the same test suite and
// CI covers whichever environment it runs in.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <sys/utsname.h>

#include <linux/io_uring.h>

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

// Regression for the mask-as-offset bug (CI-only: real kernels): N
// writes queued into ONE flush must ALL land at their own offsets.
// When sq_mask_ was taken from params (an offset, not a mask), every
// queued SQE collapsed onto slot 0 and only the last write survived.
TEST(RingTest, ThreeWritesOneFlushAllLand) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  const std::string path = TempPath("three");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);
  const std::string a = "aaaaaaaaaa", b = "bbbbbbbbbb", c = "cccccccccc";
  ASSERT_TRUE(ring->QueueWrite(fd, a.data(), a.size(), 0, 1));
  ASSERT_TRUE(ring->QueueWrite(fd, b.data(), b.size(), 10, 2));
  ASSERT_TRUE(ring->QueueWrite(fd, c.data(), c.size(), 20, 3));
  ASSERT_TRUE(ring->Flush(true));
  int seen = 0;
  ASSERT_TRUE(ring->Reap(
      [&](uint64_t token, int res) {
        EXPECT_EQ(res, 10) << "token " << token;
        ++seen;
      },
      /*wait=*/false));
  EXPECT_EQ(seen, 3);
  std::string buf(30, '\0');
  ASSERT_EQ(::pread(fd, buf.data(), buf.size(), 0), 30);
  EXPECT_EQ(buf, a + b + c);
  ::close(fd);
  ::unlink(path.c_str());
}

// Reap(wait=true) contract: returns only after EVERY in-flight op's CQE
// has been delivered, including ops still queued from a previous
// unflushed batch.
TEST(RingTest, ReapWaitDeliversEveryCompletion) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  const std::string path = TempPath("reapwait");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);
  std::vector<std::string> blobs;
  for (int i = 0; i < 5; ++i) {
    blobs.push_back(std::string(10, static_cast<char>('a' + i)));
    ASSERT_TRUE(ring->QueueWrite(fd, blobs.back().data(), 10,
                                 static_cast<uint64_t>(i) * 10, i + 1));
  }
  // Submit WITHOUT waiting, then let the waiting reap finish the job.
  ASSERT_TRUE(ring->Flush(false));
  int seen = 0;
  std::vector<uint64_t> tokens;
  ASSERT_TRUE(ring->Reap(
      [&](uint64_t token, int res) {
        EXPECT_GE(res, 0);
        ++seen;
        tokens.push_back(token);
      },
      /*wait=*/true));
  EXPECT_EQ(seen, 5);
  EXPECT_EQ(ring->outstanding(), 0u);
  EXPECT_EQ(ring->pending(), 0u);
  std::sort(tokens.begin(), tokens.end());
  EXPECT_EQ(tokens, (std::vector<uint64_t>{1, 2, 3, 4, 5}));
  ::close(fd);
  ::unlink(path.c_str());
}

// Error propagation contract: a failed op completes with a negative
// result that Reap delivers verbatim (the DB turns this into a Put error
// at its durability points).
TEST(RingTest, FailedOpSurfacesNegativeResult) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  // A write against a closed fd must come back as -EBADF, not vanish.
  const int bad_fd = ::open("/dev/null", O_WRONLY);
  ASSERT_GE(bad_fd, 0);
  ::close(bad_fd);
  const std::string data = "doomed";
  ASSERT_TRUE(ring->QueueWrite(bad_fd, data.data(), data.size(), 0, 9));
  ASSERT_TRUE(ring->Flush(true));
  int errors = 0;
  ASSERT_TRUE(ring->Reap(
      [&](uint64_t token, int res) {
        EXPECT_EQ(token, 9u);
        EXPECT_EQ(res, -EBADF);
        if (res < 0) {
          ++errors;
        }
      },
      /*wait=*/false));
  EXPECT_EQ(errors, 1);
  EXPECT_EQ(ring->outstanding(), 0u);
}

}  // namespace
