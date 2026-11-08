#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_component_manager/execution_host_exit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <future>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <controller_manager_msgs/srv/set_hardware_component_state.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <manyforge_roboplan/ros2_control_component_state.hpp>
#include <manyforge_core/time/received_sample_age.hpp>
#include <manyforge_core/types/error_catalog.generated.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_ros2_control_execution/selected_ros_endpoint_facts.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/float64_multi_array.hpp>

namespace manyforge::roboplan_backend {
namespace {

std::uint64_t next_active_tcp_registration() {
  static std::atomic<std::uint64_t> registrations{0U};
  return ++registrations;
}

// Apply may prepare a successor while its predecessor is still retained.
// A process-wide generation keeps both epoch node names distinct.
std::shared_ptr<ros2_control_execution::Ros2TransportResources>
make_jtc_epoch_resources(const ros2_control_execution::Ros2TransportResources &machine,
                         std::string_view component_id) {
  static std::atomic<std::uint64_t> generations{0U};
  constexpr std::string_view prefix = "manyforge_ros2_control_jtc_epoch_";
  const std::string suffix = "_" + std::to_string(++generations);
  // Managed controller component IDs are ROS tokens. Direct Bind callers can
  // supply other IDs, so make only the graph label safe without refusing them.
  std::string label;
  label.reserve(std::min(component_id.size(), 255U - prefix.size() - suffix.size()));
  for (const auto ch : component_id) {
    if (label.size() == 255U - prefix.size() - suffix.size()) break;
    const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                       (ch >= '0' && ch <= '9') || ch == '_';
    label.push_back(valid ? ch : '_');
  }
  if (label.empty()) label = "_";
  const auto node = machine.node();
  const auto context = node->get_node_base_interface()->get_context();
  return ros2_control_execution::Ros2TransportResources::create(
      std::string(prefix) + label + suffix,
      node->get_namespace(), 1U, [context] { return context; });
}

using ActivityMessage = controller_manager_msgs::msg::ControllerManagerActivity;
using SwitchService = controller_manager_msgs::srv::SwitchController;
using HardwareStateService =
    controller_manager_msgs::srv::SetHardwareComponentState;

// Every call a Real arm sends to its controller manager (a
// switch, a hardware-state request, a status query) is allowed
// min(this shared cap, the step's remaining time). No answer within it is
// the connection lost (`controller_manager_unresponsive` and the fault). 10 s
// until measured live.
constexpr std::chrono::nanoseconds kComponentCallResponseCap{
    core::kComponentCallResponseCapNs};

// The time one call is allowed: min(the cap, the remaining time).
[[nodiscard]] std::chrono::steady_clock::time_point
allowed_until(const std::chrono::steady_clock::time_point now,
              const std::optional<std::chrono::steady_clock::time_point> step_deadline) {
  return std::min(now + kComponentCallResponseCap,
                  step_deadline.value_or(std::chrono::steady_clock::time_point::max()));
}

// One INFO line per sent controller-manager call with its
// measured duration, so that per-call caps can be set from live data.
void log_call(const rclcpp::Logger &logger, const char *call, const char *outcome,
              const std::chrono::steady_clock::time_point started) {
  RCLCPP_INFO(logger, "manyforge_component_call call=%s outcome=%s duration_ms=%.1f", call,
              outcome,
              std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                        started)
                  .count());
}

// A prepare waits for its own endpoints (the controller manager reporting
// its controller) until the component's lifecycle command time ends: `check`
// returns the refusal while they aren't there yet. Its last refusal is the
// outcome when the time ends or a Stop interrupts the wait.
template <typename Check>
[[nodiscard]] std::optional<RetainedRos2ControlSwitchResult>
wait_for_endpoints(const std::stop_token &cancellation,
                   const std::chrono::steady_clock::time_point deadline, Check check) {
  while (true) {
    auto refused = check();
    if (!refused || cancellation.stop_requested() ||
        std::chrono::steady_clock::now() >= deadline)
      return refused;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

template <typename Value>
[[nodiscard]] core::Result<Value> reject(std::string reason,
                                         std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

// Ordinary execution-host route publication changes the route generation of
// the same controller occurrence. Product resealing stays strict. Compare
// every physical fact with that one publication field held at its predecessor value;
// callers and the participant retain the actual candidate generation below.
[[nodiscard]] std::string_view route_physical_owner_difference(
    const core::RuntimeParticipantBinding &predecessor,
    const core::RuntimeParticipantBinding &candidate) {
  auto owner = candidate.value();
  if (owner.control_path && predecessor.value().control_path) {
    owner.control_path->active_control_configuration_generation =
        predecessor.value().control_path->active_control_configuration_generation;
  }
  auto verified = core::verify_runtime_participant_binding(std::move(owner));
  if (!verified.ok()) return "candidate_binding";
  return retained_product_reseal_physical_owner_difference(predecessor,
                                                          verified.value());
}

// Stage reuses a physical transport beside the route owner's qualified
// prospective bindings. Normalize only this comparison copy; the descriptor
// and every observation retain the complete unmodified candidate tuple.
// This is deliberately separate from the unchanged effect-side Rebind guard.
[[nodiscard]] std::string_view measured_candidate_physical_owner_difference(
    const core::RuntimeParticipantBinding &predecessor,
    const core::RuntimeParticipantBinding &candidate) {
  auto owner = candidate.value();
  if (!owner.control_path || !predecessor.value().control_path)
    return "control_path";
  auto &path = *owner.control_path;
  const auto &before = *predecessor.value().control_path;
  if (path.active_control_configuration_generation.value() <
      before.active_control_configuration_generation.value())
    return "control_path.active_control_configuration_generation";
  path.control_binding_revision = before.control_binding_revision;
  path.control_route_id = before.control_route_id;
  path.activation_set_id = before.activation_set_id;
  path.active_control_configuration_id = before.active_control_configuration_id;
  path.active_control_configuration_generation =
      before.active_control_configuration_generation;
  path.runtime_scale_qualification = before.runtime_scale_qualification;
  auto verified = core::verify_runtime_participant_binding(std::move(owner));
  if (!verified.ok())
    return "candidate_binding";
  return retained_product_reseal_physical_owner_difference(predecessor,
                                                          verified.value());
}

[[nodiscard]] core::ErrorDTO switch_error(std::string reason,
                                          std::string message) {
  return core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
      std::move(message),
      {{"reason", std::move(reason)}}};
}

[[nodiscard]] std::string bounded_switch_detail(std::string text) {
  constexpr std::size_t limit = 256U;
  constexpr std::string_view marker = "[truncated]";
  if (text.size() <= limit)
    return text;
  auto end = limit - marker.size();
  // Reuse the controller-launch diagnostic's complete UTF-8 prefix rule.
  while (end > 0U && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
    --end;
  text.resize(end);
  text += marker;
  return text;
}

[[nodiscard]] RetainedRos2ControlSwitchResult not_sent(std::string reason,
                                                       std::string message) {
  return {RetainedRos2ControlSwitchOutcome::kNotSent,
          switch_error(std::move(reason), std::move(message))};
}

/// Prepare on a component this host enabled is refused, nothing sent: its
/// Disable takes it back to prepared.
[[nodiscard]] RetainedRos2ControlSwitchResult component_enabled() {
  return {RetainedRos2ControlSwitchOutcome::kNotSent,
          core::ErrorDTO{std::string(core::error_codes::kComponentEnabled),
                         "The component is enabled; Disable it first", {}}};
}

// No time left at the send refuses a request before dispatch.
[[nodiscard]] std::optional<RetainedRos2ControlSwitchResult>
refuse_if_expired_at_send(const std::optional<std::chrono::steady_clock::time_point> &deadline) {
  if (!deadline || std::chrono::steady_clock::now() < *deadline) return std::nullopt;
  return RetainedRos2ControlSwitchResult{
      RetainedRos2ControlSwitchOutcome::kNotSent,
      core::ErrorDTO{"component_lifecycle_deadline_expired",
                     "the lifecycle deadline expired before dispatch",
                     {{"reason", "deadline_exhausted"}}}};
}

/// A display joint sample in the arm binding's compiled names: one position
/// per name, every bound joint named exactly once (by its source name) with a
/// finite value; otherwise none. Whether the values are a pose is the
/// family's check (`DisplayJointSource::use`).
struct DisplayJointSample final {
  std::vector<std::string> compiled_names;
  std::vector<double> positions;
};

[[nodiscard]] std::optional<DisplayJointSample>
display_joint_sample(const core::RuntimeParticipantBinding &binding,
                     const sensor_msgs::msg::JointState &message) {
  const auto &resources = binding.value().resources;
  if (resources.empty() || message.name.size() != message.position.size())
    return std::nullopt;
  DisplayJointSample sample;
  sample.compiled_names.reserve(resources.size());
  sample.positions.reserve(resources.size());
  for (const auto &resource : resources) {
    if (!resource.source_joint_name) return std::nullopt;
    const auto &name = *resource.source_joint_name;
    const auto found = std::find(message.name.begin(), message.name.end(), name);
    if (found == message.name.end() ||
        std::find(std::next(found), message.name.end(), name) != message.name.end())
      return std::nullopt;
    const double value =
        message.position[static_cast<std::size_t>(found - message.name.begin())];
    if (!std::isfinite(value)) return std::nullopt;
    sample.compiled_names.push_back(resource.compiled_resource_name);
    sample.positions.push_back(value);
  }
  return sample;
}

/// The streams one bound component instance owns on its measured lane,
/// declared at bind so a replacement instance of the same component id starts
/// from its own fresh slot (never its predecessor's stale one).
struct BoundComponentStreams final {
  std::shared_ptr<core::StreamRecorder> measured;
  std::shared_ptr<core::StreamRecorder> gripper_feedback;
};

[[nodiscard]] core::Result<BoundComponentStreams> declare_bound_component_streams(
    const std::optional<RetainedMeasuredJointStateLane> &lane,
    const core::RuntimeParticipantBinding &owner,
    const std::optional<core::RuntimeParticipantBinding> &gripper) {
  using Out = core::Result<BoundComponentStreams>;
  BoundComponentStreams streams;
  // A lane without a qualified budget is refused by measured staging.
  if (!lane || lane->freshness_budget_ns <= 0) return Out::Ok(std::move(streams));
  auto measured = host_component_stream_health()->Declare(
      owner.value().controllable_component_id,
      core::StreamDeclarationDTO{"measured_joints", lane->freshness_budget_ns,
                                 lane->stream_severity, lane->stream_recovery,
                                 core::StreamTimestampSourceDTO::kMessageStamp});
  if (!measured.ok()) return Out::Err(measured.error());
  streams.measured = std::move(measured.value());
  if (lane->gripper_feedback_stream && gripper) {
    auto feedback = host_component_stream_health()->Declare(
        gripper->value().controllable_component_id, *lane->gripper_feedback_stream);
    if (!feedback.ok()) return Out::Err(feedback.error());
    streams.gripper_feedback = std::move(feedback.value());
  }
  return Out::Ok(std::move(streams));
}

} // namespace

/// One binding epoch's port as Core sees it: the JTC adapter in the
/// participant's compiled joint names, with the lane guard's motion gate
/// on every motion command. Readings and Cancel/SetScale pass through.
class RetainedRos2ControlRuntimeHandle::ArmPort final
    : public core::IExecutionPort {
public:
  using Gate = std::function<bool()>;

  ArmPort(std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend>
              adapter,
          std::vector<std::string> compiled,
          std::vector<std::string> controller, std::shared_ptr<const Gate> gate,
          std::shared_ptr<const std::atomic<bool>> reset_motion_admitted)
      : adapter_(std::move(adapter)), compiled_(std::move(compiled)),
        controller_(std::move(controller)), gate_(std::move(gate)),
        reset_motion_admitted_(std::move(reset_motion_admitted)) {}

  [[nodiscard]] ros2_control_execution::Ros2ControlExecutionBackend &
  adapter() const noexcept {
    return *adapter_;
  }

  core::ExecutionPortCommandResultDTO
  Submit(const core::ExecutionGoalKey &key,
         const core::TrajectoryDTO &trajectory,
         std::shared_ptr<core::ExecutionDispatchToken> token = {}) override {
    if (const auto refused = Refusal()) return *refused;
    auto renamed = Rename(trajectory);
    if (!renamed) return InvalidJoints();
    return adapter_->Submit(key, *renamed, std::move(token));
  }

  core::ExecutionPortCommandResultDTO
  Replace(const core::ExecutionGoalKey &active_key,
          const core::ExecutionGoalKey &next_key,
          const core::TrajectoryDTO &trajectory,
          std::shared_ptr<core::ExecutionDispatchToken> token = {}) override {
    if (const auto refused = Refusal()) return *refused;
    auto renamed = Rename(trajectory);
    if (!renamed) return InvalidJoints();
    return adapter_->Replace(active_key, next_key, *renamed, std::move(token));
  }

  core::ExecutionPortCommandResultDTO
  ReplaceScheduled(const core::ExecutionGoalKey &active_key,
                   const core::ExecutionGoalKey &next_key,
                   const core::TrajectoryDTO &trajectory,
                   const core::ExecutionReplacementStartDTO &start,
                   std::shared_ptr<core::ExecutionDispatchToken> token = {}) override {
    if (const auto refused = Refusal())
      return *refused;
    auto renamed = Rename(trajectory);
    if (!renamed)
      return InvalidJoints();
    return adapter_->ReplaceScheduled(active_key, next_key, *renamed, start, std::move(token));
  }

  core::ExecutionPortCommandResultDTO
  Cancel(const core::ExecutionGoalKey &key, std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) override {
    return adapter_->Cancel(key, deadline);
  }

  std::uint64_t ReserveScaleSequence() noexcept override { return adapter_->ReserveScaleSequence(); }
  bool IsLatestScaleSequence(std::uint64_t sequence) const noexcept override { return adapter_->IsLatestScaleSequence(sequence); }
  core::ScaleSnapshot SnapshotScaleAfterFence(std::chrono::steady_clock::time_point deadline) override { return adapter_->SnapshotScaleAfterFence(deadline); }
  std::optional<bool> ScaleInputListening() const noexcept override { return adapter_->ScaleInputListening(); }
  core::ExecutionPortCommandResultDTO SetScale(double value, std::uint64_t sequence) override {
    return adapter_->SetScale(value, sequence);
  }

  std::optional<core::ExecutionGoalObservationDTO>
  ReadGoal(const core::ExecutionGoalKey &key) const override {
    return adapter_->ReadGoal(key);
  }

  std::optional<core::ExecutionControllerSampleDTO> ReadSample() const override {
    return adapter_->ReadSample();
  }

  core::ExecutionReadinessDTO ReadReadiness() const override {
    return adapter_->ReadReadiness();
  }

  core::ExecutionTimingDTO ReadTiming() override {
    return adapter_->ReadTiming();
  }

  core::ExecutionLaneFactsDTO ReadLaneFacts() const override {
    auto facts = adapter_->ReadLaneFacts();
    // Same coordinate order; Core addresses the binding's compiled names.
    facts.joint_names = compiled_;
    return facts;
  }

private:
  [[nodiscard]] std::optional<core::ExecutionPortCommandResultDTO>
  Refusal() const {
    if (!reset_motion_admitted_->load())
      return core::ExecutionPortCommandResultDTO{
          core::ExecutionPortDispatch::kNotDispatched,
          core::ExecutionPortRefusal::kNotReady,
          core::ErrorDTO{"execution_rejected",
                         "The accepted position Reset has not completed",
                         {{"reason", "mock_reset_position_pending"}}}};
    if (!*gate_ || (*gate_)()) return std::nullopt;
    return core::ExecutionPortCommandResultDTO{
        core::ExecutionPortDispatch::kNotDispatched,
        core::ExecutionPortRefusal::kNotReady,
        core::ErrorDTO{"execution_rejected",
                       "The lane guard does not admit motion on this arm",
                       {{"reason", "lane_guard_motion_not_admitted"}}}};
  }

  [[nodiscard]] static core::ExecutionPortCommandResultDTO InvalidJoints() {
    return {core::ExecutionPortDispatch::kNotDispatched,
            core::ExecutionPortRefusal::kInvalidInput,
            core::ErrorDTO{"execution_rejected",
                           "The trajectory names a joint outside the binding",
                           {{"reason", "trajectory_joint_outside_binding"}}}};
  }

  [[nodiscard]] std::optional<core::TrajectoryDTO>
  Rename(const core::TrajectoryDTO &trajectory) const {
    auto renamed = trajectory;
    for (auto &name : renamed.joint_names) {
      const auto found = std::find(compiled_.begin(), compiled_.end(), name);
      if (found == compiled_.end()) return std::nullopt;
      name = controller_[static_cast<std::size_t>(found - compiled_.begin())];
    }
    return renamed;
  }

  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter_;
  std::vector<std::string> compiled_;
  std::vector<std::string> controller_;
  std::shared_ptr<const Gate> gate_;
  std::shared_ptr<const std::atomic<bool>> reset_motion_admitted_;
};

struct RetainedRos2ControlRuntimeHandle::ArmState final {
  RetainedRos2ControlArmTransport transport;
  std::string component_id;
  /// The current binding epoch and its adapter instance.
  std::uint64_t epoch{1U};
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> epoch_resources;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  std::shared_ptr<ArmPort> port;
  /// An activation succeeded (or may have) on the current epoch: the next
  /// activation is a reactivation and needs the next epoch.
  bool epoch_used{false};
  /// A new epoch's port the participant has not accepted yet.
  bool rebind_pending{false};
  /// This execution host's component coordinator activated the controller on
  /// the current epoch and the controller manager has not reported the
  /// controller inactive since it reported it active.
  bool bound{false};
  bool active_seen_while_bound{false};
  /// Enable's own-client discovery postcondition is established once per
  /// adapter epoch. Later graph loss does not change binding/readiness facts.
  bool activation_client_discovered{false};
  /// The lifecycle state this host's component adapter established.
  core::ComponentLifecycleStateDTO requested{
      core::ComponentLifecycleStateDTO::kDeclared};
  RetainedRos2ControlArmActivity activity;
  std::shared_ptr<ArmPort::Gate> gate{std::make_shared<ArmPort::Gate>()};
  rclcpp::Subscription<ActivityMessage>::SharedPtr activity_subscription;
  std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts>
      activity_endpoints;
  rclcpp::Client<SwitchService>::SharedPtr switch_client;
  /// The stock `set_hardware_component_state` client (the re-arm).
  rclcpp::Client<HardwareStateService>::SharedPtr hardware_state_client;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr
      reset_publisher;
  /// The hardware's command output (the target's `command_echo_topic`).
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
      reset_echo_subscription;
  std::optional<ResetCommandEcho> reset_echo;
  bool reset_writer_loaded{false};
  bool reset_writer_active{false};
  bool reset_writer_possibly_active{false};
  bool reset_position_pending{false};
  std::shared_ptr<std::atomic<bool>> reset_motion_admitted{
      std::make_shared<std::atomic<bool>>(true)};
  /// The controller manager's statistics for this controller.
  std::shared_ptr<RetainedControllerStatisticsMonitor> statistics_monitor;
  rclcpp::Subscription<pal_statistics_msgs::msg::Statistics>::SharedPtr
      statistics_subscription;
};

// Aliasing reader leases own the channel, including its real receiver clock.
// The callback retains no handle or executor whose last destruction could drain
// the callback's own executor thread.
struct RetainedRos2ControlRuntimeHandle::GripperOnlyState final {
  RetainedRos2ControlArmController controller;
  bool physical_controller_required{true};
  std::optional<RetainedMeasuredJointStateLane> measured_lane;
  std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity_endpoints;
  rclcpp::Subscription<ActivityMessage>::SharedPtr activity_subscription;
  rclcpp::Client<SwitchService>::SharedPtr switch_client;
};

// Unpublished replacement resources. The stage pins both epochs and all
// entity handles until the enclosing authority finishes its commit.
struct RetainedRos2ControlRuntimeHandle::StagedMachineOwner final {
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> previous;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> candidate;
  std::shared_ptr<ArmState> previous_arm;
  std::shared_ptr<GripperOnlyState> previous_gripper;
  std::optional<RetainedRos2ControlArmTransport> arm_transport;
  std::string component_id;
  std::uint64_t previous_epoch{};
  std::shared_ptr<const ArmPort::Gate> motion_gate;
  std::shared_ptr<const std::atomic<bool>> reset_motion_admitted;
  RetainedRos2ControlArmController controller;
  bool has_controller{};
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> epoch;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> previous_epoch_owner;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> previous_adapter;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  std::shared_ptr<ArmPort> port;
  std::shared_ptr<core::ExecutionPortParticipant> participant;
  std::shared_ptr<core::ExecutionPortParticipant::OwnerRebindStage> participant_stage;
  std::shared_ptr<ros2_control_execution::Ros2GripperCommandBackend::StagedRosRebind> gripper;
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources> installed_sources;
  std::shared_ptr<StagedMeasuredOwnerRebind> measured;
  std::function<bool(const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &,
                     void (*)(void *) noexcept, void *)> source_commit;
  std::function<void()> notify;
  std::function<void()> fault_wake;
  std::function<bool(std::uint64_t, std::uint64_t)> qualified;
  std::shared_ptr<StagedMachineOwner> previous_stage;
  std::shared_ptr<const TransportOwners> published_owners;
  std::shared_ptr<const TransportOwners> previous_published_owners;
  std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity_endpoints;
  rclcpp::Subscription<ActivityMessage>::SharedPtr activity_subscription;
  rclcpp::Client<SwitchService>::SharedPtr switch_client;
  rclcpp::Client<HardwareStateService>::SharedPtr hardware_state_client;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr reset_publisher;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr reset_echo_subscription;
  std::shared_ptr<RetainedControllerStatisticsMonitor> statistics_monitor;
  rclcpp::Subscription<pal_statistics_msgs::msg::Statistics>::SharedPtr statistics_subscription;
  rclcpp::SubscriptionBase::SharedPtr measured_subscription;
  rclcpp::SubscriptionBase::SharedPtr display_subscription;
  rclcpp::TimerBase::SharedPtr sample_timer;
  std::function<void(const sensor_msgs::msg::JointState &)> measured_observer;
  std::function<void(const core::ExecutionControllerSampleDTO &)> sample_observer;
  std::shared_ptr<const DisplayJointSource> display_source;
  std::shared_ptr<RetainedObservedStateChannel> display_channel;
  std::shared_ptr<std::atomic<bool>> callbacks_enabled{std::make_shared<std::atomic<bool>>(false)};
  std::mutex activity_mutex;
  ActivityMessage::ConstSharedPtr activity_sample;
  std::optional<core::TimePoint> activity_received_at;
  std::optional<std::size_t> activity_matches;
  std::optional<core::TimePoint> matched_received_at;
  std::uint64_t activity_event_sequence{};
  std::uint64_t sample_sequence{};
  std::uint64_t matched_sequence{};
};

std::shared_ptr<RetainedRos2ControlRuntimeHandle::StagedMachineOwner>
RetainedRos2ControlRuntimeHandle::StageMachineOwner() {
  auto stage = std::make_shared<StagedMachineOwner>();
  std::function<std::shared_ptr<ros2_control_execution::Ros2TransportResources>()> factory;
  std::function<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>()> source_snapshot;
  ros2_control_execution::Ros2GripperCommandBackend *gripper = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (closing_) throw std::logic_error("ROS runtime handle is closing");
    if (!transport_factory_ || !resources_ || (!arm_ && !gripper_only_))
      throw std::logic_error("machine rebind has no retained transport factory or owner");
    factory = transport_factory_;
    source_snapshot = measured_source_snapshot_;
    stage->source_commit = measured_source_commit_;
    stage->notify = owner_recovery_notify_;
    stage->fault_wake = owner_fault_wake_;
    stage->qualified = owner_recovery_qualified_;
    stage->previous_stage = live_machine_stage_;
    stage->previous_published_owners = published_transport_owners_.load(std::memory_order_acquire);
    stage->previous = resources_;
    stage->previous_arm = arm_;
    stage->previous_gripper = gripper_only_;
    stage->measured_observer = measured_state_observer_;
    stage->sample_observer = sample_observer_;
    stage->display_source = display_joint_source_;
    stage->display_channel = display_joint_channel_.lock();
    gripper = gripper_.get();
    if (arm_ && arm_->epoch == std::numeric_limits<std::uint64_t>::max())
      throw std::overflow_error("ROS controller binding epoch is exhausted");
    if (arm_) {
      stage->arm_transport = arm_->transport;
      stage->component_id = arm_->component_id;
      stage->previous_epoch_owner = arm_->epoch_resources;
      stage->previous_adapter = arm_->adapter;
      stage->participant = participant_;
      stage->previous_epoch = arm_->epoch;
      stage->motion_gate = arm_->gate;
      stage->reset_motion_admitted = arm_->reset_motion_admitted;
      stage->controller = arm_->transport.controller;
      stage->has_controller = true;
    } else {
      stage->controller = gripper_only_->controller;
      stage->has_controller = gripper_only_->physical_controller_required;
    }
  }
  stage->candidate = factory();
  if (!stage->candidate || stage->candidate == stage->previous)
    throw std::logic_error("machine rebind factory did not create a fresh owner");
  // OD-66: measured state and gripper action callbacks run on the machine
  // owner's time-critical thread.
  if (!stage->candidate->started()) stage->candidate->enable_time_critical_callbacks();
  const auto node = stage->candidate->node();
  if (stage->arm_transport) {
    const auto &arm = *stage->arm_transport;
    auto adapter_config = arm.adapter;
    adapter_config.binding_epoch = core::ExecutionBindingEpoch(stage->previous_epoch + 1U);
    stage->epoch = make_jtc_epoch_resources(*stage->candidate, stage->component_id);
    stage->adapter = std::make_shared<ros2_control_execution::Ros2ControlExecutionBackend>(
        stage->epoch, clock_owner_, *scheduler_, std::move(adapter_config), stage->callbacks_enabled);
    stage->port = std::make_shared<ArmPort>(
        stage->adapter, arm.compiled_joint_names,
        arm.adapter.lane_facts.joint_names, stage->motion_gate, stage->reset_motion_admitted);
    if (stage->participant) {
      auto staged_port = stage->participant->StageOwnerRebind(stage->port);
      if (!staged_port.ok()) throw std::runtime_error(staged_port.error().message);
      stage->participant_stage = std::move(staged_port.value());
    }
  }
  if (gripper)
    stage->gripper = gripper->stage_ros_rebind(stage->candidate);

  if (stage->has_controller) {
    const auto controller = stage->controller;
    stage->activity_endpoints =
        std::make_shared<ros2_control_execution::SelectedRosEndpointFacts>(
            node, controller.controller_manager + "/activity");
    const std::weak_ptr<StagedMachineOwner> weak_stage = stage;
    const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak_handle = weak_from_this();
    const auto candidate_endpoints = stage->activity_endpoints;
    rclcpp::SubscriptionOptions options;
    options.event_callbacks.matched_callback = [weak_stage, weak_handle, candidate_endpoints](rclcpp::MatchedInfo &info) {
      const auto current = weak_stage.lock();
      const auto handle = weak_handle.lock();
      if (!current || !handle) return;
      const auto received = handle->clock_->now();
      candidate_endpoints->ObserveMatchedCount(info.current_count);
      candidate_endpoints->Refresh();
      std::lock_guard lock(current->activity_mutex);
      current->activity_matches = info.current_count;
      current->matched_received_at = received;
      current->matched_sequence = ++current->activity_event_sequence;
      if (current->callbacks_enabled->load(std::memory_order_acquire))
        handle->PublishStagedActivityLocked(current);
    };
    stage->activity_subscription = stage->candidate->retain_entity(
        node->create_subscription<ActivityMessage>(
            controller.controller_manager + "/activity",
            rclcpp::QoS(1).reliable().transient_local(),
            [weak_stage, weak_handle](const ActivityMessage::ConstSharedPtr message) {
              const auto current = weak_stage.lock();
              const auto handle = weak_handle.lock();
              if (!current || !handle) return;
              const auto received = handle->clock_->now();
              std::lock_guard lock(current->activity_mutex);
              current->activity_sample = message;
              current->activity_received_at = received;
              current->sample_sequence = ++current->activity_event_sequence;
              if (current->callbacks_enabled->load(std::memory_order_acquire))
                handle->PublishStagedActivityLocked(current);
            }, options));
    stage->switch_client = stage->candidate->retain_entity(
        node->create_client<SwitchService>(controller.controller_manager + "/switch_controller"));
    if (stage->previous_arm)
      stage->hardware_state_client = stage->candidate->retain_entity(
          node->create_client<HardwareStateService>(
              controller.controller_manager + "/set_hardware_component_state"));
  }
  if (stage->arm_transport) {
    const auto &transport = *stage->arm_transport;
    const auto reset = transport.mock_reset_position;
    const auto gate = stage->callbacks_enabled;
    const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak = weak_from_this();
    if (reset) {
      stage->reset_publisher = stage->candidate->retain_entity(
          node->create_publisher<std_msgs::msg::Float64MultiArray>(
              reset->commands_topic, rclcpp::QoS(1).reliable().durability_volatile()));
      if (reset->command_echo_topic)
        stage->reset_echo_subscription = stage->candidate->retain_entity(
            node->create_subscription<sensor_msgs::msg::JointState>(
                *reset->command_echo_topic, rclcpp::SensorDataQoS().keep_last(1),
                [weak, gate, joints = reset->joint_names](
                    const sensor_msgs::msg::JointState::ConstSharedPtr message) {
                  if (!gate->load(std::memory_order_acquire)) return;
                  std::vector<double> positions;
                  positions.reserve(joints.size());
                  for (const auto &joint : joints) {
                    const auto found = std::find(message->name.begin(), message->name.end(), joint);
                    const auto index = static_cast<std::size_t>(found - message->name.begin());
                    if (found == message->name.end() || index >= message->position.size()) return;
                    positions.push_back(message->position[index]);
                  }
                  const auto self = weak.lock();
                  if (!self) return;
                  {
                    std::lock_guard lock(self->mutex_);
                    if (!self->arm_) return;
                    const auto sequence = self->arm_->reset_echo
                        ? self->arm_->reset_echo->sequence + 1U : 1U;
                    self->arm_->reset_echo = ResetCommandEcho{
                        std::move(positions), self->clock_->now(), sequence};
                  }
                  self->reset_observation_wake_->notify();
                }));
    }
    stage->statistics_monitor = std::make_shared<RetainedControllerStatisticsMonitor>(
        RetainedControllerStatisticsConfig{
            transport.controller.controller_manager, transport.controller.controller_name,
            transport.adapter.lane_facts.controller_update_rate_hz},
        clock_owner_, [logger = node->get_logger(), gate](const std::string &text) {
          if (gate->load(std::memory_order_acquire)) RCLCPP_WARN(logger, "%s", text.c_str());
        });
    stage->statistics_subscription = stage->candidate->retain_entity(
        RetainedControllerStatisticsMonitor::Subscribe(node, stage->statistics_monitor));
    if (stage->measured_observer) {
      // OD-66: one time-critical group for this retained subscription.
      rclcpp::SubscriptionOptions measured_options;
      measured_options.callback_group = stage->candidate->create_time_critical_callback_group();
      stage->measured_subscription = stage->candidate->retain_entity(
          node->create_subscription<sensor_msgs::msg::JointState>(
              transport.measured_joint_state_lane->absolute_topic,
              rclcpp::SensorDataQoS().keep_last(1),
              [gate, observer = stage->measured_observer](
                  const sensor_msgs::msg::JointState::ConstSharedPtr message) {
                if (gate->load(std::memory_order_acquire)) observer(*message);
              }, measured_options),
          measured_options.callback_group);
    }
    if (stage->display_source && stage->display_channel)
      stage->display_subscription = stage->candidate->retain_entity(
          CreateDisplayJointSubscription(node, *stage->display_source, stage->display_channel, gate));
    if (stage->sample_observer || stage->measured_observer) {
      const auto rate = transport.adapter.lane_facts.state_rate_hz;
      if (!(rate > 0.0)) throw std::invalid_argument("ROS arm lane has no controller state rate");
      stage->sample_timer = stage->candidate->retain_entity(node->create_wall_timer(
          std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / rate)),
          [weak, gate, reset_wake = reset ? reset_observation_wake_ : nullptr]() {
            if (!gate->load(std::memory_order_acquire)) return;
            const auto self = weak.lock();
            if (!self) return;
            self->RefreshEndpointFacts();
            std::shared_ptr<ArmPort> port;
            std::function<void(const core::ExecutionControllerSampleDTO &)> observer;
            {
              std::lock_guard lock(self->mutex_);
              if (!self->arm_) return;
              port = self->arm_->port;
              observer = self->sample_observer_;
            }
            if (!observer) return;
            const auto sample = port->adapter().ReadSample();
            if (!sample) return;
            const std::pair<std::uint64_t, std::uint64_t> identity{
                sample->binding_epoch.value(), sample->local_receipt_count.value()};
            {
              std::lock_guard lock(self->mutex_);
              if (self->last_observed_sample_ == identity) return;
              self->last_observed_sample_ = identity;
            }
            observer(*sample);
            if (reset_wake) reset_wake->notify();
          }));
    }
  }
  if (source_snapshot) {
    stage->installed_sources = source_snapshot();
    if (stage->installed_sources && stage->installed_sources->ros_sources)
      stage->measured = StageMeasuredOwnerRebind(stage->installed_sources->ros_sources,
                                                stage->candidate, stage->activity_endpoints);
  }
  stage->published_owners = std::make_shared<const TransportOwners>(
      TransportOwners{stage->candidate, stage->epoch});
  if (stage->fault_wake) {
    const auto wake = stage->fault_wake;
    const auto observer = [wake](std::string_view) noexcept {
      try { wake(); } catch (...) {}
    };
    stage->candidate->set_executor_fault_observer(observer);
    if (stage->epoch) stage->epoch->set_executor_fault_observer(observer);
  }
  if (stage->epoch) stage->epoch->start();
  stage->candidate->start();
  return stage;
}

