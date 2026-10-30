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
#include <controller_manager_msgs/srv/list_controllers.hpp>
#include <controller_manager_msgs/srv/list_hardware_components.hpp>
#include <controller_manager_msgs/srv/set_hardware_component_state.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_core/time/received_sample_age.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
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
using ListControllersService = controller_manager_msgs::srv::ListControllers;
using ListHardwareService = controller_manager_msgs::srv::ListHardwareComponents;

// R7 package 4: the generic hardware primitive pair of a Real arm, and the
// facts its motion domain adds to the shared ros2_control facts (the Real
// catalog's predicates; Mock and Isaac never report them).
constexpr std::string_view kHardwareActivatePrimitiveId =
    "ros2_control.hardware.activate";
constexpr std::string_view kHardwareDeactivatePrimitiveId =
    "ros2_control.hardware.deactivate";
// A Real arm's Prepare re-activates the joint state broadcaster on its own
// when the vendor driver's reactivation left it off (strict switch).
constexpr std::string_view kBroadcasterActivatePrimitiveId =
    "ros2_control.broadcaster.activate";
// V-B2 §2.4 (V2-3 neutral name): Enable's tool-context re-sync, a nested
// substep of `ros2_control.controller.activate`.
constexpr std::string_view kToolContextSyncSubstepId = "device.tool_context.sync";
constexpr std::string_view kBroadcasterActiveFactId =
    "external.broadcaster_active";
constexpr std::string_view kControllerActiveFactId =
    "external.controller_active";
constexpr std::string_view kControllerSpawnedFactId =
    "external.controller_spawned";
constexpr std::string_view kHardwareActiveFactId = "external.hardware_active";
constexpr std::string_view kMeasuredJointsValidFactId =
    "external.measured_joints_valid";
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

// M6 XADAPT: one bounded state query; none when it is not sent (`not_sent`
// says why: SX-695 the service not discovered, SX-690 no time left; never a
// fault) or sent and not answered in time.
template <typename Service>
[[nodiscard]] typename Service::Response::SharedPtr
query_once(const typename rclcpp::Client<Service>::SharedPtr &client,
           const std::optional<std::chrono::steady_clock::time_point> wait_until,
           const std::stop_token cancellation, const rclcpp::Logger &logger,
           const char *call, std::optional<core::ErrorDTO> &not_sent_error) {
  if (!client || !client->service_is_ready()) {
    not_sent_error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "the controller manager's list_controllers or list_hardware_components "
        "service is not available",
        {{"reason", "ros2_control_state_query_service_unavailable"}}};
    return nullptr;
  }
  const auto started = std::chrono::steady_clock::now();
  if (wait_until && started >= *wait_until) {
    not_sent_error = core::ErrorDTO{"component_lifecycle_deadline_expired",
                                    "the lifecycle deadline expired before dispatch",
                                    {{"reason", "deadline_exhausted"}}};
    return nullptr;
  }
  auto future = client->async_send_request(std::make_shared<typename Service::Request>());
  const auto deadline = allowed_until(started, wait_until);
  while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
    if (cancellation.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
      client->remove_pending_request(future);
      log_call(logger, call, cancellation.stop_requested() ? "cancelled" : "timeout", started);
      return nullptr;
    }
  }
  log_call(logger, call, "answered", started);
  return future.get();
}

// M6 XADAPT: vendor-neutral, user-facing: the component did not answer in
// time. `reason` names the call.
[[nodiscard]] core::ErrorDTO controller_manager_unresponsive(std::string reason) {
  return core::ErrorDTO{"controller_manager_unresponsive",
                        "the controller manager did not answer in time",
                        {{"reason", std::move(reason)}}};
}

[[nodiscard]] core::ErrorDTO robot_controller_unresponsive(std::string reason) {
  return core::ErrorDTO{"robot_controller_unresponsive",
                        "the robot controller did not answer in time",
                        {{"reason", std::move(reason)}}};
}
// SX-291/SX-297: a lifecycle controller switch the manager accepts but never
// answers ends as an unknown effect (CCLC reconciles and fences the claim) at
// the end of the transaction's remaining command time; only where none is
// available (a direct call outside CCLC) at this fallback.
constexpr std::chrono::nanoseconds kLifecycleSwitchFallbackBound =
    std::chrono::seconds(10);

[[nodiscard]] std::chrono::steady_clock::time_point
lifecycle_switch_deadline(const std::optional<std::chrono::steady_clock::time_point>
                              &transaction_deadline) {
  return transaction_deadline.value_or(std::chrono::steady_clock::now() +
                                       kLifecycleSwitchFallbackBound);
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

[[nodiscard]] core::LifecycleFactDTO boolean_fact(std::string id,
                                                  const bool value) {
  return {core::LifecycleFactId(std::move(id)),
          {core::LifecycleFactValueKindDTO::kBoolean, value, std::nullopt,
           std::nullopt}};
}

[[nodiscard]] core::LifecycleFactDTO
cause_fact(const core::LifecycleBlockingCauseClassDTO cause) {
  return {core::LifecycleFactId(
              std::string(core::kLifecycleBlockingCauseFactId)),
          {core::LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
           std::string(core::to_string(cause))}};
}

// Fact ids are sorted: every `external.*` id precedes the standard
// `manyforge.blocking_cause_class` id. A Real arm's vendor device facts
// (`device.*`, `manyforge.vendor_code.*`) are merged into the same order.
[[nodiscard]] std::vector<core::LifecycleFactDTO>
lifecycle_facts(const bool controller_ready,
                const bool controller_inactive_unrequested,
                const bool controller_loaded, const bool fault_present,
                const bool transport_ready,
                const core::LifecycleBlockingCauseClassDTO cause,
                const std::vector<core::LifecycleFactDTO> &device_facts = {}) {
  std::vector<core::LifecycleFactDTO> result{
      boolean_fact("external.controller_configuration_ready", controller_ready),
      // Diagnostic only: the trajectory controller is inactive although this
      // host's CCLC did not deactivate it after it was once active.
      boolean_fact("external.controller_inactive_unrequested",
                   controller_inactive_unrequested),
      boolean_fact("external.controller_loaded", controller_loaded),
      boolean_fact("external.fault_present", fault_present),
      boolean_fact("external.transport_ready", transport_ready),
      cause_fact(cause),
  };
  if (!device_facts.empty()) {
    result.insert(result.end(), device_facts.begin(), device_facts.end());
    std::sort(result.begin(), result.end(),
              [](const auto &left, const auto &right) {
                return left.lifecycle_fact_id < right.lifecycle_fact_id;
              });
  }
  return result;
}

// Changes whenever the vendor status changes (publication generation).
[[nodiscard]] std::string
device_status_signature(const RetainedDeviceStatus &status) {
  std::ostringstream stream;
  stream << ":device:" << core::to_string(status.cause);
  for (const auto &fact : status.facts) {
    stream << ':' << fact.lifecycle_fact_id.value() << '='
           << static_cast<int>(fact.value.kind) << '/'
           << fact.value.boolean_value.value_or(false) << '/'
           << fact.value.integer_value.value_or(0) << '/'
           << fact.value.token_value.value_or("");
  }
  return stream.str();
}

[[nodiscard]] bool
any_reason(const std::vector<std::string> &reasons,
           const std::initializer_list<std::string_view> candidates) {
  return std::any_of(reasons.begin(), reasons.end(),
                     [&](const std::string &reason) {
                       return std::find(candidates.begin(), candidates.end(),
                                        reason) != candidates.end();
                     });
}

[[nodiscard]] bool
only_reasons(const std::vector<std::string> &reasons,
             const std::initializer_list<std::string_view> candidates) {
  return !reasons.empty() &&
         std::all_of(reasons.begin(), reasons.end(),
                     [&](const std::string &reason) {
                       return std::find(candidates.begin(), candidates.end(),
                                        reason) != candidates.end();
                     });
}

[[nodiscard]] core::LifecycleBlockingCauseClassDTO gripper_transport_cause(
    const ros2_control_execution::Ros2GripperTransportReadinessSnapshot
        &snapshot) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  const auto &reasons = snapshot.reason_codes;
  if (snapshot.ready) {
    return Cause::kNone;
  }
  if (any_reason(reasons, {"gripper_identity_missing",
                           "gripper_transport_not_configured",
                           "gripper_transport_kind_invalid"})) {
    return Cause::kConfiguration;
  }
  if (only_reasons(reasons, {"gripper_action_server_unavailable",
                             "gripper_command_subscriber_unavailable",
                             "gripper_state_feedback_unavailable",
                             "gripper_state_publisher_unavailable",
                             "gripper_state_message_unreceived",
                             "gripper_state_joint_unobserved",
                             "gripper_state_feedback_stale",
                             "gripper_transport_replaced"})) {
    return Cause::kAvailability;
  }
  return Cause::kUnknown;
}

[[nodiscard]] std::string gripper_signature(
    const ros2_control_execution::Ros2GripperTransportReadinessSnapshot
        &snapshot) {
  std::ostringstream stream;
  stream << "gripper:" << snapshot.transport_kind << ':' << snapshot.configured
         << ':' << snapshot.command_path_ready << ':'
         << snapshot.feedback_path_ready << ':' << snapshot.ready;
  for (const auto &reason : snapshot.reason_codes) {
    stream << ':' << reason;
  }
  return stream.str();
}

} // namespace

