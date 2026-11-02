#include "../src/io_provider_lifecycle_product.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_msgs/msg/bool.hpp>
#include <thread>

namespace manyforge::roboplan_backend::host_detail {
namespace {

using core::ActionOutcome;
using core::ComponentAction;
using core::ComponentLifecycle;
const core::ContentDigest kDigest("sha256:" + std::string(64U, 'a'));
const core::ControlDomainId kDomain("io.domain");
const core::RuntimeUnitId kUnit("shared.host");
const core::InterruptSourceId kSource("io.source");
const core::IoSignalId kSignal{core::ControllableComponentId("io.device"), "ready"};

class CapturingIngress final : public core::IIoSignalIngress {
public:
  bool TryEnqueue(const core::IoSourceAcquisitionDTO &frame) noexcept override {
    frames.push_back(frame);
    return true;
  }
  void
  NotifyLoss(const core::IoIngressLossNotificationDTO &loss) noexcept override {
    losses.push_back(loss);
  }
  std::vector<core::IoSourceAcquisitionDTO> frames;
  std::vector<core::IoIngressLossNotificationDTO> losses;
};

/// The generic I/O driver as the component adapter of its I/O provider
/// component (CONTRACTS §6): state mapping and the four lifecycle actions.
class IoProviderComponentAdapterTest : public ::testing::Test {
protected:
  void SetUp() override { driver = MakeDriver(); }

  std::shared_ptr<IoProviderDriverRecord> MakeDriver() {
    const core::ProviderManifestId manifest_id{std::string(kIoProviderManifestId)};
    const core::ProviderManifestRevision manifest_revision(kDigest.value());
    core::ProviderControlDomainDTO provider{kSignal.controllable_component_id,
                                            kUnit,
                                            core::ProviderInstanceId("io.provider"),
                                            manifest_id,
                                            manifest_revision,
                                            kDigest,
                                            kDigest,
                                            {kSignal.name},
                                            30'000'000'000LL};
    core::ResolvedLifecycleControlDomainScopeDTO scope{
        kDomain, core::ControlDomainRevision(kDigest.value()),
        core::ResolvedProviderDomainScopeDTO{provider, core::RuntimeEpoch("runtime.1"),
                                             std::nullopt, {}}};
    core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
        kDomain, scope.control_domain_revision, provider.provider_instance_id,
        manifest_id, manifest_revision, kUnit, core::RuntimeEpoch("runtime.1"), kDigest};
    const core::ProviderManifestDTO manifest{manifest_id, manifest_revision, {}, {},
                                             std::nullopt};
    auto created = IoProviderDriverRecord::Create(scope, occurrence, manifest, clock);
    EXPECT_TRUE(created.ok()) << created.error().message;
    return created.ok() ? std::move(created.value()) : nullptr;
  }

  void InstallMockInput() {
    const core::IoSignalDescriptorDTO descriptor{
        kSignal,
        kSource,
        core::IoSignalValueType::kBoolean,
        core::IoSignalDirection::kInput,
        core::IoSignalUpdateMode::kChangeOnly,
        core::IoTransitionCapability::kSampledOnly,
        {core::IoFreshnessMode::kProviderValidity, std::nullopt},
        std::nullopt,
        core::IoSignalLivenessMode::kInProcess};
    deployment::IoSignalProviderConfiguration provider{"mock", {}, {}, false, 8};
    IoProviderInputSourceInstallation input{
        {{kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"), descriptor,
          provider}},
        {},
        ingress,
        core::IoSourceIncarnation("apply.1"),
        std::nullopt};
    auto lock = driver->AcquireDelivery();
    const auto installed = driver->InstallResourcesLocked({input}, {}, [] {});
    ASSERT_TRUE(installed.ok()) << installed.error().message;
  }

  core::ActionResult Run(ComponentAction action) { return driver->Run(action, {}); }

  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<CapturingIngress> ingress = std::make_shared<CapturingIngress>();
  std::shared_ptr<IoProviderDriverRecord> driver;
};

TEST_F(IoProviderComponentAdapterTest, NotPreparedUntilResourcesAreInstalled) {
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kNotPrepared);
  EXPECT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kFailed);
}

