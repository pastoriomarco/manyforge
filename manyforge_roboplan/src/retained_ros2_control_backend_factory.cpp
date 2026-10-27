#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <span>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <manyforge_roboplan/host_component_stream_health.hpp>
#include <manyforge_roboplan/retained_execution_state_sink.hpp>

namespace manyforge::roboplan_backend {
namespace {

/// CP9: the gripper's `gripper_feedback` stream with the component's row
/// budget/severity/recovery (carried in the gripper config). A gripper
/// without output declares none.
/// - Isaac effort transport: declared here; the backend records the arrival
///   of every matched raw feedback sample.
/// - Mock (stock GripperCommand): its sample is the gripper joint in the
///   controller manager's measured JointState, so the declaration rides the
///   measured lane; the runtime handle declares it with that lane and records
///   the stamp of every message carrying the joint.
void declare_gripper_feedback_stream(RetainedRos2ControlBackendConfig &config,
                                     const core::RuntimeParticipantBinding &binding) {
  auto &gripper = *config.gripper;
  if (gripper.output_mode == core::GripperOutputMode::kNone) return;
  core::StreamDeclarationDTO defaults{
      "gripper_feedback",
      static_cast<core::Duration>(gripper.state_freshness_timeout_s * 1e9),
      gripper.feedback_stream_severity, gripper.feedback_stream_recovery,
      core::StreamTimestampSourceDTO::kArrival};
  if (gripper.transport_kind == "ros_gripper_command") {
    if (config.arm.measured_joint_state_lane) {
      defaults.timestamp_source = core::StreamTimestampSourceDTO::kMessageStamp;
      config.arm.measured_joint_state_lane->gripper_feedback_stream = std::move(defaults);
    }
    return;
  }
  if (gripper.transport_kind != "isaac_joint_state_effort" || gripper.state_topic.empty())
    return;
  auto declared = host_component_stream_health()->Declare(
      binding.value().controllable_component_id, std::move(defaults));
  if (!declared.ok()) throw std::invalid_argument(declared.error().message);
  gripper.feedback_stream = std::move(declared.value());
}

class SingleUseRos2ControlBackendConfig final {
public:
  explicit SingleUseRos2ControlBackendConfig(
      RetainedRos2ControlBackendConfig config)
      : config_(std::move(config)) {}

  [[nodiscard]] RetainedRos2ControlBackendConfig take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.has_value()) {
      throw std::logic_error(
          "retained ros2_control backend factory was already consumed");
    }
    auto selected = std::move(*config_);
    config_.reset();
    return selected;
  }

private:
  std::mutex mutex_;
  std::optional<RetainedRos2ControlBackendConfig> config_;
};

[[nodiscard]] std::vector<std::string>
compiled_resources(const core::RuntimeParticipantBinding &binding) {
  std::vector<std::string> result;
  result.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    if (!resource.source_joint_name.has_value() ||
        resource.compiled_resource_name.empty()) {
      throw std::invalid_argument(
          "ros2_control motion binding contains a non-model resource");
    }
    result.push_back(resource.compiled_resource_name);
  }
  return result;
}

[[nodiscard]] std::vector<std::string>
source_resources(const core::RuntimeParticipantBinding &binding) {
  std::vector<std::string> result;
  result.reserve(binding.value().resources.size());
  for (const auto &resource : binding.value().resources) {
    if (!resource.source_joint_name.has_value() ||
        resource.source_joint_name->empty()) {
      throw std::invalid_argument(
          "ros2_control motion binding contains a non-model resource");
    }
    result.push_back(*resource.source_joint_name);
  }
  return result;
}

[[nodiscard]] std::string
sole_compiled_group(const core::RuntimeParticipantBinding &binding,
                    const char *description) {
  if (binding.value().model_groups.size() != 1U ||
      binding.value().model_groups.front().compiled_group_name.empty()) {
    throw std::invalid_argument(std::string(description) +
                                " has no sole compiled model group");
  }
  return binding.value().model_groups.front().compiled_group_name;
}

/// The lane inputs that have no decided value yet refuse composition with
/// their exact names (no default is chosen here).
void require_decided_lane_inputs(const core::ExecutionPortParticipantConfig &config) {
  std::string missing;
  if (!config.stop.replacement_start_allowance_s.has_value()) {
    missing += " replacement_start_allowance_s (Q26/Q42)";
  }
  if (!(config.late_cycle_margin_s > 0.0)) {
    missing += " late_cycle_margin_s (Q52)";
  }
  if (!missing.empty()) {
    throw std::invalid_argument(
        "ros2_control lane has no decided value for required inputs:" + missing);
  }
}

