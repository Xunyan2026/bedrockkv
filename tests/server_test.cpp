// BedrockKV — RedisServer integration tests over real localhost sockets.
//
// These exercise the whole stack: kernel socket → epoll loop → parser →
// command dispatch → DB → reply bytes back on the wire. The client side
// speaks raw RESP with the exact bytes a redis-cli session would produce.
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "bedrockkv/db.h"
#include "bedrockkv/resp.h"
#include "bedrockkv/server.h"

namespace {

std::atomic<int> g_dir_counter{0};

std::string FreshDir(const char* tag) {
  return "/tmp/bedrockkv_srv_" + std::string(tag) + "_" +
         std::to_string(::getpid()) + "_" +
         std::to_string(g_dir_counter.fetch_add(1));
}

// ---- minimal blocking test client -----------------------------------------

int Connect(uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  timeval tv{.tv_sec = 10, .tv_usec = 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

void SendAll(int fd, const std::string& bytes) {
  size_t sent = 0;
  while (sent < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + sent, bytes.size() - sent);
    ASSERT_GT(n, 0) << std::strerror(errno);
    sent += static_cast<size_t>(n);
  }
}

// Reads until exactly `n` reply bytes have arrived, then returns them.
std::string RecvExactly(int fd, size_t n) {
  std::string acc;
  char buf[4096];
  while (acc.size() < n) {
    const ssize_t got = ::read(fd, buf, sizeof(buf));
    if (got <= 0) {
      ADD_FAILURE() << "short read (" << got << ", errno " << errno
                    << ") after " << acc.size() << "/" << n << " bytes";
      return acc;
    }
    acc.append(buf, static_cast<size_t>(got));
  }
  return acc.substr(0, n);
}

std::string Encode(const std::vector<std::string>& args) {
  std::string out = "*" + std::to_string(args.size()) + "\r\n";
  for (const auto& a : args) {
    out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
  }
  return out;
}

// Reads exactly expected.size() reply bytes and compares. Length is taken
// from the expectation itself — hand-counted lengths were wrong twice.
void ExpectReply(int fd, const std::string& expected) {
  EXPECT_EQ(RecvExactly(fd, expected.size()), expected);
}

// Reads one CRLF-terminated reply line (-ERR replies have variable length,
// so a fixed-size read would leave bytes behind and desync later asserts).
std::string RecvLine(int fd) {
  std::string acc;
  char buf[1];
  while (true) {
    const ssize_t got = ::read(fd, buf, 1);
    if (got <= 0) {
      ADD_FAILURE() << "short read while expecting a reply line";
      return acc;
    }
    acc.push_back(buf[0]);
    if (acc.size() >= 2 && acc.substr(acc.size() - 2) == "\r\n") {
      return acc;
    }
  }
}

// ---- server fixture --------------------------------------------------------

struct ServerHandle {
  std::string dir;
  std::unique_ptr<bedrockkv::DB> db;
  std::unique_ptr<bedrockkv::RedisServer> server;
};

ServerHandle StartServer(const char* tag) {
  ServerHandle h;
  h.dir = FreshDir(tag);
  h.db = bedrockkv::DB::Open(h.dir);
  h.server = bedrockkv::RedisServer::Open(h.db.get(), "127.0.0.1", 0,
                                          nullptr);
  EXPECT_NE(h.server, nullptr);
  EXPECT_TRUE(h.server->Start());
  return h;
}

void StopServer(ServerHandle* h) {
  if (h->server) {
    h->server->Stop();
    h->server.reset();
  }
  if (h->db) {
    h->db.reset();
  }
}

}  // namespace

TEST(ServerTest, PingEchoUnknownCommand) {
  auto h = StartServer("ping");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);

  SendAll(fd, Encode({"PING"}));
  ExpectReply(fd, "+PONG\r\n");

  SendAll(fd, Encode({"PING", "hello"}));
  ExpectReply(fd, "$5\r\nhello\r\n");

  SendAll(fd, Encode({"ECHO", "abc"}));
  ExpectReply(fd, "$3\r\nabc\r\n");

  SendAll(fd, Encode({"FROBNICATE", "x"}));
  const std::string reply = RecvLine(fd);
  EXPECT_EQ(reply.substr(0, 3), "-ER");
  EXPECT_NE(reply.find("unknown command"), std::string::npos);