TEST_F(IoProviderComponentAdapterTest, PrepareEnableDisableFollowTheDriver) {
  InstallMockInput();
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kPrepared);
  ASSERT_EQ(Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  const auto enabled = driver->State(clock->now());
  EXPECT_EQ(enabled.lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_EQ(enabled.activity, core::ComponentActivity::kIdle);
  // Enable resumes the Mock input: its baseline reaches the ingress.
  ASSERT_EQ(ingress->frames.size(), 1U);
  // CONTRACTS §2: already enabled, Enable succeeds at once.
  EXPECT_EQ(Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(ingress->frames.size(), 1U);
  ASSERT_EQ(Run(ComponentAction::kDisable).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kPrepared);
}

TEST_F(IoProviderComponentAdapterTest, EnableNeedsPrepare) {
  InstallMockInput();
  const auto refused = Run(ComponentAction::kEnable);
  EXPECT_EQ(refused.outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "io_provider_lifecycle_unavailable");
}

TEST_F(IoProviderComponentAdapterTest, ExitedProcessIsAProcessExitedFault) {
  InstallMockInput();
  driver->SetSupervisedProcessFaultSource([] {
    return std::optional<core::ErrorDTO>(
        core::ErrorDTO{"simulated_plc_exited", "The simulated PLC exited", {}});
  });
  const auto state = driver->State(clock->now());
  ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->code, core::FaultCode::kProcessExited);
  EXPECT_EQ(state.fault->message, "The simulated PLC exited");
  EXPECT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kFailed);
}

core::StreamLivenessStateDTO StreamState(const std::string &stream_id) {
  for (const auto &row :
       host_component_stream_health()->Read(kSignal.controllable_component_id))
    if (row.stream_id == stream_id) return row.state;
  return core::StreamLivenessStateDTO::kAbsent;
}

// Declares one stream of the component, has the registry classify a sample
// live and waits until the stream reads stale. The host registry runs on the
// wall clock with a 1 ms budget: a sample classified after its budget never
// goes live, so this records until one does.
std::unique_ptr<core::StreamRecorder> StaleStream(const std::string &stream_id,
                                                  core::StreamSeverityDTO severity) {
  const auto health = host_component_stream_health();
  auto declared = health->Declare(
      kSignal.controllable_component_id,
      {stream_id, 1'000'000, severity, core::StreamRecoveryDTO::kAuto,
       core::StreamTimestampSourceDTO::kArrival});
  EXPECT_TRUE(declared.ok());
  auto recorder = std::move(declared.value());
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (StreamState(stream_id) != core::StreamLivenessStateDTO::kLive &&
         std::chrono::steady_clock::now() < give_up) {
    recorder->RecordArrival();
    health->Evaluate();
  }
  while (StreamState(stream_id) != core::StreamLivenessStateDTO::kStale &&
         std::chrono::steady_clock::now() < give_up) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    health->Evaluate();
  }
  return recorder;
}

TEST_F(IoProviderComponentAdapterTest, StaleWarningStreamIsShownNotAFault) {
  // A pose source's viewer overlay (`warning`) going stale never faults.
  InstallMockInput();
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  const auto stream = StaleStream("poses/robot/viewer.measured_robot",
                                  core::StreamSeverityDTO::kWarning);
  const auto state = driver->State(clock->now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_FALSE(state.fault.has_value());
  ASSERT_EQ(state.streams.size(), 1U);
  EXPECT_EQ(state.streams[0].freshness, core::StreamFreshness::kStale);
}

TEST_F(IoProviderComponentAdapterTest, StaleFaultStreamIsAStreamStaleFault) {
  InstallMockInput();
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  const auto stream = StaleStream("ready", core::StreamSeverityDTO::kFault);
  const auto state = driver->State(clock->now());
  ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->code, core::FaultCode::kStreamStale);
}

TEST_F(IoProviderComponentAdapterTest, InputOnlyComponentOffersNoManualWrite) {
  InstallMockInput();
  EXPECT_TRUE(driver->capabilities().empty());
}

bool HasStream(const core::ComponentState &state, const std::string_view stream_id) {
  return std::any_of(state.streams.begin(), state.streams.end(),
                     [&](const auto &stream) { return stream.stream_id == stream_id; });
}

