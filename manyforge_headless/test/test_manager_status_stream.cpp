// The Manager's status stream (OD-216), server side: the board and the
// writers. Real SEQPACKET socket pairs and a real listening socket; short
// real bounds; no forced deadlock, signal or resource exhaustion.

#include "../src/manager_status_stream.hpp"
#include "../src/scene_change_ring_internal.hpp"

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

// OD-302: a slower producer's older read never replaces a newer one.
TEST(ManagerStatusBoard, KeepsTheValueOfTheNewestOrderKey) {
  core::ExecutionEventChangeSignal signal;
  ManagerStatusBoard board(signal);
  constexpr auto kScene = ManagerStatusSection::kLogicalScene;
  constexpr auto kIndex = static_cast<std::size_t>(kScene);
  EXPECT_EQ(board.PublishOrdered(kScene, 2U, 5, Payload("newer")), 1U);
  board.NotifyIfChanged();
  const auto before = signal.Generation();
  // An older key with other bytes is refused: the newer value, its
  // generation and its time stay; the writers are not woken.
  EXPECT_EQ(board.PublishOrdered(kScene, 1U, 9, Payload("older")), 1U);
  // An equal key is refused too: one key is one read.
  EXPECT_EQ(board.PublishOrdered(kScene, 2U, 9, Payload("other")), 1U);
  const auto view = board.Load();
  EXPECT_EQ(*view->sections[kIndex].payload,
            content_identity::canonical_json(Payload("newer")));
  EXPECT_EQ(view->sections[kIndex].generation, 1U);
  EXPECT_EQ(view->sections[kIndex].observed_at_ns, 5);
  EXPECT_EQ(view->sections[kIndex].order, 2U);
  board.NotifyIfChanged();
  EXPECT_EQ(signal.Generation(), before);
  // A newer key with the same bytes keeps the generation and time and takes
  // the key, so a later-arriving read between the two is still refused.
  EXPECT_EQ(board.PublishOrdered(kScene, 4U, 11, Payload("newer")), 1U);
  EXPECT_EQ(board.Load()->sections[kIndex].order, 4U);
  EXPECT_EQ(board.Load()->sections[kIndex].observed_at_ns, 5);
  EXPECT_EQ(board.PublishOrdered(kScene, 3U, 12, Payload("between")), 1U);
  EXPECT_EQ(*board.Load()->sections[kIndex].payload,
            content_identity::canonical_json(Payload("newer")));
  board.NotifyIfChanged();
  EXPECT_EQ(signal.Generation(), before);
  // A newer key with other bytes raises the generation.
  EXPECT_EQ(board.PublishOrdered(kScene, 5U, 13, Payload("newest")), 2U);
  EXPECT_EQ(board.Load()->sections[kIndex].observed_at_ns, 13);
  // Sections keep their own keys.
  EXPECT_EQ(board.PublishOrdered(ManagerStatusSection::kSceneRuntime, 1U, 14,
                                 Payload("runtime")),
            1U);
}

