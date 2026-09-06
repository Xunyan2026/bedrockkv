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
constexpr uint8_t kManifestTagVlogFile = 3;

// The MANIFEST is a full snapshot of the SST list (with levels), the
// live vLog generations, and the current log generation, rewritten
// atomically on every change. Using our own log format is deliberate:
// records are CRC-protected and the reader machinery already exists.
struct ManifestState {
  std::vector<sst::FileMeta> files;
  std::vector<uint64_t> vlogs;  // live vLog generations, ascending
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
  uint64_t last_vlog_number = 0;
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
    } else if (tag == kManifestTagVlogFile) {
      // One record per live vLog generation, ascending. Multiple
      // generations are live while a GC rewrite is in progress (or was
      // interrupted) — pointers in the LSM may still select the old one.
      if (record.size() != 9) {
        ok = false;
        break;
      }
      const uint64_t n = GetFixed64(record.data() + 1);
      if (n <= last_vlog_number) {
        ok = false;
        break;
      }
      last_vlog_number = n;
      state->vlogs.push_back(n);
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

std::string DB::VlogFileName(uint64_t number) {
  return VLog::FileName(number);
}

DB::DB()
    : vlog_cache_(16, 32u << 20,
                  [](const std::string& v) { return v.size(); }) {}

void DB::RemoveOrphanFiles(const std::vector<sst::FileMeta>& live,
                           uint64_t log_floor,
                           const std::set<uint64_t>& live_vlogs) const {
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
    } else if (ParseNumberedFile(name, ".vlog", &number) &&
               !live_vlogs.count(number)) {
      // A vLog generation the MANIFEST does not name: either it was
      // never published (crash right after GC rotation) or GC finished
      // and dropped it before the unlink. Unnamed vLogs are unreachable
      // by construction — the MANIFEST is the only authority.
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

  // ---- 2b. open the value logs (WiscKey) ----
  // The MANIFEST names every live vLog generation: while a GC rewrite is
  // in flight (or was interrupted by a crash) pointers in the LSM may
  // still select the OLD generation, so it stays published until every
  // entry has been rewritten. The newest generation is the append
  // target. A database with no named vLogs either is fresh or predates
  // value separation — generation 1 starts the separated era.
  std::set<uint64_t> live_vlogs(ms.vlogs.begin(), ms.vlogs.end());
  // Refuse, don't degrade: with separation off, the vLogs would neither be
  // opened nor re-published, and the orphan sweep in step 4 would DELETE
  // them — silent destruction of live values. The check must scan the
  // DISK, not just the MANIFEST: a memtable pending flush means the
  // MANIFEST can predate the newest vLog generation (it is republished
  // only at flush/GC), so a freshly separated DB can have .vlog files on
  // disk with an empty ms.vlogs.
  bool vlog_on_disk = false;
  {
    DIR* d = ::opendir(dir.c_str());
    if (d != nullptr) {
      while (const dirent* e = ::readdir(d)) {
        uint64_t n = 0;
        if (ParseNumberedFile(e->d_name, ".vlog", &n)) {
          vlog_on_disk = true;
          break;
        }
      }
      ::closedir(d);
    }
  }
  if (vlog_on_disk && !options.enable_value_separation) {
    return fail("directory contains value logs but enable_value_separation "
                "is off; reopen with the switch on to avoid orphaning them");
  }
  if (!live_vlogs.empty() && !options.enable_value_separation) {
    return fail("MANIFEST names value logs but enable_value_separation is "
                "off; reopen with the switch on to avoid orphaning them");
  }
  if (options.enable_value_separation) {
    if (live_vlogs.empty()) {
      live_vlogs.insert(1);
    }
    for (const uint64_t number : live_vlogs) {
      Status s = Status::Ok();
      auto vl = VLog::Open(dir, number, &s);
      if (vl == nullptr) {
        return fail("cannot open vlog " + VLog::FileName(number) + ": " +
                    s.message());
      }
      max_seen_number = std::max(max_seen_number, number);
      db->vlogs_.emplace(number, std::move(vl));
    }
    db->vlog_current_ = db->vlogs_.rbegin()->second;
    db->vsep_enabled_ = true;
    db->vlog_gc_size_ = std::max<size_t>(options.vlog_gc_size, 64u << 10);
    // A 21-byte inline value starting with 0xFF would be misread as a
    // pointer, so the threshold can never be clamped into that range.
    db->vsep_threshold_ =
        std::max<size_t>(options.value_separation_threshold, 64);
  }

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
  db->RemoveOrphanFiles(ms.files, replay_floor, live_vlogs);
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
  //
  // The ring is opened BEFORE the fd: its outcome decides the O_APPEND
  // flag. Sync WAL keeps O_APPEND (after a tail truncation, writes must
  // continue at the NEW end of file); async WAL needs a plain O_RDWR fd
  // because O_APPEND overrides the SQE's explicit offset on real kernels
  // (writes land at EOF in completion order — see ring.h).
  if (db->options_.enable_io_uring) {
    Status rs = Status::Ok();
    db->ring_ = Ring::Open(64, &rs);
    if (db->ring_ != nullptr) {
      db->io_uring_active_ = true;
    } else {
      db->io_uring_reason_ = rs.message();
    }
  } else {
    db->io_uring_reason_ = "not requested (Options::enable_io_uring)";
  }
  const int wal_flags =
      db->io_uring_active_ ? (O_RDWR | O_CREAT) : (O_RDWR | O_CREAT | O_APPEND);
  const int fd = ::open(db->LogPath(db->log_number_).c_str(), wal_flags, 0644);
  if (fd < 0) {
    return fail("cannot open log " + DB::LogFileName(db->log_number_) +
                ": " + std::string(std::strerror(errno)));
  }
  db->log_fd_ = fd;
  const uint64_t log_end = static_cast<uint64_t>(::lseek(fd, 0, SEEK_END));
  db->log_writer_ = std::make_unique<log::Writer>(fd, log_end);
  if (db->io_uring_active_) {
    db->wal_size_ = log_end;
  }
  // A fresh open may have CREATED the log file (O_CREAT): persist its
  // directory entry before anything is acknowledged into it.
  if (const Status ds = fs::SyncDir(db->dir_); !ds.ok()) {
    return fail("cannot sync db directory: " + ds.message());
  }

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
  if (io_uring_active_) {
    // Last writer standing: make sure in-flight WAL bytes reached the
    // page cache before the fd (and the ring) disappear. Best-effort —
    // a failure here has nowhere to go at shutdown.
    static_cast<void>(DrainWalRing());
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
      // Every live vLog generation, ascending (std::map iterates sorted).
      // No records when separation is off — the parser accepts both.
      for (const auto& [number, vl] : vlogs_) {
        (void)vl;
        std::string p;
        p.push_back(static_cast<char>(kManifestTagVlogFile));
        PutFixed64(&p, number);
        result = writer.AddRecord(p);
        if (!result.ok()) {
          break;
        }
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
                      std::string_view value, bool count_user_bytes) {
  // Mutex held. WAL first, MemTable second: if we crash in between, the
  // record is simply replayed on recovery. The reverse order could lose
  // an acknowledged write — the cardinal sin of a persistent store.
  //
  // With value separation the "value" that flows onward is a 21-byte
  // pointer: the real bytes go to the vLog FIRST (append order = fsync
  // order below), so a durable WAL record can never reference vLog bytes
  // that were not durable first. Orphaned vLog bytes (key never reached
  // the WAL) are harmless — GC reclaims them.
  std::string_view stored = value;
  std::string pointer;
  if (type == kTypeValue && vsep_enabled_ && value.size() >= vsep_threshold_) {
    uint64_t offset = 0;
    uint64_t entry_bytes = 0;
    const Status s = vlog_current_->Append(key, value, &offset, &entry_bytes);
    if (!s.ok()) {
      return s;
    }
    pointer = EncodeValuePointer(
        ValuePointer{vlog_current_->file_number(), offset,
                     static_cast<uint32_t>(value.size())});
    stored = pointer;
    // The vLog shares the WAL's sync budget: one threshold drives both
    // fsyncs, and the WAL's schedule covers the vLog's.
    unsynced_bytes_ += entry_bytes;
    stats_vlog_bytes_ += entry_bytes;
    // Value separation shrinks memtable entries to ~40 bytes, so user
    // rotations (the usual wakeup source) become rare and the vLog can
    // cross its GC trigger with the background thread still asleep.
    // Notify as soon as the trigger is crossed — not only on rotation.
    if (vlog_current_->file_size() >=
        (vlog_gc_size_ > vlog_gc_floor_ ? vlog_gc_size_ : vlog_gc_floor_)) {
      signal_.notify_one();
    }
  }
  std::string payload;
  PutFixed64(&payload, next_seq_);
  payload.push_back(static_cast<char>(type));
  PutFixed32(&payload, static_cast<uint32_t>(key.size()));
  payload.append(key);
  if (type == kTypeValue) {
    PutFixed32(&payload, static_cast<uint32_t>(stored.size()));
    payload.append(stored);
  }
  Status s = Status::Ok();
  if (io_uring_active_) {
    // Async path: encode at the log's absolute end offset (no O_APPEND —
    // pwrite SQEs carry explicit offsets, so disjoint writes can complete
    // in any order without corrupting the record stream) and submit one
    // SQE. The buffer lives in wal_pending_ until its completion is
    // reaped (before every fsync / rotation / close). One enter() per
    // record here mirrors the sync path's one write() per record — the
    // batching wins come from the fsync pair in MaybeSync, not from
    // deferring submissions (a queued-but-unsubmitted record would break
    // the WAL's process-crash contract).
    std::string buf;
    s = log::Writer::EncodeRecord(payload, wal_size_, &buf);
    if (!s.ok()) {
      return s;
    }
    ++wal_token_;
    wal_pending_.emplace(wal_token_, std::move(buf));
    if (!ring_->QueueWrite(log_fd_, wal_pending_[wal_token_].data(),
                           wal_pending_[wal_token_].size(), wal_size_,
                           wal_token_)) {
      wal_pending_.erase(wal_token_);
      return Status::IOError("io_uring submission queue full");
    }
    unsynced_bytes_ += wal_pending_[wal_token_].size();
    stats_wal_bytes_ += wal_pending_[wal_token_].size();
    wal_size_ += wal_pending_[wal_token_].size();
    // Submit immediately (a queued-but-unsubmitted record breaks the
    // WAL's process-crash contract) and opportunistically reap whatever
    // already completed — no extra syscall, and wal_pending_'s buffers
    // stay bounded even under kSyncNever, which rarely drains.
    if (!ring_->Flush(false)) {
      return Status::IOError("io_uring_enter submit failed: " +
                             std::string(std::strerror(errno)));
    }
    ring_->Reap(
        [this](uint64_t token, int res) {
          wal_pending_.erase(token);
          if (res < 0 && wal_ring_error_.ok()) {
            wal_ring_error_ = Status::IOError("async WAL write failed: " +
                                              std::string(std::strerror(-res)));
          }
        },
        /*wait=*/false);
  } else {
    // Account the REAL encoded size (one header per fragment + block
    // tail padding), not payload+header — payload.size()+kHeaderSize
    // undercounts multi-fragment records and diverges from the async
    // path's accounting (found by review).
    const uint64_t before = log_writer_->file_offset();
    s = log_writer_->AddRecord(payload);
    if (!s.ok()) {
      return s;
    }
    const uint64_t written = log_writer_->file_offset() - before;
    unsynced_bytes_ += written;
    stats_wal_bytes_ += written;
  }
  if (count_user_bytes) {
    stats_user_bytes_ += key.size() + value.size();
  }
  s = MaybeSync();
  if (!s.ok()) {
    return s;
  }
  if (type == kTypeValue) {
    mem_->Put(next_seq_, key, stored);
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
  if (io_uring_active_) {
    // An earlier reap may have found a failed write CQE after its Put
    // had already returned — surface it at the next durability point
    // (the caller of Put/MaybeSync is the same writer thread).
    if (!wal_ring_error_.ok()) {
      const Status s = wal_ring_error_;
      wal_ring_error_ = Status::Ok();
      return s;
    }
    if (need) {
      // Order matters: every WAL byte must be at least in the page cache
      // before any fsync is allowed to claim it durable.
      const Status d = DrainWalRing();
      if (!d.ok()) {
        return d;
      }
      // The headline io_uring win: vLog + WAL fsyncs as one parallel
      // SQE pair — one round trip where the sync path pays two.
      const Status p = SyncViaRing();
      if (!p.ok()) {
        return p;
      }
      // Reset ONLY after a sync actually ran: zeroing it unconditionally
      // would re-zero the accumulator on every Put and kSyncPeriodic
      // could never cross its threshold again (found by review).
      unsynced_bytes_ = 0;
    }
    return Status::Ok();
  }
  if (need) {
    // vLog BEFORE WAL (see WriteEntry): the WAL record's durability must
    // imply the durability of the vLog bytes it points at.
    if (vlog_current_ != nullptr) {
      const Status s = vlog_current_->Sync();
      if (!s.ok()) {
        return s;
      }
    }
    if (::fsync(log_fd_) != 0) {
      return Status::IOError(std::string("log fsync failed: ") +
                             std::strerror(errno));
    }
    unsynced_bytes_ = 0;
  }
  return Status::Ok();
}

Status DB::DrainWalRing() {
  // Mutex held. Submit whatever is queued and sleep until EVERY
  // outstanding completion (writes and fsyncs alike) has arrived — the
  // points that call this (pre-fsync, pre-rotation, shutdown) are exactly
  // the ones after which WAL bytes must be at least in the page cache.
  // queued_>0 counts too: a previously failed enter() leaves published
  // SQEs unsubmitted, and ignoring them could close the fd they target.
  if (ring_->outstanding() == 0 && ring_->pending() == 0) {
    return Status::Ok();
  }
  if (!ring_->Flush(true)) {
    return Status::IOError("io_uring_enter wait failed: " +
                           std::string(std::strerror(errno)));
  }
  Status err = Status::Ok();
  ring_->Reap(
      [&](uint64_t token, int res) {
        wal_pending_.erase(token);
        if (res < 0 && err.ok()) {
          err = Status::IOError("async WAL write failed: " +
                                std::string(std::strerror(-res)));
        }
      },
      /*wait=*/false);  // Flush(true) already waited
  return err;
}

Status DB::ForceDurabilityLocked() {
  // Mutex held. A durability point the caller can rely on regardless of
  // the user's sync mode: everything written so far is on the device
  // when this returns Ok. Used before publishing a MANIFEST that
  // retires data (vlog GC) — without it, kSyncNever/kSyncPeriodic could
  // lose rewrite records while the retired generations are deleted.
  if (io_uring_active_) {
    const Status d = DrainWalRing();
    if (!d.ok()) {
      return d;
    }
    const Status p = SyncViaRing();  // fsyncs vLog + WAL in one pair
    if (p.ok()) {
      unsynced_bytes_ = 0;
    }
    return p;
  }
  if (vlog_current_ != nullptr) {
    const Status s = vlog_current_->Sync();
    if (!s.ok()) {
      return s;
    }
  }
  if (::fsync(log_fd_) != 0) {
    return Status::IOError(std::string("log fsync failed: ") +
                           std::strerror(errno));
  }
  unsynced_bytes_ = 0;
  return Status::Ok();
}

Status DB::SyncViaRing() {
  // Mutex held; DrainWalRing already ran, so the fsyncs below claim only
  // completed writes. Two independent files, one parallel SQE pair, one
  // wait — the sequential-sync-path's two round trips collapse into one.
  // Invariant note: the sequential path guarantees "durable WAL ⇒
  // durable vLog" by ORDER (vLog fsync completes first). The parallel
  // pair can persist the WAL while the vLog fsync is still in flight —
  // but only for writes whose Put had not yet returned, so no
  // acknowledged state can regress; vLog CRCs surface any torn tail.
  ++wal_token_;
  if (!ring_->QueueFsync(log_fd_, wal_token_)) {
    return Status::IOError("io_uring submission queue full");
  }
  if (vlog_current_ != nullptr) {
    ++wal_token_;
    if (!ring_->QueueFsync(vlog_current_->fd(), wal_token_)) {
      return Status::IOError("io_uring submission queue full");
    }
  }
  if (!ring_->Flush(true)) {
    return Status::IOError("io_uring_enter wait failed: " +
                           std::string(std::strerror(errno)));
  }
  Status err = Status::Ok();
  ring_->Reap(
      [&](uint64_t, int res) {
        if (res < 0 && err.ok()) {
          err = Status::IOError("async fsync failed: " +
                                std::string(std::strerror(-res)));
        }
      },
      /*wait=*/false);
  return err;
}

Status DB::RotateForFlush() {
  // Mutex held. Freeze the memtable, switch to a fresh log generation.
  // The new log file is created and fsynced HERE so that the background
  // flush can safely publish a MANIFEST naming it (see the flush step).
  if (io_uring_active_) {
    // Outstanding write SQEs reference the old fd, and the old log file
    // is unlinked as soon as its flush installs — drain first so no CQE
    // can ever outlive the file it wrote.
    const Status d = DrainWalRing();
    if (!d.ok()) {
      return d;
    }
  }
  const uint64_t new_log = next_file_number_++;
  // O_APPEND only for the sync path (see Open): async WAL writes carry
  // explicit offsets and need a plain O_RDWR fd.
  const int fd = ::open(LogPath(new_log).c_str(),
                        io_uring_active_
                            ? (O_RDWR | O_CREAT | O_TRUNC)
                            : (O_RDWR | O_CREAT | O_TRUNC | O_APPEND),
                        0644);
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
  // fsync(file) does not persist the file's directory entry: without
  // SyncDir, a power cut before the next MANIFEST publish could leave
  // this generation nonexistent while records written into it were
  // already acknowledged (found by review).
  if (const Status ds = fs::SyncDir(dir_); !ds.ok()) {
    ::close(fd);
    next_file_number_--;
    return ds;
  }
  imm_log_number_ = log_number_;      // retired after the flush installs
  imm_sst_number_ = next_file_number_++;  // pre-allocated output number
  imm_ = mem_;
  mem_ = std::make_shared<MemTable>();
  const int old_fd = log_fd_;
  log_fd_ = fd;
  log_number_ = new_log;
  log_writer_ = std::make_unique<log::Writer>(fd, 0);
  wal_size_ = 0;  // fresh generation: SQE offsets restart at 0
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

// ---- read path ----

// The shared read traversal. No locking: `Get` passes snapshot copies,
// the GC passes the live members while holding the mutex. `stored`
// receives the raw value-slot contents — an inline value, or (with
// separation) a 21-byte value pointer that only the DB layer resolves.
// A real I/O error from a table lookup is returned via *error (may be
// nullptr when the caller treats any miss the same); a plain miss is not.
MemTable::Lookup DB::LookupIn(const std::shared_ptr<MemTable>& mem,
                              const std::shared_ptr<MemTable>& imm,
                              const std::shared_ptr<Version>& v,
                              std::string_view key, std::string* stored,
                              Status* error, uint64_t max_seq) const {
  switch (mem->Get(key, stored, max_seq)) {
    case MemTable::Lookup::kFound:
      return MemTable::Lookup::kFound;
    case MemTable::Lookup::kDeleted:
      return MemTable::Lookup::kDeleted;
    case MemTable::Lookup::kMissing:
      break;
  }
  if (imm != nullptr) {
    switch (imm->Get(key, stored, max_seq)) {
      case MemTable::Lookup::kFound:
        return MemTable::Lookup::kFound;
      case MemTable::Lookup::kDeleted:
        return MemTable::Lookup::kDeleted;
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
    switch (r.table->Get(key, stored, max_seq, &s)) {
      case MemTable::Lookup::kFound:
        return MemTable::Lookup::kFound;
      case MemTable::Lookup::kDeleted:
        return MemTable::Lookup::kDeleted;
      case MemTable::Lookup::kMissing:
        if (!s.ok()) {
          if (error != nullptr) {
            *error = s;
          }
          return MemTable::Lookup::kMissing;
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
    switch (it->table->Get(key, stored, max_seq, &s)) {
      case MemTable::Lookup::kFound:
        return MemTable::Lookup::kFound;
      case MemTable::Lookup::kDeleted:
        return MemTable::Lookup::kDeleted;
      case MemTable::Lookup::kMissing:
        if (!s.ok()) {
          if (error != nullptr) {
            *error = s;
          }
          return MemTable::Lookup::kMissing;
        }
        break;
    }
  }
  return MemTable::Lookup::kMissing;
}

Status DB::Get(std::string_view key, std::string* value) const {
  return Get(key, value, nullptr);
}

Status DB::Get(std::string_view key, std::string* value,
               const Snapshot* snapshot) const {
  // Snapshot under the mutex, then read lock-free: the shared_ptrs keep
  // the memtables and every table of the version alive even if a
  // concurrent flush/compaction publishes a new Version right away.
  // The read point (seq) is captured in the same critical section — a
  // snapshot read must see exactly the writes up to ITS sequence, so the
  // version set and the sequence bound must come from one instant.
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<Version> v;
  uint64_t seq = MemTable::kMaxSeq;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    mem = mem_;
    imm = imm_;
    v = current_;
    if (snapshot != nullptr) {
      seq = snapshot->sequence();
    }
  }

  std::string stored;
  Status error = Status::Ok();
  switch (LookupIn(mem, imm, v, key, &stored, &error, seq)) {
    case MemTable::Lookup::kFound:
      *value = std::move(stored);
      return MaybeResolvePointer(value);
    case MemTable::Lookup::kDeleted:
      return Status::NotFound("deleted: " + std::string(key));
    case MemTable::Lookup::kMissing:
      // A real I/O error during table lookup must not be reported as a
      // mere missing key.
      if (!error.ok()) {
        return error;
      }
      break;
  }
  return Status::NotFound("missing: " + std::string(key));
}

Status DB::Scan(std::string_view begin, std::string_view end,
                const std::function<void(std::string_view, std::string_view)>&
                    fn) const {
  return Scan(begin, end, fn, nullptr);
}

Status DB::Scan(std::string_view begin, std::string_view end,
                const std::function<void(std::string_view, std::string_view)>&
                    fn,
                const Snapshot* snapshot) const {
  std::shared_ptr<MemTable> mem, imm;
  std::shared_ptr<Version> v;
  uint64_t seq = MemTable::kMaxSeq;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    mem = mem_;
    imm = imm_;
    v = current_;
    if (snapshot != nullptr) {
      seq = snapshot->sequence();
    }
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
  // Latest read (seq = kMaxSeq): the tag overflows to ~0 — the same
  // newest-version seek as before. Snapshot read: seek to
  // (seq << 8) | 0xFF so the start key's invisible newer versions are
  // pruned by the seek itself; later keys are pruned by the loop below.
  PutFixed64(&target, seq >= MemTable::kMaxSeq
                          ? ~static_cast<uint64_t>(0)
                          : (seq << 8) | 0xff);
  iter.Seek(target);

  std::string current_user;
  bool has_user = false;
  bool current_emitted = false;  // snapshot: newest visible version found?
  while (iter.Valid()) {
    const std::string_view user = ExtractUserKey(iter.key());
    if (user >= end) {
      break;
    }
    if (!has_user || user != current_user) {
      has_user = true;
      current_user.assign(user);
      current_emitted = false;
    }
    if (!current_emitted) {
      const uint64_t tag = ExtractTag(iter.key());
      // For a snapshot read, versions with seq > S are invisible: keep
      // walking to the oldest side until the first visible one. For a
      // latest read the very first occurrence always passes (no version
      // has seq > kMaxSeq), which reproduces the old first-wins rule.
      if ((tag >> 8) <= seq) {
        current_emitted = true;
        if ((tag & 0xff) == kTypeValue) {
          // Separated values arrive here as 21-byte pointers; resolve
          // them so fn sees real bytes, exactly like Get does.
          std::string resolved(iter.value());
          const Status s = MaybeResolvePointer(&resolved);
          if (!s.ok()) {
            return s;
          }
          fn(user, resolved);
        }
      }
    }
    iter.Next();
  }
  return Status::Ok();
}

// ---- MVCC snapshots ----

Snapshot* DB::GetSnapshot() {
  // The read point is the last assigned sequence: a write in flight has
  // already taken its seq under this same mutex, so the snapshot is
  // guaranteed to see every Put/Delete that returned before GetSnapshot
  // was called (and nothing newer).
  std::lock_guard<std::mutex> lk(mutex_);
  // Direct new, not make_unique: the constructor is private (DB is the
  // friend make_unique lacks).
  snapshots_.emplace_back(new Snapshot(next_seq_ - 1));
  return snapshots_.back().get();
}

void DB::ReleaseSnapshot(Snapshot* snapshot) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = snapshots_.begin(); it != snapshots_.end(); ++it) {
    if (it->get() == snapshot) {
      snapshots_.erase(it);
      return;
    }
  }
}

uint64_t DB::SmallestSnapshotSeq() const {
  // Mutex held. kMaxSeq with no live snapshots — the "read everything"
  // bound that makes compaction keep only the newest version.
  uint64_t smallest = MemTable::kMaxSeq;
  for (const auto& s : snapshots_) {
    smallest = std::min(smallest, s->sequence());
  }
  return smallest;
}

// ---- value separation: pointer resolution + GC ----

Status DB::MaybeResolvePointer(std::string* value) const {
  // Inline values (separation off, or below the threshold) pass through
  // untouched: anything that is not exactly the 21-byte 0xFF-tagged
  // pointer shape cannot be a pointer.
  if (value->size() != kValuePointerSize ||
      (*value)[0] != kValuePointerTag) {
    return Status::Ok();
  }
  ValuePointer p;
  if (!DecodeValuePointer(*value, &p)) {
    return Status::Corruption("undecodable value pointer");
  }
  std::string cached;
  if (vlog_cache_.Get(*value, &cached)) {
    *value = std::move(cached);
    return Status::Ok();
  }
  // Copy the generation's shared_ptr out under the mutex, then pread
  // lock-free — the same snapshot discipline as the Version read path.
  const std::shared_ptr<VLog> vl = VLogFor(p.vlog_number);
  if (vl == nullptr) {
    return Status::Corruption("value log " + VLog::FileName(p.vlog_number) +
                              " is gone (separation disabled after use?)");
  }
  std::string resolved;
  const Status s = vl->ReadValue(p.offset, p.value_size, &resolved);
  if (!s.ok()) {
    return s;
  }
  vlog_cache_.Put(*value, resolved);
  *value = std::move(resolved);
  return Status::Ok();
}

std::shared_ptr<VLog> DB::VLogFor(uint64_t number) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const auto it = vlogs_.find(number);
  return it != vlogs_.end() ? it->second : nullptr;
}

bool DB::VlogGcNeeded() const {
  // Mutex held (BackgroundLoop / wait_for_background_work). Triggers:
  //   * a leftover older generation exists (a previous pass was
  //     interrupted — it must be reclaimed, not leaked);
  //   * the current file crossed max(vlog_gc_size_, vlog_gc_floor_).
  // The floor is the anti-livelock device: a pass that finds the file
  // mostly live sets floor = 2x what it rewrote, so the next rewrite
  // only happens after enough NEW garbage accumulated. Without it, a
  // live data volume above the configured trigger would be rewritten
  // forever (each pass rewrites everything, file stays above trigger).
  if (!vsep_enabled_ || vlog_current_ == nullptr) {
    return false;
  }
  // A live snapshot may pin an OLD version of a key whose 21-byte
  // pointer still selects bytes in a retired generation. A GC pass
  // classifies that old version dead (its liveness check reads the
  // LATEST state) and unlinks the file — the snapshot's next read would
  // hit a gone generation. So GC is deferred entirely while any
  // snapshot exists; releasing the last one re-enables it. (Same shape
  // as compaction starvation: a long-lived snapshot delays reclamation,
  // correctness is never at risk.)
  if (!snapshots_.empty()) {
    return false;
  }
  if (vlogs_.size() > 1) {
    return true;
  }
  const uint64_t size = vlog_current_->file_size();
  const uint64_t trigger = vlog_gc_size_ > vlog_gc_floor_ ? vlog_gc_size_
                                                          : vlog_gc_floor_;
  return size >= trigger;
}

Status DB::RunVlogGC() {
  // Background thread, called WITHOUT the mutex (it releases and
  // reacquires it many times). User writers and readers run throughout;
  // every mutation below is under the mutex.
  uint64_t fresh_number = 0;
  {
    std::lock_guard<std::mutex> lk(mutex_);

    // Snapshot guard (start): a snapshot pins old pointers into the
    // generations this pass would retire. VlogGcNeeded already gates on
    // it, but a snapshot can be created between that check and here —
    // re-check under the same mutex that GetSnapshot holds.
    if (!snapshots_.empty()) {
      return Status::Ok();  // defer the whole pass
    }

    // 1. Rotate to a fresh generation FIRST: the scans below then read
    //    files nobody appends to, and all rewrites (GC + user) land in
    //    the new one. Publish the new generation immediately — from now
    //    on a crash must find it named, because new WAL records already
    //    reference it.
    fresh_number = next_file_number_++;
    Status s = Status::Ok();
    auto fresh = VLog::Open(dir_, fresh_number, &s);
    if (fresh == nullptr) {
      next_file_number_--;
      return s;
    }
    vlogs_.emplace(fresh_number, std::move(fresh));
    vlog_current_ = vlogs_[fresh_number];
    const Status ms = WriteManifest();
    if (!ms.ok()) {
      // Roll the rotation back: keep appending to the previous
      // generation. (Concurrent writers only touched memtables/WAL —
      // the vlog switch is invisible to them until their next write.)
      vlogs_.erase(fresh_number);
      vlog_current_ = std::prev(vlogs_.end())->second;
      next_file_number_--;
      return ms;
    }
  }

  // 2. Scan EVERY older generation (normally one; more if a previous
  //    GC pass was interrupted — they must not leak forever) and re-Put
  //    live entries through the normal write path (which re-separates
  //    into the new generation). The liveness check + re-write happen
  //    under ONE mutex hold, so a concurrent overwrite can never slip
  //    between "this entry is the key's current value" and writing it
  //    back — that race would resurrect a stale value over a fresh
  //    user write.
  std::vector<std::shared_ptr<VLog>> retired;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto& [number, vl] : vlogs_) {
      if (number != fresh_number) {
        retired.push_back(vl);
      }
    }
  }

  const auto drain_flush = [&]() -> Status {
    // Flush a pending imm_ inline: the GC runs ON the background thread,
    // so if it doesn't, nobody will until the scan finishes — and user
    // writers block on a pending imm_. No lock while flushing (the
    // callee takes the mutex itself).
    std::shared_ptr<MemTable> imm;
    uint64_t retired_log = 0, sst_number = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      imm = imm_;
      retired_log = imm_log_number_;
      sst_number = imm_sst_number_;
    }
    if (imm == nullptr) {
      return Status::Ok();
    }
    const Status s = FlushImmMemTable(imm, retired_log, sst_number);
    std::lock_guard<std::mutex> lk(mutex_);
    if (s.ok()) {
      imm_ = nullptr;
    }
    signal_.notify_all();
    return s;
  };

  uint64_t rewritten = 0;
  Status err = Status::Ok();
  for (const std::shared_ptr<VLog>& gen : retired) {
    if (!err.ok()) {
      break;
    }
    const uint64_t gen_number = gen->file_number();
    const Status scan_s = gen->ScanEntries(
        gen->file_size(),
        [&err, &rewritten, gen_number, this, &drain_flush](
            std::string_view key, std::string_view value, uint64_t offset) {
      if (!err.ok()) {
        return;
      }
      {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string stored;
        ValuePointer p;
        if (LookupIn(mem_, imm_, current_, key, &stored, nullptr) !=
                MemTable::Lookup::kFound ||
            !DecodeValuePointer(stored, &p) ||
            p.vlog_number != gen_number || p.offset != offset) {
          return;  // dead: overwritten, deleted, or a newer version exists
        }
        err = WriteEntry(kTypeValue, key, value,
                         /*count_user_bytes=*/false);  // GC traffic, not user
        if (err.ok()) {
          rewritten += value.size();
        }
      }
      if (err.ok()) {
        err = drain_flush();  // rotations must not stall user writers
      }
    });
    if (err.ok() && !scan_s.ok()) {
      err = scan_s;
    }
  }
  if (!err.ok()) {
    // Best-effort: keep everything published; every rewritten entry
    // already went through the normal (durable) write path, and the
    // rest are still readable in their old file. A later pass retries.
    return err;
  }

  // 3. Drop the old generations, republish the MANIFEST, then unlink.
  //    Crash before the manifest write: all generations stay named and
  //    the next Open re-runs GC. Crash after it: the old files are
  //    unnamed orphans and are removed on Open.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    // Snapshot guard (retirement): the scans above classified entries
    // dead by LATEST visibility while releasing the mutex between
    // entries. A snapshot created mid-pass may need exactly such an
    // entry: its pointer was current when the snapshot was taken, but a
    // newer write (after the snapshot) made it look dead. Rewriting
    // cannot help — the rewrite gets a new seq the snapshot cannot see.
    // So if any snapshot is alive NOW, abort the retirement: every
    // generation stays published, the snapshot's pointers stay
    // resolvable, and a later pass (after the last release) retries.
    // Snapshots created AND released during the scan are safe either
    // way: they could only read while the old files were still intact.
    if (!snapshots_.empty()) {
      return Status::Ok();
    }
    // Durability BEFORE retirement: the MANIFEST below un-names the old
    // generations, and without this barrier a crash could drop the
    // (not-yet-fsynced) rewrite records while Open deletes the retired
    // files — turning tail loss into permanent Corruption. The fsyncs
    // here cost one durability point per GC pass, which is rare.
    const Status d = ForceDurabilityLocked();
    if (!d.ok()) {
      return d;
    }
    for (const std::shared_ptr<VLog>& gen : retired) {
      vlogs_.erase(gen->file_number());
    }
    // The floor is the anti-livelock device: the next rewrite waits
    // until the file holds roughly this much NEW garbage. A pass that
    // found the file mostly live raises the floor to 2x the live data;
    // a pass that found mostly garbage lowers it toward zero.
    vlog_gc_floor_ = 2 * rewritten;
    const Status ms = WriteManifest();
    if (!ms.ok()) {
      // Old generations stay published and on disk; retry later. Their
      // entries are dead already (every live one was rewritten), so the
      // live estimate for the fresh generation stands.
      for (const std::shared_ptr<VLog>& gen : retired) {
        vlogs_.emplace(gen->file_number(), gen);
      }
      return ms;
    }
  }
  for (const std::shared_ptr<VLog>& gen : retired) {
    ::unlink(VlogPath(gen->file_number()).c_str());
  }
  stats_vlog_gcs_ += 1;
  return Status::Ok();
}

