#include "../src/io_provider_lifecycle_product.hpp"

#include <gtest/gtest.h>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <future>

#include <algorithm>
#include <chrono>
#include <functional>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_srvs/srv/set_bool.hpp>
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
/// component: state mapping and the four lifecycle actions.
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

  void InstallMockInput(IoProviderDriverRecord::InputLifecycleHook hook = {}) {
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
    const auto installed = driver->InstallResourcesLocked(lock, {input}, {}, [] {}, std::move(hook));
    ASSERT_TRUE(installed.ok()) << installed.error().message;
  }

  core::ActionResult Run(ComponentAction action) { return driver->Run(action, {}); }

  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<CapturingIngress> ingress = std::make_shared<CapturingIngress>();
  std::shared_ptr<IoProviderDriverRecord> driver;
};

TEST_F(IoProviderComponentAdapterTest, LifecycleHookDoesNotHoldDeliveryMutex) {
  std::promise<void> read_done;
  auto read_future = read_done.get_future();
  std::thread reader;
  bool read_during_hook = false;
  InstallMockInput([&](bool) {
    reader = std::thread([&] { static_cast<void>(driver->State(clock->now())); read_done.set_value(); });
    read_during_hook = read_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    return core::Result<void>::Ok();
  });
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  const auto result = Run(ComponentAction::kEnable);
  if (reader.joinable()) reader.join();
  EXPECT_TRUE(read_during_hook);
  EXPECT_EQ(result.outcome, ActionOutcome::kSucceeded);
}

TEST_F(IoProviderComponentAdapterTest, FailedDisableClosesThenRestoresOutputAdmission) {
  std::optional<SetOutputResult> during_disable;
  InstallMockInput([&](bool disabling) {
    if (!disabling) return core::Result<void>::Ok();
    during_disable = driver->SetOutput(kSignal.name, true, {});
    return core::Result<void>::Err({"test_disable_refused", "no effect", {}});
  });
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(Run(ComponentAction::kEnable).outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(Run(ComponentAction::kDisable).outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(during_disable);
  ASSERT_TRUE(during_disable->error);
  EXPECT_EQ(during_disable->error->code, "io_output_not_submitted");
  const auto after = driver->SetOutput(kSignal.name, true, {});
  ASSERT_TRUE(after.error);
  EXPECT_EQ(after.error->code, "io_signal_unknown");
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kEnabled);
}

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
  // Already enabled, Enable succeeds at once.
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
  EXPECT_FALSE(driver->RosOwner());
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

  void InstallRosInput(const std::shared_ptr<IoProviderDriverRecord> &record,
                       std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner = {},
                       IoProviderDriverRecord::InputLifecycleHook lifecycle = {}) {
    watched = record;
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
        owner ? owner->node() : node,
        ingress,
        core::IoSourceIncarnation("apply.1"),
        std::nullopt};
    auto lock = record->AcquireDelivery();
    const auto installed = record->InstallResourcesLocked(
        lock, {input}, {}, [] {}, std::move(lifecycle), {}, owner);
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
  // The provider learns of a sender from the matched event, which this
  // fixture's executor delivers for inputs on its own node.
  void StartSender() {
    sender = node->create_publisher<std_msgs::msg::Bool>(topic, 8);
    ASSERT_TRUE(WaitFor([&] { return watched->RosInputMatchedPublishersForTest() >= 1U; }));
  }
  void StopSender() {
    sender.reset();
    ASSERT_TRUE(WaitFor([&] { return watched->RosInputMatchedPublishersForTest() == 0U; }));
  }
  // For inputs hosted on an owner node: the match is delivered on that
  // owner's executor, which these tests don't spin, so only the publisher is
  // created and the caller waits for the sender side it needs.
  void CreateSender() {
    sender = node->create_publisher<std_msgs::msg::Bool>(topic, 8);
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
  // The record whose ROS input the last InstallRosInput installed.
  std::shared_ptr<IoProviderDriverRecord> watched;
  std::string topic;
};

TEST_F(IoDeviceSendersTest, RosOwnerRecoveryKeepsDriverAndReturnsPrepared) {
  auto old = ros2_control_execution::Ros2TransportResources::create(
      "io_driver_recovery_old", "", 1U, [this] { return context; });
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "io_driver_recovery_new", "", 1U, [this] { return context; });
  EXPECT_FALSE(driver->RosOwner());
  InstallRosInput(driver, old);
  EXPECT_EQ(driver->RosOwner(), old);
  CreateSender();
  PrepareAndEnable(*driver);
  auto closed = driver->CloseAndCaptureRosOwnerRecovery(old);
  ASSERT_TRUE(closed.ok());
  EXPECT_EQ(driver->Run(ComponentAction::kEnable, {}).outcome,
            ActionOutcome::kFailed);
  auto staged = IoProviderDriverRecord::StageRosOwnerRecovery(closed.value(),
                                                              replacement);
  ASSERT_TRUE(staged.ok());
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->MatchesRosOwnerRecoveryLocked(*closed.value()));
    driver->CommitRosOwnerRecoveryLocked(*staged.value());
    EXPECT_FALSE(driver->MatchesRosOwnerRecoveryLocked(*closed.value()));
  }
  IoProviderDriverRecord::ActivateRosOwnerRecovery(*staged.value());
  EXPECT_EQ(driver->RosOwner(), replacement);
  EXPECT_EQ(driver->State(clock->now()).lifecycle,
            core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(driver->Run(ComponentAction::kEnable, {}).outcome,
            ActionOutcome::kSucceeded);
  driver->Shutdown();
  EXPECT_FALSE(driver->RosOwner());
}

