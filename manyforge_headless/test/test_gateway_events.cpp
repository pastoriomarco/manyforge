// The gateway core's event follower, mirrors, subscriptions and wake set,
// against a fake execution host on a temporary run root. No sleeps: the tests
// synchronize on the host's accept/recv and on the subscription's messages.

#include <gtest/gtest.h>

#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "manyforge_headless/gateway_events.hpp"

namespace manyforge::headless::gateway {
namespace {

using V = CanonicalValue;
using O = CanonicalValue::Object;
using A = CanonicalValue::Array;
using namespace std::chrono_literals;

V N(double number) { return V(number); }
V S(const char *text) { return V(text); }

V Message(const char *type, O fields) {
  fields.emplace_back("api_version", S("manyforge.execution_event_stream.v4"));
  fields.emplace_back("type", S(type));
  return V(std::move(fields));
}

V Entry(double operation_id, const char *state = "executing") {
  return V(O{{"control_pending", V(false)},
             {"last_control", V(nullptr)},
             {"operation", V(O{{"operation_id", N(operation_id)},
                               {"operation_revision", N(1)},
                               {"state", S(state)}})},
             {"operation_id", N(operation_id)}});
}

V Job(const char *job_id, const char *state, V run = N(3)) {
  return V(O{{"job_id", S(job_id)},
             {"kind", S("pick")},
             {"program_run_id", std::move(run)},
             {"state", S(state)}});
}

V State(double change, A entries = {}, A jobs = {},
        std::optional<double> total = std::nullopt) {
  const double resident_total =
      total ? *total : static_cast<double>(entries.size());
  return V(O{{"active_tcp_baseline", V(A{V(O{{"robot", S("arm")}})})},
             {"operations", V(O{{"change", N(change)},
                                {"faulted", V(A{})},
                                {"faulted_total", N(0)},
                                {"resident", V(std::move(entries))},
                                {"resident_total", N(resident_total)}})},
             {"planning_jobs", V(std::move(jobs))}});
}

V Snapshot(double cursor, V state = State(0)) {
  return Message("snapshot", O{{"cursor", N(cursor)},
                               {"reason", S("subscribed")},
                               {"state", std::move(state)}});
}
V StateMessage(V state) { return Message("state", O{{"state", std::move(state)}}); }
V Event(double sequence) {
  return Message("event", O{{"event", V(O{{"payload", V(O{})},
                                          {"record_kind", S("operation_terminal")},
                                          {"sequence", N(sequence)}})}});
}
V Heartbeat(double cursor) { return Message("heartbeat", O{{"cursor", N(cursor)}}); }
V Progress(A rows) {
  return Message("progress", O{{"operation_progress", V(std::move(rows))}});
}

std::string Type(const EventMessage &message) {
  EXPECT_NE(message, nullptr);
  if (message == nullptr) return "<none>";
  return message->find_object_member("type")->as_string();
}

std::string Code(const EventMessage &message) {
  if (Type(message) != "disconnected") return "<not disconnected>";
  return message->find_object_member("code")->as_string();
}

/// One accepted subscriber connection of the fake host.
class Connection {
 public:
  explicit Connection(int fd) : fd_(fd) {}
  ~Connection() { Close(); }
  /// The subscribe packet's `after_cursor` (checks the whole packet).
  double ReadSubscribe() {
    std::string buffer(4096, '\0');
    const auto count = ::recv(fd_, buffer.data(), buffer.size(), 0);
    EXPECT_GT(count, 0);
    buffer.resize(static_cast<std::size_t>(std::max<ssize_t>(count, 0)));
    const auto packet = content_identity::parse_canonical_json(buffer);
    EXPECT_EQ(packet.as_object().size(), 3U);
    EXPECT_EQ(packet.find_object_member("api_version")->as_string(),
              "manyforge.execution_command.v4");
    EXPECT_EQ(packet.find_object_member("command")->as_string(), "subscribe_events");
    return packet.find_object_member("after_cursor")->as_number();
  }
  void SendBytes(const std::string &bytes) {
    ASSERT_EQ(::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(bytes.size()));
  }
  void Send(const V &message) { SendBytes(content_identity::canonical_json(message)); }
  /// Waits until the follower closed this connection.
  void ExpectClosed() {
    char byte = 0;
    EXPECT_EQ(::recv(fd_, &byte, 1, 0), 0);
  }
  void Close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
  }

