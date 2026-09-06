#include "bedrockkv/db.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "bedrockkv/encoding.h"
#include "bedrockkv/fs_util.h"

namespace bedrockkv {
namespace {

constexpr uint64_t kSyncThresholdBytes = 1u << 20;  // 1 MiB

// PickCompaction source sentinel for "inputs come from L0".
constexpr size_t kL0Source = std::numeric_limits<size_t>::max();

// MANIFEST record tags (each is the payload of one log::Writer record).
constexpr uint8_t kManifestTagSstFile = 1;
constexpr uint8_t kManifestTagLogNumber = 2;

// The MANIFEST is a full snapshot of the SST list (with levels) plus the
// current log generation, rewritten atomically on every change. Using
// our own log format is deliberate: records are CRC-protected and the
// reader machinery already exists.
struct ManifestState {
  std::vector<sst::FileMeta> files;
  uint64_t log_number = 0;
  bool has_log_number = false;
};

// [tag][file_number u64][level u32][smallest_seq u64][largest_seq u64]
// [entry_count u64][file_size u64][skey_len u32][skey][lkey_len u32][lkey]
bool ParseFileMetaRecord(const std::string& r, sst::FileMeta* m) {
  constexpr size_t kFixed = 1 + 8 + 4 + 8 + 8 + 8 + 8 + 4;
  if (r.size() < kFixed + 4) {
    return false;
  }
  size_t pos = 1;
  m->file_number = GetFixed64(r.data() + pos); pos += 8;
  m->level = GetFixed32(r.data() + pos); pos += 4;
  m->smallest_seq = GetFixed64(r.data() + pos); pos += 8;
  m->largest_seq = GetFixed64(r.data() + pos); pos += 8;
  m->entry_count = GetFixed64(r.data() + pos); pos += 8;
  m->file_size = GetFixed64(r.data() + pos); pos += 8;
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
      if (!ParseFileMetaRecord(record, &m) ||
          m.file_number <= last_file_number || m.level >= kMaxLevels) {
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

void DB::RemoveOrphanFiles(const std::vector<sst::FileMeta>& live,
                           uint64_t log_floor) const {
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
      // Written but never published by a MANIFEST (crash mid-flush or
      // crash between manifest publish and input unlink).
      ::unlink((dir_ + "/" + name).c_str());
    } else if (ParseNumberedFile(name, ".log", &number) &&
               number < log_floor) {
      // Below the replay floor: the MANIFEST guarantees every record in
      // it either reached an SST or is worthless. At-or-above-floor logs
      // are KEPT — a memtable may still be pending flush with its only
      // records inside.
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
  db->options_ = options;
  db->sync_mode_ = options.sync_mode;

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

  // ---- 2. open every published SST and assemble the first Version ----
  auto version = std::make_shared<Version>();
  version->levels.resize(kMaxLevels - 1);  // levels[0] = L1 ... [5] = L6
  uint64_t max_seen_number = ms.log_number;
  for (const sst::FileMeta& meta : ms.files) {
    Status s = Status::Ok();
    auto table = sst::Table::Open(meta.file_number, db->SstPath(meta.file_number), &s);
    if (table == nullptr) {
      return corrupt("cannot open SST " + DB::SstFileName(meta.file_number) +
                     ": " + s.message());
    }
    max_seen_number = std::max(max_seen_number, meta.file_number);
    const TableRef ref{std::move(table), meta};
    if (meta.level == 0) {
      version->l0.push_back(ref);
    } else {
      version->levels[meta.level - 1].push_back(ref);
    }
  }
  // L0: newest file first. Ln (n>=1): key order.
  std::sort(version->l0.begin(), version->l0.end(),
            [](const TableRef& a, const TableRef& b) {
              return a.meta.file_number > b.meta.file_number;
            });
  for (auto& level : version->levels) {
    std::sort(level.begin(), level.end(),
              [](const TableRef& a, const TableRef& b) {
                return a.table->smallest_user_key() <
                       b.table->smallest_user_key();
              });
  }
  db->current_ = std::move(version);

  // ---- 3. enumerate the logs to replay ----
  // The MANIFEST's log number is a REPLAY FLOOR, not "the one log": a
  // shutdown/crash can leave a memtable pending flush in the background,
  // with its records in a retired log the MANIFEST does not yet name
  // (the flush that would name its replacement never finished). Every
  // generation >= the floor is replayed, oldest first; only generations
  // BELOW the floor are orphans.
  const uint64_t replay_floor = ms.log_number;
  std::vector<uint64_t> log_numbers;
  {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
      return fail("cannot read directory " + dir);
    }
    while (const dirent* e = ::readdir(d)) {
      uint64_t n = 0;
      if (ParseNumberedFile(e->d_name, ".log", &n) && n >= replay_floor) {
        log_numbers.push_back(n);
      }
    }
    ::closedir(d);
  }
  std::sort(log_numbers.begin(), log_numbers.end());
  if (log_numbers.empty()) {
    log_numbers.push_back(replay_floor);  // fresh or fully cleaned database
  }
  db->log_number_ = log_numbers.back();  // new writes append to the newest

  // ---- 4. remove leftovers from crashed flushes/compactions ----
  db->RemoveOrphanFiles(ms.files, replay_floor);
  db->next_file_number_ =
      std::max(max_seen_number, log_numbers.back()) + 1;

  // ---- 5. replay every log >= the floor, oldest first ----
  db->mem_ = std::make_shared<MemTable>();
  std::string record;
  uint64_t max_seq = 0;
  for (const uint64_t number : log_numbers) {
    // O_APPEND: after a tail truncation below, writes must continue at
    // the NEW end of file — a plain fd offset could point past the
    // truncation point and punch a hole of zeros into the log.
    const int fd = ::open(db->LogPath(number).c_str(),
                          O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
      return fail("cannot open log " + DB::LogFileName(number) + ": " +
                  std::string(std::strerror(errno)));
    }
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
        db->mem_->Put(seq, key, value);
      } else {
        db->mem_->Delete(seq, key);
      }
      if (seq > max_seq) {
        max_seq = seq;
      }
      applied_end = reader.last_good_end();
    }
    ::close(fd);
  }

