// BedrockKV — minimal io_uring ring, raw syscalls, zero dependencies.
//
// Why hand-rolled (interview gold): liburing is a thin convenience wrapper
// over three syscalls (io_uring_setup / io_uring_enter / io_uring_register)
// plus three mmaps. Vendoring ~200 lines removes the packaging dependency
// and — more importantly for a storage engine — forces every subtle
// contract to be written down explicitly instead of inherited invisibly.
//
// The three syscalls (x86_64 numbers):
//   425 io_uring_setup(entries, struct io_uring_params*) -> ring fd
//   426 io_uring_enter(fd, to_submit, min_complete, flags, arg, size)
//   (io_uring_register is not needed for our write/fsync use.)
//
// The kernel creates a shared-memory submission queue (SQ), completion
// queue (CQ) and an array of 64-byte submission queue entries (SQEs),
// which the caller mmaps from the ring fd at magic offsets:
//   IORING_OFF_SQ_RING (0)          — SQ indices + SQ index array + CQ
//   IORING_OFF_CQ_RING (0x8000000)  — CQ indices + CQEs (never separate
//                                     from SQ unless IORING_SETUP_SQE128
//                                     style layout splits them; the params
//                                     offsets are authoritative)
//   IORING_OFF_SQES    (0x10000000) — the SQE array
// All sizes and offsets come from io_uring_params — never hard-coded.
//
// Threading: NOT internally synchronized. BedrockKV submits from the
// writer's mutex-held sections only, so plain (non-atomic) ring-index
// access is correct; if you reuse this elsewhere, add barriers.
//
// Kernel requirement: IORING_OP_WRITE (non-vectored, explicit offset)
// needs Linux >= 5.6. On anything older (or a sandbox that hides
// io_uring — gVisor returns ENOSYS when it is disabled), Open() simply
// fails and the caller falls back to the synchronous write path.
#pragma once

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>

#include "bedrockkv/status.h"

namespace bedrockkv {

class Ring {
 public:
  // Probes the kernel: returns true only if io_uring_setup actually
  // works here. Cheap (one syscall), safe to call repeatedly.
  static bool Supported();

  // Opens a ring with `entries` submission slots. On failure returns
  // nullptr and (if given) sets *status with the errno text — callers
  // must fall back to synchronous I/O.
  static std::unique_ptr<Ring> Open(unsigned entries, Status* status);

  ~Ring();
  Ring(const Ring&) = delete;
  Ring& operator=(const Ring&) = delete;

  // Queues one IORING_OP_WRITE (pwrite semantics: explicit offset).
  // CALLER CONTRACT: `fd` must NOT carry O_APPEND — on real kernels
  // O_APPEND overrides the explicit offset (the write lands at EOF in
  // the kernel's completion order), which would scramble an async WAL.
  // With a plain O_RDWR fd, disjoint-offset writes may complete in any
  // order without corrupting the record stream. The DB's sync path keeps
  // O_APPEND on purpose (post-truncation recovery appends); the async
  // path's WAL fds are opened without it.
  // `buf` must stay valid until the matching completion is reaped.
  // Returns false if the SQ is full — call Flush()/Reap() first.
  bool QueueWrite(int fd, const void* buf, size_t len, uint64_t offset,
                  uint64_t user_data);

  // Queues one IORING_OP_FSYNC. Pairing this with QueueWrite lets the DB
  // sync two independent files (vLog + WAL) in one round trip instead of
  // two sequential fsyncs — the headline win on io_uring-capable hosts.
  bool QueueFsync(int fd, uint64_t user_data);

  // Pushes every queued SQE to the kernel. With wait_all, also sleeps
  // until ALL outstanding completions (including earlier ones) have
  // arrived. Returns false on io_uring_enter failure.
  bool Flush(bool wait_all);

  // Number of submitted-but-not-yet-reaped operations.
  size_t outstanding() const { return outstanding_; }

  // Drains every available CQE. `fn(user_data, result)` is called for
  // each: result < 0 is the operation's -errno. Returns false if the
  // enter-for-events step failed (only when wait=true).
  template <typename Fn>
  bool Reap(Fn fn, bool wait) {
    if (wait && outstanding_ > 0 && !Flush(false)) {
      return false;
    }
    if (wait && outstanding_ > 0) {
      // enter() with to_submit=0 and GETEVENTS: sleep until min_complete.
      // EINTR-safe retry (same reasoning as Flush).
      long rc;
      do {
        rc = ::syscall(kEnter, fd_, 0, outstanding_, kGetEvents, nullptr, 0);
      } while (rc < 0 && errno == EINTR);
      if (rc < 0) {
        return false;
      }
    }
    for (;;) {
      const uint32_t head = *cq_head_;
      if (head == *cq_tail_) {
        break;  // nothing more available right now
      }
      const auto* cqe = reinterpret_cast<const Cqe*>(
          reinterpret_cast<const char*>(cq_ring_) + cq_off_.cqes +
          (head & cq_mask_) * sizeof(Cqe));
      *cq_head_ = head + 1;  // publish the slot as consumed
      if (outstanding_ > 0) {
        --outstanding_;  // belt-and-braces: never let this underflow
      }
      fn(cqe->user_data, cqe->res);
    }
    return true;
  }

 private:
  Ring() = default;

  struct SqOffsets {
    uint32_t head, tail, ring_mask, ring_entries, flags, dropped, array;
  };
  struct CqOffsets {
    uint32_t head, tail, ring_mask, ring_entries, overflow, cqes;
  };
  struct Cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
  };

  static constexpr int kSetup = 425;   // io_uring_setup
  static constexpr int kEnter = 426;   // io_uring_enter
  static constexpr uint64_t kOffSqRing = 0;            // IORING_OFF_SQ_RING
  static constexpr uint64_t kOffCqRing = 0x8000000ULL; // IORING_OFF_CQ_RING
  static constexpr uint64_t kOffSqes = 0x10000000ULL;  // IORING_OFF_SQES
  static constexpr uint32_t kGetEvents = 1u;           // IORING_ENTER_GETEVENTS
  static constexpr uint8_t kOpWrite = 23;  // IORING_OP_WRITE (5.6+)
  static constexpr uint8_t kOpFsync = 3;   // IORING_OP_FSYNC

  int fd_ = -1;
  unsigned entries_ = 0;
  size_t outstanding_ = 0;  // submitted, not yet reaped
  uint32_t sq_tail_ = 0;    // next SQ slot to fill (index into sqes_)
  uint32_t queued_ = 0;     // SQEs queued since the last Flush()

  // mmap'd regions and their byte sizes (for munmap).
  void* sq_ring_ = nullptr;
  size_t sq_ring_size_ = 0;
  void* cq_ring_ = nullptr;
  size_t cq_ring_size_ = 0;
  void* sqes_ = nullptr;
  size_t sqes_size_ = 0;

  // Into the SQ ring: kernel-visible head, our tail, the index array.
  volatile uint32_t* sq_head_ = nullptr;
  uint32_t* sq_tail_pub_ = nullptr;
  uint32_t* sq_array_ = nullptr;
  // Into the CQ ring: kernel-written tail, our consumed head, CQE array.
  volatile uint32_t* cq_tail_ = nullptr;
  uint32_t* cq_head_ = nullptr;
  uint32_t sq_mask_ = 0, cq_mask_ = 0;
  CqOffsets cq_off_{};
};

}  // namespace bedrockkv