std::optional<VendorJointDisplaySample>
vendor_joint_display_sample(const core::RuntimeParticipantBinding &binding,
                            const sensor_msgs::msg::JointState &message) {
  const auto &resources = binding.value().resources;
  if (resources.empty() || message.name.size() != message.position.size())
    return std::nullopt;
  VendorJointDisplaySample sample;
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
  /// R7 package 4: a Real arm's stock `set_hardware_component_state` client.
  rclcpp::Client<HardwareStateService>::SharedPtr hardware_state_client;
  // M6 XADAPT: a Real arm's state queries.
  rclcpp::Client<ListControllersService>::SharedPtr list_controllers_client;
  rclcpp::Client<ListHardwareService>::SharedPtr list_hardware_client;
  rclcpp::Publisher<std_msgs::msg::Float64MultiArray>::SharedPtr
      reset_publisher;
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

// The subscription callback holds only this cache and the handle's clock.
struct RetainedRos2ControlRuntimeHandle::DeviceMeasuredJoints final {
  void Store(const sensor_msgs::msg::JointState &message,
             const core::TimePoint received_ns) {
    std::lock_guard lock(mutex);
    names = message.name;
    positions = message.position;
    velocities = message.velocity;
    received = received_ns;
    stamp_ns = static_cast<std::int64_t>(message.header.stamp.sec) * 1'000'000'000LL +
               message.header.stamp.nanosec;
  }

  mutable std::mutex mutex;
  std::vector<std::string> names;
  std::vector<double> positions;
  /// M6 ADAPTERM: the at-rest check after a re-engage.
  std::vector<double> velocities;
  /// M6 ADAPTERM (SX-582): the sample's own stamp (ROS time); after an own
  /// re-arm only a sample stamped after its start counts, so a message queued
  /// before it never ends the re-arm, whenever it is delivered.
  std::int64_t stamp_ns{};
  std::optional<std::int64_t> not_before_stamp_ns;
  std::optional<core::TimePoint> received;
  /// The handle's last hardware activation plus one freshness budget, on the
  /// receipt clock: an earlier sample may precede the vendor driver's
  /// activation stall, so it never counts as current (live 2026-10-28). A
  /// receipt cutoff with one budget of margin makes a later sample very
  /// likely post-activation; a late-delivered older message still passes.
  std::optional<core::TimePoint> not_before;
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
  if (!measured_stream_) {
    // CP9: one `measured_joints` stream per bound component, kept across
    // restaging; the lane carries the adapter-kind defaults.
    const auto &owner = motion_binding_ ? *motion_binding_ : *gripper_binding_;
    auto declared = host_component_stream_health()->Declare(
        owner.value().controllable_component_id,
        core::StreamDeclarationDTO{"measured_joints", lane.freshness_budget_ns,
                                   lane.stream_severity, lane.stream_recovery,
                                   core::StreamTimestampSourceDTO::kMessageStamp});
    if (!declared.ok())
      return core::Result<Sources>::Err(declared.error());
    measured_stream_ = std::move(declared.value());
  }
  if (lane.gripper_feedback_stream && gripper_binding_ && !gripper_feedback_stream_) {
    auto declared = host_component_stream_health()->Declare(
        gripper_binding_->value().controllable_component_id,
        *lane.gripper_feedback_stream);
    if (!declared.ok())
      return core::Result<Sources>::Err(declared.error());
    gripper_feedback_stream_ = std::move(declared.value());
  }
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
  vendor_joint_display_subscription_.reset();
  measured_state_observer_ = {};
  device_measured_joints_.reset();
  device_status_adapter_.reset();
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
    const std::optional<core::SelectedControllerOccurrenceDTO>& successor_occurrence,
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
  DomainGenerationState generation;
  if (successor_occurrence) {
    if (!successor_occurrence->locally_valid() || successor_occurrence->arm_binding_epoch ||
        successor_occurrence->domains.size() != 1U ||
        successor_occurrence->domains.front().control_domain_id != domain ||
        successor_occurrence->domains.front().observation_generation == 0U)
      return reject_owned("ros2_control_handle_successor_invalid",
                          "Gripper successor requires its exact no-arm domain occurrence");
    generation.publication_generation = successor_occurrence->domains.front().observation_generation;
  }
  auto owner = std::make_shared<GripperOnlyState>();
  owner->controller = std::move(controller);
  owner->physical_controller_required = !no_controller;
  owner->measured_lane = std::move(measured_lane);
  if (owner->physical_controller_required) {
    // StageMeasuredStateSources precedes StartBackend and captures this same
    // facts object without retaining this handle. No subscription starts here.
    owner->activity_endpoints =
        std::make_shared<ros2_control_execution::SelectedRosEndpointFacts>(
            resources->node(), owner->controller.controller_manager + "/activity",
            "controller_manager_msgs/msg/ControllerManagerActivity");
  }
  std::lock_guard lock(mutex_);
  if (resources_ || arm_ || gripper_only_)
    return reject_owned("ros2_control_handle_already_bound", "ROS runtime handle is already bound");
  clock_owner_ = std::move(clock_owner);
  clock_ = &clock;
  resources_ = std::move(resources);
  gripper_ = std::move(gripper);
  gripper_binding_ = binding;
  gripper_domain_id_ = domain;
  gripper_id_ = binding.value().command_target_id.value();
  domain_generations_.emplace(domain, std::move(generation));
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
    const std::optional<core::SelectedControllerOccurrenceDTO>
        &successor_occurrence,
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
  std::map<core::ControlDomainId, DomainGenerationState> domain_generations;
  domain_generations.emplace(motion_domain, DomainGenerationState{});
  if (gripper_domain)
    domain_generations.emplace(*gripper_domain, DomainGenerationState{});
  if (successor_occurrence) {
    if (!successor_occurrence->locally_valid() ||
        !successor_occurrence->arm_binding_epoch ||
        successor_occurrence->domains.size() != domain_generations.size()) {
      return reject_owned(
          "ros2_control_handle_successor_invalid",
          "ROS successor requires the exact arm and domain occurrence");
    }
    for (const auto &domain : successor_occurrence->domains) {
      const auto found = domain_generations.find(domain.control_domain_id);
      if (found == domain_generations.end() ||
          domain.observation_generation == 0U) {
        return reject_owned(
            "ros2_control_handle_successor_invalid",
            "ROS successor requires exact positive domain generations");
      }
      found->second.publication_generation = domain.observation_generation;
    }
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
  state->epoch = successor_occurrence
                     ? successor_occurrence->arm_binding_epoch->value()
                     : 1U;
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

  std::lock_guard lock(mutex_);
  clock_owner_ = std::move(clock_owner);
  resources_ = std::move(resources);
  clock_ = &clock;
  arm_ = std::move(state);
  participant_ = std::move(participant);
  gripper_ = std::move(gripper);
  motion_binding_ = motion_binding;
  gripper_binding_ = gripper_binding;
  domain_generations_ = std::move(domain_generations);
  motion_domain_id_ = motion_domain;
  gripper_domain_id_ = gripper_domain;
  if (gripper_binding.has_value()) {
    gripper_id_ = gripper_binding->value().command_target_id.value();
  }
  return core::Result<void>::Ok();
}

core::SelectedControllerOccurrenceDTO
RetainedRos2ControlRuntimeHandle::ReadOwnedControllerOccurrence() const {
  std::lock_guard lock(mutex_);
  core::SelectedControllerOccurrenceDTO result{std::nullopt, {}};
  if (arm_)
    result.arm_binding_epoch = core::ExecutionBindingEpoch(arm_->epoch);
  for (const auto &[domain, state] : domain_generations_)
    result.domains.push_back({domain, state.publication_generation});
  return result;
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

void RetainedRos2ControlRuntimeHandle::InstallDeviceStatusAdapter(
    std::shared_ptr<IRetainedDeviceStatusAdapter> adapter) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || device_status_adapter_ || !adapter || gripper_only_) {
    throw std::logic_error(
        "ROS arm device status adapter is installed once, before start");
  }
  device_status_adapter_ = std::move(adapter);
}

void RetainedRos2ControlRuntimeHandle::InstallToolContextSource(
    RetainedToolContextSourceFn source) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || tool_context_source_ || !source ||
      !device_status_adapter_) {
    throw std::logic_error(
        "ROS arm tool context source is installed once, before start, with "
        "a vendor adapter");
  }
  tool_context_source_ = std::move(source);
}

void RetainedRos2ControlRuntimeHandle::InstallCommittedToolContextSource(
    CommittedToolContextSourceFn source) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || committed_tool_context_source_ || !source ||
      !device_status_adapter_)
    throw std::logic_error("committed tool context source must be installed before start");
  committed_tool_context_source_ = std::move(source);
}

void RetainedRos2ControlRuntimeHandle::InstallCommittedEnableToolContextSource(
    CommittedToolContextSourceFn source) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || committed_enable_tool_context_source_ || !source || !device_status_adapter_)
    throw std::logic_error("committed Enable tool context source must be installed before start");
  committed_enable_tool_context_source_ = std::move(source);
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

bool RetainedRos2ControlRuntimeHandle::HasToolContextSource() const {
  std::lock_guard lock(mutex_);
  return static_cast<bool>(tool_context_source_);
}

core::Result<void> RetainedRos2ControlRuntimeHandle::RebindRetainedToolContext(
    std::vector<RobotTcp> rows,
    std::optional<core::ProgramBindingId> direct_control_binding,
    RetainedToolContextSourceFn source, CommittedToolContextSourceFn committed_source,
    std::function<std::optional<core::Pose3DTO>(const std::string &)> fixed_pose,
    bool real_vendor_lane, CommittedSceneSourceFn scene_source,
    CommittedToolContextSourceFn enable_source) {
  std::lock_guard switch_lock(switch_mutex_);
  if (real_vendor_lane && (!source || !committed_source || !fixed_pose))
    return core::Result<void>::Err({"retained_tool_context_invalid", "Candidate tool context source is incomplete", {}});
  std::optional<RobotTcp> previous;
  std::optional<RobotTcp> selected;
  bool reset = false;
  bool unchanged_no_tcp = false;
  bool enabled = false;
  bool sync = false;
  std::shared_ptr<IRetainedDeviceStatusAdapter> device;
  std::optional<VendorToolOffsetCommand> last_offset;
  {
    std::lock_guard lock(mutex_);
    if (!active_tcp_robot_)
      return core::Result<void>::Err({"retained_tool_context_invalid", "Retained robot has no Active TCP owner", {}});
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
    device = device_status_adapter_;
    sync = real_vendor_lane && active_tcp_offset_sync_ && device;
    last_offset = last_tool_offset_;
  }
  bool offset_sent = false;
  if (enabled && sync && !unchanged_no_tcp) {
    const auto pose = selected && fixed_pose ? fixed_pose(selected->frame.compiled_frame_name)
                                             : std::optional<core::Pose3DTO>{};
    const std::optional<VendorToolOffsetCommand> wanted = selected
        ? (pose ? std::optional<VendorToolOffsetCommand>(VendorToolOffsetSet{*pose}) : std::nullopt)
        : std::optional<VendorToolOffsetCommand>(VendorToolOffsetZero{});
    if (wanted && (reset || !last_offset || *last_offset != *wanted)) {
      const auto sent = [&]() -> core::Result<bool> {
        try {
          return SendPreparedToolOffset(device, *wanted);
        } catch (const std::exception &error) {
          core::ErrorDTO fault{std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
                               error.what(), {{"reason", "tool_context_send_unconfirmed"}}};
          std::lock_guard lock(mutex_);
          if (!tool_context_fault_) tool_context_fault_ = fault;
          reengage_rebind_required_ = true;
          return core::Result<bool>::Err(std::move(fault));
        } catch (...) {
          core::ErrorDTO fault{std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
                               "Tool context send outcome is unknown",
                               {{"reason", "tool_context_send_unconfirmed"}}};
          std::lock_guard lock(mutex_);
          if (!tool_context_fault_) tool_context_fault_ = fault;
          reengage_rebind_required_ = true;
          return core::Result<bool>::Err(std::move(fault));
        }
      }();
      if (sent.ok()) {
        offset_sent = true;
      } else {
        // The published candidate remains selected, but an enabled
        // controller may keep its predecessor offset until Recover/Enable.
        {
          std::lock_guard lock(mutex_);
          if (!tool_context_fault_) tool_context_fault_ = sent.error();
        }
      }
      // The published selection remains committed even if the send fails.
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

void RetainedRos2ControlRuntimeHandle::ReportToolContextFailure(
    const VendorCallResult &applied) {
  // T-N1: sent and not confirmed: the controller may differ from the Active
  // TCP, a fault only Recover ends (then Enable re-syncs). A pure lost reply
  // with nothing sent is D8's unresponsive mark. A refusal before any send
  // is typed only.
  if (applied.effect_possible) {
    std::lock_guard lock(mutex_);
    if (!tool_context_fault_) tool_context_fault_ = applied.error;
  } else if (applied.unresponsive) {
    MarkUnresponsive(*applied.error);
  }
}

core::Result<bool> RetainedRos2ControlRuntimeHandle::SendToolContextAtRest(
    const std::optional<std::string> &tcp_frame) {
  std::shared_ptr<IRetainedDeviceStatusAdapter> device;
  RetainedToolContextSourceFn source;
  {
    std::lock_guard lock(mutex_);
    // T-C2e: with the checkbox off a TCP change is ManyForge's selection only
    // (the controller keeps a zero offset, sent at Enable).
    if (!active_tcp_offset_sync_) return core::Result<bool>::Ok(false);
    device = device_status_adapter_;
    source = tool_context_source_;
  }
  if (!device || !source) return core::Result<bool>::Ok(false);
  VendorToolContextCommand command;
  if (tcp_frame) {
    const auto context = source(tcp_frame);
    if (!context.ok()) return core::Result<bool>::Err(context.error());
    command.offset = VendorToolOffsetSet{context.value().tcp_pose_in_flange};
  } else {
    command.offset = VendorToolOffsetZero{};  // B-run-1: no TCP, the flange
  }
  return SendPreparedToolOffset(device, command.offset);
}

core::Result<bool> RetainedRos2ControlRuntimeHandle::SendPreparedToolOffset(
    const std::shared_ptr<IRetainedDeviceStatusAdapter> &device,
    const VendorToolOffsetCommand &offset) {
  VendorToolContextCommand command;
  command.offset = offset;
  // Not a lifecycle step: each call the fixed call cap, no cancellation.
  const auto applied = device->SendToolContext(command, false, std::nullopt, std::stop_token{});
  if (applied.error) {
    // A sent call (or its re-arm) may still take effect: the driver then
    // cycles its controllers, so the next motion rebinds first.
    if (applied.effect_possible) {
      std::lock_guard lock(mutex_);
      reengage_rebind_required_ = true;
    }
    ReportToolContextFailure(applied);
    return core::Result<bool>::Err(*applied.error);
  }
  {
    std::lock_guard lock(mutex_);
    last_tool_offset_ = command.offset;
  }
  // M6 ADAPTERM TOOLCTX: the adapter re-armed the robot (xArm: a TCP offset
  // send leaves the controller's ready state); the driver reactivated its
  // controllers. Bind the reactivated controller before any motion goal, as
  // after the re-engage.
  bool rebind = applied.rearmed;
  {
    std::lock_guard lock(mutex_);
    rebind = rebind || reengage_rebind_required_;
  }
  if (rebind) {
    const auto rebound = RebindAfterOwnRearm(*device);
    if (!rebound.ok()) {
      // T-N1: the offset went out but the robot did not come back: the
      // controller differs from the Active TCP until Recover and Enable.
      if (applied.rearmed) {
        std::lock_guard lock(mutex_);
        if (!tool_context_fault_) tool_context_fault_ = rebound.error();
      }
      return core::Result<bool>::Err(rebound.error());
    }
  }
  return core::Result<bool>::Ok(applied.rearmed);
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
    if (!device_status_adapter_ && !committed_scene_source_)
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
  std::shared_ptr<IRetainedDeviceStatusAdapter> device;
  CommittedToolContextSourceFn source;
  std::optional<std::string> frame;
  bool enabled = false;
  {
    std::lock_guard lock(mutex_);
    enabled = arm_ && arm_->requested == core::ComponentLifecycleStateDTO::kActive;
    device = device_status_adapter_;
    source = committed_tool_context_source_;
    if (active_tcp_.value()) frame = active_tcp_.value()->frame.compiled_frame_name;
  }
  if (device && !source) return unavailable("Committed tool context source is unavailable");
  // No Scene was sent while disabled; Enable synchronizes it.
  if (!enabled) return core::Result<void>::Ok();
  if (enabled && device) {
    core::Result<core::ToolContextDTO> context = [&]() {
      try { return source(frame, *scene); }
      catch (const std::exception &error) {
        return core::Result<core::ToolContextDTO>::Err(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kExecutionRejected)), error.what(),
            {{"reason", "tool_payload_composition_failed"}}});
      } catch (...) {
        return core::Result<core::ToolContextDTO>::Err(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
            "Committed tool context composition failed",
            {{"reason", "tool_payload_composition_failed"}}});
      }
    }();
    if (!context.ok()) return fault(context.error());
    const VendorToolLoadSet load{context.value().payload_mass_kg,
                                 context.value().payload_center_of_mass_m};
    bool changed;
    { std::lock_guard lock(mutex_); changed = last_tool_load_ != load; }
    if (changed) {
      VendorCallResult applied;
      try {
        applied = device->SendToolContext({VendorToolOffsetKeep{}, load}, false,
                                          std::nullopt, std::stop_token{});
      } catch (const std::exception &error) {
        // An exception cannot prove whether the controller received the load.
        return fault(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kExecutionRejected)), error.what(),
            {{"reason", "tool_payload_send_unconfirmed"}}});
      } catch (...) {
        return fault(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
            "Committed tool payload send failed",
            {{"reason", "tool_payload_send_unconfirmed"}}});
      }
      if (applied.error) {
        ReportToolContextFailure(applied);
        return fault(*applied.error);
      }
      std::lock_guard lock(mutex_);
      last_tool_load_ = load;
    }
  }
  return core::Result<void>::Ok();
}