/// Waits until `deadline`, a cancellation, or the lane's next controller
/// sample period, whichever comes first (the participant re-reads the port
/// after every return).
[[nodiscard]] core::StopStageWaiter make_sample_period_waiter(
    const core::IClock &clock, const double state_rate_hz) {
  if (!(state_rate_hz > 0.0)) {
    throw std::invalid_argument("ros2_control lane has no controller state rate");
  }
  const auto period = static_cast<core::Duration>(1e9 / state_rate_hz);
  return [&clock, period](const core::TimePoint deadline, const std::stop_token &cancellation) {
    if (cancellation.stop_requested()) return;
    const auto wait = std::clamp<core::Duration>(deadline - clock.now(), 0, period);
    std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
  };
}

} // namespace

RetainedObservedStateChannelConfig retained_ros2_control_motion_state_config(
    const RetainedRos2ControlBackendConfig &config) {
  if (config.participant.stop.trip_authority == core::StateAuthorityClass::kMeasured) {
    // CP8 step 1: a measured authority's addressed arm state is the bound
    // lane's measured JointState, with the lane's freshness budget. There is
    // no commanded or seeded fallback: an absent, incomplete (NaN) or stale
    // sample leaves the state unavailable.
    const auto &lane = config.arm.measured_joint_state_lane;
    if (!lane.has_value() || lane->absolute_topic.empty() ||
        lane->absolute_topic.front() != '/' || lane->freshness_budget_ns <= 0 ||
        lane->measured_source_profile_id.empty() ||
        config.observed_state_authority_profile_id !=
            lane->measured_source_profile_id) {
      throw std::invalid_argument(
          "ros2_control measured arm state requires its qualified measured "
          "joint-state lane");
    }
    return RetainedObservedStateChannelConfig{
        core::ObservedStateSourceKind::kJointState,
        lane->absolute_topic,
        lane->freshness_budget_ns,
        1U,
        {core::ObservedRobotStateSourceConfig{
            core::ObservedRobotStateSourceKind::kJointState, lane->absolute_topic,
            core::ObservedRobotStateAuthorityProfile::kMeasured,
            lane->freshness_budget_ns, lane->measured_source_profile_id}}};
  }
  if (config.participant.stop.trip_authority != core::StateAuthorityClass::kCommanded ||
      config.observed_state_authority_profile_id.empty()) {
    throw std::invalid_argument(
        "ros2_control runtime composes commanded-simulation or measured arm "
        "state only");
  }
  // Non-expiring commanded source (item 20): the controller's own reference.
  const std::string source_id = "commanded_state";
  return RetainedObservedStateChannelConfig{
      core::ObservedStateSourceKind::kCommandedState,
      source_id,
      0,
      1U,
      {core::ObservedRobotStateSourceConfig{
          core::ObservedRobotStateSourceKind::kCommandedState, source_id,
          core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0,
          config.observed_state_authority_profile_id}}};
}

RetainedObservedStateChannelConfig retained_ros2_control_gripper_state_config(
    const core::RuntimeParticipantBinding &binding) {
  if (!binding.value().control_path.has_value() ||
      binding.value().control_path->state_authority_class !=
          core::StateAuthorityClass::kCommanded) {
    throw std::invalid_argument(
        "ros2_control simulation gripper requires commanded state authority");
  }
  const std::string source_id =
      binding.value().command_target_id.value() + ".commanded_state";
  return RetainedObservedStateChannelConfig{
      core::ObservedStateSourceKind::kCommandedState,
      source_id,
      0,
      1U,
      {core::ObservedRobotStateSourceConfig{
          core::ObservedRobotStateSourceKind::kCommandedState, source_id,
          core::ObservedRobotStateAuthorityProfile::kCommandedSimulation, 0,
          "ros2_control_gripper_commanded_sim_v1"}}};
}