void RetainedRos2ControlRuntimeHandle::PublishStagedActivityLocked(
    const std::shared_ptr<StagedMachineOwner> &stage) {
  if (!stage->sample_sequence && !stage->matched_sequence) return;
  if (!stage->previous_arm && !stage->sample_sequence &&
      stage->activity_matches && *stage->activity_matches > 0U)
    return;
  RetainedRos2ControlArmActivity activity;
  std::optional<RetainedRos2ControlArmActivity> gripper_activity;
  const bool lost_after_sample = stage->matched_sequence > stage->sample_sequence &&
      stage->activity_matches && *stage->activity_matches == 0U;
  if (stage->activity_sample && !lost_after_sample) {
    activity.controller_manager_present = true;
    activity.observed = true;
    for (const auto &row : stage->activity_sample->controllers) {
      if (row.name == kJointStateBroadcasterControllerName) {
        activity.broadcaster_loaded =
            row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
            row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
        activity.broadcaster_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
      }
      if (row.name == stage->controller.controller_name) {
        activity.controller_loaded =
            row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
            row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
        activity.controller_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
      }
    }
    for (const auto &row : stage->activity_sample->hardware_components)
      if (row.name == stage->controller.hardware_component_name)
        activity.hardware_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
    if (stage->arm_transport && stage->arm_transport->gripper_controller) {
      gripper_activity = activity;
      gripper_activity->controller_loaded = false;
      gripper_activity->controller_active = false;
      for (const auto &row : stage->activity_sample->controllers) {
        if (row.name != stage->arm_transport->gripper_controller->controller_name) continue;
        gripper_activity->controller_loaded =
            row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
            row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
        gripper_activity->controller_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
      }
    }
  } else if (stage->activity_matches && *stage->activity_matches > 0U) {
    activity.controller_manager_present = true;
  }
  if (stage->previous_arm)
    ObserveActivity(activity, gripper_activity);
  else
    ObserveActivity({}, activity);
  if (stage->arm_transport && stage->arm_transport->mock_reset_position &&
      stage->activity_sample && !lost_after_sample) {
    std::lock_guard lock(mutex_);
    if (!arm_) return;
    arm_->reset_writer_loaded = false;
    arm_->reset_writer_active = false;
    for (const auto &row : stage->activity_sample->controllers) {
      if (row.name != stage->arm_transport->mock_reset_position->controller_name) continue;
      arm_->reset_writer_loaded =
          row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
          row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
      arm_->reset_writer_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
    }
    reset_observation_wake_->notify();
  }
}

void RetainedRos2ControlRuntimeHandle::ActivateStagedMachineOwner(
    const std::shared_ptr<StagedMachineOwner> &stage) {
  if (!stage || stage->candidate->executor_failure_started()) return;
  {
    std::lock_guard lock(mutex_);
    if (resources_ != stage->candidate) return;
  }
  std::lock_guard lock(stage->activity_mutex);
  PublishStagedActivityLocked(stage);
  stage->callbacks_enabled->store(true, std::memory_order_release);
}

struct PreparedMeasuredStateSources::Storage final {
  struct Channel final {
    std::unique_ptr<core::AddressedObservedStatePublication> publication;
    std::vector<double> positions;
    std::vector<double> velocities;
    // Last visible coordinates, mirroring the observed-state channel's render
    // generation: identical freshness samples do not wake the viewer.
    std::vector<double> render_positions;
    bool render_valid{false};
  };
  Storage(std::vector<core::MeasuredStateSourceDescriptor> descriptors,
          std::shared_ptr<core::IClock> clock, rclcpp::Node::SharedPtr node,
          std::shared_ptr<RetainedObservationWake> observation_wake,
          std::shared_ptr<RetainedObservationWake> render_wake,
          std::shared_ptr<core::StreamRecorder> stream,
          std::shared_ptr<core::StreamRecorder> gripper_stream,
          std::string gripper_joint)
      : descriptors(std::move(descriptors)), clock(std::move(clock)),
        node(std::move(node)), observation_wake(std::move(observation_wake)),
        render_wake(std::move(render_wake)), stream(std::move(stream)),
        gripper_stream(std::move(gripper_stream)),
        gripper_joint(std::move(gripper_joint)) {}
  ~Storage() {
    // The subscription is an owner borrow (`retain_entity`): its final
    // release is routed to the owner, so drop it after unlocking.
    rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr released;
    {
      std::lock_guard lock(mutex);
      active = false;
      released = std::move(subscription);
    }
  }
  std::mutex mutex;
  bool active{false};
  const std::vector<core::MeasuredStateSourceDescriptor> descriptors;
  const std::shared_ptr<core::IClock> clock;
  rclcpp::Node::SharedPtr node;
  const std::shared_ptr<RetainedObservationWake> observation_wake;
  const std::shared_ptr<RetainedObservationWake> render_wake;
  /// Liveness of the component's measured joints (may be null).
  const std::shared_ptr<core::StreamRecorder> stream;
  /// Liveness of a Mock gripper: samples carrying `gripper_joint`
  /// (may be null).
  const std::shared_ptr<core::StreamRecorder> gripper_stream;
  const std::string gripper_joint;
  std::vector<Channel> channels;
  std::vector<std::uint64_t> owner_rebuild_next_sequences;
  std::shared_ptr<const void> owner_rebuild_owner;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr subscription;
};

PreparedMeasuredStateSources::PreparedMeasuredStateSources(
    std::shared_ptr<Storage> storage,
    std::vector<std::shared_ptr<const core::IObservedStateProvider>> readers)
    : storage_(std::move(storage)), readers_(std::move(readers)) {}

const std::vector<core::MeasuredStateSourceDescriptor> &
PreparedMeasuredStateSources::descriptors() const noexcept {
  return storage_->descriptors;
}

std::optional<std::uint64_t> PreparedMeasuredStateSources::OwnerRebuildNextSequence(
    const std::size_t reader_index) const {
  std::lock_guard lock(storage_->mutex);
  if (reader_index >= storage_->owner_rebuild_next_sequences.size()) return std::nullopt;
  return storage_->owner_rebuild_next_sequences[reader_index];
}

std::optional<PreparedMeasuredStateSources::RebuildBoundary>
PreparedMeasuredStateSources::OwnerRebuildBoundary(const std::size_t reader_index) const {
  std::lock_guard lock(storage_->mutex);
  if (!storage_->owner_rebuild_owner || reader_index >= storage_->owner_rebuild_next_sequences.size())
    return std::nullopt;
  return RebuildBoundary{storage_->owner_rebuild_owner, storage_->owner_rebuild_next_sequences[reader_index]};
}

std::function<std::optional<RetainedPreparedRobotMeasuredSources::RebuildBoundary>(std::size_t)>
MakeOwnerRebuildBoundaryReader(std::shared_ptr<PreparedMeasuredStateSources> publication) {
  return [publication = std::move(publication)](std::size_t index)
      -> std::optional<RetainedPreparedRobotMeasuredSources::RebuildBoundary> {
    if (!publication) return std::nullopt;
    const auto boundary = publication->OwnerRebuildBoundary(index);
    if (!boundary) return std::nullopt;
    return RetainedPreparedRobotMeasuredSources::RebuildBoundary{
        boundary->owner, boundary->next_sequence};
  };
}

rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
RetainedRos2ControlRuntimeHandle::CreateMeasuredSubscription(
    const std::shared_ptr<PreparedMeasuredStateSources::Storage> &storage,
    const rclcpp::Node::SharedPtr &node, const std::string &topic,
    std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity_endpoints,
    std::shared_ptr<std::atomic<bool>> callbacks_enabled,
    rclcpp::CallbackGroup::SharedPtr callback_group) {
  const std::weak_ptr<PreparedMeasuredStateSources::Storage> weak_channels = storage;
  rclcpp::SubscriptionOptions options;
  options.callback_group = std::move(callback_group);
  return node->create_subscription<
          sensor_msgs::msg::JointState>(
          topic, rclcpp::SensorDataQoS().keep_last(1),
          [weak_channels, activity_endpoints, callbacks_enabled](
              const sensor_msgs::msg::JointState::ConstSharedPtr message) {
            if (callbacks_enabled &&
                !callbacks_enabled->load(std::memory_order_acquire)) return;
            const auto channels = weak_channels.lock();
            if (!channels)
              return;
            // Liveness: lock-free, no allocation.
            const auto stamp_ns =
                static_cast<std::int64_t>(message->header.stamp.sec) * 1'000'000'000LL +
                message->header.stamp.nanosec;
            if (channels->stream)
              channels->stream->Record(stamp_ns);
            if (channels->gripper_stream &&
                std::find(message->name.begin(), message->name.end(),
                          channels->gripper_joint) != message->name.end())
              channels->gripper_stream->Record(stamp_ns);
            std::unique_lock publication_lock(channels->mutex);
            if (!channels->active)
              return;
            const auto received = channels->clock->now();
            bool render_changed = false;
            const auto record_render = [&](auto &channel, const bool visible) {
              if (!channels->render_wake) return;
              if (!visible) {
                render_changed = render_changed || channel.render_valid;
                channel.render_valid = false;
                return;
              }
              if (channel.render_valid && channel.render_positions == channel.positions)
                return;
              channel.render_positions = channel.positions;
              channel.render_valid = true;
              render_changed = true;
            };
            const auto source_time = static_cast<std::int64_t>(message->header.stamp.sec) *
                                         1'000'000'000LL + message->header.stamp.nanosec;
            for (std::size_t channel_index = 0;
                 channel_index < channels->channels.size(); ++channel_index) {
              auto &channel = channels->channels[channel_index];
              const auto &descriptor = channels->descriptors[channel_index];
              const auto &config = descriptor.publication;
              // Preserve the existing bounded numeric producer's rejection for
              // oversized input; do not allocate or truncate the ROS sample.
              if (message->name.size() > config.maximum_source_resource_count) {
                const auto published = channel.publication->publish(
                    core::ObservedStateSourceKind::kJointState, message->name,
                    message->position, message->velocity, source_time, received);
                if (published.published) record_render(channel, false);
                continue;
              }
              bool complete = message->name.size() == message->position.size();
              const bool velocities = message->velocity.size() == message->name.size();
              for (std::size_t index = 0; complete && index < channel.positions.size(); ++index) {
                const auto &name = descriptor.source_joint_names[index];
                const auto found = std::find(message->name.begin(), message->name.end(), name);
                complete = found != message->name.end() &&
                    std::find(std::next(found), message->name.end(), name) == message->name.end();
                if (!complete)
                  break;
                const auto source_index = static_cast<std::size_t>(found - message->name.begin());
                channel.positions[index] = message->position[source_index];
                if (velocities)
                  channel.velocities[index] = message->velocity[source_index];
              }
              // An incomplete/new invalid sample replaces prior completeness;
              // source names never become authority or compiled coordinate names.
              const auto published = channel.publication->publish(
                  core::ObservedStateSourceKind::kJointState,
                  config.query_context.joint_names,
                  complete ? std::span<const double>(channel.positions) : std::span<const double>{},
                  complete && velocities ? std::span<const double>(channel.velocities) :
                                           std::span<const double>{},
                  source_time, received);
              if (published.published) record_render(channel, published.complete);
            }
            publication_lock.unlock();
            if (channels->observation_wake)
              channels->observation_wake->notify();
            if (render_changed)
              channels->render_wake->notify();
            // This existing NRT sample checkpoint also sees graph edges after
            // the latched activity's matched callback. Refresh's unchanged
            // clean/event fast path performs no SDK graph query when steady.
            if (activity_endpoints)
              activity_endpoints->Refresh();
          }, options);
}

struct RetainedRos2ControlRuntimeHandle::StagedMeasuredOwnerRebind final {
  const RetainedRos2ControlRuntimeHandle *handle{};
  std::shared_ptr<PreparedMeasuredStateSources> installed;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> previous_owner;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> candidate;
  rclcpp::Node::SharedPtr previous_node;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr previous_subscription;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr candidate_subscription;
  std::shared_ptr<std::atomic<bool>> callbacks_enabled;
  std::vector<std::uint64_t> next_sequences;
  std::shared_ptr<const void> owner_marker;
};

std::shared_ptr<RetainedRos2ControlRuntimeHandle::StagedMeasuredOwnerRebind>
RetainedRos2ControlRuntimeHandle::StageMeasuredOwnerRebind(
    std::shared_ptr<PreparedMeasuredStateSources> installed,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> candidate,
    std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> candidate_activity) {
  if (!installed || !candidate)
    throw std::invalid_argument("measured source rebind requires installed source and owner");
  auto stage = std::make_shared<StagedMeasuredOwnerRebind>();
  stage->handle = this;
  stage->installed = std::move(installed);
  stage->candidate = std::move(candidate);
  stage->owner_marker = stage->candidate;
  stage->callbacks_enabled = std::make_shared<std::atomic<bool>>(false);
  std::string topic;
  {
    std::lock_guard lock(mutex_);
    stage->previous_owner = resources_;
    if (!stage->previous_owner || stage->previous_owner == stage->candidate ||
        (!arm_ && !gripper_only_))
      throw std::logic_error("measured source rebind has no exact previous owner or lane");
    const auto &lane = arm_ ? arm_->transport.measured_joint_state_lane
                            : gripper_only_->measured_lane;
    if (!lane)
      throw std::logic_error("measured source rebind has no exact previous owner or lane");
    if (gripper_only_ && gripper_only_->activity_endpoints && !candidate_activity)
      throw std::logic_error("measured source rebind requires the candidate activity endpoint");
    topic = lane->absolute_topic;
  }
  const auto storage = stage->installed->storage_;
  bool has_channels = false;
  {
    std::lock_guard lock(storage->mutex);
    stage->previous_node = storage->node;
    stage->previous_subscription = storage->subscription;
    has_channels = !storage->channels.empty();
    stage->next_sequences.resize(storage->channels.size());
    if (stage->previous_node != stage->previous_owner->node())
      throw std::logic_error("measured source belongs to another ROS owner");
  }
  if (has_channels) {
    stage->candidate->invoke_creation([&] {
      // OD-66: one time-critical group for this retained subscription.
      auto group = stage->candidate->create_time_critical_callback_group();
      auto subscription = CreateMeasuredSubscription(
          storage, stage->candidate->node(), topic, std::move(candidate_activity),
          stage->callbacks_enabled, group);
      stage->candidate_subscription =
          stage->candidate->retain_entity(std::move(subscription), std::move(group));
    });
  }
  return stage;
}

core::Result<void> RetainedRos2ControlRuntimeHandle::CommitMeasuredOwnerRebind(
    const std::shared_ptr<StagedMeasuredOwnerRebind> &stage) {
  if (CommitMeasuredOwnerRebindWith(stage, [](void *) noexcept { return true; }, nullptr))
    return core::Result<void>::Ok();
  return reject<void>("measured_state_owner_mismatch", "measured source owner is not ready or changed during rebind");
}

