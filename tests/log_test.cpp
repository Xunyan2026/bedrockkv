// Unit tests for the WAL log module: round-trips (small, fragmented,
// padding), and the crash-consistency behavior — flipped bytes, truncated
// tails, zero-header padding.
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bedrockkv/log.h"

namespace {

using bedrockkv::log::Reader;
using bedrockkv::log::Writer;
using Result = Reader::Result;

std::string TempPath(const char* name) {
  return testing::TempDir() + name;
}

// RAII wrapper for a temp file so a failing ASSERT can't leak descriptors.
class TempFile {
 public:
  explicit TempFile(const std::string& path) : path_(path) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  }
  ~TempFile() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    ::unlink(path_.c_str());
  }
  int fd() const { return fd_; }

 private:
  std::string path_;
  int fd_ = -1;
};

void WriteAllPayloads(Writer* writer, const std::vector<std::string>& payloads) {
  for (const auto& p : payloads) {
    ASSERT_TRUE(writer->AddRecord(p).ok());
  }
}

TEST(LogTest, RoundTripSmallRecords) {
  TempFile f(TempPath("log_roundtrip"));
  ASSERT_GE(f.fd(), 0);
  Writer writer(f.fd());

  const std::vector<std::string> payloads = {
      "",                         // empty payload is a legal record
      "a",
      "hello world",
      std::string(5000, 'x'),     // comfortably inside one block
  };
  WriteAllPayloads(&writer, payloads);

  const int rdfd = ::open(TempPath("log_roundtrip").c_str(), O_RDONLY);
  ASSERT_GE(rdfd, 0);
  Reader reader(rdfd);

  std::string out;
  for (const auto& expected : payloads) {
    ASSERT_EQ(reader.ReadRecord(&out), Result::kOk);
    EXPECT_EQ(out, expected);
  }
  EXPECT_EQ(reader.ReadRecord(&out), Result::kEof);
  ::close(rdfd);
}

TEST(LogTest, RoundTripFragmentedRecord) {
  TempFile f(TempPath("log_fragmented"));
  ASSERT_GE(f.fd(), 0);
  Writer writer(f.fd());

  const std::string big(100000, 'z');  // spans > 3 blocks
  const std::vector<std::string> payloads = {big, "after-the-wall"};
  WriteAllPayloads(&writer, payloads);

  const int rdfd = ::open(TempPath("log_fragmented").c_str(), O_RDONLY);
  ASSERT_GE(rdfd, 0);
  Reader reader(rdfd);

  std::string out;
  for (const auto& expected : payloads) {
    ASSERT_EQ(reader.ReadRecord(&out), Result::kOk);
    EXPECT_EQ(out, expected);
  }
  EXPECT_EQ(reader.ReadRecord(&out), Result::kEof);
  ::close(rdfd);
}

TEST(LogTest, BlockPaddingIsSkipped) {
  TempFile f(TempPath("log_padding"));
  ASSERT_GE(f.fd(), 0);
  Writer writer(f.fd());

  // Leaves only kHeaderSize-3 bytes in the block: forces a zero-padded
  // tail before the next record starts on a fresh block.
  const std::string first(bedrockkv::log::kBlockSize -
                              bedrockkv::log::kHeaderSize - 3,
                          'a');
  const std::vector<std::string> payloads = {first, "second"};
  WriteAllPayloads(&writer, payloads);

  const int rdfd = ::open(TempPath("log_padding").c_str(), O_RDONLY);
  ASSERT_GE(rdfd, 0);
  Reader reader(rdfd);

  std::string out;
  for (const auto& expected : payloads) {
    ASSERT_EQ(reader.ReadRecord(&out), Result::kOk);
    EXPECT_EQ(out, expected);
  }
  EXPECT_EQ(reader.ReadRecord(&out), Result::kEof);
  ::close(rdfd);
}

