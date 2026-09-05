#include "bedrockkv/db.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "bedrockkv/encoding.h"

namespace bedrockkv {
namespace {

constexpr uint64_t kSyncThresholdBytes = 1u << 20;  // 1 MiB

std::string WalPath(const std::string& dir) {
  return dir + "/" + DB::kWalFileName;
}

// Defensive parse of a WAL payload. Every field boundary is checked; a
// record that got a valid CRC but impossible content must never happen
// (we wrote it), but storage code trusts nothing — a malformed record is
// treated exactly like a torn tail.
bool ParseRecord(const std::string& payload, uint64_t* seq, uint8_t* type,
                 std::string_view* key, std::string_view* value) {
  if (payload.size() < 8 + 1 + 4) {
    return false;
  }
  *seq = GetFixed64(payload.data());
  *type = static_cast<uint8_t>(payload[8]);
  const uint32_t klen = GetFixed32(payload.data() + 9);
  if (klen > payload.size() - 13) {
    return false;
  }
  *key = std::string_view(payload.data() + 13, klen);
  if (*type == kTypeValue) {
    if (payload.size() < 13 + klen + 4) {
      return false;
    }
    const uint32_t vlen = GetFixed32(payload.data() + 13 + klen);
    if (payload.size() != 13 + klen + 4 + static_cast<size_t>(vlen)) {
      return false;
    }
    *value = std::string_view(payload.data() + 17 + klen, vlen);
  } else if (*type == kTypeDeletion) {
    if (payload.size() != 13 + klen) {
      return false;
    }
    *value = std::string_view();
  } else {
    return false;  // unknown type byte
  }
  return true;
}

}  // namespace

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& options,
                             Status* status) {
  const auto fail = [status](const std::string& msg) {
    if (status != nullptr) {
      *status = Status::IOError(msg);
    }
    return std::unique_ptr<DB>();
  };

  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return fail("cannot create directory " + dir + ": " +
                std::strerror(errno));
  }
  // O_APPEND: after a tail truncation below, writes must continue at the
  // NEW end of file — a plain fd offset could point past the truncation
  // point and punch a hole of zeros into the log.
  const int fd = ::open(WalPath(dir).c_str(), O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return fail("cannot open WAL: " + std::string(std::strerror(errno)));
  }

  auto db = std::unique_ptr<DB>(new DB());
  db->wal_fd_ = fd;
  db->sync_mode_ = options.sync_mode;

  // ---- recovery: replay the WAL into a fresh MemTable ----
  std::string record;
  uint64_t max_seq = 0;
  uint64_t applied_end = 0;  // end offset of the last record we replayed
  log::Reader reader(fd);
  for (;;) {
    uint64_t corruption_offset = 0;
    const auto r = reader.ReadRecord(&record, &corruption_offset);
    if (r == log::Reader::Result::kEof) {
      break;
    }
    if (r == log::Reader::Result::kCorruption) {
      if (::ftruncate(fd, static_cast<off_t>(corruption_offset)) != 0 ||
          ::fsync(fd) != 0) {
        return fail("cannot truncate torn WAL tail: " +
                    std::string(std::strerror(errno)));
      }
      db->wal_truncated_ = true;
      break;
    }

    uint64_t seq = 0;
    uint8_t type = 0;
    std::string_view key, value;
    if (!ParseRecord(record, &seq, &type, &key, &value)) {
      // Intact CRC but garbage content: cut before this record and stop.
      if (::ftruncate(fd, static_cast<off_t>(applied_end)) != 0 ||
          ::fsync(fd) != 0) {
        return fail("cannot truncate malformed WAL record: " +
                    std::string(std::strerror(errno)));
      }
      db->wal_truncated_ = true;
      break;
    }

    if (type == kTypeValue) {
      db->memtable_.Put(seq, key, value);
    } else {
      db->memtable_.Delete(seq, key);
    }
    if (seq > max_seq) {
      max_seq = seq;
    }
    applied_end = reader.last_good_end();
  }

  db->next_seq_ = max_seq + 1;
  if (status != nullptr) {
    *status = Status::Ok();
  }

  // The appender must continue at the REAL end of file: its in-block
  // position decides where records fragment, so a stale offset would
  // corrupt the log at real block boundaries (found by the reopen tests).
  const uint64_t wal_end = static_cast<uint64_t>(::lseek(fd, 0, SEEK_END));
  db->log_writer_ = std::make_unique<log::Writer>(fd, wal_end);
  return db;
}

DB::~DB() {
  if (wal_fd_ >= 0) {
    ::close(wal_fd_);
  }
}

Status DB::WriteRecord(uint8_t type, std::string_view key,
                       std::string_view value) {
  std::string payload;
  PutFixed64(&payload, next_seq_);
  payload.push_back(static_cast<char>(type));
  PutFixed32(&payload, static_cast<uint32_t>(key.size()));
  payload.append(key);
  if (type == kTypeValue) {
    PutFixed32(&payload, static_cast<uint32_t>(value.size()));
    payload.append(value);
  }

  // WAL first, MemTable second: if we crash in between, the record is
  // simply replayed on recovery. The reverse order could lose an
  // acknowledged write — the cardinal sin of a persistent store.
  Status s = log_writer_->AddRecord(payload);
  if (!s.ok()) {
    return s;
  }
  unsynced_bytes_ += payload.size() + log::kHeaderSize;
  return MaybeSync();
}

Status DB::MaybeSync() {
  bool need = false;
  switch (sync_mode_) {
    case SyncMode::kSyncAlways:
      need = true;
      break;
    case SyncMode::kSyncPeriodic:
      need = unsynced_bytes_ >= kSyncThresholdBytes;
      break;
    case SyncMode::kSyncNever:
      return Status::Ok();
  }
  if (need && ::fsync(wal_fd_) != 0) {
    return Status::IOError(std::string("WAL fsync failed: ") +
                           std::strerror(errno));
  }
  unsynced_bytes_ = 0;
  return Status::Ok();
}

Status DB::Put(std::string_view key, std::string_view value) {
  const Status s = WriteRecord(kTypeValue, key, value);
  if (!s.ok()) {
    return s;
  }
  memtable_.Put(next_seq_, key, value);
  ++next_seq_;
  return Status::Ok();
}

Status DB::Delete(std::string_view key) {
  const Status s = WriteRecord(kTypeDeletion, key, std::string_view());
  if (!s.ok()) {
    return s;
  }
  memtable_.Delete(next_seq_, key);
  ++next_seq_;
  return Status::Ok();
}

Status DB::Get(std::string_view key, std::string* value) const {
  switch (memtable_.Get(key, value)) {
    case MemTable::Lookup::kFound:
      return Status::Ok();
    case MemTable::Lookup::kDeleted:
      return Status::NotFound("deleted: " + std::string(key));
    case MemTable::Lookup::kMissing:
      return Status::NotFound("missing: " + std::string(key));
  }
  return Status::NotFound("missing");
}

}  // namespace bedrockkv
