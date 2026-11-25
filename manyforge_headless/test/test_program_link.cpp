// The Program runner's command and status sockets (program_link.hpp): real
// sockets in a private temporary root; waits are the link's own blocking
// calls with bounds, never sleeps.

#include "manyforge_headless/program_link.hpp"

#include "manyforge_headless/gateway_core.hpp"

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::headless::gateway {
namespace {

using namespace std::chrono_literals;

class ProgramLinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "program-link-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    root_ = std::filesystem::canonical(pattern);
    ASSERT_EQ(::chmod(root_.c_str(), 0700), 0);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  std::filesystem::path root_;
};

std::string Request(const std::string &command) {
  return "{\"api_version\":\"manyforge.program_command.v1\",\"command\":\"" +
         command + "\",\"request\":{}}";
}

std::string CodeOf(const GatewayError &error) { return error.code(); }

std::string TransportError(const GatewayError &error) {
  for (const auto &[name, value] : error.details())
    if (name == "transport_error")
      return value.as_string();
  return {};
}

/// A listening runner socket served by the test itself (packets as given).
class RawRunner {
 public:
  explicit RawRunner(const std::filesystem::path &root)
      : listener_(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) {
    const auto path = (root / "program-command.sock").string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    EXPECT_EQ(::bind(listener_, reinterpret_cast<const sockaddr *>(&address),
                     sizeof(address)),
              0);
    EXPECT_EQ(::chmod(path.c_str(), 0600), 0);
    EXPECT_EQ(::listen(listener_, 1), 0);
  }
  ~RawRunner() { ::close(listener_); }
  RawRunner(const RawRunner &) = delete;
  RawRunner &operator=(const RawRunner &) = delete;

  /// Accepts one client, reads its request (up to its last packet), sends
  /// `packets` and closes.
  void Serve(const std::vector<std::string> &packets) const {
    const int client = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
    ASSERT_GE(client, 0);
    std::string packet(1U << 20U, '\0');
    for (;;) {
      const auto size = ::recv(client, packet.data(), packet.size(), 0);
      ASSERT_GT(size, 0);
      if (packet[0] != '\0')
        break;
    }
    for (const auto &bytes : packets)
      EXPECT_EQ(::send(client, bytes.data(), bytes.size(), MSG_NOSIGNAL),
                static_cast<ssize_t>(bytes.size()));
    ::close(client);
  }

 private:
  int listener_;
};

TEST_F(ProgramLinkTest, CommandRoundTripKeepsBytesBothWays) {
  ProgramCommandServer server(root_);
  server.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  const auto request = Request("cycle_status");
  // Above one packet on every kernel: the reply goes in several packets.
  const std::string reply = "{\"ok\":true,\"value\":\"" +
                            std::string(3U * 1024U * 1024U, 'x') + "\"}";
  std::string answered;
  std::thread client([&] { answered = link.Call(request); });
  const auto received = server.Next(5000ms);
  ASSERT_TRUE(received.has_value());
  EXPECT_EQ(received->bytes, request);
  server.Reply(received->token, reply);
  client.join();
  EXPECT_EQ(answered, reply);
  EXPECT_THROW(server.Reply(received->token, reply), std::invalid_argument);
  struct stat status {};
  ASSERT_EQ(::stat((root_ / "program-command.sock").c_str(), &status), 0);
  EXPECT_EQ(status.st_mode & 07777, 0600);
  server.Close();
  EXPECT_FALSE(std::filesystem::exists(root_ / "program-command.sock"));
}

TEST_F(ProgramLinkTest, OversizedRequestIsRefusedWithoutReachingPython) {
  ProgramCommandServer server(root_);
  server.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  const std::string request(kMaximumProgramCommandRequestBytes + 1U, ' ');
  const auto reply = link.Call(request);
  EXPECT_NE(reply.find("\"code\":\"program_runner_request_body_oversized\""),
            std::string::npos);
  EXPECT_NE(reply.find("\"ok\":false"), std::string::npos);
  EXPECT_FALSE(server.Next(0ms).has_value());
}

TEST_F(ProgramLinkTest, FullIntakeRefusesAtOnceWithTheLaneBusyCode) {
  ProgramCommandServer server(root_);
  server.Start();
  const ProgramRunnerLink link(root_, 10000ms);
  constexpr std::size_t kClients = kMaximumQueuedProgramCommands + 1U;
  std::vector<std::string> replies(kClients);
  std::mutex mutex;
  std::condition_variable done;
  std::size_t finished = 0U;
  std::vector<std::thread> clients;
  for (std::size_t index = 0U; index < kClients; ++index)
    clients.emplace_back([&, index] {
      replies[index] = link.Call(Request("health"));
      const std::lock_guard lock(mutex);
      ++finished;
      done.notify_all();
    });
  {
    // Nothing is taken yet, so exactly one request finds the intake full.
    std::unique_lock lock(mutex);
    ASSERT_TRUE(done.wait_for(lock, 10s, [&] { return finished == 1U; }));
  }
  for (std::size_t index = 0U; index < kMaximumQueuedProgramCommands; ++index) {
    const auto received = server.Next(5000ms);
    ASSERT_TRUE(received.has_value());
    server.Reply(received->token, "{\"ok\":true,\"value\":null}");
  }
  for (auto &client : clients)
    client.join();
  std::size_t busy = 0U;
  for (const auto &reply : replies)
    busy += reply.find("execution_control_ingress_busy") != std::string::npos;
  EXPECT_EQ(busy, 1U);
}

TEST_F(ProgramLinkTest, ASilentOrPartlySentClientDelaysNoOtherCommand) {
  ProgramCommandServer server(root_);
  server.Start();
  const auto connect = [&] {
    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    const auto path = (root_ / "program-command.sock").string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    EXPECT_EQ(::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                        sizeof(address)),
              0);
    return fd;
  };
  const auto send = [](const int fd, const std::string &packet) {
    EXPECT_EQ(::send(fd, packet.data(), packet.size(), MSG_NOSIGNAL),
              static_cast<ssize_t>(packet.size()));
  };
  const int silent = connect();
  const int partial = connect();
  const auto request = Request("pause");
  // The first packet of a two-packet request; the rest comes later.
  send(partial, std::string(1U, '\0') + request.substr(0U, 10U));
  const ProgramRunnerLink link(root_, 2000ms);
  std::string answered;
  std::thread client([&] { answered = link.Call(Request("health"), 10000ms); });
  const auto first = server.Next(5000ms);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->bytes, Request("health"));
  server.Reply(first->token, "{\"ok\":true,\"value\":null}");
  client.join();
  EXPECT_EQ(answered, "{\"ok\":true,\"value\":null}");
  // The partly sent request is assembled once its last packet arrives.
  send(partial, request.substr(10U));
  const auto second = server.Next(5000ms);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->bytes, request);
  server.Reply(second->token, "{\"ok\":true,\"value\":1}");
  std::string reply(64U, '\0');
  const auto size = ::recv(partial, reply.data(), reply.size(), 0);
  ASSERT_GT(size, 0);
  reply.resize(static_cast<std::size_t>(size));
  EXPECT_EQ(reply, "{\"ok\":true,\"value\":1}");
  // Close ends the silent connection: its client sees the runner gone.
  server.Close();
  char byte = 0;
  EXPECT_EQ(::recv(silent, &byte, 1U, 0), 0);
  ::close(silent);
  ::close(partial);
}