RetainedRuntimeBackendFactory make_retained_ros2_control_backend_factory(
    RetainedRos2ControlBackendConfig config,
    std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle) {
  auto single_use =
      std::make_shared<SingleUseRos2ControlBackendConfig>(std::move(config));
  if (!runtime_handle) {
    runtime_handle = RetainedRos2ControlRuntimeHandle::Create();
  }
  return
      [single_use = std::move(single_use),
       runtime_handle = std::move(runtime_handle)](
          const core::RuntimeParticipantBinding &motion_binding,
          const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
          const std::shared_ptr<RetainedObservedStateChannel> &motion_channel,
          const std::shared_ptr<RetainedObservedStateChannel> &gripper_channel,
          core::IClock &clock,
          core::IScheduler &scheduler) -> RetainedRuntimeBackendSet {
        auto selected = single_use->take();
        require_decided_lane_inputs(selected.participant);
        if (selected.arm.measured_joint_state_lane.has_value()) {
          if (!selected.measured_receiver_clock) {
            throw std::invalid_argument(
                "measured_receiver_clock_missing: measured lane requires its "
                "owning receiver clock");
          }
          if (selected.measured_receiver_clock.get() != &clock) {
            throw std::invalid_argument(
                "measured_receiver_clock_mismatch: measured lane requires the "
                "exact factory receiver clock");
          }
        }
        if (!motion_channel ||
            !motion_binding.value().control_path.has_value() ||
            motion_binding.value().control_path->state_authority_class !=
                selected.participant.stop.trip_authority) {
          throw std::invalid_argument(
              "ros2_control backend differs from the retained state authority");
        }
        // The controller's joints are the binding's source joints, in the
        // binding's coordinate order; Core keeps the compiled names.
        const auto compiled = compiled_resources(motion_binding);
        if (source_resources(motion_binding) !=
            selected.arm.adapter.lane_facts.joint_names) {
          throw std::invalid_argument(
              "ros2_control controller joints differ from retained source "
              "resources");
        }
        selected.arm.compiled_joint_names = compiled;
        const auto motion_group =
            sole_compiled_group(motion_binding, "ros2_control motion binding");

        std::vector<RetainedExecutionStateSinkBinding> sink_bindings{
            {motion_group, motion_channel, false}};
        if (gripper_binding.has_value()) {
          if (!selected.gripper.has_value() || !gripper_channel) {
            throw std::invalid_argument(
                "retained ros2_control gripper product is incomplete");
          }
          const auto gripper_group = sole_compiled_group(
              *gripper_binding, "ros2_control gripper binding");
          if (selected.gripper->group_id != gripper_group ||
              gripper_binding->value().resources.size() != 1U ||
              selected.gripper->joint_name != gripper_binding->value()
                                                  .resources.front()
                                                  .compiled_resource_name) {
            throw std::invalid_argument(
                "ros2_control gripper config differs from retained resources");
          }
          sink_bindings.push_back({gripper_group, gripper_channel, true});
          // No-feedback pneumatic output uses the commanded-state sink; the
          // display callback remains for pneumatic feedback paths.
          if (selected.gripper->actuation_kind == core::GripperActuationKind::kPneumatic) {
            // Gripper callbacks run on the handle's executor: internal lease.
            const std::weak_ptr<RetainedRos2ControlRuntimeHandle> owner =
                runtime_handle->weak_from_this();
            selected.gripper->publish_display_position = [owner](const double position) {
              const auto current = owner.lock();
              return current ? current->PublishGripperDisplayPosition(position)
                  : core::Result<void>::Err({"gripper_display_owner_unavailable",
                      "Display owner has retired", {}});
            };
          }
        } else if (selected.gripper.has_value()) {
          throw std::invalid_argument(
              "ros2_control product config contains an unbound gripper");
        }

        auto sink = std::make_shared<RetainedExecutionStateSink>(
            clock, std::move(sink_bindings));
        auto resources =
            std::make_unique<ros2_control_execution::Ros2TransportResources>(
                selected.node_name, selected.node_namespace,
                selected.executor_thread_count, sink, motion_group, clock,
                selected.ros_context_provider);
        std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend>
            gripper;
        if (gripper_binding.has_value()) {
          gripper = std::make_unique<
              ros2_control_execution::Ros2GripperCommandBackend>(
              *resources, sink, clock, scheduler, true);
          declare_gripper_feedback_stream(selected, *gripper_binding);
          gripper->configure_gripper_action(
              gripper_binding->value().command_target_id.value(),
              *selected.gripper);
        }
        auto *const gripper_pointer = gripper.get();
        const auto state_rate_hz = selected.arm.adapter.lane_facts.state_rate_hz;
        auto participant_config = selected.participant;
        if (const auto trace = selected.arm.adapter.motion_trace) {
          const auto action = selected.arm.adapter.action_name;
          participant_config.observe_goal_context = [trace, action](const auto& key,
              const auto& participant, const auto epoch) {
            trace->Preregister(action, epoch, key, participant);
          };
        }
        const core::ExecutionPortParticipantStateAuthority state_authority{
            &motion_channel->publication().numeric_publication(),
            [] { return core::Result<void>::Ok(); }};
        const auto bound = runtime_handle->Bind(
            std::move(resources), clock, std::move(selected.arm),
            [&](std::shared_ptr<core::IExecutionPort> port) {
              return std::make_shared<core::ExecutionPortParticipant>(
                  motion_binding, std::move(port), clock, scheduler,
                  make_sample_period_waiter(clock, state_rate_hz),
                  state_authority, std::move(participant_config));
            },
            std::move(gripper), motion_binding, gripper_binding,
            selected.successor_occurrence,
            std::move(selected.measured_receiver_clock));
        if (!bound.ok()) {
          throw std::invalid_argument(bound.error().message);
        }

        if (gripper_binding && selected.gripper->actuation_kind == core::GripperActuationKind::kPneumatic) {
          const auto display = runtime_handle->BindGripperDisplayChannel(gripper_channel);
          if (!display.ok()) throw std::invalid_argument(display.error().message);
        }

        const std::weak_ptr<RetainedObservedStateChannel> weak_channel =
            motion_channel;
        if (selected.participant.stop.trip_authority ==
            core::StateAuthorityClass::kMeasured) {
          // CP8 step 1: the measured lane's JointState is the addressed arm
          // state. Samples are reordered by the controller's (source) joint
          // names; a missing, duplicated or non-finite joint publishes an
          // incomplete sample, which replaces the prior one (never a fill).
          runtime_handle->InstallMeasuredStateObserver(
              [weak_channel, sink, motion_group, compiled,
               source = source_resources(motion_binding), &clock,
               positions = std::vector<double>(compiled.size()),
               velocities = std::vector<double>(compiled.size())](
                  const sensor_msgs::msg::JointState &message) mutable {
                const auto channel = weak_channel.lock();
                if (!channel) return;
                bool complete = message.name.size() == message.position.size();
                const bool with_velocities =
                    message.velocity.size() == message.name.size();
                for (std::size_t index = 0; complete && index < source.size();
                     ++index) {
                  const auto found = std::find(message.name.begin(),
                                               message.name.end(), source[index]);
                  complete = found != message.name.end() &&
                             std::find(std::next(found), message.name.end(),
                                       source[index]) == message.name.end();
                  if (!complete) break;
                  const auto at =
                      static_cast<std::size_t>(found - message.name.begin());
                  positions[index] = message.position[at];
                  complete = std::isfinite(positions[index]);
                  if (with_velocities) velocities[index] = message.velocity[at];
                }
                const auto source_time =
                    static_cast<std::int64_t>(message.header.stamp.sec) *
                        1'000'000'000LL +
                    message.header.stamp.nanosec;
                const auto published = channel->publish_adapter_state(
                    core::ObservedRobotStateSourceKind::kJointState, compiled,
                    complete ? std::span<const double>(positions)
                             : std::span<const double>{},
                    complete && with_velocities
                        ? std::span<const double>(velocities)
                        : std::span<const double>{},
                    source_time, clock.now());
                if (published.published && published.complete) {
                  (void)sink->apply_group_joint_positions(
                      motion_group, positions,
                      core::ExecutionStateUpdateMode::kBestEffort);
                }
              });
        } else {
          // Item 20: the commanded state is the controller's own reference,
          // published for every new sample (velocities at the applied factor).
          runtime_handle->InstallSampleObserver(
              [weak_channel, sink, motion_group, compiled,
               &clock](const core::ExecutionControllerSampleDTO &sample) {
                const auto channel = weak_channel.lock();
                if (!channel ||
                    sample.reference_positions_rad.size() != compiled.size()) {
                  return;
                }
                std::vector<double> velocities;
                if (sample.reference_velocities_rad_s.size() == compiled.size()) {
                  velocities = sample.reference_velocities_rad_s;
                  const double factor = sample.applied_factor.value_or(1.0);
                  for (auto &velocity : velocities) velocity *= factor;
                }
                (void)channel->publish_adapter_state(
                    core::ObservedRobotStateSourceKind::kCommandedState, compiled,
                    sample.reference_positions_rad, velocities,
                    sample.local_receipt_stamp_ns, clock.now());
                (void)sink->apply_group_joint_positions(
                    motion_group, sample.reference_positions_rad,
                    core::ExecutionStateUpdateMode::kBestEffort);
              });
        }
        if (!selected.defer_ros_start) {
          runtime_handle->StartBackend();
        }

        const auto participant = runtime_handle->participant();
        RetainedRuntimeBackendSet result;
        // The handle owns every child and their borrowed resources. Retaining
        // either port alone retains that same lifetime without an ownership
        // cycle.
        auto owned_participant =
            std::shared_ptr<core::ExecutionPortParticipant>(runtime_handle,
                                                            participant.get());
        result.execution = owned_participant;
        if (gripper_binding.has_value()) {
          result.gripper = std::shared_ptr<core::IGripperCommandBackend>(
              runtime_handle, gripper_pointer);
          // RetainedNativeGripperRuntime registers its same Core gripper
          // participant as the logical-cancellation Stop port.
          result.rebind_gripper_route =
              [runtime_handle](
                  core::RuntimeParticipantBinding binding,
                  std::shared_ptr<IRetainedObservedStateChannel> channel) {
                return runtime_handle->RebindGripperRoute(std::move(binding),
                                                          std::move(channel));
              };
        }
        result.rebind_motion_route =
            [runtime_handle](
                core::RuntimeParticipantBinding binding,
                std::vector<core::RuntimeParticipantBinding> aliases) {
              return runtime_handle->RebindMotionRoute(std::move(binding),
                                                       std::move(aliases));
            };
        return result;
      };
}