bool RetainedRos2ControlRuntimeHandle::CommitMeasuredOwnerRebindWith(
    const std::shared_ptr<StagedMeasuredOwnerRebind> &stage,
    bool (*commit)(void *) noexcept, void *context,
    const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &failed_dependency) {
  if (!stage || stage->handle != this || !stage->previous_owner->joined() ||
      !commit ||
      !(stage->previous_owner->executor_failure_started() ||
        (failed_dependency && failed_dependency->joined() &&
         failed_dependency->executor_failure_started())) ||
      !stage->candidate->started() || stage->candidate->joined() ||
      stage->candidate->executor_failure_started())
    return false;
  const auto storage = stage->installed->storage_;
  std::lock_guard handle_lock(mutex_);
  if (resources_ != stage->previous_owner && resources_ != stage->candidate)
    return false;
  std::lock_guard storage_lock(storage->mutex);
  if (storage->node != stage->previous_node ||
      storage->subscription != stage->previous_subscription ||
      stage->callbacks_enabled->load(std::memory_order_acquire))
    return false;
  if (!commit(context))
    return false;
  for (std::size_t i = 0; i < storage->channels.size(); ++i)
    stage->next_sequences[i] = storage->channels[i].publication->numeric_publication().next_sequence();
  storage->owner_rebuild_next_sequences.swap(stage->next_sequences);
  storage->owner_rebuild_owner.swap(stage->owner_marker);
  storage->node = stage->candidate->node();
  storage->subscription = stage->candidate_subscription;
  return true;
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ActivateMeasuredOwnerRebind(
    const std::shared_ptr<StagedMeasuredOwnerRebind> &stage) {
  if (!stage || stage->handle != this || stage->candidate->joined() ||
      stage->candidate->executor_failure_started())
    return reject<void>("measured_state_owner_mismatch", "measured source owner is not ready");
  const auto storage = stage->installed->storage_;
  std::lock_guard handle_lock(mutex_);
  if (resources_ != stage->candidate)
    return reject<void>("measured_state_owner_mismatch", "measured source handle changed during rebind");
  std::lock_guard storage_lock(storage->mutex);
  if (storage->node != stage->candidate->node() ||
      storage->subscription != stage->candidate_subscription)
    return reject<void>("measured_state_owner_mismatch", "measured source changed during rebind");
  stage->callbacks_enabled->store(true, std::memory_order_release);
  return core::Result<void>::Ok();
}

bool RetainedRos2ControlRuntimeHandle::OwnsPreparedMeasuredSource(
    const std::shared_ptr<PreparedMeasuredStateSources> &source) const {
  if (!source) return false;
  std::lock_guard handle_lock(mutex_);
  if (!resources_) return false;
  std::lock_guard storage_lock(source->storage_->mutex);
  return source->storage_->node == resources_->node();
}

void PreparedMeasuredStateSources::EnablePublication() noexcept {
  std::lock_guard lock(storage_->mutex);
  storage_->active = true;
}

core::Result<std::shared_ptr<PreparedMeasuredStateSources>>
RetainedRos2ControlRuntimeHandle::StageMeasuredStateSources(
    std::vector<core::MeasuredStateSourceDescriptor> descriptors,
    std::shared_ptr<core::IClock> receiver_clock,
    const core::RuntimeParticipantBinding &candidate_motion,
    const std::optional<core::RuntimeParticipantBinding> &candidate_gripper,
    std::shared_ptr<RetainedObservationWake> render_wake) {
  return StageMeasuredStateSources(std::move(descriptors), std::move(receiver_clock),
      std::optional(candidate_motion), candidate_gripper, std::move(render_wake));
}

core::Result<std::shared_ptr<PreparedMeasuredStateSources>>
RetainedRos2ControlRuntimeHandle::StageMeasuredStateSources(
    std::vector<core::MeasuredStateSourceDescriptor> descriptors,
    std::shared_ptr<core::IClock> receiver_clock,
    const std::optional<core::RuntimeParticipantBinding> &candidate_motion,
    const std::optional<core::RuntimeParticipantBinding> &candidate_gripper,
    std::shared_ptr<RetainedObservationWake> render_wake) {
  using Sources = std::shared_ptr<PreparedMeasuredStateSources>;
  using Readers = std::vector<std::shared_ptr<const core::IObservedStateProvider>>;
  // Validate and capture under `mutex_`; the subscription is created on the
  // owner after releasing it (no state lock across a ROS call).
  std::unique_lock lock(mutex_);
  if (!resources_ || (!arm_ && !gripper_only_) || !receiver_clock ||
      clock_ != receiver_clock.get() || weak_from_this().expired())
    return reject<Sources>(
        "measured_state_owner_mismatch",
        "Measured source requires the bound node and its exact owning clock.");
  if (motion_binding_.has_value() != candidate_motion.has_value() ||
      candidate_gripper.has_value() != gripper_binding_.has_value())
    return reject<Sources>(
        "measured_state_candidate_owner_mismatch",
        "Measured candidate must retain the bound motion and tool owners.");
  const auto motion_difference = candidate_motion
      ? measured_candidate_physical_owner_difference(*motion_binding_, *candidate_motion)
      : std::string_view{};
  const auto gripper_difference =
      candidate_gripper ? measured_candidate_physical_owner_difference(
                              *gripper_binding_, *candidate_gripper)
                        : std::string_view{};
  const auto difference =
      motion_difference.empty() ? gripper_difference : motion_difference;
  if (!difference.empty())
    return reject<Sources>(
        "measured_state_candidate_owner_mismatch",
        "Measured candidate differs from the bound physical owner: " +
            std::string(difference));
  const auto& measured_lane = arm_ ? arm_->transport.measured_joint_state_lane : gripper_only_->measured_lane;
  if (!measured_lane)
    return reject<Sources>(
        "measured_state_lane_unavailable",
        "Bound lane has no qualified continuous measured source.");
  const auto &lane = *measured_lane;
  if (lane.absolute_topic.empty() || lane.absolute_topic.front() != '/' ||
      lane.freshness_budget_ns <= 0 ||
      lane.measured_state_authority_id.value() !=
          kMeasuredJointStateAuthorityId ||
      lane.measured_source_profile_id != kMeasuredJointStateProfileId)
    return reject<Sources>(
        "measured_state_lane_unqualified",
        "Bound lane measured source contract is unavailable.");
  for (const auto &descriptor : descriptors) {
    const auto valid = descriptor.validate();
    if (!valid.ok())
      return core::Result<Sources>::Err(valid.error());
    const auto &config = descriptor.publication;
    const auto &query = config.query_context;
    const auto matches =
        [&](const core::RuntimeParticipantBinding *binding) {
          if (!binding)
            return false;
          auto expected = core::make_observed_state_query_context(
              *binding, lane.freshness_budget_ns);
          if (!expected.ok())
            return false;
          // Only the secondary measured role differs from the candidate's
          // execution authority. Route association remains exact throughout.
          expected.value().state_authority_id =
              lane.measured_state_authority_id;
          expected.value().state_authority_class =
              core::StateAuthorityClass::kMeasured;
          if (query != expected.value() ||
              config.authority_generation !=
                  query.active_control_configuration_generation.value())
            return false;
          const auto &resources = binding->value().resources;
          if (resources.size() != descriptor.source_joint_names.size())
            return false;
          for (std::size_t index = 0; index < resources.size(); ++index)
            if (!resources[index].source_joint_name ||
                *resources[index].source_joint_name !=
                    descriptor.source_joint_names[index])
              return false;
          return true;
        };
    if ((!matches(candidate_motion ? &*candidate_motion : nullptr) &&
         !matches(candidate_gripper ? &*candidate_gripper : nullptr)) ||
        config.source_precedence.front().source_id != lane.absolute_topic ||
        config.source_precedence.front().authority_profile_id !=
            lane.measured_source_profile_id)
      return reject<Sources>(
          "measured_state_physical_source_mismatch",
          "Measured source differs from the actual bound physical lane.");
  }
  // Every staged subscription records the streams declared at bind.
  auto replacement = std::make_shared<PreparedMeasuredStateSources::Storage>(
      std::move(descriptors), std::move(receiver_clock), resources_->node(),
      arm_ && arm_->transport.mock_reset_position ? reset_observation_wake_
                                                  : nullptr,
      std::move(render_wake), measured_stream_, gripper_feedback_stream_,
      gripper_feedback_stream_
          ? *gripper_binding_->value().resources.front().source_joint_name
          : std::string());
  Readers readers;
  replacement->channels.reserve(replacement->descriptors.size());
  readers.reserve(replacement->descriptors.size());
  for (const auto &descriptor : replacement->descriptors) {
    const auto count = descriptor.source_joint_names.size();
    auto publication =
        std::make_unique<core::AddressedObservedStatePublication>(
            descriptor.publication);
    readers.emplace_back(replacement, publication.get());
    replacement->channels.push_back({std::move(publication),
                                     std::vector<double>(count),
                                     std::vector<double>(count),
                                     std::vector<double>(count)});
  }
  if (!replacement->channels.empty()) {
    // The callback retains only the exact endpoint facts, not the handle.
    const auto activity_endpoints =
        gripper_only_ ? gripper_only_->activity_endpoints : nullptr;
    const auto owner = resources_;
    const auto arm = arm_;
    const auto gripper_only = gripper_only_;
    const std::string topic = lane.absolute_topic;
    lock.unlock();
    rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr subscription;
    try {
      owner->invoke_creation([&] {
        // OD-66: one time-critical group for this retained subscription.
        auto group = owner->create_time_critical_callback_group();
        subscription = owner->retain_entity(
            CreateMeasuredSubscription(replacement, replacement->node, topic,
                                       activity_endpoints, {}, group),
            group);
      });
    } catch (const std::exception &error) {
      // A valid commanded route must not gain a measured transport admission
      // rule. Keep the actual operational failure visible at its producer.
      RCLCPP_WARN(replacement->node->get_logger(), "Measured state subscription unavailable: %s", error.what());
      return core::Result<Sources>::Ok(nullptr);
    }
    lock.lock();
    // A lost race drops the subscription; its borrow releases on the owner.
    if (resources_ != owner || arm_ != arm || gripper_only_ != gripper_only) {
      lock.unlock();
      return reject<Sources>(
          "measured_state_owner_mismatch",
          "Measured source requires the bound node and its exact owning clock.");
    }
    std::lock_guard storage_lock(replacement->mutex);
    replacement->subscription = std::move(subscription);
  }
  return core::Result<Sources>::Ok(Sources(new PreparedMeasuredStateSources(
      std::move(replacement), std::move(readers))));
}

// Holds the internal lease of every external owner. Its destructor runs on
// the thread that releases the last external owner, which is never one of the
// handle's executor threads: runtime callbacks hold internal leases only.
struct RetainedRos2ControlRuntimeHandle::ExternalOwner final {
  explicit ExternalOwner(std::shared_ptr<RetainedRos2ControlRuntimeHandle> owned)
      : handle(std::move(owned)) {}
  ExternalOwner(const ExternalOwner &) = delete;
  ExternalOwner &operator=(const ExternalOwner &) = delete;
  // Quiesce while this lease still keeps every child alive; a callback lease
  // in flight ends inside the join. Destruction then follows on this thread
  // unless a non-executor lease (a gripper command's display) still runs.
  ~ExternalOwner() {
    if (handle->StopAndQuiesce().outcome != HostQuiesceOutcome::kSettled) {
      std::fputs("{\"code\":\"execution_host_shutdown_incomplete\",\"phase\":\"final_owner_quiesce\"}\n", stderr);
      std::fflush(stderr);
      std::_Exit(component_manager::kExecutionHostShutdownIncompleteExitStatus);
    }
  }
  const std::shared_ptr<RetainedRos2ControlRuntimeHandle> handle;
};

std::shared_ptr<RetainedRos2ControlRuntimeHandle>
RetainedRos2ControlRuntimeHandle::Create() {
  auto owner = std::make_shared<ExternalOwner>(
      std::make_shared<RetainedRos2ControlRuntimeHandle>(ConstructionKey{}));
  auto *const handle = owner->handle.get();
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> external(std::move(owner),
                                                             handle);
  handle->external_owner_ = external;
  return external;
}

RetainedRos2ControlRuntimeHandle::HostQuiesceResult
RetainedRos2ControlRuntimeHandle::StopAndQuiesce(
    const std::chrono::steady_clock::time_point deadline) noexcept {
  const auto started = std::chrono::steady_clock::now();
  // The lane guard stops first (it calls back into this handle). Children
  // then close admission while callbacks and borrowed resources are
  // quiesced; repeated calls repeat only their idempotent local shutdown.
  std::optional<RetainedRos2ControlArmGuardHooks> hooks;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> epoch_resources;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> machine_resources;
  ros2_control_execution::Ros2GripperCommandBackend *gripper = nullptr;
  bool had_arm = false;
  core::Duration lifecycle_timeout{};
  std::optional<core::Duration> gripper_timeout;
  {
    std::lock_guard lock(mutex_);
    if (shutdown_complete_)
      return {HostQuiesceOutcome::kSettled, "", ""};
    closing_ = true;
    machine_resources = resources_;
    gripper = gripper_.get();
    gripper_timeout = owner_shutdown_timeout_ns_;
    hooks = std::move(guard_hooks_);
    guard_hooks_.reset();
    if (arm_) {
      had_arm = true;
      adapter = arm_->adapter;
      epoch_resources = arm_->epoch_resources;
      lifecycle_timeout = arm_->transport.adapter.lifecycle_command_timeout_ns;
    } else if (owner_shutdown_timeout_ns_) lifecycle_timeout = *owner_shutdown_timeout_ns_;
  }
  // A guard worker in a poll holds an internal lease; joining it here, while
  // this owner still holds one, keeps that poll from being the last owner.
  if (hooks && hooks->stop)
    hooks->stop();
  hooks.reset();
  if (!machine_resources && !epoch_resources && !gripper) {
    std::lock_guard lock(mutex_);
    if (!resources_ && !gripper_ && !arm_)
      return {HostQuiesceOutcome::kSettled, "", ""};
    return {HostQuiesceOutcome::kUnresolved, "join", "internal_error"};
  }
  if (deadline == std::chrono::steady_clock::time_point::max() && lifecycle_timeout <= 0)
    return {HostQuiesceOutcome::kUnresolved, "join", "internal_error"};
  const auto effective_deadline = lifecycle_timeout > 0
      ? std::min(deadline, started + std::chrono::nanoseconds(lifecycle_timeout))
      : deadline;
  auto machine_deadline = effective_deadline;
  // JTC epoch resources are independently created under the machine context;
  // the machine owner stays live until gripper cancellation is settled.
  if (adapter) {
    const bool leaf_drained = adapter->close_admission(effective_deadline);
    auto unresolved = adapter->ReadUnresolvedLateCancels();
    const bool has_unresolved = !unresolved.empty();
    {
      std::lock_guard lock(mutex_);
      RetainLateCancelObligationsLocked(std::move(unresolved));
    }
    const auto handoff = FlushLateCancelHandoff();
    if (!handoff.ok())
      return {HostQuiesceOutcome::kUnresolved, "journal_handoff", handoff.error().code};
    if (!leaf_drained)
      return {HostQuiesceOutcome::kUnresolved, "leaf", "component_owner_stalled"};
    if (!adapter->loss_cancel_shutdown_settled() && !has_unresolved)
      return {HostQuiesceOutcome::kUnresolved, "late_cancel_close", "component_owner_stalled"};
  }
  if (epoch_resources && !epoch_resources->quiesce_until(effective_deadline))
    return {HostQuiesceOutcome::kUnresolved, "join", "component_owner_stalled"};
  if (gripper) {
    const auto gripper_deadline = std::min(deadline, started + std::chrono::nanoseconds(gripper_timeout.value()));
    machine_deadline = std::min(machine_deadline, gripper_deadline);
    const auto status = gripper->CloseGripperLateCancels(gripper_deadline);
    if (status == ros2_control_execution::Ros2GripperCommandBackend::LateCancelShutdownStatus::kStuck)
      return {HostQuiesceOutcome::kUnresolved, "late_cancel_close", "component_owner_stalled"};
    if (status == ros2_control_execution::Ros2GripperCommandBackend::LateCancelShutdownStatus::kPendingOrFailed)
      return {HostQuiesceOutcome::kUnresolved, "late_cancel_close", "internal_error"};
    // kDurableUnknown is already journaled; its recovery claim remains live.
    // Stop the gripper's producers while its owner and the borrowed scheduler
    // are still live; the destructor's shutdown() is then a no-op.
    gripper->shutdown();
    if (!gripper->StopSceneProjectionUntil(gripper_deadline))
      return {HostQuiesceOutcome::kUnresolved, "join", "component_owner_stalled"};
  }
  if (machine_resources && !machine_resources->quiesce_until(machine_deadline))
    return {HostQuiesceOutcome::kUnresolved, "join", "component_owner_stalled"};
  {
    std::lock_guard lock(mutex_);
    if (resources_ != machine_resources || gripper_.get() != gripper || bool(arm_) != had_arm ||
        (arm_ && (arm_->adapter != adapter || arm_->epoch_resources != epoch_resources)))
      return {HostQuiesceOutcome::kUnresolved, "join", "internal_error"};
  }
  if (adapter) adapter->release_ros_entities();
  {
    std::lock_guard lock(mutex_);
    shutdown_complete_ = true;
  }
  return {HostQuiesceOutcome::kSettled, "", ""};
}

RetainedRos2ControlRuntimeHandle::HostQuiesceResult
RetainedRos2ControlRuntimeHandle::QuiesceForHostShutdown(
    const std::chrono::steady_clock::time_point deadline) noexcept {
  const char *stage = "join";
  try {
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
    }
    const auto retained_participant = participant();
    if (retained_participant) {
      const auto detached = retained_participant->DetachSchedulerForHostShutdown(deadline);
      if (!detached.ok()) {
        const auto reason = detached.error().details.find("reason");
        if (reason != detached.error().details.end() && reason->second == "scheduler_detach_timeout")
          return {HostQuiesceOutcome::kUnresolved, stage, "component_owner_stalled"};
        return {HostQuiesceOutcome::kUnresolved, stage, detached.error().code};
      }
    }
    const auto stopped = StopAndQuiesce(deadline);
    if (stopped.outcome != HostQuiesceOutcome::kSettled) return stopped;
    return {HostQuiesceOutcome::kSettled, "", ""};
  } catch (...) {
    return {HostQuiesceOutcome::kUnresolved, stage, "internal_error"};
  }
}

std::optional<std::chrono::steady_clock::time_point>
RetainedRos2ControlRuntimeHandle::OwnerShutdownDeadline() const noexcept {
  core::Duration timeout{};
  {
    std::lock_guard lock(mutex_);
    if (arm_) timeout = arm_->transport.adapter.lifecycle_command_timeout_ns;
    else if (owner_shutdown_timeout_ns_) timeout = *owner_shutdown_timeout_ns_;
  }
  if (timeout <= 0) return std::nullopt;
  return std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::SetOwnerShutdownTimeout(
    const core::Duration timeout_ns) {
  if (timeout_ns <= 0)
    return reject<void>("owner_shutdown_timeout_invalid", "Owner shutdown timeout must be positive");
  std::lock_guard lock(mutex_);
  if (owner_shutdown_timeout_ns_ && *owner_shutdown_timeout_ns_ != timeout_ns)
    return reject<void>("owner_shutdown_timeout_changed", "Owner shutdown timeout changed");
  owner_shutdown_timeout_ns_ = timeout_ns;
  return core::Result<void>::Ok();
}

RetainedRos2ControlRuntimeHandle::~RetainedRos2ControlRuntimeHandle() {
  if (StopAndQuiesce().outcome != HostQuiesceOutcome::kSettled) {
    std::fputs("{\"code\":\"execution_host_shutdown_incomplete\",\"phase\":\"handle_quiesce\"}\n", stderr);
    std::fflush(stderr);
    std::_Exit(component_manager::kExecutionHostShutdownIncompleteExitStatus);
  }
  sample_timer_.reset();
  sample_observer_ = {};
  measured_state_subscription_.reset();
  display_joint_subscription_.reset();
  measured_state_observer_ = {};
  tool_context_hook_.reset();
  participant_.reset();
  arm_.reset();
  gripper_only_.reset();
  gripper_.reset();
  resources_.reset();
}


core::Result<void> RetainedRos2ControlRuntimeHandle::BindGripperOnly(
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> resources,
    core::IClock& clock,
    std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend> gripper,
    const core::RuntimeParticipantBinding& binding,
    RetainedRos2ControlArmController controller,
    std::shared_ptr<core::IClock> clock_owner,
    std::optional<RetainedMeasuredJointStateLane> measured_lane) {
  const auto reject_owned = [&](const char* reason, const char* message) {
    if (gripper) gripper->shutdown();
    if (resources) resources->quiesce();
    gripper.reset();
    resources.reset();
    return reject<void>(reason, message);
  };
  const bool no_output = gripper &&
      gripper->gripper_output_mode(binding.value().command_target_id.value()) == core::GripperOutputMode::kNone &&
      gripper->gripper_actuation_kind(binding.value().command_target_id.value()) == core::GripperActuationKind::kPneumatic;
  // A commanded owner without a controller manager is an Isaac transport
  // (merge 6 tool): its lifecycle is the action bridge's, as for a robot that
  // carries its Isaac gripper.
  const bool no_controller = controller.controller_manager.empty() &&
      controller.controller_name.empty() && controller.hardware_component_name.empty();
  const bool controller_valid = no_controller ||
      (!no_output && !controller.controller_manager.empty() &&
       !controller.controller_name.empty() && !controller.hardware_component_name.empty());
  if (!resources || !gripper || !binding.value().control_path || !controller_valid ||
      (clock_owner && clock_owner.get() != &clock))
    return reject_owned("ros2_control_gripper_only_binding_invalid",
                        "Gripper-only transport requires its exact binding/controller/clock");
  const auto domain = binding.value().control_path->control_domain_id;
  auto owner = std::make_shared<GripperOnlyState>();
  owner->controller = std::move(controller);
  owner->physical_controller_required = !no_controller;
  owner->measured_lane = std::move(measured_lane);
  if (owner->physical_controller_required) {
    // StageMeasuredStateSources precedes StartBackend and captures this same
    // facts object without retaining this handle. No subscription starts here.
    owner->activity_endpoints =
        std::make_shared<ros2_control_execution::SelectedRosEndpointFacts>(
            resources->node(), owner->controller.controller_manager + "/activity");
  }
  std::unique_lock lock(mutex_);
  if (closing_) {
    lock.unlock();
    return reject_owned("ros2_control_handle_closing", "ROS runtime handle is closing");
  }
  if (resources_ || arm_ || gripper_only_) {
    lock.unlock();
    return reject_owned("ros2_control_handle_already_bound", "ROS runtime handle is already bound");
  }
  auto streams = declare_bound_component_streams(owner->measured_lane, binding, binding);
  if (!streams.ok()) {
    if (gripper) gripper->shutdown();
    if (resources) resources->quiesce();
    return core::Result<void>::Err(streams.error());
  }
  auto published_owners = std::make_shared<const TransportOwners>(TransportOwners{resources, {}});
  measured_stream_ = std::move(streams.value().measured);
  gripper_feedback_stream_ = std::move(streams.value().gripper_feedback);
  clock_owner_ = std::move(clock_owner);
  clock_ = &clock;
  resources_ = std::move(resources);
  gripper_ = std::move(gripper);
  gripper_binding_ = binding;
  gripper_domain_id_ = domain;
  gripper_id_ = binding.value().command_target_id.value();
  gripper_only_ = std::move(owner);
  published_transport_owners_.store(std::move(published_owners), std::memory_order_release);
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::BindProcessOnly(
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> resources,
    core::IClock &clock, RetainedRos2ControlArmController controller,
    std::shared_ptr<core::IClock> clock_owner) {
  const auto cleanup_started = std::chrono::steady_clock::now();
  std::optional<core::Duration> cleanup_timeout;
  {
    std::lock_guard lock(mutex_);
    cleanup_timeout = owner_shutdown_timeout_ns_;
  }
  const auto cleanup = [&] {
    if (!resources) return;
    if (!cleanup_timeout || *cleanup_timeout <= 0 ||
        !resources->quiesce_until(cleanup_started + std::chrono::nanoseconds(*cleanup_timeout))) {
      std::fputs("{\"code\":\"execution_host_shutdown_incomplete\",\"phase\":\"bind_cleanup\"}\n", stderr);
      std::fflush(stderr);
      std::_Exit(component_manager::kExecutionHostShutdownIncompleteExitStatus);
    }
  };
  if (!resources || controller.controller_manager.empty() ||
      controller.hardware_component_name.empty() || !controller.controller_name.empty() ||
      (clock_owner && clock_owner.get() != &clock)) {
    cleanup();
    return reject<void>("ros2_control_process_only_binding_invalid",
                        "A machine's process requires its controller manager, hardware and clock");
  }
  // The no-arm state of a gripper-only binding, without a gripper: its
  // activity subscription observes the controller manager and the hardware.
  auto owner = std::make_shared<GripperOnlyState>();
  owner->controller = std::move(controller);
  owner->activity_endpoints = std::make_shared<ros2_control_execution::SelectedRosEndpointFacts>(
      resources->node(), owner->controller.controller_manager + "/activity");
  std::unique_lock lock(mutex_);
  if (closing_) {
    lock.unlock();
    cleanup();
    return reject<void>("ros2_control_handle_closing", "ROS runtime handle is closing");
  }
  if (resources_ || arm_ || gripper_only_) {
    lock.unlock();
    cleanup();
    return reject<void>("ros2_control_handle_already_bound", "ROS runtime handle is already bound");
  }
  auto shared_owner = std::shared_ptr<ros2_control_execution::Ros2TransportResources>(std::move(resources));
  auto published_owners = std::make_shared<const TransportOwners>(TransportOwners{shared_owner, {}});
  clock_owner_ = std::move(clock_owner);
  clock_ = &clock;
  resources_ = std::move(shared_owner);
  gripper_only_ = std::move(owner);
  published_transport_owners_.store(std::move(published_owners), std::memory_order_release);
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::Bind(
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> resources,
    core::IClock &clock, core::IScheduler &scheduler, RetainedRos2ControlArmTransport arm,
    const ParticipantFactory &make_participant,
    std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend> gripper,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    std::shared_ptr<core::IClock> clock_owner) {
  const auto reject_owned = [&](const char *reason, const char *message) {
    if (gripper)
      gripper->shutdown();
    if (resources)
      resources->quiesce();
    gripper.reset();
    resources.reset();
    return reject<void>(reason, message);
  };
  if (!clock_owner || clock_owner.get() != &clock)
    return reject_owned(
        "ros2_control_handle_clock_owner_mismatch",
        "ROS runtime handle clock owner must name its exact receiver clock");
  if (!resources || !make_participant ||
      gripper_binding.has_value() != (gripper != nullptr) ||
      !motion_binding.value().control_path.has_value() ||
      (gripper_binding.has_value() &&
       !gripper_binding->value().control_path.has_value()) ||
      arm.compiled_joint_names.size() !=
          arm.adapter.lane_facts.joint_names.size() ||
      arm.controller.controller_manager.empty() ||
      arm.controller.controller_name.empty() ||
      arm.controller.hardware_component_name.empty()) {
    return reject_owned("ros2_control_handle_binding_invalid",
                        "ROS runtime handle requires exact control-addressed "
                        "physical bindings and arm controller facts");
  }

  if (arm.gripper_controller.has_value() &&
      (!gripper_binding.has_value() ||
       arm.gripper_controller->controller_name.empty() ||
       arm.gripper_controller->controller_name ==
           arm.controller.controller_name ||
       arm.gripper_controller->controller_manager !=
           arm.controller.controller_manager ||
       arm.gripper_controller->hardware_component_name !=
           arm.controller.hardware_component_name)) {
    return reject_owned(
        "ros2_control_gripper_controller_binding_invalid",
        "the gripper controller requires its exact domain in the "
        "same controller manager and hardware");
  }
  if (arm.mock_reset_position) {
    const auto &target = *arm.mock_reset_position;
    if (target.component_runtime_identity.empty() ||
        target.target_digest.empty() || target.controller_name.empty() ||
        target.commands_topic.empty() ||
        target.controller_name == arm.controller.controller_name ||
        target.joint_names != arm.adapter.lane_facts.joint_names ||
        target.positions.size() != target.joint_names.size() ||
        !std::all_of(target.positions.begin(), target.positions.end(),
                     [](double value) { return std::isfinite(value); }))
      return reject_owned(
          "mock_reset_target_binding_invalid",
          "Mock reset writer must retain its exact ordered immutable target");
  }
  const auto &motion_domain =
      motion_binding.value().control_path->control_domain_id;
  const auto gripper_domain =
      gripper_binding.has_value()
          ? std::optional(
                gripper_binding->value().control_path->control_domain_id)
          : std::nullopt;
  if (gripper_domain.has_value() && *gripper_domain == motion_domain) {
    return reject_owned("ros2_control_handle_domain_alias",
                        "motion and gripper bindings cannot alias one control "
                        "domain");
  }
  {
    std::unique_lock lock(mutex_);
    if (closing_ || arm_ || motion_domain_id_.has_value()) {
      const bool closing = closing_;
      lock.unlock();
      if (closing) return reject_owned("ros2_control_handle_closing", "ROS runtime handle is closing");
      return reject_owned("ros2_control_handle_already_bound",
                          "ROS runtime handle is already bound");
    }
  }

  auto state = std::make_shared<ArmState>();
  state->transport = std::move(arm);
  state->component_id = motion_binding.value().controllable_component_id.value();
  state->epoch = 1U;
  state->transport.adapter.binding_epoch =
      core::ExecutionBindingEpoch(state->epoch);
  std::shared_ptr<core::ExecutionPortParticipant> participant;
  try {
    state->epoch_resources = make_jtc_epoch_resources(*resources, state->component_id);
    state->adapter =
        std::make_shared<ros2_control_execution::Ros2ControlExecutionBackend>(
            state->epoch_resources, clock_owner, scheduler, state->transport.adapter);
    state->port = std::make_shared<ArmPort>(
        state->adapter, state->transport.compiled_joint_names,
        state->transport.adapter.lane_facts.joint_names, state->gate,
        state->reset_motion_admitted);
    participant = make_participant(state->port);
  } catch (const std::exception &error) {
    // Bind has not published the port or admitted a send at this point.
    if (state->adapter)
      state->adapter->shutdown();
    if (gripper)
      gripper->shutdown();
    resources->quiesce();
    return reject<void>("ros2_control_handle_participant_invalid", error.what());
  }
  if (!participant) {
    state->adapter->shutdown();
    return reject_owned("ros2_control_handle_participant_invalid",
                        "ROS runtime handle has no Core participant");
  }
  auto streams = declare_bound_component_streams(
      state->transport.measured_joint_state_lane, motion_binding, gripper_binding);
  if (!streams.ok()) {
    state->adapter->shutdown();
    if (gripper) gripper->shutdown();
    resources->quiesce();
    return core::Result<void>::Err(streams.error());
  }

  auto published_owners = std::make_shared<const TransportOwners>(
      TransportOwners{resources, state->epoch_resources});
  std::unique_lock lock(mutex_);
  if (closing_ || resources_ || arm_ || gripper_only_) {
    const bool closing = closing_;
    lock.unlock();
    return reject_owned(closing ? "ros2_control_handle_closing" : "ros2_control_handle_already_bound",
                        closing ? "ROS runtime handle is closing" : "ROS runtime handle is already bound");
  }
  measured_stream_ = std::move(streams.value().measured);
  gripper_feedback_stream_ = std::move(streams.value().gripper_feedback);
  clock_owner_ = std::move(clock_owner);
  resources_ = std::move(resources);
  clock_ = &clock;
  scheduler_ = &scheduler;
  arm_ = std::move(state);
  participant_ = std::move(participant);
  gripper_ = std::move(gripper);
  motion_binding_ = motion_binding;
  gripper_binding_ = gripper_binding;
  motion_domain_id_ = motion_domain;
  gripper_domain_id_ = gripper_domain;
  if (gripper_binding.has_value()) {
    gripper_id_ = gripper_binding->value().command_target_id.value();
  }
  published_transport_owners_.store(std::move(published_owners), std::memory_order_release);
  return core::Result<void>::Ok();
}

std::shared_ptr<core::ExecutionPortParticipant>
RetainedRos2ControlRuntimeHandle::participant() const {
  std::lock_guard lock(mutex_);
  return participant_;
}

std::shared_ptr<core::IExecutionPort>
RetainedRos2ControlRuntimeHandle::current_port() const {
  std::lock_guard lock(mutex_);
  return arm_ ? arm_->port : nullptr;
}

std::shared_ptr<ros2_control_execution::Ros2TransportResources>
RetainedRos2ControlRuntimeHandle::TestEpochResources() const {
  std::lock_guard lock(mutex_);
  return arm_ ? arm_->epoch_resources : nullptr;
}

std::vector<std::string>
RetainedRos2ControlRuntimeHandle::controller_joint_names() const {
  std::lock_guard lock(mutex_);
  return arm_ ? arm_->transport.adapter.lane_facts.joint_names
              : std::vector<std::string>{};
}

rclcpp::Node::SharedPtr RetainedRos2ControlRuntimeHandle::node() const {
  std::lock_guard lock(mutex_);
  return resources_ ? resources_->node() : nullptr;
}

void RetainedRos2ControlRuntimeHandle::InstallGuardHooks(
    RetainedRos2ControlArmGuardHooks hooks) {
  std::lock_guard lock(mutex_);
  if (!arm_ || start_attempted_ || guard_hooks_.has_value() ||
      !hooks.motion_admitted || !hooks.before_activation || !hooks.on_recover ||
      !hooks.fault) {
    throw std::logic_error(
        "ROS arm guard hooks are installed once, complete, before start");
  }
  *arm_->gate = hooks.motion_admitted;
  guard_hooks_ = std::move(hooks);
}

void RetainedRos2ControlRuntimeHandle::InstallToolContext(
    ToolContextHook hook, RetainedToolContextSourceFn source,
    CommittedToolContextSourceFn committed_source,
    CommittedToolContextSourceFn enable_source) {
  std::lock_guard lock(mutex_);
  if (tool_context_hook_ || !hook.apply || !source || !committed_source || !enable_source ||
      gripper_only_)
    throw std::logic_error("ROS arm tool context is installed once, complete");
  tool_context_hook_ = std::move(hook);
  tool_context_source_ = std::move(source);
  committed_tool_context_source_ = std::move(committed_source);
  committed_enable_tool_context_source_ = std::move(enable_source);
}

rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
RetainedRos2ControlRuntimeHandle::CreateDisplayJointSubscription(
    const rclcpp::Node::SharedPtr &node, const DisplayJointSource &source,
    const std::shared_ptr<RetainedObservedStateChannel> &channel,
    std::shared_ptr<std::atomic<bool>> callbacks_enabled) {
  return node->create_subscription<sensor_msgs::msg::JointState>(
          source.topic, rclcpp::SensorDataQoS().keep_last(1),
          [weak = weak_from_this(), weak_channel = std::weak_ptr(channel), use = source.use,
           clock = clock_, callbacks_enabled](const sensor_msgs::msg::JointState::ConstSharedPtr message) {
            if (callbacks_enabled &&
                !callbacks_enabled->load(std::memory_order_acquire)) return;
            const auto display = weak_channel.lock();
            if (!display) return;
            const auto binding = display->binding();
            const auto sample = display_joint_sample(binding, *message);
            if (!sample) return;
            const auto sample_use = use(clock->now(), sample->positions);
            if (sample_use == DisplayJointSampleUse::kIgnore) return;
            const bool leads = sample_use == DisplayJointSampleUse::kLeads;
            // The component's stream, declared at bind.
            if (const auto self = leads ? weak.lock() : nullptr) {
              std::shared_ptr<core::StreamRecorder> stream;
              {
                std::lock_guard lock(self->mutex_);
                stream = self->measured_stream_;
              }
              if (stream)
                stream->Record(static_cast<std::int64_t>(message->header.stamp.sec) *
                                   1'000'000'000LL +
                               message->header.stamp.nanosec);
            }
            if (display->update_render_positions(binding, sample->compiled_names,
                                                 sample->positions).ok())
              display->set_display_input_leads(leads);
          });
}

void RetainedRos2ControlRuntimeHandle::InstallDisplayJointSource(
    const DisplayJointSource &source, std::shared_ptr<RetainedObservedStateChannel> channel) {
  std::lock_guard lock(mutex_);
  if (display_joint_subscription_ || source.topic.empty() || !source.use || !arm_ ||
      !arm_->transport.measured_joint_state_lane || !motion_binding_ || !channel ||
      channel->binding().value() != motion_binding_->value() || resources_ == nullptr)
    throw std::logic_error("ROS arm display joint source is installed once, on a measured arm");
  channel->follow_display_input();
  display_joint_subscription_ = CreateDisplayJointSubscription(resources_->node(), source, channel);
  display_joint_source_ = std::make_shared<const DisplayJointSource>(source);
  display_joint_channel_ = channel;
}

void RetainedRos2ControlRuntimeHandle::InstallCommittedSceneSource(
    CommittedSceneSourceFn source) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || committed_scene_source_ || !source)
    throw std::logic_error("committed Scene source must be installed before start");
  committed_scene_source_ = std::move(source);
}

void RetainedRos2ControlRuntimeHandle::InstallActiveTcp(
    core::ControllableComponentId robot, std::vector<RobotTcp> rows,
    std::vector<TcpFrameAssociation> associations,
    const bool controller_tcp_offset_sync,
    std::optional<core::ProgramBindingId> direct_control_binding) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || active_tcp_robot_ || robot.empty())
    throw std::logic_error("ROS arm Active TCP is installed once, before start");
  ActiveTcp candidate(std::move(rows), std::move(associations));
  if (candidate.reference() && candidate.find(*candidate.reference()) == nullptr)
    throw std::logic_error("End-effector TCP lacks retained association");
  active_tcp_robot_ = std::move(robot);
  active_tcp_ = std::move(candidate);
  active_tcp_offset_sync_ = controller_tcp_offset_sync;
  active_tcp_direct_binding_ = std::move(direct_control_binding);
  // A greater registration generation at every install (runtime start or
  // restart) in this host process; its sequence starts at 0.
  active_tcp_registration_generation_ = next_active_tcp_registration();
  active_tcp_sequence_ = 0U;
}

