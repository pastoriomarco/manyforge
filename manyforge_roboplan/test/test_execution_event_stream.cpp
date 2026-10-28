// Event subscription stream (contract §8.1): delivery, resume, gap/snapshot,
// heartbeat, slow subscriber isolation and shutdown, on a real SEQPACKET
// socket with an in-memory journal.

#include "execution_event_stream.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using manyforge::content_identity::CanonicalValue;
using manyforge::content_identity::canonical_json;
using manyforge::content_identity::parse_canonical_json;
using manyforge::core::ExecutionEventChangeSignal;
using manyforge::roboplan_backend::host_detail::ExecutionEventStream;
using manyforge::roboplan_backend::host_detail::ExecutionEventStreamSource;
using namespace std::chrono_literals;

/// The journal and owner facts the stream reads; `mutex` stands for the
/// owner/journal locks a Stop or control command also takes.
struct FakeOwner final {
  std::mutex mutex;
  std::string epoch = "kernel-1";
  std::vector<std::uint64_t> sequences;
  std::string carried = "none";
  std::optional<double> progress;  // One executing operation, or none.

  ExecutionEventStreamSource Source() {
    return {
        [this] {
          std::lock_guard lock(mutex);
          return std::pair{epoch, CanonicalValue(CanonicalValue::Object{
                                      {"carried", CanonicalValue(carried)}})};
        },
        [this](const std::string &owner_epoch, const std::uint64_t after) {
          std::lock_guard lock(mutex);
          EXPECT_EQ(owner_epoch, epoch);
          CanonicalValue::Array events;
          bool more = false;
          for (const auto sequence : sequences) {
            if (sequence <= after) continue;
            if (events.size() == 16U) {
              more = true;
              break;
            }
            events.emplace_back(CanonicalValue::Object{
                {"record_kind", CanonicalValue("operation_terminal")},
                {"sequence", CanonicalValue(static_cast<double>(sequence))}});
          }
          return CanonicalValue(CanonicalValue::Object{
              {"events", CanonicalValue(std::move(events))},
              {"has_more", CanonicalValue(more)}});
        },
        [this] {
          std::lock_guard lock(mutex);
          CanonicalValue::Array rows;
          if (progress)
            rows.emplace_back(CanonicalValue::Object{
                {"operation_id", CanonicalValue("operation.1")},
                {"progress", CanonicalValue(*progress)}});
          return CanonicalValue(std::move(rows));
        }};
  }

  void Append(ExecutionEventChangeSignal &signal, const std::uint64_t sequence) {
    {
      std::lock_guard lock(mutex);
      sequences.push_back(sequence);
    }
    signal.Notify();
  }
};

class StreamFixture : public ::testing::Test {
protected:
  void SetUp() override {
    char pattern[] = "/tmp/mf-event-stream-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern), nullptr);
    directory_ = pattern;
    path_ = directory_ + "/execution-events.sock";
    listener_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    ASSERT_GE(listener_, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1U);
    ASSERT_EQ(::bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener_, 16), 0);
    stream_.emplace(owner_.Source(), signal_);
    runner_ = std::jthread([this](std::stop_token stop) { stream_->Run(listener_, stop); });
  }

  void TearDown() override {
    runner_ = {};
    ::close(listener_);
    std::filesystem::remove_all(directory_);
  }

  [[nodiscard]] int Subscribe(const std::uint64_t after,
                              const std::optional<std::string> &epoch,
                              const int receive_buffer = 0) const {
    const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (receive_buffer > 0)
      ::setsockopt(client, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1U);
    EXPECT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
    const auto request = canonical_json(CanonicalValue(CanonicalValue::Object{
        {"after_cursor", CanonicalValue(static_cast<double>(after))},
        {"api_version", CanonicalValue("manyforge.execution_command.v1")},
        {"command", CanonicalValue("subscribe_events")},
        {"owner_epoch", epoch ? CanonicalValue(*epoch) : CanonicalValue(nullptr)}}));
    EXPECT_EQ(::send(client, request.data(), request.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(request.size()));
    return client;
  }

  /// The next message, or nullopt on timeout or EOF.
  [[nodiscard]] static std::optional<CanonicalValue>
  Receive(const int client, const std::chrono::milliseconds timeout = 2000ms) {
    pollfd inbound{client, POLLIN, 0};
    if (::poll(&inbound, 1, static_cast<int>(timeout.count())) <= 0) return std::nullopt;
    std::string buffer(1U << 20U, '\0');
    const auto count = ::recv(client, buffer.data(), buffer.size(), 0);
    if (count <= 0) return std::nullopt;
    buffer.resize(static_cast<std::size_t>(count));
    return parse_canonical_json(buffer);
  }

  [[nodiscard]] static std::string Type(const CanonicalValue &message) {
    return message.find_object_member("type")->as_string();
  }

  [[nodiscard]] static double Field(const CanonicalValue &message, const char *name) {
    return message.find_object_member(name)->as_number();
  }

  [[nodiscard]] static double Sequence(const CanonicalValue &message) {
    EXPECT_EQ(Type(message), "event");
    return message.find_object_member("event")->find_object_member("sequence")->as_number();
  }

  /// Skips heartbeats.
  [[nodiscard]] static CanonicalValue Next(const int client,
                                           const std::chrono::milliseconds timeout = 2000ms) {
    for (;;) {
      auto message = Receive(client, timeout);
      EXPECT_TRUE(message.has_value());
      if (!message) return CanonicalValue(nullptr);
      if (Type(*message) != "heartbeat") return *message;
    }
  }

  ExecutionEventChangeSignal signal_;
  FakeOwner owner_;
  std::string directory_;
  std::string path_;
  int listener_ = -1;
  std::optional<ExecutionEventStream> stream_;
  std::jthread runner_;
};

