// BedrockKV — a tiny Redis-compatible TCP server over the DB engine.
//
// Architecture (deliberately Redis-shaped, deliberately single-threaded):
//
//   accept loop ──► epoll(LT) wait ──► read socket ──► resp::Parser
//                                                     └─► Execute(cmd, db)
//                                                         └─► write socket
//
// ONE event-loop thread accepts, parses, executes and replies. This is
// not laziness — it is the correct first design, for two reasons:
//   * BedrockKV's Put/Delete contract is single-writer (leveldb-style);
//     serializing commands through one thread satisfies it with no locks
//     and no ordering hazards.
//   * It is exactly Redis' own model: networked storage spends its time
//     on syscalls and fsyncs, not command arithmetic, so a serial loop
//     scales until those dominate (Redis famously ships this way).
// A thread pool would buy nothing here except races; if profiling ever
// shows the parser/CPU as the bottleneck, sharded loops are the escape
// hatch — the protocol layer is already loop-agnostic.
//
// Level-triggered epoll: with LT, a readable socket keeps firing until
// drained, so "read once, let the next wait() re-arm" is stateless and
// starvation-free; the price (an extra wait() round per partially-read
// connection) is irrelevant at this scale. Writes: replies are buffered
// per connection and flushed inline; a partial write arms EPOLLOUT until
// the buffer drains. Long-lived non-readers are evicted once their reply
// backlog crosses kMaxOutBytes — a client that never reads must not be
// able to grow our memory without bound.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "bedrockkv/db.h"
#include "bedrockkv/resp.h"
#include "bedrockkv/status.h"

namespace bedrockkv {

class RedisServer {
 public:
  // Binds and listens (does not start serving yet). `port` 0 lets the OS
  // pick one — tests use that to dodge port collisions; the chosen port
  // is then readable via port(). On failure returns nullptr with *status.
  static std::unique_ptr<RedisServer> Open(DB* db, const std::string& addr,
                                           uint16_t port, Status* status);
  ~RedisServer();

  RedisServer(const RedisServer&) = delete;
  RedisServer& operator=(const RedisServer&) = delete;

  // Spawns the event-loop thread. Idempotent.
  bool Start();
  // Stops the loop and closes every connection. Called by the destructor.
  void Stop();

  uint16_t port() const { return port_; }

  // Executes one parsed command against the DB and encodes the reply.
  // Exposed for tests: the full command layer without sockets.
  static std::string Execute(const std::vector<std::string>& args,
                             DB* db);

 private:
  RedisServer() = default;

  void Loop();
  // Returns false if the connection died (caller closes it).
  bool HandleReadable(int fd);
  bool HandleWritable(int fd);
  // Flushes conn.out; arms EPOLLOUT only while bytes remain.
  bool FlushOut(int fd);
  void AcceptNewClients();
  void CloseConn(int fd);

  static constexpr size_t kMaxOutBytes = 64u << 20;   // reply backlog cap
  static constexpr size_t kMaxInBytes = 64u << 20;    // unread input cap
  static constexpr size_t kMaxConns = 4096;

  DB* db_ = nullptr;
  int listen_fd_ = -1;
  int epfd_ = -1;
  int wake_fd_ = -1;  // eventfd: Stop() writes here to break epoll_wait
  uint16_t port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread loop_;

  // One state machine per connection. Read buffer lives inside the
  // parser; out accumulates replies until the socket takes them all.
  // out_off_ is the write cursor into out — replying in place and
  // compacting once per full flush keeps a 64 MiB backlog O(n) instead
  // of erasing the prefix after every write (O(n^2) memmoves).
  struct Conn {
    resp::Parser parser;
    std::string out;
    size_t out_off_ = 0;
    bool closing = false;  // protocol error: flush pending reply, then die
  };
  std::map<int, Conn> conns_;  // loop thread only
};

}  // namespace bedrockkv
