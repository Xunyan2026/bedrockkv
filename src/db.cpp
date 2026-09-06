#include "bedrockkv/db.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "bedrockkv/encoding.h"
#include "bedrockkv/fs_util.h"

namespace bedrockkv {
namespace {

constexpr uint64_t kSyncThresholdBytes = 1u << 20;  // 1 MiB

// MANIFEST record tags (each is the payload of one log::Writer record).
constexpr uint8_t kManifestTagSstFile = 1;
constexpr uint8_t kManifestTagLogNumber = 2;

// The MANIFEST is a full snapshot of the L0 file list plus the current
// log generation, rewritten atomically on every change. Using our own
// log format for it is deliberate: records are CRC-protected and the
// reader machinery already exists.
struct ManifestState {
  std::vector<sst::FileMeta> files;  // ascending file number
  uint64_t log_number = 0;
  bool has_log_number = false;
};

bool ParseFileMetaRecord(const std::string& r, sst::FileMeta* m) {
  // [tag][file_number u64][smallest_seq u64][largest_seq u64]
  // [entry_count u64][skey_len u32][skey][lkey_len u32][lkey]
  size_t pos = 1;
  if (r.size() < pos + 32 + 4) {
    return false;
  }
  m->file_number = GetFixed64(r.data() + pos); pos += 8;
  m->smallest_seq = GetFixed64(r.data() + pos); pos += 8;
  m->largest_seq = GetFixed64(r.data() + pos); pos += 8;
  m->entry_count = GetFixed64(r.data() + pos); pos += 8;
  const uint32_t slen = GetFixed32(r.data() + pos); pos += 4;
  if (pos + slen + 4 > r.size()) {
    return false;
  }
  m->smallest_key = r.substr(pos, slen); pos += slen;
  const uint32_t llen = GetFixed32(r.data() + pos); pos += 4;
  if (pos + llen != r.size()) {
    return false;
  }
  m->largest_key = r.substr(pos, llen);
  return true;
}

// Returns false on any corruption — a MANIFEST can never be torn (it is
// published by rename), so corruption here means real damage.
bool ParseManifest(const std::string& path, ManifestState* state) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return false;
  }
  log::Reader reader(fd);
  std::string record;
  uint64_t last_file_number = 0;
  bool ok = true;
  for (;;) {
    uint64_t unused = 0;
    const auto res = reader.ReadRecord(&record, &unused);
    if (res == log::Reader::Result::kEof) {
      break;
    }
    if (res == log::Reader::Result::kCorruption || record.empty()) {
      ok = false;
      break;
    }
    const uint8_t tag = static_cast<uint8_t>(record[0]);
    if (tag == kManifestTagSstFile) {
      sst::FileMeta m;
      if (!ParseFileMetaRecord(record, &m) || m.file_number <= last_file_number) {
        ok = false;
        break;
      }
      last_file_number = m.file_number;
      state->files.push_back(std::move(m));
    } else if (tag == kManifestTagLogNumber) {
      if (record.size() != 9) {
        ok = false;
        break;
      }
      state->log_number = GetFixed64(record.data() + 1);
      state->has_log_number = true;
    } else {
      ok = false;
      break;
    }
  }
  ::close(fd);
  return ok;
}

// Matches "<digits><suffix>" exactly and extracts the number.
bool ParseNumberedFile(const std::string& name, const char* suffix,
                       uint64_t* number) {
  const size_t suflen = std::strlen(suffix);
  if (name.size() <= suflen ||
      name.compare(name.size() - suflen, suflen, suffix) != 0) {
    return false;
  }
  uint64_t n = 0;
  for (size_t i = 0; i < name.size() - suflen; ++i) {
    const char c = name[i];
    if (c < '0' || c > '9') {
      return false;
    }
    if (n > (std::numeric_limits<uint64_t>::max() - (c - '0')) / 10) {
      return false;
    }
    n = n * 10 + static_cast<uint64_t>(c - '0');
  }
  *number = n;
  return true;
}

// Defensive parse of a log payload. Every field boundary is checked; a
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