bool RetainedRos2ControlRuntimeHandle::SerializedStepAdmissibleLocked(
    const SerializedStepKind kind) const {
  switch (kind) {
  case SerializedStepKind::kArm: return !lifecycle_step_ && !tool_payload_step_;
  case SerializedStepKind::kGripper: return !lifecycle_step_;
  case SerializedStepKind::kToolPayload:
    return !tool_payload_step_ && lifecycle_step_ != SerializedStepKind::kArm;
  }
  return false;
}

bool RetainedRos2ControlRuntimeHandle::MotionStartMeetsLifecycleStepLocked(
    const SerializedStepKind kind) const {
  // The payload update concerns the arm only; a TCP or re-arm step makes
  // the driver cycle its controllers, so any controller switch concerns it.
  return kind == SerializedStepKind::kToolPayload
             ? lifecycle_step_ == SerializedStepKind::kArm
             : lifecycle_step_.has_value();
}

namespace {

[[nodiscard]] core::ErrorDTO lifecycle_step_in_flight_error() {
  return core::ErrorDTO{std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
                        "A lifecycle step of this component is in progress",
                        {{"reason", "component_lifecycle_step_in_flight"}}};
}

} // namespace

// The wait holds no lock (the condition variable releases `mutex_`); the
// step's calls run with nothing held. Only the thread that runs the step
// waits here, never a ROS callback or an owner thread.
RetainedRos2ControlRuntimeHandle::SerializedStep::SerializedStep(
    RetainedRos2ControlRuntimeHandle &handle, const SerializedStepKind kind,
    const SerializedStepEntry entry)
    : handle_(handle), kind_(kind) {
  const bool motion_start = entry == SerializedStepEntry::kMotionStart;
  std::unique_lock lock(handle_.mutex_);
  handle_.serialized_step_released_.wait(lock, [&] {
    return handle_.SerializedStepAdmissibleLocked(kind_) ||
           (motion_start && handle_.MotionStartMeetsLifecycleStepLocked(kind_));
  });
  if (!handle_.SerializedStepAdmissibleLocked(kind_)) return;
  admitted_ = true;
  if (kind_ == SerializedStepKind::kToolPayload)
    handle_.tool_payload_step_ = true;
  else
    handle_.lifecycle_step_ = kind_;
}

RetainedRos2ControlRuntimeHandle::SerializedStep::~SerializedStep() {
  if (!admitted_) return;
  {
    std::lock_guard lock(handle_.mutex_);
    if (kind_ == SerializedStepKind::kToolPayload)
      handle_.tool_payload_step_ = false;
    else
      handle_.lifecycle_step_.reset();
  }
  handle_.serialized_step_released_.notify_all();
}

bool RetainedRos2ControlRuntimeHandle::HasToolContext() const {
  std::lock_guard lock(mutex_);
  return tool_context_hook_.has_value();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RebindRetainedToolContext(
    std::vector<RobotTcp> rows,
    std::vector<TcpFrameAssociation> retained_associations,
    std::optional<core::ProgramBindingId> direct_control_binding,
    RetainedToolContextSourceFn source, CommittedToolContextSourceFn committed_source,
    std::function<std::optional<core::Pose3DTO>(const std::string &)> fixed_pose,
    CommittedSceneSourceFn scene_source, CommittedToolContextSourceFn enable_source) {
  SerializedStep step(*this, SerializedStepKind::kArm);
  std::optional<RobotTcp> previous;
  std::optional<RobotTcp> selected;
  std::optional<core::TcpEntryReferenceDTO> previous_reference;
  std::optional<core::TcpEntryReferenceDTO> selected_reference;
  bool reset = false;
  bool unchanged_no_tcp = false;
  bool enabled = false;
  bool sync = false;
  std::optional<ToolContextHook> hook;
  std::optional<core::Pose3DTO> last_offset;
  {
    std::lock_guard lock(mutex_);
    if (!active_tcp_robot_)
      return core::Result<void>::Err({"retained_tool_context_invalid", "Retained robot has no Active TCP owner", {}});
    if (tool_context_hook_ && (!source || !committed_source || !fixed_pose))
      return core::Result<void>::Err({"retained_tool_context_invalid", "Candidate tool context source is incomplete", {}});
    previous = active_tcp_.value();
    previous_reference = active_tcp_.reference();
    ActiveTcp candidate(std::move(rows), retained_associations);
    if (candidate.reference() &&
        candidate.find(*candidate.reference()) == nullptr)
      return core::Result<void>::Err({"retained_tool_context_invalid",
                                      "End-effector TCP lacks retained association", {}});
    unchanged_no_tcp = !previous && !candidate.value();
    reset = !unchanged_no_tcp &&
        (!previous_reference || candidate.find(*previous_reference) == nullptr);
    if (!reset && previous_reference) candidate.commit(*previous_reference);
    selected = candidate.value();
    selected_reference = candidate.reference();
    active_tcp_ = std::move(candidate);
    active_tcp_direct_binding_ = std::move(direct_control_binding);
    if (source) tool_context_source_ = std::move(source);
    if (committed_source) committed_tool_context_source_ = std::move(committed_source);
    if (enable_source) committed_enable_tool_context_source_ = std::move(enable_source);
    if (scene_source) {
      committed_scene_source_ = std::move(scene_source);
    }
    if (reset) {
      active_tcp_registration_generation_ = next_active_tcp_registration();
      active_tcp_sequence_ = 0U;
    }
    enabled = arm_ && arm_->requested == core::ComponentLifecycleStateDTO::kActive;
    hook = tool_context_hook_;
    sync = hook.has_value() && active_tcp_offset_sync_;
    last_offset = last_tool_offset_;
  }
  bool offset_sent = false;
  if (enabled && sync && !unchanged_no_tcp) {
    const auto pose = selected && fixed_pose ? fixed_pose(selected->frame.compiled_frame_name)
                                             : std::optional<core::Pose3DTO>{};
    // No TCP: the identity offset (the flange).
    const std::optional<core::Pose3DTO> wanted =
        selected ? pose : std::optional<core::Pose3DTO>(core::Pose3DTO{});
    if (wanted && (reset || !last_offset || *last_offset != *wanted)) {
      // The published selection remains committed even if the send fails; an
      // enabled controller may keep its predecessor offset until Recover and
      // Enable (`tool_context_fault_`).
      const auto sent = SendPreparedToolOffset(*hook, *wanted);
      if (sent.ok()) {
        offset_sent = true;
      } else {
        std::lock_guard lock(mutex_);
        if (!tool_context_fault_) tool_context_fault_ = sent.error();
      }
    }
  }
  if (reset) {
    std::lock_guard lock(mutex_);
    ++active_tcp_sequence_;
    if (active_tcp_events_)
      active_tcp_events_(core::ActiveTcpChangeDTO{
          *active_tcp_robot_, previous_reference, selected_reference,
          core::ActiveTcpChangeSource::kReset, offset_sent,
          active_tcp_registration_generation_, active_tcp_sequence_, 0});
  }
  return core::Result<void>::Ok();
}

void RetainedRos2ControlRuntimeHandle::BindActiveTcpEvents(ActiveTcpEventSink sink) {
  std::lock_guard lock(mutex_);
  active_tcp_events_ = std::move(sink);
}

ActiveTcpReading RetainedRos2ControlRuntimeHandle::ReadActiveTcp() const {
  std::lock_guard lock(mutex_);
  return {active_tcp_robot_, active_tcp_registration_generation_, active_tcp_.value(),
          active_tcp_sequence_, active_tcp_.reference()};
}

ActiveTcp RetainedRos2ControlRuntimeHandle::ReadRobotTcps() const {
  std::lock_guard lock(mutex_);
  auto snapshot = active_tcp_;
  snapshot.set_read_provenance(active_tcp_registration_generation_, active_tcp_sequence_);
  return snapshot;
}

bool RetainedRos2ControlRuntimeHandle::ActiveTcpHoldsRobot() const {
  std::lock_guard lock(mutex_);
  return active_tcp_offset_sync_;
}

namespace {

/// A failed tool-context call that may still act (`details.effect`).
[[nodiscard]] bool tool_context_effect_remains(const core::ErrorDTO &error) {
  const auto effect = error.details.find("effect");
  return effect != error.details.end() &&
         (effect->second == "possible" || effect->second == "observed");
}

} // namespace

void RetainedRos2ControlRuntimeHandle::ReportToolContextFailure(const core::ErrorDTO &error) {
  // Sent and not confirmed: the controller may differ from the Active
  // TCP, a fault only Recover ends (then Enable re-syncs). Anything else is
  // the call's typed error alone.
  if (!tool_context_effect_remains(error)) return;
  std::lock_guard lock(mutex_);
  if (!tool_context_fault_) tool_context_fault_ = error;
}

core::Result<bool> RetainedRos2ControlRuntimeHandle::SendToolContextAtRest(
    const std::optional<std::string> &tcp_frame) {
  std::optional<ToolContextHook> hook;
  RetainedToolContextSourceFn source;
  {
    std::lock_guard lock(mutex_);
    // With the checkbox off a TCP change is ManyForge's selection only
    // (the controller keeps a zero offset, sent at Enable).
    if (!active_tcp_offset_sync_) return core::Result<bool>::Ok(false);
    hook = tool_context_hook_;
    source = tool_context_source_;
  }
  if (!hook || !source) return core::Result<bool>::Ok(false);
  core::Pose3DTO offset;  // No TCP: the flange
  if (tcp_frame) {
    const auto context = source(tcp_frame);
    if (!context.ok()) return core::Result<bool>::Err(context.error());
    offset = context.value().tcp_pose_in_flange;
  }
  return SendPreparedToolOffset(*hook, offset);
}

core::Result<bool> RetainedRos2ControlRuntimeHandle::SendPreparedToolOffset(
    const ToolContextHook &hook, const core::Pose3DTO &offset) {
  // Not a lifecycle step: each call the hook's own cap, no cancellation.
  const auto applied = hook.apply(offset, std::nullopt, false, std::nullopt, std::stop_token{});
  if (!applied.ok()) {
    // A sent call (or its re-arm) may still take effect: the driver then
    // cycles its controllers, so the next motion rebinds first.
    if (tool_context_effect_remains(applied.error())) {
      std::lock_guard lock(mutex_);
      rearm_rebind_required_ = true;
    }
    ReportToolContextFailure(applied.error());
    return applied;
  }
  bool rebind = applied.value();
  {
    std::lock_guard lock(mutex_);
    last_tool_offset_ = offset;
    rebind = rebind || rearm_rebind_required_;
  }
  // The hook re-armed the robot (xArm: a TCP offset send leaves the
  // controller's ready state); the driver reactivates its controllers. Bind
  // the reactivated controller before any motion goal.
  if (rebind) {
    const auto rebound = RebindAfterRearm(hook.rearm_bound);
    if (!rebound.ok()) {
      // The offset went out but the robot did not come back: the
      // controller differs from the Active TCP until Recover and Enable.
      if (applied.value()) {
        std::lock_guard lock(mutex_);
        if (!tool_context_fault_) tool_context_fault_ = rebound.error();
      }
      return core::Result<bool>::Err(rebound.error());
    }
  }
  return applied;
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ChangeActiveTcp(
    const std::optional<core::TcpEntryReferenceDTO> &entry,
    const std::optional<std::string> &tcp_frame,
    const core::ActiveTcpChangeSource source, const bool send,
    const bool send_failure_completes) {
  bool offset_sent = false;
  if (send) {
    const auto sent = SendToolContextAtRest(tcp_frame);
    if (sent.ok()) {
      offset_sent = sent.value();
    } else if (!send_failure_completes) {
      return core::Result<void>::Err(sent.error());
    }
  }
  // The commit and its event, under the handle's lock.
  std::lock_guard lock(mutex_);
  const auto previous = active_tcp_.reference();
  const bool changed = entry ? active_tcp_.commit(*entry) : active_tcp_.reset();
  if (!changed) return core::Result<void>::Ok();
  ++active_tcp_sequence_;
  if (!active_tcp_events_ || !active_tcp_robot_) return core::Result<void>::Ok();
  active_tcp_events_(core::ActiveTcpChangeDTO{
      *active_tcp_robot_, previous, active_tcp_.reference(), source,
      offset_sent, active_tcp_registration_generation_, active_tcp_sequence_, 0});
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::SetActiveTcp(
    const core::TcpEntryReferenceDTO &entry, const core::ActiveTcpChangeSource source) {
  auto permit = EnterTransportEffect();
  if (!permit.ok()) return core::Result<void>::Err(permit.error());
  // Serialized with the lifecycle steps (an Enable re-syncs this value).
  SerializedStep step(*this, SerializedStepKind::kArm);
  std::string frame;
  bool enabled = false;
  {
    std::lock_guard lock(mutex_);
    const auto *row = active_tcp_.find(entry);
    if (row == nullptr)
      return core::Result<void>::Err(core::active_tcp_unknown_error(
          active_tcp_robot_.value_or(core::ControllableComponentId("")), entry));
    if (active_tcp_.reference() && *active_tcp_.reference() == entry)
      return core::Result<void>::Ok();
    frame = row->frame.compiled_frame_name;
    // Not enabled, the controller takes it at the next Enable.
    enabled = arm_ && arm_->requested == core::ComponentLifecycleStateDTO::kActive;
  }
  return ChangeActiveTcp(entry, frame, source, enabled, false);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyMotionToolContext(
    const core::TcpEntryReferenceDTO &entry,
    const std::optional<core::ProgramBindingId> &program_binding) {
  {
    // A motion start never waits behind a lifecycle step (OD-69): an arm
    // step in flight refuses it; the move's TCP already the Active TCP needs
    // nothing more.
    std::lock_guard lock(mutex_);
    if (lifecycle_step_ == SerializedStepKind::kArm)
      return core::Result<void>::Err(lifecycle_step_in_flight_error());
    if (active_tcp_.reference() && *active_tcp_.reference() == entry)
      return core::Result<void>::Ok();
  }
  // A TCP step re-arms the robot, whose driver cycles its controllers: any
  // lifecycle step in flight refuses it.
  SerializedStep step(*this, SerializedStepKind::kArm, SerializedStepEntry::kMotionStart);
  if (!step.admitted()) return core::Result<void>::Err(lifecycle_step_in_flight_error());
  std::string frame;
  auto source = core::ActiveTcpChangeSource::kProgram;
  {
    std::lock_guard lock(mutex_);
    const auto *row = active_tcp_.find(entry);
    if (row == nullptr)
      return core::Result<void>::Err(core::active_tcp_unknown_error(
          active_tcp_robot_.value_or(core::ControllableComponentId("")), entry));
    // No event, no step: the move uses the Active TCP.
    if (active_tcp_.reference() && *active_tcp_.reference() == entry)
      return core::Result<void>::Ok();
    frame = row->frame.compiled_frame_name;
    if (program_binding && program_binding == active_tcp_direct_binding_)
      source = core::ActiveTcpChangeSource::kRobotControl;
  }
  // An admitted move: the robot is enabled and at rest.
  return ChangeActiveTcp(entry, frame, source, true, false);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ResetActiveTcp() {
  auto permit = EnterTransportEffect();
  if (!permit.ok()) return core::Result<void>::Err(permit.error());
  return ResetActiveTcpAdmitted();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ResetActiveTcpAdmitted() {
  SerializedStep step(*this, SerializedStepKind::kArm);
  std::optional<std::string> frame;
  bool enabled = false;
  {
    std::lock_guard lock(mutex_);
    const auto &rows = active_tcp_.rows();
    const auto end_effector = std::find_if(rows.begin(), rows.end(),
                                           [](const RobotTcp &row) { return row.end_effector; });
    const bool none = end_effector == rows.end();
    if (none ? !active_tcp_.value()
             : active_tcp_.reference() && *active_tcp_.reference() == TcpReference(*end_effector))
      return core::Result<void>::Ok();
    if (!none) frame = end_effector->frame.compiled_frame_name;
    enabled = arm_ && arm_->requested == core::ComponentLifecycleStateDTO::kActive;
  }
  // Told at once when it can be; otherwise the reset still
  // completes and the next Enable re-syncs.
  return ChangeActiveTcp(std::nullopt, frame, core::ActiveTcpChangeSource::kReset, enabled,
                         true);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyCommittedToolPayload(
    const CommittedSceneSourceFn &read) {
  SerializedStep step(*this, SerializedStepKind::kToolPayload);
  return ApplyCommittedToolPayloadClaimed(read);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyLatestCommittedToolPayload() {
  return ApplyLatestCommittedToolPayloadAs(SerializedStepEntry::kMotionStart);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::FollowLatestCommittedToolPayload() {
  return ApplyLatestCommittedToolPayloadAs(SerializedStepEntry::kWait);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyLatestCommittedToolPayloadAs(
    const SerializedStepEntry entry) {
  CommittedSceneSourceFn read;
  bool hook = false;
  {
    std::lock_guard lock(mutex_);
    if (!tool_context_hook_ && !committed_scene_source_)
      return core::Result<void>::Ok();
    read = committed_scene_source_;
    // Installed once, before start, and never removed.
    hook = tool_context_hook_.has_value();
  }
  // Without a hook nothing is sent (Mock, Isaac): the Scene checks alone,
  // no step to wait for.
  if (!hook) return ApplyCommittedToolPayloadClaimed(read);
  // A payload update: never behind a gripper switch; a motion start never
  // behind an arm lifecycle step (OD-69).
  SerializedStep step(*this, SerializedStepKind::kToolPayload, entry);
  if (!step.admitted()) return core::Result<void>::Err(lifecycle_step_in_flight_error());
  return ApplyCommittedToolPayloadClaimed(read);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyCommittedToolPayloadClaimed(
    const CommittedSceneSourceFn &read) {
  const auto fault = [this](core::ErrorDTO error) {
    std::lock_guard lock(mutex_);
    if (!tool_context_fault_) tool_context_fault_ = error;
    return core::Result<void>::Err(std::move(error));
  };
  const auto unavailable = [](const std::string &message) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)), message,
        {{"reason", "execution_port_not_ready"}}});
  };
  {
    std::lock_guard lock(mutex_);
    if (tool_context_fault_) return core::Result<void>::Err(*tool_context_fault_);
  }
  if (!read) return unavailable("Scene source is unavailable");
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> scene;
  try { scene = read(); }
  catch (const std::exception &error) { return unavailable(error.what()); }
  catch (...) { return unavailable("Committed Scene read failed"); }
  if (!scene || !scene->revision.valid())
    return unavailable("Committed Scene snapshot is unavailable or stale");
  std::optional<ToolContextHook> hook;
  CommittedToolContextSourceFn source;
  std::optional<std::string> frame;
  bool enabled = false;
  {
    std::lock_guard lock(mutex_);
    enabled = arm_ && arm_->requested == core::ComponentLifecycleStateDTO::kActive;
    hook = tool_context_hook_;
    source = committed_tool_context_source_;
    if (active_tcp_.value()) frame = active_tcp_.value()->frame.compiled_frame_name;
  }
  // No Scene was sent while disabled; Enable synchronizes it.
  if (!enabled || !hook) return core::Result<void>::Ok();
  core::Result<core::ToolContextDTO> context = [&]() {
    try { return source(frame, *scene); }
    catch (const std::exception &error) {
      return core::Result<core::ToolContextDTO>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kExecutionRejected)), error.what(),
          {{"reason", "tool_payload_composition_failed"}}});
    }
  }();
  if (!context.ok()) return fault(context.error());
  const core::ToolContextPointMassDTO load{context.value().payload_mass_kg,
                                           context.value().payload_center_of_mass_m};
  {
    std::lock_guard lock(mutex_);
    if (last_tool_load_ == load) return core::Result<void>::Ok();
  }
  const auto applied = hook->apply(std::nullopt, load, false, std::nullopt, std::stop_token{});
  if (!applied.ok()) {
    ReportToolContextFailure(applied.error());
    return fault(applied.error());
  }
  std::lock_guard lock(mutex_);
  last_tool_load_ = load;
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RebindPendingRearm() {
  std::optional<ToolContextHook> hook;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !rearm_rebind_required_ || !tool_context_hook_)
      return core::Result<void>::Ok();
    hook = tool_context_hook_;
  }
  // A motion start's re-arm rebind: any lifecycle step in flight refuses it
  // (OD-69).
  SerializedStep step(*this, SerializedStepKind::kArm, SerializedStepEntry::kMotionStart);
  if (!step.admitted()) return core::Result<void>::Err(lifecycle_step_in_flight_error());
  {
    std::lock_guard lock(mutex_);
    if (!rearm_rebind_required_) return core::Result<void>::Ok();
  }
  return RebindAfterRearm(hook->rearm_bound);
}

core::Result<void>
RetainedRos2ControlRuntimeHandle::RebindAfterRearm(const std::chrono::nanoseconds bound) {
  // Caller holds a `kArm` step. Required until the rebind completes.
  {
    std::lock_guard lock(mutex_);
    rearm_rebind_required_ = true;
  }
  // The vendor driver reactivates every controller itself within its bound;
  // the controller manager's own activity report says when.
  const auto deadline = std::chrono::steady_clock::now() + bound;
  while (true) {
    {
      std::lock_guard lock(mutex_);
      const auto &activity = arm_->activity;
      if (activity.controller_manager_present && activity.observed &&
          activity.hardware_active && activity.controller_active &&
          (!activity.broadcaster_loaded || activity.broadcaster_active))
        break;
    }
    if (std::chrono::steady_clock::now() >= deadline)
      return core::Result<void>::Err(switch_error(
          "ros2_control_reengage_controllers_inactive",
          "the driver did not reactivate the trajectory controller and the joint state "
          "broadcaster in time; Recover the robot"));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // The reactivated controller is a new binding epoch, bound as an Enable
  // binds it (no switch: the driver already activated it).
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  {
    std::unique_lock lock(mutex_);
    const auto rotated = RotateEpochLocked(lock, deadline);
    if (!rotated.ok()) return rotated;
    arm_->epoch_used = true;
    arm_->bound = true;
    arm_->active_seen_while_bound = true;
    rearm_rebind_required_ = false;
    PublishReadinessLocked();
    adapter = arm_->adapter;
  }
  while (!adapter->ActionServerIsReady()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return core::Result<void>::Err(switch_error(
          "ros2_control_reengage_action_server_unavailable",
          "the trajectory controller's action server was not discovered after the re-arm"));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return core::Result<void>::Ok();
}

void RetainedRos2ControlRuntimeHandle::InstallControllerProcessFaultSource(
    std::function<std::optional<std::string>()> source,
    std::function<core::Result<std::optional<std::string>>()> health_source) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || process_fault_source_ || !source) {
    throw std::logic_error(
        "ROS controller process fault source is installed once, before start");
  }
  process_fault_source_ = std::move(source);
  process_health_source_ = std::move(health_source);
}

void RetainedRos2ControlRuntimeHandle::InstallOwnerRecoveryHooks(
    std::function<void()> notify,
    std::function<bool(std::uint64_t, std::uint64_t)> qualified) {
  std::lock_guard lock(mutex_);
  if (owner_recovery_notify_ || owner_recovery_qualified_ || !notify || !qualified)
    throw std::logic_error("ROS owner recovery hooks must be installed exactly once");
  owner_recovery_notify_ = std::move(notify);
  owner_recovery_qualified_ = std::move(qualified);
}

void RetainedRos2ControlRuntimeHandle::InstallTransportEffectClaim(
    std::function<core::Result<std::shared_ptr<void>>()> claim) {
  std::lock_guard lock(mutex_);
  if (transport_effect_claim_ || !claim)
    throw std::logic_error("ROS transport effect claim must be installed exactly once");
  transport_effect_claim_ = std::move(claim);
}

void RetainedRos2ControlRuntimeHandle::InstallMeasuredSourceRecoveryHooks(
    std::function<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>()> snapshot,
    std::function<bool(const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &)> matches) {
  std::lock_guard lock(mutex_);
  if (measured_source_snapshot_ || measured_source_matches_ || !snapshot || !matches)
    throw std::logic_error("measured source recovery hooks must be installed exactly once");
  measured_source_snapshot_ = std::move(snapshot);
  measured_source_matches_ = std::move(matches);
}

void RetainedRos2ControlRuntimeHandle::InstallMeasuredSourceCommitHook(
    std::function<bool(const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &,
                       void (*)(void *) noexcept, void *)> commit) {
  std::lock_guard lock(mutex_);
  if (measured_source_commit_ || !commit)
    throw std::logic_error("measured source commit hook must be installed exactly once");
  measured_source_commit_ = std::move(commit);
}

void RetainedRos2ControlRuntimeHandle::InstallOwnerFaultWake(std::function<void()> wake) {
  std::lock_guard lock(mutex_);
  if (owner_fault_wake_ || !wake || start_attempted_)
    throw std::logic_error("owner fault wake must be installed once before backend start");
  owner_fault_wake_ = std::move(wake);
}

core::Result<std::shared_ptr<void>>
RetainedRos2ControlRuntimeHandle::EnterTransportEffect() const {
  std::function<core::Result<std::shared_ptr<void>>()> claim;
  {
    std::lock_guard lock(mutex_);
    claim = transport_effect_claim_;
  }
  return claim ? claim() : core::Result<std::shared_ptr<void>>::Ok(std::shared_ptr<void>{});
}

void RetainedRos2ControlRuntimeHandle::InstallTransportFactory(
    std::function<std::shared_ptr<ros2_control_execution::Ros2TransportResources>()> factory) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || transport_factory_ || !factory)
    throw std::logic_error("ROS transport factory must be installed once before start");
  transport_factory_ = std::move(factory);
}