TEST(LogTest, DetectsFlippedPayloadByte) {
  TempFile f(TempPath("log_flip"));
  ASSERT_GE(f.fd(), 0);
  Writer writer(f.fd());
  WriteAllPayloads(&writer, {"first-record", "second-record"});
  ::close(f.fd());

  // Corrupt one payload byte of the FIRST record (header stays intact).
  std::string contents = [&] {
    FILE* fp = std::fopen(TempPath("log_flip").c_str(), "rb");
    std::string s;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) {
      s.append(buf, n);
    }
    std::fclose(fp);
    return s;
  }();
  const size_t kVictim = bedrockkv::log::kHeaderSize + 5;
  ASSERT_LT(kVictim, contents.size());
  contents[kVictim] = static_cast<char>(contents[kVictim] ^ 0xFF);
  FILE* fp = std::fopen(TempPath("log_flip").c_str(), "wb");
  std::fwrite(contents.data(), 1, contents.size(), fp);
  std::fclose(fp);

  const int rdfd = ::open(TempPath("log_flip").c_str(), O_RDONLY);
  ASSERT_GE(rdfd, 0);
  Reader reader(rdfd);

  std::string out;
  uint64_t corruption_offset = 12345;
  EXPECT_EQ(reader.ReadRecord(&out, &corruption_offset), Result::kCorruption);
  EXPECT_EQ(corruption_offset, 0u);  // nothing intact read before the hit
  ::close(rdfd);
}

TEST(LogTest, DetectsTruncatedTail) {
  TempFile f(TempPath("log_truncated"));
  ASSERT_GE(f.fd(), 0);
  Writer writer(f.fd());

  const int kCount = 50;
  const std::string payload(100, 'p');
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(writer.AddRecord(payload).ok());
  }
  ::close(f.fd());

  // Each physical record occupies kHeaderSize + 100 bytes. Cut the file so
  // record #49 has a complete header but a torn payload: a crash mid-write
  // replica. (Cutting shorter than a full header would be a clean EOF —
  // the reader cannot even parse a length there.)
  const size_t kPhysical = bedrockkv::log::kHeaderSize + payload.size();
  const size_t kCut = 49 * kPhysical + bedrockkv::log::kHeaderSize + 5;
  ASSERT_EQ(::truncate(TempPath("log_truncated").c_str(),
                       static_cast<off_t>(kCut)), 0);

  const int rdfd = ::open(TempPath("log_truncated").c_str(), O_RDONLY);
  ASSERT_GE(rdfd, 0);
  Reader reader(rdfd);

  std::string out;
  for (int i = 0; i < 49; ++i) {
    ASSERT_EQ(reader.ReadRecord(&out), Result::kOk) << "record " << i;
    EXPECT_EQ(out, payload);
  }
  uint64_t corruption_offset = 0;
  EXPECT_EQ(reader.ReadRecord(&out, &corruption_offset), Result::kCorruption);
  EXPECT_EQ(corruption_offset, 49 * kPhysical);  // exact truncation point
  EXPECT_EQ(reader.last_good_end(), 49 * kPhysical);
  ::close(rdfd);
}

TEST(LogTest, ZeroHeaderPaddingIsSkipped) {
  TempFile f(TempPath("log_zeroheader"));
  ASSERT_GE(f.fd(), 0);
  Writer writer(f.fd());
  WriteAllPayloads(&writer, {"one"});
  ::close(f.fd());

  // Append an all-zero header plus trailing junk — e.g. remnants of a torn
  // overwrite or a pre-zeroed region. The reader must skip to the block
  // end and then hit a clean EOF.
  FILE* fp = std::fopen(TempPath("log_zeroheader").c_str(), "ab");
  const char zeros[bedrockkv::log::kHeaderSize] = {0};
  std::fwrite(zeros, 1, sizeof(zeros), fp);
  std::fwrite("junk", 1, 4, fp);
  std::fclose(fp);

  const int rdfd = ::open(TempPath("log_zeroheader").c_str(), O_RDONLY);
  ASSERT_GE(rdfd, 0);
  Reader reader(rdfd);

  std::string out;
  ASSERT_EQ(reader.ReadRecord(&out), Result::kOk);
  EXPECT_EQ(out, "one");
  EXPECT_EQ(reader.ReadRecord(&out), Result::kEof);
  ::close(rdfd);
}

}  // namespace
