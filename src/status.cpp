#include "bedrockkv/status.h"

namespace bedrockkv {

std::string Status::ToString() const {
  switch (code_) {
    case Code::kOk:
      return "OK";
    case Code::kNotFound:
      return "NotFound: " + message_;
    case Code::kCorruption:
      return "Corruption: " + message_;
    case Code::kIOError:
      return "IOError: " + message_;
    case Code::kInvalidArgument:
      return "InvalidArgument: " + message_;
    case Code::kNotSupported:
      return "NotSupported: " + message_;
  }
  return "Unknown: " + message_;
}

}  // namespace bedrockkv