void RetainedRos2ControlRuntimeHandle::InstallControllerLaunchSettle(
    std::function<bool()> settled, const std::chrono::steady_clock::time_point started_at,
    const std::chrono::steady_clock::duration span) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || launch_settled_ || !settled)
    throw std::logic_error("ROS controller launch settle source is installed once, before start");
  launch_settled_ = std::move(settled);
  launch_settle_span_ = span;
  launch_settle_bound_ = started_at + span;
}

void RetainedRos2ControlRuntimeHandle::ReplaceControllerLaunchSettle(
    std::function<bool()> settled, const std::chrono::steady_clock::time_point started_at) {
  std::lock_guard lock(mutex_);
  if (!launch_settled_ || !settled) return;
  launch_settled_ = std::move(settled);
  launch_settle_bound_ = started_at + launch_settle_span_;
}

std::optional<RetainedRos2ControlSwitchResult>
RetainedRos2ControlRuntimeHandle::WaitForControllerLaunch(const std::stop_token cancellation) const {
  std::function<bool()> settled;
  std::chrono::steady_clock::time_point bound;
  {
    std::lock_guard lock(mutex_);
    settled = launch_settled_;
    bound = launch_settle_bound_;
  }
  if (!settled) return std::nullopt;
  return wait_for_endpoints(
      cancellation, bound, [&]() -> std::optional<RetainedRos2ControlSwitchResult> {
        if (settled()) return std::nullopt;
        return RetainedRos2ControlSwitchResult{
            RetainedRos2ControlSwitchOutcome::kNotSent,
            core::ErrorDTO{"component_launch_not_settled",
                           "The component's launch did not finish starting its controllers in time",
                           {}}};
      });
}

bool RetainedRos2ControlRuntimeHandle::ControllerLaunchSettling() const {
  std::function<bool()> settled;
  std::function<core::Result<std::optional<std::string>>()> health;
  std::chrono::steady_clock::time_point bound;
  {
    std::lock_guard lock(mutex_);
    settled = launch_settled_;
    health = process_health_source_;
    bound = launch_settle_bound_;
  }
  if (!settled || std::chrono::steady_clock::now() >= bound || settled()) return false;
  if (health) {
    const auto exited = health();
    if (exited.ok() && exited.value().has_value()) return false;
  }
  return true;
}

void RetainedRos2ControlRuntimeHandle::ReplaceControllerProcessFaultSource(
    std::function<std::optional<std::string>()> source,
    std::function<core::Result<std::optional<std::string>>()> health_source) {
  std::lock_guard lock(mutex_);
  if (!process_fault_source_ || !source)
    throw std::logic_error("ROS controller process fault source replaces an installed one");
  process_fault_source_ = std::move(source);
  process_health_source_ = std::move(health_source);
  process_diagnostic_incident_id_.clear();
  process_diagnostic_snapshot_.reset();
}

bool RetainedRos2ControlRuntimeHandle::CacheControllerProcessIncidentDiagnostic(
    std::string incident_id,
    std::shared_ptr<const content_identity::CanonicalValue> snapshot) {
  if (incident_id.empty() || !snapshot)
    return false;
  std::lock_guard lock(mutex_);
  if (!process_fault_source_ || process_diagnostic_incident_id_ == incident_id)
    return false;
  process_diagnostic_incident_id_ = std::move(incident_id);
  process_diagnostic_snapshot_ = std::move(snapshot);
  return true;
}

bool RetainedRos2ControlRuntimeHandle::HasControllerProcessIncidentDiagnostic(
    const std::string &incident_id) const {
  std::lock_guard lock(mutex_);
  return process_diagnostic_snapshot_ &&
         process_diagnostic_incident_id_ == incident_id;
}

bool RetainedRos2ControlRuntimeHandle::HasControllerProcessIncidentDiagnostic()
    const {
  std::lock_guard lock(mutex_);
  return process_diagnostic_snapshot_ && !process_diagnostic_incident_id_.empty();
}

void RetainedRos2ControlRuntimeHandle::InstallMeasuredStateObserver(
    std::function<void(const sensor_msgs::msg::JointState &)> observer) {
  std::lock_guard lock(mutex_);
  if (!arm_ || start_attempted_ || sample_observer_ ||
      measured_state_observer_ || !observer ||
      !arm_->transport.measured_joint_state_lane.has_value()) {
    throw std::logic_error(
        "ROS arm measured-state observer is installed once, before start, "
        "on a measured lane");
  }
  measured_state_observer_ = std::move(observer);
}

void RetainedRos2ControlRuntimeHandle::InstallSampleObserver(
    std::function<void(const core::ExecutionControllerSampleDTO &)> observer) {
  std::lock_guard lock(mutex_);
  if (!arm_ || start_attempted_ || sample_observer_ ||
      measured_state_observer_ || !observer) {
    throw std::logic_error(
        "ROS arm sample observer is installed once, before start");
  }
  sample_observer_ = std::move(observer);
}

bool RetainedRos2ControlRuntimeHandle::OwnsBackend(
    const core::IExecutionBackend *backend,
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding)
    const {
  std::lock_guard lock(mutex_);
  return participant_ != nullptr && participant_.get() == backend &&
         motion_binding_.has_value() &&
         motion_binding_->value() == motion_binding.value() &&
         gripper_binding_.has_value() == gripper_binding.has_value() &&
         (!gripper_binding.has_value() ||
          gripper_binding_->value() == gripper_binding->value());
}

bool RetainedRos2ControlRuntimeHandle::OwnsGripper(
    const core::IGripperCommandBackend *gripper,
    const core::RuntimeParticipantBinding &gripper_binding) const {
  std::lock_guard lock(mutex_);
  return gripper_ != nullptr && gripper_.get() == gripper &&
         gripper_binding_.has_value() &&
         gripper_binding_->value() == gripper_binding.value();
}

bool RetainedRos2ControlRuntimeHandle::OwnsBinding(
    const core::RuntimeParticipantBinding &binding) const {
  std::lock_guard lock(mutex_);
  return (motion_binding_ && motion_binding_->value() == binding.value()) ||
         (gripper_binding_ && gripper_binding_->value() == binding.value());
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RebindMotionRoute(
    core::RuntimeParticipantBinding binding,
    std::vector<core::RuntimeParticipantBinding> aliases) {
  std::shared_ptr<core::ExecutionPortParticipant> participant;
  {
    std::lock_guard lock(mutex_);
    if (!participant_ || !motion_binding_)
      return reject<void>("ros2_control_motion_route_rebind_mismatch",
                          "ROS route publication has no physical arm owner");
    const auto difference = route_physical_owner_difference(*motion_binding_, binding);
    if (!difference.empty())
      return reject<void>("ros2_control_motion_route_rebind_mismatch",
                          "ROS route publication changed the physical arm owner: " +
                              std::string(difference));
    participant = participant_;
  }
  auto rebound = participant->RebindRoute(binding, std::move(aliases));
  if (!rebound.ok())
    return rebound;
  std::lock_guard lock(mutex_);
  motion_binding_ = std::move(binding);
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::BindGripperDisplayChannel(
    const std::shared_ptr<RetainedObservedStateChannel>& channel) {
  std::lock_guard lock(mutex_);
  if (!gripper_binding_ || !channel || channel->binding().value() != gripper_binding_->value())
    return reject<void>("ros2_control_gripper_display_binding_mismatch",
                        "Display channel must name the current exact gripper binding");
  gripper_display_channel_ = channel;
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::PublishGripperDisplayPosition(
    const double position) {
  std::shared_ptr<RetainedObservedStateChannel> channel;
  std::optional<core::RuntimeParticipantBinding> binding;
  {
    std::lock_guard lock(mutex_);
    channel = gripper_display_channel_.lock();
    binding = gripper_binding_;
  }
  if (!channel || !binding || binding->value().resources.size() != 1U)
    return reject<void>("ros2_control_gripper_display_source_unavailable",
                        "Display has no current exact scalar gripper channel");
  const std::array<std::string, 1U> names{binding->value().resources.front().compiled_resource_name};
  const std::array<double, 1U> positions{position};
  // Route validation is inside the channel: a concurrent rebind refuses this
  // old snapshot instead of attributing coordinates to a successor identity.
  return channel->update_render_positions(*binding, names, positions);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RebindGripperRoute(
    core::RuntimeParticipantBinding binding,
    std::shared_ptr<IRetainedObservedStateChannel> channel) {
  std::lock_guard lock(mutex_);
  if (!gripper_ || !gripper_binding_ || !channel)
    return reject<void>("ros2_control_gripper_route_rebind_mismatch",
                        "ROS route publication has no physical gripper owner");
  const auto difference = route_physical_owner_difference(*gripper_binding_, binding);
  if (!difference.empty())
    return reject<void>("ros2_control_gripper_route_rebind_mismatch",
                        "ROS route publication changed the physical gripper owner: " +
                            std::string(difference));
  const auto expected = core::make_observed_state_query_context(
      binding, channel->query_context().freshness_budget_ns);
  if (!expected.ok() || expected.value() != channel->query_context())
    return reject<void>(
        "ros2_control_gripper_route_rebind_mismatch",
        "ROS gripper route crosses its addressed state authority");
  if (!gripper_display_channel_.expired()) {
    auto display = std::dynamic_pointer_cast<RetainedObservedStateChannel>(channel);
    if (!display)
      return reject<void>("ros2_control_gripper_display_source_unavailable",
                          "Rebound display requires the actual retained channel");
    gripper_display_channel_ = std::move(display);
  }
  gripper_binding_ = std::move(binding);
  return core::Result<void>::Ok();
}

void RetainedRos2ControlRuntimeHandle::StartBackend() {
  std::lock_guard lock(mutex_);
  if (closing_) throw std::logic_error("ROS runtime handle is closing");
  if (start_failure_ != nullptr)
    std::rethrow_exception(start_failure_);
  if (start_attempted_)
    return;
  if ((!arm_ && !gripper_only_) || resources_ == nullptr)
    throw std::logic_error("ROS initial installation has no live backend");
  start_attempted_ = true;
  try {
    if (gripper_ && owner_fault_wake_)
      gripper_->set_retirement_wake(owner_fault_wake_);
    if (owner_fault_wake_) {
      const auto wake = owner_fault_wake_;
      const auto observer = [wake](std::string_view) noexcept {
        try { wake(); } catch (...) {}
      };
      if (!resources_->started()) resources_->set_executor_fault_observer(observer);
      if (arm_ && arm_->epoch_resources && !arm_->epoch_resources->started())
        arm_->epoch_resources->set_executor_fault_observer(observer);
    }
    if (gripper_)
      gripper_->complete_ros_configuration();
    // OD-66: measured state and gripper action callbacks run on the machine
    // owner's time-critical thread.
    if (!resources_->started()) resources_->enable_time_critical_callbacks();
    const auto node = resources_->node();
    if (gripper_only_) {
      if (!gripper_only_->physical_controller_required) {
        // No ROS command endpoint, controller observer or switch client exists
        // for the strict local none owner. Its existing node owns lifetime only.
        resources_->start();
        return;
      }
      const auto controller = gripper_only_->controller;
      const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak = weak_from_this();
      const auto endpoint = gripper_only_->activity_endpoints;
      rclcpp::SubscriptionOptions options;
      options.event_callbacks.matched_callback = [weak, endpoint](rclcpp::MatchedInfo& info) {
        endpoint->ObserveMatchedCount(info.current_count);
        endpoint->Refresh();
        if (const auto self = weak.lock(); self && info.current_count == 0) {
          self->ObserveActivity({}, RetainedRos2ControlArmActivity{});
        }
      };
      gripper_only_->activity_subscription = node->create_subscription<ActivityMessage>(
          controller.controller_manager + "/activity", rclcpp::QoS(1).reliable().transient_local(),
          [weak, controller](const ActivityMessage::ConstSharedPtr message) {
            const auto self = weak.lock();
            if (!self) return;
            RetainedRos2ControlArmActivity activity;
            activity.controller_manager_present = true;
            activity.observed = true;
            for (const auto& row : message->controllers) {
              if (row.name != controller.controller_name) continue;
              activity.controller_loaded =
                  row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
                  row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
              activity.controller_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
            }
            for (const auto& row : message->hardware_components)
              if (row.name == controller.hardware_component_name)
                activity.hardware_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
            self->ObserveActivity({}, activity);
          }, options);
      gripper_only_->switch_client = node->create_client<SwitchService>(
          controller.controller_manager + "/switch_controller");
      resources_->start();
      return;
    }
    const auto &controller = arm_->transport.controller;
    const auto gripper_controller = arm_->transport.gripper_controller;
    const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak =
        weak_from_this();
    arm_->activity_endpoints =
        std::make_shared<ros2_control_execution::SelectedRosEndpointFacts>(
            node, controller.controller_manager + "/activity");
    const auto endpoint_facts = arm_->activity_endpoints;
    const auto reset_target = arm_->transport.mock_reset_position;
    if (reset_target)
      arm_->reset_publisher =
          node->create_publisher<std_msgs::msg::Float64MultiArray>(
              reset_target->commands_topic,
              rclcpp::QoS(1).reliable().durability_volatile());
    if (reset_target && reset_target->command_echo_topic) {
      // The hardware publishes its commands every cycle; a message without
      // every target joint is another robot's.
      arm_->reset_echo_subscription =
          node->create_subscription<sensor_msgs::msg::JointState>(
              *reset_target->command_echo_topic, rclcpp::SensorDataQoS().keep_last(1),
              [weak, joints = reset_target->joint_names](
                  const sensor_msgs::msg::JointState::ConstSharedPtr message) {
                std::vector<double> positions;
                positions.reserve(joints.size());
                for (const auto &joint : joints) {
                  const auto found =
                      std::find(message->name.begin(), message->name.end(), joint);
                  const auto index =
                      static_cast<std::size_t>(found - message->name.begin());
                  if (found == message->name.end() || index >= message->position.size())
                    return;
                  positions.push_back(message->position[index]);
                }
                const auto self = weak.lock();
                if (!self)
                  return;
                {
                  std::lock_guard lock(self->mutex_);
                  const auto sequence =
                      self->arm_->reset_echo ? self->arm_->reset_echo->sequence + 1U : 1U;
                  self->arm_->reset_echo = ResetCommandEcho{
                      std::move(positions), self->clock_->now(), sequence};
                }
                self->reset_observation_wake_->notify();
              });
    }
    rclcpp::SubscriptionOptions options;
    // The controller manager appearing or disappearing changes what its
    // (transient-local) activity sample can still claim.
    options.event_callbacks.matched_callback =
        [weak, endpoint_facts](rclcpp::MatchedInfo &info) {
          endpoint_facts->ObserveMatchedCount(info.current_count);
          endpoint_facts->Refresh();
          if (const auto self = weak.lock()) {
            auto activity = self->arm_activity();
            activity.controller_manager_present = info.current_count > 0;
            if (!activity.controller_manager_present) {
              activity.observed = false;
              activity.controller_loaded = activity.controller_active =
                  activity.hardware_active = activity.broadcaster_loaded =
                      activity.broadcaster_active = false;
            }
            self->ObserveActivity(activity);
          }
        };
    arm_->activity_subscription = node->create_subscription<ActivityMessage>(
        controller.controller_manager + "/activity",
        rclcpp::QoS(1).reliable().transient_local(),
        [weak, controller, reset_target,
         gripper_controller](const ActivityMessage::ConstSharedPtr message) {
          const auto self = weak.lock();
          if (!self)
            return;
          RetainedRos2ControlArmActivity activity;
          activity.controller_manager_present = true;
          activity.observed = true;
          for (const auto &row : message->controllers) {
            if (row.name == kJointStateBroadcasterControllerName) {
              activity.broadcaster_loaded =
                  row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
                  row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
              activity.broadcaster_active =
                  row.state.id ==
                  lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
            }
            if (row.name != controller.controller_name)
              continue;
            activity.controller_loaded =
                row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
                row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
            activity.controller_active =
                row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
          }
          for (const auto &row : message->hardware_components) {
            if (row.name == controller.hardware_component_name) {
              activity.hardware_active =
                  row.state.id ==
                  lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
            }
          }

          std::optional<RetainedRos2ControlArmActivity> gripper_activity;
          if (gripper_controller) {
            gripper_activity = activity;
            gripper_activity->controller_loaded = false;
            gripper_activity->controller_active = false;
            for (const auto &row : message->controllers) {
              if (row.name == gripper_controller->controller_name) {
                gripper_activity->controller_loaded =
                    row.state.id ==
                        lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
                    row.state.id ==
                        lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
                gripper_activity->controller_active =
                    row.state.id ==
                    lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
              }
            }
          }
          self->ObserveActivity(activity, gripper_activity);
          if (reset_target) {
            std::lock_guard lock(self->mutex_);
            self->arm_->reset_writer_loaded = false;
            self->arm_->reset_writer_active = false;
            for (const auto &row : message->controllers) {
              if (row.name != reset_target->controller_name)
                continue;
              self->arm_->reset_writer_loaded =
                  row.state.id ==
                      lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE ||
                  row.state.id ==
                      lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
              self->arm_->reset_writer_active =
                  row.state.id ==
                  lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
            }
            self->reset_observation_wake_->notify();
          }
        },
        options);
    arm_->switch_client = node->create_client<SwitchService>(
        controller.controller_manager + "/switch_controller");
    arm_->hardware_state_client = node->create_client<HardwareStateService>(
        controller.controller_manager + "/set_hardware_component_state");
    // One subscription to the controller manager's statistics;
    // observed values only, a throttled warning, no execution authority.
    arm_->statistics_monitor = std::make_shared<RetainedControllerStatisticsMonitor>(
        RetainedControllerStatisticsConfig{
            controller.controller_manager, controller.controller_name,
            arm_->transport.adapter.lane_facts.controller_update_rate_hz},
        clock_owner_, [logger = node->get_logger()](const std::string &text) {
          RCLCPP_WARN(logger, "%s", text.c_str());
        });
    arm_->statistics_subscription =
        RetainedControllerStatisticsMonitor::Subscribe(node, arm_->statistics_monitor);
    if (measured_state_observer_) {
      // The observer runs on this owner's time-critical thread (OD-66); it
      // holds only the channel and sink it publishes to, never this handle.
      // The subscription is released only after the owner has joined.
      rclcpp::SubscriptionOptions measured_options;
      measured_options.callback_group = resources_->create_time_critical_callback_group();
      measured_state_subscription_ =
          node->create_subscription<sensor_msgs::msg::JointState>(
              arm_->transport.measured_joint_state_lane->absolute_topic,
              rclcpp::SensorDataQoS().keep_last(1),
              [observer = measured_state_observer_](
                  const sensor_msgs::msg::JointState::ConstSharedPtr message) {
                observer(*message);
              }, measured_options);
    }
    if (sample_observer_ || measured_state_observer_) {
      const auto rate = arm_->transport.adapter.lane_facts.state_rate_hz;
      if (!(rate > 0.0))
        throw std::invalid_argument("ROS arm lane has no controller state rate");
      sample_timer_ = node->create_wall_timer(
          std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / rate)),
          [weak,
           reset_wake = reset_target ? reset_observation_wake_ : nullptr]() {
            const auto self = weak.lock();
            if (!self)
              return;
            // The existing observer timer also runs while no state sample is
            // present. It refreshes the endpoint publisher counts (including
            // unmatched QoS endpoints) without a separate poller or a graph
            // call in Core.
            self->RefreshEndpointFacts();
            std::shared_ptr<ArmPort> port;
            std::function<void(const core::ExecutionControllerSampleDTO &)>
                observer;
            {
              std::lock_guard lock(self->mutex_);
              if (!self->arm_)
                return;
              port = self->arm_->port;
              observer = self->sample_observer_;
            }
            // A measured arm keeps this timer for the endpoint facts only.
            if (!observer)
              return;
            const auto sample = port->adapter().ReadSample();
            if (!sample)
              return;
            const std::pair<std::uint64_t, std::uint64_t> identity{
                sample->binding_epoch.value(),
                sample->local_receipt_count.value()};
            {
              std::lock_guard lock(self->mutex_);
              if (self->last_observed_sample_ == identity)
                return;
              self->last_observed_sample_ = identity;
            }
            observer(*sample);
            if (reset_wake)
              reset_wake->notify();
          });
    }
    arm_->epoch_resources->start();
    resources_->start();
  } catch (...) {
    start_failure_ = std::current_exception();
    throw;
  }
}

void RetainedRos2ControlRuntimeHandle::RefreshEndpointFacts() {
  std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ && !gripper_only_) return;
    activity = arm_ ? arm_->activity_endpoints : gripper_only_->activity_endpoints;
    adapter = arm_ ? arm_->adapter : nullptr;
  }
  // No owner/command lock spans SDK graph reads.
  if (activity)
    activity->Refresh();
  if (adapter)
    adapter->RefreshEndpointFacts();
}