std::string DB::SstFileName(uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.sst",
                static_cast<unsigned long long>(number));
  return buf;
}

std::string DB::LogFileName(uint64_t number) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu.log",
                static_cast<unsigned long long>(number));
  return buf;
}

void DB::RemoveOrphanFiles(const std::vector<sst::FileMeta>& live) const {
  std::set<uint64_t> live_sst;
  for (const sst::FileMeta& m : live) {
    live_sst.insert(m.file_number);
  }
  DIR* d = ::opendir(dir_.c_str());
  if (d == nullptr) {
    return;
  }
  while (const dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name == kManifestTmpFileName) {
      ::unlink((dir_ + "/" + name).c_str());  // crashed manifest rewrite
      continue;
    }
    uint64_t number = 0;
    if (ParseNumberedFile(name, ".sst", &number) && !live_sst.count(number)) {
      // Written but never published by a MANIFEST (crash mid-flush).
      ::unlink((dir_ + "/" + name).c_str());
    } else if (ParseNumberedFile(name, ".log", &number) && number != log_number_) {
      // A log generation the MANIFEST no longer references.
      ::unlink((dir_ + "/" + name).c_str());
    }
  }
  ::closedir(d);
}

std::unique_ptr<DB> DB::Open(const std::string& dir, const Options& options,
                             Status* status) {
  const auto fail = [status](const std::string& msg) {
    if (status != nullptr) {
      *status = Status::IOError(msg);
    }
    return std::unique_ptr<DB>();
  };
  const auto corrupt = [status](const std::string& msg) {
    if (status != nullptr) {
      *status = Status::Corruption(msg);
    }
    return std::unique_ptr<DB>();
  };

  if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    return fail("cannot create directory " + dir + ": " +
                std::strerror(errno));
  }

  auto db = std::unique_ptr<DB>(new DB());
  db->dir_ = dir;
  db->sync_mode_ = options.sync_mode;
  db->write_buffer_size_ = options.write_buffer_size;

  // ---- 1. read the MANIFEST (the database's table of contents) ----
  ManifestState ms;
  if (::access(db->ManifestPath().c_str(), F_OK) == 0) {
    if (!ParseManifest(db->ManifestPath(), &ms) || !ms.has_log_number) {
      return corrupt("MANIFEST is corrupt");
    }
  } else {
    ms.log_number = 1;  // fresh database
  }
  db->log_number_ = ms.log_number;

  // ---- 2. open every published SST ----
  uint64_t max_seen_number = ms.log_number;
  for (const sst::FileMeta& meta : ms.files) {
    Status s = Status::Ok();
    auto table = sst::Table::Open(meta.file_number, db->SstPath(meta.file_number), &s);
    if (table == nullptr) {
      return corrupt("cannot open SST " + DB::SstFileName(meta.file_number) +
                     ": " + s.message());
    }
    max_seen_number = std::max(max_seen_number, meta.file_number);
    db->l0_.push_back({meta, std::move(table)});
  }

  // ---- 3. remove leftovers from crashed flushes / manifest rewrites ----
  db->RemoveOrphanFiles(ms.files);
  db->next_file_number_ = max_seen_number + 1;

  // ---- 4. open the current log and replay it ----
  // O_APPEND: after a tail truncation below, writes must continue at the
  // NEW end of file — a plain fd offset could point past the truncation
  // point and punch a hole of zeros into the log.
  const int fd = ::open(db->LogPath(db->log_number_).c_str(),
                        O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return fail("cannot open log " +
                DB::LogFileName(db->log_number_) + ": " +
                std::string(std::strerror(errno)));
  }
  db->log_fd_ = fd;
  db->memtable_ = std::make_unique<MemTable>();

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
        return fail("cannot truncate torn log tail: " +
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
        return fail("cannot truncate malformed log record: " +
                    std::string(std::strerror(errno)));
      }
      db->wal_truncated_ = true;
      break;
    }

    if (type == kTypeValue) {
      db->memtable_->Put(seq, key, value);
    } else {
      db->memtable_->Delete(seq, key);
    }
    if (seq > max_seq) {
      max_seq = seq;
    }
    applied_end = reader.last_good_end();
  }

  // The sequence continues after everything durable: SST records (via
  // the MANIFEST) and replayed log records.
  for (const SstFile& f : db->l0_) {
    max_seq = std::max(max_seq, f.meta.largest_seq);
  }
  db->next_seq_ = max_seq + 1;
  if (status != nullptr) {
    *status = Status::Ok();
  }

  // The appender must continue at the REAL end of file: its in-block
  // position decides where records fragment, so a stale offset would
  // corrupt the log at real block boundaries (found by the reopen tests).
  const uint64_t log_end = static_cast<uint64_t>(::lseek(fd, 0, SEEK_END));
  db->log_writer_ = std::make_unique<log::Writer>(fd, log_end);
  return db;
}

