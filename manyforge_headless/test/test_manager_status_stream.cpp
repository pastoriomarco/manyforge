// The Manager's status stream (OD-216), server side: the board and the
// writers. Real SEQPACKET socket pairs and a real listening socket; short
// real bounds; no forced deadlock, signal or resource exhaustion.

#include "../src/manager_status_stream.hpp"

#include <gtest/gtest.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
using content_identity::parse_canonical_json;
using private_stream::Fd;
using namespace std::chrono_literals;

constexpr std::string_view kStart = "0123456789abcdef";

struct Pair final {
  Fd server;
  Fd client;
};

Pair MakePair() {
  int ends[2]{-1, -1};
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ends), 0);
  return Pair{Fd(ends[0]), Fd(ends[1])};
}

/// The next frame, or nullopt on timeout or EOF.
std::optional<CanonicalValue> Receive(const int fd,
                                      const std::chrono::milliseconds timeout = 2000ms) {
  pollfd inbound{fd, POLLIN, 0};
  if (::poll(&inbound, 1, static_cast<int>(timeout.count())) <= 0)
    return std::nullopt;
  std::string buffer(1U << 20U, '\0');
  const auto count = ::recv(fd, buffer.data(), buffer.size(), 0);
  if (count <= 0)
    return std::nullopt;
  buffer.resize(static_cast<std::size_t>(count));
  return parse_canonical_json(buffer, content_identity::CanonicalJsonParseLimits{
                                          content_identity::kMaximumCanonicalJsonBytes,
                                          content_identity::kMaximumCanonicalJsonDepth,
                                          content_identity::kMaximumCanonicalJsonNodes});
}

std::string Type(const CanonicalValue &frame) {
  return frame.find_object_member("type")->as_string();
}

std::uint64_t Tick(const CanonicalValue &frame) {
  return static_cast<std::uint64_t>(frame.find_object_member("tick")->as_number());
}

std::string Name(const CanonicalValue &frame) {
  return frame.find_object_member("name")->as_string();
}

const CanonicalValue &Value(const CanonicalValue &frame) {
  return *frame.find_object_member("section");
}

std::uint64_t Generation(const CanonicalValue &frame) {
  return static_cast<std::uint64_t>(
      Value(frame).find_object_member("generation")->as_number());
}

CanonicalValue Payload(const std::string &text) {
  return CanonicalValue(CanonicalValue::Object{{"text", CanonicalValue(text)}});
}

/// One board, its stream and one writer per subscriber on socket pairs.
struct Fixture final {
  core::ExecutionEventChangeSignal signal;
  ManagerStatusBoard board{signal};
  ManagerStatusStream stream{board, std::string(kStart)};
  std::vector<std::jthread> writers;

  Fd Subscribe() {
    auto pair = MakePair();
    writers.emplace_back([this, server = std::make_shared<Fd>(std::move(pair.server))](
                             const std::stop_token stop) {
      stream.Serve(server->get(), stop);
    });
    return std::move(pair.client);
  }

  void Publish(const ManagerStatusSection section, const std::string &text,
               const std::int64_t observed_at_ns = 7) {
    static_cast<void>(board.Publish(section, observed_at_ns, Payload(text)));
    board.NotifyIfChanged();
  }

  void Tick() {
    board.AdvanceTick();
    board.NotifyIfChanged();
  }
};

TEST(ManagerStatusBoard, RaisesTheGenerationOnlyWhenTheBytesChange) {
  core::ExecutionEventChangeSignal signal;
  ManagerStatusBoard board(signal);
  EXPECT_EQ(board.Publish(ManagerStatusSection::kManager, 5, Payload("a")), 1U);
  EXPECT_EQ(board.Publish(ManagerStatusSection::kManager, 9, Payload("a")), 1U);
  // An unchanged value keeps its time too.
  EXPECT_EQ(board.Load()->sections[0].observed_at_ns, 5);
  EXPECT_EQ(board.Publish(ManagerStatusSection::kManager, 11, Payload("b")), 2U);
  EXPECT_EQ(board.Load()->sections[0].observed_at_ns, 11);
  EXPECT_EQ(board.Load()->sections[1].generation, 0U);
  const auto before = signal.Generation();
  board.NotifyIfChanged();
  EXPECT_EQ(signal.Generation(), before + 1U);
  board.NotifyIfChanged();  // Nothing changed since.
  EXPECT_EQ(signal.Generation(), before + 1U);
}

