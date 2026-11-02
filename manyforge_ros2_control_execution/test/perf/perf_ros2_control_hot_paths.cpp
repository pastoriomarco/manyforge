// Report-only measurements through surviving public ROS APIs.
// Counts process-wide C++ new (including aligned new), not C/RMW malloc.
// Whole feedback delivery includes the fake server, SDK, executor and ReadGoal
// polling. It is intentionally not the former isolated callback/storage probe.
#include "ros2_control_fake_controller.hpp"
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_roboplan/execution_host_support.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <stdexcept>
#include <thread>

namespace {
std::atomic<bool> counting{false};
std::atomic<std::size_t> allocations{0};
void count_allocation() {
  if (counting.load(std::memory_order_relaxed))
    allocations.fetch_add(1, std::memory_order_relaxed);
}
}
void* operator new(std::size_t size) {
  count_allocation();
  if (auto p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, std::align_val_t alignment) {
  count_allocation();
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<std::size_t>(alignment), size ? size : 1) == 0)
    return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t s, std::align_val_t a) { return ::operator new(s, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {
namespace core = manyforge::core;
namespace ros = manyforge::ros2_control_execution;
namespace runtime = manyforge::roboplan_backend;
namespace fake = ros::fake_controller;
using namespace std::chrono_literals;
using Action = control_msgs::action::FollowJointTrajectory;
using ServerGoal = rclcpp_action::ServerGoalHandle<Action>;

void require(bool condition, const char* reason) {
  if (!condition) throw std::runtime_error(reason);
}
template<class Predicate> void await(Predicate predicate, const char* reason) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!predicate()) {
    require(std::chrono::steady_clock::now() < deadline, reason);
    std::this_thread::sleep_for(1ms);
  }
}
std::size_t positive_env(const char* name, std::size_t fallback) {
  const char* raw = std::getenv(name);
  if (!raw || !*raw) return fallback;
  const std::string text(raw);
  require(text.find_first_not_of("0123456789") == std::string::npos,
          "Measurement dimensions must be positive integers");
  const auto value = std::stoull(text);
  require(value > 0 && value <= 1'000'000, "Measurement dimension out of range");
  return value;
}
struct Measurement {
  std::size_t count;
  double elapsed_us;
};
class AllocationWindow {
public:
  AllocationWindow() : start_(std::chrono::steady_clock::now()) {
    allocations.store(0);
    counting.store(true);
  }
  ~AllocationWindow() { counting.store(false); }
  Measurement finish() {
    counting.store(false);
    return {allocations.load(), std::chrono::duration<double, std::micro>(
      std::chrono::steady_clock::now() - start_).count()};
  }
private:
  std::chrono::steady_clock::time_point start_;
};
class NullSink final : public core::IExecutionStateSink {
public:
  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string&, const std::vector<double>&, core::ExecutionStateUpdateMode) override { return {}; }
  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string&, const std::string&, double, core::ExecutionStateUpdateMode) override { return {}; }
  std::optional<double> group_joint_position(const std::string&, const std::string&) const override { return 0.0; }
};