TEST_F(IoDeviceSendersTest,
       RecoverAfterUnknownWriteAndRebuildAcceptsWithoutLatchOrResend) {
  using Service = std_srvs::srv::SetBool;
  const core::ResourceId resource("output.test");
  const core::ProviderManifestId manifest_id{
      std::string(kIoProviderManifestId)};
  const core::ProviderManifestRevision manifest_revision(kDigest.value());
  core::ProviderControlDomainDTO provider{
      kSignal.controllable_component_id,
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
      core::ResolvedProviderDomainScopeDTO{
          provider, core::RuntimeEpoch("runtime.1"), std::nullopt, {resource}}};
  core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
      kDomain,
      scope.control_domain_revision,
      provider.provider_instance_id,
      manifest_id,
      manifest_revision,
      kUnit,
      core::RuntimeEpoch("runtime.1"),
      kDigest};
  const core::ProviderManifestDTO manifest{
      manifest_id, manifest_revision, {}, {}, std::nullopt};
  auto created =
      IoProviderDriverRecord::Create(scope, occurrence, manifest, clock);
  ASSERT_TRUE(created.ok());
  driver = std::move(created.value());
  auto old = ros2_control_execution::Ros2TransportResources::create(
      "unknown_output_old", "", 1U, [this] { return context; });
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "unknown_output_new", "", 1U, [this] { return context; });
  unsigned requests = 0;
  auto server = node->create_service<Service>(
      "/test/unknown_output",
      [&](Service::Request::SharedPtr, Service::Response::SharedPtr response) {
        ++requests;
        response->success = true;
      });
  auto leaf = RosBooleanOutputProvider::Create(
      old->node(),
      {resource,
       RosSetBoolOutputAddress{"/test/unknown_output"},
       {{kSignal, occurrence, 1U, driver}},
       core::ActiveBundlePublicationGeneration(1U),
       500'000'000},
      {}, old);
  ASSERT_TRUE(leaf.ok());
  auto output =
      std::shared_ptr<RosBooleanOutputProvider>(std::move(leaf.value()));
  const core::IoSignalDescriptorDTO descriptor{
      kSignal,
      kSource,
      core::IoSignalValueType::kBoolean,
      core::IoSignalDirection::kOutput,
      core::IoSignalUpdateMode::kChangeOnly,
      core::IoTransitionCapability::kSampledOnly,
      {core::IoFreshnessMode::kProviderValidity, std::nullopt},
      resource};
  deployment::IoSignalProviderConfiguration config;
  config.profile = "ros_set_bool";
  config.service = "/test/unknown_output";
  IoProviderOutputChannelInstallation installed{
      resource,
      {{kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"),
        descriptor, config}},
      output};
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(
        driver->InstallResourcesLocked(
                  lock, {}, {installed}, [] {}, {}, {}, old)
            .ok());
  }
  ASSERT_TRUE(WaitFor([&] { return output->ServiceReady(); }));
  PrepareAndEnable(*driver);
  std::stop_source stop;
  stop.request_stop();
  const auto result = driver->SetOutput(kSignal.name, true, stop.get_token());
  // The write's typed outcome is returned; the component's activity is not
  // latched by an unknown output (OD-44: outputs rely on read-back).
  ASSERT_EQ(result.outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(driver->State(clock->now()).activity,
            core::ComponentActivity::kIdle);
  auto closed = driver->CloseAndCaptureRosOwnerRecovery(old);
  ASSERT_TRUE(closed.ok());
  auto output_snapshot = output->CaptureTransportRecovery();
  auto output_stage = RosBooleanOutputProvider::StageTransportRecovery(
      output_snapshot, replacement);
  ASSERT_TRUE(output_stage.ok());
  auto staged = IoProviderDriverRecord::StageRosOwnerRecovery(closed.value(),
                                                              replacement);
  ASSERT_TRUE(staged.ok());
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->MatchesRosOwnerRecoveryLocked(*closed.value()));
    ASSERT_TRUE(output->MatchesTransportRecovery(*output_snapshot));
    output->CommitTransportRecovery(*output_stage.value());
    driver->CommitRosOwnerRecoveryLocked(*staged.value());
  }
  replacement->start();
  ASSERT_TRUE(WaitFor([&] { return output->ServiceReady(); }));
  EXPECT_EQ(driver->State(clock->now()).activity,
            core::ComponentActivity::kIdle);
  EXPECT_EQ(driver->State(clock->now()).lifecycle,
            core::ComponentLifecycle::kPrepared);
  // Recover accepts directly after the rebuild: no latch, nothing re-sent.
  EXPECT_EQ(driver->Run(ComponentAction::kRecover, {}).outcome,
            ActionOutcome::kSucceeded);
  EXPECT_EQ(driver->State(clock->now()).activity,
            core::ComponentActivity::kIdle);
  EXPECT_FALSE(driver->State(clock->now()).fault.has_value());
  EXPECT_EQ(result.outcome, ActionOutcome::kUnknown);
  EXPECT_LE(requests, 1U);
  driver->Shutdown();
  replacement->quiesce();
  server.reset();
}