std::optional<core::ErrorDTO>
RetainedRos2ControlRuntimeHandle::ReadEndpointDiagnostic() const {
  std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  {
    std::lock_guard lock(mutex_);
    if ((!arm_ && !gripper_only_) || !start_attempted_) return std::nullopt;
    activity = arm_ ? arm_->activity_endpoints : gripper_only_->activity_endpoints;
    adapter = arm_ ? arm_->adapter : nullptr;
  }
  // Before StartBackend there is no installed CM observation checkpoint.
  // Preserve the existing unstarted-lane classification.
  if (!activity)
    return std::nullopt;
  const auto activity_error = activity->ReadError();
  const auto state_error = adapter ? adapter->ReadEndpointDiagnostic() : std::nullopt;
  // A positive conflict takes precedence over an unrelated unsettled stream.
  if (activity_error && activity_error->code == "duplicate_component_instance")
    return activity_error;
  if (state_error && state_error->code == "duplicate_component_instance")
    return state_error;
  return activity_error ? activity_error : state_error;
}

RetainedRos2ControlArmActivity
RetainedRos2ControlRuntimeHandle::arm_activity() const {
  std::lock_guard lock(mutex_);
  return arm_ ? arm_->activity : RetainedRos2ControlArmActivity{};
}

void RetainedRos2ControlRuntimeHandle::ObserveActivity(
    RetainedRos2ControlArmActivity activity,
    std::optional<RetainedRos2ControlArmActivity> gripper_activity) {
  std::lock_guard lock(mutex_);
  if (!arm_) {
    if (gripper_only_ && gripper_activity) gripper_activity_ = *gripper_activity;
    return;
  }
  arm_->activity = activity;
  if (gripper_activity)
    gripper_activity_ = *gripper_activity;
  else if (!activity.controller_manager_present)
    gripper_activity_ = {};
  if (arm_->bound && activity.controller_manager_present &&
      activity.controller_active) {
    arm_->active_seen_while_bound = true;
  } else if (arm_->bound && arm_->active_seen_while_bound) {
    // Deactivated (or lost) after this host's activation: the binding ended;
    // only a new component enable, on a new epoch, binds the controller again.
    arm_->bound = false;
    arm_->active_seen_while_bound = false;
  }
  PublishReadinessLocked();
  if (arm_->transport.mock_reset_position)
    reset_observation_wake_->notify();
}

void RetainedRos2ControlRuntimeHandle::PublishReadinessLocked() {
  const auto &activity = arm_->activity;
  core::ExecutionReadinessDTO readiness;
  readiness.binding_epoch = core::ExecutionBindingEpoch(arm_->epoch);
  readiness.controller_active =
      activity.controller_manager_present && activity.controller_active;
  // Physical binding readiness also enables JTC feedback reception. The
  // accepted Kernel Reset claim and ArmPort's pending gate separately
  // prevent motion until exact post-effect readback has completed.
  readiness.controller_bound = arm_->bound;
  readiness.hardware_active =
      activity.controller_manager_present && activity.hardware_active;
  arm_->adapter->ObserveReadiness(readiness);
}

void RetainedRos2ControlRuntimeHandle::RetainLateCancelObligationsLocked(
    std::vector<ros2_control_execution::Ros2ControlExecutionBackend::UnresolvedLateCancel> goals) {
  for (auto &goal : goals) {
    const auto found = std::find_if(pending_late_cancel_handoff_.begin(), pending_late_cancel_handoff_.end(),
        [&](const auto &prior) {
          return prior.binding_epoch == goal.binding_epoch && prior.key == goal.key &&
                 prior.execution_id == goal.execution_id;
        });
    if (found == pending_late_cancel_handoff_.end())
      pending_late_cancel_handoff_.push_back(std::move(goal));
  }
}

void RetainedRos2ControlRuntimeHandle::InstallLateCancelJournalHook(
    const void *engine_identity, std::function<core::Result<void>(std::uint64_t)> hook) {
  if (!engine_identity || !hook) throw std::invalid_argument("late-cancel journal hook is required");
  std::lock_guard lock(mutex_);
  if (late_cancel_journal_hook_) {
    if (late_cancel_engine_identity_ == engine_identity) return;
    throw std::logic_error("late-cancel journal engine changed");
  }
  late_cancel_engine_identity_ = engine_identity;
  late_cancel_journal_hook_ = std::move(hook);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::InstallGripperLateCancelJournalHook(
    const void *engine_identity,
    ros2_control_execution::Ros2GripperCommandBackend::LateCancelJournalHook hook) {
  ros2_control_execution::Ros2GripperCommandBackend *gripper = nullptr;
  std::optional<std::string> id;
  {
    std::lock_guard lock(mutex_);
    gripper = gripper_.get();
    id = gripper_id_;
  }
  if (!gripper || !id)
    return reject<void>("gripper_journal_owner_missing", "Gripper journal owner is unavailable");
  return gripper->InstallGripperLateCancelJournalHook(*id, engine_identity, std::move(hook));
}

core::Result<void> RetainedRos2ControlRuntimeHandle::FlushLateCancelHandoff() {
  decltype(pending_late_cancel_handoff_) pending;
  decltype(late_cancel_journal_hook_) hook;
  {
    std::lock_guard lock(mutex_);
    pending = pending_late_cancel_handoff_;
    hook = late_cancel_journal_hook_;
  }
  if (pending.empty()) return core::Result<void>::Ok();
  if (!hook) return reject<void>("late_cancel_journal_unavailable",
                                 "Late-cancel execution journal is unavailable");
  for (const auto &row : pending) {
    if (!row.execution_id || *row.execution_id == 0U)
      return reject<void>("late_cancel_execution_identity_missing",
                          "Late-cancel goal has no owning execution identity");
    const auto recorded = hook(*row.execution_id);
    if (!recorded.ok()) return recorded;
    std::lock_guard lock(mutex_);
    std::erase_if(pending_late_cancel_handoff_, [&](const auto &current) {
      return current.binding_epoch == row.binding_epoch && current.key == row.key &&
             current.execution_id == row.execution_id;
    });
  }
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RotateEpochLocked(
    std::unique_lock<std::mutex> &lock,
    const std::chrono::steady_clock::time_point deadline) {
  if (closing_)
    return reject<void>("ros2_control_handle_closing", "ROS runtime handle is closing");
  if (arm_->epoch_used) {
    if (arm_->epoch == std::numeric_limits<std::uint64_t>::max()) {
      return reject<void>("ros2_control_binding_epoch_exhausted",
                          "ROS controller binding epoch is exhausted");
    }
    // The caller's `kArm` step serializes this lifecycle operation. Pin the exact old
    // epoch while its bounded cancel drain and executor join run without the
    // handle facts mutex, which callbacks and State readers may need.
    const auto old_adapter = arm_->adapter;
    const auto old_epoch = arm_->epoch_resources;
    const auto old_machine = resources_;
    const auto old_epoch_number = arm_->epoch;
    lock.unlock();
    const bool leaf_drained = old_adapter->close_admission(deadline);
    auto unresolved = old_adapter->ReadUnresolvedLateCancels();
    if (!leaf_drained) {
      lock.lock();
      RetainLateCancelObligationsLocked(std::move(unresolved));
      lock.unlock();
      const auto handed_off = FlushLateCancelHandoff();
      lock.lock();
      if (!handed_off.ok()) return handed_off;
      return reject<void>("component_owner_stalled",
                          "ROS controller action leaf did not drain before its lifecycle deadline");
    }
    const bool joined = old_epoch->quiesce_until(deadline);
    lock.lock();
    RetainLateCancelObligationsLocked(std::move(unresolved));
    if (!joined)
      return reject<void>("ros2_control_owner_join_timeout",
                          "ROS controller owner did not join before its lifecycle deadline");
    // The caller's `kArm` step excludes every other epoch change: the one
    // identity check is the one before install, below.
    lock.unlock();
    old_adapter->release_ros_entities();
    const auto handed_off = FlushLateCancelHandoff();
    lock.lock();
    if (!handed_off.ok()) return handed_off;
    // Build the next epoch (node, backend entities, owner start, port)
    // without the handle facts mutex, which machine-owner callbacks and State
    // readers take; install it below only if the captured epoch is current.
    const auto old_arm = arm_;
    auto config = arm_->transport.adapter;
    config.binding_epoch = core::ExecutionBindingEpoch(arm_->epoch + 1U);
    const auto component_id = arm_->component_id;
    const auto compiled_joint_names = arm_->transport.compiled_joint_names;
    const auto lane_joint_names = arm_->transport.adapter.lane_facts.joint_names;
    const auto gate = arm_->gate;
    const auto reset_motion_admitted = arm_->reset_motion_admitted;
    const auto clock_owner = clock_owner_;
    auto *const scheduler = scheduler_;
    const auto fault_wake = owner_fault_wake_;
    lock.unlock();
    auto epoch_resources = make_jtc_epoch_resources(*old_machine, component_id);
    auto adapter =
        std::make_shared<ros2_control_execution::Ros2ControlExecutionBackend>(
            epoch_resources, clock_owner, *scheduler, std::move(config));
    if (fault_wake) {
      epoch_resources->set_executor_fault_observer([fault_wake](std::string_view) noexcept {
        try { fault_wake(); } catch (...) {}
      });
    }
    if (old_machine->started()) epoch_resources->start();
    auto port = std::make_shared<ArmPort>(
        adapter, compiled_joint_names, lane_joint_names, gate, reset_motion_admitted);
    lock.lock();
    if (closing_ || arm_ != old_arm || arm_->adapter != old_adapter ||
        arm_->epoch_resources != old_epoch || resources_ != old_machine ||
        arm_->epoch != old_epoch_number) {
      // Lost race: retire the unpublished epoch through the normal close path.
      lock.unlock();
      port.reset();
      adapter->close_admission_without_cancel();
      const bool built_joined = epoch_resources->quiesce_until(deadline);
      if (built_joined) adapter->release_ros_entities();
      lock.lock();
      if (!built_joined)
        return reject<void>("ros2_control_owner_join_timeout",
                            "ROS controller owner did not join before its lifecycle deadline");
      return reject<void>("ros2_control_binding_epoch_changed",
                          "ROS controller binding changed during epoch construction");
    }
    auto published_owners = std::make_shared<const TransportOwners>(
        TransportOwners{resources_, epoch_resources});
    ++arm_->epoch;
    arm_->epoch_resources = std::move(epoch_resources);
    arm_->adapter = std::move(adapter);
    arm_->activation_client_discovered = false;
    arm_->port = std::move(port);
    arm_->epoch_used = false;
    arm_->bound = false;
    arm_->rebind_pending = true;
    published_transport_owners_.store(std::move(published_owners), std::memory_order_release);
    PublishReadinessLocked();
  }
  if (arm_->rebind_pending) {
    // Dispatch and speed values go only through the new port from here on;
    // the participant drops every fact read from the old one. The caller's
    // `kArm` step serializes this; the facts mutex is not held across it.
    const auto port = arm_->port;
    const auto participant = participant_;
    lock.unlock();
    const auto rebound = participant->RebindPort(port);
    lock.lock();
    if (!rebound.ok())
      return rebound;
    arm_->rebind_pending = false;
  }
  return core::Result<void>::Ok();
}

RetainedRos2ControlSwitchResult RetainedRos2ControlRuntimeHandle::SendSwitch(
    const bool activate, const std::stop_token cancellation,
    const std::optional<std::string> selected_controller,
    const std::optional<MockResetPositionStepDeadline> lifecycle_deadline) {
  std::string controller;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ && !gripper_only_)
      return not_sent("ros2_control_switch_unavailable",
                      "the controller-manager switch client is not started");
    controller = selected_controller.value_or(
        arm_ ? arm_->transport.controller.controller_name
             : gripper_only_->controller.controller_name);
  }
  const auto activate_list = activate ? std::vector<std::string>{controller}
                                      : std::vector<std::string>{};
  const auto deactivate_list = activate ? std::vector<std::string>{}
                                        : std::vector<std::string>{controller};
  return SendControllerSwitch(activate_list, deactivate_list, cancellation, std::nullopt,
                              lifecycle_deadline);
}

std::optional<RetainedRos2ControlSwitchResult>
RetainedRos2ControlRuntimeHandle::WaitForReportedActivity(
    const std::vector<std::string> &activate, const std::vector<std::string> &deactivate,
    const std::optional<bool> hardware_active, const std::stop_token &cancellation,
    const std::chrono::steady_clock::time_point deadline) {
  // Whether the latest report shows `name` active: nullopt while no report
  // shows it (none yet, or a controller this handle doesn't observe).
  const auto reported_active = [&](const std::string &name) -> std::optional<bool> {
    const auto shown = [](const RetainedRos2ControlArmActivity &activity,
                          const bool active) -> std::optional<bool> {
      if (!activity.controller_manager_present || !activity.observed) return std::nullopt;
      return active;
    };
    if (arm_) {
      const auto &activity = arm_->activity;
      if (name == arm_->transport.controller.controller_name)
        return shown(activity, activity.controller_active);
      if (name == kJointStateBroadcasterControllerName)
        return shown(activity, activity.broadcaster_active);
      if (arm_->transport.gripper_controller &&
          name == arm_->transport.gripper_controller->controller_name)
        return shown(gripper_activity_, gripper_activity_.controller_active);
      if (arm_->transport.mock_reset_position &&
          name == arm_->transport.mock_reset_position->controller_name)
        return shown(activity, arm_->reset_writer_active);
    } else if (gripper_only_ && name == gripper_only_->controller.controller_name) {
      return shown(gripper_activity_, gripper_activity_.controller_active);
    }
    return std::nullopt;
  };
  const auto observed_here = [&](const std::string &name) {
    if (arm_)
      return name == arm_->transport.controller.controller_name ||
             name == kJointStateBroadcasterControllerName ||
             (arm_->transport.gripper_controller &&
              name == arm_->transport.gripper_controller->controller_name) ||
             (arm_->transport.mock_reset_position &&
              name == arm_->transport.mock_reset_position->controller_name);
    return gripper_only_ && name == gripper_only_->controller.controller_name;
  };
  const auto not_observed = [](const std::string &name) {
    return RetainedRos2ControlSwitchResult{
        RetainedRos2ControlSwitchOutcome::kFailed,
        core::ErrorDTO{std::string(core::error_codes::kComponentActionResultNotObserved),
                       "the controller manager's activity report didn't show " + name +
                           " in the requested state",
                       {{"reason", "activity_not_reported"},
                        {"controller_name", bounded_switch_detail(name)}}}};
  };
  return wait_for_endpoints(
      cancellation, deadline, [&]() -> std::optional<RetainedRos2ControlSwitchResult> {
        std::lock_guard lock(mutex_);
        for (const auto &name : activate)
          if (observed_here(name) && reported_active(name) != true) return not_observed(name);
        for (const auto &name : deactivate)
          if (observed_here(name) && reported_active(name) != false) return not_observed(name);
        if (hardware_active && arm_) {
          const auto &activity = arm_->activity;
          if (!activity.controller_manager_present || !activity.observed ||
              activity.hardware_active != *hardware_active)
            return not_observed(arm_->transport.controller.hardware_component_name);
        }
        return std::nullopt;
      });
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::SendControllerSwitch(
    std::vector<std::string> activate_controllers,
    std::vector<std::string> deactivate_controllers,
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> response_deadline,
    const std::optional<MockResetPositionStepDeadline> lifecycle_deadline,
    const bool best_effort) {
  rclcpp::Client<SwitchService>::SharedPtr client;
  rclcpp::Node::SharedPtr node;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner;
  const bool activate = !activate_controllers.empty();
  const auto controller =
      activate ? activate_controllers.front() : deactivate_controllers.front();
  {
    std::lock_guard lock(mutex_);
    if ((!arm_ || !arm_->switch_client) && (!gripper_only_ || !gripper_only_->switch_client))
      return not_sent("ros2_control_switch_unavailable",
                      "the controller-manager switch client is not started");
    client = arm_ ? arm_->switch_client : gripper_only_->switch_client;
    owner = resources_;
    node = owner->node();
  }
  // A lifecycle step waits, within its own command time, for this client to
  // discover the service (discovery lags the activity topic that Prepare
  // waited for); without a step deadline an undiscovered service refuses.
  if (lifecycle_deadline)
    while (!client->service_is_ready() && !cancellation.stop_requested() &&
           std::chrono::steady_clock::now() < *lifecycle_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!client->service_is_ready())
    return not_sent("ros2_control_switch_service_unavailable",
                    "the controller manager's switch_controller service is not "
                    "available");
  auto request = std::make_shared<SwitchService::Request>();
  request->activate_controllers = activate_controllers;
  request->deactivate_controllers = deactivate_controllers;
  request->strictness = best_effort ? SwitchService::Request::BEST_EFFORT
                                    : SwitchService::Request::STRICT;
  request->activate_asap = false;
  if (response_deadline &&
      std::chrono::steady_clock::now() >= *response_deadline)
    return not_sent("ros2_control_switch_timeout",
                    "The controller switch step deadline elapsed before dispatch");
  const auto started = std::chrono::steady_clock::now();
  const auto logger = node->get_logger();
  std::optional<decltype(client->async_send_request(request))> pending;
  if (!owner->try_sdk_send([&] { pending.emplace(client->async_send_request(request)); }))
    return not_sent("ros2_control_switch_unavailable", "The ROS owner closed before switch dispatch");
  auto future = std::move(*pending);
  while (future.wait_for(std::chrono::milliseconds(10)) !=
         std::future_status::ready) {
    if (cancellation.stop_requested()) {
      client->remove_pending_request(future);
      log_call(logger, "switch_controller", "cancelled", started);
      return {RetainedRos2ControlSwitchOutcome::kUncertain,
              switch_error("ros2_control_switch_cancelled",
                           "the controller switch was cancelled before its "
                           "answer; its effect is unknown")};
    }
    if (response_deadline &&
        std::chrono::steady_clock::now() >= *response_deadline) {
      client->remove_pending_request(future);
      log_call(logger, "switch_controller", "timeout", started);
      return {RetainedRos2ControlSwitchOutcome::kUncertain,
              switch_error("ros2_control_switch_timeout",
                           "The controller switch response exceeded the stock "
                           "CM 1 s fault bound; its effect is unknown")};
    }
    // A response that is ready is still accepted below (the lifecycle
    // callers' existing semantics); only a missing one ends here.
    if (lifecycle_deadline &&
        std::chrono::steady_clock::now() >= *lifecycle_deadline) {
      client->remove_pending_request(future);
      log_call(logger, "switch_controller", "timeout", started);
      return {RetainedRos2ControlSwitchOutcome::kUncertain,
              switch_error("ros2_control_switch_timeout",
                           "the controller manager did not answer the "
                           "controller switch within the lifecycle command "
                           "time; its effect is unknown")};
    }
  }
  const auto response = future.get();
  log_call(logger, "switch_controller", "answered", started);
  // A response can become ready during the final wait after its deadline.
  // The optional Reset bound still applies before accepting that response;
  // ordinary lifecycle callers retain their existing cancellation semantics.
  if (response_deadline && cancellation.stop_requested()) {
    return {RetainedRos2ControlSwitchOutcome::kUncertain,
            switch_error("ros2_control_switch_cancelled",
                         "the controller switch was cancelled before its "
                         "answer; its effect is unknown")};
  }
  if (response_deadline &&
      std::chrono::steady_clock::now() >= *response_deadline) {
    return {RetainedRos2ControlSwitchOutcome::kUncertain,
            switch_error("ros2_control_switch_timeout",
                         "The controller switch response exceeded the stock "
                         "CM 1 s fault bound; its effect is unknown")};
  }
  if (!response || !response->ok) {
    auto error = switch_error(activate ? "ros2_control_activation_refused"
                                      : "ros2_control_deactivation_refused",
                             "the controller manager refused the controller switch");
    if (response) {
      const auto action = bounded_switch_detail(activate ? "activate" : "deactivate");
      const auto name = bounded_switch_detail(controller);
      const auto service = bounded_switch_detail(client->get_service_name());
      const auto response_message = bounded_switch_detail(response->message);
      error.details.emplace("switch_action", action);
      error.details.emplace("controller_name", name);
      error.details.emplace("controller_service", service);
      if (!response_message.empty())
        error.details.emplace("controller_response_message", response_message);
      // One actual refused response, one bounded warning. Cached health reads
      // and missing responses do not emit or manufacture controller text.
      RCLCPP_WARN(node->get_logger(),
                  "Controller-manager switch refused: action=%s controller=%s service=%s response=%s",
                  action.c_str(), name.c_str(), service.c_str(), response_message.c_str());
    }
    return {RetainedRos2ControlSwitchOutcome::kFailed, std::move(error)};
  }
  // The switch's result is the reported activity.
  if (auto unseen = WaitForReportedActivity(
          activate_controllers, deactivate_controllers, std::nullopt, cancellation,
          response_deadline    ? *response_deadline
          : lifecycle_deadline ? *lifecycle_deadline
                               : allowed_until(std::chrono::steady_clock::now(), std::nullopt)))
    return *unseen;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult RetainedRos2ControlRuntimeHandle::PrepareArmController(
    const std::stop_token cancellation, const MockResetPositionStepDeadline deadline) {
  SerializedStep step(*this, SerializedStepKind::kArm);
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
  }
  if (auto waited = wait_for_endpoints(
          cancellation, deadline, [&]() -> std::optional<RetainedRos2ControlSwitchResult> {
            RefreshEndpointFacts();
            if (auto error = ReadEndpointDiagnostic())
              return RetainedRos2ControlSwitchResult{RetainedRos2ControlSwitchOutcome::kNotSent,
                                                     std::move(error)};
            std::lock_guard lock(mutex_);
            const auto &activity = arm_->activity;
            if (!activity.controller_manager_present || !activity.observed ||
                !activity.controller_loaded)
              return not_sent("ros2_control_controller_not_loaded",
                              "the trajectory controller is not loaded");
            return std::nullopt;
          }))
    return *waited;
  {
    std::lock_guard lock(mutex_);
    const auto &activity = arm_->activity;
    // Enabled by this host: Prepare doesn't stop it; Disable does.
    if (activity.controller_active && arm_->bound)
      return component_enabled();
    if (activity.controller_active)
      return not_sent("ros2_control_controller_active_unrequested",
                      "the trajectory controller is active without an Enable");
  }
  // The hardware is left as it is: a vendor driver arms its robot when its
  // hardware is switched on, which its adapter does at Enable
  // (`SwitchArmHardware`).
  std::lock_guard lock(mutex_);
  arm_->requested = core::ComponentLifecycleStateDTO::kPrepared;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::ActivateArmController(
    const std::stop_token cancellation, const MockResetPositionStepDeadline deadline) {
  SerializedStep step(*this, SerializedStepKind::kArm);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  std::function<core::Result<void>()> before_activation;
  std::optional<ToolContextHook> hook;
  CommittedToolContextSourceFn committed_tool_context;
  CommittedSceneSourceFn committed_scene;
  std::optional<std::string> active_tcp_frame;
  bool offset_sync = true;
  // The controller took a TCP offset (and was re-armed) in
  // this step.
  bool offset_applied = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !participant_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    // Already enabled by this host (bound and active), the
    // Enable is satisfied; a switch would be refused.
    if (arm_->bound && arm_->activity.observed && arm_->activity.controller_active)
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    if (guard_hooks_.has_value())
      before_activation = guard_hooks_->before_activation;
    hook = tool_context_hook_;
    committed_tool_context = committed_enable_tool_context_source_;
    committed_scene = committed_scene_source_;
    // Active TCP: Enable keeps it and re-syncs it.
    if (active_tcp_.value())
      active_tcp_frame = active_tcp_.value()->frame.compiled_frame_name;
    offset_sync = active_tcp_offset_sync_;
  }
  if (before_activation) {
    const auto gated = before_activation();
    if (!gated.ok())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, gated.error()};
  }
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> committed_snapshot;
  if (committed_scene) {
    try { committed_snapshot = committed_scene(); }
    catch (...) {
      return not_sent("execution_port_not_ready", "Scene snapshot is unavailable");
    }
    if (!committed_snapshot || !committed_snapshot->revision.valid())
      return not_sent("execution_port_not_ready", "Scene snapshot is unavailable");
  }
  if (hook) {
    // Active TCP: the Enable re-sync at the Active TCP (the
    // flange with no TCP): the hook sends its offset when the
    // controller reports another (zero with the TCP offset sync off), then
    // the total payload (as part of this step, its remaining time and
    // cancellation). It runs before the switch (after it the re-arm would
    // make the driver reactivate every controller). A sent offset or re-arm
    // that may still act leaves the step uncertain.
    if (!committed_snapshot || !committed_tool_context)
      return not_sent("ros2_control_switch_unavailable", "Committed Scene source is unavailable");
    const auto context = committed_tool_context(active_tcp_frame, *committed_snapshot);
    if (!context.ok())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, context.error()};
    const core::ToolContextPointMassDTO load{context.value().payload_mass_kg,
                                             context.value().payload_center_of_mass_m};
    const core::Pose3DTO offset = active_tcp_frame && offset_sync
                                      ? context.value().tcp_pose_in_flange
                                      : core::Pose3DTO{};
    const auto applied = hook->apply(offset, load, true, deadline, cancellation);
    if (!applied.ok())
      return {tool_context_effect_remains(applied.error())
                  ? RetainedRos2ControlSwitchOutcome::kUncertain
                  : RetainedRos2ControlSwitchOutcome::kNotSent,
              applied.error()};
    {
      std::lock_guard lock(mutex_);
      last_tool_load_ = load;
      last_tool_offset_ = offset;
    }
    offset_applied = applied.value();
  }
  // After an applied offset the step had an effect: a switch then
  // refused or not sent is a known failure with that observed effect.
  const auto with_offset_effect = [&](RetainedRos2ControlSwitchResult result) {
    if (offset_applied && (result.outcome == RetainedRos2ControlSwitchOutcome::kNotSent ||
                           result.outcome == RetainedRos2ControlSwitchOutcome::kFailed ||
                           result.outcome == RetainedRos2ControlSwitchOutcome::kFailedNoEffect))
      result.outcome = RetainedRos2ControlSwitchOutcome::kFailedEffectObserved;
    return result;
  };
  // A controller the activity already reports active (e.g. a
  // vendor driver reactivated it after the tool-context hook's re-arm)
  // satisfies the Enable; it is bound as a fresh epoch with no switch (the
  // controller manager refuses a strict activation of an active controller).
  bool already_active = false;
  {
    std::unique_lock lock(mutex_);
    const auto rotated = RotateEpochLocked(lock, deadline);
    if (!rotated.ok())
      return with_offset_effect({RetainedRos2ControlSwitchOutcome::kNotSent, rotated.error()});
    const auto &activity = arm_->activity;
    already_active = activity.controller_manager_present && activity.observed &&
                     activity.controller_active &&
                     (!activity.broadcaster_loaded || activity.broadcaster_active);
  }
  auto result = already_active
                    ? RetainedRos2ControlSwitchResult{RetainedRos2ControlSwitchOutcome::kSucceeded,
                                                      std::nullopt}
                    : SendSwitch(true, cancellation, std::nullopt, deadline);
  std::lock_guard lock(mutex_);
  if (result.outcome != RetainedRos2ControlSwitchOutcome::kNotSent)
    arm_->epoch_used = true;  // it may be active on this epoch now
  result = with_offset_effect(std::move(result));
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
    arm_->bound = true;
    arm_->active_seen_while_bound = arm_->activity.controller_manager_present &&
                                    arm_->activity.controller_active;
    arm_->requested = core::ComponentLifecycleStateDTO::kActive;
    // An Enable binds a fresh epoch too.
    rearm_rebind_required_ = false;
    PublishReadinessLocked();
  }
  return result;
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::DeactivateArmController(
    const std::stop_token cancellation, const MockResetPositionStepDeadline deadline) {
  SerializedStep step(*this, SerializedStepKind::kArm);
  bool active = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    active = !arm_->activity.observed || arm_->activity.controller_active;
  }
  const auto result =
      active ? SendSwitch(false, cancellation, std::nullopt, deadline)
             : RetainedRos2ControlSwitchResult{
                   RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
  std::lock_guard lock(mutex_);
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
    arm_->bound = false;
    arm_->requested = core::ComponentLifecycleStateDTO::kDisabled;
    PublishReadinessLocked();
  }
  return result;
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::SwitchArmHardware(const bool active,
                                                    const std::stop_token cancellation) {
  SerializedStep step(*this, SerializedStepKind::kArm);
  bool hardware_active = false;
  bool broadcaster_loaded = false;
  bool broadcaster_off = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    const auto &activity = arm_->activity;
    if (!activity.observed || activity.controller_active)
      return not_sent("ros2_control_controller_still_active",
                      "the trajectory controller is switched off first");
    hardware_active = activity.hardware_active;
    broadcaster_loaded = activity.broadcaster_loaded;
    broadcaster_off = broadcaster_loaded && !activity.broadcaster_active;
  }
  if (hardware_active != active) {
    auto switched = SendHardwareState(active, cancellation, std::nullopt);
    if (switched.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded || !active)
      return switched;
    // The hardware's activation may or may not have deactivated the joint
    // state broadcaster (it stays active e.g. after a driver restart, whose
    // spawner activated it while the hardware was inactive): switch it on
    // unless already active (best effort: an active one is left as is).
    if (broadcaster_loaded)
      return SendControllerSwitch({std::string(kJointStateBroadcasterControllerName)}, {},
                                  cancellation, std::nullopt, std::nullopt, true);
  }
  // Switched on: the joint state broadcaster too, when it is off (a vendor
  // driver that deactivated its controllers while its robot was not ready
  // and didn't reactivate them). Best effort: one the driver switched on
  // meanwhile is left as is.
  if (active && broadcaster_off)
    return SendControllerSwitch({std::string(kJointStateBroadcasterControllerName)}, {},
                                cancellation, std::nullopt, std::nullopt, true);
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::SendHardwareState(
    const bool active, const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> step_deadline) {
  rclcpp::Client<HardwareStateService>::SharedPtr client;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner;
  std::string hardware;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !arm_->hardware_state_client)
      return not_sent("ros2_control_hardware_state_unavailable",
                      "the hardware-state client is not started");
    client = arm_->hardware_state_client;
    owner = resources_;
    hardware = arm_->transport.controller.hardware_component_name;
  }
  if (!client->service_is_ready())
    return not_sent("ros2_control_hardware_state_service_unavailable",
                    "the controller manager's set_hardware_component_state "
                    "service is not available");
  if (cancellation.stop_requested())
    return not_sent("ros2_control_hardware_state_cancelled",
                    "the hardware transition was cancelled before dispatch");
  auto request = std::make_shared<HardwareStateService::Request>();
  request->name = hardware;
  request->target_state.id =
      active ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
             : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
  request->target_state.label = active ? "active" : "inactive";
  // Allowed min(the call cap, the step's
  // remaining time), checked right at the send; none left, nothing is sent.
  if (auto refused = refuse_if_expired_at_send(step_deadline)) return *refused;
  const auto logger = owner->node()->get_logger();
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = allowed_until(started, step_deadline);
  std::optional<decltype(client->async_send_request(request))> pending;
  if (!owner->try_sdk_send([&] { pending.emplace(client->async_send_request(request)); }))
    return not_sent("ros2_control_hardware_state_unavailable", "The ROS owner closed before hardware-state dispatch");
  auto future = std::move(*pending);
  while (future.wait_for(std::chrono::milliseconds(10)) !=
         std::future_status::ready) {
    if (cancellation.stop_requested() ||
        std::chrono::steady_clock::now() >= deadline) {
      client->remove_pending_request(future);
      log_call(logger, "set_hardware_component_state",
               cancellation.stop_requested() ? "cancelled" : "timeout", started);
      return {RetainedRos2ControlSwitchOutcome::kUncertain,
              switch_error(cancellation.stop_requested()
                               ? "ros2_control_hardware_state_cancelled"
                               : "ros2_control_hardware_state_timeout",
                           "the hardware transition got no answer; its "
                           "effect is unknown")};
    }
  }
  const auto response = future.get();
  log_call(logger, "set_hardware_component_state", "answered", started);
  if (!response || !response->ok)
    return {RetainedRos2ControlSwitchOutcome::kFailed,
            switch_error(active ? "ros2_control_hardware_activation_refused"
                                : "ros2_control_hardware_deactivation_refused",
                         "the controller manager refused the hardware "
                         "transition")};
  // The transition's result is the reported hardware state.
  if (auto unseen = WaitForReportedActivity({}, {}, active, cancellation, deadline))
    return *unseen;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