DB::~DB() {
  if (log_fd_ >= 0) {
    ::close(log_fd_);
  }
}

Status DB::WriteManifest() {
  const std::string tmp = dir_ + "/" + kManifestTmpFileName;
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::IOError("cannot create " + tmp + ": " +
                           std::string(std::strerror(errno)));
  }
  Status result = Status::Ok();
  {
    log::Writer writer(fd, 0);
    for (const SstFile& f : l0_) {
      const sst::FileMeta& m = f.meta;
      std::string p;
      p.push_back(static_cast<char>(kManifestTagSstFile));
      PutFixed64(&p, m.file_number);
      PutFixed64(&p, m.smallest_seq);
      PutFixed64(&p, m.largest_seq);
      PutFixed64(&p, m.entry_count);
      PutFixed32(&p, static_cast<uint32_t>(m.smallest_key.size()));
      p.append(m.smallest_key);
      PutFixed32(&p, static_cast<uint32_t>(m.largest_key.size()));
      p.append(m.largest_key);
      result = writer.AddRecord(p);
      if (!result.ok()) {
        break;
      }
    }
    if (result.ok()) {
      std::string p;
      p.push_back(static_cast<char>(kManifestTagLogNumber));
      PutFixed64(&p, log_number_);
      result = writer.AddRecord(p);
    }
  }
  if (result.ok() && ::fsync(fd) != 0) {
    result = Status::IOError(std::string("fsync MANIFEST.tmp failed: ") +
                             std::strerror(errno));
  }
  ::close(fd);
  if (!result.ok()) {
    ::unlink(tmp.c_str());
    return result;
  }
  // Atomic publish: rename never exposes a torn MANIFEST. The directory
  // fsync makes the rename itself durable.
  if (::rename(tmp.c_str(), ManifestPath().c_str()) != 0) {
    return Status::IOError(std::string("rename MANIFEST failed: ") +
                           std::strerror(errno));
  }
  return fs::SyncDir(dir_);
}

