// BedrockKV — RESP2 parser & encoder tests.
//
// The parser's whole reason to exist is that TCP does not preserve message
// boundaries, so the test suite attacks it with every fragmentation pattern
// we can think of: whole buffers, one byte at a time, splits inside the
// bulk payload, pipelined commands, and a deterministic random-split fuzz.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bedrockkv/resp.h"
#include "test_util.h"

using bedrockkv::resp::ArrayHeader;
using bedrockkv::resp::BulkString;
using bedrockkv::resp::Error;
using bedrockkv::resp::Integer;
using bedrockkv::resp::NextStatus;
using bedrockkv::resp::NullBulk;
using bedrockkv::resp::Parser;
using bedrockkv::resp::SimpleString;
using bedrockkv::testing::TestRng;

namespace {

// Feeds `bytes` to the parser in random-sized chunks (1..7 bytes) and
// returns everything the parser managed to extract, in order.
std::vector<std::vector<std::string>> FeedFragmented(
    const std::string& bytes, uint64_t seed) {
  Parser p;
  std::vector<std::vector<std::string>> requests;
  TestRng rng(seed);
  size_t fed = 0;
  while (fed < bytes.size()) {
    const size_t chunk = 1 + rng.Uniform(7);
    p.Feed(std::string_view(bytes).substr(fed, chunk));
    fed += chunk;
    std::vector<std::string> args;
    std::string error;
    while (p.Next(&args, &error) == NextStatus::kRequest) {
      requests.emplace_back(args.begin(), args.end());
    }
    EXPECT_TRUE(error.empty()) << error;
    if (!error.empty()) {
      return requests;
    }
  }
  std::vector<std::string> args;
  std::string error;
  EXPECT_EQ(p.Next(&args, &error), NextStatus::kNeedMore);
  return requests;
}

// The canonical request for `args`, as redis-cli would send it.
std::string Encode(const std::vector<std::string>& args) {
  std::string out = ArrayHeader(args.size());
  for (const auto& a : args) {
    out += BulkString(a);
  }
  return out;
}

}  // namespace

TEST(RespTest, MultibulkWholeBuffer) {
  Parser p;
  p.Feed("*2\r\n$3\r\nGET\r\n$1\r\nx\r\n");
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "GET");
  EXPECT_EQ(args[1], "x");
  EXPECT_EQ(p.pending(), 0u);
  EXPECT_EQ(p.Next(&args, &error), NextStatus::kNeedMore);
}

TEST(RespTest, ByteByByteFeeding) {
  const std::string req = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n";
  Parser p;
  std::vector<std::string> args;
  std::string error;
  for (size_t i = 0; i + 1 < req.size(); ++i) {
    p.Feed(std::string_view(req).substr(i, 1));
    EXPECT_EQ(p.Next(&args, &error), NextStatus::kNeedMore) << "at byte " << i;
    EXPECT_TRUE(error.empty());
  }
  p.Feed(std::string_view(req).substr(req.size() - 1, 1));
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(args[1], "k");
  EXPECT_EQ(args[2], "v");
}

TEST(RespTest, SplitInsideBulkPayload) {
  Parser p;
  p.Feed("*2\r\n$5\r\nhel");
  std::vector<std::string> args;
  std::string error;
  EXPECT_EQ(p.Next(&args, &error), NextStatus::kNeedMore);
  p.Feed("lo\r\n$2\r\nwo");
  EXPECT_EQ(p.Next(&args, &error), NextStatus::kNeedMore);
  p.Feed("\r\n");
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], "hello");
  EXPECT_EQ(args[1], "wo");
}

TEST(RespTest, PipelinedCommands) {
  Parser p;
  p.Feed(Encode({"SET", "a", "1"}) + Encode({"GET", "a"}) +
         Encode({"PING"}));
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  EXPECT_EQ(args[0], "SET");
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  EXPECT_EQ(args[0], "GET");
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  EXPECT_EQ(args[0], "PING");
  EXPECT_EQ(p.Next(&args, &error), NextStatus::kNeedMore);
}

TEST(RespTest, BinarySafeArgs) {
  Parser p;
  const std::string key = std::string("a\0b", 3);
  const std::string val = "x\r\ny\0z";
  p.Feed(Encode({"SET", key, val}));
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(args[1], std::string_view(key));
  EXPECT_EQ(args[2], std::string_view(val));
}

TEST(RespTest, InlineCommand) {
  Parser p;
  p.Feed("SET foo bar\r\n");
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  ASSERT_EQ(args.size(), 3u);
  EXPECT_EQ(args[0], "SET");
  EXPECT_EQ(args[1], "foo");
  EXPECT_EQ(args[2], "bar");
}