class FeedbackRig {
public:
  runtime::WallClock clock;
  ros::Ros2TransportResources resources;
  std::unique_ptr<ros::Ros2ControlExecutionBackend> adapter;
  core::ExecutionGoalKey key{core::ControllerGoalId("perf.feedback"), core::ControllerGoalGeneration(1)};
  FeedbackRig() : resources("manyforge_perf_feedback_" + fake::unique_suffix(), "", 2,
                            std::make_shared<NullSink>(), "arm", clock) {
    const auto route = "/manyforge_perf_" + fake::unique_suffix();
    server_ = rclcpp_action::create_server<Action>(resources.node(), route + "/fjt",
      [](const auto&, auto) { return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE; },
      [](auto) { return rclcpp_action::CancelResponse::ACCEPT; },
      [this](std::shared_ptr<ServerGoal> value) { std::lock_guard lock(mutex_); goal_ = std::move(value); });
    ros::JtcExecutionAdapterConfig config;
    config.action_name = route + "/fjt";
    config.state_topic = route + "/state";
    config.scale_topic = route + "/scale";
    config.binding_epoch = core::ExecutionBindingEpoch(1);
    config.lane_facts.joint_names = {"joint_1"};
    config.lane_facts.controller_update_rate_hz = 100.0;
    adapter = std::make_unique<ros::Ros2ControlExecutionBackend>(resources, clock, config);
    adapter->ObserveReadiness({true, true, true, core::ExecutionBindingEpoch(1)});
    resources.start();
    await([&] { return adapter->ActionServerIsReady(); }, "Fake FJT discovery timed out");
    core::TrajectoryDTO trajectory;
    trajectory.joint_names = {"joint_1"};
    trajectory.waypoints = {{0.0, {0.0}, {0.0}, {0.0}}, {1.0, {0.1}, {0.0}, {0.0}}};
    require(adapter->Submit(key, trajectory).dispatch == core::ExecutionPortDispatch::kDispatched,
            "Fake FJT submission refused");
    await([&] {
      const auto observation = adapter->ReadGoal(key);
      std::lock_guard lock(mutex_);
      return goal_ && observation && observation->accepted_stamp_ns.has_value();
    }, "Fake FJT acceptance timed out");
  }
  ~FeedbackRig() { adapter->shutdown(); resources.quiesce(); }
  void deliver(const std::shared_ptr<Action::Feedback>& feedback, std::uint32_t sequence) {
    feedback->header.stamp.sec = 1;
    feedback->header.stamp.nanosec = sequence;
    std::shared_ptr<ServerGoal> goal;
    { std::lock_guard lock(mutex_); goal = goal_; }
    goal->publish_feedback(feedback);
    await([&] {
      const auto observation = adapter->ReadGoal(key);
      return observation && observation->latest_feedback &&
             observation->latest_feedback->source_stamp_ns == 1'000'000'000 + sequence;
    }, "Fake FJT feedback delivery timed out");
  }
private:
  std::mutex mutex_;
  std::shared_ptr<ServerGoal> goal_;
  rclcpp_action::Server<Action>::SharedPtr server_;
};

core::RuntimeParticipantBinding ArmBinding() {
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.physical"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding"),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      core::ControllableComponentId("robot_1"),
      core::ComponentTypeId("robot.type"),
      core::ComponentTypeRevision("robot-type-revision"),
      core::CommandTargetId("arm.target"),
      core::CapabilityId("execution.trajectory.execute"),
      core::CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {core::ProgramModelRoleBindingDTO{core::ModelRoleId("body"),
                                        core::ModelInstanceId("robot.model"),
                                        core::ModelRevision("model-revision")}},
      {core::RuntimeModelGroupBindingDTO{core::ModelGroupRoleId("arm_group"),
                                         core::ModelInstanceId("robot.model"), "arm",
                                         {"joint_1"}}},
      {core::ProgramFrameRoleBindingDTO{core::FrameRoleId("tcp"),
                                        core::ModelInstanceId("robot.model"),
                                        core::ModelRevision("model-revision"), "tcp"}},
      {core::RuntimeResourceBindingDTO{core::ResourceRoleId("joint_1"),
                                       core::ResourceId("robot.joint.1"), "source_joint_1",
                                       "joint_1", core::CoordinateKind::kAngular}},
      {},
      core::RuntimeControlPathDTO{
          core::ControlDomainId("domain.arm"), core::ControlDomainRevision("domain-revision"),
          core::ControlBindingId("binding.arm"), core::ControlBindingRevision("binding-revision"),
          core::ControlInterfaceContractId("interface.trajectory"),
          core::ControlInterfaceRevision("interface-revision"),
          core::ControllerTopologyId("topology.ros2_control"),
          core::TopologyRevision("topology-revision"), core::ControlRouteId("route.ros2_control"),
          core::ActivationSetId("activation.ros2_control"),
          core::ActiveControlConfigurationId("active.ros2_control"),
          core::ActiveControlConfigurationGeneration(1U),
          core::ProviderInstanceId("provider.ros2_control"),
          core::ProviderManifestId("provider-manifest.ros2_control"),
          core::ProviderManifestRevision("provider-manifest-revision"),
          core::RuntimeUnitId("runtime.ros2_control"), core::RuntimeEpoch("runtime-epoch.1"),
          core::StateAuthorityId("state.commanded"), core::StateAuthorityClass::kCommanded,
          core::StopEvidencePolicyId("stop.ros2_control"),
          core::StopEvidencePolicyRevision("stop-revision"),
          core::ContainmentGranularity::kResource, std::nullopt},
      std::nullopt,
      {},
      {core::RuntimeUnitEpochBindingDTO{core::RuntimeUnitId("runtime.ros2_control"),
                                        core::RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) throw std::logic_error(sealed.error().message);
  return std::move(sealed.value());
}

