#include "bedrockkv/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace bedrockkv {

using resp::BulkString;
using resp::Error;
using resp::Integer;
using resp::NullBulk;
using resp::SimpleString;

// ---- command execution ----------------------------------------------------

namespace {

// Lowercases a command name in place of an allocation-heavy std::transform
// on a temporary: command names are tiny, a fixed buffer suffices.
std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
  }
  return out;
}

}  // namespace

std::string RedisServer::Execute(const std::vector<std::string>& args,
                                 DB* db) {
  if (args.empty()) {
    return Error("ERR empty command");
  }
  const std::string cmd = Lower(args[0]);
  const size_t argc = args.size();

  if (cmd == "ping") {
    // PING [message]: no arg → +PONG, else the message echoed as a bulk.
    return argc == 1 ? SimpleString("PONG") : BulkString(args[1]);
  }
  if (cmd == "echo") {
    if (argc != 2) {
      return Error("ERR wrong number of arguments for 'echo' command");
    }
    return BulkString(args[1]);
  }
  if (cmd == "set") {
    if (argc != 3) {
      return Error("ERR wrong number of arguments for 'set' command");
    }
    const Status s = db->Put(args[1], args[2]);
    return s.ok() ? SimpleString("OK")
                  : Error("ERR " + s.message());  // e.g. WAL fsync failure
  }
  if (cmd == "get") {
    if (argc != 2) {
      return Error("ERR wrong number of arguments for 'get' command");
    }
    std::string value;
    const Status s = db->Get(args[1], &value);
    if (s.ok()) {
      return BulkString(value);
    }
    if (s.code() == Status::Code::kNotFound) {
      return NullBulk();
    }
    return Error("ERR " + s.message());
  }
  if (cmd == "del") {
    if (argc < 2) {
      return Error("ERR wrong number of arguments for 'del' command");
    }
    // Redis counts only keys that actually existed: a Delete on an absent
    // key succeeds (it is a tombstone write), so existence is checked first.
    int64_t removed = 0;
    std::string sink;
    for (size_t i = 1; i < argc; ++i) {
      if (db->Get(args[i], &sink).ok() && db->Delete(args[i]).ok()) {
        ++removed;
      }
    }
    return Integer(removed);
  }
  if (cmd == "exists") {
    if (argc < 2) {
      return Error("ERR wrong number of arguments for 'exists' command");
    }
    int64_t found = 0;
    std::string sink;
    for (size_t i = 1; i < argc; ++i) {
      if (db->Get(args[i], &sink).ok()) {
        ++found;
      }
    }
    return Integer(found);
  }
  return Error("ERR unknown command '" + cmd + "'");
}

// ---- lifecycle ------------------------------------------------------------

std::unique_ptr<RedisServer> RedisServer::Open(DB* db, const std::string& addr,
                                               uint16_t port, Status* status) {
  auto fail = [&](const std::string& msg) {
    if (status != nullptr) {
      *status = Status::IOError(msg);
    }
    return nullptr;
  };
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          0);
  if (fd < 0) {
    return fail(std::string("socket: ") + std::strerror(errno));
  }
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (::inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) {
    ::close(fd);
    return fail("invalid bind address: " + addr);
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    const int err = errno;
    ::close(fd);
    return fail(std::string("bind: ") + std::strerror(err));
  }
  if (::listen(fd, 128) != 0) {
    const int err = errno;
    ::close(fd);
    return fail(std::string("listen: ") + std::strerror(err));
  }
  // Port 0 (OS-assigned) — recover the real port so tests can connect.
  sockaddr_in bound{};
  socklen_t len = sizeof(bound);
  uint16_t real_port = port;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
    real_port = ntohs(bound.sin_port);
  }

  auto server = std::unique_ptr<RedisServer>(new RedisServer());
  server->db_ = db;
  server->listen_fd_ = fd;
  server->port_ = real_port;
  server->epfd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (server->epfd_ < 0) {
    ::close(fd);
    return fail(std::string("epoll_create1: ") + std::strerror(errno));
  }
  server->wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (server->wake_fd_ < 0) {
    ::close(server->epfd_);
    ::close(fd);
    return fail(std::string("eventfd: ") + std::strerror(errno));
  }
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if (::epoll_ctl(server->epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
    const int err = errno;
    ::close(server->wake_fd_);
    ::close(server->epfd_);
    ::close(fd);
    return fail(std::string("epoll_ctl: ") + std::strerror(err));
  }
  ev.data.fd = server->wake_fd_;
  if (::epoll_ctl(server->epfd_, EPOLL_CTL_ADD, server->wake_fd_, &ev) != 0) {
    const int err = errno;
    ::close(server->wake_fd_);
    ::close(server->epfd_);
    ::close(fd);
    return fail(std::string("epoll_ctl: ") + std::strerror(err));
  }
  if (status != nullptr) {
    *status = Status::Ok();
  }
  return server;
}

RedisServer::~RedisServer() { Stop(); }

bool RedisServer::Start() {
  if (loop_.joinable()) {
    return true;  // already running
  }
  stop_ = false;
  loop_ = std::thread([this] { Loop(); });
  return true;
}

