#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_ros2_control_execution/selected_ros_endpoint_facts.hpp>

namespace manyforge::roboplan_backend {
namespace {

using ActivityMessage = controller_manager_msgs::msg::ControllerManagerActivity;
using SwitchService = controller_manager_msgs::srv::SwitchController;

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
// `manyforge.blocking_cause_class` id.
[[nodiscard]] std::vector<core::LifecycleFactDTO>
lifecycle_facts(const bool controller_ready,
                const bool controller_inactive_unrequested,
                const bool controller_loaded, const bool fault_present,
                const bool transport_ready,
                const core::LifecycleBlockingCauseClassDTO cause) {
  return {
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

[[nodiscard]] RetainedExternallyManagedPrimitiveOutcome
primitive_outcome(RetainedRos2ControlSwitchResult result) {
  using Dispatch = core::LifecyclePrimitiveDispatchStateDTO;
  using Effect = core::LifecyclePrimitiveEffectDTO;
  using Known = core::LifecyclePrimitiveKnownResultDTO;
  switch (result.outcome) {
  case RetainedRos2ControlSwitchOutcome::kNotSent:
    return {Dispatch::kNotDispatched, Effect::kNone, std::nullopt,
            std::move(result.error)};
  case RetainedRos2ControlSwitchOutcome::kSucceeded:
    return {Dispatch::kDispatchedKnown, Effect::kObserved, Known::kSucceeded,
            std::nullopt};
  case RetainedRos2ControlSwitchOutcome::kFailed:
    return {Dispatch::kDispatchedKnown, Effect::kPossible, Known::kFailed,
            std::move(result.error)};
  case RetainedRos2ControlSwitchOutcome::kUncertain:
    return {Dispatch::kDispatchedUncertain, Effect::kPossible, std::nullopt,
            std::move(result.error)};
  }
  return {Dispatch::kDispatchedUncertain, Effect::kPossible, std::nullopt,
          std::move(result.error)};
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
          std::vector<std::string> compiled, std::vector<std::string> controller,
          std::shared_ptr<const Gate> gate)
      : adapter_(std::move(adapter)), compiled_(std::move(compiled)),
        controller_(std::move(controller)), gate_(std::move(gate)) {}

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
  /// Q54: the controller manager's statistics for this controller.
  std::shared_ptr<RetainedControllerStatisticsMonitor> statistics_monitor;
  rclcpp::Subscription<pal_statistics_msgs::msg::Statistics>::SharedPtr
      statistics_subscription;
};

// Aliasing reader leases own the channel, including its real receiver clock.
// The callback retains no handle or executor whose last destruction could drain
// the callback's own executor thread.
struct PreparedMeasuredStateSources::Storage final {
  struct Channel final {
    std::unique_ptr<core::AddressedObservedStatePublication> publication;
    std::vector<double> positions;
    std::vector<double> velocities;
  };
  Storage(std::vector<core::MeasuredStateSourceDescriptor> descriptors,
          std::shared_ptr<core::IClock> clock, rclcpp::Node::SharedPtr node)
      : descriptors(std::move(descriptors)), clock(std::move(clock)),
        node(std::move(node)) {}
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
    const std::optional<core::RuntimeParticipantBinding> &candidate_gripper) {
  using Sources = std::shared_ptr<PreparedMeasuredStateSources>;
  using Readers = std::vector<std::shared_ptr<const core::IObservedStateProvider>>;
  std::lock_guard lock(mutex_);
  if (!resources_ || !arm_ || !receiver_clock ||
      clock_ != receiver_clock.get() || weak_from_this().expired())
    return reject<Sources>(
        "measured_state_owner_mismatch",
        "Measured source requires the bound node and its exact owning clock.");
  if (!motion_binding_ ||
      candidate_gripper.has_value() != gripper_binding_.has_value())
    return reject<Sources>(
        "measured_state_candidate_owner_mismatch",
        "Measured candidate must retain the bound motion and tool owners.");
  const auto motion_difference = measured_candidate_physical_owner_difference(
      *motion_binding_, candidate_motion);
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
  if (!arm_->transport.measured_joint_state_lane)
    return reject<Sources>(
        "measured_state_lane_unavailable",
        "Bound lane has no qualified continuous measured source.");
  const auto &lane = *arm_->transport.measured_joint_state_lane;
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
    if ((!matches(&candidate_motion) &&
         !matches(candidate_gripper ? &*candidate_gripper : nullptr)) ||
        config.source_precedence.front().source_id != lane.absolute_topic ||
        config.source_precedence.front().authority_profile_id !=
            lane.measured_source_profile_id)
      return reject<Sources>(
          "measured_state_physical_source_mismatch",
          "Measured source differs from the actual bound physical lane.");
  }
  auto replacement = std::make_shared<PreparedMeasuredStateSources::Storage>(
      std::move(descriptors), std::move(receiver_clock), resources_->node());
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
                                     std::vector<double>(count)});
  }
  if (!replacement->channels.empty()) {
    const std::weak_ptr<PreparedMeasuredStateSources::Storage> weak_channels =
        replacement;
    try {
      replacement->subscription = replacement->node->create_subscription<
          sensor_msgs::msg::JointState>(
          lane.absolute_topic, rclcpp::SensorDataQoS().keep_last(1),
          [weak_channels](
              const sensor_msgs::msg::JointState::ConstSharedPtr message) {
            const auto channels = weak_channels.lock();
            if (!channels)
              return;
            std::lock_guard publication_lock(channels->mutex);
            if (!channels->active)
              return;
            const auto received = channels->clock->now();
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
                (void)channel.publication->publish(core::ObservedStateSourceKind::kJointState,
                    message->name, message->position, message->velocity, source_time, received);
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
              (void)channel.publication->publish(core::ObservedStateSourceKind::kJointState,
                  config.query_context.joint_names,
                  complete ? std::span<const double>(channel.positions) : std::span<const double>{},
                  complete && velocities ? std::span<const double>(channel.velocities) :
                                           std::span<const double>{},
                  source_time, received);
            }
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

RetainedRos2ControlRuntimeHandle::~RetainedRos2ControlRuntimeHandle() {
  // The lane guard stops first (it calls back into this handle). Children
  // then close admission while callbacks and borrowed resources are
  // quiesced; destructors repeat only their idempotent local shutdown.
  std::optional<RetainedRos2ControlArmGuardHooks> hooks;
  {
    std::lock_guard lock(mutex_);
    hooks = std::move(guard_hooks_);
    guard_hooks_.reset();
  }
  hooks.reset();
  if (arm_ && arm_->adapter)
    arm_->adapter->shutdown();
  if (gripper_)
    gripper_->shutdown();
  if (resources_)
    resources_->quiesce();
  sample_timer_.reset();
  sample_observer_ = {};
  participant_.reset();
  arm_.reset();
  gripper_.reset();
  resources_.reset();
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
        state->transport.adapter.lane_facts.joint_names, state->gate);
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

void RetainedRos2ControlRuntimeHandle::InstallSampleObserver(
    std::function<void(const core::ExecutionControllerSampleDTO &)> observer) {
  std::lock_guard lock(mutex_);
  if (!arm_ || start_attempted_ || sample_observer_ || !observer) {
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
  gripper_binding_ = std::move(binding);
  return core::Result<void>::Ok();
}

void RetainedRos2ControlRuntimeHandle::StartBackend() {
  std::lock_guard lock(mutex_);
  if (start_failure_ != nullptr)
    std::rethrow_exception(start_failure_);
  if (start_attempted_)
    return;
  if (!arm_ || resources_ == nullptr)
    throw std::logic_error("ROS initial installation has no live backend");
  start_attempted_ = true;
  try {
    if (gripper_)
      gripper_->complete_ros_configuration();
    const auto node = resources_->node();
    const auto &controller = arm_->transport.controller;
    const auto gripper_controller = arm_->transport.gripper_controller;
    const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak =
        weak_from_this();
    arm_->activity_endpoints =
        std::make_shared<ros2_control_execution::SelectedRosEndpointFacts>(
            node, controller.controller_manager + "/activity",
            "controller_manager_msgs/msg/ControllerManagerActivity");
    const auto endpoint_facts = arm_->activity_endpoints;
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
                  activity.hardware_active = false;
            }
            self->ObserveActivity(activity);
          }
        };
    arm_->activity_subscription = node->create_subscription<ActivityMessage>(
        controller.controller_manager + "/activity",
        rclcpp::QoS(1).reliable().transient_local(),
        [weak, controller,
         gripper_controller](const ActivityMessage::ConstSharedPtr message) {
          const auto self = weak.lock();
          if (!self)
            return;
          RetainedRos2ControlArmActivity activity;
          activity.controller_manager_present = true;
          activity.observed = true;
          for (const auto &row : message->controllers) {
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
        },
        options);
    arm_->switch_client = node->create_client<SwitchService>(
        controller.controller_manager + "/switch_controller");
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
    if (sample_observer_) {
      const auto rate = arm_->transport.adapter.lane_facts.state_rate_hz;
      if (!(rate > 0.0))
        throw std::invalid_argument("ROS arm lane has no controller state rate");
      sample_timer_ = node->create_wall_timer(
          std::chrono::nanoseconds(static_cast<std::int64_t>(1e9 / rate)),
          [weak]() {
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
    if (!arm_)
      return;
    activity = arm_->activity_endpoints;
    adapter = arm_->adapter;
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
    if (!arm_)
      return std::nullopt;
    activity = arm_->activity_endpoints;
    adapter = arm_->adapter;
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
  if (!arm_)
    return;
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
}

void RetainedRos2ControlRuntimeHandle::PublishReadinessLocked() {
  const auto &activity = arm_->activity;
  core::ExecutionReadinessDTO readiness;
  readiness.binding_epoch = core::ExecutionBindingEpoch(arm_->epoch);
  readiness.controller_active =
      activity.controller_manager_present && activity.controller_active;
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
        arm_->transport.adapter.lane_facts.joint_names, arm_->gate);
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
    const std::optional<std::string> selected_controller) {
  rclcpp::Client<SwitchService>::SharedPtr client;
  std::string controller;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !arm_->switch_client)
      return not_sent("ros2_control_switch_unavailable",
                      "the controller-manager switch client is not started");
    client = arm_->switch_client;
    controller = selected_controller.value_or(
        arm_->transport.controller.controller_name);
  }
  if (!client->service_is_ready())
    return not_sent("ros2_control_switch_service_unavailable",
                    "the controller manager's switch_controller service is not "
                    "available");
  auto request = std::make_shared<SwitchService::Request>();
  (activate ? request->activate_controllers : request->deactivate_controllers)
      .push_back(controller);
  request->strictness = SwitchService::Request::STRICT;
  request->activate_asap = false;
  auto future = client->async_send_request(request);
  while (future.wait_for(std::chrono::milliseconds(10)) !=
         std::future_status::ready) {
    if (cancellation.stop_requested()) {
      client->remove_pending_request(future);
      return {RetainedRos2ControlSwitchOutcome::kUncertain,
              switch_error("ros2_control_switch_cancelled",
                           "the controller switch was cancelled before its "
                           "answer; its effect is unknown")};
    }
  }
  const auto response = future.get();
  if (!response || !response->ok)
    return {RetainedRos2ControlSwitchOutcome::kFailed,
            switch_error(activate ? "ros2_control_activation_refused"
                                  : "ros2_control_deactivation_refused",
                         "the controller manager refused the controller "
                         "switch")};
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
    const std::stop_token cancellation) {
  std::lock_guard switch_lock(switch_mutex_);
  RefreshEndpointFacts();
  if (auto error = ReadEndpointDiagnostic())
    return {RetainedRos2ControlSwitchOutcome::kNotSent, std::move(error)};
  std::function<core::Result<void>()> before_activation;
  {
    std::lock_guard lock(mutex_);
    if (!arm_ || !participant_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    if (guard_hooks_.has_value())
      before_activation = guard_hooks_->before_activation;
  }
  if (before_activation) {
    const auto gated = before_activation();
    if (!gated.ok())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, gated.error()};
  }
  {
    std::lock_guard lock(mutex_);
    const auto rotated = RotateEpochLocked();
    if (!rotated.ok())
      return {RetainedRos2ControlSwitchOutcome::kNotSent, rotated.error()};
  }
  const auto result = SendSwitch(true, cancellation);
  std::lock_guard lock(mutex_);
  if (result.outcome != RetainedRos2ControlSwitchOutcome::kNotSent)
    arm_->epoch_used = true;  // it may be active on this epoch now
  if (result.outcome == RetainedRos2ControlSwitchOutcome::kSucceeded) {
    arm_->bound = true;
    arm_->active_seen_while_bound = arm_->activity.controller_manager_present &&
                                    arm_->activity.controller_active;
    arm_->requested = core::ComponentLifecycleStateDTO::kActive;
    PublishReadinessLocked();
  }
  return result;
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::DeactivateArmController(
    const std::stop_token cancellation) {
  std::lock_guard switch_lock(switch_mutex_);
  bool active = false;
  {
    std::lock_guard lock(mutex_);
    if (!arm_)
      return not_sent("ros2_control_arm_unbound", "no arm controller is bound");
    active = !arm_->activity.observed || arm_->activity.controller_active;
  }
  const auto result =
      active ? SendSwitch(false, cancellation)
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
  return SendSwitch(false, cancellation);
}

RetainedRos2ControlSwitchResult
RetainedRos2ControlRuntimeHandle::RecoverArmController(
    const std::stop_token cancellation) {
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
    const auto result = SendSwitch(false, cancellation);
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
  if (!arm_ || !arm_->transport.gripper_controller ||
      !gripper_activity_.controller_manager_present ||
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
    const bool activate, const std::stop_token cancellation) {
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
    if (!arm_ || !arm_->transport.gripper_controller)
      return not_sent("ros2_control_gripper_controller_unbound",
                      "no exact controller-manager gripper is bound");
    controller = arm_->transport.gripper_controller->controller_name;
    needs_switch = activate || !gripper_activity_.observed ||
                   gripper_activity_.controller_active;
  }
  const auto result =
      needs_switch
          ? SendSwitch(activate, cancellation, controller)
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
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> weak = weak_from_this();
  return [weak](const core::ControlDomainId &domain,
                const core::LifecyclePrimitiveId &primitive,
                const std::stop_token cancellation) {
    const auto self = weak.lock();
    if (self) {
      bool gripper_domain = false;
      {
        std::lock_guard lock(self->mutex_);
        gripper_domain = self->arm_ &&
                         self->arm_->transport.gripper_controller &&
                         self->gripper_domain_id_ == domain;
      }
      if (gripper_domain) {
        if (primitive.value() == "ros2_control.controller.prepare")
          return primitive_outcome(self->PrepareGripperController());
        if (primitive.value() == "ros2_control.controller.activate")
          return primitive_outcome(
              self->SwitchGripperController(true, cancellation));
        if (primitive.value() == "ros2_control.controller.deactivate")
          return primitive_outcome(
              self->SwitchGripperController(false, cancellation));
        return primitive_outcome(
            not_sent("ros2_control_lifecycle_primitive_unknown",
                     "the gripper has no such controller primitive"));
      }
    }
    if (!self || self->motion_domain() != domain) {
      return primitive_outcome(
          not_sent("ros2_control_lifecycle_primitive_domain_unsupported",
                   "this control domain has no ros2_control controller "
                   "primitive"));
    }
    const auto &id = primitive.value();
    if (id == "ros2_control.controller.prepare")
      return primitive_outcome(self->PrepareArmController());
    if (id == "ros2_control.controller.activate")
      return primitive_outcome(self->ActivateArmController(cancellation));
    if (id == "ros2_control.controller.deactivate")
      return primitive_outcome(self->DeactivateArmController(cancellation));
    if (id == "ros2_control.controller.recover")
      return primitive_outcome(self->RecoverArmController(cancellation));
    return primitive_outcome(
        not_sent("ros2_control_lifecycle_primitive_unknown",
                 "the ros2_control catalog has no such primitive"));
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
  const auto self = shared_from_this();
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
  std::optional<std::string> guard_fault;
  std::shared_ptr<ros2_control_execution::Ros2ControlExecutionBackend> adapter;
  ros2_control_execution::Ros2GripperCommandBackend *gripper{};
  std::optional<std::string> gripper_id;
  std::optional<ros2_control_execution::Ros2GripperTransportReadinessSnapshot>
      gripper_snapshot;
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot;
  std::string diagnostic_incident_id;
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
};

core::Result<RetainedRos2ControlRuntimeHandle::LifecycleHealthInputs>
RetainedRos2ControlRuntimeHandle::CaptureLifecycleHealthInputs(
    const core::ControlDomainId &domain, const core::TimePoint now_ns) const {
  std::lock_guard lock(mutex_);
  if (!arm_ || now_ns < 0 || domain.empty())
    return reject<LifecycleHealthInputs>(
        "ros2_control_lifecycle_source_unavailable",
        "ROS runtime lifecycle source is unbound or unavailable");
  LifecycleHealthInputs inputs;
  inputs.is_motion = motion_domain_id_ == domain;
  inputs.is_gripper = gripper_ != nullptr && gripper_domain_id_ == domain;
  inputs.gripper = gripper_.get();
  inputs.gripper_id = gripper_id_;
  inputs.activity = arm_->activity;
  inputs.gripper_activity = gripper_activity_;
  inputs.gripper_requested = gripper_requested_;
  inputs.cm_gripper = arm_->transport.gripper_controller.has_value();
  inputs.requested = arm_->requested;
  inputs.bound = arm_->bound;
  inputs.activation_client_discovered = arm_->activation_client_discovered;
  inputs.adapter = arm_->adapter;
  if (guard_hooks_)
    inputs.fault_hook = guard_hooks_->fault;
  inputs.process_fault_source = process_fault_source_;
  inputs.process_health_source = process_health_source_;
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
  if (inputs.process_fault && (inputs.is_motion || inputs.is_gripper))
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
  // Q80: an exited Manager-owned controller launch fails the component.
  const auto &process_fault = inputs.process_fault;
  if (process_fault.has_value() && (is_motion || is_gripper)) {
    state = State::kFaulted;
    cause = Cause::kFault;
    fault_present = true;
    auto message = *process_fault;
    if (message.size() > core::kMaxLifecycleErrorMessageBytes) {
      auto end = core::kMaxLifecycleErrorMessageBytes;
      while (end > 0U &&
             (static_cast<unsigned char>(message[end]) & 0xC0U) == 0x80U)
        --end;
      message.resize(end);
    }
    diagnostic =
        core::ErrorDTO{"controller_launch_exited", std::move(message), {}};
    signature = std::string(is_motion ? "arm" : "gripper") +
                ":controller_process:" + *process_fault +
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
    fault_present = guard_fault.has_value();
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
    } else {
      state = State::kDegraded;
      cause = !activity.hardware_active && activity.controller_loaded
                  ? Cause::kUnknown
                  : Cause::kAvailability;
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
           << guard_fault.value_or("");
    signature = stream.str();
  } else if (is_gripper && gripper_id.has_value()) {
    const auto &snapshot = *inputs.gripper_snapshot;
    signature = gripper_signature(snapshot);
    transport_ready = snapshot.ready;
    ready = snapshot.ready;
    loaded = snapshot.ready;
    fault_present = !snapshot.configured;
    cause = gripper_transport_cause(snapshot);
    state = snapshot.ready  ? State::kActive
            : fault_present ? State::kFaulted
                            : State::kDegraded;
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
          std::move(signature)};
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
  auto classification = ComputeLifecycleHealth(inputs);
  const auto reported = classification.cause == Cause::kAvailability &&
                                inputs.retained_cause != Cause::kNone
                            ? inputs.retained_cause
                            : classification.cause;
  core::LifecycleDomainHealthDTO result{
      classification.state,
      lifecycle_facts(classification.ready, classification.inactive_unrequested,
                      classification.loaded, classification.fault_present,
                      classification.transport_ready, reported),
      std::move(classification.diagnostic)};
  if (inputs.is_gripper && inputs.gripper_id)
    result.has_activation_step = inputs.cm_gripper;
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
  // unchanged. Prepare/Enable Initialize consumes the separate pure health.
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
                      transport_ready, reported),
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