core::Result<void> RetainedRos2ControlRuntimeHandle::ReengageMotionAtRest() {
  // A refusal names the cause; the motion fails before it moves.
  const auto failed = [](const core::ErrorDTO &cause) {
    core::ErrorDTO error{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "The robot moved from its own interface was not re-engaged; the "
        "motion did not start: " + cause.message,
        {{"reason", "motion_reengage_failed"}}};
    for (const auto &[name, value] : cause.details)
      error.details[name == "reason" ? "cause" : name] = value;
    return core::Result<void>::Err(std::move(error));
  };
  std::shared_ptr<IRetainedDeviceStatusAdapter> device;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !device_measured_joints_) return core::Result<void>::Ok();
    device = device_status_adapter_;
  }
  if (!device) return core::Result<void>::Ok();
  // Serialized with the lifecycle switches; CCLC work cannot hold the cell
  // while this admitted motion does.
  std::lock_guard switch_lock(switch_mutex_);
  const auto reengaged = device->ReengageAtRest(std::stop_token{});
  if (reengaged.error) {
    // A sent call may still change the robot's mode or state later (and the
    // driver then cycles its controllers): whatever the latch says by then,
    // the next motion re-establishes the binding first.
    if (reengaged.effect_possible) {
      std::lock_guard lock(mutex_);
      reengage_rebind_required_ = true;
    }
    // M6 XADAPT: no answer in time: refused with that code, and the arm
    // reports the component fault, as for a lifecycle step.
    if (reengaged.unresponsive) {
      MarkUnresponsive(*reengaged.error);
      return core::Result<void>::Err(*reengaged.error);
    }
    return failed(*reengaged.error);
  }
  {
    std::lock_guard lock(mutex_);
    if (!reengaged.rearmed && !reengage_rebind_required_)
      return core::Result<void>::Ok();
  }
  const auto rebound = RebindAfterOwnRearm(*device);
  if (!rebound.ok()) return failed(rebound.error());
  return core::Result<void>::Ok();
}