// The normal controller-manager fixture activates the lifecycle facts without
// enabling FakeJtc's periodic publisher. One manually published sample remains
// unchanged while the real public handle's 100 Hz observer timer runs.
struct IdleRig {
  runtime::WallClock clock;
  runtime::SimpleScheduler scheduler;
  core::RuntimeParticipantBinding binding = ArmBinding();
  core::ObservedRobotStatePublication publication{
    {"joint_1"}, {core::ObservedRobotStateSourceConfig{
      core::ObservedRobotStateSourceKind::kCommandedState, "commanded_state",
      core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0}}};
  std::string prefix = "/manyforge_perf_idle_" + fake::unique_suffix();
  fake::FakeJtc jtc{prefix + "/arm_controller", {"source_joint_1"}};
  fake::FakeControllerManager manager{prefix + "/controller_manager", "arm_controller", "TestSystem", nullptr};
  std::atomic<std::size_t> observer_updates{0};
  std::atomic<std::uint64_t> observed_epoch{0}, observed_receipt{0};
  std::shared_ptr<runtime::RetainedRos2ControlRuntimeHandle> handle =
      runtime::RetainedRos2ControlRuntimeHandle::Create();

  IdleRig() {
    runtime::RetainedRos2ControlArmTransport arm;
    arm.adapter.action_name = prefix + "/arm_controller/follow_joint_trajectory";
    arm.adapter.state_topic = prefix + "/arm_controller/controller_state";
    arm.adapter.scale_topic = prefix + "/arm_controller/speed_scaling_input";
    arm.adapter.lane_facts.joint_names = {"source_joint_1"};
    arm.adapter.lane_facts.supports_scale = true;
    arm.adapter.lane_facts.state_rate_hz = 100.0;
    arm.adapter.lane_facts.feedback_rate_hz = 100.0;
    arm.adapter.lane_facts.controller_update_rate_hz = 100.0;
    arm.controller = {prefix + "/controller_manager", "arm_controller", "TestSystem"};
    arm.compiled_joint_names = {"joint_1"};
    const auto bound = handle->Bind(
      std::make_unique<ros::Ros2TransportResources>(
        "manyforge_perf_handle_" + fake::unique_suffix(), "/", 3,
        std::make_shared<NullSink>(), "arm", clock), clock, std::move(arm),
      [this](std::shared_ptr<core::IExecutionPort> port) {
        // Existing runtime-handle test fixture values; no motion is submitted.
        core::ExecutionPortParticipantConfig config;
        config.stop.trip_authority = core::StateAuthorityClass::kCommanded;
        config.stop.stop_traj_delta_s = 0.1;
        config.stop.sample_period_s = 0.01;
        config.stop.replacement_start_allowance_s = 0.02;
        config.stop.hard_stop_completion_timeout_ns = 1'000'000'000;
        config.late_cycle_margin_s = 0.5;
        return std::make_shared<core::ExecutionPortParticipant>(
          binding, std::move(port), clock, scheduler,
          [](core::TimePoint, const std::stop_token&) { std::this_thread::sleep_for(1ms); },
          core::ExecutionPortParticipantStateAuthority{
            &publication, [] { return core::Result<void>::Ok(); }}, std::move(config));
      }, nullptr, binding, std::nullopt);
    require(bound.ok(), "Idle runtime handle binding failed");
    handle->InstallSampleObserver([this](const core::ExecutionControllerSampleDTO& sample) {
      observed_epoch.store(sample.binding_epoch.value());
      observed_receipt.store(sample.local_receipt_count.value());
      observer_updates.fetch_add(1);
    });
    handle->StartBackend();
    await([&] {
      const auto activity = handle->arm_activity();
      return activity.observed && activity.controller_loaded && activity.hardware_active;
    }, "Idle controller manager discovery timed out");
    require(handle->PrepareArmController({}, std::chrono::steady_clock::now() + std::chrono::seconds(30)).outcome == runtime::RetainedRos2ControlSwitchOutcome::kSucceeded,
            "Idle controller preparation failed");
    require(handle->ActivateArmController({}, std::chrono::steady_clock::now() + std::chrono::seconds(30)).outcome == runtime::RetainedRos2ControlSwitchOutcome::kSucceeded,
            "Idle controller activation failed");
    auto publisher = jtc.node()->create_publisher<control_msgs::msg::JointTrajectoryControllerState>(
      prefix + "/arm_controller/controller_state", rclcpp::QoS(10).reliable());
    await([&] { return publisher->get_subscription_count() > 0; }, "Idle state discovery timed out");
    control_msgs::msg::JointTrajectoryControllerState sample;
    sample.header.stamp.sec = 1;
    sample.joint_names = {"source_joint_1"};
    sample.reference.positions = {0.1};
    sample.reference.velocities = {0.0};
    sample.reference.accelerations = {0.0};
    sample.feedback.positions = {0.1};
    sample.feedback.velocities = {0.0};
    sample.speed_scaling_factor = 1.0;
    publisher->publish(sample);
    await([&] { return observer_updates.load() > 0; }, "Idle sample observer timed out");
  }
};
} // namespace