  SendAll(fd, Encode({"SET", "only-key"}));  // wrong arity
  const std::string arity = RecvLine(fd);
  EXPECT_NE(arity.find("wrong number of arguments"), std::string::npos);
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, SetGetDelExistsOverSocket) {
  auto h = StartServer("kv");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);

  SendAll(fd, Encode({"SET", "k", "v1"}));
  ExpectReply(fd, "+OK\r\n");

  SendAll(fd, Encode({"GET", "k"}));
  ExpectReply(fd, "$2\r\nv1\r\n");

  SendAll(fd, Encode({"EXISTS", "k"}));
  ExpectReply(fd, ":1\r\n");

  SendAll(fd, Encode({"GET", "missing"}));
  ExpectReply(fd, "$-1\r\n");

  SendAll(fd, Encode({"DEL", "k", "other"}));
  ExpectReply(fd, ":1\r\n");

  SendAll(fd, Encode({"EXISTS", "k"}));
  ExpectReply(fd, ":0\r\n");
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, InlineCommandsOverSocket) {
  auto h = StartServer("inline");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);
  // What a human types in `telnet localhost 7379`.
  SendAll(fd, "SET a 42\r\nGET a\r\nDEL a\r\n");
  ExpectReply(fd, "+OK\r\n$2\r\n42\r\n:1\r\n");
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, PipelinedCommandsOneWrite) {
  auto h = StartServer("pipeline");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);
  std::string wire = Encode({"SET", "a", "1"}) + Encode({"SET", "b", "2"}) +
                     Encode({"GET", "a"}) + Encode({"EXISTS", "a", "b", "c"});
  SendAll(fd, wire);
  ExpectReply(fd, "+OK\r\n+OK\r\n$1\r\n1\r\n:2\r\n");
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, BinarySafeKeysAndValues) {
  auto h = StartServer("binary");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);
  const std::string key(8, '\0');  // all-NUL key
  const std::string val = std::string("a\r\nb") + std::string(4, '\0') + "c";
  SendAll(fd, Encode({"SET", key, val}));
  ExpectReply(fd, "+OK\r\n");
  SendAll(fd, Encode({"GET", key}));
  const std::string want = "$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
  EXPECT_EQ(RecvExactly(fd, want.size()), want);
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, SplitPacketAcrossWrites) {
  auto h = StartServer("split");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);
  const std::string req = Encode({"SET", "k", "some-value"});
  SendAll(fd, req.substr(0, 5));
  ::usleep(50 * 1000);  // let the server sit on the partial request
  SendAll(fd, req.substr(5));
  ExpectReply(fd, "+OK\r\n");
  SendAll(fd, Encode({"GET", "k"}));
  ExpectReply(fd, "$10\r\nsome-value\r\n");
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, ProtocolErrorClosesConnection) {
  auto h = StartServer("protoerr");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);
  SendAll(fd, "*abc\r\n");
  const std::string reply = RecvLine(fd);
  EXPECT_EQ(reply.substr(0, 4), "-ERR");
  EXPECT_NE(reply.find("multibulk"), std::string::npos);
  // The server must close, not silently resync: the next byte we read is
  // EOF (read() == 0), matching how real Redis drops a broken client.
  char buf[16];
  EXPECT_EQ(::read(fd, buf, sizeof(buf)), 0);
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, RestartPersistsDataBehindProtocol) {
  auto h = StartServer("restart");
  {
    const int fd = Connect(h.server->port());
    ASSERT_GE(fd, 0);
    SendAll(fd, Encode({"SET", "durable", "survives-restart"}));
    ExpectReply(fd, "+OK\r\n");
    ::close(fd);
  }
  StopServer(&h);  // stops loop AND closes the DB

  h.db = bedrockkv::DB::Open(h.dir);
  ASSERT_NE(h.db, nullptr);
  h.server = bedrockkv::RedisServer::Open(h.db.get(), "127.0.0.1", 0, nullptr);
  ASSERT_NE(h.server, nullptr);
  ASSERT_TRUE(h.server->Start());

  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);
  SendAll(fd, Encode({"GET", "durable"}));
  ExpectReply(fd, "$16\r\nsurvives-restart\r\n");
  ::close(fd);
  StopServer(&h);
}

TEST(ServerTest, SequentialConnectionsShareData) {
  auto h = StartServer("conns");
  for (int round = 0; round < 3; ++round) {
    const int fd = Connect(h.server->port());
    ASSERT_GE(fd, 0);
    SendAll(fd, Encode({"SET", "shared", std::to_string(round)}));
    ExpectReply(fd, "+OK\r\n");
    SendAll(fd, Encode({"GET", "shared"}));
    const std::string val = std::to_string(round);
    const std::string want =
        "$" + std::to_string(val.size()) + "\r\n" + val + "\r\n";
    EXPECT_EQ(RecvExactly(fd, want.size()), want);
    ::close(fd);
  }
  StopServer(&h);
}

TEST(ServerTest, LargeValueRoundTrip) {
  auto h = StartServer("large");
  const int fd = Connect(h.server->port());
  ASSERT_GE(fd, 0);
  const std::string big(1024 * 1024, 'z');
  SendAll(fd, Encode({"SET", "big", big}));
  ExpectReply(fd, "+OK\r\n");
  SendAll(fd, Encode({"GET", "big"}));
  EXPECT_EQ(RecvExactly(fd, big.size() + 12),
            "$" + std::to_string(big.size()) + "\r\n" + big + "\r\n");
  ::close(fd);
  StopServer(&h);
}
