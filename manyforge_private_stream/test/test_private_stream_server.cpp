// Shared private stream server parts: the bound, deadline, cap and codes a
// caller passes are the ones applied. Real SEQPACKET socket pairs, short real
// bounds.

#include <manyforge_private_stream/private_stream_server.hpp>

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using manyforge::content_identity::CanonicalValue;
using manyforge::content_identity::canonical_json;
using manyforge::content_identity::parse_canonical_json;
using manyforge::private_stream::AcceptSubscribers;
using manyforge::private_stream::ErrorFrame;
using manyforge::private_stream::Fd;
using manyforge::private_stream::PeerIsSameUser;
using manyforge::private_stream::SendFrame;
using manyforge::private_stream::SendFrameBytes;
using manyforge::private_stream::SubscriberPool;
using manyforge::private_stream::SubscriberPoolOptions;
using namespace std::chrono_literals;

struct Pair final {
  Fd server;
  Fd client;
};

Pair MakePair() {
  int ends[2]{-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends), 0);
  return Pair{Fd(ends[0]), Fd(ends[1])};
}

/// The next packet, or nullopt on timeout or EOF.
std::optional<CanonicalValue> Receive(const int fd,
                                      const std::chrono::milliseconds timeout = 2000ms) {
  pollfd inbound{fd, POLLIN, 0};
  if (::poll(&inbound, 1, static_cast<int>(timeout.count())) <= 0) return std::nullopt;
  std::string buffer(1U << 16U, '\0');
  const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
  if (count <= 0) return std::nullopt;
  buffer.resize(static_cast<std::size_t>(count));
  return parse_canonical_json(buffer);
}

CanonicalValue Frame(const std::string &text) {
  return CanonicalValue(CanonicalValue::Object{{"text", CanonicalValue(text)}});
}

/// Fills `fd`'s send capacity with small packets.
void Fill(const int fd) {
  const std::string packet(512U, 'f');
  while (::send(fd, packet.data(), packet.size(), MSG_NOSIGNAL | MSG_DONTWAIT) > 0) {
  }
}

TEST(SendFrame, AppliesTheBoundPassedIn) {
  auto pair = MakePair();
  const auto frame = Frame(std::string(100U, 'a'));
  const auto size = canonical_json(frame).size();
  EXPECT_FALSE(SendFrame(pair.server.get(), frame, size - 1U, 1000ms, {}));
  EXPECT_TRUE(SendFrame(pair.server.get(), frame, size, 1000ms, {}));
  const auto received = Receive(pair.client.get());
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(canonical_json(*received), canonical_json(frame));  // Only the one in bound.
  EXPECT_FALSE(Receive(pair.client.get(), 100ms).has_value());
}

TEST(SendFrame, GivesUpAtTheDeadlinePassedIn) {
  auto pair = MakePair();
  Fill(pair.server.get());
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(SendFrame(pair.server.get(), Frame("late"), 1U << 20U, 300ms, {}));
  const auto waited = std::chrono::steady_clock::now() - started;
  EXPECT_GE(waited, 300ms);
  EXPECT_LT(waited, 300ms + 100ms + 200ms);  // One poll slice + slack.
}

TEST(SendFrame, StopEndsAWaitWithinOneSlice) {
  auto pair = MakePair();
  Fill(pair.server.get());
  std::stop_source stop;
  std::jthread stopper([&stop] {
    std::this_thread::sleep_for(50ms);
    stop.request_stop();
  });
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(SendFrame(pair.server.get(), Frame("stopped"), 1U << 20U, 5000ms,
                         stop.get_token()));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 50ms + 100ms + 200ms);
}

TEST(SendFrame, AClosedPeerEndsAtOnce) {
  auto pair = MakePair();
  { Fd closed(std::move(pair.client)); }
  EXPECT_FALSE(SendFrame(pair.server.get(), Frame("gone"), 1U << 20U, 5000ms, {}));
}

TEST(SendFrameBytes, SendsTheEncodedFrameWithinTheBoundPassedIn) {
  auto pair = MakePair();
  const std::string frame = canonical_json(Frame("bytes"));
  EXPECT_FALSE(SendFrameBytes(pair.server.get(), frame, frame.size() - 1U, 1000ms, {}));
  EXPECT_FALSE(SendFrameBytes(pair.server.get(), "", 1U << 20U, 1000ms, {}));
  EXPECT_TRUE(SendFrameBytes(pair.server.get(), frame, frame.size(), 1000ms, {}));
  const auto received = Receive(pair.client.get());
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(canonical_json(*received), frame);
  EXPECT_FALSE(Receive(pair.client.get(), 100ms).has_value());
}