int main() {
  try {
    const auto iterations = positive_env("MANYFORGE_ROS2_CONTROL_HOT_PATH_ITERATIONS", 10000);
    const auto idle_window_ms = positive_env("MANYFORGE_ROS2_CONTROL_IDLE_WINDOW_MS", 1000);
    Measurement feedback_measurement{};
    {
      FeedbackRig rig;
      auto feedback = std::make_shared<Action::Feedback>();
      feedback->joint_names = {"joint_1"};
      feedback->desired.positions = {0.1};
      feedback->desired.velocities = {0.0};
      feedback->desired.accelerations = {0.0};
      feedback->actual.positions = {0.1};
      // Warm up the actual goal's feedback path before counting allocations.
      rig.deliver(feedback, 1);
      AllocationWindow window;
      for (std::size_t index = 0; index < iterations; ++index)
        rig.deliver(feedback, static_cast<std::uint32_t>(index + 2));
      feedback_measurement = window.finish();
    }
    Measurement idle_measurement{};
    std::size_t before{}, after{};
    std::uint64_t epoch{}, receipt{};
    bool identity_unchanged{};
    {
      IdleRig rig;
      before = rig.observer_updates.load();
      epoch = rig.observed_epoch.load();
      receipt = rig.observed_receipt.load();
      AllocationWindow window;
      std::this_thread::sleep_for(std::chrono::milliseconds(idle_window_ms));
      idle_measurement = window.finish();
      after = rig.observer_updates.load();
      const auto sample = rig.handle->current_port()->ReadSample();
      identity_unchanged = sample && sample->binding_epoch.value() == epoch &&
        sample->local_receipt_count.value() == receipt;
    }
    std::ostringstream json;
    json << std::setprecision(12)
      << "{\"qualification\":\"ros2_control_hot_paths\",\"mode\":\"report_only\","
      << "\"allocation_scope\":\"process_wide_cpp_new_including_aligned; C_and_RMW_malloc_unmeasured\","
      << "\"feedback_delivery\":{\"scope\":\"normal_fake_FJT_server_SDK_executor_and_ReadGoal_polling\","
      << "\"messages_delivered\":" << iterations
      << ",\"allocations\":" << feedback_measurement.count
      << ",\"allocations_per_message\":" << static_cast<double>(feedback_measurement.count) / iterations
      << ",\"elapsed_us\":" << feedback_measurement.elapsed_us
      << ",\"mean_delivery_us\":" << feedback_measurement.elapsed_us / iterations << "},"
      << "\"idle_observer\":{\"scope\":\"public_runtime_handle_timer_unchanged_sample_window\","
      << "\"configured_timer_hz\":100,\"requested_window_ms\":" << idle_window_ms
      << ",\"elapsed_us\":" << idle_measurement.elapsed_us
      << ",\"allocations\":" << idle_measurement.count
      << ",\"observer_updates_before\":" << before
      << ",\"observer_updates_after\":" << after
      << ",\"observer_updates_during_window\":" << after - before
      << ",\"binding_epoch\":" << epoch << ",\"local_receipt_count\":" << receipt
      << ",\"sample_identity_unchanged\":" << (identity_unchanged ? "true" : "false")
      << "}}";
    if (const char* output = std::getenv("MANYFORGE_ROS2_CONTROL_HOT_PATH_JSON_OUT"); output && *output) {
      std::ofstream file(output);
      require(static_cast<bool>(file << json.str() << '\n'), "Cannot write qualification JSON");
    }
    std::cout << "MANYFORGE_QUALIFICATION_EVIDENCE " << json.str() << '\n';
    return 0; // Measurements carry no pass/fail budget; setup/delivery errors do.
  } catch (const std::exception& error) {
    counting.store(false);
    std::cerr << "ROS hot-path measurement failed: " << error.what() << '\n';
    return 1;
  }
}