// ---- background thread: flush + compaction ----

void DB::BackgroundLoop() {
  std::unique_lock<std::mutex> lk(mutex_);
  for (;;) {
    signal_.wait(lk, [&] {
      return exit_ || imm_ != nullptr || CompactionNeeded() || VlogGcNeeded();
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
      continue;
    }
    if (VlogGcNeeded()) {
      vlog_gc_active_ = true;
      lk.unlock();
      const Status s = RunVlogGC();
      lk.lock();
      vlog_gc_active_ = false;
      if (!s.ok()) {
        // GC is also best-effort: the old generation stays published and
        // nothing is lost, just not yet reclaimed. Back off like the
        // flush path so a persistent failure doesn't spin.
        last_error_ = s;
        lk.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        lk.lock();
      }
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
  stats_sst_bytes_ += meta.file_size;
  stats_flushes_ += 1;

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
  // Merge all inputs. Retention is per user key, newest version first,
  // governed by the snapshot floor F = the smallest live snapshot's
  // sequence (kMaxSeq with no snapshots):
  //   * every version with seq > F is KEPT — some live snapshot may read
  //     it (we don't know which sequences exist above F, so be liberal);
  //   * the FIRST version with seq <= F is kept — it is what the oldest
  //     snapshot (and every snapshot, and every latest read when no
  //     newer version exists) must see;
  //   * everything older is dropped — no reader can reach it anymore.
  // With no snapshots this collapses to the classic keep-only-newest.
  // Tombstones: a kept version that is a tombstone survives UNLESS the
  // output is the bottom level — elsewhere it must keep shadowing the
  // levels below; at the bottom nothing older exists, so it (and the
  // versions it shadows) can all go.
  uint64_t snapshot_floor = 0;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    snapshot_floor = SmallestSnapshotSeq();
  }
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
  bool floor_version_seen = false;  // per user key: passed seq <= floor?
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
    stats_sst_bytes_ += meta.file_size;
    builder = sst::Builder{};
    builder_used = false;
  };

  while (iter.Valid() && s.ok()) {
    const std::string_view ik = iter.key();
    const std::string_view user = ExtractUserKey(ik);
    if (!has_user || user != current_user) {
      has_user = true;
      current_user.assign(user);
      floor_version_seen = false;
    }
    const uint64_t tag = ExtractTag(ik);
    const uint8_t type = static_cast<uint8_t>(tag & 0xff);
    bool keep;
    if ((tag >> 8) > snapshot_floor) {
      keep = true;  // above the floor: some snapshot may still read it
    } else if (!floor_version_seen) {
      floor_version_seen = true;
      // Newest version at-or-below the floor: what every snapshot sees.
      // At the bottom level a tombstone here shadows nothing anymore.
      keep = !(type == kTypeDeletion && bottom);
    } else {
      keep = false;  // older than any reader can reach
    }
    if (keep) {
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
    // Remove exactly the compaction's inputs (by file number, like the
    // leveled case). A plain clear() would be correct only while this is
    // the single thread that ever adds L0 files — a trap for any future
    // concurrent flush producer.
    remove_numbers(&newv->l0, inputs_a);
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
  stats_compactions_ += 1;
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
  // Holding the mutex while notifying is deliberate: the background
  // thread may already be past its predicate re-check, and a notify
  // sent while it evaluates work (not waiting) would be lost — the
  // caller would then sleep on a condition only the background thread
  // can make true (e.g. a pending vLog GC) with no further wakeups.
  std::unique_lock<std::mutex> lk(mutex_);
  signal_.notify_all();
  signal_.wait(lk, [&] {
    return exit_ || (imm_ == nullptr && !CompactionNeeded() &&
                     !VlogGcNeeded() && !vlog_gc_active_);
  });
}

}  // namespace bedrockkv
