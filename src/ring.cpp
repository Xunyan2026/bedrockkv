#include "bedrockkv/ring.h"

#include <cerrno>
#include <cstring>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/io_uring.h>  // kernel UAPI: io_uring_params / SQE layout

namespace bedrockkv {

bool Ring::Supported() {
  io_uring_params params;
  std::memset(&params, 0, sizeof(params));
  const int fd =
      static_cast<int>(::syscall(kSetup, 8, &params));
  if (fd >= 0) {
    ::close(fd);
    return true;
  }
  return false;
}

std::unique_ptr<Ring> Ring::Open(unsigned entries, Status* status) {
  io_uring_params params;
  std::memset(&params, 0, sizeof(params));
  const int fd =
      static_cast<int>(::syscall(kSetup, entries, &params));
  if (fd < 0) {
    if (status != nullptr) {
      *status = Status::IOError("io_uring_setup failed: " +
                                std::string(std::strerror(errno)));
    }
    return nullptr;
  }

  auto ring = std::unique_ptr<Ring>(new Ring());
  ring->fd_ = fd;
  ring->entries_ = entries;
  ring->sq_mask_ = params.sq_off.ring_mask;
  ring->cq_mask_ = params.cq_off.ring_mask;
  ring->cq_off_ = CqOffsets{params.cq_off.head, params.cq_off.tail,
                            params.cq_off.ring_mask,
                            params.cq_off.ring_entries,
                            params.cq_off.overflow,
                            params.cq_off.cqes};

  // The SQ ring region holds: the index array at sq_off.array, preceded by
  // the control fields. Size it from the kernel-provided offsets — the
  // layout changed once historically (5.4 vs 5.5) and must not be guessed.
  ring->sq_ring_size_ = params.sq_off.array + entries * sizeof(uint32_t);
  ring->sq_ring_ = ::mmap(nullptr, ring->sq_ring_size_,
                          PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          fd, static_cast<off_t>(kOffSqRing));
  ring->cq_ring_size_ = params.cq_off.cqes + entries * sizeof(Cqe);
  ring->cq_ring_ = ::mmap(nullptr, ring->cq_ring_size_,
                          PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                          fd, static_cast<off_t>(kOffCqRing));
  ring->sqes_size_ = entries * 64;  // sizeof(io_uring_sqe) == 64, ABI-fixed
  ring->sqes_ = ::mmap(nullptr, ring->sqes_size_, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, fd,
                       static_cast<off_t>(kOffSqes));
  if (ring->sq_ring_ == MAP_FAILED || ring->cq_ring_ == MAP_FAILED ||
      ring->sqes_ == MAP_FAILED) {
    if (status != nullptr) {
      *status = Status::IOError(std::string("io_uring mmap failed: ") +
                                std::strerror(errno));
    }
    return nullptr;  // ~Ring unmaps what succeeded and closes fd
  }

  auto* sq_ring = reinterpret_cast<char*>(ring->sq_ring_);
  ring->sq_head_ = reinterpret_cast<volatile uint32_t*>(sq_ring +
                                                        params.sq_off.head);
  ring->sq_tail_pub_ = reinterpret_cast<uint32_t*>(sq_ring +
                                                   params.sq_off.tail);
  ring->sq_array_ = reinterpret_cast<uint32_t*>(sq_ring +
                                                params.sq_off.array);
  auto* cq_ring = reinterpret_cast<char*>(ring->cq_ring_);
  ring->cq_head_ = reinterpret_cast<uint32_t*>(cq_ring + params.cq_off.head);
  ring->cq_tail_ = reinterpret_cast<volatile uint32_t*>(cq_ring +
                                                        params.cq_off.tail);
  return ring;
}

Ring::~Ring() {
  if (sq_ring_ != nullptr && sq_ring_ != MAP_FAILED) {
    ::munmap(sq_ring_, sq_ring_size_);
  }
  if (cq_ring_ != nullptr && cq_ring_ != MAP_FAILED) {
    ::munmap(cq_ring_, cq_ring_size_);
  }
  if (sqes_ != nullptr && sqes_ != MAP_FAILED) {
    ::munmap(sqes_, sqes_size_);
  }
  if (fd_ >= 0) {
    // Outstanding operations: the kernel finishes or cancels them as the
    // ring dies. DB always drains before closing its files, so reaching
    // the destructor with outstanding work would already be a bug there.
    ::close(fd_);
  }
}

bool Ring::QueueWrite(int fd, const void* buf, size_t len, uint64_t offset,
                      uint64_t user_data) {
  if (queued_ == entries_) {
    return false;  // SQ full: caller must Flush() first
  }
  // Build the SQE in the shared array and register it in the index array.
  // (Kernel 5.6+ for IORING_OP_WRITE; O_APPEND on fd is ignored — pwrite
  // semantics — which is what makes explicit-offset WAL writes safe.)
  auto* sqe = reinterpret_cast<std::byte*>(sqes_) +
               static_cast<size_t>(sq_tail_ & sq_mask_) * 64;
  std::memset(sqe, 0, 64);
  sqe[0] = static_cast<std::byte>(kOpWrite);           // opcode
  *reinterpret_cast<int32_t*>(sqe + 4) = fd;           // fd
  *reinterpret_cast<uint64_t*>(sqe + 8) = offset;      // off
  *reinterpret_cast<uint64_t*>(sqe + 16) =
      reinterpret_cast<uint64_t>(buf);                 // addr
  *reinterpret_cast<uint32_t*>(sqe + 24) =
      static_cast<uint32_t>(len);                      // len
  *reinterpret_cast<uint64_t*>(sqe + 32) = user_data;  // user_data

  sq_array_[sq_tail_ & sq_mask_] = sq_tail_ & sq_mask_;
  ++sq_tail_;
  ++queued_;
  return true;
}

bool Ring::QueueFsync(int fd, uint64_t user_data) {
  if (queued_ == entries_) {
    return false;
  }
  auto* sqe = reinterpret_cast<std::byte*>(sqes_) +
               static_cast<size_t>(sq_tail_ & sq_mask_) * 64;
  std::memset(sqe, 0, 64);
  sqe[0] = static_cast<std::byte>(kOpFsync);
  *reinterpret_cast<int32_t*>(sqe + 4) = fd;
  *reinterpret_cast<uint64_t*>(sqe + 32) = user_data;
  sq_array_[sq_tail_ & sq_mask_] = sq_tail_ & sq_mask_;
  ++sq_tail_;
  ++queued_;
  return true;
}

bool Ring::Flush(bool wait_all) {
  if (queued_ == 0 && !wait_all) {
    return true;  // nothing to push
  }
  const unsigned to_submit = queued_;
  const unsigned min_complete = wait_all ? outstanding_ + queued_ : 0;
  queued_ = 0;
  if (to_submit > 0) {
    *sq_tail_pub_ = sq_tail_;  // publish: kernel now owns the SQEs
  }
  // One enter() both submits and (with GETEVENTS) waits — the batching
  // point where N syscalls collapse into one.
  const long rc =
      ::syscall(kEnter, fd_, to_submit, min_complete,
                min_complete > 0 ? kGetEvents : 0u, nullptr, 0);
  if (rc < 0) {
    return false;
  }
  return true;
}

}  // namespace bedrockkv
