#include "bedrockkv/log.h"

#include <cerrno>
#include <cstring>

#include "bedrockkv/crc32.h"

#include <unistd.h>  // ::read / ::write

namespace bedrockkv::log {
namespace {

// Little-endian fixed32 encoding: keeps WAL files byte-identical across
// architectures (big-endian machines would otherwise write other bytes).
void PutFixed32(char* dst, uint32_t v) {
  dst[0] = static_cast<char>(v & 0xffu);
  dst[1] = static_cast<char>((v >> 8) & 0xffu);
  dst[2] = static_cast<char>((v >> 16) & 0xffu);
  dst[3] = static_cast<char>((v >> 24) & 0xffu);
}

uint32_t GetFixed32(const char* src) {
  return static_cast<uint32_t>(static_cast<unsigned char>(src[0])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(src[1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(src[2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(src[3])) << 24);
}

}  // namespace

// ---- Writer ----------------------------------------------------------------

Status Writer::WriteAll(const char* data, size_t n) {
  size_t done = 0;
  while (done < n) {
    const ssize_t w = ::write(fd_, data + done, n - done);
    if (w < 0) {
      if (errno == EINTR) {
        continue;  // interrupted by a signal: retry the same range
      }
      return Status::IOError(std::string("WAL write failed: ") +
                             std::strerror(errno));
    }
    done += static_cast<size_t>(w);  // short writes are legal: continue
  }
  return Status::Ok();
}

Status Writer::AddRecord(std::string_view payload) {
  const char* p = payload.data();
  size_t left = payload.size();
  bool begin = true;
  Status s;
  do {
    const size_t leftover = kBlockSize - block_offset_;
    if (leftover < kHeaderSize) {
      // Cannot even fit a header in the remaining block tail: zero-pad it
      // (readers skip short tails and zero headers) and start a new block.
      if (leftover > 0) {
        static constexpr char kZeros[kHeaderSize - 1] = {0};  // max pad = 8B
        s = WriteAll(kZeros, leftover);
        if (!s.ok()) {
          return s;
        }
      }
      block_offset_ = 0;
    }

    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t frag = left < avail ? left : avail;
    RecordType type;
    if (begin && frag == left) {
      type = kFullType;
    } else if (begin) {
      type = kFirstType;
    } else if (frag == left) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, p, frag);
    if (!s.ok()) {
      return s;
    }
    p += frag;
    left -= frag;
    begin = false;
  } while (left > 0);

  return Status::Ok();
}

Status Writer::EmitPhysicalRecord(RecordType type, const char* payload,
                                  size_t len) {
  char header[kHeaderSize];
  PutFixed32(header, static_cast<uint32_t>(len));
  uint32_t crc = Crc32(&type, 1);           // crc covers type + payload
  crc = Crc32(payload, len, crc);
  PutFixed32(header + 4, crc);
  header[8] = static_cast<char>(type);

  // One contiguous ::write for header+payload: one syscall, no writev
  // juggling. A torn write is still possible under crash — the Reader
  // exists precisely to detect and bound it.
  char buf[kHeaderSize + kBlockSize];
  std::memcpy(buf, header, kHeaderSize);
  if (len > 0) {
    std::memcpy(buf + kHeaderSize, payload, len);
  }
  Status s = WriteAll(buf, kHeaderSize + len);
  if (s.ok()) {
    block_offset_ += kHeaderSize + len;  // <= kBlockSize by construction
  }
  return s;
}

// ---- Reader ----------------------------------------------------------------

Reader::Result Reader::ReadPhysicalRecord(std::string_view* fragment,
                                          uint8_t* type, uint64_t* end_offset) {
  for (;;) {
    if (pos_ + kHeaderSize > buffer_.size()) {
      if (eof_) {
        return Result::kEof;
      }
      // Anything shorter than a header here is block padding: drop it and
      // pull in the next block.
      pos_ = buffer_.size();
      char block[kBlockSize];
      size_t n = 0;
      while (n < kBlockSize) {
        const ssize_t got = ::read(fd_, block + n, kBlockSize - n);
        if (got < 0) {
          if (errno == EINTR) {
            continue;
          }
          return Result::kCorruption;
        }
        if (got == 0) {
          eof_ = true;  // clean end of file
          break;
        }
        n += static_cast<size_t>(got);
      }
      if (n == 0) {
        return Result::kEof;
      }
      bytes_read_ += n;
      buffer_.assign(block, n);
      pos_ = 0;
      continue;
    }

    const uint32_t length = GetFixed32(buffer_.data() + pos_);
    const uint32_t stored_crc = GetFixed32(buffer_.data() + pos_ + 4);
    const auto t = static_cast<uint8_t>(buffer_[pos_ + 8]);
    pos_ += kHeaderSize;

    if (t == kZeroType && length == 0 && stored_crc == 0) {
      // All-zero header: block padding or remnants of a torn overwrite.
      // Skip the rest of this block and resync there.
      pos_ = buffer_.size();
      continue;
    }

    if (length > kBlockSize - kHeaderSize || pos_ + length > buffer_.size()) {
      // The length field points beyond this block: a torn tail (crash
      // mid-write) or corruption. Both mean "no intact record here" —
      // the parser must not trust anything after this point.
      return Result::kCorruption;
    }

    *fragment = std::string_view(buffer_.data() + pos_, length);
    pos_ += length;
    *end_offset = bytes_read_ - buffer_.size() + pos_;
    *type = t;

    uint32_t actual = Crc32(&t, 1);
    actual = Crc32(fragment->data(), length, actual);
    if (actual != stored_crc) {
      return Result::kCorruption;
    }
    return Result::kOk;
  }
}

Reader::Result Reader::ReadRecord(std::string* payload,
                                  uint64_t* corruption_offset) {
  const auto fail = [this, corruption_offset]() {
    if (corruption_offset != nullptr) {
      *corruption_offset = last_good_end_;
    }
    return Result::kCorruption;
  };

  scratch_.clear();
  bool in_fragmented = false;
  for (;;) {
    std::string_view fragment;
    uint8_t type = kZeroType;
    uint64_t end_offset = 0;
    switch (ReadPhysicalRecord(&fragment, &type, &end_offset)) {
      case Result::kOk:
        break;
      case Result::kEof:
        // Dying in the middle of a fragmented record is a torn write.
        return in_fragmented ? fail() : Result::kEof;
      case Result::kCorruption:
        return fail();
    }

    switch (type) {
      case kFullType:
        if (in_fragmented) {
          return fail();  // new record inside a fragmented one: mixed state
        }
        last_good_end_ = end_offset;
        payload->assign(fragment);
        return Result::kOk;
      case kFirstType:
        if (in_fragmented) {
          return fail();
        }
        scratch_.assign(fragment);
        in_fragmented = true;
        break;
      case kMiddleType:
        if (!in_fragmented) {
          return fail();
        }
        scratch_.append(fragment);
        break;
      case kLastType: {
        if (!in_fragmented) {
          return fail();
        }
        scratch_.append(fragment);
        last_good_end_ = end_offset;
        *payload = std::move(scratch_);
        in_fragmented = false;
        return Result::kOk;
      }
      default:
        return fail();  // unknown type byte: never trust the rest
    }
  }
}

}  // namespace bedrockkv::log