TEST_F(ProgramLinkTest, CloseEndsAWaitingCallAsUnavailable) {
  ProgramCommandServer server(root_);
  server.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  std::optional<std::string> code;
  std::thread client([&] {
    try {
      static_cast<void>(link.Call(Request("start_run")));
    } catch (const GatewayError &error) {
      code = CodeOf(error);
    }
  });
  ASSERT_TRUE(server.Next(5000ms).has_value());
  server.Close();
  client.join();
  EXPECT_EQ(code, "program_runner_unavailable");
}

TEST_F(ProgramLinkTest, ALimitedCallEndsAtItsLimitAsReadTimeout) {
  ProgramCommandServer server(root_);
  server.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  try {
    // The request is queued and never answered.
    static_cast<void>(link.Call(Request("runs"), 50ms));
    FAIL() << "the call returned without a reply";
  } catch (const GatewayError &error) {
    EXPECT_EQ(error.code(), "program_runner_unavailable");
    EXPECT_EQ(TransportError(error), "read_timeout");
  }
  const auto received = server.Next(5000ms);
  ASSERT_TRUE(received.has_value());
  // Its client is gone: the reply is dropped.
  server.Reply(received->token, "{\"ok\":true,\"value\":null}");
}

TEST_F(ProgramLinkTest, AReplyCutShortByTheRunnersEndIsUnavailable) {
  const RawRunner runner(root_);
  const ProgramRunnerLink link(root_, 2000ms);
  std::thread serve([&] {
    runner.Serve({std::string("\0{\"api_version\":", 15U)});
  });
  try {
    static_cast<void>(link.Call(Request("runs")));
    FAIL() << "a cut reply was returned";
  } catch (const GatewayError &error) {
    EXPECT_EQ(error.code(), "program_runner_unavailable");
    EXPECT_EQ(TransportError(error), "closed_mid_reply");
  }
  serve.join();
}