TEST_F(IoProviderComponentAdapterTest, MockInputsDeclareNoInputsStream) {
  // IO-1: Mock inputs have no sender, so a budget row declares nothing.
  InstallMockInput();
  auto lock = driver->AcquireDelivery();
  ASSERT_TRUE(driver->SetInputsStreamLocked(1'000'000'000).ok());
  lock.unlock();
  EXPECT_FALSE(HasStream(driver->State(clock->now()), kIoDeviceInputsStreamId));
}

/// IO-1: a generic ROS I/O device's senders (the publishers of its input
/// topics) and its optional `inputs` silence stream.
class IoDeviceSendersTest : public IoProviderComponentAdapterTest {
protected:
  void SetUp() override {
    IoProviderComponentAdapterTest::SetUp();
    context = std::make_shared<rclcpp::Context>();
    context->init(0, nullptr);
    rclcpp::NodeOptions options;
    options.context(context);
    node = std::make_shared<rclcpp::Node>("manyforge_io_senders_test", options);
    rclcpp::ExecutorOptions executor_options;
    executor_options.context = context;
    executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>(executor_options);
    executor->add_node(node);
    topic = std::string("/io_senders_test/") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
  }
  void TearDown() override {
    for (const auto &record : {driver, reinstalled})
      if (record) record->Shutdown();
    executor->remove_node(node);
    executor.reset();
    sender.reset();
    driver.reset();
    reinstalled.reset();
    node.reset();
    context->shutdown("io senders fixture complete");
  }

  void InstallRosInput(const std::shared_ptr<IoProviderDriverRecord> &record) {
    const core::IoSignalDescriptorDTO descriptor{
        kSignal,
        kSource,
        core::IoSignalValueType::kBoolean,
        core::IoSignalDirection::kInput,
        core::IoSignalUpdateMode::kChangeOnly,
        core::IoTransitionCapability::kSampledOnly,
        {core::IoFreshnessMode::kProviderValidity, std::nullopt},
        std::nullopt,
        core::IoSignalLivenessMode::kChangeOnly};
    deployment::IoSignalProviderConfiguration provider{"ros_bool", topic, {}, {}, 8};
    IoProviderInputSourceInstallation input{
        {{kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"), descriptor,
          provider}},
        node,
        ingress,
        core::IoSourceIncarnation("apply.1"),
        std::nullopt};
    auto lock = record->AcquireDelivery();
    const auto installed = record->InstallResourcesLocked({input}, {}, [] {});
    ASSERT_TRUE(installed.ok()) << installed.error().message;
  }

  bool WaitFor(const std::function<bool()> &condition) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!condition() && std::chrono::steady_clock::now() < deadline) {
      if (sender) sender->publish(std_msgs::msg::Bool());
      executor->spin_some(std::chrono::milliseconds(5));
      host_component_stream_health()->Evaluate();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
  }
  void StartSender() {
    sender = node->create_publisher<std_msgs::msg::Bool>(topic, 8);
    ASSERT_TRUE(WaitFor([&] { return node->count_publishers(topic) == 1U; }));
  }
  void StopSender() {
    sender.reset();
    ASSERT_TRUE(WaitFor([&] { return node->count_publishers(topic) == 0U; }));
  }
  void PrepareAndEnable(IoProviderDriverRecord &record) {
    ASSERT_EQ(record.Run(ComponentAction::kPrepare, {}).outcome, ActionOutcome::kSucceeded);
    ASSERT_EQ(record.Run(ComponentAction::kEnable, {}).outcome, ActionOutcome::kSucceeded);
  }

  std::shared_ptr<rclcpp::Context> context;
  rclcpp::Node::SharedPtr node;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  rclcpp::Publisher<std_msgs::msg::Bool>::SharedPtr sender;
  std::shared_ptr<IoProviderDriverRecord> reinstalled;
  std::string topic;
};

TEST_F(IoDeviceSendersTest, SendersGoneFaultsAnEnabledDevice) {
  InstallRosInput(driver);
  StartSender();
  PrepareAndEnable(*driver);
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kEnabled);
  StopSender();
  const auto state = driver->State(clock->now());
  ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->code, core::FaultCode::kCommunication);
  EXPECT_EQ(state.fault->operator_action, core::FaultOperatorAction::kRecover);
  // Recover needs a sender back.
  const auto refused = Run(ComponentAction::kRecover);
  EXPECT_EQ(refused.outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "io_device_senders_gone");
}