  // The sequence continues after everything durable: SST records (via
  // the MANIFEST) and replayed log records.
  for (const TableRef& r : db->current_->l0) {
    max_seq = std::max(max_seq, r.meta.largest_seq);
  }
  for (const auto& level : db->current_->levels) {
    for (const TableRef& r : level) {
      max_seq = std::max(max_seq, r.meta.largest_seq);
    }
  }
  db->next_seq_ = max_seq + 1;
  if (status != nullptr) {
    *status = Status::Ok();
  }

  // Attach the appender to the CURRENT (newest) log at its REAL end:
  // the writer's in-block position decides where records fragment, so a
  // stale offset would corrupt the log at real block boundaries (found
  // by the reopen tests).
  const int fd = ::open(db->LogPath(db->log_number_).c_str(),
                        O_RDWR | O_CREAT | O_APPEND, 0644);
  if (fd < 0) {
    return fail("cannot open log " + DB::LogFileName(db->log_number_) +
                ": " + std::string(std::strerror(errno)));
  }
  db->log_fd_ = fd;
  const uint64_t log_end = static_cast<uint64_t>(::lseek(fd, 0, SEEK_END));
  db->log_writer_ = std::make_unique<log::Writer>(fd, log_end);

  // ---- 6. start the background flush/compaction thread (last: no
  //        failing step may run after the thread exists) ----
  DB* raw = db.get();
  db->bg_ = std::thread([raw]() { raw->BackgroundLoop(); });
  return db;
}

DB::~DB() {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    exit_ = true;
  }
  signal_.notify_all();
  if (bg_.joinable()) {
    bg_.join();
  }
  if (log_fd_ >= 0) {
    ::close(log_fd_);
  }
}

// ---- MANIFEST ----

Status DB::WriteManifest() {
  // Mutex held by the caller. Snapshot = every TableRef in the current
  // version, ascending file number (the parser requires that), plus the
  // current log generation.
  std::vector<const TableRef*> refs;
  for (const TableRef& r : current_->l0) {
    refs.push_back(&r);
  }
  for (const auto& level : current_->levels) {
    for (const TableRef& r : level) {
      refs.push_back(&r);
    }
  }
  std::sort(refs.begin(), refs.end(), [](const TableRef* a, const TableRef* b) {
    return a->meta.file_number < b->meta.file_number;
  });

  const std::string tmp = dir_ + "/" + kManifestTmpFileName;
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return Status::IOError("cannot create " + tmp + ": " +
                           std::string(std::strerror(errno)));
  }
  Status result = Status::Ok();
  {
    log::Writer writer(fd, 0);
    for (const TableRef* r : refs) {
      const sst::FileMeta& m = r->meta;
      std::string p;
      p.push_back(static_cast<char>(kManifestTagSstFile));
      PutFixed64(&p, m.file_number);
      PutFixed32(&p, m.level);
      PutFixed64(&p, m.smallest_seq);
      PutFixed64(&p, m.largest_seq);
      PutFixed64(&p, m.entry_count);
      PutFixed64(&p, m.file_size);
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

// ---- write path ----

Status DB::WriteEntry(uint8_t type, std::string_view key,
                      std::string_view value) {
  // Mutex held. WAL first, MemTable second: if we crash in between, the
  // record is simply replayed on recovery. The reverse order could lose
  // an acknowledged write — the cardinal sin of a persistent store.
  std::string payload;
  PutFixed64(&payload, next_seq_);
  payload.push_back(static_cast<char>(type));
  PutFixed32(&payload, static_cast<uint32_t>(key.size()));
  payload.append(key);
  if (type == kTypeValue) {
    PutFixed32(&payload, static_cast<uint32_t>(value.size()));
    payload.append(value);
  }
  Status s = log_writer_->AddRecord(payload);
  if (!s.ok()) {
    return s;
  }
  unsynced_bytes_ += payload.size() + log::kHeaderSize;
  s = MaybeSync();
  if (!s.ok()) {
    return s;
  }
  if (type == kTypeValue) {
    mem_->Put(next_seq_, key, value);
  } else {
    mem_->Delete(next_seq_, key);
  }
  ++next_seq_;
  return Status::Ok();
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

Status DB::RotateForFlush() {
  // Mutex held. Freeze the memtable, switch to a fresh log generation.
  // The new log file is created and fsynced HERE so that the background
  // flush can safely publish a MANIFEST naming it (see the flush step).
  const uint64_t new_log = next_file_number_++;
  const int fd = ::open(LogPath(new_log).c_str(),
                        O_RDWR | O_CREAT | O_TRUNC | O_APPEND, 0644);
  if (fd < 0) {
    next_file_number_--;
    return Status::IOError("cannot create " + LogFileName(new_log) + ": " +
                           std::string(std::strerror(errno)));
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    next_file_number_--;
    return Status::IOError("fsync " + LogFileName(new_log) + " failed: " +
                           std::string(std::strerror(saved)));
  }
  imm_log_number_ = log_number_;      // retired after the flush installs
  imm_sst_number_ = next_file_number_++;  // pre-allocated output number
  imm_ = mem_;
  mem_ = std::make_shared<MemTable>();
  const int old_fd = log_fd_;
  log_fd_ = fd;
  log_number_ = new_log;
  log_writer_ = std::make_unique<log::Writer>(fd, 0);
  unsynced_bytes_ = 0;
  if (old_fd >= 0) {
    ::close(old_fd);  // file stays on disk until the flush publishes
  }
  return Status::Ok();
}

Status DB::Put(std::string_view key, std::string_view value) {
  std::unique_lock<std::mutex> lk(mutex_);
  // At most one immutable memtable: wait for the background flush to
  // drain before rotating again (leveldb's MakeRoomForWrite shape).
  while (imm_ != nullptr) {
    signal_.wait(lk);
  }
  if (mem_->ApproximateSize() >= options_.write_buffer_size) {
    const Status s = RotateForFlush();
    if (!s.ok()) {
      return s;
    }
    signal_.notify_one();
  }
  return WriteEntry(kTypeValue, key, value);
}

Status DB::Delete(std::string_view key) {
  std::unique_lock<std::mutex> lk(mutex_);
  while (imm_ != nullptr) {
    signal_.wait(lk);
  }
  if (mem_->ApproximateSize() >= options_.write_buffer_size) {
    const Status s = RotateForFlush();
    if (!s.ok()) {
      return s;
    }
    signal_.notify_one();
  }
  return WriteEntry(kTypeDeletion, key, std::string_view());
}

// ---- read path ----

Status DB::Get(std::string_view key, std::string* value) const {
  // Snapshot under the mutex, then read lock-free: the shared_ptrs keep
  // the memtables and every table of the version alive even if a
  // concurrent flush/compaction publishes a new Version right away.
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<Version> v;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    mem = mem_;
    imm = imm_;
    v = current_;
  }

  switch (mem->Get(key, value)) {
    case MemTable::Lookup::kFound:
      return Status::Ok();
    case MemTable::Lookup::kDeleted:
      return Status::NotFound("deleted: " + std::string(key));
    case MemTable::Lookup::kMissing:
      break;
  }
  if (imm != nullptr) {
    switch (imm->Get(key, value)) {
      case MemTable::Lookup::kFound:
        return Status::Ok();
      case MemTable::Lookup::kDeleted:
        return Status::NotFound("deleted: " + std::string(key));
      case MemTable::Lookup::kMissing:
        break;
    }
  }

  // L0, newest file first (l0 is sorted by file number DESCENDING, so a
  // plain forward walk visits the newest first). The first file that
  // CONTAINS the key decides: its newest version may be a tombstone, and
  // an older file's value must not resurrect through it.
  for (const TableRef& r : v->l0) {
    Status s = Status::Ok();
    switch (r.table->Get(key, value, &s)) {
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

  // L1..Ln: files are key-sorted and disjoint, so at most one file per
  // level can contain the key.
  for (const auto& level : v->levels) {
    auto it = std::upper_bound(
        level.begin(), level.end(), key,
        [](std::string_view k, const TableRef& r) {
          return k < std::string_view(r.table->smallest_user_key());
        });
    if (it == level.begin()) {
      continue;  // key is left of everything in this level
    }
    --it;  // last file whose smallest key is <= key
    if (key > it->table->largest_user_key()) {
      continue;  // falls into the gap after that file
    }
    Status s = Status::Ok();
    switch (it->table->Get(key, value, &s)) {
      case MemTable::Lookup::kFound:
        return Status::Ok();
      case MemTable::Lookup::kDeleted:
        return Status::NotFound("deleted: " + std::string(key));
      case MemTable::Lookup::kMissing:
        if (!s.ok()) {
          return s;
        }
        break;
    }
  }
  return Status::NotFound("missing: " + std::string(key));
}

Status DB::Scan(std::string_view begin, std::string_view end,
                const std::function<void(std::string_view, std::string_view)>&
                    fn) const {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<Version> v;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    mem = mem_;
    imm = imm_;
    v = current_;
  }

  // Merge everything that can hold keys in [begin, end): both
  // memtables, all of L0 (overlapping), and the overlapping L1..Ln
  // files. The global internal-key order resolves versions: for one
  // user key, the newest (largest seq) sorts first no matter which file
  // or level it lives in.
  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem->NewIterator());
  if (imm != nullptr) {
    children.push_back(imm->NewIterator());
  }
  for (const TableRef& r : v->l0) {
    children.push_back(r.table->NewIterator());
  }
  for (const auto& level : v->levels) {
    for (const TableRef& r : level) {
      if (r.table->largest_user_key() < begin) {
        continue;  // entirely left of the range
      }
      if (r.table->smallest_user_key() >= end) {
        break;  // key-sorted: the rest is right of the range
      }
      children.push_back(r.table->NewIterator());
    }
  }

  MergingIterator iter(std::move(children));
  std::string target;
  target.append(begin);
  PutFixed64(&target, ~static_cast<uint64_t>(0));  // newest-version seek
  iter.Seek(target);

  std::string current_user;
  bool has_user = false;
  while (iter.Valid()) {
    const std::string_view user = ExtractUserKey(iter.key());
    if (user >= end) {
      break;
    }
    // First occurrence of a user key in merge order = newest version;
    // every later occurrence is an older version to skip. Tombstones
    // suppress the key entirely (older versions stay skipped).
    if (!has_user || user != current_user) {
      has_user = true;
      current_user.assign(user);
      if ((ExtractTag(iter.key()) & 0xff) == kTypeValue) {
        fn(user, iter.value());
      }
    }
    iter.Next();
  }
  return Status::Ok();
}

// ---- background thread: flush + compaction ----

void DB::BackgroundLoop() {
  std::unique_lock<std::mutex> lk(mutex_);
  for (;;) {
    signal_.wait(lk, [&] {
      return exit_ || imm_ != nullptr || CompactionNeeded();
    });
    if (exit_) {
      break;
    }
    if (imm_ != nullptr) {
      const std::shared_ptr<MemTable> imm = imm_;
      const uint64_t retired = imm_log_number_;
      const uint64_t sst_number = imm_sst_number_;
      lk.unlock();
      const Status s = FlushImmMemTable(imm, retired, sst_number);
      lk.lock();
      if (s.ok()) {
        imm_ = nullptr;  // drained
      } else {
        // Keep imm_ and retry: its records may exist only in the
        // retired log, which is deleted only on a successful install —
        // dropping it here could lose data. Back off briefly so a
        // persistent failure doesn't spin the CPU.
        last_error_ = s;
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        lk.lock();
      }
      signal_.notify_all();
      continue;
    }
    if (CompactionNeeded()) {
      std::vector<TableRef> inputs_a, inputs_b;
      size_t src = 0, out = 0;
      PickCompaction(&inputs_a, &inputs_b, &src, &out);
      lk.unlock();
      const Status s = RunCompaction(std::move(inputs_a), std::move(inputs_b),
                                     src, out);
      lk.lock();
      if (!s.ok()) {
        // Compaction is best-effort for correctness (data is safe, just
        // less organized): stop trying rather than spinning.
        last_error_ = s;
        compaction_disabled_ = true;
      }
      // wait_for_background_work() sleeps on "no more compaction due";
      // it must be woken to re-check that predicate after each step.
      signal_.notify_all();
    }
  }
}

Status DB::FlushImmMemTable(const std::shared_ptr<MemTable>& imm,
                            uint64_t retired_log, uint64_t sst_number) {
  // No lock: imm_ is immutable and nothing else touches it. Writes go
  // to the new log/memtable concurrently — that is the point.
  sst::Builder builder;
  sst::FileMeta meta;
  meta.file_number = sst_number;
  meta.level = 0;
  Status s = Status::Ok();
  imm->ForEach([&s, &builder](uint64_t seq, uint8_t type, std::string_view key,
                              std::string_view value) {
    if (s.ok()) {
      s = builder.Add(key, seq, type, value);
    }
  });
  if (s.ok()) {
    s = builder.Finish(SstPath(sst_number), &meta);
  }
  std::shared_ptr<sst::Table> table;
  if (s.ok()) {
    s = Status::Ok();
    table = sst::Table::Open(sst_number, SstPath(sst_number), &s);
    if (table == nullptr) {
      s = Status::Corruption("flush self-check failed for " +
                             SstFileName(sst_number));
    }
  }
  if (!s.ok()) {
    ::unlink(SstPath(sst_number).c_str());
    return s;
  }

  // Install under the mutex. Order is the crash story: MANIFEST names
  // the new SST and the CURRENT (post-rotation) log; only after it is
  // durable may the retired log be deleted.
  std::shared_ptr<Version> newv;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    newv = std::make_shared<Version>(*current_);
    newv->l0.insert(newv->l0.begin(), TableRef{std::move(table), meta});
    current_ = newv;
    s = WriteManifest();
  }
  if (!s.ok()) {
    return s;  // in-memory install is harmless; on-disk truth unchanged
  }
  ::unlink(LogPath(retired_log).c_str());
  return Status::Ok();
}

size_t DB::LevelSize(size_t level_index) const {
  uint64_t sum = 0;
  for (const TableRef& r : current_->levels[level_index]) {
    sum += r.meta.file_size;
  }
  return sum;
}

bool DB::CompactionNeeded() const {
  // Mutex held.
  if (current_ == nullptr || compaction_disabled_) {
    return false;
  }
  if (current_->l0.size() >= options_.l0_compaction_trigger) {
    return true;
  }
  for (size_t i = 0; i + 1 < current_->levels.size(); ++i) {
    uint64_t limit = options_.level_base_size;
    for (size_t k = 0; k < i; ++k) {
      limit *= 10;  // L(n+1) is 10x L(n)
    }
    if (LevelSize(i) > limit) {
      return true;
    }
  }
  return false;
}

void DB::PickCompaction(std::vector<TableRef>* inputs_a,
                        std::vector<TableRef>* inputs_b,
                        size_t* source_level_index,
                        size_t* output_level_index) const {
  // Mutex held. inputs_a: from the source level. inputs_b: everything in
  // the target level that overlaps the key range — chosen by the same
  // range-expansion fixpoint leveldb uses (adding a file can extend the
  // range, which can pull in more files), guaranteeing the outputs stay
  // disjoint from every surviving target-level file.
  if (current_->l0.size() >= options_.l0_compaction_trigger) {
    *inputs_a = current_->l0;  // size-tiered: all of L0
    *source_level_index = kL0Source;
    *output_level_index = 0;   // outputs land in L1
  } else {
    for (size_t i = 0; i + 1 < current_->levels.size(); ++i) {
      uint64_t limit = options_.level_base_size;
      for (size_t k = 0; k < i; ++k) {
        limit *= 10;
      }
      if (LevelSize(i) > limit) {
        *inputs_a = {current_->levels[i].front()};  // one victim
        *source_level_index = i;
        *output_level_index = i + 1;
        break;
      }
    }
  }

  std::string lo, hi;
  bool has_range = false;
  for (const TableRef& r : *inputs_a) {
    if (!has_range) {
      lo = r.table->smallest_user_key();
      hi = r.table->largest_user_key();
      has_range = true;
    } else {
      lo = std::min(lo, r.table->smallest_user_key());
      hi = std::max(hi, r.table->largest_user_key());
    }
  }
  const auto& target = current_->levels[*output_level_index];
  std::set<uint64_t> picked;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const TableRef& r : target) {
      if (picked.count(r.meta.file_number) != 0) {
        continue;
      }
      const bool overlaps = r.table->largest_user_key() >= lo &&
                            r.table->smallest_user_key() <= hi;
      if (overlaps) {
        picked.insert(r.meta.file_number);
        inputs_b->push_back(r);
        lo = std::min(lo, r.table->smallest_user_key());
        hi = std::max(hi, r.table->largest_user_key());
        changed = true;
      }
    }
  }
}

