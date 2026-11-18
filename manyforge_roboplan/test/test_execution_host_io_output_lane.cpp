// The execution host's output lane: `set_io_output` on a deferred lane of its
// own, served by the real command surface over its ordinary socket.
#include "execution_host_commands.hpp"
#include "execution_host_config.hpp"
#include "execution_host_surface.hpp"
#include "host_component_runtime.hpp"
#include "ros_host_composition.hpp"

#include <gtest/gtest.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/testing/request_key_fixture.hpp>
#include <manyforge_roboplan/execution_host_support.hpp>
#include <manyforge_roboplan/io_output_writer.hpp>
#include <manyforge_roboplan/retained_cell_runtime.hpp>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {
namespace {

using Value = content_identity::CanonicalValue;
// Only regression bounds for the test's own waits; the product has none.
constexpr auto kTestBound = std::chrono::seconds(10);

/// An I/O component whose writes block until the test releases them, in
/// order; it records the signal of every write that reached it.
class HeldOutputAdapter final : public core::IComponentAdapter,
                                public IIoOutputWriter {
public:
  [[nodiscard]] core::ComponentState State(std::int64_t) const override {
    core::ComponentState state;
    state.component_id = "plc";
    state.lifecycle = core::ComponentLifecycle::kEnabled;
    state.activity = core::ComponentActivity::kIdle;
    return state;
  }
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action,
                                       std::stop_token) override {
    core::ActionResult result;
    result.action = action;
    result.outcome = core::ActionOutcome::kSucceeded;
    return result;
  }
  [[nodiscard]] std::vector<core::Capability> capabilities() const override {
    return {core::Capability::kIoManualWrite};
  }
  [[nodiscard]] SetOutputResult SetOutput(std::string_view signal_name,
                                          std::variant<bool, double>,
                                          std::stop_token) override {
    std::unique_lock lock(mutex_);
    const auto index = started_.size();
    started_.emplace_back(signal_name);
    changed_.notify_all();
    changed_.wait(lock, [&] { return released_ > index; });
    return {core::ActionOutcome::kSucceeded, std::nullopt};
  }
  /// Waits until `count` writes reached the device.
  [[nodiscard]] bool WaitStarted(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kTestBound,
                             [&] { return started_.size() >= count; });
  }
  void ReleaseAll() {
    {
      std::lock_guard lock(mutex_);
      released_ = static_cast<std::size_t>(-1);
    }
    changed_.notify_all();
  }
  [[nodiscard]] std::vector<std::string> Started() const {
    std::lock_guard lock(mutex_);
    return started_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::string> started_;
  std::size_t released_{};
};

/// One client connection of the ordinary socket: one request, one answer.
class Client final {
public:
  Client(const std::filesystem::path &run_root, const std::string &request)
      : descriptor_(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0)) {
    if (descriptor_ < 0)
      throw std::runtime_error("create test client socket");
    const timeval bound{static_cast<time_t>(kTestBound.count()), 0};
    ::setsockopt(descriptor_, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound));
    const auto path = (run_root / std::string(kSocketFileName)).string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    if (::connect(descriptor_, reinterpret_cast<sockaddr *>(&address),
                  sizeof(address)) != 0 ||
        ::send(descriptor_, request.data(), request.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(request.size()))
      throw std::runtime_error("send test request");
  }
  ~Client() { ::close(descriptor_); }
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  /// The answer, or empty when the host closed the connection without one
  /// (or the test bound passed).
  [[nodiscard]] std::string Answer() const {
    std::array<char, 65536> buffer{};
    const auto count = ::recv(descriptor_, buffer.data(), buffer.size(), 0);
    return count > 0 ? std::string(buffer.data(), static_cast<std::size_t>(count))
                     : std::string{};
  }

private:
  int descriptor_;
};

std::string SetOutput(const std::string &signal) {
  return content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("set_io_output")},
      {"request", Value(Value::Object{{"origin", Value("program")},
                                      {"signal_id", Value("plc/" + signal)},
                                      {"value", Value(true)}})}}));
}