// OD-302: producers racing on one section end with the value of the highest
// key, whatever order their swaps land in.
TEST(ManagerStatusBoard, ConcurrentOrderedProducersEndWithTheHighestKey) {
  core::ExecutionEventChangeSignal signal;
  ManagerStatusBoard board(signal);
  constexpr auto kRuntime = ManagerStatusSection::kSceneRuntime;
  constexpr std::uint64_t kPerThread = 500U;
  constexpr std::uint64_t kThreads = 4U;
  {
    std::vector<std::jthread> producers;
    for (std::uint64_t thread = 0U; thread < kThreads; ++thread) {
      producers.emplace_back([&board, thread] {
        // Interleaved keys: thread t publishes t+1, t+1+kThreads, ...
        for (std::uint64_t step = 0U; step < kPerThread; ++step) {
          const auto key = thread + 1U + step * kThreads;
          static_cast<void>(board.PublishOrdered(
              kRuntime, key, static_cast<std::int64_t>(key),
              Payload(std::to_string(key))));
        }
      });
    }
  }
  const auto &section =
      board.Load()->sections[static_cast<std::size_t>(kRuntime)];
  EXPECT_EQ(section.order, kThreads * kPerThread);
  EXPECT_EQ(*section.payload, content_identity::canonical_json(
                                  Payload(std::to_string(kThreads * kPerThread))));
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
            "manyforge.manager_status_stream.v2");
  EXPECT_EQ(snapshot->find_object_member("application_start_id")->as_string(), kStart);
  EXPECT_EQ(Tick(*snapshot), 1U);
  const auto *generations = snapshot->find_object_member("generations");
  EXPECT_EQ(generations->find_object_member("manager")->as_number(), 1.0);
  EXPECT_EQ(generations->find_object_member("logical_scene")->as_number(), 0.0);
  EXPECT_EQ(generations->find_object_member("scene_runtime")->as_number(), 1.0);
  EXPECT_EQ(generations->find_object_member("viewer_pose_sessions")->as_number(), 0.0);
  EXPECT_EQ(generations->find_object_member("scene_changes")->as_number(), 0.0);
  EXPECT_EQ(generations->as_object().size(), 5U);
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

// OD-400: `scene_changes` is published as encoded bytes and sent as an
// ordinary section frame, after the other four.
TEST(ManagerStatusStream, SendsAnEncodedSceneChangesValueAsASectionFrame) {
  Fixture fixture;
  const auto records = std::make_shared<const std::string>(
      R"({"records":[{"sequence":1}]})");
  EXPECT_EQ(fixture.board.PublishEncoded(ManagerStatusSection::kSceneChanges, 3,
                                         records),
            1U);
  // The same bytes again keep the generation.
  EXPECT_EQ(fixture.board.PublishEncoded(
                ManagerStatusSection::kSceneChanges, 4,
                std::make_shared<const std::string>(*records)),
            1U);
  fixture.Publish(ManagerStatusSection::kViewerPoseSessions, "viewer");
  const auto client = fixture.Subscribe();
  const auto snapshot = Receive(client.get());
  ASSERT_TRUE(snapshot.has_value());
  const auto *generations = snapshot->find_object_member("generations");
  ASSERT_NE(generations, nullptr);
  ASSERT_EQ(generations->as_object().size(), 5U);
  EXPECT_EQ(generations->find_object_member("scene_changes")->as_number(), 1.0);
  const auto viewer = Receive(client.get());
  ASSERT_TRUE(viewer.has_value());
  EXPECT_EQ(Name(*viewer), "viewer_pose_sessions");
  const auto changes = Receive(client.get());
  ASSERT_TRUE(changes.has_value());
  EXPECT_EQ(Type(*changes), "section");
  EXPECT_EQ(Name(*changes), "scene_changes");
  EXPECT_EQ(Generation(*changes), 1U);
  EXPECT_EQ(Value(*changes).find_object_member("observed_at_ns")->as_number(), 3.0);
  EXPECT_EQ(content_identity::canonical_json(
                *Value(*changes).find_object_member("payload")),
            *records);
  // A new value is sent at once.
  static_cast<void>(fixture.board.PublishEncoded(
      ManagerStatusSection::kSceneChanges, 5,
      std::make_shared<const std::string>(
          R"({"records":[{"sequence":1},{"sequence":2}]})")));
  fixture.board.NotifyIfChanged();
  const auto next = Receive(client.get());
  ASSERT_TRUE(next.has_value());
  EXPECT_EQ(Name(*next), "scene_changes");
  EXPECT_EQ(Generation(*next), 2U);
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


// ---------------------------------------------------------------------------
// The `scene_changes` section's records and ring (OD-400).
// ---------------------------------------------------------------------------

core::SceneTransactionRequestDTO SceneRequest(core::SceneOperationRequestDTO operation) {
  core::SceneTransactionRequestDTO request{
      core::PublicRequestContextDTO{core::RequestKey{}, "composer_ui"},
      {},
      {},
      std::move(operation),
      std::nullopt,
  };
  return request;
}

core::SceneTransactionRequestDTO AddObjectRequest() {
  return SceneRequest(core::AddObjectRequestDTO{
      core::SceneResourceId("box_1"),
      core::BoxShapeDTO{{0.1, 0.2, 0.3}},
      {},
      core::CellSceneFrameRefDTO{core::CellId("cell-a")},
  });
}

std::vector<double> Sequences(const std::string &payload) {
  const auto value = parse_canonical_json(payload);
  EXPECT_EQ(value.as_object().size(), 1U);
  std::vector<double> sequences;
  for (const auto &record : value.find_object_member("records")->as_array())
    sequences.push_back(record.find_object_member("sequence")->as_number());
  return sequences;
}

TEST(SceneChangeRing, KeepsTheLastRecordsOldestFirstWithConsecutiveSequences) {
  detail::SceneChangeRing ring;
  const auto request = AddObjectRequest();
  EXPECT_EQ(ring.next_sequence(), 1U);
  const auto first = ring.Add(detail::SceneChangeRecord(
      ring.next_sequence(), request, "succeeded", nullptr, 1U, 1.0));
  EXPECT_EQ(Sequences(*first), (std::vector<double>{1.0}));
  std::shared_ptr<const std::string> payload;
  for (std::uint64_t added = 2U; added <= 40U; ++added)
    payload = ring.Add(detail::SceneChangeRecord(
        ring.next_sequence(), request, "succeeded", nullptr, added, 1.0));
  EXPECT_EQ(ring.next_sequence(), 41U);
  std::vector<double> expected;
  for (std::uint64_t sequence = 9U; sequence <= 40U; ++sequence)
    expected.push_back(static_cast<double>(sequence));
  ASSERT_EQ(expected.size(), kSceneChangeRingCapacity);
  EXPECT_EQ(Sequences(*payload), expected);
  // The payload is already canonical: the board stores it as sent.
  EXPECT_EQ(content_identity::canonical_json(parse_canonical_json(*payload)),
            *payload);
}

TEST(SceneChangeRecord, ASucceededAddCarriesItsRevisionAndCaller) {
  auto request = AddObjectRequest();
  request.request_id = "req-1";
  request.request_transport = "http";
  EXPECT_EQ(detail::SceneChangeRecord(7U, request, "succeeded", nullptr, 4U, 2.5),
            R"({"duration_ms":2.5,"error":null,"operation_kind":"add_object",)"
            R"("outcome":"succeeded","principal_id":null,"request_id":"req-1",)"
            R"("scene_resource_id":"box_1","scene_revision_id":4,"sequence":7,)"
            R"("source_label":"composer_ui","transport":"http"})");
}

TEST(SceneChangeRecord, ARefusedDetachNamesNoObjectAndCarriesItsTypedError) {
  auto request = SceneRequest(core::DetachCarriedObjectRequestDTO{
      core::CarriedObjectAttachmentId("attachment.1"),
      core::CarriedObjectAttachmentRevision("revision.1"),
      {},
      core::CellSceneFrameRefDTO{core::CellId("cell-a")},
  });
  request.attribution.principal_id = "operator.7";
  const core::ErrorDTO error{"carried_object_not_found", "No carried object",
                             {{"attachment_id", "attachment.1"}}};
  EXPECT_EQ(detail::SceneChangeRecord(1U, request, "refused", &error,
                                      std::nullopt, 0.0),
            R"({"duration_ms":0,"error":{"code":"carried_object_not_found",)"
            R"("details":{"attachment_id":"attachment.1"},)"
            R"("message":"No carried object"},)"
            R"("operation_kind":"detach_carried_object","outcome":"refused",)"
            R"("principal_id":"operator.7","request_id":null,)"
            R"("scene_resource_id":null,"scene_revision_id":null,"sequence":1,)"
            R"("source_label":"composer_ui","transport":null})");
}

TEST(SceneChangeRecord, BoundsTheMessageAtACodePointAndLeavesOutLongDetails) {
  // "\xE2\x82\xAC" (3 bytes) straddles byte 2048: the cut ends before it.
  const auto message = std::string(2047U, 'a') + "\xE2\x82\xAC" + "tail";
  EXPECT_EQ(detail::SceneChangeText(message, detail::kSceneChangeTextBytes),
            std::string(2047U, 'a'));
  EXPECT_EQ(detail::SceneChangeText("short", detail::kSceneChangeTextBytes),
            "short");
  const core::ErrorDTO error{"scene_commit_failed", message,
                             {{"blob", std::string(2100U, 'x')}}};
  const auto record = parse_canonical_json(detail::SceneChangeRecord(
      2U, AddObjectRequest(), "unknown", &error, std::nullopt, 1.0));
  const auto &encoded = *record.find_object_member("error");
  EXPECT_EQ(encoded.as_object().size(), 2U);
  EXPECT_EQ(encoded.find_object_member("message")->as_string(),
            std::string(2047U, 'a'));
  EXPECT_EQ(encoded.find_object_member("details"), nullptr);
  EXPECT_EQ(record.find_object_member("outcome")->as_string(), "unknown");
}

TEST(SceneChangeRecord, TheWholeSectionOfLargestRecordsAlwaysFitsOneFrame) {
  // Every text at its bound in its most expensive encoding (control bytes
  // are 6 bytes each, quotes and backslashes 2), details at their bound and
  // the longest numbers: the static bound holds, so the section is never
  // oversized (OD-400).
  static_assert(detail::kSceneChangesPayloadMaximumBytes + 1024U <=
                ManagerStatusStream::kMaximumFrameBytes);
  auto request = AddObjectRequest();
  request.request_context.source_label = std::string(400U, '\x01');
  request.attribution.principal_id = std::string(400U, '"');
  request.request_id = std::string(80U, 'r');
  request.request_transport = "http";
  std::get<core::AddObjectRequestDTO>(request.operation).scene_resource_id =
      core::SceneResourceId(std::string(400U, '\\'));
  const core::ErrorDTO error{std::string(400U, '\x01'), std::string(5000U, '\x01'),
                             {{"k", std::string(2040U, 'x')}}};
  detail::SceneChangeRing ring;
  std::shared_ptr<const std::string> payload;
  constexpr std::uint64_t kLargest = 9007199254740991ULL;
  for (std::uint64_t index = 0U; index < 40U; ++index) {
    auto record = detail::SceneChangeRecord(kLargest - 40U + index, request,
                                            "succeeded", &error, kLargest,
                                            -1.7976931348623157e308);
    EXPECT_LE(record.size(), detail::kSceneChangeRecordMaximumBytes);
    payload = ring.Add(std::move(record));
  }
  EXPECT_LE(payload->size(), detail::kSceneChangesPayloadMaximumBytes);
  const auto value = parse_canonical_json(*payload);
  const auto &records = value.find_object_member("records")->as_array();
  ASSERT_EQ(records.size(), kSceneChangeRingCapacity);
  const auto &first = records.front();
  // The details at their bound are kept; each text is cut to its bound.
  EXPECT_NE(first.find_object_member("error")->find_object_member("details"),
            nullptr);
  EXPECT_EQ(first.find_object_member("request_id")->as_string().size(), 64U);
  EXPECT_EQ(first.find_object_member("source_label")->as_string().size(),
            detail::kSceneChangeNameBytes / 6U);
  EXPECT_EQ(first.find_object_member("principal_id")->as_string().size(),
            detail::kSceneChangeNameBytes / 2U);
  EXPECT_EQ(first.find_object_member("error")
                ->find_object_member("message")
                ->as_string()
                .size(),
            detail::kSceneChangeTextBytes / 6U);
}

} // namespace
} // namespace manyforge::headless