Status DB::RunCompaction(std::vector<TableRef> inputs_a,
                         std::vector<TableRef> inputs_b,
                         size_t source_level_index,
                         size_t output_level_index) {
  // Merge all inputs. Per user key, the merged internal-key order puts
  // the newest version first; we keep exactly that one and drop older
  // versions (no snapshot reads yet). Tombstones survive unless the
  // output is the bottom level — elsewhere they must keep shadowing the
  // levels below.
  std::vector<std::unique_ptr<Iterator>> children;
  for (const TableRef& r : inputs_a) {
    children.push_back(r.table->NewIterator());
  }
  for (const TableRef& r : inputs_b) {
    children.push_back(r.table->NewIterator());
  }
  MergingIterator iter(std::move(children));
  iter.SeekToFirst();

  const bool bottom = output_level_index + 1 == kMaxLevels - 1;
  std::vector<TableRef> outputs;
  sst::Builder builder;
  bool builder_used = false;
  std::string current_user;
  bool has_user = false;
  Status s = Status::Ok();

  const auto seal = [&]() {
    if (!builder_used || !s.ok()) {
      return;
    }
    uint64_t number = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      number = next_file_number_++;
    }
    sst::FileMeta meta;
    meta.file_number = number;
    meta.level = static_cast<uint32_t>(output_level_index) + 1;
    s = builder.Finish(SstPath(number), &meta);
    if (!s.ok()) {
      return;
    }
    std::shared_ptr<sst::Table> table =
        sst::Table::Open(number, SstPath(number), &s);
    if (table == nullptr) {
      s = Status::Corruption("compaction self-check failed for " +
                             SstFileName(number));
      return;
    }
    outputs.push_back(TableRef{std::move(table), meta});
    builder = sst::Builder{};
    builder_used = false;
  };

  while (iter.Valid() && s.ok()) {
    const std::string_view ik = iter.key();
    const std::string_view user = ExtractUserKey(ik);
    if (!has_user || user != current_user) {
      has_user = true;
      current_user.assign(user);
      const uint64_t tag = ExtractTag(ik);
      const uint8_t type = static_cast<uint8_t>(tag & 0xff);
      const bool drop = type == kTypeDeletion && bottom;
      if (!drop) {
        if (builder_used &&
            builder.ApproximateFileSize() >= options_.max_sst_size) {
          seal();  // split between user keys, never inside one
          if (!s.ok()) {
            break;
          }
        }
        s = builder.Add(user, tag >> 8, type, iter.value());
        builder_used = true;
      }
    }
    iter.Next();
  }
  if (s.ok()) {
    seal();
  }
  if (!s.ok()) {
    for (const TableRef& r : outputs) {
      ::unlink(SstPath(r.meta.file_number).c_str());
    }
    return s;
  }

  // Install. MANIFEST first (still names the inputs), unlink after —
  // a crash in between just leaves orphans the next Open removes.
  std::lock_guard<std::mutex> lk(mutex_);
  auto newv = std::make_shared<Version>(*current_);
  const auto remove_numbers = [](std::vector<TableRef>* level,
                                 const std::vector<TableRef>& removed) {
    std::set<uint64_t> numbers;
    for (const TableRef& r : removed) {
      numbers.insert(r.meta.file_number);
    }
    level->erase(std::remove_if(level->begin(), level->end(),
                                [&numbers](const TableRef& r) {
                                  return numbers.count(r.meta.file_number) != 0;
                                }),
                 level->end());
  };
  if (source_level_index == kL0Source) {
    newv->l0.clear();  // inputs_a was all of L0
  } else {
    remove_numbers(&newv->levels[source_level_index], inputs_a);
  }
  remove_numbers(&newv->levels[output_level_index], inputs_b);
  for (TableRef& r : outputs) {
    newv->levels[output_level_index].push_back(std::move(r));
  }
  std::sort(newv->levels[output_level_index].begin(),
            newv->levels[output_level_index].end(),
            [](const TableRef& a, const TableRef& b) {
              return a.table->smallest_user_key() <
                     b.table->smallest_user_key();
            });
  current_ = newv;
  s = WriteManifest();
  if (!s.ok()) {
    return s;  // inputs still published; outputs are future orphans
  }
  for (const TableRef& r : inputs_a) {
    ::unlink(SstPath(r.meta.file_number).c_str());
  }
  for (const TableRef& r : inputs_b) {
    ::unlink(SstPath(r.meta.file_number).c_str());
  }
  return Status::Ok();
}

// ---- observability ----

size_t DB::level_file_count(size_t level) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (current_ == nullptr) {
    return 0;
  }
  if (level == 0) {
    return current_->l0.size();
  }
  if (level - 1 < current_->levels.size()) {
    return current_->levels[level - 1].size();
  }
  return 0;
}

size_t DB::total_file_count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (current_ == nullptr) {
    return 0;
  }
  size_t n = current_->l0.size();
  for (const auto& level : current_->levels) {
    n += level.size();
  }
  return n;
}

void DB::wait_for_background_work() {
  std::unique_lock<std::mutex> lk(mutex_);
  signal_.wait(lk, [&] {
    return exit_ || (imm_ == nullptr && !CompactionNeeded());
  });
}

}  // namespace bedrockkv