TEST_F(IoDeviceSendersTest, RosOwnerRecoveryRejectsChangedGeneration) {
  auto old = ros2_control_execution::Ros2TransportResources::create(
      "io_driver_stale_old", "", 1U, [this] { return context; });
  InstallRosInput(driver, old);
  auto closed = driver->CloseAndCaptureRosOwnerRecovery(old);
  ASSERT_TRUE(closed.ok());
  EXPECT_EQ(driver->Run(ComponentAction::kDisable, {}).outcome,
            ActionOutcome::kSucceeded);
  auto lock = driver->AcquireDelivery();
  EXPECT_FALSE(driver->MatchesRosOwnerRecoveryLocked(*closed.value()));
}

TEST_F(IoDeviceSendersTest, FaultedOwnerRefusesEnableCreationBeforeJoin) {
  using namespace std::chrono_literals;
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "io_faulted_owner_test", "", 1U, [this] { return context; });
  std::promise<void> entered;
  auto observed = entered.get_future();
  std::promise<void> release;
  auto resume_observer = release.get_future().share();
  owner->set_executor_fault_observer([&](std::string_view) {
    entered.set_value();
    resume_observer.wait();
  });
  InstallRosInput(driver, owner);
  CreateSender();
  ASSERT_TRUE(WaitFor([&] { return sender->get_subscription_count() == 1U; }));
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  owner->start();
  rclcpp::TimerBase::SharedPtr timer;
  owner->invoke_creation([&] {
    timer = owner->node()->create_wall_timer(
        1ms, [] { throw std::runtime_error("forced owner fault"); });
  });
  const bool fault_entered = observed.wait_for(5s) == std::future_status::ready;
  core::ActionResult result;
  std::size_t subscriptions_before = 0;
  std::size_t subscriptions_after = 0;
  if (fault_entered) {
    subscriptions_before = sender->get_subscription_count();
    result = Run(ComponentAction::kEnable);
    subscriptions_after = sender->get_subscription_count();
  }
  release.set_value();
  owner->quiesce();
  timer.reset();
  ASSERT_TRUE(fault_entered);
  EXPECT_EQ(result.outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "ros_transport_executor_failed");
  EXPECT_EQ(subscriptions_after, subscriptions_before);
  ASSERT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kFault);
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "io_faulted_owner_replacement", "", 1U, [this] { return context; });
  auto closed = driver->CloseAndCaptureRosOwnerRecovery(owner);
  ASSERT_TRUE(closed.ok());
  auto staged = IoProviderDriverRecord::StageRosOwnerRecovery(closed.value(),
                                                              replacement);
  ASSERT_TRUE(staged.ok());
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->MatchesRosOwnerRecoveryLocked(*closed.value()));
    driver->CommitRosOwnerRecoveryLocked(*staged.value());
  }
  IoProviderDriverRecord::ActivateRosOwnerRecovery(*staged.value());
  EXPECT_EQ(driver->State(clock->now()).lifecycle,
            ComponentLifecycle::kPrepared);
}

