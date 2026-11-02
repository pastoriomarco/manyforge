#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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

using ActivityMessage = controller_manager_msgs::msg::ControllerManagerActivity;
using SwitchService = controller_manager_msgs::srv::SwitchController;
using HardwareStateService =
    controller_manager_msgs::srv::SetHardwareComponentState;

// M6 XADAPT (D8): every call a Real arm sends to its controller manager (a
// switch, a hardware-state request, a status query) is allowed
// min(this shared cap, the step's remaining time). No answer within it is
// the connection lost (`controller_manager_unresponsive` and the fault). 10 s
// until measured live.
constexpr std::chrono::nanoseconds kComponentCallResponseCap{
    core::kComponentCallResponseCapNs};

// M6 XADAPT: the time one call is allowed: min(the cap, the remaining time).
[[nodiscard]] std::chrono::steady_clock::time_point
allowed_until(const std::chrono::steady_clock::time_point now,
              const std::optional<std::chrono::steady_clock::time_point> step_deadline) {
  return std::min(now + kComponentCallResponseCap,
                  step_deadline.value_or(std::chrono::steady_clock::time_point::max()));
}

// M6 XADAPT: one INFO line per sent controller-manager call with its
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

// Ordinary CCLC route publication changes the route generation of the same
// controller occurrence. Product resealing itself stays strict. Compare every
// physical fact with that one publication field held at its predecessor value;
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

// SX-695: no time left at the send refuses a request before dispatch.
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

/// CP9: the streams one bound component instance owns on its measured lane,
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
/// (Q43) on every motion command. Readings and Cancel/SetScale pass through.
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
         const core::TrajectoryDTO &trajectory) override {
    if (const auto refused = Refusal()) return *refused;
    auto renamed = Rename(trajectory);
    if (!renamed) return InvalidJoints();
    return adapter_->Submit(key, *renamed);
  }

  core::ExecutionPortCommandResultDTO
  Replace(const core::ExecutionGoalKey &active_key,
          const core::ExecutionGoalKey &next_key,
          const core::TrajectoryDTO &trajectory) override {
    if (const auto refused = Refusal()) return *refused;
    auto renamed = Rename(trajectory);
    if (!renamed) return InvalidJoints();
    return adapter_->Replace(active_key, next_key, *renamed);
  }

  core::ExecutionPortCommandResultDTO
  ReplaceScheduled(const core::ExecutionGoalKey &active_key,
                   const core::ExecutionGoalKey &next_key,
                   const core::TrajectoryDTO &trajectory,
                   const core::ExecutionReplacementStartDTO &start) override {
    if (const auto refused = Refusal())
      return *refused;
    auto renamed = Rename(trajectory);
    if (!renamed)
      return InvalidJoints();
    return adapter_->ReplaceScheduled(active_key, next_key, *renamed, start);
  }

  core::ExecutionPortCommandResultDTO
  Cancel(const core::ExecutionGoalKey &key) override {
    return adapter_->Cancel(key);
  }

  core::ExecutionPortCommandResultDTO SetScale(double value) override {
    return adapter_->SetScale(value);
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
  /// The current binding epoch and its adapter instance.
  std::uint64_t epoch{1U};
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  std::shared_ptr<ArmPort> port;
  /// An activation succeeded (or may have) on the current epoch: the next
  /// activation is a reactivation and needs the next epoch.
  bool epoch_used{false};
  /// A new epoch's port the participant has not accepted yet.
  bool rebind_pending{false};
  /// This host's CCLC activated the controller on the current epoch and the
  /// controller manager has not reported it inactive since it reported it
  /// active.
  bool bound{false};
  bool active_seen_while_bound{false};
  /// Enable's own-client discovery postcondition is established once per
  /// adapter epoch. Later graph loss does not change binding/readiness facts.
  bool activation_client_discovered{false};
  /// The lifecycle state this host's CCLC primitives established.
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
  /// Q54: the controller manager's statistics for this controller.
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
    std::lock_guard lock(mutex);
    active = false;
    subscription.reset();
  }
  std::mutex mutex;
  bool active{false};
  const std::vector<core::MeasuredStateSourceDescriptor> descriptors;
  const std::shared_ptr<core::IClock> clock;
  const rclcpp::Node::SharedPtr node;
  const std::shared_ptr<RetainedObservationWake> observation_wake;
  const std::shared_ptr<RetainedObservationWake> render_wake;
  /// CP9 liveness of the component's measured joints (may be null).
  const std::shared_ptr<core::StreamRecorder> stream;
  /// CP9 liveness of a Mock gripper: samples carrying `gripper_joint`
  /// (may be null).
  const std::shared_ptr<core::StreamRecorder> gripper_stream;
  const std::string gripper_joint;
  std::vector<Channel> channels;
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
  std::lock_guard lock(mutex_);
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
  // CP9: every staged subscription records the streams declared at bind.
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
    const std::weak_ptr<PreparedMeasuredStateSources::Storage> weak_channels =
        replacement;
    // Keep only the scalar's exact facts, never a handle/executor owner whose
    // last release could drain this callback's own executor.
    const auto activity_endpoints =
        gripper_only_ ? gripper_only_->activity_endpoints : nullptr;
    try {
      replacement->subscription = replacement->node->create_subscription<
          sensor_msgs::msg::JointState>(
          lane.absolute_topic, rclcpp::SensorDataQoS().keep_last(1),
          [weak_channels, activity_endpoints](
              const sensor_msgs::msg::JointState::ConstSharedPtr message) {
            const auto channels = weak_channels.lock();
            if (!channels)
              return;
            // CP9 liveness: lock-free, no allocation.
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
          });
    } catch (const std::exception &error) {
      // A valid commanded route must not gain a measured transport admission
      // rule. Keep the actual operational failure visible at its producer.
      RCLCPP_WARN(replacement->node->get_logger(), "Measured state subscription unavailable: %s", error.what());
      return core::Result<Sources>::Ok(nullptr);
    }
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
  ~ExternalOwner() { handle->StopAndQuiesce(); }
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

