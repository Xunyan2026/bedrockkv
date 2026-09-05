// BedrockKV — Status: the single error-handling primitive of the engine.
//
// Every fallible operation in BedrockKV returns a Status instead of throwing.
// Rationale (mirrors leveldb's include/leveldb/status.h design):
//   * errors are expected values here (corruption, not-found, IO), so
//     exceptions add an unacceptable cost on the hot read/write path;
//   * a value type is trivially testable and serializable into logs.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace bedrockkv {

// [[nodiscard]] makes ignoring a returned Status a compile error:
// silently dropping "did my Put survive?" is exactly the bug class a
// storage engine must never allow.
class [[nodiscard]] Status {
 public:
  // Kept small (uint8_t) and dense: Status is returned by every API call,
  // so we keep it cheap to construct, copy and compare.
  enum class Code : std::uint8_t {
    kOk = 0,
    kNotFound,        // the requested key does not exist
    kCorruption,      // on-disk data failed validation (CRC, parse, ...)
    kIOError,         // the OS refused our file/system request
    kInvalidArgument, // caller passed something nonsensical
    kNotSupported,    // requested feature is not implemented/available
  };

  // Default construction = OK. This makes `Status s;` usable in code that
  // assigns a real result later without extra ceremony.
  Status() noexcept = default;

  // Named constructors instead of public ctor(Status::Code, msg):
  // call sites read as prose and each error site is greppable.
  static Status Ok() noexcept { return Status(); }
  static Status NotFound(std::string msg) {
    return Status(Code::kNotFound, std::move(msg));
  }
  static Status Corruption(std::string msg) {
    return Status(Code::kCorruption, std::move(msg));
  }
  static Status IOError(std::string msg) {
    return Status(Code::kIOError, std::move(msg));
  }
  static Status InvalidArgument(std::string msg) {
    return Status(Code::kInvalidArgument, std::move(msg));
  }
  static Status NotSupported(std::string msg) {
    return Status(Code::kNotSupported, std::move(msg));
  }

  bool ok() const noexcept { return code_ == Code::kOk; }
  Code code() const noexcept { return code_; }

  // The empty string for OK status: no allocation was made at all.
  const std::string& message() const noexcept { return message_; }

  // Human-readable, e.g. "Corruption: WAL record CRC mismatch at offset 4096".
  // Used by tests, logs and future crash dumps — never on the hot path.
  std::string ToString() const;

  // Two statuses are equal iff their codes match and, for error statuses,
  // their messages match. OK statuses always compare equal regardless of
  // their (always empty) message.
  friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && (a.ok() || a.message_ == b.message_);
  }

 private:
  Status(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  Code code_ = Code::kOk;
  std::string message_;  // empty when ok
};

}  // namespace bedrockkv