Status DB::FlushMemTable() {
  if (memtable_->Count() == 0) {
    return Status::Ok();
  }
  const uint64_t sst_number = next_file_number_++;
  const uint64_t new_log_number = next_file_number_++;
  const auto cleanup = [this](uint64_t sst, uint64_t log) {
    ::unlink(SstPath(sst).c_str());
    ::unlink(LogPath(log).c_str());
  };

  // 1. The NEXT log generation must exist before the MANIFEST names it:
  //    a crash right after the switch has to find an openable log.
  const int new_fd =
      ::open(LogPath(new_log_number).c_str(), O_RDWR | O_CREAT | O_TRUNC | O_APPEND,
             0644);
  if (new_fd < 0) {
    next_file_number_ -= 2;
    return Status::IOError("cannot create " + LogFileName(new_log_number) +
                           ": " + std::string(std::strerror(errno)));
  }
  if (::fsync(new_fd) != 0) {
    const int saved = errno;
    ::close(new_fd);
    next_file_number_ -= 2;
    return Status::IOError("fsync " + LogFileName(new_log_number) +
                           " failed: " + std::string(std::strerror(saved)));
  }

  // 2. Export the frozen memtable into an SST (single-threaded: no write
  //    can interleave, so the memtable is quiescent during the build).
  sst::Builder builder;
  sst::FileMeta meta;
  meta.file_number = sst_number;
  Status s = Status::Ok();
  memtable_->ForEach([&s, &builder](uint64_t seq, uint8_t type,
                                    std::string_view key,
                                    std::string_view value) {
    if (s.ok()) {
      s = builder.Add(key, seq, type, value);
    }
  });
  if (s.ok()) {
    s = builder.Finish(SstPath(sst_number), &meta);
  }
  if (!s.ok()) {
    ::close(new_fd);
    cleanup(sst_number, new_log_number);
    next_file_number_ -= 2;
    return s;
  }

  // 3. Publish: MANIFEST names the SST and the NEW log generation. The
  //    new log file already exists (step 1), so naming it here is safe;
  //    naming the OLD one would be fatal — this manifest is the last
  //    word, and the old log is about to be deleted.
  const uint64_t old_log_number = log_number_;
  log_number_ = new_log_number;
  l0_.push_back({meta, nullptr});
  s = WriteManifest();
  if (!s.ok()) {
    l0_.pop_back();
    log_number_ = old_log_number;
    ::close(new_fd);
    cleanup(sst_number, new_log_number);
    next_file_number_ -= 2;
    return s;
  }

  // 4. Open what we just wrote — doubles as a CRC self-check.
  s = Status::Ok();
  auto table = sst::Table::Open(sst_number, SstPath(sst_number), &s);
  if (table == nullptr) {
    l0_.pop_back();
    ::close(new_fd);
    cleanup(sst_number, new_log_number);
    next_file_number_ -= 2;
    return s;
  }
  l0_.back().table = std::move(table);

  // 5. Switch the writer to the new log generation, retire the old one.
  //    The old log's records are now redundant (they live in the SST),
  //    so it can be deleted; its fsync state dies with it.
  const int old_fd = log_fd_;
  log_fd_ = new_fd;
  log_writer_ = std::make_unique<log::Writer>(new_fd, 0);
  unsynced_bytes_ = 0;
  if (old_fd >= 0) {
    ::close(old_fd);
    ::unlink(LogPath(old_log_number).c_str());
  }

  // 6. Fresh write buffer.
  memtable_ = std::make_unique<MemTable>();
  return Status::Ok();
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
  if (need && ::fsync(log_fd_) != 0) {
    return Status::IOError(std::string("log fsync failed: ") +
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
  memtable_->Put(next_seq_, key, value);
  ++next_seq_;
  if (memtable_->ApproximateSize() >= write_buffer_size_) {
    return FlushMemTable();
  }
  return Status::Ok();
}

Status DB::Delete(std::string_view key) {
  const Status s = WriteRecord(kTypeDeletion, key, std::string_view());
  if (!s.ok()) {
    return s;
  }
  memtable_->Delete(next_seq_, key);
  ++next_seq_;
  if (memtable_->ApproximateSize() >= write_buffer_size_) {
    return FlushMemTable();
  }
  return Status::Ok();
}

Status DB::Get(std::string_view key, std::string* value) const {
  switch (memtable_->Get(key, value)) {
    case MemTable::Lookup::kFound:
      return Status::Ok();
    case MemTable::Lookup::kDeleted:
      return Status::NotFound("deleted: " + std::string(key));
    case MemTable::Lookup::kMissing:
      break;
  }

  // L0, newest file first. The first file that CONTAINS the key decides:
  // its newest version may be a tombstone, and an older file's value must
  // not resurrect through it.
  for (auto it = l0_.rbegin(); it != l0_.rend(); ++it) {
    Status s = Status::Ok();
    switch (it->table->Get(key, value, &s)) {
      case MemTable::Lookup::kFound:
        return Status::Ok();
      case MemTable::Lookup::kDeleted:
        return Status::NotFound("deleted: " + std::string(key));
      case MemTable::Lookup::kMissing:
        if (!s.ok()) {
          return s;  // real corruption, not an absent key
        }
        continue;
    }
  }
  return Status::NotFound("missing: " + std::string(key));
}

}  // namespace bedrockkv