TEST_F(StreamFixture, DeliversSnapshotThenEveryEventInOrderThenLiveAppends) {
  for (std::uint64_t sequence = 1; sequence <= 40; ++sequence) owner_.sequences.push_back(sequence);
  const int client = Subscribe(0, std::nullopt);
  const auto snapshot = Next(client);
  ASSERT_EQ(Type(snapshot), "snapshot");
  EXPECT_EQ(Field(snapshot, "cursor"), 0.0);
  EXPECT_EQ(snapshot.find_object_member("reason")->as_string(), "subscribed");
  for (double expected = 1; expected <= 40; ++expected) EXPECT_EQ(Sequence(Next(client)), expected);
  const auto started = std::chrono::steady_clock::now();
  owner_.Append(signal_, 41);
  EXPECT_EQ(Sequence(Next(client)), 41.0);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 200ms);  // Event-to-message.
  ::close(client);
}

TEST_F(StreamFixture, ResumesStrictlyAfterItsCursor) {
  owner_.sequences = {3, 5, 8};
  const int client = Subscribe(5, std::string("kernel-1"));
  const auto snapshot = Next(client);
  ASSERT_EQ(Type(snapshot), "snapshot");
  EXPECT_EQ(Field(snapshot, "cursor"), 5.0);
  EXPECT_EQ(Sequence(Next(client)), 8.0);
  ::close(client);
}

TEST_F(StreamFixture, AnotherOwnerEpochIsAGapThenASnapshotFromTheStart) {
  owner_.sequences = {1, 2};
  const int client = Subscribe(9, std::string("kernel-0"));  // Host restarted.
  const auto gap = Next(client);
  ASSERT_EQ(Type(gap), "gap");
  EXPECT_EQ(gap.find_object_member("reason")->as_string(), "owner_epoch_changed");
  const auto snapshot = Next(client);
  ASSERT_EQ(Type(snapshot), "snapshot");
  EXPECT_EQ(Field(snapshot, "cursor"), 0.0);
  EXPECT_EQ(snapshot.find_object_member("owner_epoch")->as_string(), "kernel-1");
  EXPECT_EQ(Sequence(Next(client)), 1.0);
  EXPECT_EQ(Sequence(Next(client)), 2.0);
  ::close(client);
}

TEST_F(StreamFixture, StateChangeIsOneTypedMessage) {
  const int client = Subscribe(0, std::nullopt);
  ASSERT_EQ(Type(Next(client)), "snapshot");
  {
    std::lock_guard lock(owner_.mutex);
    owner_.carried = "lost";
  }
  signal_.Notify();
  const auto state = Next(client);
  ASSERT_EQ(Type(state), "state");
  EXPECT_EQ(state.find_object_member("state")->find_object_member("carried")->as_string(), "lost");
  ::close(client);
}

