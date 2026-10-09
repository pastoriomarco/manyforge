#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>

#include <algorithm>
#include <mutex>
#include <stdexcept>
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

[[nodiscard]] core::ObservedStateSourceKind
addressed_source_kind(const core::ObservedRobotStateSourceKind source) {
  switch (source) {
  case core::ObservedRobotStateSourceKind::kActionFeedback:
    return core::ObservedStateSourceKind::kActionFeedback;
  case core::ObservedRobotStateSourceKind::kControllerState:
    return core::ObservedStateSourceKind::kControllerState;
  case core::ObservedRobotStateSourceKind::kJointState:
    return core::ObservedStateSourceKind::kJointState;
  case core::ObservedRobotStateSourceKind::kCommandedState:
    return core::ObservedStateSourceKind::kCommandedState;
  case core::ObservedRobotStateSourceKind::kPredictedState:
    return core::ObservedStateSourceKind::kPredictedState;
  }
  throw std::invalid_argument("unknown ros2_control state source kind");
}

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

[[nodiscard]] core::Result<void> gripper_rebind_result(
    const core::RuntimeParticipantBinding &binding,
    const std::shared_ptr<IRetainedObservedStateChannel> &channel,
    const core::CommandTargetId &expected_target) {
  if (!channel || binding.value().command_target_id != expected_target) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
        "ros2_control gripper route crosses its configured target",
        {{"reason", "ros2_control_gripper_route_rebind_mismatch"}}});
  }
  auto expected = core::make_observed_state_query_context(
      binding, channel->query_context().freshness_budget_ns);
  if (!expected.ok() || expected.value() != channel->query_context()) {
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
        "ros2_control gripper route crosses its addressed state authority",
        {{"reason", "ros2_control_gripper_route_rebind_mismatch"}}});
  }
  return core::Result<void>::Ok();
}

} // namespace

RetainedObservedStateChannelConfig retained_ros2_control_motion_state_config(
    const ros2_control_execution::Ros2ControlExecutionBackendConfig &config) {
  auto sources =
      ros2_control_execution::ros2_control_addressed_state_sources(config);
  if (sources.empty()) {
    throw std::invalid_argument(
        "ros2_control runtime has no addressed observed-state source");
  }
  const auto &primary = sources.front();
  return RetainedObservedStateChannelConfig{
      addressed_source_kind(primary.kind), primary.source_id,
      primary.freshness_budget_ns, 1U, std::move(sources)};
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
        const auto expected_authority =
            selected.execution.execution_state_source_mode == "commanded_sim"
                ? core::StateAuthorityClass::kCommanded
            : selected.execution.execution_state_source_mode ==
                    "authored_preview"
                ? core::StateAuthorityClass::kPredicted
                : core::StateAuthorityClass::kMeasured;
        if (!motion_channel ||
            !motion_binding.value().control_path.has_value() ||
            motion_binding.value().control_path->state_authority_class !=
                expected_authority) {
          throw std::invalid_argument(
              "ros2_control backend differs from the retained state authority");
        }
        const auto joints = compiled_resources(motion_binding);
        if (selected.execution.required_joints != joints) {
          throw std::invalid_argument(
              "ros2_control required joints differ from retained resources");
        }
        const auto transport_joints = source_resources(motion_binding);
        if (selected.execution.transport_joint_names.empty()) {
          selected.execution.transport_joint_names = transport_joints;
        } else if (selected.execution.transport_joint_names !=
                   transport_joints) {
          throw std::invalid_argument(
              "ros2_control transport joints differ from retained source "
              "resources");
        }
        const auto motion_group =
            sole_compiled_group(motion_binding, "ros2_control motion binding");
        selected.execution.prepared_participant_binding = motion_binding;

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
        const std::weak_ptr<RetainedObservedStateChannel> weak_motion_channel =
            motion_channel;
        auto resources = std::make_unique<
            ros2_control_execution::Ros2TransportResources>(
            selected.execution.node_name, selected.execution.node_namespace,
            selected.execution.executor_thread_count,
            selected.execution.command_gate_topic, sink, motion_group, clock);
        auto backend = std::make_unique<
            ros2_control_execution::Ros2ControlExecutionBackend>(
            *resources,
            ros2_control_execution::Ros2ControlExecutionBackend::
                DeferredRosExecutorStartTag{},
            motion_channel->addressed_publication_writer(),
            [weak_motion_channel](
                const core::ObservedRobotStateSourceKind source,
                const std::span<const std::string> source_joint_names,
                const std::span<const double> positions,
                const std::span<const double> velocities,
                const std::int64_t source_timestamp_ns,
                const std::int64_t received_timestamp_ns) {
              const auto channel = weak_motion_channel.lock();
              if (!channel) {
                return core::ObservedRobotStatePublishResult{
                    false, false, false, 0, "publication_unavailable"};
              }
              return channel->publish_adapter_state(
                  source, source_joint_names, positions, velocities,
                  source_timestamp_ns, received_timestamp_ns);
            },
            sink, motion_group, clock, scheduler, selected.execution);
        std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend>
            gripper;
        if (gripper_binding.has_value()) {
          gripper = std::make_unique<
              ros2_control_execution::Ros2GripperCommandBackend>(
              *resources, sink, clock, scheduler,
              selected.execution.execution_state_source_mode == "commanded_sim");
          gripper->configure_gripper_action(
              gripper_binding->value().command_target_id.value(),
              *selected.gripper);
        }
        auto *const backend_pointer = backend.get();
        auto *const gripper_pointer = gripper.get();
        const auto bound = runtime_handle->BindBackend(
            std::move(resources), std::move(backend), std::move(gripper),
            motion_binding, gripper_binding);
        if (!bound.ok()) {
          throw std::invalid_argument(bound.error().message);
        }
        if (!selected.defer_ros_start) {
          runtime_handle->StartBackend();
        }

        const auto gripper_target =
            gripper_binding.has_value()
                ? std::optional(gripper_binding->value().command_target_id)
                : std::nullopt;
        RetainedRuntimeBackendSet result;
        // The handle owns both children and their borrowed resources. Retaining
        // either port alone retains that same lifetime without an ownership cycle.
        auto owned_backend = std::shared_ptr<
            ros2_control_execution::Ros2ControlExecutionBackend>(
            runtime_handle, backend_pointer);
        result.execution = owned_backend;
        if (gripper_binding.has_value()) {
          result.gripper = std::shared_ptr<core::IGripperCommandBackend>(
              runtime_handle, gripper_pointer);
          result.gripper_stop_execution_port = owned_backend.get();
          result.rebind_gripper_route =
              [runtime_handle, expected_target = *gripper_target](
                  core::RuntimeParticipantBinding binding,
                  std::shared_ptr<IRetainedObservedStateChannel> channel) {
                static_cast<void>(runtime_handle);
                return gripper_rebind_result(binding, channel, expected_target);
              };
        }
        result.rebind_motion_route =
            [backend = std::move(owned_backend)](core::RuntimeParticipantBinding binding,
                      std::vector<core::RuntimeParticipantBinding> aliases) {
              return backend->RebindRoute(std::move(binding),
                                          std::move(aliases));
            };
        return result;
      };
}

} // namespace manyforge::roboplan_backend