// An ordinary request the ordinary thread answers itself.
std::string ResetRuleActivation() {
  return content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("reset_rule_activation")}}));
}

bool Succeeded(const std::string &answer) {
  return answer.find("\"ok\":true") != std::string::npos &&
         answer.find("\"outcome\":\"succeeded\"") != std::string::npos;
}

bool RefusedWith(const std::string &answer, const std::string &code) {
  return answer.find("\"ok\":false") != std::string::npos &&
         answer.find("\"code\":\"" + code + "\"") != std::string::npos;
}

class ExecutionHostIoOutputLaneTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> next{0U};
    // POSIX socket paths are bounded: keep the run root short.
    root_ = std::filesystem::temp_directory_path() /
            ("mf-io-lane-" + std::to_string(::getpid()) + "-" +
             std::to_string(next.fetch_add(1U)));
    std::filesystem::create_directory(root_);
    const auto graph = core::GraphRevision("graph.io-output-lane");
    core::ResourceAddressedExecutionKernelConfig config{
        core::kTestApplicationStartId, core::CellId("cell.io-output-lane"), 32U, {},
        nullptr, graph};
    config.empty_runtime_retained_bundle_id =
        core::RetainedBundleId("bundle.io-output-lane");
    config.empty_runtime_graph_revision = graph;
    cell_ = std::make_unique<RetainedCellRuntime>(RetainedCellRuntimeInput{
        .clock = std::make_shared<WallClock>(),
        .scheduler = std::make_shared<SimpleScheduler>(),
        .operation_journal_path = root_ / "journal.sqlite",
        .participant_registrations = {},
        .kernel_config = std::move(config)});
    access_.addressed_kernel.store(&cell_->addressed_kernel(),
                                   std::memory_order_release);
    // The cell's component runtime, as the host installs it once; the test
    // feeds component states to the Kernel itself.
    access_.component_runtime =
        std::make_shared<HostComponentRuntime>(clock_, HostComponentRuntime::Hooks{});
    auto removed = access_.component_runtime->SetComponents(
        {core::CoordinatedComponent{"plc", adapter_}});
    ASSERT_TRUE(removed.empty());
    control_ = std::make_unique<ManagedExecutionSurfaceControl>(
        root_, &access_, &admission_, core::kTestApplicationStartId);
    surface_ = std::jthread([this](std::stop_token stop_token) {
      RunManagedExecutionCommandSurface(*control_, stop_token);
    });
    std::unique_lock lock(control_->state_mutex);
    ASSERT_TRUE(control_->state_changed.wait_for(lock, kTestBound, [&] {
      return control_->ready || control_->failure != nullptr;
    }));
    ASSERT_TRUE(control_->ready);
  }
  void TearDown() override {
    adapter_->ReleaseAll();
    if (surface_.joinable()) {
      surface_.request_stop();
      surface_.join();
    }
    access_.detach_addressed_owner();
    access_.component_runtime.reset();
    cell_.reset();
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  std::filesystem::path root_;
  WallClock clock_;
  std::unique_ptr<RetainedCellRuntime> cell_;
  std::shared_ptr<HeldOutputAdapter> adapter_ = std::make_shared<HeldOutputAdapter>();
  ExecutionCompositionAccess access_;
  component_manager::detail::ManagerLivenessCommandAdmission admission_;
  std::unique_ptr<ManagedExecutionSurfaceControl> control_;
  std::jthread surface_;
};

TEST_F(ExecutionHostIoOutputLaneTest, AHeldWriteDoesNotHoldUpOtherOrdinaryRequests) {
  Client held(root_, SetOutput("DO0"));
  ASSERT_TRUE(adapter_->WaitStarted(1U));
  // The ordinary thread accepts and answers the next request meanwhile.
  Client other(root_, ResetRuleActivation());
  EXPECT_FALSE(other.Answer().empty());
  adapter_->ReleaseAll();
  EXPECT_TRUE(Succeeded(held.Answer()));
}

