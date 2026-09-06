#include "bedrockkv/log.h"

#include <cerrno>
#include <cstring>

#include "bedrockkv/crc32.h"
#include "bedrockkv/encoding.h"

#include <unistd.h>  // ::read / ::write

namespace bedrockkv::log {

// ---- Writer ----------------------------------------------------------------

namespace {

// Block tail shorter than a header is zero-padded (readers skip zero
// headers); max pad = kHeaderSize - 1 bytes.
constexpr char kZeros[kHeaderSize - 1] = {0};

}  // namespace

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

Status Writer::EncodeRecord(std::string_view payload, uint64_t absolute_offset,
                            std::string* out) {
  out->clear();
  size_t block_off = absolute_offset % kBlockSize;
  const char* p = payload.data();
  size_t left = payload.size();
  bool begin = true;
  do {
    const size_t leftover = kBlockSize - block_off;
    if (leftover < kHeaderSize) {
      // Cannot even fit a header in the remaining block tail: zero-pad it
      // (readers skip short tails and zero headers) and start a new block.
      if (leftover > 0) {
        out->append(kZeros, leftover);
      }
      block_off = 0;
    }

    const size_t avail = kBlockSize - block_off - kHeaderSize;
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

    // Header + payload as one contiguous span: the sync path writes it in
    // a single syscall, the async path submits one SQE.
    char header[kHeaderSize];
    PutFixed32(header, static_cast<uint32_t>(frag));
    uint32_t crc = Crc32(&type, 1);  // crc covers type + payload
    crc = Crc32(p, frag, crc);
    PutFixed32(header + 4, crc);
    header[8] = static_cast<char>(type);
    out->append(header, kHeaderSize);
    out->append(p, frag);

    p += frag;
    left -= frag;
    begin = false;
    block_off += kHeaderSize + frag;  // <= kBlockSize by construction
  } while (left > 0);

  return Status::Ok();
}

Status Writer::AddRecord(std::string_view payload) {
  std::string buf;
  const Status s = EncodeRecord(payload, file_offset_, &buf);
  if (!s.ok()) {
    return s;
  }
  // One contiguous write per logical record (fragments merged). A torn
  // write is still possible under crash — the Reader exists to bound it.
  const Status w = WriteAll(buf.data(), buf.size());
  if (w.ok()) {
    file_offset_ += buf.size();
  }
  return w;
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