void RetainedRos2ControlRuntimeHandle::StopAndQuiesce() noexcept {
  // The lane guard stops first (it calls back into this handle). Children
  // then close admission while callbacks and borrowed resources are
  // quiesced; repeated calls repeat only their idempotent local shutdown.
  std::optional<RetainedRos2ControlArmGuardHooks> hooks;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  {
    std::lock_guard lock(mutex_);
    hooks = std::move(guard_hooks_);
    guard_hooks_.reset();
    if (arm_)
      adapter = arm_->adapter;
  }
  // A guard worker in a poll holds an internal lease; joining it here, while
  // this owner still holds one, keeps that poll from being the last owner.
  if (hooks && hooks->stop)
    hooks->stop();
  hooks.reset();
  if (adapter)
    adapter->shutdown();
  if (gripper_)
    gripper_->shutdown();
  if (resources_)
    resources_->quiesce();
}

RetainedRos2ControlRuntimeHandle::~RetainedRos2ControlRuntimeHandle() {
  StopAndQuiesce();
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
    std::unique_ptr<ros2_control_execution::Ros2TransportResources> resources,
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
  std::lock_guard lock(mutex_);
  if (resources_ || arm_ || gripper_only_)
    return reject_owned("ros2_control_handle_already_bound", "ROS runtime handle is already bound");
  auto streams = declare_bound_component_streams(owner->measured_lane, binding, binding);
  if (!streams.ok()) {
    if (gripper) gripper->shutdown();
    if (resources) resources->quiesce();
    return core::Result<void>::Err(streams.error());
  }
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
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::BindProcessOnly(
    std::unique_ptr<ros2_control_execution::Ros2TransportResources> resources,
    core::IClock &clock, RetainedRos2ControlArmController controller,
    std::shared_ptr<core::IClock> clock_owner) {
  if (!resources || controller.controller_manager.empty() ||
      controller.hardware_component_name.empty() || !controller.controller_name.empty() ||
      (clock_owner && clock_owner.get() != &clock)) {
    if (resources) resources->quiesce();
    return reject<void>("ros2_control_process_only_binding_invalid",
                        "A machine's process requires its controller manager, hardware and clock");
  }
  // The no-arm state of a gripper-only binding, without a gripper: its
  // activity subscription observes the controller manager and the hardware.
  auto owner = std::make_shared<GripperOnlyState>();
  owner->controller = std::move(controller);
  owner->activity_endpoints = std::make_shared<ros2_control_execution::SelectedRosEndpointFacts>(
      resources->node(), owner->controller.controller_manager + "/activity");
  std::lock_guard lock(mutex_);
  if (resources_ || arm_ || gripper_only_) {
    resources->quiesce();
    return reject<void>("ros2_control_handle_already_bound", "ROS runtime handle is already bound");
  }
  clock_owner_ = std::move(clock_owner);
  clock_ = &clock;
  resources_ = std::move(resources);
  gripper_only_ = std::move(owner);
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::Bind(
    std::unique_ptr<ros2_control_execution::Ros2TransportResources> resources,
    core::IClock &clock, RetainedRos2ControlArmTransport arm,
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
  if (clock_owner && clock_owner.get() != &clock)
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
    std::lock_guard lock(mutex_);
    if (arm_ || motion_domain_id_.has_value()) {
      return reject_owned("ros2_control_handle_already_bound",
                          "ROS runtime handle is already bound");
    }
  }

  auto state = std::make_shared<ArmState>();
  state->transport = std::move(arm);
  state->epoch = 1U;
  state->transport.adapter.binding_epoch =
      core::ExecutionBindingEpoch(state->epoch);
  std::shared_ptr<core::ExecutionPortParticipant> participant;
  try {
    state->adapter =
        std::make_shared<ros2_control_execution::Ros2ControlExecutionBackend>(
            *resources, clock, state->transport.adapter);
    state->port = std::make_shared<ArmPort>(
        state->adapter, state->transport.compiled_joint_names,
        state->transport.adapter.lane_facts.joint_names, state->gate,
        state->reset_motion_admitted);
    participant = make_participant(state->port);
  } catch (const std::exception &error) {
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

  std::lock_guard lock(mutex_);
  measured_stream_ = std::move(streams.value().measured);
  gripper_feedback_stream_ = std::move(streams.value().gripper_feedback);
  clock_owner_ = std::move(clock_owner);
  resources_ = std::move(resources);
  clock_ = &clock;
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

void RetainedRos2ControlRuntimeHandle::InstallDisplayJointSource(
    const DisplayJointSource &source, std::shared_ptr<RetainedObservedStateChannel> channel) {
  std::lock_guard lock(mutex_);
  if (display_joint_subscription_ || source.topic.empty() || !source.use || !arm_ ||
      !arm_->transport.measured_joint_state_lane || !motion_binding_ || !channel ||
      channel->binding().value() != motion_binding_->value() || resources_ == nullptr)
    throw std::logic_error("ROS arm display joint source is installed once, on a measured arm");
  channel->follow_display_input();
  display_joint_subscription_ =
      resources_->node()->create_subscription<sensor_msgs::msg::JointState>(
          source.topic, rclcpp::SensorDataQoS().keep_last(1),
          [weak = weak_from_this(), weak_channel = std::weak_ptr(channel), use = source.use,
           clock = clock_](const sensor_msgs::msg::JointState::ConstSharedPtr message) {
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

void RetainedRos2ControlRuntimeHandle::InstallCommittedSceneSource(
    CommittedSceneSourceFn source) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || committed_scene_source_ || !source)
    throw std::logic_error("committed Scene source must be installed before start");
  committed_scene_source_ = std::move(source);
}

void RetainedRos2ControlRuntimeHandle::InstallActiveTcp(
    core::ControllableComponentId robot, std::vector<RobotTcp> rows,
    const bool controller_tcp_offset_sync,
    std::optional<core::ProgramBindingId> direct_control_binding) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || active_tcp_robot_ || robot.empty())
    throw std::logic_error("ROS arm Active TCP is installed once, before start");
  active_tcp_robot_ = std::move(robot);
  active_tcp_ = ActiveTcp(std::move(rows));
  active_tcp_offset_sync_ = controller_tcp_offset_sync;
  active_tcp_direct_binding_ = std::move(direct_control_binding);
  // A greater registration generation at every install (runtime start or
  // restart) in this host process; its sequence starts at 0.
  active_tcp_registration_generation_ = next_active_tcp_registration();
  active_tcp_sequence_ = 0U;
}

bool RetainedRos2ControlRuntimeHandle::HasToolContext() const {
  std::lock_guard lock(mutex_);
  return tool_context_hook_.has_value();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RebindRetainedToolContext(
    std::vector<RobotTcp> rows,
    std::optional<core::ProgramBindingId> direct_control_binding,
    RetainedToolContextSourceFn source, CommittedToolContextSourceFn committed_source,
    std::function<std::optional<core::Pose3DTO>(const std::string &)> fixed_pose,
    CommittedSceneSourceFn scene_source, CommittedToolContextSourceFn enable_source) {
  std::lock_guard switch_lock(switch_mutex_);
  std::optional<RobotTcp> previous;
  std::optional<RobotTcp> selected;
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
    ActiveTcp candidate(std::move(rows));
    unchanged_no_tcp = !previous && !candidate.value();
    reset = !unchanged_no_tcp && (!previous || candidate.find(previous->entry) == nullptr);
    if (!reset && previous) candidate.commit(previous->entry);
    selected = candidate.value();
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
          *active_tcp_robot_, previous ? std::optional(previous->entry) : std::nullopt,
          selected ? std::optional(selected->entry) : std::nullopt,
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
          active_tcp_sequence_};
}

ActiveTcp RetainedRos2ControlRuntimeHandle::ReadRobotTcps() const {
  std::lock_guard lock(mutex_);
  return active_tcp_;
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
  // T-N1: sent and not confirmed: the controller may differ from the Active
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
    // T-C2e: with the checkbox off a TCP change is ManyForge's selection only
    // (the controller keeps a zero offset, sent at Enable).
    if (!active_tcp_offset_sync_) return core::Result<bool>::Ok(false);
    hook = tool_context_hook_;
    source = tool_context_source_;
  }
  if (!hook || !source) return core::Result<bool>::Ok(false);
  core::Pose3DTO offset;  // B-run-1: no TCP, the flange
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
      // T-N1: the offset went out but the robot did not come back: the
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
    const std::optional<core::TcpEntryDTO> &entry,
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
  // The commit and its event, under the handle's lock (design 2, 4).
  std::lock_guard lock(mutex_);
  const auto previous = active_tcp_.value();
  const bool changed = entry ? active_tcp_.commit(*entry) : active_tcp_.reset();
  if (!changed) return core::Result<void>::Ok();
  ++active_tcp_sequence_;
  if (!active_tcp_events_ || !active_tcp_robot_) return core::Result<void>::Ok();
  active_tcp_events_(core::ActiveTcpChangeDTO{
      *active_tcp_robot_, previous ? std::optional(previous->entry) : std::nullopt,
      active_tcp_.value() ? std::optional(active_tcp_.value()->entry) : std::nullopt, source,
      offset_sent, active_tcp_registration_generation_, active_tcp_sequence_, 0});
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::SetActiveTcp(
    const core::TcpEntryDTO &entry, const core::ActiveTcpChangeSource source) {
  // Serialized with the lifecycle switches (an Enable re-syncs this value).
  std::lock_guard switch_lock(switch_mutex_);
  std::string frame;
  bool enabled = false;
  {
    std::lock_guard lock(mutex_);
    const auto *row = active_tcp_.find(entry);
    if (row == nullptr)
      return core::Result<void>::Err(core::active_tcp_unknown_error(
          active_tcp_robot_.value_or(core::ControllableComponentId("")), entry));
    if (active_tcp_.value() && active_tcp_.value()->entry == entry)
      return core::Result<void>::Ok();
    frame = row->frame.compiled_frame_name;
    // A2a-1: not enabled, the controller takes it at the next Enable.
    enabled = arm_ && arm_->requested == core::ComponentLifecycleStateDTO::kActive;
  }
  return ChangeActiveTcp(entry, frame, source, enabled, false);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyMotionToolContext(
    const core::TcpEntryDTO &entry,
    const std::optional<core::ProgramBindingId> &program_binding) {
  // Serialized with the lifecycle switches, as the re-engage.
  std::lock_guard switch_lock(switch_mutex_);
  std::string frame;
  auto source = core::ActiveTcpChangeSource::kProgram;
  {
    std::lock_guard lock(mutex_);
    const auto *row = active_tcp_.find(entry);
    if (row == nullptr)
      return core::Result<void>::Err(core::active_tcp_unknown_error(
          active_tcp_robot_.value_or(core::ControllableComponentId("")), entry));
    // No event, no step (design 3.1): the move uses the Active TCP.
    if (active_tcp_.value() && active_tcp_.value()->entry == entry)
      return core::Result<void>::Ok();
    frame = row->frame.compiled_frame_name;
    if (program_binding && program_binding == active_tcp_direct_binding_)
      source = core::ActiveTcpChangeSource::kRobotControl;
  }
  // An admitted move: the robot is enabled and at rest.
  return ChangeActiveTcp(entry, frame, source, true, false);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ResetActiveTcp() {
  std::lock_guard switch_lock(switch_mutex_);
  std::optional<std::string> frame;
  bool enabled = false;
  {
    std::lock_guard lock(mutex_);
    const auto &rows = active_tcp_.rows();
    const auto end_effector = std::find_if(rows.begin(), rows.end(),
                                           [](const RobotTcp &row) { return row.end_effector; });
    const bool none = end_effector == rows.end();
    if (none ? !active_tcp_.value()
             : active_tcp_.value() && active_tcp_.value()->entry == end_effector->entry)
      return core::Result<void>::Ok();
    if (!none) frame = end_effector->frame.compiled_frame_name;
    enabled = arm_ && arm_->requested == core::ComponentLifecycleStateDTO::kActive;
  }
  // T-MQ9: told at once when it can be; T-MQ9b: otherwise the reset still
  // completes and the next Enable re-syncs.
  return ChangeActiveTcp(std::nullopt, frame, core::ActiveTcpChangeSource::kReset, enabled,
                         true);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyCommittedToolPayload(
    const CommittedSceneSourceFn &read) {
  std::lock_guard switch_lock(switch_mutex_);
  return ApplyCommittedToolPayloadLocked(read);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyLatestCommittedToolPayload() {
  std::lock_guard switch_lock(switch_mutex_);
  CommittedSceneSourceFn read;
  {
    std::lock_guard lock(mutex_);
    if (!tool_context_hook_ && !committed_scene_source_)
      return core::Result<void>::Ok();
    read = committed_scene_source_;
  }
  return ApplyCommittedToolPayloadLocked(read);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ApplyCommittedToolPayloadLocked(
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
  // Serialized with the lifecycle switches.
  std::lock_guard switch_lock(switch_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (!rearm_rebind_required_) return core::Result<void>::Ok();
  }
  return RebindAfterRearm(hook->rearm_bound);
}

core::Result<void>
RetainedRos2ControlRuntimeHandle::RebindAfterRearm(const std::chrono::nanoseconds bound) {
  // Caller holds `switch_mutex_`. Required until the rebind completes.
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
    std::lock_guard lock(mutex_);
    const auto rotated = RotateEpochLocked();
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
  if (start_failure_ != nullptr)
    std::rethrow_exception(start_failure_);
  if (start_attempted_)
    return;
  if ((!arm_ && !gripper_only_) || resources_ == nullptr)
    throw std::logic_error("ROS initial installation has no live backend");
  start_attempted_ = true;
  try {
    if (gripper_)
      gripper_->complete_ros_configuration();
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
    // Q54 (DX-136): one subscription to the controller manager's statistics;
    // observed values only, a throttled warning, no execution authority.
    arm_->statistics_monitor = std::make_shared<RetainedControllerStatisticsMonitor>(
        RetainedControllerStatisticsConfig{
            controller.controller_manager, controller.controller_name,
            arm_->transport.adapter.lane_facts.controller_update_rate_hz},
        *clock_, [logger = node->get_logger()](const std::string &text) {
          RCLCPP_WARN(logger, "%s", text.c_str());
        });
    arm_->statistics_subscription =
        RetainedControllerStatisticsMonitor::Subscribe(node, arm_->statistics_monitor);
    if (measured_state_observer_) {
      // The observer runs on this executor; it holds only the channel and
      // sink it publishes to, never this handle.
      measured_state_subscription_ =
          node->create_subscription<sensor_msgs::msg::JointState>(
              arm_->transport.measured_joint_state_lane->absolute_topic,
              rclcpp::SensorDataQoS().keep_last(1),
              [observer = measured_state_observer_](
                  const sensor_msgs::msg::JointState::ConstSharedPtr message) {
                observer(*message);
              });
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
    // only a new CCLC activation, on a new epoch, binds the controller again.
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
  // accepted Reset claim/CCLC scope and ArmPort's pending gate separately
  // prevent motion until exact post-effect readback has completed.
  readiness.controller_bound = arm_->bound;
  readiness.hardware_active =
      activity.controller_manager_present && activity.hardware_active;
  arm_->adapter->ObserveReadiness(readiness);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RotateEpochLocked() {
  if (arm_->epoch_used) {
    if (arm_->epoch == std::numeric_limits<std::uint64_t>::max()) {
      return reject<void>("ros2_control_binding_epoch_exhausted",
                          "ROS controller binding epoch is exhausted");
    }
    // §3.1 / port contract: a reactivated controller is a new binding epoch
    // served by a new adapter instance. Retire the old one first: admission
    // closes, its goals end unknown and late callbacks or samples are ignored.
    // shutdown() detaches and releases the instance's SDK entities (in-flight
    // SDK callbacks keep their own ownership; its closed state blocks late
    // effects), so the retired instance is released once Core drops it.
    arm_->adapter->shutdown();
    auto config = arm_->transport.adapter;
    config.binding_epoch = core::ExecutionBindingEpoch(arm_->epoch + 1U);
    auto adapter =
        std::make_shared<ros2_control_execution::Ros2ControlExecutionBackend>(
            *resources_, *clock_, std::move(config));
    ++arm_->epoch;
    arm_->adapter = std::move(adapter);
    arm_->activation_client_discovered = false;
    arm_->port = std::make_shared<ArmPort>(
        arm_->adapter, arm_->transport.compiled_joint_names,
        arm_->transport.adapter.lane_facts.joint_names, arm_->gate,
        arm_->reset_motion_admitted);
    arm_->epoch_used = false;
    arm_->bound = false;
    arm_->rebind_pending = true;
    PublishReadinessLocked();
  }
  if (arm_->rebind_pending) {
    // Dispatch and speed values go only through the new port from here on;
    // the participant drops every fact read from the old one.
    const auto rebound = participant_->RebindPort(arm_->port);
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
  const bool activate = !activate_controllers.empty();
  const auto controller =
      activate ? activate_controllers.front() : deactivate_controllers.front();
  {
    std::lock_guard lock(mutex_);
    if ((!arm_ || !arm_->switch_client) && (!gripper_only_ || !gripper_only_->switch_client))
      return not_sent("ros2_control_switch_unavailable",
                      "the controller-manager switch client is not started");
    client = arm_ ? arm_->switch_client : gripper_only_->switch_client;
    node = resources_->node();
  }
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
  auto future = client->async_send_request(request);
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
  // CONTRACTS §6: the switch's result is the reported activity.
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
  std::lock_guard switch_lock(switch_mutex_);
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
  std::lock_guard switch_lock(switch_mutex_);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  std::function<core::Result<void>()> before_activation;
  std::optional<ToolContextHook> hook;
  CommittedToolContextSourceFn committed_tool_context;
  CommittedSceneSourceFn committed_scene;
  std::optional<std::string> active_tcp_frame;
  bool offset_sync = true;
  // M6b (SX-727): the controller took a TCP offset (and was re-armed) in
  // this step.
  bool offset_applied = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !participant_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    // CONTRACTS §2: already enabled by this host (bound and active), the
    // Enable is satisfied; a switch would be refused.
    if (arm_->bound && arm_->activity.observed && arm_->activity.controller_active)
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    if (guard_hooks_.has_value())
      before_activation = guard_hooks_->before_activation;
    hook = tool_context_hook_;
    committed_tool_context = committed_enable_tool_context_source_;
    committed_scene = committed_scene_source_;
    // Active TCP (M7, design 3.3): Enable keeps it and re-syncs it.
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
    // Active TCP (M7, design 3.3): the Enable re-sync at the Active TCP (the
    // flange with no TCP, B-run-1): the hook sends its offset when the
    // controller reports another (zero with the TCP offset sync off), then
    // the total payload (SX-727: as part of this step, its remaining time and
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
  // SX-727: after an applied offset the step had an effect: a switch then
  // refused or not sent is a known failure with that observed effect.
  const auto with_offset_effect = [&](RetainedRos2ControlSwitchResult result) {
    if (offset_applied && (result.outcome == RetainedRos2ControlSwitchOutcome::kNotSent ||
                           result.outcome == RetainedRos2ControlSwitchOutcome::kFailed ||
                           result.outcome == RetainedRos2ControlSwitchOutcome::kFailedNoEffect))
      result.outcome = RetainedRos2ControlSwitchOutcome::kFailedEffectObserved;
    return result;
  };
  // CONTRACTS §2: a controller the activity already reports active (e.g. a
  // vendor driver reactivated it after the tool-context hook's re-arm)
  // satisfies the Enable; it is bound as a fresh epoch with no switch (the
  // controller manager refuses a strict activation of an active controller).
  bool already_active = false;
  {
    std::lock_guard lock(mutex_);
    const auto rotated = RotateEpochLocked();
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
  std::lock_guard switch_lock(switch_mutex_);
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
  std::lock_guard switch_lock(switch_mutex_);
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
  std::string hardware;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !arm_->hardware_state_client)
      return not_sent("ros2_control_hardware_state_unavailable",
                      "the hardware-state client is not started");
    client = arm_->hardware_state_client;
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
  // M6 XADAPT (D8, SX-690/699): allowed min(the call cap, the step's
  // remaining time), checked right at the send; none left, nothing is sent.
  if (auto refused = refuse_if_expired_at_send(step_deadline)) return *refused;
  const auto logger = resources_->node()->get_logger();
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = allowed_until(started, step_deadline);
  auto future = client->async_send_request(request);
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
  // CONTRACTS §6: the transition's result is the reported hardware state.
  if (auto unseen = WaitForReportedActivity({}, {}, active, cancellation, deadline))
    return *unseen;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::DeactivateArmControllerForFault(
    const std::stop_token cancellation) {
  std::lock_guard switch_lock(switch_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    // Containment: the switch below ends the binding; nothing is admitted on
    // it from now on.
    arm_->bound = false;
    PublishReadinessLocked();
  }
  std::optional<std::string> writer;
  {
    std::lock_guard lock(mutex_);
    if (arm_->transport.mock_reset_position &&
        (arm_->reset_writer_possibly_active || arm_->reset_writer_active))
      writer = arm_->transport.mock_reset_position->controller_name;
  }
  if (writer) {
    const auto contained = SendSwitch(false, cancellation, *writer, std::nullopt);
    if (contained.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
      return contained;
    std::lock_guard lock(mutex_);
    arm_->reset_writer_possibly_active = false;
  }
  return SendSwitch(false, cancellation, std::nullopt, std::nullopt);
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
  std::lock_guard switch_lock(switch_mutex_);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  MockResetPositionTarget target;
  std::string controller;
  std::uint64_t predecessor{};
  std::uint64_t expected_successor{};
  std::optional<core::ExecutionBindingEpoch> successor;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr publisher;
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
    // Contract142: the existing one-second budget now starts at actual step
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
    publisher->publish(command); // Exactly one immutable target publication.
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
    std::lock_guard lock(mutex_);
    accepted = RotateEpochLocked();
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
  std::lock_guard switch_lock(switch_mutex_);
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
  // Active TCP (T-N1): Recover ends a tool-context fault; Enable re-syncs.
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
            std::lock_guard lock(mutex_);
            if (gripper_ && gripper_id_ &&
                gripper_->gripper_actuation_kind(*gripper_id_) ==
                    core::GripperActuationKind::kPneumatic &&
                gripper_->gripper_output_mode(*gripper_id_) == core::GripperOutputMode::kNone)
              return std::nullopt;
            if ((!arm_ || !arm_->transport.gripper_controller) && !gripper_only_)
              return std::nullopt;  // refused below, nothing to wait for
            if (gripper_only_ && !gripper_only_->physical_controller_required) {
              // The Isaac command bridge: its command path up (its action
              // server), no controller manager. Its state is a stream that
              // gates commands, never the lifecycle (CP9).
              if (gripper_->gripper_transport_readiness(*gripper_id_).command_path_ready)
                return std::nullopt;
              return RetainedRos2ControlSwitchResult{
                  RetainedRos2ControlSwitchOutcome::kNotSent,
                  core::ErrorDTO{"ros2_control_gripper_bridge_not_ready",
                                 "The exact gripper command bridge is not ready", {}}};
            }
            if (!gripper_activity_.controller_manager_present ||
                !gripper_activity_.observed || !gripper_activity_.controller_loaded ||
                !gripper_activity_.hardware_active)
              return not_sent("ros2_control_gripper_controller_not_loaded",
                              "the exact gripper controller is not loaded on active hardware");
            return std::nullopt;
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
      // controller. CCLC already closed command eligibility under its claim.
      gripper_requested_ = core::ComponentLifecycleStateDTO::kPrepared;
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    }
    if ((!arm_ || !arm_->transport.gripper_controller) && !gripper_only_)
      return not_sent("ros2_control_gripper_controller_unbound", "No exact gripper controller is bound");
  }
  // gripper_only_ is set at bind time and never rebound.
  if (gripper_only_ && !gripper_only_->physical_controller_required)
    return RunGripperBridgeLifecycle(core::ComponentLifecycleStateDTO::kPrepared, cancellation);
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
  std::lock_guard switch_lock(switch_mutex_);
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
                                     cancellation);
  {
    std::lock_guard lock(mutex_);
    controller = arm_ ? arm_->transport.gripper_controller->controller_name
                      : gripper_only_->controller.controller_name;
    // CONTRACTS §2: an already satisfied Enable or Disable succeeds at once
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
    const core::ComponentLifecycleStateDTO target, const std::stop_token cancellation) {
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
    if (target == Lifecycle::kDisabled) return gripper.disable_lifecycle_gripper(id, cancellation);
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

Ros2ControlObservation
RetainedRos2ControlRuntimeHandle::ReadObservation(const core::TimePoint now_ns) const {
  static_cast<void>(now_ns);
  Ros2ControlObservation observation;
  std::function<core::Result<std::optional<std::string>>()> process_health;
  std::function<std::optional<std::string>()> guard_fault;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  ros2_control_execution::Ros2GripperCommandBackend *gripper{};
  std::optional<std::string> gripper_id;
  bool cm_controller = false;
  core::IClock *clock{};
  {
    std::lock_guard lock(mutex_);
    if (!arm_ && !gripper_only_) return observation;
    clock = clock_;
    process_health = process_health_source_;
    if (resources_) {
      if (const auto failed = resources_->executor_fault())
        observation.process_fault = "ROS transport executor thread failed: " + *failed;
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
  // Q80: the exact launch owner's cached exit, never its advancement source.
  if (!observation.process_fault && process_health) {
    auto exited = process_health();
    if (!exited.ok())
      observation.process_fault = exited.error().message;
    else if (exited.value())
      observation.process_fault = *exited.value();
  }
  if (guard_fault) observation.guard_fault = guard_fault();
  // REC-1: a second publisher on the component's controller-manager or
  // controller topics (another instance) is the component's fault.
  // An endpoint not yet established is no fault.
  if (auto endpoint = ReadEndpointDiagnostic();
      endpoint && endpoint->code == "duplicate_component_instance")
    observation.guard_fault = endpoint->message;
  if (adapter) {
    const auto goals = adapter->ReadGoalActivity();
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
      const auto owner = gripper->gripper_transport_readiness(*gripper_id);
      observation.local_owner_ready =
          owner.output_mode == core::GripperOutputMode::kNone
              ? owner.configured && owner.logical_owner_ready
              // The command path; the state is a stream (CP9), not lifecycle.
              : owner.command_path_ready;
    }
    observation.goal_running =
        observation.goal_running ||
        gripper->get_gripper_status(*gripper_id).state == core::GripperCommandState::kExecuting;
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