std::optional<MockResetPositionTarget>
RetainedRos2ControlRuntimeHandle::mock_reset_position_target() const {
  std::lock_guard lock(mutex_);
  return arm_ ? arm_->transport.mock_reset_position : std::nullopt;
}

bool RetainedRos2ControlRuntimeHandle::mock_reset_writer_inactive() const {
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr publisher;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !arm_->transport.mock_reset_position ||
        !arm_->activity.controller_manager_present ||
        !arm_->activity.observed || !arm_->reset_writer_loaded ||
        arm_->reset_writer_active || arm_->reset_writer_possibly_active)
      return false;
    publisher = arm_->reset_publisher;
  }
  return publisher && publisher->get_subscription_count() == 1U;
}

bool RetainedRos2ControlRuntimeHandle::mock_reset_writer_active() const {
  std::lock_guard lock(mutex_);
  return arm_ && arm_->transport.mock_reset_position &&
         arm_->activity.controller_manager_present && arm_->activity.observed &&
         arm_->activity.hardware_active && arm_->activity.controller_loaded &&
         !arm_->activity.controller_active && arm_->reset_writer_loaded &&
         arm_->reset_writer_active;
}

std::shared_ptr<RetainedObservationWake>
RetainedRos2ControlRuntimeHandle::mock_reset_observation_wake() const noexcept {
  return reset_observation_wake_;
}

std::optional<ResetCommandEcho>
RetainedRos2ControlRuntimeHandle::reset_command_echo() const {
  std::lock_guard lock(mutex_);
  return arm_ ? arm_->reset_echo : std::nullopt;
}

