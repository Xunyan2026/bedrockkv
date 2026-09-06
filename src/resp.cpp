#include "bedrockkv/resp.h"

#include <charconv>

namespace bedrockkv::resp {

namespace {

// Parses [begin, end) as decimal digits. Returns false on any other byte
// (a protocol error at this position — no sign, no whitespace tolerated,
// matching Redis' string2ll strictness).
bool ParseDigits(std::string_view s, long long* out) {
  if (s.empty()) {
    return false;
  }
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  return std::from_chars(s.data(), s.data() + s.size(), *out).ec ==
         std::errc{};
}

}  // namespace

NextStatus Parser::Next(std::vector<std::string>* args,
                        std::string* error) {
  for (;;) {
    if (multibulk_ < 0) {
      // Idle: skip empty lines first (telnet users press Enter; Redis
      // ignores empty inline commands rather than erroring).
      while (parse_pos_ < buf_.size() &&
             (buf_[parse_pos_] == '\r' || buf_[parse_pos_] == '\n')) {
        ++parse_pos_;
      }
      if (parse_pos_ > 0) {
        // Consumed prefix: compact so buf_ stays bounded across a long
        // connection. Only safe while no partial multibulk is in flight
        // (spans are offsets into buf_).
        buf_.erase(0, parse_pos_);
        parse_pos_ = 0;
      }
      if (buf_.empty()) {
        return NextStatus::kNeedMore;
      }
      if (buf_[0] == '*') {
        // ---- multibulk header: *<n>\r\n ----
        const size_t nl = buf_.find("\r\n", 1);
        if (nl == std::string::npos) {
          // No terminator yet — unless the number line is already longer
          // than any legal value, in which case this is an attack or a
          // broken client (kMaxMultibulk has 7 digits; 32 is generous).
          if (buf_.size() > 32) {
            *error = "Protocol error: too big multibulk length";
            return NextStatus::kError;
          }
          return NextStatus::kNeedMore;
        }
        long long n = 0;
        if (!ParseDigits(std::string_view(buf_).substr(1, nl - 1), &n)) {
          *error = "Protocol error: invalid multibulk length";
          return NextStatus::kError;
        }
        if (n > static_cast<long long>(kMaxMultibulk)) {
          *error = "Protocol error: invalid multibulk length";
          return NextStatus::kError;
        }
        if (n <= 0) {
          // *0 / *-1: nothing to execute — consume and look at the next
          // request (Redis treats these as empty commands too).
          buf_.erase(0, nl + 2);
          parse_pos_ = 0;
          continue;
        }
        multibulk_ = n;
        arg_spans_.clear();
        parse_pos_ = nl + 2;
        continue;
      }
      return NextInline(args, error);
    }

    // ---- inside a multibulk: expect $<len>\r\n<data>\r\n ----
    if (parse_pos_ >= buf_.size()) {
      return NextStatus::kNeedMore;
    }
    if (buf_[parse_pos_] != '$') {
      *error = "Protocol error: expected '$', got '" +
               std::string(1, buf_[parse_pos_]) + "'";
      return NextStatus::kError;
    }
    const size_t nl = buf_.find("\r\n", parse_pos_ + 1);
    if (nl == std::string::npos) {
      if (buf_.size() - parse_pos_ > 32) {
        *error = "Protocol error: invalid bulk length";
        return NextStatus::kError;
      }
      return NextStatus::kNeedMore;
    }
    long long len = 0;
    if (!ParseDigits(std::string_view(buf_).substr(parse_pos_ + 1,
                                                   nl - parse_pos_ - 1),
                     &len) ||
        len < 0 || static_cast<unsigned long long>(len) > kMaxBulkLen) {
      *error = "Protocol error: invalid bulk length";
      return NextStatus::kError;
    }
    const size_t data_pos = nl + 2;
    // The +2 covers the record's own trailing CRLF, which must be present
    // and byte-exact: silently tolerating a missing terminator would let
    // a bulk string swallow the next command's bytes.
    if (buf_.size() < data_pos + static_cast<size_t>(len) + 2) {
      return NextStatus::kNeedMore;
    }
    if (buf_[data_pos + len] != '\r' || buf_[data_pos + len + 1] != '\n') {
      *error = "Protocol error: bulk string missing terminator";
      return NextStatus::kError;
    }
    arg_spans_.emplace_back(data_pos, static_cast<size_t>(len));
    parse_pos_ = data_pos + static_cast<size_t>(len) + 2;
    if (--multibulk_ == 0) {
      args->clear();
      args->reserve(arg_spans_.size());
      for (const auto& [off, n] : arg_spans_) {
        args->emplace_back(buf_, off, n);  // copy: buf_ gets compacted below
      }
      multibulk_ = -1;
      arg_spans_.clear();
      buf_.erase(0, parse_pos_);
      parse_pos_ = 0;
      return NextStatus::kRequest;
    }
  }
}

NextStatus Parser::NextInline(std::vector<std::string>* args,
                              std::string* error) {
  // Inline commands end at '\n' (a preceding '\r' is stripped below), the
  // same leniency telnet sessions rely on with real Redis.
  const size_t nl = buf_.find('\n');
  if (nl == std::string::npos) {
    if (buf_.size() > kMaxInlineLine) {
      *error = "Protocol error: too big inline request";
      return NextStatus::kError;
    }
    return NextStatus::kNeedMore;
  }
  const size_t end = (nl > 0 && buf_[nl - 1] == '\r') ? nl - 1 : nl;
  args->clear();
  size_t i = 0;
  while (i < end) {
    while (i < end && (buf_[i] == ' ' || buf_[i] == '\t')) {
      ++i;
    }
    const size_t start = i;
    while (i < end && buf_[i] != ' ' && buf_[i] != '\t') {
      ++i;
    }
    if (i > start) {
      args->emplace_back(buf_, start, i - start);
    }
  }
  buf_.erase(0, nl + 1);
  parse_pos_ = 0;
  if (args->empty()) {
    // Whitespace-only line: not a command, keep scanning.
    return NextStatus::kNeedMore;
  }
  return NextStatus::kRequest;
}

// ---- reply encoders -------------------------------------------------------

std::string SimpleString(std::string_view s) {
  return "+" + std::string(s) + "\r\n";
}

std::string Error(std::string_view msg) {
  std::string out = "-";
  out.reserve(msg.size() + 3);
  for (const char c : msg) {
    // A raw \r or \n inside the message would end the reply early and
    // hide the rest from the client; sanitize like Redis does.
    out.push_back(c == '\r' || c == '\n' ? ' ' : c);
  }
  out += "\r\n";
  return out;
}

std::string Integer(int64_t n) {
  return ":" + std::to_string(n) + "\r\n";
}

std::string BulkString(std::string_view s) {
  return "$" + std::to_string(s.size()) + "\r\n" + std::string(s) + "\r\n";
}

std::string NullBulk() { return "$-1\r\n"; }

std::string ArrayHeader(size_t n) {
  return "*" + std::to_string(n) + "\r\n";
}

}  // namespace bedrockkv::resp