void RedisServer::Stop() {
  stop_ = true;
  if (wake_fd_ >= 0) {
    const uint64_t one = 1;
    // Best-effort wake: if the loop thread has already exited the loop,
    // this write lands in a dying eventfd and that is fine.
    const ssize_t rc = ::write(wake_fd_, &one, sizeof(one));
    (void)rc;
  }
  if (loop_.joinable()) {
    loop_.join();
  }
  for (const auto& [fd, conn] : conns_) {
    ::close(fd);
  }
  conns_.clear();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (wake_fd_ >= 0) {
    ::close(wake_fd_);
    wake_fd_ = -1;
  }
  if (epfd_ >= 0) {
    ::close(epfd_);
    epfd_ = -1;
  }
}

// ---- event loop -----------------------------------------------------------

void RedisServer::Loop() {
  epoll_event events[128];
  while (!stop_) {
    const int n = ::epoll_wait(epfd_, events, 128, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;  // epoll itself broke; nothing sane to do
    }
    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      if (fd == wake_fd_) {
        continue;  // Stop() requested a wake; the while condition exits
      }
      if (fd == listen_fd_) {
        AcceptNewClients();
        continue;
      }
      if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0 &&
          (events[i].events & EPOLLIN) == 0) {
        CloseConn(fd);
        continue;
      }
      if ((events[i].events & EPOLLOUT) != 0) {
        if (!HandleWritable(fd)) {
          CloseConn(fd);
          continue;
        }
      }
      if ((events[i].events & EPOLLIN) != 0) {
        if (!HandleReadable(fd)) {
          CloseConn(fd);
        }
      }
    }
  }
}

void RedisServer::AcceptNewClients() {
  // Level-triggered listener: drain until EAGAIN, then let epoll re-arm.
  for (;;) {
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const int fd = ::accept4(listen_fd_, reinterpret_cast<sockaddr*>(&peer),
                             &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;  // EAGAIN: done; EMFILE etc.: nothing we can do this round
    }
    if (conns_.size() >= kMaxConns) {
      ::close(fd);  // shed load; the client sees an immediate EOF
      continue;
    }
    // TCP_NODELAY: replies are tiny (OK/PONG/counts) and latency-bound;
    // Nagle's algorithm would hold them for a coalescing window that
    // never pays off for a request-response protocol.
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
      ::close(fd);
      continue;
    }
    // try_emplace: Conn holds a non-movable resp::Parser, so it must be
    // built in place inside the map node (emplace(fd, Conn{}) would try
    // to construct a pair from a temporary and fail to move it).
    conns_.try_emplace(fd);
  }
}

bool RedisServer::HandleReadable(int fd) {
  Conn& conn = conns_.at(fd);
  char buf[16384];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN) {
        break;
      }
      return false;  // real socket error
    }
    if (n == 0) {
      return false;  // peer closed
    }
    conn.parser.Feed(std::string_view(buf, static_cast<size_t>(n)));
    if (conn.parser.pending() > kMaxInBytes) {
      // Unread-input cap: a half-issued multibulk streams bytes forever
      // (the parser cannot compact a request it has not finished), so
      // without a ceiling N connections pin kMaxBulkLen each —
      // attacker-chosen memory. Redis hard-limits its query buffer the
      // same way and drops the client. closing with an empty out means
      // FlushOut below returns false: immediate, silent teardown.
      conn.closing = true;
      break;
    }

    // Drain every complete request this chunk completed (pipelining:
    // redis-benchmark sends many commands per packet; replying per
    // command but writing once keeps syscalls proportional to packets,
    // not commands).
    std::vector<std::string> args;
    std::string error;
    for (;;) {
      const auto st = conn.parser.Next(&args, &error);
      if (st == resp::NextStatus::kNeedMore) {
        break;
      }
      if (st == resp::NextStatus::kError) {
        conn.out += Error("ERR " + error);
        conn.closing = true;
        break;
      }
      conn.out += Execute(args, db_);
      if (conn.out.size() - conn.out_off_ > kMaxOutBytes) {
        // The peer stopped reading: shed it before its backlog eats us.
        conn.closing = true;
        break;
      }
    }
    if (conn.closing) {
      break;
    }
    if (conn.out.size() - conn.out_off_ > kMaxOutBytes / 2) {
      break;  // handle writes before reading more; LT re-arms EPOLLIN
    }
  }
  // A `closing` conn must NOT be torn down here: its protocol-error reply
  // may still sit in out (FlushOut hit EAGAIN and armed EPOLLOUT). FlushOut
  // owns the death decision — it returns false once the reply is fully
  // flushed on a closing conn, or on any socket error.
  return FlushOut(fd);
}

bool RedisServer::HandleWritable(int fd) {
  return FlushOut(fd);
}

bool RedisServer::FlushOut(int fd) {
  Conn& conn = conns_.at(fd);
  while (conn.out_off_ < conn.out.size()) {
    const ssize_t n =
        ::write(fd, conn.out.data() + conn.out_off_,
                conn.out.size() - conn.out_off_);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN) {
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT;
        ev.data.fd = fd;
        return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
      }
      return false;
    }
    conn.out_off_ += static_cast<size_t>(n);
  }
  // Fully flushed: compact the buffer once per cycle (an O(n) move
  // instead of erasing the prefix after every write).
  if (conn.out_off_ > 0) {
    conn.out.erase(0, conn.out_off_);
    conn.out_off_ = 0;
  }
  if (conn.closing) {
    return false;  // protocol error reply (or backlog) flushed: now die
  }
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}

void RedisServer::CloseConn(int fd) {
  ::close(fd);  // also removes it from the epoll set
  conns_.erase(fd);
}

}  // namespace bedrockkv