TEST(ManagerStatusStream, SendsTheFullStateAtConnect) {
  Fixture fixture;
  fixture.Publish(ManagerStatusSection::kManager, "manager");
  fixture.Publish(ManagerStatusSection::kSceneRuntime, "runtime");
  fixture.Tick();
  const auto client = fixture.Subscribe();
  const auto snapshot = Receive(client.get());
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(Type(*snapshot), "snapshot");
  EXPECT_EQ(snapshot->find_object_member("api_version")->as_string(),
            "manyforge.manager_status_stream.v1");
  EXPECT_EQ(snapshot->find_object_member("application_start_id")->as_string(), kStart);
  EXPECT_EQ(Tick(*snapshot), 1U);
  const auto *generations = snapshot->find_object_member("generations");
  EXPECT_EQ(generations->find_object_member("manager")->as_number(), 1.0);
  EXPECT_EQ(generations->find_object_member("logical_scene")->as_number(), 0.0);
  EXPECT_EQ(generations->find_object_member("scene_runtime")->as_number(), 1.0);
  EXPECT_EQ(generations->find_object_member("viewer_pose_sessions")->as_number(), 0.0);
  const auto manager = Receive(client.get());
  ASSERT_TRUE(manager.has_value());
  EXPECT_EQ(Type(*manager), "section");
  EXPECT_EQ(Name(*manager), "manager");
  EXPECT_EQ(Generation(*manager), 1U);
  EXPECT_FALSE(Value(*manager).find_object_member("oversized")->as_bool());
  EXPECT_EQ(*Value(*manager).find_object_member("payload"), Payload("manager"));
  EXPECT_EQ(Value(*manager).find_object_member("observed_at_ns")->as_number(), 7.0);
  const auto runtime = Receive(client.get());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(Name(*runtime), "scene_runtime");
  // Never published sections are not sent; nothing else follows.
  EXPECT_FALSE(Receive(client.get(), 150ms).has_value());
}

TEST(ManagerStatusStream, SendsAPartWhenItChangesAndNotOtherwise) {
  Fixture fixture;
  fixture.Publish(ManagerStatusSection::kLogicalScene, "scene-1");
  const auto client = fixture.Subscribe();
  ASSERT_EQ(Type(*Receive(client.get())), "snapshot");
  ASSERT_EQ(Name(*Receive(client.get())), "logical_scene");
  // The same bytes again: no frame (only a tick would send a keepalive).
  fixture.Publish(ManagerStatusSection::kLogicalScene, "scene-1");
  EXPECT_FALSE(Receive(client.get(), 150ms).has_value());
  // A change arrives without waiting for a tick, with the same tick.
  const auto published = std::chrono::steady_clock::now();
  fixture.Publish(ManagerStatusSection::kLogicalScene, "scene-2");
  const auto changed = Receive(client.get());
  ASSERT_TRUE(changed.has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - published, 500ms);
  EXPECT_EQ(Name(*changed), "logical_scene");
  EXPECT_EQ(Generation(*changed), 2U);
  EXPECT_EQ(Tick(*changed), 0U);
  // A tick alone sends one keepalive with the new tick.
  fixture.Tick();
  const auto keepalive = Receive(client.get());
  ASSERT_TRUE(keepalive.has_value());
  EXPECT_EQ(Type(*keepalive), "keepalive");
  EXPECT_EQ(Tick(*keepalive), 1U);
  EXPECT_EQ(keepalive->as_object().size(), 4U);
  EXPECT_FALSE(Receive(client.get(), 150ms).has_value());
}

TEST(ManagerStatusStream, ALateReaderGetsTheCurrentStateOnly) {
  Fixture fixture;
  for (int index = 0; index < 5; ++index)
    fixture.Publish(ManagerStatusSection::kViewerPoseSessions,
                    "sessions-" + std::to_string(index));
  fixture.Tick();
  fixture.Tick();
  const auto client = fixture.Subscribe();
  const auto snapshot = Receive(client.get());
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(Tick(*snapshot), 2U);
  const auto sessions = Receive(client.get());
  ASSERT_TRUE(sessions.has_value());
  EXPECT_EQ(Generation(*sessions), 5U);
  EXPECT_EQ(*Value(*sessions).find_object_member("payload"), Payload("sessions-4"));
  EXPECT_FALSE(Receive(client.get(), 150ms).has_value());
}

TEST(ManagerStatusStream, AReconnectingReaderGetsTheNewState) {
  Fixture fixture;
  fixture.Publish(ManagerStatusSection::kManager, "before");
  {
    const auto first = fixture.Subscribe();
    ASSERT_EQ(Type(*Receive(first.get())), "snapshot");
    ASSERT_EQ(Generation(*Receive(first.get())), 1U);
  }  // The reader hangs up.
  fixture.Publish(ManagerStatusSection::kManager, "after");
  const auto second = fixture.Subscribe();
  ASSERT_EQ(Type(*Receive(second.get())), "snapshot");
  const auto manager = Receive(second.get());
  ASSERT_TRUE(manager.has_value());
  EXPECT_EQ(Generation(*manager), 2U);
  EXPECT_EQ(*Value(*manager).find_object_member("payload"), Payload("after"));
}

TEST(ManagerStatusStream, AnOversizedSectionIsSentAsUnavailableAndTheStreamGoesOn) {
  Fixture fixture;
  fixture.Publish(ManagerStatusSection::kLogicalScene,
                  std::string(ManagerStatusStream::kMaximumFrameBytes, 'x'));
  fixture.Publish(ManagerStatusSection::kSceneRuntime, "small");
  const auto client = fixture.Subscribe();
  ASSERT_EQ(Type(*Receive(client.get())), "snapshot");
  const auto scene = Receive(client.get());
  ASSERT_TRUE(scene.has_value());
  EXPECT_EQ(Name(*scene), "logical_scene");
  EXPECT_EQ(Generation(*scene), 1U);
  EXPECT_TRUE(Value(*scene).find_object_member("oversized")->as_bool());
  EXPECT_TRUE(Value(*scene).find_object_member("payload")->is_null());
  const auto runtime = Receive(client.get());
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(Name(*runtime), "scene_runtime");
  EXPECT_FALSE(Value(*runtime).find_object_member("oversized")->as_bool());
}

TEST(ManagerStatusStream, AReaderThatNeverReadsDelaysNeitherProducersNorOtherReaders) {
  Fixture fixture;
  const auto silent = fixture.Subscribe();  // Never reads.
  const auto reader = fixture.Subscribe();
  ASSERT_EQ(Type(*Receive(reader.get())), "snapshot");
  // Large changes fill the silent reader's buffer within a few frames.
  const std::string large(300U * 1024U, 'p');
  auto slowest = std::chrono::steady_clock::duration::zero();
  std::uint64_t last_tick = 0U;
  const auto started = std::chrono::steady_clock::now();
  for (int round = 0; round < 12; ++round) {
    const auto before = std::chrono::steady_clock::now();
    fixture.Publish(ManagerStatusSection::kLogicalScene, large + std::to_string(round));
    fixture.Tick();
    slowest = std::max(slowest, std::chrono::steady_clock::now() - before);
    // The reading subscriber sees every round's tick.
    for (;;) {
      const auto frame = Receive(reader.get());
      ASSERT_TRUE(frame.has_value());
      last_tick = std::max(last_tick, Tick(*frame));
      if (last_tick == static_cast<std::uint64_t>(round + 1))
        break;
    }
    std::this_thread::sleep_for(250ms);
  }
  // Producers do memory work only, whatever the readers do.
  EXPECT_LT(slowest, 50ms);
  // The silent reader was dropped after the 2 s send deadline: its socket
  // reports the hang-up once its buffer is drained.
  ASSERT_GE(std::chrono::steady_clock::now() - started, 2500ms);
  bool closed = false;
  for (int index = 0; index < 64 && !closed; ++index)
    closed = !Receive(silent.get(), 500ms).has_value();
  EXPECT_TRUE(closed);
}

/// A listening socket in a fresh directory, as the Manager's run root holds.
struct Listener final {
  std::filesystem::path directory;
  Fd socket;
};

Listener Listen() {
  char pattern[] = "/tmp/manyforge-manager-status-XXXXXX";
  const char *created = ::mkdtemp(pattern);
  EXPECT_NE(created, nullptr);
  Listener listener{created, Fd(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0))};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = (listener.directory / std::string(kManagerStatusSocketFileName)).string();
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  EXPECT_EQ(::bind(listener.socket.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  EXPECT_EQ(::listen(listener.socket.get(), 16), 0);
  return listener;
}

Fd Connect(const Listener &listener) {
  Fd client(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = (listener.directory / std::string(kManagerStatusSocketFileName)).string();
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  EXPECT_EQ(::connect(client.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  return client;
}

TEST(ManagerStatusStream, AFifthSubscriberIsRefusedAndStopEndsEverySubscription) {
  Fixture fixture;
  auto listener = Listen();
  std::vector<Fd> clients;
  {
    std::jthread acceptor([&](const std::stop_token stop) {
      fixture.stream.Run(listener.socket.get(), stop);
    });
    for (std::size_t index = 0; index < ManagerStatusStream::kMaximumSubscribers; ++index) {
      clients.push_back(Connect(listener));
      const auto snapshot = Receive(clients.back().get());
      ASSERT_TRUE(snapshot.has_value());
      EXPECT_EQ(Type(*snapshot), "snapshot");
    }
    const auto refused = Connect(listener);
    const auto error = Receive(refused.get());
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(Type(*error), "error");
    EXPECT_EQ(error->find_object_member("error")->find_object_member("code")->as_string(),
              "manager_status_subscribers_full");
    // The four keep their streams.
    fixture.Tick();
    for (const auto &client : clients)
      EXPECT_EQ(Type(*Receive(client.get())), "keepalive");
  }  // Stop: the acceptor ends every writer before it returns.
  for (const auto &client : clients)
    EXPECT_FALSE(Receive(client.get(), 300ms).has_value());
  std::filesystem::remove_all(listener.directory);
}

} // namespace
} // namespace manyforge::headless