TEST_F(IoDeviceSendersTest, RecoveryPreservesUnrelatedFaultWithSameErrorCode) {
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "io_unrelated_fault_owner", "", 1U, [this] { return context; });
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "io_unrelated_fault_replacement", "", 1U, [this] { return context; });
  InstallRosInput(driver, owner, [](bool disabled) {
    if (disabled)
      return core::Result<void>::Ok();
    return core::Result<void>::Err({"ros_transport_executor_failed",
                                    "independent lifecycle failure",
                                    {{"io_source_transition_effects", "1"}}});
  });
  ASSERT_EQ(Run(ComponentAction::kPrepare).outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(Run(ComponentAction::kEnable).outcome, ActionOutcome::kFailed);
  auto closed = driver->CloseAndCaptureRosOwnerRecovery(owner);
  ASSERT_TRUE(closed.ok());
  auto staged = IoProviderDriverRecord::StageRosOwnerRecovery(closed.value(),
                                                              replacement);
  ASSERT_TRUE(staged.ok());
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->MatchesRosOwnerRecoveryLocked(*closed.value()));
    driver->CommitRosOwnerRecoveryLocked(*staged.value());
  }
  IoProviderDriverRecord::ActivateRosOwnerRecovery(*staged.value());
  const auto state = driver->State(clock->now());
  ASSERT_EQ(state.lifecycle, ComponentLifecycle::kFault);
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->message, "independent lifecycle failure");
}

TEST_F(IoDeviceSendersTest, RecoveryClearsMatchingOwnerFaultFromInputRebind) {
  using namespace std::chrono_literals;
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "io_rebind_fault_owner", "", 1U, [this] { return context; });
  auto replacement =
      ros2_control_execution::Ros2TransportResources::create(
          "io_rebind_fault_replacement", "", 1U, [this] { return context; });
  std::promise<void> faulted;
  auto observed = faulted.get_future();
  owner->set_executor_fault_observer(
      [&](std::string_view) { faulted.set_value(); });
  InstallRosInput(driver, owner);
  owner->start();
  rclcpp::TimerBase::SharedPtr timer;
  owner->invoke_creation([&] {
    timer = owner->node()->create_wall_timer(
        1ms, [] { throw std::runtime_error("forced owner fault"); });
  });
  ASSERT_EQ(observed.wait_for(5s), std::future_status::ready);
  owner->quiesce();
  timer.reset();
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
  deployment::IoSignalProviderConfiguration config{
      "ros_bool", topic, {}, {}, 8};
  IoProviderInputSourceInstallation input{
      {{kUnit, core::CommandTargetId("gpio"), core::ResourceRoleId("ready"),
        descriptor, config}},
      owner->node(),
      ingress,
      core::IoSourceIncarnation("apply.2"),
      std::nullopt};
  {
    auto lock = driver->AcquireDelivery();
    const auto rebound =
        driver->RebindInputsLocked(lock, {input}, [] {}, {}, {}, owner);
    ASSERT_FALSE(rebound.ok());
    EXPECT_EQ(rebound.error().code, "ros_transport_executor_failed");
  }
  ASSERT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kFault);
  auto closed = driver->CloseAndCaptureRosOwnerRecovery(owner);
  ASSERT_TRUE(closed.ok());
  auto staged = IoProviderDriverRecord::StageRosOwnerRecovery(closed.value(),
                                                              replacement);
  ASSERT_TRUE(staged.ok());
  {
    auto lock = driver->AcquireDelivery();
    ASSERT_TRUE(driver->MatchesRosOwnerRecoveryLocked(*closed.value()));
    driver->CommitRosOwnerRecoveryLocked(*staged.value());
  }
  IoProviderDriverRecord::ActivateRosOwnerRecovery(*staged.value());
  EXPECT_EQ(driver->State(clock->now()).lifecycle,
            ComponentLifecycle::kPrepared);
}

TEST_F(IoDeviceSendersTest, SendersGoneFaultsAnEnabledDevice) {
  InstallRosInput(driver);
  StartSender();
  PrepareAndEnable(*driver);
  EXPECT_EQ(driver->State(clock->now()).lifecycle, ComponentLifecycle::kEnabled);
  StopSender();
  ASSERT_TRUE(WaitFor([&] { return driver->State(clock->now()).lifecycle == ComponentLifecycle::kFault; }));
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
  ASSERT_TRUE(WaitFor([&] { return driver->State(clock->now()).lifecycle == ComponentLifecycle::kFault; }));
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
  ASSERT_TRUE(WaitFor([&] { return driver->State(clock->now()).lifecycle == ComponentLifecycle::kOffline; }));
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
  ASSERT_TRUE(WaitFor([&] { return driver->State(clock->now()).lifecycle == ComponentLifecycle::kOffline; }));
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