 private:
  int fd_;
};

/// A private run root with a listening `execution-events.sock`.
class FakeHost {
 public:
  explicit FakeHost(bool listen = true) {
    char pattern[] = "/tmp/mf-gateway-events-XXXXXX";
    root_ = ::mkdtemp(pattern);
    ::chmod(root_.c_str(), 0700);
    if (!listen) return;
    listener_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (root_ / std::string(kExecutionEventsSocketFile)).string();
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    EXPECT_EQ(::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    ::chmod(path.c_str(), 0600);
    EXPECT_EQ(::listen(listener_, 8), 0);
  }
  ~FakeHost() {
    if (listener_ >= 0) ::close(listener_);
    std::filesystem::remove_all(root_);
  }
  /// Waits for the follower's next connection.
  std::unique_ptr<Connection> Accept() {
    return std::make_unique<Connection>(::accept(listener_, nullptr, nullptr));
  }
  [[nodiscard]] const std::filesystem::path &root() const { return root_; }

 private:
  std::filesystem::path root_;
  int listener_{-1};
};

ExecutionEventsOptions FastOptions() {
  ExecutionEventsOptions options;
  options.backoff_initial = 1ms;
  options.backoff_maximum = 2ms;
  return options;
}

constexpr auto kLong = std::chrono::nanoseconds(30s);

// ---------------------------------------------------------------------------
// Message validation.
// ---------------------------------------------------------------------------

TEST(DecodeEventStreamMessage, ValidatesTheEnvelopeAndRaisesAnErrorsCode) {
  EXPECT_EQ(DecodeEventStreamMessage(content_identity::canonical_json(Event(3)))
                .find_object_member("event")
                ->find_object_member("sequence")
                ->as_number(),
            3.0);
  const std::vector<V> bad{
      Message("event", O{{"event", V(O{{"sequence", N(-1)}})}}),
      Message("snapshot", O{{"state", V(O{})}}),  // No cursor.
      Message("gap", O{{"reason", S("owner_epoch_changed")}}),
      Message("unknown", O{}),
      Message("progress", O{{"operation_progress", V(O{})}}),
      Message("state", O{{"state", V(A{})}}),
      V(O{{"api_version", S("other")}, {"type", S("heartbeat")}, {"cursor", N(1)}}),
  };
  for (const auto &message : bad) {
    try {
      static_cast<void>(DecodeEventStreamMessage(content_identity::canonical_json(message)));
      ADD_FAILURE() << content_identity::canonical_json(message);
    } catch (const GatewayError &error) {
      EXPECT_EQ(error.code(), "execution_event_stream_invalid");
    }
  }
  for (const std::string text : {"not json", "[]", "{\"b\":1,\"a\":2}"}) {
    try {
      static_cast<void>(DecodeEventStreamMessage(text));
      ADD_FAILURE() << text;
    } catch (const GatewayError &error) {
      EXPECT_EQ(error.code(), "execution_event_stream_invalid");
    }
  }
  try {
    static_cast<void>(DecodeEventStreamMessage(content_identity::canonical_json(Message(
        "error", O{{"error", V(O{{"code", S("execution_event_subscribers_full")},
                                 {"details", V(O{})},
                                 {"message", S("full")}})}}))));
    ADD_FAILURE();
  } catch (const GatewayError &error) {
    EXPECT_EQ(error.code(), "execution_event_subscribers_full");
    EXPECT_STREQ(error.what(), "full");
  }
}

// ---------------------------------------------------------------------------
// The pushed operations mirror (ports manyforge_behavior/tests/
// test_kernel_client.py, OD-213).
// ---------------------------------------------------------------------------

void ExpectCode(const std::function<void()> &call, std::string_view code) {
  try {
    call();
    ADD_FAILURE() << "expected " << code;
  } catch (const GatewayError &error) {
    EXPECT_EQ(error.code(), code);
  }
}

TEST(PushedOperationMirror, AnswersACurrentOperationAsAReadReply) {
  PushedOperationMirror mirror;
  mirror.ApplyState(State(7, A{Entry(4)}));
  const auto envelope = mirror.Envelope(4, 7);
  ASSERT_TRUE(envelope);
  EXPECT_EQ(*envelope,
            V(O{{"ok", V(true)},
                {"command", S("read")},
                {"operation", *Entry(4).find_object_member("operation")},
                {"resident", V(true)},
                {"last_control", V(nullptr)},
                {"control_pending", V(false)},
                {"active_tcp", V(A{V(O{{"robot", S("arm")}})})}}));
}

TEST(PushedOperationMirror, OlderThanTheAcceptanceIsBehind) {
  PushedOperationMirror mirror;
  mirror.ApplyState(State(6));
  ExpectCode([&] { static_cast<void>(mirror.Envelope(4, 7)); }, kEventMirrorBehind);
}

TEST(PushedOperationMirror, CannotShowRetiredUnlistedOrStatelessOperations) {
  PushedOperationMirror mirror;
  mirror.ApplyState(State(9, A{Entry(5)}));
  EXPECT_FALSE(mirror.Envelope(4, 7));  // Retired.
  mirror.ApplyState(State(9, {}, {}, 1));
  EXPECT_FALSE(mirror.Envelope(4, 7));  // Beyond the list's bound.
  O stateless = Entry(4).as_object();
  for (auto &[name, value] : stateless)
    if (name == "operation") value = V(nullptr);
  mirror.ApplyState(State(9, A{V(stateless)}));
  EXPECT_FALSE(mirror.Envelope(4, 7));  // Listed without its state.
}

TEST(PushedOperationMirror, UnavailableUntilTheFirstAndAfterALostStream) {
  PushedOperationMirror mirror;
  ExpectCode([&] { static_cast<void>(mirror.Envelope(4, 0)); }, kEventMirrorUnavailable);
  mirror.ApplyState(State(7, A{Entry(4)}));
  mirror.StreamLost();
  ExpectCode([&] { static_cast<void>(mirror.Envelope(4, 0)); }, kEventMirrorUnavailable);
  mirror.ApplyState(State(8, A{Entry(4, "completed")}));
  EXPECT_EQ(mirror.Envelope(4, 7)
                ->find_object_member("operation")
                ->find_object_member("state")
                ->as_string(),
            "completed");
}

V WithOperations(V operations) {
  O state = State(1).as_object();
  for (auto &[name, value] : state)
    if (name == "operations") value = std::move(operations);
  return V(std::move(state));
}

V Replace(const V &object, const char *member, V value) {
  O members = object.as_object();
  for (auto &[name, current] : members)
    if (name == member) current = value;
  return V(std::move(members));
}

TEST(PushedOperationMirror, MalformedTableThrowsAndMakesTheCopyUnavailable) {
  const auto table = [](V change, V resident, V total) {
    return V(O{{"change", std::move(change)},
               {"resident", std::move(resident)},
               {"resident_total", std::move(total)}});
  };
  const std::vector<V> bad{
      V(nullptr),
      table(N(1), V(A{}), N(-1)),
      table(N(-1), V(A{}), N(0)),
      table(N(1), V(nullptr), N(0)),
      table(N(1), V(A{Entry(4)}), N(0)),
      table(N(1), V(A{Entry(4), Entry(4)}), N(2)),
      table(N(1), V(A{Replace(Entry(4), "operation_id", N(5))}), N(1)),
      table(N(1), V(A{Replace(Entry(4), "control_pending", V(nullptr))}), N(1)),
      table(N(1), V(A{Replace(Entry(4), "last_control", N(3))}), N(1)),
      table(N(1), V(A{Replace(Entry(0), "operation", V(nullptr))}), N(1)),
      table(N(1.5), V(A{}), N(0)),
      V(O{{"change", N(1)}, {"resident", V(A{})}}),
  };
  for (const auto &operations : bad) {
    PushedOperationMirror mirror;
    mirror.ApplyState(State(7, A{Entry(4)}));
    ExpectCode([&] { mirror.ApplyState(WithOperations(operations)); },
               "execution_event_stream_invalid");
    ExpectCode([&] { static_cast<void>(mirror.Envelope(4, 7)); }, kEventMirrorUnavailable);
  }
}

TEST(PushedOperationMirror, PlanningJobStateIsMemoryOnly) {
  PushedOperationMirror mirror;
  ExpectCode([&] { static_cast<void>(mirror.PlanningJobState("job-1")); },
             kEventMirrorUnavailable);
  mirror.ApplyState(State(1, {}, A{Job("job-1", "done")}));
  EXPECT_EQ(mirror.PlanningJobState("job-1"), std::optional<std::string>("done"));
  EXPECT_EQ(mirror.PlanningJobState("job-2"), std::nullopt);  // Not pushed yet.
  mirror.StreamLost();
  ExpectCode([&] { static_cast<void>(mirror.PlanningJobState("job-1")); },
             kEventMirrorUnavailable);
}

TEST(PushedOperationMirror, MalformedPlanningJobsMakeBothCopiesUnavailable) {
  A nine;
  for (int index = 0; index < 9; ++index)
    nine.push_back(Job(std::to_string(index).c_str(), "done", V(nullptr)));
  const std::vector<V> bad{
      V(nullptr),
      V(A{Job("", "done", V(nullptr))}),
      V(A{Replace(Job("a", "done", V(nullptr)), "kind", S("move"))}),
      V(A{Job("a", "waiting", V(nullptr))}),
      V(A{Job("a", "done", V(nullptr)), Job("a", "done", V(nullptr))}),
      V(std::move(nine)),
      V(A{Job("a", "done", N(0))}),
      V(A{S("row")}),
  };
  for (const auto &rows : bad) {
    PushedOperationMirror mirror;
    mirror.ApplyState(State(7, A{Entry(4)}));
    ExpectCode([&] { mirror.ApplyState(Replace(State(8, A{Entry(4)}), "planning_jobs", rows)); },
               "execution_event_stream_invalid");
    ExpectCode([&] { static_cast<void>(mirror.PlanningJobState("a")); },
               kEventMirrorUnavailable);
    ExpectCode([&] { static_cast<void>(mirror.Envelope(4, 7)); }, kEventMirrorUnavailable);
  }
}

// ---------------------------------------------------------------------------
// WakeSet.
// ---------------------------------------------------------------------------

TEST(WakeSet, CoalescesWakesFromAnyThreadAndEndsAtItsDeadline) {
  WakeSet wake;
  EXPECT_TRUE(wake.Wait(std::chrono::steady_clock::now()).empty());
  wake.Wake(3);
  wake.Wake(3);
  wake.Wake(1);
  EXPECT_EQ(wake.Wait(std::chrono::steady_clock::now() + kLong),
            (std::vector<WakeSet::RunId>{1, 3}));
  std::thread waker([&] { wake.Wake(5); });
  EXPECT_EQ(wake.Wait(std::chrono::steady_clock::now() + kLong),
            (std::vector<WakeSet::RunId>{5}));
  waker.join();
  const auto started = std::chrono::steady_clock::now();
  EXPECT_TRUE(wake.Wait(started + 20ms).empty());
  EXPECT_GE(std::chrono::steady_clock::now() - started, 20ms);
}

TEST(WakeSet, WakesOnlyTheRunsWatchingAChange) {
  WakeSet wake;
  wake.WatchOperation(7, 4);
  wake.WatchOperation(8, 5);
  wake.WatchPlanningJob(9, "job");
  wake.WakeOperations([](std::uint64_t operation_id) { return operation_id == 4; });
  wake.WakePlanningJobs([](const std::string &job_id) { return job_id == "job"; });
  EXPECT_EQ(wake.Wait(std::chrono::steady_clock::now()),
            (std::vector<WakeSet::RunId>{7, 9}));
  wake.UnwatchRun(7);
  wake.WakeAllWatched();
  EXPECT_EQ(wake.Wait(std::chrono::steady_clock::now()),
            (std::vector<WakeSet::RunId>{8, 9}));
  // One watch of a run ends; its other watches and other runs' stay.
  wake.WatchOperation(8, 6);
  wake.WatchOperation(10, 5);
  wake.UnwatchOperation(8, 5);
  wake.UnwatchOperation(8, 99);  // Not watched: nothing changes.
  wake.WakeOperations([](std::uint64_t) { return true; });
  EXPECT_EQ(wake.Wait(std::chrono::steady_clock::now()),
            (std::vector<WakeSet::RunId>{8, 10}));
  wake.UnwatchOperation(8, 6);
  wake.WakeOperations([](std::uint64_t) { return true; });
  EXPECT_EQ(wake.Wait(std::chrono::steady_clock::now()),
            (std::vector<WakeSet::RunId>{10}));
  wake.UnwatchPlanningJob(9, "job");
  wake.WakePlanningJobs([](const std::string &) { return true; });
  wake.WakeAllWatched();
  EXPECT_EQ(wake.Wait(std::chrono::steady_clock::now()),
            (std::vector<WakeSet::RunId>{10}));
}

// ---------------------------------------------------------------------------
// The follower against a fake host.
// ---------------------------------------------------------------------------

TEST(ExecutionEvents, DeliversSnapshotEventStateAndProgressInOrder) {
  FakeHost host;
  ExecutionEvents events(host.root(), nullptr, FastOptions());
  const auto subscription = events.Subscribe();
  events.Start();
  auto connection = host.Accept();
  EXPECT_EQ(connection->ReadSubscribe(), 0.0);
  connection->Send(Snapshot(0));
  connection->Send(Event(1));
  connection->Send(StateMessage(State(5, A{Entry(4)}, A{Job("job", "planning")})));
  connection->Send(Progress(A{V(O{{"operation_id", N(4)}})}));
  connection->Send(Heartbeat(1));
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_TRUE(events.Connected());
  const auto event = subscription->Next(kLong);
  EXPECT_EQ(event->find_object_member("event")->find_object_member("sequence")->as_number(), 1.0);
  EXPECT_EQ(Type(subscription->Next(kLong)), "state");
  // The mirrors are updated before the message is handed out.
  EXPECT_TRUE(events.OperationEnvelope(4, 5));
  EXPECT_EQ(events.PlanningJobState("job"), std::optional<std::string>("planning"));
  EXPECT_EQ(Type(subscription->Next(kLong)), "progress");
  EXPECT_EQ(Type(subscription->Next(kLong)), "heartbeat");
  EXPECT_EQ(subscription->Next(0ns), nullptr);
  events.Close();
  EXPECT_EQ(subscription->Next(kLong), nullptr);  // Closed with the follower.
}

TEST(ExecutionEvents, ResumesStrictlyAfterTheLastDeliveredCursor) {
  FakeHost host;
  ExecutionEvents events(host.root(), nullptr, FastOptions());
  const auto subscription = events.Subscribe();
  events.Start();
  auto first = host.Accept();
  EXPECT_EQ(first->ReadSubscribe(), 0.0);
  first->Send(Snapshot(0, State(3, A{Entry(4)})));
  first->Send(Event(1));
  first->Send(Event(2));
  first->Close();
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_EQ(Type(subscription->Next(kLong)), "event");
  EXPECT_EQ(Type(subscription->Next(kLong)), "event");
  const auto lost = subscription->Next(kLong);
  EXPECT_EQ(Type(lost), "disconnected");
  EXPECT_EQ(Code(lost), "execution_owner_unavailable");
  EXPECT_EQ(lost->find_object_member("message")->as_string(), "event stream closed");
  // Unavailable, never stale, until the next snapshot.
  ExpectCode([&] { static_cast<void>(events.OperationEnvelope(4, 0)); },
             kEventMirrorUnavailable);
  auto second = host.Accept();
  EXPECT_EQ(second->ReadSubscribe(), 2.0);
  second->Send(Snapshot(2, State(4, A{Entry(4)})));
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_TRUE(events.OperationEnvelope(4, 4));
  events.Close();
}

TEST(ExecutionEvents, ASilentLinkIsDeadAndReconnects) {
  FakeHost host;
  auto options = FastOptions();
  options.dead_link = 50ms;
  ExecutionEvents events(host.root(), nullptr, options);
  const auto subscription = events.Subscribe();
  events.Start();
  auto first = host.Accept();
  first->ReadSubscribe();
  first->Send(Heartbeat(6));
  EXPECT_EQ(Type(subscription->Next(kLong)), "heartbeat");
  const auto lost = subscription->Next(kLong);
  EXPECT_EQ(Code(lost), "execution_event_stream_dead");
  EXPECT_FALSE(events.Connected());
  auto second = host.Accept();
  EXPECT_EQ(second->ReadSubscribe(), 6.0);
  events.Close();
}

TEST(ExecutionEvents, AnErrorMessageEndsTheLinkWithItsCode) {
  FakeHost host;
  ExecutionEvents events(host.root(), nullptr, FastOptions());
  const auto subscription = events.Subscribe();
  events.Start();
  auto first = host.Accept();
  first->ReadSubscribe();
  first->Send(Snapshot(0));
  first->Send(Message("error", O{{"error", V(O{{"code", S("execution_event_subscribers_full")},
                                               {"details", V(O{})},
                                               {"message", S("full")}})}}));
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  const auto lost = subscription->Next(kLong);
  EXPECT_EQ(Code(lost), "execution_event_subscribers_full");
  EXPECT_EQ(lost->find_object_member("message")->as_string(), "full");
  auto second = host.Accept();
  EXPECT_EQ(second->ReadSubscribe(), 0.0);
  events.Close();
}

TEST(ExecutionEvents, AnInvalidMessageOrTableIsNotDeliveredAndNotPassed) {
  FakeHost host;
  ExecutionEvents events(host.root(), nullptr, FastOptions());
  const auto subscription = events.Subscribe();
  events.Start();
  auto first = host.Accept();
  first->ReadSubscribe();
  first->Send(Snapshot(0, State(1, A{Entry(4)})));
  first->Send(Event(3));
  first->SendBytes("{\"type\":\"event\"}");
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_EQ(Type(subscription->Next(kLong)), "event");
  EXPECT_EQ(Code(subscription->Next(kLong)), "execution_event_stream_invalid");
  auto second = host.Accept();
  EXPECT_EQ(second->ReadSubscribe(), 3.0);
  second->Send(Snapshot(3));
  second->Send(Event(4));
  second->Send(StateMessage(WithOperations(V(O{{"change", N(6)}}))));
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_EQ(Type(subscription->Next(kLong)), "event");
  const auto lost = subscription->Next(kLong);
  EXPECT_EQ(Code(lost), "execution_event_stream_invalid");
  ExpectCode([&] { static_cast<void>(events.OperationEnvelope(4, 0)); },
             kEventMirrorUnavailable);
  auto third = host.Accept();
  EXPECT_EQ(third->ReadSubscribe(), 4.0);
  events.Close();
}

double Sequence(const EventMessage &message) {
  if (Type(message) != "event") return -1.0;
  return message->find_object_member("event")->find_object_member("sequence")->as_number();
}

TEST(ExecutionEvents, AFullSubscriptionResumesOnItsOwnConnectionLosingNothing) {
  FakeHost host;
  auto options = FastOptions();
  options.subscription_capacity = 2;
  ExecutionEvents events(host.root(), nullptr, options);
  const auto lagging = events.Subscribe();
  const auto other = events.Subscribe();
  events.Start();
  auto shared = host.Accept();
  EXPECT_EQ(shared->ReadSubscribe(), 0.0);
  shared->Send(Snapshot(5));
  EXPECT_EQ(Type(other->Next(kLong)), "snapshot");
  EXPECT_EQ(Type(lagging->Next(kLong)), "snapshot");
  for (const double sequence : {6.0, 7.0, 8.0}) {
    // `lagging` holds 6 and 7; 8 finds it full (it is handed out first).
    shared->Send(Event(sequence));
    EXPECT_EQ(Sequence(other->Next(kLong)), sequence);
  }
  // Its own connection, after the last message its consumer took.
  auto own = host.Accept();
  EXPECT_EQ(own->ReadSubscribe(), 5.0);
  own->Send(Snapshot(5));
  for (const double sequence : {6.0, 7.0, 8.0}) own->Send(Event(sequence));
  const auto lost = lagging->Next(kLong);
  EXPECT_EQ(Type(lost), "disconnected");
  EXPECT_EQ(Code(lost), kEventSubscriptionOverflow);
  // Read only while there is room (a queue of two): nothing is lost.
  EXPECT_EQ(Type(lagging->Next(kLong)), "snapshot");
  for (const double sequence : {6.0, 7.0, 8.0})
    EXPECT_EQ(Sequence(lagging->Next(kLong)), sequence);
  // The other subscription and the shared connection are undisturbed, and
  // the shared connection no longer feeds `lagging`.
  shared->Send(Event(9));
  EXPECT_EQ(Sequence(other->Next(kLong)), 9.0);
  own->Send(Event(9));
  EXPECT_EQ(Sequence(lagging->Next(kLong)), 9.0);
  EXPECT_EQ(lagging->Next(0ns), nullptr);
  EXPECT_EQ(other->Next(0ns), nullptr);
  // A lost own connection resumes from its own cursor.
  own->Close();
  EXPECT_EQ(Code(lagging->Next(kLong)), "execution_owner_unavailable");
  auto again = host.Accept();
  EXPECT_EQ(again->ReadSubscribe(), 9.0);
  EXPECT_TRUE(events.Connected());
  events.Close();
  EXPECT_EQ(lagging->Next(kLong), nullptr);  // Closed with the follower too.
}

TEST(ExecutionEvents, ALinkLossOnAFullSubscriptionResumesItOnItsOwnConnection) {
  FakeHost host;
  auto options = FastOptions();
  options.subscription_capacity = 2;
  ExecutionEvents events(host.root(), nullptr, options);
  const auto subscription = events.Subscribe();
  events.Start();
  auto first = host.Accept();
  first->ReadSubscribe();
  first->Send(Snapshot(0));
  first->Send(Event(1));
  first->Close();  // Its `disconnected` finds the queue full.
  // The shared connection resumes after 1, the subscription after 0 (it
  // took nothing): its backlog is read again. (Taken only after both
  // reconnected, so the queue was full at the loss.)
  auto a = host.Accept();
  auto b = host.Accept();
  EXPECT_EQ(Code(subscription->Next(kLong)), "execution_owner_unavailable");
  const double a_cursor = a->ReadSubscribe();
  const double b_cursor = b->ReadSubscribe();
  EXPECT_EQ(std::min(a_cursor, b_cursor), 0.0);
  EXPECT_EQ(std::max(a_cursor, b_cursor), 1.0);
  auto &own = a_cursor == 0.0 ? a : b;
  own->Send(Snapshot(0));
  own->Send(Event(1));
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_EQ(Sequence(subscription->Next(kLong)), 1.0);
  events.Close();
}

TEST(ExecutionEvents, AResyncRereadsOneSubscriptionFromBeforeItsLastMessage) {
  FakeHost host;
  ExecutionEvents events(host.root(), nullptr, FastOptions());
  const auto resyncing = events.Subscribe();
  const auto other = events.Subscribe();
  events.Start();
  auto shared = host.Accept();
  shared->ReadSubscribe();
  shared->Send(Snapshot(0));
  shared->Send(Event(1));
  shared->Send(Event(2));
  EXPECT_EQ(Type(other->Next(kLong)), "snapshot");
  EXPECT_EQ(Sequence(other->Next(kLong)), 1.0);
  EXPECT_EQ(Sequence(other->Next(kLong)), 2.0);
  EXPECT_EQ(Type(resyncing->Next(kLong)), "snapshot");
  EXPECT_EQ(Sequence(resyncing->Next(kLong)), 1.0);
  // Row 1 could not be applied: a fresh snapshot after 0, then 1 again.
  resyncing->Resync();
  const auto lost = resyncing->Next(kLong);
  EXPECT_EQ(Type(lost), "disconnected");
  EXPECT_EQ(Code(lost), "execution_event_stream_invalid");
  auto own = host.Accept();
  EXPECT_EQ(own->ReadSubscribe(), 0.0);
  own->Send(Snapshot(0));
  own->Send(Event(1));
  own->Send(Event(2));
  EXPECT_EQ(Type(resyncing->Next(kLong)), "snapshot");
  EXPECT_EQ(Sequence(resyncing->Next(kLong)), 1.0);
  EXPECT_EQ(Sequence(resyncing->Next(kLong)), 2.0);
  // Again on its own connection: that one closes, a new one starts after 1.
  resyncing->Resync();
  EXPECT_EQ(Type(resyncing->Next(kLong)), "disconnected");
  auto again = host.Accept();
  EXPECT_EQ(again->ReadSubscribe(), 1.0);
  own->ExpectClosed();
  again->Send(Snapshot(1));
  again->Send(Event(2));
  EXPECT_EQ(Type(resyncing->Next(kLong)), "snapshot");
  EXPECT_EQ(Sequence(resyncing->Next(kLong)), 2.0);
  // The other subscription saw none of it.
  shared->Send(Event(3));
  EXPECT_EQ(Sequence(other->Next(kLong)), 3.0);
  EXPECT_EQ(other->Next(0ns), nullptr);
  events.Close();
}

TEST(ExecutionEvents, ConsecutiveResyncsBackOffUntilTheConsumerAppliesAMessage) {
  FakeHost host;
  auto options = FastOptions();
  options.backoff_initial = 100ms;
  options.backoff_maximum = 200ms;
  ExecutionEvents events(host.root(), nullptr, options);
  const auto resyncing = events.Subscribe();
  events.Start();
  auto shared = host.Accept();
  shared->ReadSubscribe();
  shared->Send(Snapshot(0));
  EXPECT_EQ(Type(resyncing->Next(kLong)), "snapshot");
  // Each resync's connection opens no earlier than its delay.
  const auto resync_after = [&](std::chrono::milliseconds delay,
                                std::uint32_t resyncs) {
    const auto started = std::chrono::steady_clock::now();
    resyncing->Resync();
    EXPECT_EQ(resyncing->CurrentFeed().resyncs, resyncs);
    EXPECT_EQ(Type(resyncing->Next(kLong)), "disconnected");
    auto own = host.Accept();
    EXPECT_GE(std::chrono::steady_clock::now() - started, delay);
    EXPECT_EQ(own->ReadSubscribe(), 0.0);
    own->Send(Snapshot(0));
    EXPECT_EQ(Type(resyncing->Next(kLong)), "snapshot");
    return own;
  };
  auto first = resync_after(100ms, 1);
  // The snapshot could not be applied either: the delay doubles to its bound.
  auto second = resync_after(200ms, 2);
  auto third = resync_after(200ms, 3);
  // Asking again after the snapshot: it was applied, the delay starts over.
  EXPECT_EQ(resyncing->Next(0ns), nullptr);
  auto fourth = resync_after(100ms, 1);
  events.Close();
}

TEST(ExecutionEvents, AResyncDelayLeavesTheOtherSubscriptionsServed) {
  FakeHost host;
  auto options = FastOptions();
  options.backoff_initial = 30s;
  options.backoff_maximum = 30s;
  ExecutionEvents events(host.root(), nullptr, options);
  const auto resyncing = events.Subscribe();
  const auto other = events.Subscribe();
  events.Start();
  auto shared = host.Accept();
  shared->ReadSubscribe();
  shared->Send(Snapshot(0));
  EXPECT_EQ(Type(resyncing->Next(kLong)), "snapshot");
  EXPECT_EQ(Type(other->Next(kLong)), "snapshot");
  resyncing->Resync();
  EXPECT_EQ(Type(resyncing->Next(kLong)), "disconnected");
  // The follower waits for the resync's connection in its poll, not asleep.
  shared->Send(Event(1));
  shared->Send(Event(2));
  EXPECT_EQ(Sequence(other->Next(std::chrono::nanoseconds(5s))), 1.0);
  EXPECT_EQ(Sequence(other->Next(std::chrono::nanoseconds(5s))), 2.0);
  EXPECT_EQ(resyncing->Next(0ns), nullptr);
  const auto closing = std::chrono::steady_clock::now();
  events.Close();
  EXPECT_LT(std::chrono::steady_clock::now() - closing, 5s);
}

TEST(ExecutionEvents, AMalformedTableWakesTheWatchers) {
  FakeHost host;
  const auto wake = std::make_shared<WakeSet>();
  wake->WatchOperation(7, 4);
  ExecutionEvents events(host.root(), wake, FastOptions());
  const auto subscription = events.Subscribe();
  events.Start();
  auto connection = host.Accept();
  connection->ReadSubscribe();
  const auto now = [] { return std::chrono::steady_clock::now(); };
  connection->Send(Snapshot(0, State(1, A{Entry(4)})));
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_EQ(wake->Wait(now()), (std::vector<WakeSet::RunId>{7}));
  // The table clears the copy: the run waiting on operation 4 must re-read.
  connection->Send(StateMessage(WithOperations(V(O{{"change", N(6)}}))));
  EXPECT_EQ(Code(subscription->Next(kLong)), "execution_event_stream_invalid");
  EXPECT_EQ(wake->Wait(now()), (std::vector<WakeSet::RunId>{7}));
  events.Close();
}

TEST(ExecutionEvents, WakesTheRunsWatchingWhatAMessageChanged) {
  FakeHost host;
  const auto wake = std::make_shared<WakeSet>();
  wake->WatchOperation(7, 4);
  wake->WatchOperation(8, 5);
  wake->WatchPlanningJob(9, "job");
  ExecutionEvents events(host.root(), wake, FastOptions());
  const auto subscription = events.Subscribe();
  events.Start();
  auto connection = host.Accept();
  connection->ReadSubscribe();
  const auto now = [] { return std::chrono::steady_clock::now(); };
  // The first snapshot makes every copy available.
  connection->Send(Snapshot(0, State(1, A{Entry(4), Entry(5)}, A{Job("job", "planning")})));
  EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
  EXPECT_EQ(wake->Wait(now()), (std::vector<WakeSet::RunId>{7, 8, 9}));
  // Operation 4 changes; 5 and the job do not.
  connection->Send(StateMessage(
      State(2, A{Entry(4, "completed"), Entry(5)}, A{Job("job", "planning")})));
  EXPECT_EQ(Type(subscription->Next(kLong)), "state");
  EXPECT_EQ(wake->Wait(now()), (std::vector<WakeSet::RunId>{7}));
  connection->Send(StateMessage(
      State(2, A{Entry(4, "completed"), Entry(5)}, A{Job("job", "done")})));
  EXPECT_EQ(Type(subscription->Next(kLong)), "state");
  EXPECT_EQ(wake->Wait(now()), (std::vector<WakeSet::RunId>{9}));
  // Progress rows wake no run (the runner reads no progress).
  connection->Send(Progress(A{V(O{{"operation_id", N(5)}, {"progress", N(1)}})}));
  EXPECT_EQ(Type(subscription->Next(kLong)), "progress");
  EXPECT_TRUE(wake->Wait(now()).empty());
  connection->Send(Event(1));
  EXPECT_EQ(Type(subscription->Next(kLong)), "event");
  EXPECT_TRUE(wake->Wait(now()).empty());
  // A lost link wakes every watcher.
  connection->Close();
  EXPECT_EQ(Type(subscription->Next(kLong)), "disconnected");
  EXPECT_EQ(wake->Wait(now()), (std::vector<WakeSet::RunId>{7, 8, 9}));
  events.Close();
}

TEST(ExecutionEvents, CloseEndsTheFollowerAtOnce) {
  {
    // Waiting on a quiet link (dead-link limit 3 s).
    FakeHost host;
    ExecutionEvents events(host.root(), nullptr, FastOptions());
    const auto subscription = events.Subscribe();
    events.Start();
    auto connection = host.Accept();
    connection->ReadSubscribe();
    connection->Send(Snapshot(0));
    EXPECT_EQ(Type(subscription->Next(kLong)), "snapshot");
    const auto started = std::chrono::steady_clock::now();
    events.Close();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
  }
  {
    // Waiting out a reconnect delay (no listener).
    FakeHost host(false);
    auto options = FastOptions();
    options.backoff_initial = options.backoff_maximum = 60s;
    ExecutionEvents events(host.root(), nullptr, options);
    const auto subscription = events.Subscribe();
    events.Start();
    EXPECT_EQ(Code(subscription->Next(kLong)), "execution_owner_unavailable");
    const auto started = std::chrono::steady_clock::now();
    events.Close();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
  }
}

}  // namespace
}  // namespace manyforge::headless::gateway