bool RetainedRos2ControlRuntimeHandle::mock_reset_successor_ready() const {
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !arm_->reset_position_pending || !arm_->bound ||
        !arm_->activity.controller_manager_present ||
        !arm_->activity.observed || !arm_->activity.hardware_active ||
        !arm_->activity.controller_active || !arm_->reset_writer_loaded ||
        arm_->reset_writer_active || arm_->reset_writer_possibly_active)
      return false;
    adapter = arm_->adapter;
    activity = arm_->activity_endpoints;
  }
  if (!activity)
    return false;
  // The existing switchback wait owns endpoint discovery within its step
  // deadline. Reset does not synthesize a separate lifecycle transport loss.
  activity->Refresh();
  adapter->RefreshEndpointFacts();
  if (ReadObservation(clock_->now()).process_fault)
    return false;
  const bool discovered = adapter->ActionServerIsReady();
  std::lock_guard lock(mutex_);
  return discovered && arm_ && arm_->adapter == adapter &&
         arm_->activity_endpoints == activity && arm_->bound &&
         arm_->activity.controller_manager_present &&
         arm_->activity.hardware_active && arm_->activity.controller_active &&
         !arm_->reset_writer_active && !arm_->reset_writer_possibly_active;
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::ResetMockPosition(
    const std::stop_token cancellation,
    const MockResetPositionPhaseCallback &phase) {
  SerializedStep step(*this, SerializedStepKind::kArm);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  MockResetPositionTarget target;
  std::string controller;
  std::uint64_t predecessor{};
  std::uint64_t expected_successor{};
  std::optional<core::ExecutionBindingEpoch> successor;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr publisher;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> publisher_owner;
  {
    std::lock_guard lock(mutex_);
    if (!phase || !arm_ || !arm_->transport.mock_reset_position ||
        !arm_->reset_publisher || !arm_->bound ||
        arm_->reset_position_pending ||
        !arm_->activity.controller_manager_present ||
        !arm_->activity.observed || !arm_->activity.hardware_active ||
        !arm_->activity.controller_active || !arm_->reset_writer_loaded ||
        arm_->reset_writer_active || arm_->reset_writer_possibly_active)
      return not_sent("mock_reset_position_unavailable",
                      "Mock position reset requires the active exact arm and "
                      "inactive writer");
    target = *arm_->transport.mock_reset_position;
    controller = arm_->transport.controller.controller_name;
    predecessor = arm_->epoch;
    if (arm_->epoch_used &&
        arm_->epoch == std::numeric_limits<std::uint64_t>::max())
      return not_sent("ros2_control_binding_epoch_exhausted",
                      "ROS controller binding epoch is exhausted");
    expected_successor = arm_->epoch_used ? arm_->epoch + 1U : arm_->epoch;
    publisher = arm_->reset_publisher;
    publisher_owner = resources_;
  }
  if (cancellation.stop_requested())
    return not_sent("mock_reset_position_cancelled",
                    "Mock position reset was cancelled before effects");
  if (publisher->get_subscription_count() != 1U)
    return not_sent("mock_reset_writer_endpoint_unavailable",
                    "Mock position reset requires exactly one discovered stock "
                    "writer subscription");
  MockResetPositionStepDeadline step_deadline;
  const auto notify = [&](MockResetPositionPhase next,
                          bool acknowledged = false, bool published = false) {
    // The existing one-second budget now starts at actual step
    // entry, so local validation/dispatch consume it as explicitly approved.
    // Baseline capture, the single writer publication and consumption ACK are
    // one step. The post-publication callback cannot restart its budget.
    if (next != MockResetPositionPhase::kSettingState || !published)
      step_deadline = std::chrono::steady_clock::now() +
          std::chrono::nanoseconds(kMockResetPositionWriteAcknowledgmentTimeoutNs);
    const MockResetPositionTransition transition{
        next, core::ExecutionBindingEpoch(predecessor), successor, acknowledged,
        published};
    try {
      auto result = phase(transition, step_deadline);
      if (result.ok() && std::chrono::steady_clock::now() >= step_deadline)
        return core::Result<void>::Err(switch_error(
            "mock_reset_step_timeout", "The position Reset step deadline elapsed"));
      return result;
    } catch (const std::exception &error) {
      return core::Result<void>::Err(
          switch_error("mock_reset_phase_failed", error.what()));
    } catch (...) {
      return core::Result<void>::Err(switch_error(
          "mock_reset_phase_failed", "Reset phase callback failed"));
    }
  };
  auto accepted = notify(MockResetPositionPhase::kSwitchingToWriter);
  if (!accepted.ok())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, accepted.error()};
  if (cancellation.stop_requested())
    return not_sent("mock_reset_position_cancelled",
                    "Mock position reset was cancelled before effects");
  const auto send = [&](std::vector<std::string> activate,
                        std::vector<std::string> deactivate) {
    try {
      return SendControllerSwitch(
          std::move(activate), std::move(deactivate), cancellation,
          step_deadline);
    } catch (const std::exception &error) {
      return RetainedRos2ControlSwitchResult{
          RetainedRos2ControlSwitchOutcome::kUncertain,
          switch_error("mock_reset_switch_unknown", error.what())};
    } catch (...) {
      return RetainedRos2ControlSwitchResult{
          RetainedRos2ControlSwitchOutcome::kUncertain,
          switch_error("mock_reset_switch_unknown",
                       "Controller switch outcome is unknown")};
    }
  };
  {
    std::lock_guard lock(mutex_);
    arm_->reset_position_pending = true;
    arm_->reset_motion_admitted->store(false);
    arm_->reset_writer_possibly_active = true;
    PublishReadinessLocked();
  }
  // This is containment only: never resend a target, reactivate JTC or roll
  // back after a failed/unknown effect. The owner retains the Reset claim.
  const auto abort = [&](RetainedRos2ControlSwitchResult failed) {
    static_cast<void>(notify(MockResetPositionPhase::kContaining));
    bool possibly_active;
    {
      std::lock_guard lock(mutex_);
      possibly_active =
          arm_->reset_writer_possibly_active || arm_->reset_writer_active;
      arm_->bound = false;
      PublishReadinessLocked();
    }
    if (possibly_active) {
      const auto contained = send({}, {target.controller_name});
      if (contained.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
        std::lock_guard lock(mutex_);
        arm_->reset_writer_possibly_active = false;
      } else {
        failed.outcome = RetainedRos2ControlSwitchOutcome::kUncertain;
        if (failed.error)
          failed.error->details.emplace("writer_containment", "unresolved");
      }
    }
    if (failed.outcome == RetainedRos2ControlSwitchOutcome::kNotSent)
      failed.outcome = RetainedRos2ControlSwitchOutcome::kFailed;
    if (failed.error)
      failed.error->message +=
          " Robot position unknown after a failed position reset. Restart the "
          "workbench to recover.";
    return failed;
  };
  auto switched = send({target.controller_name}, {controller});
  if (switched.outcome == RetainedRos2ControlSwitchOutcome::kNotSent) {
    // The service was unavailable before dispatch: no physical transition
    // occurred, so preserve the original active binding and existing retry.
    std::lock_guard lock(mutex_);
    arm_->reset_position_pending = false;
    arm_->reset_motion_admitted->store(true);
    arm_->reset_writer_possibly_active = false;
    PublishReadinessLocked();
    return switched;
  }
  if (switched.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
    return abort(std::move(switched));
  // Only a positive exact ACK enters this observation barrier. Unknown or
  // timed-out switches went directly to containment above; later activity
  // never changes their classification or authorizes a compensating switch.
  accepted = notify(MockResetPositionPhase::kSwitchingToWriter, true);
  if (!accepted.ok())
    return abort({RetainedRos2ControlSwitchOutcome::kFailed, accepted.error()});
  if (!mock_reset_writer_active())
    return abort(
        {RetainedRos2ControlSwitchOutcome::kFailed,
         switch_error(
             "mock_reset_writer_activation_unobserved",
             "The acknowledged writer destination has not been observed")});
  accepted = notify(MockResetPositionPhase::kSettingState);
  if (!accepted.ok())
    return abort({RetainedRos2ControlSwitchOutcome::kFailed, accepted.error()});
  if (cancellation.stop_requested())
    return abort(
        not_sent("mock_reset_position_cancelled",
                 "Mock position reset was cancelled before publication"));
  std_msgs::msg::Float64MultiArray command;
  command.data = target.positions;
  try {
    if (!publisher_owner->try_sdk_send([&] { publisher->publish(command); }))
      return abort(not_sent("mock_reset_position_unavailable",
                            "The ROS owner closed before target publication"));
  } catch (const std::exception &error) {
    return abort({RetainedRos2ControlSwitchOutcome::kUncertain,
                  switch_error("mock_reset_publish_unknown", error.what())});
  } catch (...) {
    return abort({RetainedRos2ControlSwitchOutcome::kUncertain,
                  switch_error("mock_reset_publish_unknown",
                               "Target publication outcome is unknown")});
  }
  // GenericSystem 6.9.0 generic_system.cpp:449-476 (read's position branch)
  // loops the position command back exactly. One fresh target-valued sample
  // acknowledges writer consumption; no measured convergence/dwell follows.
  // A DDS delivery ACK alone does not prove the writer's update ran.
  accepted = notify(MockResetPositionPhase::kSettingState, false, true);
  if (!accepted.ok())
    return abort(
        {RetainedRos2ControlSwitchOutcome::kUncertain, accepted.error()});
  successor = core::ExecutionBindingEpoch(expected_successor);
  accepted = notify(MockResetPositionPhase::kSwitchingToJtc);
  if (!accepted.ok())
    return abort({RetainedRos2ControlSwitchOutcome::kFailed, accepted.error()});
  try {
    std::unique_lock lock(mutex_);
    accepted = RotateEpochLocked(lock, step_deadline);
    if (accepted.ok() && arm_->epoch != expected_successor)
      accepted = core::Result<void>::Err(switch_error(
          "mock_reset_successor_mismatch",
          "The rotated binding differs from the authorized successor"));
  } catch (const std::exception &error) {
    accepted = core::Result<void>::Err(
        switch_error("mock_reset_rotation_unknown", error.what()));
  } catch (...) {
    accepted = core::Result<void>::Err(switch_error(
        "mock_reset_rotation_unknown", "Binding rotation did not complete"));
  }
  if (!accepted.ok())
    return abort(
        {RetainedRos2ControlSwitchOutcome::kUncertain, accepted.error()});
  if (cancellation.stop_requested())
    return abort(not_sent(
        "mock_reset_position_cancelled",
        "Mock position reset was cancelled before successor activation"));
  switched = send({controller}, {target.controller_name});
  {
    std::lock_guard lock(mutex_);
    if (switched.outcome != RetainedRos2ControlSwitchOutcome::kNotSent)
      arm_->epoch_used = true;
    if (switched.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
      arm_->reset_writer_possibly_active = false;
      arm_->bound = true;
      arm_->active_seen_while_bound = arm_->activity.controller_active;
      arm_->requested = core::ComponentLifecycleStateDTO::kActive;
      PublishReadinessLocked();
    }
  }
  if (switched.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
    return abort(std::move(switched));
  accepted = notify(MockResetPositionPhase::kSwitchingToJtc, true);
  if (!accepted.ok())
    return abort({RetainedRos2ControlSwitchOutcome::kFailed, accepted.error()});
  if (!mock_reset_successor_ready())
    return abort(
        {RetainedRos2ControlSwitchOutcome::kFailed,
         switch_error(
             "mock_reset_jtc_activation_unobserved",
             "The acknowledged successor destination has not been observed")});
  {
    std::lock_guard lock(mutex_);
    arm_->reset_position_pending = false;
    arm_->reset_motion_admitted->store(true);
    PublishReadinessLocked();
  }
  return switched;
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::RecoverArmController(
    const std::stop_token cancellation, const MockResetPositionStepDeadline deadline) {
  SerializedStep step(*this, SerializedStepKind::kArm);
  std::function<void()> on_recover;
  bool active = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    if (guard_hooks_.has_value())
      on_recover = guard_hooks_->on_recover;
    active = arm_->activity.controller_active;
  }
  if (active) {
    // A controller still active after an incident is deactivated first; the
    // next Enable reactivates it on a new binding epoch.
    const auto result =
        SendSwitch(false, cancellation, std::nullopt, deadline);
    if (result.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
      return result;
  }
  if (on_recover)
    on_recover();
  // The hardware is left as it is (see PrepareArmController).
  std::lock_guard lock(mutex_);
  // Active TCP: Recover ends a tool-context fault; Enable re-syncs.
  tool_context_fault_.reset();
  arm_->bound = false;
  arm_->requested = core::ComponentLifecycleStateDTO::kPrepared;
  PublishReadinessLocked();
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::PrepareGripperController(
    const std::stop_token cancellation, const MockResetPositionStepDeadline deadline) {
  if (auto waited = wait_for_endpoints(
          cancellation, deadline, [&]() -> std::optional<RetainedRos2ControlSwitchResult> {
            RefreshEndpointFacts();
            if (auto error = ReadEndpointDiagnostic())
              return RetainedRos2ControlSwitchResult{RetainedRos2ControlSwitchOutcome::kNotSent,
                                                     std::move(error)};
            ros2_control_execution::Ros2GripperCommandBackend *bridge_gripper = nullptr;
            std::optional<std::string> bridge_id;
            std::shared_ptr<GripperOnlyState> bridge_owner;
            {
              std::lock_guard lock(mutex_);
              if (gripper_ && gripper_id_ &&
                  gripper_->gripper_actuation_kind(*gripper_id_) ==
                      core::GripperActuationKind::kPneumatic &&
                  gripper_->gripper_output_mode(*gripper_id_) == core::GripperOutputMode::kNone)
                return std::nullopt;
              if ((!arm_ || !arm_->transport.gripper_controller) && !gripper_only_)
                return std::nullopt;  // refused below, nothing to wait for
              if (gripper_only_ && !gripper_only_->physical_controller_required) {
                bridge_gripper = gripper_.get();
                bridge_id = gripper_id_;
                bridge_owner = gripper_only_;
              } else {
                if (!gripper_activity_.controller_manager_present ||
                    !gripper_activity_.observed || !gripper_activity_.controller_loaded ||
                    !gripper_activity_.hardware_active)
                  return not_sent("ros2_control_gripper_controller_not_loaded",
                                  "the exact gripper controller is not loaded on active hardware");
                return std::nullopt;
              }
            }
            {
              // The Isaac command bridge: its command path up (its action
              // server), no controller manager. Its state is a stream that
              // gates commands, never the lifecycle. Query outside mutex_: the
              // owner may be running a display callback that needs that mutex.
              const auto ready =
                  bridge_gripper->gripper_transport_readiness(*bridge_id).command_path_ready;
              std::lock_guard lock(mutex_);
              // These identities bind once and remain until handle destruction.
              if (gripper_.get() != bridge_gripper || gripper_id_ != bridge_id ||
                  gripper_only_ != bridge_owner || bridge_owner->physical_controller_required)
                throw std::logic_error("gripper bridge identity changed while bound");
              if (ready)
                return std::nullopt;
              return RetainedRos2ControlSwitchResult{
                  RetainedRos2ControlSwitchOutcome::kNotSent,
                  core::ErrorDTO{"ros2_control_gripper_bridge_not_ready",
                                 "The exact gripper command bridge is not ready", {}}};
            }
          }))
    return *waited;
  {
    std::lock_guard lock(mutex_);
    if (gripper_ && gripper_id_ &&
        gripper_->gripper_actuation_kind(*gripper_id_) == core::GripperActuationKind::kPneumatic &&
        gripper_->gripper_output_mode(*gripper_id_) == core::GripperOutputMode::kNone) {
      const auto owner = gripper_->gripper_transport_readiness(*gripper_id_);
      if (!owner.configured || !owner.logical_owner_ready)
        return not_sent("ros2_control_gripper_logical_owner_unavailable",
                        "The exact no-output gripper owner is not configured and ready");
      // This is the same configured logical owner, not a fabricated loaded CM
      // controller. The component coordinator already closed command
      // eligibility for this action.
      gripper_requested_ = core::ComponentLifecycleStateDTO::kPrepared;
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    }
    if ((!arm_ || !arm_->transport.gripper_controller) && !gripper_only_)
      return not_sent("ros2_control_gripper_controller_unbound", "No exact gripper controller is bound");
  }
  // gripper_only_ is set at bind time and never rebound.
  if (gripper_only_ && !gripper_only_->physical_controller_required)
    return RunGripperBridgeLifecycle(core::ComponentLifecycleStateDTO::kPrepared, cancellation, deadline);
  std::lock_guard lock(mutex_);
  // Enabled by this host: Prepare doesn't stop it; Disable does.
  if (gripper_activity_.controller_active &&
      gripper_requested_ == core::ComponentLifecycleStateDTO::kActive)
    return component_enabled();
  if (gripper_activity_.controller_active)
    return not_sent("ros2_control_gripper_controller_active_unrequested",
                    "the gripper controller is active without CCLC activation");
  gripper_requested_ = core::ComponentLifecycleStateDTO::kPrepared;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::SwitchGripperController(
    const bool activate, const std::stop_token cancellation,
    const MockResetPositionStepDeadline deadline) {
  SerializedStep step(*this, SerializedStepKind::kGripper);
  if (activate) {
    RefreshEndpointFacts();
    if (auto error = ReadEndpointDiagnostic())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  }
  std::string controller;
  bool needs_switch;
  bool bridge;
  {
    std::lock_guard lock(mutex_);
    if (gripper_ && gripper_id_ &&
        gripper_->gripper_actuation_kind(*gripper_id_) == core::GripperActuationKind::kPneumatic &&
        gripper_->gripper_output_mode(*gripper_id_) == core::GripperOutputMode::kNone) {
      if (cancellation.stop_requested())
        return not_sent("ros2_control_switch_cancelled",
                        "No-output gripper lifecycle request was cancelled before its local change");
      const auto owner = gripper_->gripper_transport_readiness(*gripper_id_);
      if (!owner.configured || !owner.logical_owner_ready)
        return not_sent("ros2_control_gripper_logical_owner_unavailable",
                        "The exact no-output gripper owner is not configured and ready");
      gripper_requested_ = activate ? core::ComponentLifecycleStateDTO::kActive
                                    : core::ComponentLifecycleStateDTO::kDisabled;
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    }
    if ((!arm_ || !arm_->transport.gripper_controller) && !gripper_only_)
      return not_sent("ros2_control_gripper_controller_unbound",
                      "no exact controller-manager gripper is bound");
    bridge = gripper_only_ && !gripper_only_->physical_controller_required;
  }
  if (bridge)
    return RunGripperBridgeLifecycle(activate ? core::ComponentLifecycleStateDTO::kActive
                                              : core::ComponentLifecycleStateDTO::kDisabled,
                                     cancellation, deadline);
  {
    std::lock_guard lock(mutex_);
    controller = arm_ ? arm_->transport.gripper_controller->controller_name
                      : gripper_only_->controller.controller_name;
    // An already satisfied Enable or Disable succeeds at once
    // (the controller manager refuses a switch to the current state).
    needs_switch = !gripper_activity_.observed ||
                   gripper_activity_.controller_active != activate;
  }
  const auto result =
      needs_switch
          ? SendSwitch(activate, cancellation, controller, deadline)
          : RetainedRos2ControlSwitchResult{
                RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
    std::lock_guard lock(mutex_);
    gripper_requested_ = activate ? core::ComponentLifecycleStateDTO::kActive
                                  : core::ComponentLifecycleStateDTO::kDisabled;
  }
  return result;
}

RetainedRos2ControlSwitchResult RetainedRos2ControlRuntimeHandle::RunGripperBridgeLifecycle(
    const core::ComponentLifecycleStateDTO target, const std::stop_token cancellation,
    const MockResetPositionStepDeadline deadline) {
  using Lifecycle = core::ComponentLifecycleStateDTO;
  // Bound at bind time and never rebound: no lock spans the backend's calls.
  auto &gripper = *gripper_;
  const auto &id = *gripper_id_;
  const auto result = [&]() -> ros2_control_execution::Ros2GripperLifecycleResult {
    const auto transport = gripper.gripper_transport_readiness(id);
    if (transport.transport_kind == "isaac_joint_state_effort") {
      // No remote command owner to prepare, authorize or cancel: the
      // lifecycle is the local request; transport readiness stays observed.
      if (target != Lifecycle::kDisabled && !transport.command_path_ready)
        return {false, false, core::ErrorDTO{"ros2_control_gripper_bridge_not_ready",
                                             "The exact gripper command transport is not ready", {}}};
      return {true, false, std::nullopt};
    }
    if (target == Lifecycle::kDisabled) return gripper.disable_lifecycle_gripper(id, cancellation, deadline);
    auto prepared = gripper.prepare_lifecycle_gripper(id);
    if (target == Lifecycle::kPrepared || !prepared.succeeded) return prepared;
    return gripper.authorize_lifecycle_gripper(id);
  }();
  if (!result.succeeded)
    return {result.effect_started ? RetainedRos2ControlSwitchOutcome::kUncertain
                                  : RetainedRos2ControlSwitchOutcome::kNotSent,
            result.error};
  std::lock_guard lock(mutex_);
  gripper_requested_ = target;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

std::optional<RetainedControllerStatisticsSnapshot>
RetainedRos2ControlRuntimeHandle::controller_statistics() const {
  std::shared_ptr<RetainedControllerStatisticsMonitor> monitor;
  {
    std::lock_guard lock(mutex_);
    if (arm_)
      monitor = arm_->statistics_monitor;
  }
  if (!monitor)
    return std::nullopt;
  return monitor->Read();
}

std::optional<core::ControlDomainId>
RetainedRos2ControlRuntimeHandle::motion_domain() const {
  std::lock_guard lock(mutex_);
  return motion_domain_id_;
}

RetainedRos2ControlRuntimeHandle::TransportOwners
RetainedRos2ControlRuntimeHandle::TransportOwnerSnapshot() const {
  std::lock_guard lock(mutex_);
  return {resources_, arm_ ? arm_->epoch_resources : nullptr};
}

std::shared_ptr<const RetainedRos2ControlRuntimeHandle::TransportOwners>
RetainedRos2ControlRuntimeHandle::PublishedTransportOwners() const noexcept {
  return published_transport_owners_.load(std::memory_order_acquire);
}

core::ActionResult RetainedRos2ControlRuntimeHandle::RecoverFaultedOwner(
    TransportOwners expected, std::stop_token stop, TransportOwners &replacement) {
  replacement = {};
  const auto fail = [](std::string message) {
    core::ActionResult result;
    result.action = core::ComponentAction::kRecover;
    result.outcome = core::ActionOutcome::kFailed;
    result.error = core::ActionError{"component_not_ready", std::move(message), {}, {}};
    return result;
  };
  const auto recovery_started = std::chrono::steady_clock::now();
  core::Duration recovery_timeout = 0;
  {
    std::lock_guard lock(mutex_);
    if (arm_) recovery_timeout = arm_->transport.adapter.lifecycle_command_timeout_ns;
    else if (owner_shutdown_timeout_ns_) recovery_timeout = *owner_shutdown_timeout_ns_;
  }
  if (recovery_timeout <= 0) return fail("The authored owner recovery timeout is unavailable");
  const auto recovery_deadline = recovery_started + std::chrono::nanoseconds(recovery_timeout);
  const auto current = TransportOwnerSnapshot();
  if (!expected.machine || current.machine != expected.machine || current.epoch != expected.epoch)
    return fail("The accepted ROS owner identity is no longer current");
  const bool machine_failed = expected.machine->executor_failure_started();
  const bool epoch_failed = expected.epoch && expected.epoch->executor_failure_started();
  if (!machine_failed && !epoch_failed) return fail("The accepted ROS owner is not faulted");
  if (stop.stop_requested()) return fail("ROS owner recovery was interrupted");
  try {
    auto stage = StageMachineOwner();
    if (stage->previous != expected.machine || stage->previous_epoch_owner != expected.epoch)
      return fail("The accepted ROS owner changed while staging recovery");
    if (stage->previous_arm && (!stage->participant || !stage->participant_stage ||
                                !stage->notify || !stage->qualified))
      return fail("The arm recovery observation owner is unavailable");
    const auto failed = machine_failed ? expected.machine : expected.epoch;
    const auto fault = failed->executor_fault();
    if (!fault) return fail("The ROS owner fault is not yet published");
    std::string pending_fault = "ROS transport executor thread failed: " + *fault +
        " (owner " + failed->node()->get_fully_qualified_name() + ")";
    if (stage->previous_adapter) {
      stage->previous_adapter->close_admission_without_cancel();
      auto unresolved = stage->previous_adapter->ReadUnresolvedLateCancels();
      if (!unresolved.empty()) {
        std::lock_guard lock(mutex_);
        RetainLateCancelObligationsLocked(std::move(unresolved));
      }
    }
    const auto handed_off = FlushLateCancelHandoff();
    if (!handed_off.ok()) return fail(handed_off.error().message);
    if (machine_failed) {
      if (expected.epoch && !expected.epoch->quiesce_until(recovery_deadline))
        return fail("ROS controller owner did not join before the recovery deadline");
      if (stage->previous_adapter) stage->previous_adapter->release_ros_entities();
      (void)ros2_control_execution::Ros2TransportResources::rebuild_faulted(
          *expected.machine, recovery_deadline, [] {}, [&] { return stage->candidate; }, [](auto &) {});
    } else {
      (void)ros2_control_execution::Ros2TransportResources::rebuild_faulted(
          *expected.epoch, recovery_deadline,
          [&] {
            if (!expected.machine->quiesce_until(recovery_deadline))
              throw std::runtime_error("ROS machine owner did not join before the recovery deadline");
            if (stage->previous_adapter) stage->previous_adapter->release_ros_entities();
          }, [&] { return stage->epoch; }, [](auto &) {});
    }
    if (stage->participant && stage->participant_stage) {
      const auto retired = stage->participant->RetireOwnerRebindTimer(stage->participant_stage);
      if (!retired.ok()) return fail(retired.error().message);
    }
    if (stop.stop_requested()) return fail("ROS owner recovery was interrupted after retirement");

    struct Commit final {
      RetainedRos2ControlRuntimeHandle *self;
      std::shared_ptr<StagedMachineOwner> stage;
      TransportOwners expected;
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> failed;
      std::string pending_fault;
      TransportOwners *replacement{};
      bool participant_locked{};
      bool swapped{};
      bool needs_motion_proof{};
      bool (*swap_slots)(void *) noexcept{};
      bool (*commit_gripper)(void *) noexcept{};
    } commit{this, stage, expected, failed, std::move(pending_fault), &replacement};
    struct UnlockParticipant final {
      std::shared_ptr<StagedMachineOwner> stage;
      bool &locked;
      ~UnlockParticipant() {
        if (locked) {
          stage->participant->UnlockOwnerRebind(stage->participant_stage);
          locked = false;
        }
      }
    } unlock{stage, commit.participant_locked};

    const auto swap_slots = [](void *opaque) noexcept -> bool {
      auto &c = *static_cast<Commit *>(opaque);
      auto &s = *c.stage;
      auto &self = *c.self;
      try {
        if (self.closing_) return false;
        if (s.participant && s.participant_stage) {
          const auto locked = s.participant->LockOwnerRebind(s.participant_stage);
          if (!locked.ok()) return false;
          c.participant_locked = true;
          c.needs_motion_proof = s.participant->LockedOwnerRebindNeedsMotionProof(s.participant_stage);
          s.participant->CommitLockedOwnerRebind(s.participant_stage);
        }
        self.resources_ = s.candidate;
        if (s.previous_arm) {
          auto &arm = *self.arm_;
          arm.epoch_resources.swap(s.epoch);
          arm.adapter.swap(s.adapter);
          arm.port.swap(s.port);
          arm.activity_endpoints.swap(s.activity_endpoints);
          arm.activity_subscription.swap(s.activity_subscription);
          arm.switch_client.swap(s.switch_client);
          arm.hardware_state_client.swap(s.hardware_state_client);
          arm.reset_publisher.swap(s.reset_publisher);
          arm.reset_echo_subscription.swap(s.reset_echo_subscription);
          arm.statistics_monitor.swap(s.statistics_monitor);
          arm.statistics_subscription.swap(s.statistics_subscription);
          ++arm.epoch;
          arm.epoch_used = false;
          arm.rebind_pending = false;
          arm.bound = false;
          arm.active_seen_while_bound = false;
          arm.activity = {};
          self.gripper_activity_ = {};
          arm.activation_client_discovered = false;
          self.owner_recovery_marker_.reset();
          if (c.needs_motion_proof)
            self.owner_recovery_pending_fault_.swap(c.pending_fault);
          else
            self.owner_recovery_pending_fault_.clear();
        } else if (s.previous_gripper) {
          self.gripper_activity_ = {};
          self.gripper_only_->activity_endpoints.swap(s.activity_endpoints);
          self.gripper_only_->activity_subscription.swap(s.activity_subscription);
          self.gripper_only_->switch_client.swap(s.switch_client);
        }
        self.measured_state_subscription_.swap(s.measured_subscription);
        self.display_joint_subscription_.swap(s.display_subscription);
        self.sample_timer_.swap(s.sample_timer);
        self.live_machine_stage_.swap(c.stage);
        self.published_transport_owners_.store(s.published_owners, std::memory_order_release);
        c.replacement->machine = self.resources_;
        c.replacement->epoch = self.arm_ ? self.arm_->epoch_resources : nullptr;
        c.swapped = true;
        return true;
      } catch (...) { return false; }
    };
    const auto commit_gripper = [](void *opaque) noexcept -> bool {
      auto &c = *static_cast<Commit *>(opaque);
      auto &self = *c.self;
      const auto &s = c.stage;
      try {
        if (self.closing_ || self.resources_ != c.expected.machine || self.arm_ != s->previous_arm ||
            self.gripper_only_ != s->previous_gripper ||
            (self.arm_ && (self.arm_->epoch_resources != c.expected.epoch ||
                           self.arm_->epoch != s->previous_epoch)) ||
            !s->candidate->started() || s->candidate->joined() ||
            s->candidate->executor_failure_started() ||
            (s->epoch && (!s->epoch->started() || s->epoch->joined() ||
                          s->epoch->executor_failure_started()))) return false;
        if (s->gripper)
          return self.gripper_->commit_ros_rebind_with(s->gripper, c.swap_slots, &c, c.failed);
        return c.swap_slots(&c);
      } catch (...) { return false; }
    };
    const auto commit_selected = [](void *opaque) noexcept {
      auto &c = *static_cast<Commit *>(opaque);
      try {
        if (c.stage->measured)
          (void)c.self->CommitMeasuredOwnerRebindWith(c.stage->measured, c.commit_gripper, &c, c.failed);
        else {
          std::lock_guard lock(c.self->mutex_);
          (void)c.commit_gripper(&c);
        }
      } catch (...) {}
    };
    commit.swap_slots = swap_slots;
    commit.commit_gripper = commit_gripper;
    std::optional<std::string> post_commit_error;
    {
      SerializedStep step(*this, SerializedStepKind::kArm);
      if (stage->source_commit)
        (void)stage->source_commit(stage->installed_sources, commit_selected, &commit);
      else
        commit_selected(&commit);
      if (commit.participant_locked)
      {
        stage->participant->UnlockOwnerRebind(stage->participant_stage);
        commit.participant_locked = false;
      }
      if (!commit.swapped) return fail("ROS owner identities changed before the joint commit");
      try {
        CommittedSceneSourceFn read;
        {
          std::lock_guard lock(mutex_);
          read = committed_scene_source_;
        }
        if (read) {
          const auto payload = ApplyCommittedToolPayloadClaimed(read);
          if (!payload.ok()) post_commit_error = payload.error().message;
        }
      } catch (const std::exception &error) {
        post_commit_error = error.what();
      } catch (...) {
        post_commit_error = "Committed tool payload refresh failed";
      }
    }
    if (stage->participant) {
      try {
        const auto marker = stage->participant->owner_rebuild_observation();
        std::lock_guard lock(mutex_);
        owner_recovery_marker_ = marker;
        if (!marker) owner_recovery_pending_fault_.clear();
      } catch (...) {
        if (!post_commit_error) post_commit_error = "Owner recovery observation failed";
      }
      try { if (stage->notify) stage->notify(); }
      catch (const std::exception &error) {
        if (!post_commit_error) post_commit_error = error.what();
      } catch (...) {
        if (!post_commit_error) post_commit_error = "Owner recovery notification failed";
      }
    }
    try { ActivateStagedMachineOwner(stage); }
    catch (...) { if (!post_commit_error) post_commit_error = "Machine observer activation failed"; }
    try {
      if (stage->measured) {
        const auto activated = ActivateMeasuredOwnerRebind(stage->measured);
        if (!activated.ok() && !post_commit_error) post_commit_error = activated.error().message;
      }
    } catch (...) { if (!post_commit_error) post_commit_error = "Measured observer activation failed"; }
    try {
      if (stage->gripper) {
        const auto activated = gripper_->activate_ros_rebind(stage->gripper);
        if (!activated.ok() && !post_commit_error) post_commit_error = activated.error().message;
      }
    } catch (...) { if (!post_commit_error) post_commit_error = "Gripper observer activation failed"; }
    stage->previous.reset();
    stage->previous_epoch_owner.reset();
    stage->previous_adapter.reset();
    stage->previous_stage.reset();
    stage->previous_published_owners.reset();
    stage->epoch.reset();
    stage->adapter.reset();
    stage->port.reset();
    stage->activity_endpoints.reset();
    stage->activity_subscription.reset();
    stage->switch_client.reset();
    stage->hardware_state_client.reset();
    stage->reset_publisher.reset();
    stage->reset_echo_subscription.reset();
    stage->statistics_monitor.reset();
    stage->statistics_subscription.reset();
    stage->measured_subscription.reset();
    stage->display_subscription.reset();
    stage->sample_timer.reset();
    stage->gripper.reset();
    stage->measured.reset();
    stage->participant_stage.reset();
    if (replacement.machine->executor_failure_started() ||
        (replacement.epoch && replacement.epoch->executor_failure_started()))
      return fail("The replacement ROS owner failed during recovery");
    if (post_commit_error) return fail(*post_commit_error);
    core::ActionResult result;
    result.action = core::ComponentAction::kRecover;
    result.outcome = core::ActionOutcome::kSucceeded;
    return result;
  } catch (const std::exception &error) {
    return fail(error.what());
  } catch (...) {
    return fail("ROS owner recovery failed with an unknown exception");
  }
}

Ros2ControlObservation
RetainedRos2ControlRuntimeHandle::ReadObservation(const core::TimePoint now_ns) const {
  static_cast<void>(now_ns);
  Ros2ControlObservation observation;
  // Owner stall snapshots are independent of the handle and owner locks.
  // Return before reading any state that the stalled task may hold.
  if (const auto owners = PublishedTransportOwners()) {
    for (const auto &owner : {owners->machine, owners->epoch}) {
      if (!owner) continue;
      if (const auto stalled = owner->owner_stall()) {
        observation.owner_stalled =
            "ROS transport owner " + stalled->owner + " stalled in " + stalled->task;
        return observation;
      }
    }
  }
  std::function<core::Result<std::optional<std::string>>()> process_health;
  std::function<std::optional<std::string>()> guard_fault;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  ros2_control_execution::Ros2GripperCommandBackend *gripper{};
  std::optional<std::string> gripper_id;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> recovery_marker;
  std::function<bool(std::uint64_t, std::uint64_t)> recovery_qualified;
  std::string pending_recovery_fault;
  bool retired_late_cancel_unknown = false;
  bool cm_controller = false;
  core::IClock *clock{};
  {
    std::lock_guard lock(mutex_);
    if (!arm_ && !gripper_only_) return observation;
    clock = clock_;
    process_health = process_health_source_;
    recovery_marker = owner_recovery_marker_;
    recovery_qualified = owner_recovery_qualified_;
    pending_recovery_fault = owner_recovery_pending_fault_;
    retired_late_cancel_unknown = !pending_late_cancel_handoff_.empty();
    if (resources_) {
      if (const auto failed = resources_->executor_fault())
        observation.process_fault = "ROS transport executor thread failed: " + *failed +
                                    " (owner " + resources_->owner_name() + ")";
    }
    if (!observation.process_fault && arm_ && arm_->epoch_resources) {
      if (const auto failed = arm_->epoch_resources->executor_fault())
        observation.process_fault = "ROS transport executor thread failed: " + *failed +
                                    " (owner " + arm_->epoch_resources->owner_name() + ")";
    }
    if (guard_hooks_) guard_fault = guard_hooks_->fault;
    if (tool_context_fault_) observation.tool_context_fault = tool_context_fault_->message;
    gripper = gripper_.get();
    gripper_id = gripper_id_;
    const auto &activity = arm_ ? arm_->activity : gripper_activity_;
    cm_controller = arm_ || gripper_only_->physical_controller_required;
    observation.controller_manager_present = activity.controller_manager_present;
    observation.observed = activity.observed;
    observation.controller_loaded = activity.controller_loaded;
    observation.hardware_active = activity.hardware_active;
    observation.controller_active = activity.controller_active;
    if (arm_) {
      adapter = arm_->adapter;
      observation.reset_in_progress = arm_->reset_position_pending;
    } else {
      observation.local_owner_enabled =
          gripper_requested_ == core::ComponentLifecycleStateDTO::kActive;
    }
  }
  if (!pending_recovery_fault.empty()) {
    const bool qualified = recovery_marker && recovery_qualified &&
        recovery_qualified(recovery_marker->first, recovery_marker->second);
    if (qualified) {
      std::lock_guard lock(mutex_);
      if (owner_recovery_marker_ == recovery_marker &&
          owner_recovery_pending_fault_ == pending_recovery_fault) {
        owner_recovery_marker_.reset();
        owner_recovery_pending_fault_.clear();
      } else if (!owner_recovery_pending_fault_.empty() && !observation.process_fault) {
        observation.process_fault = owner_recovery_pending_fault_;
      }
    } else if (!observation.process_fault) {
      observation.process_fault = pending_recovery_fault;
    }
  }
  // The exact launch owner's cached exit, never its advancement source.
  if (!observation.process_fault && process_health) {
    auto exited = process_health();
    if (!exited.ok())
      observation.process_fault = exited.error().message;
    else if (exited.value())
      observation.process_fault = *exited.value();
  }
  if (guard_fault) observation.guard_fault = guard_fault();
  // A second publisher on the component's controller-manager or
  // controller topics (another instance) is the component's fault.
  // An endpoint not yet established is no fault.
  if (auto endpoint = ReadEndpointDiagnostic();
      endpoint && endpoint->code == "duplicate_component_instance")
    observation.guard_fault = endpoint->message;
  if (adapter) {
    const auto goals = adapter->ReadGoalActivity();
    if (goals.late_cancel_not_issued) {
      if (!observation.guard_fault)
        observation.guard_fault = "A required late goal cancel could not be scheduled";
      observation.goal_outcome_unknown = true;
    }
    observation.goal_running = goals.goal_running;
    if (goals.expected_end && clock != nullptr)
      observation.goal_past_end_ns = clock->now() - *goals.expected_end;
    if (goals.last_cancelled) {
      // JTC reports a cancelled goal finished before it brakes; its own
      // reference shows the braking.
      const auto sample = adapter->ReadSample();
      observation.decelerating =
          sample && std::any_of(sample->reference_velocities_rad_s.begin(),
                                sample->reference_velocities_rad_s.end(),
                                [](const double velocity) { return velocity != 0.0; });
    }
  }
  if (gripper && gripper_id) {
    if (!cm_controller) {
      observation.local_owner_ready = gripper->cached_gripper_local_owner_ready(*gripper_id);
    }
    observation.goal_running =
        observation.goal_running ||
        gripper->get_gripper_status(*gripper_id).state == core::GripperCommandState::kExecuting;
    if (gripper->gripper_late_cancel_failed()) {
      observation.goal_outcome_unknown = true;
      if (!observation.guard_fault)
        observation.guard_fault = "A required gripper goal cancel remains unresolved";
    }
  }
  if (retired_late_cancel_unknown) {
    observation.goal_outcome_unknown = true;
    if (!observation.guard_fault)
      observation.guard_fault = "A required old-epoch goal cancel remains unresolved";
  }
  return observation;
}

std::optional<ros2_control_execution::Ros2GripperTransportReadinessSnapshot>
RetainedRos2ControlRuntimeHandle::gripper_transport_readiness_snapshot() const {
  ros2_control_execution::Ros2GripperCommandBackend *gripper = nullptr;
  std::optional<std::string> gripper_id;
  {
    std::lock_guard lock(mutex_);
    gripper = gripper_.get();
    gripper_id = gripper_id_;
  }
  if (!gripper || !gripper_id.has_value()) {
    return std::nullopt;
  }
  return gripper->gripper_transport_readiness(*gripper_id);
}

} // namespace manyforge::roboplan_backend