TEST(ErrorFrame, CarriesTheVersionCodeAndCutMessage) {
  const auto frame = ErrorFrame("example.stream.v1", "example_code", std::string(2000U, 'm'));
  EXPECT_EQ(frame.find_object_member("api_version")->as_string(), "example.stream.v1");
  EXPECT_EQ(frame.find_object_member("type")->as_string(), "error");
  const auto *error = frame.find_object_member("error");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->find_object_member("code")->as_string(), "example_code");
  EXPECT_EQ(error->find_object_member("message")->as_string().size(), 1024U);
  EXPECT_TRUE(error->find_object_member("details")->as_object().empty());
}

TEST(PeerIsSameUser, AcceptsAPeerOfThisUser) {
  auto pair = MakePair();
  EXPECT_TRUE(PeerIsSameUser(pair.server.get()));
  EXPECT_FALSE(PeerIsSameUser(-1));
}

/// Serves until the client hangs up or `stop`; counts finished writers.
SubscriberPool::Serve Holding(std::atomic<int> &finished) {
  return [&finished](const int client, const std::stop_token stop) {
    static_cast<void>(SendFrame(client, Frame("served"), 1U << 20U, 1000ms, stop));
    while (!stop.stop_requested()) {
      pollfd hangup{client, 0, 0};
      if (::poll(&hangup, 1, 10) > 0) break;
    }
    finished.fetch_add(1);
  };
}

SubscriberPoolOptions Options(const std::size_t cap) {
  return SubscriberPoolOptions{cap, "example.stream.v1", "example_subscribers_full",
                               "the example serves its bounded number of subscriptions",
                               1U << 20U, 1000ms};
}

TEST(SubscriberPool, RefusesAboveTheCapWithTheCodePassedIn) {
  std::atomic<int> finished{0};
  SubscriberPool pool(Options(2));
  std::vector<Pair> served;
  for (int index = 0; index < 2; ++index) {
    served.push_back(MakePair());
    pool.Admit(std::move(served.back().server), Holding(finished), {});
    const auto frame = Receive(served.back().client.get());
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->find_object_member("text")->as_string(), "served");
  }
  auto refused = MakePair();
  pool.Admit(std::move(refused.server), Holding(finished), {});
  const auto error = Receive(refused.client.get());
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->find_object_member("api_version")->as_string(), "example.stream.v1");
  EXPECT_EQ(error->find_object_member("type")->as_string(), "error");
  EXPECT_EQ(error->find_object_member("error")->find_object_member("code")->as_string(),
            "example_subscribers_full");
  EXPECT_FALSE(Receive(refused.client.get(), 200ms).has_value());  // Closed.
}

SubscriberPoolOptions Options(const std::size_t cap,
                              const std::chrono::milliseconds refusal_read_deadline) {
  auto options = Options(cap);
  options.refusal_read_deadline = refusal_read_deadline;
  return options;
}

void SendSubscribe(const int fd) {
  const std::string request = R"({"command":"subscribe"})";
  ASSERT_EQ(::send(fd, request.data(), request.size(), MSG_NOSIGNAL),
            static_cast<ssize_t>(request.size()));
}

TEST(SubscriberPool, ARefusedClientsQueuedPacketIsReadSoTheFullErrorArrives) {
  std::atomic<int> finished{0};
  SubscriberPool pool(Options(0, 1000ms));
  auto refused = MakePair();
  SendSubscribe(refused.client.get());
  pool.Admit(std::move(refused.server), Holding(finished), {});
  const auto error = Receive(refused.client.get());
  ASSERT_TRUE(error.has_value());  // Not a reset.
  EXPECT_EQ(error->find_object_member("error")->find_object_member("code")->as_string(),
            "example_subscribers_full");
  EXPECT_FALSE(Receive(refused.client.get(), 200ms).has_value());  // Closed.
  EXPECT_EQ(finished.load(), 0);
}

TEST(SubscriberPool, TheRefusalReadWaitsAtMostItsDeadlineForASilentClient) {
  std::atomic<int> finished{0};
  SubscriberPool pool(Options(0, 300ms));
  auto refused = MakePair();
  const auto started = std::chrono::steady_clock::now();
  pool.Admit(std::move(refused.server), Holding(finished), {});
  const auto waited = std::chrono::steady_clock::now() - started;
  EXPECT_GE(waited, 300ms);
  EXPECT_LT(waited, 300ms + 200ms);
  const auto error = Receive(refused.client.get());
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->find_object_member("error")->find_object_member("code")->as_string(),
            "example_subscribers_full");
}

TEST(SubscriberPool, WithoutARefusalReadTheQueuedPacketStaysUnreadAsBefore) {
  std::atomic<int> finished{0};
  SubscriberPool pool(Options(0));
  auto refused = MakePair();
  SendSubscribe(refused.client.get());
  const auto started = std::chrono::steady_clock::now();
  pool.Admit(std::move(refused.server), Holding(finished), {});
  EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
  char buffer[256];
  errno = 0;
  EXPECT_LT(::recv(refused.client.get(), buffer, sizeof(buffer), MSG_DONTWAIT), 0);
  EXPECT_EQ(errno, ECONNRESET);  // The kernel reset; the error frame is lost.
}

TEST(SubscriberPool, ReapsFinishedWritersBeforeTheCapCheck) {
  std::atomic<int> finished{0};
  SubscriberPool pool(Options(1));
  auto first = MakePair();
  pool.Admit(std::move(first.server), Holding(finished), {});
  ASSERT_TRUE(Receive(first.client.get()).has_value());
  { Fd hung_up(std::move(first.client)); }
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (finished.load() == 0 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(5ms);
  ASSERT_EQ(finished.load(), 1);
  std::this_thread::sleep_for(20ms);  // The writer marks itself done after `serve`.
  auto second = MakePair();
  pool.Admit(std::move(second.server), Holding(finished), {});
  const auto frame = Receive(second.client.get());
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->find_object_member("text")->as_string(), "served");
}

TEST(SubscriberPool, DestructionStopsAndJoinsEveryWriter) {
  std::atomic<int> finished{0};
  auto first = MakePair();
  auto second = MakePair();
  {
    SubscriberPool pool(Options(4));
    pool.Admit(std::move(first.server), Holding(finished), {});
    pool.Admit(std::move(second.server), Holding(finished), {});
    ASSERT_TRUE(Receive(first.client.get()).has_value());
    ASSERT_TRUE(Receive(second.client.get()).has_value());
  }
  EXPECT_EQ(finished.load(), 2);
  EXPECT_FALSE(Receive(first.client.get(), 200ms).has_value());  // EOF.
  EXPECT_FALSE(Receive(second.client.get(), 200ms).has_value());
}

/// A listening SEQPACKET socket in a fresh directory.
struct Listener final {
  std::filesystem::path directory;
  Fd socket;
};

Listener Listen() {
  char pattern[] = "/tmp/manyforge-private-stream-XXXXXX";
  const char *created = ::mkdtemp(pattern);
  EXPECT_NE(created, nullptr);
  Listener listener{created, Fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0))};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = (listener.directory / "stream.sock").string();
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  EXPECT_EQ(::bind(listener.socket.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  EXPECT_EQ(::listen(listener.socket.get(), 16), 0);
  return listener;
}

Fd Connect(const Listener &listener) {
  Fd client(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = (listener.directory / "stream.sock").string();
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  EXPECT_EQ(::connect(client.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  return client;
}

TEST(AcceptSubscribers, ServesEachPeerUpToTheCapAndStopsEveryWriter) {
  auto listener = Listen();
  std::atomic<int> finished{0};
  std::optional<Fd> first;
  std::optional<Fd> second;
  {
    std::jthread acceptor([&](const std::stop_token stop) {
      AcceptSubscribers(listener.socket.get(), Options(1), Holding(finished), stop);
    });
    first.emplace(Connect(listener));
    const auto served = Receive(first->get());
    ASSERT_TRUE(served.has_value());
    EXPECT_EQ(served->find_object_member("text")->as_string(), "served");
    second.emplace(Connect(listener));
    const auto refused = Receive(second->get());
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->find_object_member("error")->find_object_member("code")->as_string(),
              "example_subscribers_full");
  }  // The acceptor's stop ends and joins the one writer.
  EXPECT_EQ(finished.load(), 1);
  EXPECT_FALSE(Receive(first->get(), 200ms).has_value());  // EOF.
  std::filesystem::remove_all(listener.directory);
}

} // namespace