TEST_F(ProgramLinkTest, ARunnerThatEndsWithoutAReplyIsUnavailable) {
  const RawRunner runner(root_);
  const ProgramRunnerLink link(root_, 2000ms);
  std::thread serve([&] { runner.Serve({}); });
  try {
    static_cast<void>(link.Call(Request("runs")));
    FAIL() << "a reply was returned";
  } catch (const GatewayError &error) {
    EXPECT_EQ(TransportError(error), "closed_without_reply");
  }
  serve.join();
}

TEST_F(ProgramLinkTest, ASecondServerOfASocketIsRefusedByCode) {
  ProgramCommandServer first(root_);
  first.Start();
  ProgramCommandServer second(root_);
  try {
    second.Start();
    FAIL() << "a second server took the socket";
  } catch (const GatewayError &error) {
    EXPECT_EQ(error.code(), "program_runner_socket_unavailable");
    EXPECT_EQ(error.producer(), "program_runner");
  }
  // The first keeps serving its socket.
  const ProgramRunnerLink link(root_, 2000ms);
  std::string answered;
  std::thread client([&] { answered = link.Call(Request("health")); });
  const auto received = first.Next(5000ms);
  ASSERT_TRUE(received.has_value());
  first.Reply(received->token, "{\"ok\":true,\"value\":null}");
  client.join();
  EXPECT_EQ(answered, "{\"ok\":true,\"value\":null}");

  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  ProgramStatusPublisher other(root_);
  EXPECT_THROW(other.Start(), GatewayError);

  // Once the first has ended, a new server takes the socket.
  first.Close();
  ProgramCommandServer next(root_);
  EXPECT_NO_THROW(next.Start());
}

TEST_F(ProgramLinkTest, NoRunnerIsUnavailable) {
  const ProgramRunnerLink link(root_, 100ms);
  try {
    static_cast<void>(link.Call(Request("health")));
    FAIL() << "the call succeeded without a runner";
  } catch (const GatewayError &error) {
    EXPECT_EQ(error.code(), "program_runner_unavailable");
    EXPECT_EQ(error.producer(), "program_runner");
  }
}

std::string Frame(ProgramRunnerLink::Subscription &subscription) {
  const auto message = subscription.Next(5000ms);
  EXPECT_TRUE(message.has_value());
  return message.value_or("");
}

std::string Expected(const std::string &event_class, const std::string &cycle,
                     const int sequence) {
  return "{\"api_version\":\"manyforge.program_status_stream.v1\",\"frame\":"
         "{\"event_class\":\"" +
         event_class + "\",\"program_cycle\":" + cycle +
         ",\"schema_version\":\"manyforge.program_control_state.v4\","
         "\"server_sequence\":\"" +
         std::to_string(sequence) + "\"},\"type\":\"frame\"}";
}

TEST_F(ProgramLinkTest, StatusFirstFrameAtOnceThenNewestWithItsClass) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  publisher.Publish("{\"n\":1}", false);
  auto subscription = link.SubscribeStatus();
  // The first frame of a connection is critical.
  EXPECT_EQ(Frame(*subscription), Expected("critical", "{\"n\":1}", 1));
  publisher.Publish("{\"n\":2}", false);
  EXPECT_EQ(Frame(*subscription), Expected("node_progress", "{\"n\":2}", 2));
  publisher.Publish("{\"n\":3}", true);
  publisher.Publish("{\"n\":4}", false);
  // Either both frames, or only the newest one, which then carries the
  // critical change it replaced.
  const auto third = Frame(*subscription);
  if (third == Expected("critical", "{\"n\":3}", 3))
    EXPECT_EQ(Frame(*subscription), Expected("node_progress", "{\"n\":4}", 4));
  else
    EXPECT_EQ(third, Expected("critical", "{\"n\":4}", 3));
  // A second subscriber starts its own sequence with the newest value.
  auto second = link.SubscribeStatus();
  EXPECT_EQ(Frame(*second), Expected("critical", "{\"n\":4}", 1));
  EXPECT_FALSE(subscription->Next(0ms).has_value());
}

TEST_F(ProgramLinkTest, WithdrawEndsSubscriptionsWithItsError) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  publisher.Publish("{}", true);
  auto subscription = link.SubscribeStatus();
  static_cast<void>(Frame(*subscription));
  publisher.Withdraw("program_cycle_journal_invalid", "the journal is unreadable");
  const auto error = Frame(*subscription);
  EXPECT_NE(error.find("\"type\":\"error\""), std::string::npos);
  EXPECT_NE(error.find("program_cycle_journal_invalid"), std::string::npos);
  try {
    static_cast<void>(subscription->Next(5000ms));
    FAIL() << "the subscription did not end";
  } catch (const GatewayError &failure) {
    EXPECT_EQ(failure.code(), "program_runner_unavailable");
  }
}

