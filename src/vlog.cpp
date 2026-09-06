#include "bedrockkv/vlog.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "bedrockkv/crc32.h"
#include "bedrockkv/encoding.h"

namespace bedrockkv {
namespace {

// [crc u32][klen u32][vsize u32]
constexpr size_t kEntryHeaderSize = 12;

}  // namespace

std::string EncodeValuePointer(const ValuePointer& p) {
  std::string out;
  out.reserve(kValuePointerSize);
  out.push_back(kValuePointerTag);
  PutFixed64(&out, p.vlog_number);
  PutFixed64(&out, p.offset);
  PutFixed32(&out, p.value_size);
  return out;
}

bool DecodeValuePointer(std::string_view stored, ValuePointer* p) {
  if (stored.size() != kValuePointerSize ||
      stored[0] != kValuePointerTag) {
    return false;
  }
  const char* d = stored.data() + 1;
  p->vlog_number = GetFixed64(d);
  p->offset = GetFixed64(d + 8);
  p->value_size = GetFixed32(d + 16);
  return true;
}

std::string VLog::FileName(uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.vlog",
                static_cast<unsigned long long>(number));
  return buf;
}

std::unique_ptr<VLog> VLog::Open(const std::string& dir, uint64_t number,
                                 Status* status) {
  const std::string path = dir + "/" + FileName(number);
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    if (status != nullptr) {
      *status = Status::IOError("cannot open " + path + ": " +
                                std::string(std::strerror(errno)));
    }
    return nullptr;
  }
  const uint64_t end = static_cast<uint64_t>(::lseek(fd, 0, SEEK_END));
  if (status != nullptr) {
    *status = Status::Ok();
  }
  return std::unique_ptr<VLog>(new VLog(fd, number, end));
}

VLog::~VLog() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Status VLog::Append(std::string_view key, std::string_view value,
                    uint64_t* offset, uint64_t* entry_bytes) {
  *offset = end_.load(std::memory_order_relaxed);
  // Build the entry with a zero crc first, then patch it in — one
  // buffer, one write syscall per append. The 4-byte crc field is the
  // buffer head; klen/vsize/key/value follow (crc covers them all).
  std::string buf;
  buf.reserve(kEntryHeaderSize + key.size() + value.size());
  buf.resize(4);
  PutFixed32(&buf, static_cast<uint32_t>(key.size()));
  PutFixed32(&buf, static_cast<uint32_t>(value.size()));
  buf.append(key);
  buf.append(value);
  const uint32_t crc =
      Crc32(buf.data() + 4, buf.size() - 4);  // everything after the crc field
  PutFixed32(buf.data(), crc);                // patch the crc field in place

  size_t written = 0;
  while (written < buf.size()) {
    const ssize_t n = ::write(fd_, buf.data() + written, buf.size() - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::IOError("vlog append failed: " +
                             std::string(std::strerror(errno)));
    }
    written += static_cast<size_t>(n);
  }
  end_.store(end_.load(std::memory_order_relaxed) + buf.size(),
             std::memory_order_relaxed);
  if (entry_bytes != nullptr) {
    *entry_bytes = buf.size();
  }
  return Status::Ok();
}

Status VLog::ReadValue(uint64_t offset, uint32_t expected_size,
                       std::string* value) const {
  char header[kEntryHeaderSize];
  const uint64_t end = end_.load(std::memory_order_relaxed);
  if (offset > end || end - offset < kEntryHeaderSize) {
    return Status::NotFound("value pointer past end of vlog " +
                            FileName(number_));
  }
  ssize_t n = ::pread(fd_, header, sizeof(header), static_cast<off_t>(offset));
  if (n < 0) {
    return Status::IOError("vlog pread header failed: " +
                           std::string(std::strerror(errno)));
  }
  if (static_cast<size_t>(n) < sizeof(header)) {
    return Status::NotFound("torn vlog tail in " + FileName(number_));
  }
  const uint32_t klen = GetFixed32(header + 4);
  const uint32_t vsize = GetFixed32(header + 8);
  // Range check FIRST: a torn tail's garbage header would otherwise be
  // reported as a size mismatch (corruption) when it is really just an
  // entry that was never fully written (bounded loss).
  if (klen > end || vsize > end ||
      klen + vsize > end - offset - kEntryHeaderSize) {
    return Status::NotFound("torn vlog tail in " + FileName(number_));
  }
  if (vsize != expected_size) {
    return Status::Corruption("vlog entry size " + std::to_string(vsize) +
                              " != pointer size " +
                              std::to_string(expected_size));
  }
  std::string payload(klen + vsize, '\0');
  n = ::pread(fd_, payload.data(), payload.size(),
              static_cast<off_t>(offset + kEntryHeaderSize));
  if (n < 0 || static_cast<size_t>(n) < payload.size()) {
    return Status::NotFound("torn vlog tail in " + FileName(number_));
  }
  // crc covers klen + vsize + key + value: header[4..12] then payload.
  const uint32_t crc =
      Crc32(payload.data(), payload.size(), Crc32(header + 4, 8));
  if (crc != GetFixed32(header)) {
    return Status::Corruption("vlog entry CRC mismatch in " +
                              FileName(number_));
  }
  value->assign(payload.data() + klen, vsize);
  return Status::Ok();
}

Status VLog::Sync() const {
  if (::fsync(fd_) != 0) {
    return Status::IOError("vlog fsync failed: " +
                           std::string(std::strerror(errno)));
  }
  return Status::Ok();
}

Status VLog::ScanEntries(
    uint64_t limit,
    const std::function<void(std::string_view key, std::string_view value,
                             uint64_t offset)>& fn) const {
  // Per-entry preads (header, then payload): the GC runs in the
  // background where two syscalls per reclaimed entry are cheap, and
  // this keeps the code free of chunk-boundary plumbing.
  const uint64_t file_end =
      limit < end_.load(std::memory_order_relaxed)
          ? limit
          : end_.load(std::memory_order_relaxed);
  uint64_t off = 0;
  while (off + kEntryHeaderSize <= file_end) {
    char header[kEntryHeaderSize];
    const ssize_t n =
        ::pread(fd_, header, sizeof(header), static_cast<off_t>(off));
    if (n < 0) {
      return Status::IOError("vlog scan pread failed: " +
                             std::string(std::strerror(errno)));
    }
    if (static_cast<size_t>(n) < sizeof(header)) {
      break;  // torn tail
    }
    const uint32_t klen = GetFixed32(header + 4);
    const uint32_t vsize = GetFixed32(header + 8);
    const uint64_t total = kEntryHeaderSize + klen + vsize;
    if (off + total > file_end) {
      break;  // torn tail (or an entry never fully appended)
    }
    std::string payload(klen + vsize, '\0');
    const ssize_t m = ::pread(fd_, payload.data(), payload.size(),
                              static_cast<off_t>(off + kEntryHeaderSize));
    if (m < 0 || static_cast<uint64_t>(m) < payload.size()) {
      break;
    }
    if (Crc32(payload.data(), payload.size(), Crc32(header + 4, 8)) !=
        GetFixed32(header)) {
      break;  // CRC-invalid: stop scanning, the rest is garbage
    }
    fn(std::string_view(payload.data(), klen),
       std::string_view(payload.data() + klen, vsize), off);
    off += total;
  }
  return Status::Ok();
}

}  // namespace bedrockkv