core::Result<void>
RetainedRos2ControlRuntimeHandle::RebindAfterOwnRearm(IRetainedDeviceStatusAdapter &device) {
  // Caller holds `switch_mutex_`. Required until the rebind completes.
  {
    std::lock_guard lock(mutex_);
    reengage_rebind_required_ = true;
  }
  // SX-584: the own re-arm (`device.rearm_in_progress`) ends with this
  // attempt, bound or not; a failure leaves only `reengage_rebind_required_`.
  struct EndRearmOnExit final {
    IRetainedDeviceStatusAdapter &device;
    ~EndRearmOnExit() { device.EndRearm(); }
  } end_rearm{device};
  // The vendor driver reactivates every controller itself (the adapter's
  // re-arm contract) within the adapter's transient bound. Only a broadcaster
  // sample received one freshness budget after now counts (as after an own
  // hardware activation).
  {
    std::lock_guard lock(mutex_);
    const auto not_before =
        clock_->now() + arm_->transport.measured_joint_state_lane->freshness_budget_ns;
    // SX-582: and stamped after now (ROS time of the broadcaster's stamps).
    const auto not_before_stamp = resources_->node()->now().nanoseconds();
    std::lock_guard joints_lock(device_measured_joints_->mutex);
    device_measured_joints_->not_before = not_before;
    device_measured_joints_->not_before_stamp_ns = not_before_stamp;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + device.ActivationTransientBound();
  core::ErrorDTO last = switch_error(
      "ros2_control_reengage_controllers_inactive",
      "the vendor plugin did not reactivate the trajectory controller and the "
      "joint state broadcaster in time; use Recover cell");
  while (true) {
    const auto now = clock_->now();
    bool active = false;
    core::Result<std::vector<double>> measured =
        core::Result<std::vector<double>>::Err(last);
    {
      std::lock_guard lock(mutex_);
      const auto &activity = arm_->activity;
      active = activity.controller_manager_present && activity.observed &&
               activity.hardware_active && activity.controller_active &&
               activity.broadcaster_active;
      measured = MeasuredJointsLocked(now);
    }
    if (active && measured.ok()) {
      // The trajectory controller holds where the robot is: the measured
      // joints after the reactivation agree with the robot's own angles, and
      // the arm is at rest (measured velocities and the vendor's own state).
      const auto at_rest = [&] {
        std::lock_guard lock(mutex_);
        return MeasuredAtRestLocked();
      }();
      const auto gated = at_rest.ok()
                             ? device.CheckReengagedAtRest(now, measured.value())
                             : at_rest;
      if (gated.ok()) break;
      last = gated.error();
    } else if (active) {
      last = measured.error();
    }
    if (std::chrono::steady_clock::now() >= deadline) return core::Result<void>::Err(last);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // The reactivated controller is a new binding epoch, bound as an Enable
  // binds it (no CCLC switch: the vendor driver already activated it).
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  {
    std::lock_guard lock(mutex_);
    const auto rotated = RotateEpochLocked();
    if (!rotated.ok()) return rotated;
    arm_->epoch_used = true;
    arm_->bound = true;
    arm_->active_seen_while_bound = true;
    reengage_rebind_required_ = false;
    PublishReadinessLocked();
    adapter = arm_->adapter;
  }
  while (!adapter->ActionServerIsReady()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return core::Result<void>::Err(switch_error(
          "ros2_control_reengage_action_server_unavailable",
          "the trajectory controller's action server was not discovered "
          "after the re-engage"));
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

core::Result<void> RetainedRos2ControlRuntimeHandle::BindMotionDisplayChannel(
    const std::shared_ptr<RetainedObservedStateChannel>& channel) {
  std::lock_guard lock(mutex_);
  if (start_attempted_ || !arm_ || !motion_binding_ || !channel ||
      channel->binding().value() != motion_binding_->value())
    return reject<void>("ros2_control_motion_display_binding_mismatch",
                        "Display channel must name the current exact arm binding, before start");
  motion_display_channel_ = channel;
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
            node, controller.controller_manager + "/activity",
            "controller_manager_msgs/msg/ControllerManagerActivity");
    const auto endpoint_facts = arm_->activity_endpoints;
    const auto reset_target = arm_->transport.mock_reset_position;
    if (reset_target)
      arm_->reset_publisher =
          node->create_publisher<std_msgs::msg::Float64MultiArray>(
              reset_target->commands_topic,
              rclcpp::QoS(1).reliable().durability_volatile());
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
                  activity.hardware_active = activity.broadcaster_active =
                      false;
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
            if (row.name == kJointStateBroadcasterControllerName)
              activity.broadcaster_active =
                  row.state.id ==
                  lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
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
    if (device_status_adapter_) {
      arm_->hardware_state_client = node->create_client<HardwareStateService>(
          controller.controller_manager + "/set_hardware_component_state");
      arm_->list_controllers_client = node->create_client<ListControllersService>(
          controller.controller_manager + "/list_controllers");
      arm_->list_hardware_client = node->create_client<ListHardwareService>(
          controller.controller_manager + "/list_hardware_components");
    }
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
    if (device_status_adapter_ &&
        arm_->transport.measured_joint_state_lane.has_value())
      device_measured_joints_ = std::make_shared<DeviceMeasuredJoints>();
    if (measured_state_observer_ || device_measured_joints_) {
      // The observer runs on this executor; it holds only the channel and
      // sink it publishes to, never this handle. A Real arm also keeps the
      // latest sample for its Enable gate (R7 §3.6).
      measured_state_subscription_ =
          node->create_subscription<sensor_msgs::msg::JointState>(
              arm_->transport.measured_joint_state_lane->absolute_topic,
              rclcpp::SensorDataQoS().keep_last(1),
              [observer = measured_state_observer_,
               cache = device_measured_joints_, clock = clock_](
                  const sensor_msgs::msg::JointState::ConstSharedPtr message) {
                if (observer)
                  observer(*message);
                if (cache)
                  cache->Store(*message, clock->now());
              });
    }
    // M6: the vendor driver keeps publishing its own joint states in every
    // mode, and the controller stream is the authority: the display always
    // follows it. A well-formed sample (every arm joint named exactly once,
    // finite) that the adapter's gate does not ignore (V-A A4: not a pose,
    // e.g. a vendor e-stop signature) moves only the display; any other
    // keeps the last valid pose. When the gate lets it lead (a move from the
    // robot's own interface, no error, fresh status) it also refreshes the
    // measured stream's liveness, so a dead driver still goes stale, and
    // (item 34) it leads jog/Move-to previews.
    const auto display_channel = motion_display_channel_.lock();
    if (device_status_adapter_ && device_measured_joints_ && display_channel) {
      const auto topic = device_status_adapter_->VendorJointStateTopic();
      auto gate = device_status_adapter_->VendorJointSampleGateFn();
      if (topic) {
        display_channel->follow_display_input();
        vendor_joint_display_subscription_ =
            node->create_subscription<sensor_msgs::msg::JointState>(
                *topic, rclcpp::SensorDataQoS().keep_last(1),
                [channel = motion_display_channel_, gate = std::move(gate),
                 stream = measured_stream_, clock = clock_](
                    const sensor_msgs::msg::JointState::ConstSharedPtr message) {
                  const auto display = channel.lock();
                  if (!display) return;
                  const auto binding = display->binding();
                  const auto sample = vendor_joint_display_sample(binding, *message);
                  if (!sample) return;
                  const auto use = gate ? gate(clock->now(), sample->positions)
                                        : VendorJointSampleUse::kDisplay;
                  if (use == VendorJointSampleUse::kIgnore) return;
                  const bool leads = use == VendorJointSampleUse::kLeads;
                  if (stream && leads)
                    stream->Record(static_cast<std::int64_t>(message->header.stamp.sec) *
                                       1'000'000'000LL +
                                   message->header.stamp.nanosec);
                  if (display->update_render_positions(
                          binding, sample->compiled_names, sample->positions).ok())
                    display->set_display_input_leads(leads);
                });
      }
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
            // present. It consumes graph edges, including unmatched QoS
            // endpoints, without a separate poller or a graph call in Core.
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
    if (device_status_adapter_)
      device_status_adapter_->Start(*node, *clock_);
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
    const std::optional<std::string> selected_controller, const bool bounded,
    const std::optional<MockResetPositionStepDeadline> lifecycle_deadline) {
  std::string controller;
  bool real_arm = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ && !gripper_only_)
      return not_sent("ros2_control_switch_unavailable",
                      "the controller-manager switch client is not started");
    controller = selected_controller.value_or(
        arm_ ? arm_->transport.controller.controller_name
             : gripper_only_->controller.controller_name);
    real_arm = device_status_adapter_ != nullptr;
  }
  const auto activate_list = activate ? std::vector<std::string>{controller}
                                      : std::vector<std::string>{};
  const auto deactivate_list = activate ? std::vector<std::string>{}
                                        : std::vector<std::string>{controller};
  if (!bounded)
    return SendControllerSwitch(activate_list, deactivate_list, cancellation);
  if (!real_arm)
    return SendControllerSwitch(activate_list, deactivate_list, cancellation, std::nullopt,
                                lifecycle_switch_deadline(lifecycle_deadline));
  // M6 XADAPT (D8): a Real arm's switch is allowed min(the call cap, the
  // remaining time), and asks the manager for the same `timeout`; no time
  // left at the send refuses it before dispatch (SX-695, in
  // `SendControllerSwitch`).
  const auto now = std::chrono::steady_clock::now();
  const auto deadline = allowed_until(now, lifecycle_deadline);
  return SendControllerSwitch(
      activate_list, deactivate_list, cancellation, std::nullopt, deadline,
      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now));
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::SendControllerSwitch(
    std::vector<std::string> activate_controllers,
    std::vector<std::string> deactivate_controllers,
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> response_deadline,
    const std::optional<MockResetPositionStepDeadline> lifecycle_deadline,
    const std::optional<std::chrono::nanoseconds> request_timeout) {
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
  request->activate_controllers = std::move(activate_controllers);
  request->deactivate_controllers = std::move(deactivate_controllers);
  request->strictness = SwitchService::Request::STRICT;
  request->activate_asap = false;
  // M6 XADAPT: the manager's own switch timeout matches the per-call wait.
  if (request_timeout) request->timeout = rclcpp::Duration(*request_timeout);
  if (response_deadline &&
      std::chrono::steady_clock::now() >= *response_deadline)
    return not_sent("ros2_control_switch_timeout",
                    "The controller switch step deadline elapsed before dispatch");
  // SX-695: the Real path (which sets the request's timeout) sends nothing
  // once its allowed time is gone.
  if (request_timeout)
    if (auto refused = refuse_if_expired_at_send(lifecycle_deadline, {})) return *refused;
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
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::PrepareArmController() {
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  std::lock_guard lock(mutex_);
  if (!arm_)
    return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
  // R7 package 4: a Real arm is armed by `ActivateArmHardware`; its catalog
  // reaches this step only on armed hardware with the robot ready.
  const auto &activity = arm_->activity;
  if (!activity.controller_manager_present || !activity.observed ||
      !activity.controller_loaded || !activity.hardware_active)
    return not_sent("ros2_control_controller_not_loaded",
                    "the trajectory controller is not loaded on active "
                    "hardware");
  if (activity.controller_active)
    return not_sent("ros2_control_controller_active_unrequested",
                    "the trajectory controller is active without CCLC "
                    "activation");
  arm_->requested = core::ComponentLifecycleStateDTO::kPrepared;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::ActivateArmController(
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> deadline,
    const core::LifecycleSubstepFeedbackSink &feedback) {
  std::lock_guard switch_lock(switch_mutex_);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  std::function<core::Result<void>()> before_activation;
  std::shared_ptr<IRetainedDeviceStatusAdapter> device;
  RetainedToolContextSourceFn tool_context;
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
    if (guard_hooks_.has_value())
      before_activation = guard_hooks_->before_activation;
    device = device_status_adapter_;
    tool_context = tool_context_source_;
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
  if (device) {
    // R7 §3.6 Enable gate: a controller activated while the hardware holds a
    // zero (NaN-seeded) target would drive there. Fresh, finite measured
    // joints that agree with the robot's own angles, with the robot ready.
    const auto now = clock_->now();
    auto measured = [&] {
      std::lock_guard lock(mutex_);
      return MeasuredJointsLocked(now);
    }();
    if (!measured.ok())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, measured.error()};
    const auto gated = device->CheckActivation(now, measured.value());
    if (!gated.ok())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, gated.error()};
    // Active TCP (M7, design 3.3): the Enable re-sync at the Active TCP
    // (the flange with no TCP, B-run-1): the adapter sends its offset when
    // the controller reports another (zero with the TCP offset sync off),
    // then the total payload (SX-727: as part of this step, its remaining
    // time and cancellation). A sent offset or re-arm that may still act
    // leaves the step uncertain.
    if (tool_context) {
      if (!committed_snapshot || !committed_tool_context)
        return not_sent("ros2_control_switch_unavailable", "Committed Scene source is unavailable");
      const auto context = committed_tool_context(active_tcp_frame, *committed_snapshot);
      if (!context.ok())
        return {RetainedRos2ControlSwitchOutcome::kNotSent, context.error()};
      // V-B2 §2.4: the re-sync is a nested substep of this activation, run
      // before the switch (after it the re-arm would make the plugin
      // reactivate every controller).
      // Each event: one journal write under `switch_mutex_`; the port stamps
      // the transition's attempt.
      const auto sync_event = [&](const core::LifecycleSubstepEventDTO::Phase phase,
                                  std::optional<core::LifecycleTransitionOutcomeDTO> outcome,
                                  std::optional<std::string> error_code) {
        if (feedback)
          feedback({core::LifecyclePrimitiveAttemptId(std::string()),
                    core::LifecyclePrimitiveId(std::string(kToolContextSyncSubstepId)),
                    core::LifecyclePrimitiveId("ros2_control.controller.activate"),
                    phase, clock_->now(), std::move(outcome), std::move(error_code)});
      };
      sync_event(core::LifecycleSubstepEventDTO::Phase::kStarted, std::nullopt,
                 std::nullopt);
      // Active TCP (M7): the Active TCP's offset (zero with no TCP or the
      // checkbox off) and the total payload.
      const VendorToolLoadSet load{context.value().payload_mass_kg,
                                   context.value().payload_center_of_mass_m};
      VendorToolContextCommand command{VendorToolOffsetZero{}, load};
      if (active_tcp_frame && offset_sync)
        command.offset = VendorToolOffsetSet{context.value().tcp_pose_in_flange};
      const auto applied = device->SendToolContext(command, true, deadline, cancellation);
      if (!applied.error) {
        std::lock_guard lock(mutex_);
        last_tool_load_ = load;
        last_tool_offset_ = command.offset;
      }
      const RetainedRos2ControlSwitchResult synced{
          !applied.error ? RetainedRos2ControlSwitchOutcome::kSucceeded
          : applied.effect_possible ? RetainedRos2ControlSwitchOutcome::kUncertain
                                    : RetainedRos2ControlSwitchOutcome::kNotSent,
          applied.error};
      sync_event(core::LifecycleSubstepEventDTO::Phase::kFinished,
                 TransitionOutcome(synced),
                 synced.error ? std::optional<std::string>(synced.error->code)
                              : std::nullopt);
      if (synced.error) return synced;
      offset_applied = applied.rearmed;
    }
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
  {
    std::lock_guard lock(mutex_);
    const auto rotated = RotateEpochLocked();
    if (!rotated.ok())
      return with_offset_effect({RetainedRos2ControlSwitchOutcome::kNotSent, rotated.error()});
  }
  auto result = SendSwitch(true, cancellation, std::nullopt, true, deadline);
  std::lock_guard lock(mutex_);
  if (result.outcome != RetainedRos2ControlSwitchOutcome::kNotSent)
    arm_->epoch_used = true;  // it may be active on this epoch now
  result = with_offset_effect(std::move(result));
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
    arm_->bound = true;
    arm_->active_seen_while_bound = arm_->activity.controller_manager_present &&
                                    arm_->activity.controller_active;
    arm_->requested = core::ComponentLifecycleStateDTO::kActive;
    // M6 ADAPTERM: a CCLC Enable binds a fresh epoch too.
    reengage_rebind_required_ = false;
    PublishReadinessLocked();
    if (device) device->EndRearm();
  }
  return result;
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::DeactivateArmController(
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> deadline) {
  std::lock_guard switch_lock(switch_mutex_);
  bool active = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    active = !arm_->activity.observed || arm_->activity.controller_active;
  }
  const auto result =
      active ? SendSwitch(false, cancellation, std::nullopt, true, deadline)
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

core::Result<void> RetainedRos2ControlRuntimeHandle::MeasuredAtRestLocked() const {
  // The existing settle criterion (`kDefaultSettleVelocityThresholdRadS`):
  // every joint's measured velocity present, finite and within it.
  const auto &joints = arm_->transport.adapter.lane_facts.joint_names;
  std::lock_guard lock(device_measured_joints_->mutex);
  const auto &sample = *device_measured_joints_;
  for (const auto &joint : joints) {
    const auto found = std::find(sample.names.begin(), sample.names.end(), joint);
    const auto index =
        static_cast<std::size_t>(std::distance(sample.names.begin(), found));
    if (found == sample.names.end() || index >= sample.velocities.size() ||
        !std::isfinite(sample.velocities[index]) ||
        std::abs(sample.velocities[index]) > core::kDefaultSettleVelocityThresholdRadS)
      return core::Result<void>::Err(switch_error(
          "ros2_control_reengage_not_at_rest",
          "the measured joint velocities do not show the arm at rest"));
  }
  return core::Result<void>::Ok();
}

core::Result<std::vector<double>>
RetainedRos2ControlRuntimeHandle::MeasuredJointsLocked(
    const core::TimePoint now_ns) const {
  const auto refuse = [](std::string reason, std::string message) {
    return core::Result<std::vector<double>>::Err(
        switch_error(std::move(reason), std::move(message)));
  };
  if (!arm_ || !device_measured_joints_ ||
      !arm_->transport.measured_joint_state_lane.has_value())
    return refuse("ros2_control_measured_joints_unavailable",
                  "the arm has no measured joint state");
  const auto budget = arm_->transport.measured_joint_state_lane->freshness_budget_ns;
  const auto &joints = arm_->transport.adapter.lane_facts.joint_names;
  std::lock_guard lock(device_measured_joints_->mutex);
  const auto &sample = *device_measured_joints_;
  // A sample received after the caller's boundary `now_ns` (taken before an
  // endpoint refresh, or by the coordinator for a whole readiness pass) is
  // newer than it, never stale.
  if (!sample.received.has_value() ||
      !core::received_sample_current(*sample.received, now_ns, budget))
    return refuse("ros2_control_measured_joints_stale",
                  "the measured joint state is missing or stale");
  if (sample.not_before.has_value() && *sample.received < *sample.not_before)
    return refuse("ros2_control_measured_joints_stale",
                  "no measured joint state since the hardware activation");
  if (sample.not_before_stamp_ns.has_value() && sample.stamp_ns <= *sample.not_before_stamp_ns)
    return refuse("ros2_control_measured_joints_stale",
                  "no measured joint state stamped after the re-arm");
  std::vector<double> result;
  result.reserve(joints.size());
  for (const auto &joint : joints) {
    const auto found =
        std::find(sample.names.begin(), sample.names.end(), joint);
    const auto index =
        static_cast<std::size_t>(std::distance(sample.names.begin(), found));
    // NaN (no valid hardware read yet) is never replaced by a zero.
    if (found == sample.names.end() || index >= sample.positions.size() ||
        !std::isfinite(sample.positions[index]))
      return refuse("ros2_control_measured_joints_invalid",
                    "a measured joint is missing or not finite");
    result.push_back(sample.positions[index]);
  }
  return core::Result<std::vector<double>>::Ok(std::move(result));
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::SendHardwareState(
    const bool active, const std::stop_token cancellation,
    IRetainedDeviceStatusAdapter &device,
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
  // The vendor driver's (de)activation changes the robot's mode and state:
  // ManyForge's own effect while in flight and for a settle window after.
  device.BeginHardwareTransition(active);
  // M6 XADAPT (D8, SX-690/699): allowed min(the call cap, the step's
  // remaining time), checked right at the send; none left, nothing is sent.
  if (auto refused = refuse_if_expired_at_send(step_deadline, [&] {
        device.SettleHardwareTransition(active, false, clock_->now());
      }))
    return *refused;
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
      device.SettleHardwareTransition(active, true, clock_->now());
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
  device.SettleHardwareTransition(active, true, clock_->now());
  if (!response || !response->ok)
    return {RetainedRos2ControlSwitchOutcome::kFailed,
            switch_error(active ? "ros2_control_hardware_activation_refused"
                                : "ros2_control_hardware_deactivation_refused",
                         "the controller manager refused the hardware "
                         "transition")};
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchOutcome refused_step_outcome(
    const std::optional<std::vector<core::LifecycleFactDTO>> &report_before,
    const std::optional<std::vector<core::LifecycleFactDTO>> &report_after,
    const std::optional<RetainedRos2ControlArmActivity> &states_before,
    const std::optional<RetainedRos2ControlArmActivity> &states_after) {
  if (!report_before || !report_after || !states_before || !states_after)
    return RetainedRos2ControlSwitchOutcome::kFailed;
  // The transition's own states only.
  const bool same_states =
      states_before->controller_loaded == states_after->controller_loaded &&
      states_before->controller_active == states_after->controller_active &&
      states_before->hardware_active == states_after->hardware_active &&
      states_before->broadcaster_active == states_after->broadcaster_active;
  return *report_after == *report_before && same_states
             ? RetainedRos2ControlSwitchOutcome::kFailedNoEffect
             : RetainedRos2ControlSwitchOutcome::kFailedEffectObserved;
}

std::optional<RetainedRos2ControlArmActivity>
RetainedRos2ControlRuntimeHandle::QueryArmStates(
    const std::optional<MockResetPositionStepDeadline> wait_until,
    const std::stop_token cancellation,
    std::optional<core::ErrorDTO> &not_sent_error) const {
  rclcpp::Client<ListControllersService>::SharedPtr controllers;
  rclcpp::Client<ListHardwareService>::SharedPtr hardware;
  std::string controller;
  std::string component;
  {
    std::lock_guard lock(mutex_);
    if (!arm_) return std::nullopt;
    controllers = arm_->list_controllers_client;
    hardware = arm_->list_hardware_client;
    controller = arm_->transport.controller.controller_name;
    component = arm_->transport.controller.hardware_component_name;
  }
  const auto logger = resources_->node()->get_logger();
  const auto listed = query_once<ListControllersService>(
      controllers, wait_until, cancellation, logger, "list_controllers", not_sent_error);
  if (!listed) return std::nullopt;
  const auto components = query_once<ListHardwareService>(
      hardware, wait_until, cancellation, logger, "list_hardware_components",
      not_sent_error);
  if (!components) return std::nullopt;
  // The same reading as the activity sample's.
  RetainedRos2ControlArmActivity states;
  states.controller_manager_present = true;
  states.observed = true;
  for (const auto &row : listed->controller) {
    if (row.name == kJointStateBroadcasterControllerName)
      states.broadcaster_active = row.state == "active";
    if (row.name != controller) continue;
    states.controller_loaded = row.state == "inactive" || row.state == "active";
    states.controller_active = row.state == "active";
  }
  for (const auto &row : components->component)
    if (row.name == component)
      states.hardware_active = row.state.id == lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
  return states;
}

RetainedRos2ControlRuntimeHandle::RealArmStatus
RetainedRos2ControlRuntimeHandle::QueryRealArmStatus(
    const IRetainedDeviceStatusAdapter &device,
    const std::optional<MockResetPositionStepDeadline> wait_until,
    const std::stop_token cancellation) const {
  const auto started = clock_->now();
  // D8: the list queries are allowed min(the call cap, the remaining time);
  // the report wait is freshness (0.5 s), not a call. No answer within its
  // time is the connection lost; only cancellation is not.
  RealArmStatus status;
  status.states = QueryArmStates(wait_until, cancellation, status.not_sent);
  // SX-690/695: a query not sent is no non-answer.
  if (cancellation.stop_requested() || status.not_sent) return status;
  status.report = device.ReadReportFacts(started, cancellation);
  if (cancellation.stop_requested()) return status;
  if (!status.states)
    MarkUnresponsive(controller_manager_unresponsive("controller_manager_state_query_timeout"));
  else if (!status.report)
    MarkUnresponsive(robot_controller_unresponsive("robot_status_report_timeout"));
  else {
    // Both answered: an earlier unresponsive fault ends here.
    std::lock_guard lock(mutex_);
    unresponsive_.reset();
  }
  return status;
}

void RetainedRos2ControlRuntimeHandle::MarkUnresponsive(core::ErrorDTO error) const {
  std::lock_guard lock(mutex_);
  if (!unresponsive_) unresponsive_ = std::move(error);
}

RetainedRos2ControlSwitchResult RetainedRos2ControlRuntimeHandle::RealArmStep(
    const std::function<RetainedRos2ControlSwitchResult()> &step,
    const IRetainedDeviceStatusAdapter &device,
    const std::optional<MockResetPositionStepDeadline> step_deadline,
    const std::stop_token cancellation) const {
  // D8: no time left before sending anything: the lifecycle step timeout,
  // before effect, nothing sent and no fault.
  const auto expired = [&] {
    return step_deadline && std::chrono::steady_clock::now() >= *step_deadline;
  };
  const auto deadline_refusal = [](const RealArmStatus &status) {
    RetainedRos2ControlSwitchResult refused{
        RetainedRos2ControlSwitchOutcome::kNotSent,
        core::ErrorDTO{"component_lifecycle_deadline_expired",
                       "the lifecycle deadline expired before dispatch",
                       {{"reason", "deadline_exhausted"}}}};
    AddCurrentStatus(refused, status);
    return refused;
  };
  if (expired()) return deadline_refusal(RealArmStatus{});
  // The baseline: the manager's listed states (allowed min(the call cap, the
  // remaining time)) and a robot report received after the query started.
  // Unanswered, the connection is lost: refused before effect, the fault
  // latched.
  const auto before = QueryRealArmStatus(device, step_deadline, cancellation);
  if (cancellation.stop_requested())
    return not_sent("ros2_control_status_query_cancelled",
                    "the step was cancelled before it was sent");
  // SX-663/690: a baseline query not sent (service not discovered, or no
  // time left): refused before effect with that reason, no fault.
  if (before.not_sent) {
    RetainedRos2ControlSwitchResult refused{RetainedRos2ControlSwitchOutcome::kNotSent,
                                            before.not_sent};
    AddCurrentStatus(refused, before);
    return refused;
  }
  if (!before.states || !before.report) {
    RetainedRos2ControlSwitchResult refused{
        RetainedRos2ControlSwitchOutcome::kNotSent,
        !before.states ? controller_manager_unresponsive("controller_manager_state_query_timeout")
                       : robot_controller_unresponsive("robot_status_report_timeout")};
    AddCurrentStatus(refused, before);
    return refused;
  }
  // Answered, but no time left to send the step itself.
  if (expired()) return deadline_refusal(before);
  auto result = step();
  if (!result.error) return result;
  // The step's own call got no answer within its own bound: the connection
  // is lost, a known result (maintainer 2026-10-28; recorded like a
  // collision: the known fact, physical consequences unknown). A known
  // failure whose observed effect is the component fault it now reports,
  // carrying the typed code and status to the transaction's result. Nothing
  // more is asked of the component (no further wait).
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kUncertain) {
    const auto reason = result.error->details.contains("reason")
                            ? result.error->details.at("reason")
                            : std::string();
    // D8: no answer within the time a call was allowed, min(the call cap,
    // the remaining time), is the connection lost.
    if (reason == "ros2_control_hardware_state_timeout" ||
        reason == "ros2_control_switch_timeout")
      result.error = controller_manager_unresponsive(reason);
    if (result.error->code == "controller_manager_unresponsive" ||
        result.error->code == "robot_controller_unresponsive") {
      MarkUnresponsive(*result.error);
      result.outcome = RetainedRos2ControlSwitchOutcome::kFailedEffectObserved;
      AddCurrentStatus(result, RealArmStatus{});
      return result;
    }
    // DX-2967/2969, V-A (A2): the adapter decided this uncertain outcome by
    // its own comparison (it lacked usable comparison facts). A wrapper-only
    // query cannot complete that comparison; it could classify from
    // report-only facts, and would add a wait past the adapter's own end.
    if (result.adapter_decided) {
      AddCurrentStatus(result, RealArmStatus{});
      return result;
    }
  }
  // SX-691: the proof query follows the same rule, min(the call cap, the
  // remaining time), so the budget bounds the whole step; with no time left
  // it is not sent and the step keeps what it reported.
  const auto after = QueryRealArmStatus(device, step_deadline, cancellation);
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kFailed ||
      result.outcome == RetainedRos2ControlSwitchOutcome::kFailedNoEffect) {
    const auto queried = refused_step_outcome(before.report, after.report, before.states,
                                              after.states);
    const auto step_reason = result.error->details.contains("reason")
                                 ? result.error->details.at("reason")
                                 : result.error->code;
    if (after.not_sent) {
      // SX-691/695: the status after the step could not be read (no time
      // left, or the service not discovered). An adapter's own no-effect
      // proof stands; a refusal's effect is unknown: uncertain (never
      // `{Known, Possible}`), which CCLC reconciles.
      if (result.outcome == RetainedRos2ControlSwitchOutcome::kFailed) {
        result.outcome = RetainedRos2ControlSwitchOutcome::kUncertain;
        auto error = *after.not_sent;
        error.details.emplace("step_reason", step_reason);
        result.error = std::move(error);
      }
    } else if (queried == RetainedRos2ControlSwitchOutcome::kFailedEffectObserved) {
      // SX-664: any queried change is an effect, also after an adapter's own
      // no-effect proof (a clear's vendor facts).
      result.outcome = queried;
    } else if (queried == RetainedRos2ControlSwitchOutcome::kFailedNoEffect) {
      if (result.outcome == RetainedRos2ControlSwitchOutcome::kFailed) result.outcome = queried;
    } else if (cancellation.stop_requested()) {
      // SX-664: the owner cancelled the status query: the effect is unknown.
      // An uncertain outcome, never `{Known, Possible}` (the journal refuses
      // that after an observed step); the next owner reconciles it.
      result.outcome = RetainedRos2ControlSwitchOutcome::kUncertain;
      auto error = switch_error("ros2_control_status_query_cancelled",
                                "the step's status query was cancelled; its effect is "
                                "unknown");
      error.details.emplace("step_reason", step_reason);
      result.error = std::move(error);
    } else {
      // Answered, but the status query is no longer: the unresponsive fault,
      // a definite failure as above (accepted after an observed step).
      result.outcome = RetainedRos2ControlSwitchOutcome::kFailedEffectObserved;
      auto error = !after.states
                       ? controller_manager_unresponsive("controller_manager_state_query_timeout")
                       : robot_controller_unresponsive("robot_status_report_timeout");
      error.details.emplace("step_reason", step_reason);
      result.error = std::move(error);
    }
  }
  AddCurrentStatus(result, after);
  return result;
}

void RetainedRos2ControlRuntimeHandle::AddCurrentStatus(
    RetainedRos2ControlSwitchResult &result, const RealArmStatus &status) {
  if (!result.error) return;
  // The vendor codes as incidents render them (`C2 mode=0 state=4`) and the
  // manager's listed states, as queried, within the error's detail bound.
  std::string vendor = "unavailable";
  if (status.report) {
    vendor.clear();
    for (const auto &code : core::ComponentIncidentVendorCodes(*status.report))
      vendor += (vendor.empty() ? "" : " ") + code;
    if (vendor.empty()) vendor = "none";
  }
  const auto on = [](const bool value) { return value ? "active" : "inactive"; };
  const auto ros2_control =
      !status.states ? std::string("unavailable")
                     : std::string("hardware=") + on(status.states->hardware_active) +
                           " controller=" + on(status.states->controller_active) +
                           " broadcaster=" + on(status.states->broadcaster_active);
  auto &details = result.error->details;
  const auto add = [&](const char *key, std::string value) {
    if (details.size() < core::kMaxLifecycleErrorDetails)
      details.emplace(key, bounded_switch_detail(std::move(value)));
  };
  add("vendor_status", std::move(vendor));
  add("ros2_control_status", ros2_control);
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::ActivateArmHardware(
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> step_deadline) {
  std::unique_lock switch_lock(switch_mutex_);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  std::shared_ptr<IRetainedDeviceStatusAdapter> device;
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    if (!device_status_adapter_)
      return not_sent("ros2_control_hardware_primitive_unsupported",
                      "only a Real arm with a vendor adapter arms its hardware");
    const auto &activity = arm_->activity;
    if (!activity.controller_manager_present || !activity.observed ||
        !activity.controller_loaded)
      return not_sent("ros2_control_controller_not_loaded",
                      "the trajectory controller is not loaded");
    if (activity.controller_active)
      return not_sent("ros2_control_controller_active_unrequested",
                      "the trajectory controller is active before arming");
    if (activity.hardware_active)
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
    device = device_status_adapter_;
  }
  // D1, V-A (A5): the vendor's own arming precondition.
  if (const auto armable = device->CheckArmable(clock_->now()); !armable.ok())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, armable.error()};
  const auto result = SendHardwareState(true, cancellation, *device, step_deadline);
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kNotSent)
    return result;
  // Only a broadcaster sample received one freshness budget after the answer
  // counts as showing the controller manager's loop running again after the
  // driver's activation (the loop can publish one more sample before a
  // driver's activation stalls it). The cutoff is on receipt time, so this
  // makes a post-activation sample very likely; it does not prove it.
  std::shared_ptr<DeviceMeasuredJoints> joints;
  core::TimePoint not_before{};
  {
    std::lock_guard lock(mutex_);
    joints = device_measured_joints_;
    if (joints)
      not_before = clock_->now() +
                   arm_->transport.measured_joint_state_lane->freshness_budget_ns;
  }
  if (!joints)
    return result;
  {
    std::lock_guard lock(joints->mutex);
    joints->not_before = not_before;
  }
  switch_lock.unlock();
  if (result.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
    return result;
  // Return once the driver's transient is over: the measured joints Prepare
  // requires (fresh, after the cutoff, every joint present and finite) are
  // there (it reactivated the broadcaster, or never stopped it), or the
  // adapter's transient bound elapsed with the broadcaster still off. The
  // switch lock is released. This wait is a per-call limit like the
  // request's own cap: at most the adapter's bound, and never past the
  // lifecycle step's remaining budget, which bounds the whole step. A
  // broadcaster the driver left off then keeps step time for the
  // broadcaster step that switches it on.
  const auto deadline = std::min(
      std::chrono::steady_clock::now() + device->ActivationTransientBound(),
      step_deadline.value_or(std::chrono::steady_clock::time_point::max()));
  const auto measured_valid = [&] {
    const auto now = clock_->now();
    std::lock_guard lock(mutex_);
    return MeasuredJointsLocked(now).ok();
  };
  while (!cancellation.stop_requested() &&
         std::chrono::steady_clock::now() < deadline && !measured_valid())
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return result;
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::ActivateJointStateBroadcaster(
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> deadline) {
  std::lock_guard switch_lock(switch_mutex_);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    if (!device_status_adapter_)
      return not_sent("ros2_control_hardware_primitive_unsupported",
                      "only a Real arm with a vendor adapter re-activates its "
                      "joint state broadcaster");
    const auto &activity = arm_->activity;
    if (!activity.controller_manager_present || !activity.observed ||
        !activity.hardware_active)
      return not_sent("ros2_control_hardware_inactive",
                      "the joint state broadcaster is re-activated only on "
                      "active hardware");
    if (activity.broadcaster_active)
      return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
  }
  return SendSwitch(true, cancellation,
                    std::string(kJointStateBroadcasterControllerName), true,
                    deadline);
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::DeactivateArmHardware(
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> step_deadline) {
  std::lock_guard switch_lock(switch_mutex_);
  std::shared_ptr<IRetainedDeviceStatusAdapter> device;
  bool hardware_active = true;
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    if (!device_status_adapter_)
      return not_sent("ros2_control_hardware_primitive_unsupported",
                      "only a Real arm with a vendor adapter disarms its "
                      "hardware");
    const auto &activity = arm_->activity;
    if (!activity.observed || activity.controller_active)
      return not_sent("ros2_control_controller_still_active",
                      "the trajectory controller is switched off first");
    hardware_active = activity.hardware_active;
    device = device_status_adapter_;
  }
  auto result = RetainedRos2ControlSwitchResult{
      RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
  if (hardware_active)
    result = SendHardwareState(false, cancellation, *device, step_deadline);
  // D4: a disarm request makes the arm Disabled (not armed); the hardware's
  // own activity shows whether it took effect.
  if (result.outcome != RetainedRos2ControlSwitchOutcome::kNotSent) {
    std::lock_guard lock(mutex_);
    arm_->bound = false;
    arm_->requested = core::ComponentLifecycleStateDTO::kDisabled;
    PublishReadinessLocked();
  }
  return result;
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
    const auto contained = SendSwitch(false, cancellation, *writer, false);
    if (contained.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
      return contained;
    std::lock_guard lock(mutex_);
    arm_->reset_writer_possibly_active = false;
  }
  return SendSwitch(false, cancellation, std::nullopt, false);
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

bool RetainedRos2ControlRuntimeHandle::mock_reset_successor_ready() const {
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity;
  std::optional<core::ControlDomainId> domain;
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
    domain = motion_domain_id_;
  }
  if (!activity)
    return false;
  // The existing switchback wait owns endpoint discovery within its step
  // deadline. Reset does not synthesize a separate lifecycle transport loss.
  activity->Refresh();
  adapter->RefreshEndpointFacts();
  if (!ReadLifecycleHealth(*domain, clock_->now()).ok())
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
    const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> deadline) {
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
        SendSwitch(false, cancellation, std::nullopt, true, deadline);
    if (result.outcome != RetainedRos2ControlSwitchOutcome::kSucceeded)
      return result;
  }
  if (on_recover)
    on_recover();
  std::lock_guard lock(mutex_);
  arm_->bound = false;
  arm_->requested = core::ComponentLifecycleStateDTO::kPrepared;
  PublishReadinessLocked();
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::PrepareGripperController() {
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
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
  if (!gripper_activity_.controller_manager_present ||
      !gripper_activity_.observed || !gripper_activity_.controller_loaded ||
      !gripper_activity_.hardware_active)
    return not_sent(
        "ros2_control_gripper_controller_not_loaded",
        "the exact gripper controller is not loaded on active hardware");
  if (gripper_activity_.controller_active)
    return not_sent("ros2_control_gripper_controller_active_unrequested",
                    "the gripper controller is active without CCLC activation");
  gripper_requested_ = core::ComponentLifecycleStateDTO::kPrepared;
  return {RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::SwitchGripperController(
    const bool activate, const std::stop_token cancellation,
    const std::optional<MockResetPositionStepDeadline> deadline) {
  std::lock_guard switch_lock(switch_mutex_);
  if (activate) {
    RefreshEndpointFacts();
    if (auto error = ReadEndpointDiagnostic())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  }
  std::string controller;
  bool needs_switch;
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
    controller = arm_ ? arm_->transport.gripper_controller->controller_name
                      : gripper_only_->controller.controller_name;
    needs_switch = activate || !gripper_activity_.observed ||
                   gripper_activity_.controller_active;
  }
  const auto result =
      needs_switch
          ? SendSwitch(activate, cancellation, controller, true, deadline)
          : RetainedRos2ControlSwitchResult{
                RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
    std::lock_guard lock(mutex_);
    gripper_requested_ = activate ? core::ComponentLifecycleStateDTO::kActive
                                  : core::ComponentLifecycleStateDTO::kDisabled;
  }
  return result;
}

RetainedExternallyManagedPrimitiveDispatcher
RetainedRos2ControlRuntimeHandle::lifecycle_primitive_dispatcher() {
  // Primitives run on lifecycle threads, never on this executor; a dispatch
  // in flight keeps an external owner, so the transport stays live for it.
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak = external_owner_;
  return [weak](const core::ControlDomainId &domain,
                const core::LifecyclePrimitiveId &primitive,
                const std::stop_token cancellation,
                const std::optional<core::Duration> remaining,
                const core::LifecycleSubstepFeedbackSink &feedback)
             -> RetainedExternallyManagedPrimitiveOutcome {
    // The transaction's remaining command time bounds a switch's answer.
    std::optional<MockResetPositionStepDeadline> deadline;
    if (remaining && *remaining > 0)
      deadline = std::chrono::steady_clock::now() +
                 std::chrono::nanoseconds(*remaining);
    const auto self = weak.lock();
    if (self) {
      bool gripper_domain = false;
      bool command_bridge_domain = false;
      {
        std::lock_guard lock(self->mutex_);
        gripper_domain = ((self->arm_ && self->arm_->transport.gripper_controller) ||
                          (self->gripper_only_ && self->gripper_only_->physical_controller_required) ||
                          (self->gripper_ && self->gripper_id_ &&
                           self->gripper_->gripper_actuation_kind(*self->gripper_id_) == core::GripperActuationKind::kPneumatic &&
                           self->gripper_->gripper_output_mode(*self->gripper_id_) == core::GripperOutputMode::kNone)) &&
                         self->gripper_domain_id_ == domain;
        command_bridge_domain = self->gripper_ && self->gripper_id_ &&
                                !gripper_domain &&
                                self->gripper_domain_id_ == domain;
      }
      if (gripper_domain) {
        if (primitive.value() == "ros2_control.controller.prepare")
          return self->PrepareGripperController();
        if (primitive.value() == "ros2_control.controller.activate")
          return self->SwitchGripperController(true, cancellation, deadline);
        if (primitive.value() == "ros2_control.controller.deactivate")
          return self->SwitchGripperController(false, cancellation, deadline);
        return not_sent("ros2_control_lifecycle_primitive_unknown",
                        "the gripper has no such controller primitive");
      }
      if (command_bridge_domain) {
        // The Isaac effort transport has no remote command owner to prepare,
        // authorize or cancel. As the no-output owner, its lifecycle is the
        // local request; transport readiness stays observed.
        const auto transport =
            self->gripper_->gripper_transport_readiness(*self->gripper_id_);
        const bool local_owner =
            transport.transport_kind == "isaac_joint_state_effort";
        const auto bridge_result = [&]()
            -> ros2_control_execution::Ros2GripperLifecycleResult {
          if (local_owner) {
            if (primitive.value() != "ros2_control.controller.prepare" &&
                primitive.value() != "ros2_control.controller.activate" &&
                primitive.value() != "ros2_control.controller.deactivate")
              return {false, false, core::ErrorDTO{
                  "ros2_control_lifecycle_primitive_unknown",
                  "the gripper bridge has no such lifecycle primitive", {}}};
            if (primitive.value() != "ros2_control.controller.deactivate" &&
                !transport.ready)
              return {false, false, core::ErrorDTO{
                  "ros2_control_gripper_bridge_not_ready",
                  "The exact gripper command transport is not ready", {}}};
            return {true, false, std::nullopt};
          }
          if (primitive.value() == "ros2_control.controller.prepare")
            return self->gripper_->prepare_lifecycle_gripper(*self->gripper_id_);
          if (primitive.value() == "ros2_control.controller.activate") {
            auto prepared =
                self->gripper_->prepare_lifecycle_gripper(*self->gripper_id_);
            if (!prepared.succeeded) return prepared;
            return self->gripper_->authorize_lifecycle_gripper(
                *self->gripper_id_);
          }
          if (primitive.value() == "ros2_control.controller.deactivate")
            return self->gripper_->disable_lifecycle_gripper(
                *self->gripper_id_, cancellation);
          return ros2_control_execution::Ros2GripperLifecycleResult{
              false, false, core::ErrorDTO{
              "ros2_control_lifecycle_primitive_unknown",
              "the gripper bridge has no such lifecycle primitive", {}}};
        }();
        if (!bridge_result.succeeded)
          return RetainedRos2ControlSwitchResult{
              bridge_result.effect_started
                  ? RetainedRos2ControlSwitchOutcome::kUncertain
                  : RetainedRos2ControlSwitchOutcome::kNotSent,
              bridge_result.error};
        {
          std::lock_guard lock(self->mutex_);
          self->gripper_requested_ =
              primitive.value() == "ros2_control.controller.activate"
                  ? core::ComponentLifecycleStateDTO::kActive
              : primitive.value() == "ros2_control.controller.deactivate"
                  ? core::ComponentLifecycleStateDTO::kDisabled
                  : core::ComponentLifecycleStateDTO::kPrepared;
        }
        return RetainedRos2ControlSwitchResult{
            RetainedRos2ControlSwitchOutcome::kSucceeded, std::nullopt};
      }
    }
    if (!self || self->motion_domain() != domain) {
      return not_sent("ros2_control_lifecycle_primitive_domain_unsupported",
                      "this control domain has no ros2_control controller "
                      "primitive");
    }
    const auto &id = primitive.value();
    std::shared_ptr<IRetainedDeviceStatusAdapter> device;
    {
      std::lock_guard lock(self->mutex_);
      device = self->device_status_adapter_;
    }
    // R7 package 4: a Real arm's controller and hardware steps and its
    // vendor's primitives. M6 XADAPT: each runs between status queries and
    // ends definite; no answer in time is a component fault.
    if (device) {
      const auto step = [&](const std::function<RetainedRos2ControlSwitchResult()> &call) {
        return self->RealArmStep(call, *device, deadline, cancellation);
      };
      if (id == "ros2_control.controller.prepare")
        return step([&] { return self->PrepareArmController(); });
      if (id == "ros2_control.controller.activate")
        return step([&] {
          return self->ActivateArmController(cancellation, deadline, feedback);
        });
      if (id == "ros2_control.controller.deactivate")
        return step([&] { return self->DeactivateArmController(cancellation, deadline); });
      if (id == kHardwareActivatePrimitiveId)
        return step([&] { return self->ActivateArmHardware(cancellation, deadline); });
      if (id == kHardwareDeactivatePrimitiveId)
        return step([&] { return self->DeactivateArmHardware(cancellation, deadline); });
      if (id == kBroadcasterActivatePrimitiveId)
        return step([&] {
          return self->ActivateJointStateBroadcaster(cancellation, deadline);
        });
      // A Real arm's Recover of an unresponsive fault: the status queries
      // are the step (both answered end the fault). Active TCP (T-N1): it
      // also ends a tool-context fault; the next Enable re-syncs.
      if (id == "ros2_control.controller.recover")
        return step([&] {
          {
            std::lock_guard lock(self->mutex_);
            self->tool_context_fault_.reset();
          }
          return RetainedRos2ControlSwitchResult{RetainedRos2ControlSwitchOutcome::kSucceeded,
                                                 std::nullopt};
        });
      // The vendor's primitives, one call at a time as for every other
      // primitive here (the adapter verifies its own effect).
      return step([&] {
        std::lock_guard switch_lock(self->switch_mutex_);
        return device->Dispatch(primitive, deadline, cancellation);
      });
    }
    if (id == "ros2_control.controller.prepare")
      return self->PrepareArmController();
    if (id == "ros2_control.controller.activate")
      return self->ActivateArmController(cancellation, deadline, feedback);
    if (id == "ros2_control.controller.deactivate")
      return self->DeactivateArmController(cancellation, deadline);
    if (id == "ros2_control.controller.recover")
      return self->RecoverArmController(cancellation, deadline);
    return not_sent("ros2_control_lifecycle_primitive_unknown",
                    "the ros2_control catalog has no such primitive");
  };
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

RetainedExternallyManagedDomainSnapshotSource
RetainedRos2ControlRuntimeHandle::lifecycle_snapshot_source() {
  // The lifecycle registry retains this source as an existing external owner.
  const std::shared_ptr<RetainedRos2ControlRuntimeHandle> self(external_owner_);
  return [self](const core::ControlDomainId &domain_id,
                const core::TimePoint now_ns) {
    return self->ReadLifecycleSnapshot(domain_id, now_ns);
  };
}

struct RetainedRos2ControlRuntimeHandle::LifecycleHealthInputs final {
  bool is_motion{};
  bool is_gripper{};
  bool startup{true};
  bool cm_gripper{};
  bool bound{};
  bool activation_client_discovered{};
  bool discovery_attempted{};
  core::ComponentLifecycleStateDTO requested{
      core::ComponentLifecycleStateDTO::kDeclared};
  core::ComponentLifecycleStateDTO gripper_requested{
      core::ComponentLifecycleStateDTO::kDeclared};
  RetainedRos2ControlArmActivity activity;
  RetainedRos2ControlArmActivity gripper_activity;
  core::LifecycleBlockingCauseClassDTO retained_cause{
      core::LifecycleBlockingCauseClassDTO::kNone};
  std::function<std::optional<std::string>()> fault_hook;
  std::function<std::optional<std::string>()> process_fault_source;
  std::function<core::Result<std::optional<std::string>>()>
      process_health_source;
  std::optional<std::string> process_fault;
  /// The shared ROS executor stopped on an exception; no callbacks run.
  std::optional<std::string> transport_fault;
  std::optional<std::string> guard_fault;
  /// M6 XADAPT: the Real arm's unresponsive fault, if any.
  std::optional<core::ErrorDTO> unresponsive;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  ros2_control_execution::Ros2GripperCommandBackend *gripper{};
  std::optional<std::string> gripper_id;
  std::optional<ros2_control_execution::Ros2GripperTransportReadinessSnapshot>
      gripper_snapshot;
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot;
  std::string diagnostic_incident_id;
  std::shared_ptr<IRetainedDeviceStatusAdapter> device_adapter;
  std::optional<RetainedDeviceStatus> device_status;
  /// A Real arm's measured joints are fresh and finite (R7 package 4).
  bool measured_joints_valid{};
};

struct RetainedRos2ControlRuntimeHandle::LifecycleClassification final {
  core::ComponentLifecycleStateDTO state;
  bool ready;
  bool loaded;
  bool fault_present;
  bool transport_ready;
  bool inactive_unrequested;
  core::LifecycleBlockingCauseClassDTO cause;
  std::optional<core::ErrorDTO> diagnostic;
  std::string signature;
  std::vector<core::LifecycleFactDTO> device_facts{};
};

core::Result<RetainedRos2ControlRuntimeHandle::LifecycleHealthInputs>
RetainedRos2ControlRuntimeHandle::CaptureLifecycleHealthInputs(
    const core::ControlDomainId &domain, const core::TimePoint now_ns) const {
  std::lock_guard lock(mutex_);
  if ((!arm_ && !gripper_only_) || now_ns < 0 || domain.empty())
    return reject<LifecycleHealthInputs>(
        "ros2_control_lifecycle_source_unavailable",
        "ROS runtime lifecycle source is unbound or unavailable");
  LifecycleHealthInputs inputs;
  inputs.is_motion = motion_domain_id_ == domain;
  inputs.is_gripper = gripper_ != nullptr && gripper_domain_id_ == domain;
  inputs.gripper = gripper_.get();
  inputs.gripper_id = gripper_id_;
  inputs.activity = arm_ ? arm_->activity : RetainedRos2ControlArmActivity{};
  inputs.gripper_activity = gripper_activity_;
  inputs.gripper_requested = gripper_requested_;
  inputs.cm_gripper = (gripper_only_ && gripper_only_->physical_controller_required) ||
      (arm_ && arm_->transport.gripper_controller.has_value());
  if (arm_) {
    inputs.requested = arm_->requested;
    // Operation availability is closed while the physical binding continues
    // to receive feedback; only ArmPort Submit/Replace are gated.
    inputs.bound = arm_->bound && !arm_->reset_position_pending;
    inputs.activation_client_discovered = arm_->activation_client_discovered;
    inputs.adapter = arm_->adapter;
  }
  if (guard_hooks_)
    inputs.fault_hook = guard_hooks_->fault;
  if (inputs.is_motion && device_status_adapter_) {
    inputs.device_adapter = device_status_adapter_;
    // T-N1: a tool-context fault outlasts answered status queries.
    inputs.unresponsive = tool_context_fault_ ? tool_context_fault_ : unresponsive_;
    inputs.measured_joints_valid = MeasuredJointsLocked(now_ns).ok();
  }
  inputs.process_fault_source = process_fault_source_;
  inputs.process_health_source = process_health_source_;
  if (resources_)
    inputs.transport_fault = resources_->executor_fault();
  inputs.diagnostic_snapshot = process_diagnostic_snapshot_;
  inputs.diagnostic_incident_id = process_diagnostic_incident_id_;
  if (const auto found = domain_generations_.find(domain);
      found != domain_generations_.end()) {
    inputs.startup = !found->second.exact_configuration_seen;
    inputs.retained_cause = found->second.last_intervention_cause;
  }
  return core::Result<LifecycleHealthInputs>::Ok(std::move(inputs));
}

void RetainedRos2ControlRuntimeHandle::ReadLifecycleMonitorFacts(
    LifecycleHealthInputs &inputs) {
  using State = core::ComponentLifecycleStateDTO;
  if ((inputs.process_fault || inputs.transport_fault) &&
      (inputs.is_motion || inputs.is_gripper))
    return;
  if (inputs.is_motion) {
    inputs.guard_fault = inputs.fault_hook ? inputs.fault_hook() : std::nullopt;
    const auto &activity = inputs.activity;
    if (!inputs.activation_client_discovered && inputs.bound &&
        activity.controller_manager_present && activity.observed &&
        activity.controller_loaded && activity.hardware_active &&
        activity.controller_active && inputs.requested == State::kActive) {
      inputs.discovery_attempted = true;
      inputs.activation_client_discovered =
          inputs.adapter->ActionServerIsReady();
    }
  } else if (inputs.is_gripper && inputs.gripper_id) {
    inputs.gripper_snapshot =
        inputs.gripper->gripper_transport_readiness(*inputs.gripper_id);
  }
}

RetainedRos2ControlRuntimeHandle::LifecycleClassification
RetainedRos2ControlRuntimeHandle::ComputeLifecycleHealth(
    const LifecycleHealthInputs &inputs) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  using State = core::ComponentLifecycleStateDTO;
  const auto is_motion = inputs.is_motion;
  const auto is_gripper = inputs.is_gripper;
  const auto startup = inputs.startup;
  const auto &activity = inputs.activity;
  const auto &gripper_activity = inputs.gripper_activity;
  const auto cm_gripper = inputs.cm_gripper;
  const auto requested = inputs.requested;
  const auto gripper_requested = inputs.gripper_requested;
  const auto bound = inputs.bound;
  const auto activation_client_discovered = inputs.activation_client_discovered;
  const auto &gripper_id = inputs.gripper_id;
  const auto &diagnostic_incident_id = inputs.diagnostic_incident_id;
  std::optional<core::ErrorDTO> diagnostic;
  State state = State::kDegraded;
  bool ready = false;
  bool loaded = false;
  bool fault_present = false;
  bool transport_ready = false;
  bool inactive_unrequested = false;
  Cause cause = Cause::kAvailability;
  std::string signature;
  std::vector<core::LifecycleFactDTO> device_facts;
  // Q80: an exited Manager-owned controller launch fails the component, and
  // so does a ROS executor thread that failed on an exception.
  const auto &process_fault = inputs.process_fault;
  const auto &transport_fault = inputs.transport_fault;
  if ((process_fault.has_value() || transport_fault.has_value()) &&
      (is_motion || is_gripper)) {
    state = State::kFaulted;
    cause = Cause::kFault;
    fault_present = true;
    auto message = process_fault
                       ? *process_fault
                       : "ROS transport executor thread failed (the other "
                         "threads keep running): " + *transport_fault;
    if (message.size() > core::kMaxLifecycleErrorMessageBytes) {
      auto end = core::kMaxLifecycleErrorMessageBytes;
      while (end > 0U &&
             (static_cast<unsigned char>(message[end]) & 0xC0U) == 0x80U)
        --end;
      message.resize(end);
    }
    diagnostic = core::ErrorDTO{process_fault ? "controller_launch_exited"
                                              : "ros_transport_executor_failed",
                                std::move(message), {}};
    signature = std::string(is_motion ? "arm" : "gripper") +
                (process_fault ? ":controller_process:" + *process_fault
                               : ":ros_transport_executor:" + *transport_fault) +
                ":diagnostic:" + diagnostic_incident_id;
  } else if (is_motion) {
    const auto &guard_fault = inputs.guard_fault;
    transport_ready = activity.controller_manager_present && activity.observed;
    loaded = transport_ready && activity.controller_loaded &&
             activity.hardware_active;
    const bool binding_ready = bound && activation_client_discovered;
    ready = loaded && activity.controller_active && binding_ready &&
            requested == State::kActive;
    inactive_unrequested = !startup && transport_ready &&
                           !activity.controller_active &&
                           requested == State::kActive;
    // M6 XADAPT: an unresponsive controller manager or robot is a component
    // fault (an incident), with its typed error as the diagnostic.
    fault_present = guard_fault.has_value() || inputs.unresponsive.has_value();
    if (inputs.unresponsive) diagnostic = inputs.unresponsive;
    // Classification (as the retained lane did with list-service facts):
    // unavailable status is `availability`; before the exact configuration was
    // first active a not-yet-ready controller or hardware is waiting (`none`);
    // afterwards a missing or inactive controller is `availability` and
    // inactive hardware `unknown`. A lane guard fault is a `fault`.
    if (!transport_ready) {
      cause = Cause::kAvailability;
      state = State::kDegraded;
    } else if (fault_present) {
      cause = Cause::kFault;
      state = State::kFaulted;
    } else if (requested == State::kActive) {
      state = ready ? State::kActive : State::kDegraded;
      cause = ready                       ? Cause::kNone
              : !activity.hardware_active ? Cause::kUnknown
                                          : Cause::kAvailability;
    } else if (loaded) {
      state = requested;
      cause = Cause::kNone;
    } else if (startup && requested == State::kDeclared) {
      state = State::kDeclared;
      cause = Cause::kNone;
    } else if (inputs.device_status.has_value() &&
               requested == State::kDisabled && activity.controller_loaded &&
               !activity.hardware_active) {
      // R7 D4: a disabled Real arm is not armed; its hardware is inactive.
      state = State::kDisabled;
      cause = Cause::kNone;
    } else {
      state = State::kDegraded;
      cause = !activity.hardware_active && activity.controller_loaded
                  ? Cause::kUnknown
                  : Cause::kAvailability;
    }
    // R7 package 3: a Real arm's vendor status. A vendor intervention cause
    // faults the domain with that cause (D8: CCLC prefers it over loss);
    // stale or missing vendor status is `availability` (after the target was
    // reached CCLC makes it `provider_lost`); its device facts join.
    if (inputs.device_status.has_value()) {
      const auto vendor = inputs.device_status->cause;
      if (core::lifecycle_cause_requires_intervention(vendor)) {
        cause = vendor;
        state = State::kFaulted;
        fault_present = true;
      } else if (vendor == Cause::kAvailability &&
                 !core::lifecycle_cause_requires_intervention(cause)) {
        cause = Cause::kAvailability;
        state = State::kDegraded;
      } else if ((requested == State::kPrepared || requested == State::kActive) &&
                 !core::lifecycle_cause_requires_intervention(cause) &&
                 std::any_of(inputs.device_status->facts.begin(),
                             inputs.device_status->facts.end(),
                             [](const core::LifecycleFactDTO &fact) {
                               return fact.lifecycle_fact_id.value() ==
                                          core::kDeviceMovedExternallyFactId &&
                                      fact.value.boolean_value == true;
                             })) {
        // m5.5: moved from the robot's own interface while armed: not servo
        // ready until ManyForge re-arms it (CCLC decides how).
        cause = Cause::kAvailability;
        state = State::kDegraded;
      }
      device_facts = inputs.device_status->facts;
      // R7 package 4: the activity the Real catalog sequences on.
      device_facts.push_back(boolean_fact(
          std::string(kBroadcasterActiveFactId),
          transport_ready && activity.broadcaster_active));
      device_facts.push_back(boolean_fact(
          std::string(kControllerActiveFactId),
          transport_ready && activity.controller_active));
      device_facts.push_back(boolean_fact(
          std::string(kControllerSpawnedFactId),
          transport_ready && activity.controller_loaded));
      device_facts.push_back(boolean_fact(
          std::string(kHardwareActiveFactId),
          transport_ready && activity.hardware_active));
      device_facts.push_back(boolean_fact(
          std::string(kMeasuredJointsValidFactId),
          inputs.measured_joints_valid));
    }
    // Only the active state proves the exact configuration (a fault or a
    // degraded binding never does).
    ready = ready && state == State::kActive;
    std::ostringstream stream;
    stream << "arm:" << activity.controller_manager_present << ':'
           << activity.observed << ':' << activity.controller_loaded << ':'
           << activity.controller_active << ':' << activity.hardware_active
           << ':' << bound << ':' << binding_ready << ':'
           << core::to_string(requested) << ':'
           << guard_fault.value_or("") << ':'
           << (inputs.unresponsive ? inputs.unresponsive->code : std::string());
    if (inputs.device_status.has_value())
      stream << device_status_signature(*inputs.device_status) << ":jsb:"
             << activity.broadcaster_active << ":measured:"
             << inputs.measured_joints_valid;
    signature = stream.str();
  } else if (is_gripper && gripper_id.has_value()) {
    const auto &snapshot = *inputs.gripper_snapshot;
    signature = gripper_signature(snapshot);
    const bool local_none = snapshot.output_mode == core::GripperOutputMode::kNone;
    const bool owner_ready = local_none ? snapshot.configured && snapshot.logical_owner_ready : snapshot.ready;
    // The existing no-CM owner path reports its loaded logical control owner.
    // Physical expectation is separately sealed not-applicable for this exact
    // None domain; physical command/feedback path flags remain false.
    transport_ready = owner_ready;
    ready = owner_ready;
    loaded = owner_ready;
    fault_present = !snapshot.configured;
    cause = local_none ? (owner_ready ? Cause::kNone : Cause::kAvailability) : gripper_transport_cause(snapshot);
    state = owner_ready     ? State::kActive
            : fault_present ? State::kFaulted
                            : State::kDegraded;
    if (local_none) {
      // Retain the existing no-CM initial active owner, then report only the
      // real local lifecycle request. No physical controller activity exists.
      if (owner_ready && gripper_requested != State::kDeclared)
        state = gripper_requested;
      ready = owner_ready && state == State::kActive;
      signature += ":logical:" + std::string(core::to_string(gripper_requested));
    } else if (!cm_gripper &&
               snapshot.transport_kind == "isaac_joint_state_effort") {
      // As before b9c5798c, the ready effort transport is the active owner
      // until a local lifecycle request is recorded (the no-output rule).
      if (owner_ready && gripper_requested != State::kDeclared)
        state = gripper_requested;
      ready = owner_ready && state == State::kActive;
      signature += ":local:" + std::string(core::to_string(gripper_requested));
    } else if (!cm_gripper) {
      // The action bridge has no controller-manager primitive. Its lifecycle
      // owns command admission while transport readiness remains observed.
      state = owner_ready ? gripper_requested
                          : (fault_present ? State::kFaulted
                                           : State::kDegraded);
      ready = owner_ready && state == State::kActive;
      if (inputs.gripper && gripper_id &&
          inputs.gripper->lifecycle_disable_pending(*gripper_id)) {
        state = State::kDegraded;
        ready = false;
        cause = Cause::kUnknown;
      }
      signature += ":bridge:" +
                   std::string(core::to_string(gripper_requested));
    }
    if (cm_gripper) {
      transport_ready = gripper_activity.controller_manager_present &&
                        gripper_activity.observed;
      loaded = transport_ready && gripper_activity.controller_loaded &&
               gripper_activity.hardware_active;
      ready = loaded && gripper_activity.controller_active && snapshot.ready &&
              gripper_requested == State::kActive;
      const bool wanted_active = gripper_requested == State::kActive;
      const bool matches_request =
          loaded && gripper_activity.controller_active == wanted_active &&
          (!wanted_active || ready);
      cause = fault_present     ? Cause::kFault
              : matches_request ? Cause::kNone
              : gripper_activity.controller_loaded &&
                      !gripper_activity.hardware_active
                  ? Cause::kUnknown
                  : Cause::kAvailability;
      state = fault_present     ? State::kFaulted
              : matches_request ? gripper_requested
                                : State::kDegraded;
      inactive_unrequested =
          loaded && wanted_active && !gripper_activity.controller_active;
      signature += ":cm:" + std::to_string(transport_ready) + ":" +
                   std::to_string(loaded) + ":" +
                   std::to_string(gripper_activity.controller_active) + ":" +
                   std::string(core::to_string(gripper_requested));
    }
  }

  return {state,
          ready,
          loaded,
          fault_present,
          transport_ready,
          inactive_unrequested,
          cause,
          std::move(diagnostic),
          std::move(signature),
          std::move(device_facts)};
}

core::Result<core::LifecycleDomainHealthDTO>
RetainedRos2ControlRuntimeHandle::ReadLifecycleHealth(
    const core::ControlDomainId &domain, const core::TimePoint now_ns) const {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  auto captured = CaptureLifecycleHealthInputs(domain, now_ns);
  if (!captured.ok())
    return core::Result<core::LifecycleDomainHealthDTO>::Err(captured.error());
  auto &inputs = captured.value();
  if (!inputs.is_motion && !(inputs.is_gripper && inputs.gripper_id))
    return reject<core::LifecycleDomainHealthDTO>(
        "ros2_control_lifecycle_domain_unknown",
        "Requested health domain is not bound to this ROS runtime");
  if (inputs.process_fault_source) {
    if (!inputs.process_health_source)
      return reject<core::LifecycleDomainHealthDTO>(
          "ros2_control_process_health_unavailable",
          "Exact process owner has no pure cached health projection");
    auto fault = inputs.process_health_source();
    if (!fault.ok())
      return core::Result<core::LifecycleDomainHealthDTO>::Err(fault.error());
    inputs.process_fault = std::move(fault.value());
  }
  if (auto endpoint_error = ReadEndpointDiagnostic())
    return core::Result<core::LifecycleDomainHealthDTO>::Err(*endpoint_error);
  ReadLifecycleMonitorFacts(inputs);
  if (inputs.device_adapter && !inputs.process_fault) {
    if (inputs.activity.controller_manager_present && inputs.activity.observed)
      inputs.device_adapter->ObserveHardwareActive(inputs.activity.hardware_active);
    inputs.device_status = inputs.device_adapter->ReadHealth(now_ns);
  }
  auto classification = ComputeLifecycleHealth(inputs);
  const auto reported = classification.cause == Cause::kAvailability &&
                                inputs.retained_cause != Cause::kNone
                            ? inputs.retained_cause
                            : classification.cause;
  core::LifecycleDomainHealthDTO result{
      classification.state,
      lifecycle_facts(classification.ready, classification.inactive_unrequested,
                      classification.loaded, classification.fault_present,
                      classification.transport_ready, reported,
                      classification.device_facts),
      std::move(classification.diagnostic)};
  if (inputs.is_gripper && inputs.gripper_id)
    result.has_activation_step =
        inputs.cm_gripper ||
        (inputs.gripper_snapshot &&
         inputs.gripper_snapshot->transport_kind == "ros_gripper_command" &&
         inputs.gripper_snapshot->output_mode != core::GripperOutputMode::kNone);
  if (!result.locally_valid())
    return reject<core::LifecycleDomainHealthDTO>(
        "ros2_control_lifecycle_health_invalid",
        "ROS lifecycle health projection is invalid");
  return core::Result<core::LifecycleDomainHealthDTO>::Ok(std::move(result));
}

core::Result<RetainedExternallyManagedDomainSnapshot>
RetainedRos2ControlRuntimeHandle::ReadLifecycleSnapshot(
    const core::ControlDomainId &domain, const core::TimePoint now_ns) {
  // Lifecycle Observe is already an NRT observation checkpoint. Refresh
  // endpoint facts here, while leaving its state/Disable/Recover semantics
  // unchanged. A newly established publisher identity deliberately needs a
  // following stable capture; perform that bounded capture at this same NRT
  // checkpoint so Prepare/Enable's immediately following pure-health read
  // does not expose an intermediate cache state. Either capture still fails
  // closed on missing, conflicting, or concurrently changing endpoints.
  RefreshEndpointFacts();
  RefreshEndpointFacts();
  auto captured = CaptureLifecycleHealthInputs(domain, now_ns);
  if (!captured.ok())
    return core::Result<RetainedExternallyManagedDomainSnapshot>::Err(
        captured.error());
  auto inputs = std::move(captured.value());
  // The original advancement callback remains exclusive to lifecycle Observe.
  inputs.process_fault = inputs.process_fault_source
                             ? inputs.process_fault_source()
                             : std::nullopt;
  if (!(inputs.process_fault && (inputs.is_motion || inputs.is_gripper)) &&
      !inputs.is_motion && !(inputs.is_gripper && inputs.gripper_id))
    return reject<RetainedExternallyManagedDomainSnapshot>(
        "ros2_control_lifecycle_domain_unknown",
        "requested lifecycle domain is not bound to this ROS runtime");
  ReadLifecycleMonitorFacts(inputs);
  if (inputs.device_adapter && !inputs.process_fault) {
    if (inputs.activity.controller_manager_present && inputs.activity.observed)
      inputs.device_adapter->ObserveHardwareActive(inputs.activity.hardware_active);
    inputs.device_status = inputs.device_adapter->Observe(now_ns);
  }
  auto classification = ComputeLifecycleHealth(inputs);
  return CommitLifecycleObservation(domain, std::move(inputs),
                                    std::move(classification));
}

core::Result<RetainedExternallyManagedDomainSnapshot>
RetainedRos2ControlRuntimeHandle::CommitLifecycleObservation(
    const core::ControlDomainId &control_domain_id,
    LifecycleHealthInputs inputs, LifecycleClassification classification) {
  using Cause = core::LifecycleBlockingCauseClassDTO;
  using State = core::ComponentLifecycleStateDTO;
  // Preserve the original epoch/activity recheck before committing discovery.
  if (inputs.discovery_attempted) {
    {
      std::lock_guard lock(mutex_);
      if (arm_ && arm_->adapter == inputs.adapter && arm_->bound &&
          arm_->requested == State::kActive &&
          arm_->activity.controller_manager_present &&
          arm_->activity.observed && arm_->activity.controller_loaded &&
          arm_->activity.hardware_active && arm_->activity.controller_active) {
        arm_->activation_client_discovered =
            arm_->activation_client_discovered ||
            inputs.activation_client_discovered;
        inputs.activation_client_discovered =
            arm_->activation_client_discovered;
      } else {
        // Original classification retained its captured flag if the recheck
        // failed; a pure discovery result was never committed to that epoch.
        inputs.activation_client_discovered = false;
      }
    }
    classification = ComputeLifecycleHealth(inputs);
  }
  auto &state = classification.state;
  auto &ready = classification.ready;
  auto &loaded = classification.loaded;
  auto &fault_present = classification.fault_present;
  auto &transport_ready = classification.transport_ready;
  auto &inactive_unrequested = classification.inactive_unrequested;
  auto &cause = classification.cause;
  auto &signature = classification.signature;
  auto &diagnostic = classification.diagnostic;
  const auto &process_fault = inputs.process_fault;
  auto diagnostic_snapshot = std::move(inputs.diagnostic_snapshot);
  std::lock_guard lock(mutex_);
  auto &generation = domain_generations_[control_domain_id];
  // An availability observation cannot clear an intervention-requiring cause;
  // any other classification is fresh status and replaces it.
  const auto reported =
      cause == Cause::kAvailability &&
              generation.last_intervention_cause != Cause::kNone
          ? generation.last_intervention_cause
          : cause;
  if (cause != Cause::kAvailability) {
    generation.last_intervention_cause =
        core::lifecycle_cause_requires_intervention(cause) ? cause
                                                           : Cause::kNone;
  }
  generation.exact_configuration_seen =
      generation.exact_configuration_seen || ready;
  signature += ":cause:";
  signature += core::to_string(reported);
  if (generation.first_observation_pending) {
    if (generation.publication_generation == 0U)
      generation.publication_generation = 1U;
    generation.first_observation_pending = false;
    generation.source_signature = std::move(signature);
  } else if (generation.source_signature != signature) {
    if (generation.publication_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      return reject<RetainedExternallyManagedDomainSnapshot>(
          "ros2_control_lifecycle_generation_exhausted",
          "ROS lifecycle observation generation is exhausted");
    }
    ++generation.publication_generation;
    generation.source_signature = std::move(signature);
  }
  RetainedExternallyManagedDomainSnapshot result{
      generation.publication_generation,
      state,
      ready,
      lifecycle_facts(ready, inactive_unrequested, loaded, fault_present,
                      transport_ready, reported, classification.device_facts),
      std::move(diagnostic),
      process_fault ? std::move(diagnostic_snapshot) : nullptr};
  if (!result.locally_valid()) {
    return reject<RetainedExternallyManagedDomainSnapshot>(
        "ros2_control_lifecycle_snapshot_invalid",
        "ROS lifecycle normalization produced an invalid snapshot");
  }
  return core::Result<RetainedExternallyManagedDomainSnapshot>::Ok(
      std::move(result));
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