TEST_F(IoDeviceSendersTest, FaultIsHeldUntilRecoverAfterSendersReturn) {
  InstallRosInput(driver);
  StartSender();
  PrepareAndEnable(*driver);
  StopSender();
  ASSERT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kFault);
  StartSender();
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(Run(ComponentAction::kEnable).outcome, ActionOutcome::kFailed);
  ASSERT_EQ(Run(ComponentAction::kRecover).outcome, ActionOutcome::kSucceeded);
  const auto state = driver->State(clock->now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_FALSE(state.fault.has_value());
  ASSERT_EQ(Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
}

TEST_F(IoDeviceSendersTest, NeverHadSendersIsNoFault) {
  // A change-only device whose sender hasn't appeared yet.
  InstallRosInput(driver);
  PrepareAndEnable(*driver);
  const auto state = driver->State(clock->now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_FALSE(state.fault.has_value());
}

TEST_F(IoDeviceSendersTest, SilenceAloneIsNoFault) {
  // Change-only: a present sender that sends nothing is healthy.
  InstallRosInput(driver);
  StartSender();
  PrepareAndEnable(*driver);
  const auto publishing = std::move(sender);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kEnabled);
}

TEST_F(IoDeviceSendersTest, DeviceNotEnabledIsOfflineWhileSendersAreGone) {
  InstallRosInput(driver);
  StartSender();
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kPrepared);
  StopSender();
  const auto state = driver->State(clock->now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kOffline);
  ASSERT_TRUE(state.reason.has_value());
  EXPECT_EQ(state.reason->code, "io_device_senders_gone");
  EXPECT_EQ(Run(ComponentAction::kEnable).error->code, "io_device_senders_gone");
  StartSender();
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kPrepared);
}

TEST_F(IoDeviceSendersTest, ReinstalledDeviceCountsOnlyAfterItsInstall) {
  // An Apply restarting the simulated sender: the replaced device (prepared,
  // not enabled) shows offline, and the new install starts uncounted.
  InstallRosInput(driver);
  StartSender();
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kPrepared);
  StopSender();
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kOffline);
  driver->Shutdown();
  reinstalled = MakeDriver();
  ASSERT_TRUE(reinstalled);
  InstallRosInput(reinstalled);
  PrepareAndEnable(*reinstalled);
  EXPECT_EQ(reinstalled->State(clock->now()).lifecycle, ComponentLifecycle::kEnabled);
  StartSender();
  const auto state = reinstalled->State(clock->now());
  EXPECT_EQ(state.lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_FALSE(state.fault.has_value());
}

TEST_F(IoDeviceSendersTest, InputsStreamIsDeclaredOnlyWithABudgetRow) {
  InstallRosInput(driver);
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->SetInputsStreamLocked(std::nullopt).ok());
  }
  EXPECT_FALSE(HasStream(driver->State(clock->now()), kIoDeviceInputsStreamId));
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->SetInputsStreamLocked(1'000'000'000).ok());
  }
  EXPECT_TRUE(HasStream(driver->State(clock->now()), kIoDeviceInputsStreamId));
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->SetInputsStreamLocked(std::nullopt).ok());
  }
  EXPECT_FALSE(HasStream(driver->State(clock->now()), kIoDeviceInputsStreamId));
}

TEST_F(IoDeviceSendersTest, SilentInputsStreamIsAStreamStaleFault) {
  InstallRosInput(driver);
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->SetInputsStreamLocked(50'000'000).ok());
  }
  StartSender();
  // Prepared, not enabled: every message is an arrival all the same.
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  const auto fresh = [&] {
    for (const auto &stream : driver->State(clock->now()).streams)
      if (stream.stream_id == kIoDeviceInputsStreamId)
        return stream.freshness == core::StreamFreshness::kFresh;
    return false;
  };
  ASSERT_TRUE(WaitFor(fresh));
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kPrepared);
  // The sender stays but goes silent.
  const auto silent = std::move(sender);
  ASSERT_TRUE(WaitFor([&] {
    return driver->State(clock->now()).lifecycle == ComponentLifecycle::kFault;
  }));
  const auto state = driver->State(clock->now());
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->code, core::FaultCode::kStreamStale);
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail
