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

// TEMPORARY CI-only diagnostic (gVisor has no io_uring): pin down where
// an off=0 write lands on a real kernel. Prints everything; remove once
// the async-WAL corruption is root-caused.
TEST(RingTest, DIAGNOSTIC_OffsetProbe) {
  Status s = Status::Ok();
  auto ring = Ring::Open(8, &s);
  if (ring == nullptr) {
    GTEST_SKIP() << "io_uring unavailable here: " << s.message();
  }
  utsname u{};
  uname(&u);
  std::printf("DIAG uname: %s %s\n", u.sysname, u.release);

  const std::string path = TempPath("diag");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);

  auto reap_log = [&](const char* tag) {
    ring->Reap(
        [&](uint64_t token, int res) {
          std::printf("DIAG %s: cqe token=%llu res=%d\n", tag,
                      static_cast<unsigned long long>(token), res);
        },
        /*wait=*/false);
  };
  auto dump = [&](const char* tag, size_t n) {
    std::string out(n, '\0');
    const ssize_t got = ::pread(fd, out.data(), n, 0);
    std::printf("DIAG %s: end=%ld pread=%zd bytes=", tag,
                static_cast<long>(::lseek(fd, 0, SEEK_END)), got);
    for (char c : out) {
      if (c >= 32 && c < 127) {
        std::putchar(c);
      } else {
        std::printf("<%02x>", static_cast<unsigned char>(c));
      }
    }
    std::printf("\n");
  };

  // Stage 1: single write at offset 0.
  ASSERT_EQ(::ftruncate(fd, 0), 0);
  const std::string a = "AAAAAAAAAA";
  ASSERT_TRUE(ring->QueueWrite(fd, a.data(), a.size(), 0, 1));
  ASSERT_TRUE(ring->Flush(true));
  reap_log("stage1");
  dump("stage1-single-off0", 32);

  // Stage 2: single write at offset 5.
  ASSERT_EQ(::ftruncate(fd, 0), 0);
  const std::string b = "BBBBBBBBBB";
  ASSERT_TRUE(ring->QueueWrite(fd, b.data(), b.size(), 5, 2));
  ASSERT_TRUE(ring->Flush(true));
  reap_log("stage2");
  dump("stage2-single-off5", 32);

  // Stage 3: two writes in one flush (the failing shape).
  ASSERT_EQ(::ftruncate(fd, 0), 0);
  const std::string c = "CCCCCCCCCC";
  const std::string d = "DDDDDDDDDD";
  ASSERT_TRUE(ring->QueueWrite(fd, c.data(), c.size(), 0, 3));
  ASSERT_TRUE(ring->QueueWrite(fd, d.data(), d.size(), 10, 4));
  ASSERT_TRUE(ring->Flush(true));
  reap_log("stage3");
  dump("stage3-two-oneflush", 32);

  // Stage 4: two writes with a flush between them.
  ASSERT_EQ(::ftruncate(fd, 0), 0);
  const std::string e = "EEEEEEEEEE";
  const std::string f = "FFFFFFFFFF";
  ASSERT_TRUE(ring->QueueWrite(fd, e.data(), e.size(), 0, 5));
  ASSERT_TRUE(ring->Flush(true));
  reap_log("stage4a");
  ASSERT_TRUE(ring->QueueWrite(fd, f.data(), f.size(), 10, 6));
  ASSERT_TRUE(ring->Flush(true));
  reap_log("stage4b");
  dump("stage4-two-sepflush", 32);

  // Stage 5: non-sparse file — pre-fill with 'Z', overwrite at 0.
  ASSERT_EQ(::ftruncate(fd, 0), 0);
  std::string z(32, 'Z');
  ASSERT_EQ(::pwrite(fd, z.data(), z.size(), 0), 32);
  const std::string g = "GGGGGGGGGG";
  ASSERT_TRUE(ring->QueueWrite(fd, g.data(), g.size(), 0, 7));
  ASSERT_TRUE(ring->Flush(true));
  reap_log("stage5");
  dump("stage5-overwrite-off0", 32);

  ::close(fd);
  ::unlink(path.c_str());
}

// TEMPORARY CI-only diagnostic v2 (gVisor has no io_uring): bypass the
// Ring class entirely — raw syscalls + mmaps — and dump the raw ring
// memory so the CQE placement question is answered with bytes, not
// theories. Remove once root-caused.
TEST(RingTest, DIAGNOSTIC_RawRingDump) {
  utsname u{};
  uname(&u);
  std::printf("DIAG2 uname: %s %s\n", u.sysname, u.release);

  io_uring_params params;
  std::memset(&params, 0, sizeof(params));
  const int rfd =
      static_cast<int>(::syscall(425, 8, &params));  // io_uring_setup
  ASSERT_GE(rfd, 0);
  std::printf(
      "DIAG2 sq_entries=%u cq_entries=%u | sq_off: head=%u tail=%u mask=%u "
      "entries=%u flags=%u dropped=%u array=%u | cq_off: head=%u tail=%u "
      "mask=%u entries=%u overflow=%u cqes=%u\n",
      params.sq_entries, params.cq_entries, params.sq_off.head,
      params.sq_off.tail, params.sq_off.ring_mask, params.sq_off.ring_entries,
      params.sq_off.flags, params.sq_off.dropped, params.sq_off.array,
      params.cq_off.head, params.cq_off.tail, params.cq_off.ring_mask,
      params.cq_off.ring_entries, params.cq_off.overflow, params.cq_off.cqes);

  const size_t sq_sz = params.sq_off.array + params.sq_entries * 4;
  const size_t cq_sz = params.cq_off.cqes + params.cq_entries * 16;
  auto* sq = static_cast<std::byte*>(
      ::mmap(nullptr, sq_sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_POPULATE, rfd, 0));
  auto* cq = static_cast<std::byte*>(
      ::mmap(nullptr, cq_sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_POPULATE, rfd, 0x8000000));
  auto* sqes = static_cast<std::byte*>(
      ::mmap(nullptr, params.sq_entries * 64, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_POPULATE, rfd, 0x10000000));
  ASSERT_NE(sq, MAP_FAILED);
  ASSERT_NE(cq, MAP_FAILED);
  ASSERT_NE(sqes, MAP_FAILED);
  std::printf("DIAG2 sq_sz=%zu cq_sz=%zu same_pages=%d\n", sq_sz, cq_sz,
              sq == cq ? 1 : 0);

  auto dump = [](const char* tag, const std::byte* p, size_t n) {
    std::printf("DIAG2 %s:", tag);
    for (size_t i = 0; i < n; ++i) {
      if (i % 16 == 0) {
        std::printf("\n  [%3zu]", i);
      }
      std::printf(" %02x", static_cast<unsigned>(p[i]));
    }
    std::printf("\n");
  };

  auto head_of = [&](const std::byte* base, uint32_t off) {
    uint32_t v;
    std::memcpy(&v, base + off, 4);
    return v;
  };

  const std::string path = TempPath("diag2");
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  ASSERT_GE(fd, 0);

  // One flush, two writes: the failing shape.
  const std::string c = "CCCCCCCCCC";
  const std::string d = "DDDDDDDDDD";
  for (int i = 0; i < 2; ++i) {
    auto* sqe = sqes + i * 64;
    std::memset(sqe, 0, 64);
    sqe[0] = static_cast<std::byte>(23);  // IORING_OP_WRITE
    int32_t f = fd;
    uint64_t off = i == 0 ? 0 : 10;
    uint64_t addr =
        reinterpret_cast<uint64_t>(i == 0 ? c.data() : d.data());
    uint32_t len = 10;
    uint64_t ud = i == 0 ? 100 : 200;
    std::memcpy(sqe + 4, &f, 4);
    std::memcpy(sqe + 8, &off, 8);
    std::memcpy(sqe + 16, &addr, 8);
    std::memcpy(sqe + 24, &len, 4);
    std::memcpy(sqe + 32, &ud, 8);
    uint32_t idx = static_cast<uint32_t>(i);
    std::memcpy(sq + params.sq_off.array + 4 * i, &idx, 4);
  }
  uint32_t tail = 2;
  std::memcpy(sq + params.sq_off.tail, &tail, 4);
  std::printf("DIAG2 pre-enter: sq_head=%u sq_tail=%u cq_head=%u cq_tail=%u\n",
              head_of(sq, params.sq_off.head), head_of(sq, params.sq_off.tail),
              head_of(cq, params.cq_off.head), head_of(cq, params.cq_off.tail));
  dump("sqes[0..1]", sqes, 128);
  const long rc = ::syscall(426, rfd, 2, 2, 1, nullptr, 0);  // enter, wait 2
  std::printf("DIAG2 enter rc=%ld errno=%d\n", rc, rc < 0 ? errno : 0);
  std::printf("DIAG2 post-enter: sq_head=%u cq_tail=%u\n",
              head_of(sq, params.sq_off.head), head_of(cq, params.cq_off.tail));
  dump("cq[0..cqesz]", cq, cq_sz < 256 ? cq_sz : 256);

  char fbuf[32];
  std::memset(fbuf, 0, sizeof(fbuf));
  const ssize_t got = ::pread(fd, fbuf, sizeof(fbuf), 0);
  std::printf("DIAG2 file pread=%zd bytes=", got);
  for (char ch : fbuf) {
    if (ch >= 32 && ch < 127) {
      std::putchar(ch);
    } else {
      std::printf("<%02x>", static_cast<unsigned char>(ch));
    }
  }
  std::printf("\n");

  ::close(fd);
  ::unlink(path.c_str());
  ::close(rfd);
}

}  // namespace