TEST(RespTest, InlineBareNewlineAndExtraWhitespace) {
  Parser p;
  p.Feed("GET   k\n");
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[1], "k");

  Parser p2;
  p2.Feed("  \t \r\n");
  EXPECT_EQ(p2.Next(&args, &error), NextStatus::kNeedMore);
}

TEST(RespTest, EmptyLinesBeforeRequestAreSkipped) {
  Parser p;
  p.Feed("\r\n\r\n*1\r\n$4\r\nPING\r\n");
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  ASSERT_EQ(args.size(), 1u);
  EXPECT_EQ(args[0], "PING");
}

TEST(RespTest, EmptyMultibulkIsIgnored) {
  // *0 and *-1 carry no command; Redis consumes and continues.
  Parser p;
  p.Feed("*0\r\n*1\r\n$4\r\nPING\r\n");
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kRequest);
  EXPECT_EQ(args[0], "PING");
}

TEST(RespTest, ProtocolErrors) {
  const struct {
    const char* bytes;
    const char* error_contains;
  } cases[] = {
      {"*abc\r\n", "multibulk"},         // non-numeric arity
      {"*1\r\n:5\r\n", "expected '$'"},  // non-bulk inside a multibulk
      {"*1\r\n$-1\r\n", "bulk"},         // null bulk is not a request arg
      {"*2\r\n$2\r\nab\r\n$zz\r\n", "bulk"},  // non-numeric bulk length
  };
  for (const auto& c : cases) {
    Parser p;
    p.Feed(c.bytes);
    std::vector<std::string> args;
    std::string error;
    ASSERT_EQ(p.Next(&args, &error), NextStatus::kError) << c.bytes;
    EXPECT_NE(error.find(c.error_contains), std::string::npos)
        << c.bytes << " -> " << error;
  }

  Parser p;
  p.Feed("*1\r\n$3\r\nabXx\r\n");  // terminator bytes are not actually \r\n
  std::vector<std::string> args;
  std::string error;
  ASSERT_EQ(p.Next(&args, &error), NextStatus::kError);
  EXPECT_NE(error.find("terminator"), std::string::npos);
}

TEST(RespTest, OversizedLimits) {
  Parser p;
  // Digits with no terminator: line longer than any legal arity header.
  p.Feed("*" + std::string(40, '1'));
  std::vector<std::string> args;
  std::string error;
  EXPECT_EQ(p.Next(&args, &error), NextStatus::kError);

  Parser p2;
  p2.Feed("*99999999999999\r\n");  // arity beyond kMaxMultibulk
  EXPECT_EQ(p2.Next(&args, &error), NextStatus::kError);

  Parser p3;
  p3.Feed("*1\r\n$" + std::to_string(600ull << 20) + "\r\n");  // > kMaxBulkLen
  EXPECT_EQ(p3.Next(&args, &error), NextStatus::kError);
}

TEST(RespTest, RandomSplitFuzzMatchesWholeBuffer) {
  const std::vector<std::vector<std::string>> plan = {
      {"SET", "key:1", "value-one"},
      {"GET", "key:1"},
      {"PING", "payload with spaces"},
      {"DEL", "a", "b", "c", "d"},
      {"SET", "bin\0ary", "va\r\nlue\0"},
      {"EXISTS", "key:1", "missing"},
  };
  std::string wire;
  for (const auto& args : plan) {
    wire += Encode(args);
  }
  // Different seeds = different fragmentation patterns; all must yield
  // exactly the same request sequence as a single whole-buffer feed.
  for (const uint64_t seed : {1ull, 2ull, 42ull, 0xdeadbeefull}) {
    auto got = FeedFragmented(wire, seed);
    ASSERT_EQ(got.size(), plan.size()) << "seed " << seed;
    for (size_t i = 0; i < plan.size(); ++i) {
      EXPECT_EQ(got[i], plan[i]) << "seed " << seed << " request " << i;
    }
  }
}

TEST(RespTest, ReplyEncodersExactBytes) {
  EXPECT_EQ(SimpleString("OK"), "+OK\r\n");
  EXPECT_EQ(Error("ERR bad thing"), "-ERR bad thing\r\n");
  EXPECT_EQ(Error("line\r\nbreak"), "-line  break\r\n");
  EXPECT_EQ(Integer(-3), ":-3\r\n");
  EXPECT_EQ(BulkString("hi"), "$2\r\nhi\r\n");
  EXPECT_EQ(BulkString(""), "$0\r\n\r\n");
  EXPECT_EQ(NullBulk(), "$-1\r\n");
  EXPECT_EQ(ArrayHeader(3), "*3\r\n");
}