TEST_F(ExecutionHostIoOutputLaneTest, WritesReachTheDeviceInArrivalOrder) {
  Client first(root_, SetOutput("DO0"));
  ASSERT_TRUE(adapter_->WaitStarted(1U));
  Client second(root_, SetOutput("DO1"));
  Client third(root_, SetOutput("DO2"));
  // Answered after the ordinary thread took both writes (one accept order).
  Client marker(root_, ResetRuleActivation());
  ASSERT_FALSE(marker.Answer().empty());
  // Queued writes are not sent while one is in flight.
  EXPECT_EQ(adapter_->Started(), (std::vector<std::string>{"DO0"}));
  adapter_->ReleaseAll();
  EXPECT_TRUE(Succeeded(first.Answer()));
  EXPECT_TRUE(Succeeded(second.Answer()));
  EXPECT_TRUE(Succeeded(third.Answer()));
  EXPECT_EQ(adapter_->Started(), (std::vector<std::string>{"DO0", "DO1", "DO2"}));
}

TEST_F(ExecutionHostIoOutputLaneTest, AFullLaneRefusesTheNextWriteWithItsBusyCode) {
  // One in flight and seven queued: the lane's bound of pending requests.
  std::vector<std::unique_ptr<Client>> pending;
  pending.push_back(std::make_unique<Client>(root_, SetOutput("DO0")));
  ASSERT_TRUE(adapter_->WaitStarted(1U));
  for (std::size_t index = 1U; index < kMaximumPendingDeferredCommands; ++index)
    pending.push_back(
        std::make_unique<Client>(root_, SetOutput("DO" + std::to_string(index))));
  Client refused(root_, SetOutput("DO9"));
  EXPECT_TRUE(RefusedWith(refused.Answer(), "execution_io_output_lane_busy"));
  adapter_->ReleaseAll();
  for (const auto &client : pending)
    EXPECT_TRUE(Succeeded(client->Answer()));
  const auto started = adapter_->Started();
  EXPECT_EQ(started.size(), kMaximumPendingDeferredCommands);
  EXPECT_EQ(std::count(started.begin(), started.end(), "DO9"), 0);
}

TEST_F(ExecutionHostIoOutputLaneTest,
       AWriteQueuedBeforeAHaltIsAdmittedWhenItStartsAndTheSentOneCompletes) {
  auto &kernel = cell_->addressed_kernel();
  Client sent(root_, SetOutput("DO0"));
  ASSERT_TRUE(adapter_->WaitStarted(1U));
  Client queued(root_, SetOutput("DO1"));
  Client marker(root_, ResetRuleActivation());
  ASSERT_FALSE(marker.Answer().empty());
  // A faulted component does not latch the idle cell; the Halt arms the
  // latch, so the Kernel now refuses output writes.
  core::ComponentState faulted;
  faulted.component_id = "plc";
  faulted.lifecycle = core::ComponentLifecycle::kFault;
  faulted.activity = core::ComponentActivity::kIdle;
  faulted.fault = core::Fault{core::FaultCode::kProtectiveStop, "protective stop", true,
                              core::FaultOperatorAction::kRecover, std::nullopt,
                              std::nullopt, {}, 0};
  kernel.observe_component_states({faulted});
  ASSERT_FALSE(kernel.get_cell_latch().latched);
  static_cast<void>(kernel.halt(core::FullStopIntentDTO{
      core::PublicRequestContextDTO{core::TestRequestKey("io-output-lane.halt"), "test"},
      {}}));
  ASSERT_TRUE(kernel.get_cell_latch().latched);
  adapter_->ReleaseAll();
  // The write already sent completes with its real outcome.
  EXPECT_TRUE(Succeeded(sent.Answer()));
  // The queued one was not sent: the Kernel refused it when it started.
  EXPECT_TRUE(RefusedWith(queued.Answer(), "cell_latched"));
  EXPECT_EQ(adapter_->Started(), (std::vector<std::string>{"DO0"}));
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail
