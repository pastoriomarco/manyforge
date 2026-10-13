#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <manyforge_roboplan/retained_execution_state_sink.hpp>

namespace manyforge::roboplan_backend {
namespace {

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
  if (config.participant.stop.trip_authority != core::StateAuthorityClass::kCommanded ||
      config.observed_state_authority_profile_id.empty()) {
    throw std::invalid_argument(
        "ros2_control runtime composes commanded-simulation arm state only");
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
    runtime_handle = std::make_shared<RetainedRos2ControlRuntimeHandle>();
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
        } else if (selected.gripper.has_value()) {
          throw std::invalid_argument(
              "ros2_control product config contains an unbound gripper");
        }

        auto sink = std::make_shared<RetainedExecutionStateSink>(
            clock, std::move(sink_bindings));
        auto resources =
            std::make_unique<ros2_control_execution::Ros2TransportResources>(
                selected.node_name, selected.node_namespace,
                selected.executor_thread_count, sink, motion_group, clock);
        std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend>
            gripper;
        if (gripper_binding.has_value()) {
          gripper = std::make_unique<
              ros2_control_execution::Ros2GripperCommandBackend>(
              *resources, sink, clock, scheduler, true);
          gripper->configure_gripper_action(
              gripper_binding->value().command_target_id.value(),
              *selected.gripper);
        }
        auto *const gripper_pointer = gripper.get();
        const auto state_rate_hz = selected.arm.adapter.lane_facts.state_rate_hz;
        auto participant_config = selected.participant;
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

        // Item 20: the commanded state is the controller's own reference,
        // published for every new sample (velocities at the applied factor).
        const std::weak_ptr<RetainedObservedStateChannel> weak_channel =
            motion_channel;
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
        if (selected.isaac_guard.has_value()) {
          const auto binding = RetainedIsaacArmGuardBinding::Create(
              *selected.isaac_guard, runtime_handle->node(),
              make_retained_isaac_arm_host(runtime_handle), clock);
          runtime_handle->InstallGuardHooks(binding->hooks());
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
          // The gripper's Stop stages name the arm participant (as `main`'s
          // ROS lane named the motion backend).
          result.gripper_stop_execution_port = owned_participant.get();
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

} // namespace manyforge::roboplan_backend