std::shared_ptr<core::IGripperCommandBackend>
make_retained_ros2_control_gripper_backend(
    RetainedRos2ControlBackendConfig config,
    const core::RuntimeParticipantBinding& binding,
    const std::shared_ptr<RetainedObservedStateChannel>& channel,
    core::IClock& clock, core::IScheduler& scheduler,
    const std::shared_ptr<RetainedRos2ControlRuntimeHandle>& handle) {
  const bool no_output = config.gripper && config.gripper->output_mode == core::GripperOutputMode::kNone &&
      config.gripper->actuation_kind == core::GripperActuationKind::kPneumatic;
  if (!handle || !channel || !config.gripper ||
      (!no_output && (!config.arm.gripper_controller || !config.arm.measured_joint_state_lane)) ||
      (no_output && config.arm.gripper_controller) || !binding.value().control_path ||
      binding.value().control_path->state_authority_class != core::StateAuthorityClass::kCommanded ||
      channel->binding().value() != binding.value() || binding.value().resources.size() != 1U)
    throw std::invalid_argument("ROS gripper-only backend requires its exact commanded binding/transport");
  const auto group = sole_compiled_group(binding, "ROS gripper-only binding");
  if (config.gripper->group_id != group ||
      config.gripper->joint_name != binding.value().resources.front().compiled_resource_name)
    throw std::invalid_argument("ROS gripper-only config differs from retained resources");
  const bool pneumatic = config.gripper->actuation_kind == core::GripperActuationKind::kPneumatic;
  // The same sink publishes no-feedback pneumatic commanded positions;
  // feedback paths retain their display-only callback.
  std::vector<RetainedExecutionStateSinkBinding> sinks{{group, channel, true}};
  if (pneumatic) {
    // Gripper callbacks run on the handle's executor: internal lease.
    const std::weak_ptr<RetainedRos2ControlRuntimeHandle> owner =
        handle->weak_from_this();
    config.gripper->publish_display_position = [owner](const double position) {
      const auto current = owner.lock();
      return current ? current->PublishGripperDisplayPosition(position)
          : core::Result<void>::Err({"gripper_display_owner_unavailable",
              "Display owner has retired", {}});
    };
  }
  auto sink = std::make_shared<RetainedExecutionStateSink>(clock, std::move(sinks));
  auto resources = std::make_unique<ros2_control_execution::Ros2TransportResources>(
      config.node_name, config.node_namespace, config.executor_thread_count, sink, group, clock,
      config.ros_context_provider);
  auto gripper = std::make_unique<ros2_control_execution::Ros2GripperCommandBackend>(
      *resources, sink, clock, scheduler, true);
  declare_gripper_feedback_stream(config, binding);
  gripper->configure_gripper_action(binding.value().command_target_id.value(), *config.gripper);
  auto* const backend = gripper.get();
  const auto bound = handle->BindGripperOnly(
      std::move(resources), clock, std::move(gripper), binding,
      config.arm.gripper_controller.value_or(RetainedRos2ControlArmController{}),
      config.successor_occurrence, std::move(config.measured_receiver_clock),
      no_output ? std::nullopt : config.arm.measured_joint_state_lane);
  if (!bound.ok()) throw std::invalid_argument(bound.error().message);
  if (pneumatic) {
    const auto display = handle->BindGripperDisplayChannel(channel);
    if (!display.ok()) throw std::invalid_argument(display.error().message);
  }
  if (!config.defer_ros_start) handle->StartBackend();
  // Aliasing ownership keeps the actual transport, scheduler borrowing and
  // controller observations alive until the component owner drains.
  return std::shared_ptr<core::IGripperCommandBackend>(handle, backend);
}

} // namespace manyforge::roboplan_backend