TEST_F(ProgramLinkTest, AStatusFrameAboveOneMebibyteArrivesWhole) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  const std::string cycle = "{\"text\":\"" + std::string(3U * 1024U * 1024U, 'x') + "\"}";
  publisher.Publish(cycle, true);
  auto subscription = link.SubscribeStatus();
  EXPECT_EQ(Frame(*subscription), Expected("critical", cycle, 1));
  publisher.Publish("{\"n\":2}", false);
  EXPECT_EQ(Frame(*subscription), Expected("node_progress", "{\"n\":2}", 2));
}

TEST_F(ProgramLinkTest, AStatusFrameAboveItsBoundEndsTheSubscription) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  publisher.Publish("\"" + std::string(kMaximumProgramStatusMessageBytes, 'x') + "\"",
                    true);
  auto subscription = link.SubscribeStatus();
  const auto error = Frame(*subscription);
  EXPECT_NE(error.find("program_runner_response_oversized"), std::string::npos);
}

TEST_F(ProgramLinkTest, ASubscriberAfterAWithdrawAsksForAFreshRead) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  EXPECT_FALSE(publisher.WaitReadRequest(0ms));
  publisher.RequestRead();
  EXPECT_TRUE(publisher.WaitReadRequest(0ms));
  EXPECT_FALSE(publisher.WaitReadRequest(0ms));
  publisher.Publish("{\"n\":1}", true);
  publisher.Withdraw("program_cycle_journal_invalid", "the journal is unreadable");
  // The withdrawn value is not sent to a new subscriber: it asks for a read.
  auto subscription = link.SubscribeStatus();
  EXPECT_TRUE(publisher.WaitReadRequest(5000ms));
  EXPECT_FALSE(subscription->Next(0ms).has_value());
  publisher.Publish("{\"n\":2}", false);
  EXPECT_EQ(Frame(*subscription), Expected("critical", "{\"n\":2}", 1));
  // A later withdraw ends each subscription it reaches with its error.
  auto second = link.SubscribeStatus();
  EXPECT_EQ(Frame(*second), Expected("critical", "{\"n\":2}", 1));
  publisher.Withdraw("program_cycle_journal_invalid", "still unreadable");
  EXPECT_NE(Frame(*second).find("still unreadable"), std::string::npos);
  publisher.Close();
  EXPECT_FALSE(publisher.WaitReadRequest(std::nullopt));
}

TEST_F(ProgramLinkTest, FifthSubscriberIsRefused) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  publisher.Publish("{}", true);
  std::vector<std::unique_ptr<ProgramRunnerLink::Subscription>> admitted;
  for (std::size_t index = 0U; index < kMaximumProgramStatusSubscribers; ++index) {
    admitted.push_back(link.SubscribeStatus());
    // Its first frame proves it holds a writer.
    static_cast<void>(Frame(*admitted.back()));
  }
  auto refused = link.SubscribeStatus();
  const auto error = Frame(*refused);
  EXPECT_NE(error.find("\"type\":\"error\""), std::string::npos);
  EXPECT_NE(error.find("program_runner_unavailable"), std::string::npos);
}

TEST_F(ProgramLinkTest, GoneSubscribersFreeTheirPlacesWithoutAPublication) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  publisher.Publish("{}", true);
  for (int round = 0; round < 3; ++round) {
    std::vector<std::unique_ptr<ProgramRunnerLink::Subscription>> admitted;
    for (std::size_t index = 0U; index < kMaximumProgramStatusSubscribers;
         ++index) {
      // A writer sees its subscriber's hangup at once, but on its own
      // thread: a subscription made before that is refused and retried, as
      // Headless retries.
      for (int attempt = 0;; ++attempt) {
        ASSERT_LT(attempt, 100000);
        auto subscription = link.SubscribeStatus();
        if (Frame(*subscription).find("\"type\":\"frame\"") != std::string::npos) {
          admitted.push_back(std::move(subscription));
          break;
        }
      }
    }
  }
}

TEST_F(ProgramLinkTest, LocalCloseEndsAWaitingNext) {
  ProgramStatusPublisher publisher(root_);
  publisher.Start();
  const ProgramRunnerLink link(root_, 2000ms);
  auto subscription = link.SubscribeStatus();
  std::optional<std::string> code;
  std::thread reader([&] {
    try {
      static_cast<void>(subscription->Next(60000ms));
    } catch (const GatewayError &error) {
      code = error.code();
    }
  });
  subscription->Close();
  reader.join();
  EXPECT_EQ(code, "program_runner_unavailable");
}

}  // namespace
}  // namespace manyforge::headless::gateway