TEST_F(StreamFixture, ProgressIsStreamedWhenItChangesAndSampledWhileExecuting) {
  const int client = Subscribe(0, std::nullopt);
  ASSERT_EQ(Type(Next(client)), "snapshot");
  const auto progress = [](const CanonicalValue &message) {
    EXPECT_EQ(Type(message), "progress");
    return message.find_object_member("operation_progress")->as_array();
  };
  {
    std::lock_guard lock(owner_.mutex);
    owner_.progress = 0.1;
  }
  signal_.Notify();  // The operation started (a journal commit).
  auto rows = progress(Next(client));
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].find_object_member("progress")->as_number(), 0.1);
  {
    std::lock_guard lock(owner_.mutex);
    owner_.progress = 0.5;  // A control-rate sample: no signal.
  }
  const auto started = std::chrono::steady_clock::now();
  rows = progress(Next(client));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 300ms);
  EXPECT_EQ(rows[0].find_object_member("progress")->as_number(), 0.5);
  {
    std::lock_guard lock(owner_.mutex);
    owner_.progress.reset();
  }
  signal_.Notify();  // The operation ended.
  EXPECT_TRUE(progress(Next(client)).empty());
  // Unchanged progress sends nothing more: only heartbeats follow.
  const auto quiet = Receive(client, 1500ms);
  ASSERT_TRUE(quiet.has_value());
  EXPECT_EQ(Type(*quiet), "heartbeat");
  ::close(client);
}

TEST_F(StreamFixture, HeartbeatCarriesTheCursorEverySecond) {
  owner_.sequences = {4};
  const int client = Subscribe(0, std::nullopt);
  ASSERT_EQ(Type(Next(client)), "snapshot");
  EXPECT_EQ(Sequence(Next(client)), 4.0);
  const auto heartbeat = Receive(client, 1500ms);
  ASSERT_TRUE(heartbeat.has_value());
  EXPECT_EQ(Type(*heartbeat), "heartbeat");
  EXPECT_EQ(Field(*heartbeat, "cursor"), 4.0);
  ::close(client);
}

TEST_F(StreamFixture, SlowSubscriberBlocksNeitherOtherSubscribersNorTheOwnerLocks) {
  // A subscriber that never reads fills its socket; its writer waits in its
  // own send. A concurrent "Stop/control" (the owner lock) and another
  // subscriber are unaffected.
  for (std::uint64_t sequence = 1; sequence <= 4000; ++sequence) owner_.sequences.push_back(sequence);
  const int slow = Subscribe(0, std::nullopt, 4096);
  std::this_thread::sleep_for(200ms);  // Its writer is now stuck sending.
  const auto locked_at = std::chrono::steady_clock::now();
  {
    std::unique_lock lock(owner_.mutex, std::defer_lock);
    ASSERT_TRUE(lock.try_lock());  // No lock is held across a send.
    owner_.sequences.push_back(4001);
  }
  signal_.Notify();
  EXPECT_LT(std::chrono::steady_clock::now() - locked_at, 50ms);
  const int fast = Subscribe(4000, std::nullopt);
  ASSERT_EQ(Type(Next(fast)), "snapshot");
  EXPECT_EQ(Sequence(Next(fast)), 4001.0);
  ::close(fast);
  ::close(slow);
}

TEST_F(StreamFixture, ShutdownClosesEverySubscription) {
  const int first = Subscribe(0, std::nullopt);
  const int second = Subscribe(0, std::nullopt);
  ASSERT_EQ(Type(Next(first)), "snapshot");
  ASSERT_EQ(Type(Next(second)), "snapshot");
  const auto started = std::chrono::steady_clock::now();
  runner_ = {};  // Stop and join: every writer is joined, its socket closed.
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
  for (const int client : {first, second}) {
    std::optional<CanonicalValue> message;
    do {
      message = Receive(client, 500ms);
    } while (message && Type(*message) == "heartbeat");
    EXPECT_FALSE(message.has_value());  // EOF.
    ::close(client);
  }
}

TEST_F(StreamFixture, MalformedSubscribeIsRefused) {
  const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path_.c_str(), sizeof(address.sun_path) - 1U);
  ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  const std::string request = R"({"command":"subscribe_events"})";
  ASSERT_GT(::send(client, request.data(), request.size(), MSG_NOSIGNAL), 0);
  const auto error = Receive(client);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(Type(*error), "error");
  EXPECT_EQ(error->find_object_member("error")->find_object_member("code")->as_string(),
            "execution_event_subscription_invalid");
  ::close(client);
}

} // namespace
