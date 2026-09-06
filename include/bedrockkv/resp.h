// BedrockKV — RESP2 (REdis Serialization Protocol, version 2) codec.
//
// The network face of the engine: redis-cli and redis-benchmark speak
// this dialect, so the server can be driven by stock Redis tooling.
// Spec: https://redis.io/docs/reference/protocol-spec/
//
// A client request is either:
//   * an Array of Bulk Strings ("multibulk", what every real client sends):
//       *<n>\r\n $<len>\r\n<arg bytes>\r\n ... (n times)
//   * an inline command: one line of space-separated tokens ("GET foo\r\n"),
//     what a human types with telnet.
//
// The parser is INCREMENTAL: TCP delivers arbitrary fragments (and
// coalesces commands), so Feed() accepts any byte run and Next() extracts
// one complete request at a time, keeping partial state between calls —
// the classic sticky-packet/split-packet problem, solved by buffering
// instead of hoping reads align with message boundaries.
//
// Replies are built by the Reply* helpers below; the server is the only
// consumer, but they are free functions so tests can assert on exact bytes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bedrockkv::resp {

// ---- limits (same spirit as Redis' PROTO_* defines) -----------------------
// Bounding every length BEFORE allocating keeps a hostile peer from making
// the parser allocate gigabytes from a single 10-byte packet.
inline constexpr size_t kMaxMultibulk = 1024 * 1024;     // args per command
inline constexpr size_t kMaxInlineLine = 64 * 1024;      // inline cmd length
inline constexpr size_t kMaxBulkLen = 512u << 20;        // 512 MiB, redis' cap

// Outcome of trying to pull one request out of the buffer.
enum class NextStatus {
  kRequest,   // *args holds one complete command
  kNeedMore,  // buffer holds a prefix of a request; feed more bytes
  kError,     // protocol violation; *error says why — the connection must
              // be closed (Redis closes on any protocol error too)
};

// An incremental RESP2 request parser. One instance per connection.
// Not copyable (it owns a buffer that callers may hold views into — see
// the lifetime note on Next()).
class Parser {
 public:
  Parser() = default;
  Parser(const Parser&) = delete;
  Parser& operator=(const Parser&) = delete;

  // Appends bytes from the socket. Any fragmentation pattern is valid.
  void Feed(std::string_view bytes) { buf_.append(bytes.data(), bytes.size()); }

  // Extracts the next complete request. `args` receives the command name
  // (args[0]; the server lowercases it, not the parser) and its arguments,
  // BY VALUE: the parser compacts its buffer after every request, so views
  // into it cannot outlive the call — copying is the only safe contract,
  // and at three bytes per command name it costs nothing.
  NextStatus Next(std::vector<std::string>* args, std::string* error);

  // Bytes currently buffered (tests / connection backpressure observability).
  size_t pending() const { return buf_.size(); }
  // Drops all buffered state (after a protocol error, before closing).
  void Reset() {
    buf_.clear();
    multibulk_ = -1;
    parse_pos_ = 0;
    arg_spans_.clear();
  }

 private:
  // Tries to parse one inline command at buf_[0..]; internal helper.
  NextStatus NextInline(std::vector<std::string>* args, std::string* error);

  std::string buf_;      // unparsed bytes from the socket
  long multibulk_ = -1;  // args still expected of the current multibulk,
                         // -1 = not inside one; `long` because a hostile
                         // length can exceed int range before we reject it
  // Parse position WITHIN buf_ (valid while a partial multibulk is in
  // flight). Bookkeeping as offsets rather than string_views is what
  // makes partial state survive a Feed(): a std::string append may
  // reallocate the buffer, and a stored view would dangle.
  size_t parse_pos_ = 0;
  // Spans of the args collected so far for the multibulk in flight;
  // materialized into views only when the command is complete.
  std::vector<std::pair<size_t, size_t>> arg_spans_;
};

// ---- reply encoders (RESP2) ----------------------------------------------
// Each returns the exact wire bytes. Named after the five RESP2 types.

// +PONG\r\n
std::string SimpleString(std::string_view s);
// -ERR message\r\n (newlines inside the message are sanitized: a raw \r\n
// would truncate the error on the client side and hide the rest)
std::string Error(std::string_view msg);
// :42\r\n
std::string Integer(int64_t n);
// $5\r\nhello\r\n  (null is $-1\r\n)
std::string BulkString(std::string_view s);
std::string NullBulk();
// *<n>\r\n followed by already-encoded elements (the caller appends them)
std::string ArrayHeader(size_t n);

}  // namespace bedrockkv::resp
