#include "../src/manager_local_exchange_internal.hpp"

#include <gtest/gtest.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace manyforge::headless::detail {
namespace {

// The Manager's Kernel Stop control and Program Runner exchanges have no time
// limit (OD-296 (a), OD-306 (a)): a wait ends on the reply, on the peer
// closing the connection or on the caller's application stop, never because
// time passed.

struct Pair final {
  Pair() {
    int fds[2]{-1, -1};
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds), 0);
    manager = fds[0];
    peer = fds[1];
  }
  ~Pair() {
    if (manager >= 0) ::close(manager);
    if (peer >= 0) ::close(peer);
  }
  int manager{-1};
  int peer{-1};
};

struct Event final {
  Event() : fd(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)) {}
  ~Event() {
    if (fd >= 0) ::close(fd);
  }
  void Write() const {
    const std::uint64_t one = 1U;
    EXPECT_EQ(::write(fd, &one, sizeof(one)),
              static_cast<ssize_t>(sizeof(one)));
  }
  int fd;
};

std::atomic<bool> g_forced{false};
bool Forced() noexcept { return g_forced.load(); }

TEST(ManagerLocalExchange, EndsOnTheReplyAfterASlowPeer) {
  Pair pair;
  Event stop;
  // The peer answers from another thread whenever it gets to it; the wait
  // has no limit and returns on the reply.
  std::thread peer([&] {
    char request = 0;
    ASSERT_EQ(::recv(pair.peer, &request, 1U, 0), 1);
    ASSERT_EQ(::send(pair.peer, "r", 1U, MSG_NOSIGNAL), 1);
  });
  ASSERT_EQ(::send(pair.manager, "q", 1U, MSG_NOSIGNAL), 1);
  EXPECT_EQ(WaitForLocalPeer(pair.manager, POLLIN, stop.fd),
            LocalPeerWaitEnd::kReady);
  peer.join();
}

TEST(ManagerLocalExchange, EndsWhenThePeerClosesTheConnection) {
  Pair pair;
  Event stop;
  std::thread peer([&] {
    ::close(pair.peer);
    pair.peer = -1;
  });
  // A closed SEQPACKET peer is readable (end of stream) as well as hung up:
  // the wait ends as ready so a reply sent just before the close is still
  // read; the read that follows returns 0, which the exchange reports as the
  // peer having closed. It never waits for the stop.
  const auto ended = WaitForLocalPeer(pair.manager, POLLIN, stop.fd);
  EXPECT_TRUE(ended == LocalPeerWaitEnd::kReady ||
              ended == LocalPeerWaitEnd::kPeerClosed);
  peer.join();
  char byte = 0;
  EXPECT_EQ(::recv(pair.manager, &byte, 1U, MSG_DONTWAIT), 0);
}

TEST(ManagerLocalExchange, EndsOnTheApplicationStopSignal) {
  Pair pair;
  Event stop;
  // The peer stays and never answers; the abandon eventfd ends the wait.
  std::thread stopper([&] { stop.Write(); });
  EXPECT_EQ(WaitForLocalPeer(pair.manager, POLLIN, stop.fd),
            LocalPeerWaitEnd::kStopped);
  stopper.join();
}

TEST(ManagerLocalExchange, HaltWaitEndsOnlyOnTheForcedStopNotOnItsSlices) {
  Pair pair;
  // The application-stop Halt: no stop fd (the abandon signal is already
  // written), the forced stop is re-checked between slices. Slices pass
  // without ending the wait until the reply comes.
  g_forced.store(false);
  std::thread peer([&] {
    char request = 0;
    ASSERT_EQ(::recv(pair.peer, &request, 1U, 0), 1);
    ASSERT_EQ(::send(pair.peer, "r", 1U, MSG_NOSIGNAL), 1);
  });
  ASSERT_EQ(::send(pair.manager, "q", 1U, MSG_NOSIGNAL), 1);
  EXPECT_EQ(WaitForLocalPeer(pair.manager, POLLIN, -1, &Forced),
            LocalPeerWaitEnd::kReady);
  peer.join();
  char reply = 0;
  ASSERT_EQ(::recv(pair.manager, &reply, 1U, 0), 1);  // The reply is consumed.

  // A second stop request forces the stop: the wait on a silent peer ends.
  std::thread forcer([] { g_forced.store(true); });
  EXPECT_EQ(WaitForLocalPeer(pair.manager, POLLIN, -1, &Forced),
            LocalPeerWaitEnd::kStopped);
  forcer.join();
  g_forced.store(false);
}

}  // namespace
}  // namespace manyforge::headless::detail
