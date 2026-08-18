#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend::qualification_fixture {

[[nodiscard]] inline core::RuntimeParticipantBinding make_binding() {
  const std::vector<std::string> joints{"joint_1", "joint_2"};
  std::vector<core::RuntimeResourceBindingDTO> resources;
  for (std::size_t index = 0U; index < joints.size(); ++index) {
    resources.push_back(core::RuntimeResourceBindingDTO{
        core::ResourceRoleId("arm_joints"),
        core::ResourceId("resource.joint." + std::to_string(index + 1U)),
        joints[index], joints[index], core::CoordinateKind::kAngular});
  }
  core::RuntimeParticipantBindingDTO value{
      core::ProgramId("program.native.stop_qualification"),
      core::ProgramRevision("program-revision"),
      core::ProgramBindingId("program-binding"),
      core::RetainedBundleId("bundle"),
      core::GraphRevision("graph"),
      core::ComponentRoleId("robot"),
      core::TargetRoleId("arm"),
      core::ControllableComponentId("robot.1"),
      core::ComponentTypeId("aggregate.robot"),
      core::ComponentTypeRevision("aggregate-revision"),
      core::CommandTargetId("arm.target"),
      core::CapabilityId("execution.trajectory.execute"),
      core::CapabilityDefinitionRevision("capability-revision"),
      std::nullopt,
      {core::ProgramModelRoleBindingDTO{core::ModelRoleId("body"),
                                        core::ModelInstanceId("robot.model"),
                                        core::ModelRevision("model-revision")}},
      {core::RuntimeModelGroupBindingDTO{core::ModelGroupRoleId("arm_group"),
                                         core::ModelInstanceId("robot.model"),
                                         "arm", joints}},
      {core::ProgramFrameRoleBindingDTO{
           core::FrameRoleId("base"), core::ModelInstanceId("robot.model"),
           core::ModelRevision("model-revision"), "base"},
       core::ProgramFrameRoleBindingDTO{
           core::FrameRoleId("tcp"), core::ModelInstanceId("robot.model"),
           core::ModelRevision("model-revision"), "tool0"}},
      std::move(resources),
      {},
      core::RuntimeControlPathDTO{
          core::ControlDomainId("domain.arm"),
          core::ControlDomainRevision("domain-revision"),
          core::ControlBindingId("binding.arm"),
          core::ControlBindingRevision("binding-revision"),
          core::ControlInterfaceContractId("trajectory.position"),
          core::ControlInterfaceRevision("interface-revision"),
          core::ControllerTopologyId("topology.native"),
          core::TopologyRevision("topology-revision"),
          core::ControlRouteId("route.native"),
          core::ActivationSetId("activation.native"),
          core::ActiveControlConfigurationId("active.native"),
          core::ActiveControlConfigurationGeneration(1U),
          core::ProviderInstanceId("provider.native"),
          core::ProviderManifestId("provider-manifest.native"),
          core::ProviderManifestRevision("provider-manifest-revision"),
          core::RuntimeUnitId("runtime.native"),
          core::RuntimeEpoch("runtime-epoch.1"),
          core::StateAuthorityId("state.native"),
          core::StateAuthorityClass::kPredicted,
          core::StopEvidencePolicyId("stop.native"),
          core::StopEvidencePolicyRevision("stop-revision"),
          core::ContainmentGranularity::kResource,
          core::RuntimeScaleRouteQualificationDTO{
              core::ControlRouteId("route.native"),
              core::ScalingDomainId("scale.native"),
              core::EndpointRoleId("runtime_scale_command"),
              core::EndpointRoleId("runtime_scale_state"), std::nullopt,
              core::RuntimeScaleAcknowledgementKind::kAppliedState, 4.0,
              100'000'000}},
      std::nullopt,
      {core::RuntimeCapabilityProviderPathDTO{
          core::CapabilityId("planning.motion.joint_goal"),
          core::CapabilityDefinitionRevision("sha256:"
                                             "0f6cb2fda7e1d05f20a5867203b84c0e0"
                                             "427440f4b11657941a907c2e2a23dce"),
          core::CapabilityProviderBindingId("planner-binding"),
          core::CapabilityProviderBindingRevision("planner-binding-revision"),
          core::ProviderInstanceId("planner-provider"),
          core::ProviderManifestId("planner-provider-manifest"),
          core::ProviderManifestRevision("planner-provider-revision"),
          core::RuntimeUnitId("runtime.native"),
          core::RuntimeEpoch("runtime-epoch.1")}},
      {core::RuntimeUnitEpochBindingDTO{
          core::RuntimeUnitId("runtime.native"),
          core::RuntimeEpoch("runtime-epoch.1")}}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] inline core::CoordinatePositionDTO
tagged_position(core::CoordinateKind kind, double value) {
  return kind == core::CoordinateKind::kAngular
             ? core::CoordinatePositionDTO{core::AngularPositionDTO{value}}
             : core::CoordinatePositionDTO{core::LinearPositionDTO{value}};
}

[[nodiscard]] inline core::CoordinateVelocityDTO
tagged_velocity(core::CoordinateKind kind, double value) {
  return kind == core::CoordinateKind::kAngular
             ? core::CoordinateVelocityDTO{core::AngularVelocityDTO{value}}
             : core::CoordinateVelocityDTO{core::LinearVelocityDTO{value}};
}

class StateChannel final : public IRetainedObservedStateChannel {
public:
  explicit StateChannel(const core::RuntimeParticipantBinding &binding)
      : context_([&] {
          auto context = core::make_observed_state_query_context(binding, 0);
          if (!context.ok() || context.value().resources.size() != 2U) {
            throw std::logic_error("invalid qualification state context");
          }
          return std::move(context.value());
        }()),
        state_{context_.controllable_component_id,
               context_.command_target_id,
               context_.control_domain_id,
               context_.control_domain_revision,
               context_.control_binding_id,
               context_.control_binding_revision,
               context_.controller_topology_id,
               context_.topology_revision,
               context_.control_route_id,
               context_.active_control_configuration_id,
               context_.active_control_configuration_generation,
               context_.resources,
               context_.joint_names,
               {tagged_position(core::CoordinateKind::kAngular, 0.0),
                tagged_position(core::CoordinateKind::kAngular, 0.0)},
               std::nullopt} {}

  RetainedStateProjectionResult
  publish_state(std::span<const double> positions,
                std::span<const double> velocities,
                std::int64_t source_timestamp_ns,
                std::int64_t received_timestamp_ns) override {
    std::lock_guard lock(mutex_);
    if (positions.size() != context_.resources.size() ||
        (!velocities.empty() &&
         velocities.size() != context_.resources.size())) {
      return failure("qualification_state_shape_mismatch");
    }
    state_.positions.clear();
    for (std::size_t index = 0U; index < positions.size(); ++index) {
      state_.positions.push_back(
          tagged_position(context_.coordinate_kinds[index], positions[index]));
    }
    if (velocities.empty()) {
      state_.velocities.reset();
    } else {
      state_.velocities.emplace();
      for (std::size_t index = 0U; index < velocities.size(); ++index) {
        state_.velocities->push_back(tagged_velocity(
            context_.coordinate_kinds[index], velocities[index]));
      }
    }
    source_timestamp_ns_ = source_timestamp_ns;
    received_timestamp_ns_ = received_timestamp_ns;
    ++sequence_;
    return {RetainedStateProjectionStatus::kApplied, std::nullopt};
  }

  RetainedStateProjectionResult
  project_latest(std::int64_t, RetainedStateProjectionMode) override {
    return {RetainedStateProjectionStatus::kApplied, std::nullopt};
  }

  RetainedStateProjectionResult publish_and_project(
      std::span<const double> positions, std::span<const double> velocities,
      std::int64_t source_timestamp_ns, std::int64_t received_timestamp_ns,
      RetainedStateProjectionMode mode) override {
    auto result = publish_state(positions, velocities, source_timestamp_ns,
                                received_timestamp_ns);
    return result.applied() ? project_latest(received_timestamp_ns, mode)
                            : result;
  }

  core::Result<core::ObservedStateSnapshotDTO>
  current_state(std::int64_t) const override {
    std::lock_guard lock(mutex_);
    return core::Result<core::ObservedStateSnapshotDTO>::Ok(
        core::ObservedStateSnapshotDTO{
            context_.controllable_component_id,
            context_.command_target_id,
            context_.control_domain_id,
            context_.control_domain_revision,
            context_.control_binding_id,
            context_.control_binding_revision,
            context_.provider_instance_id,
            context_.provider_manifest_id,
            context_.provider_manifest_revision,
            context_.controller_topology_id,
            context_.topology_revision,
            context_.control_route_id,
            context_.active_control_configuration_id,
            context_.active_control_configuration_generation,
            context_.resources,
            sequence_,
            context_.joint_names,
            state_.positions,
            state_.velocities,
            "qualification.native.state",
            source_epoch_,
            core::ObservedStateSourceKind::kPredictedState,
            source_timestamp_ns_,
            received_timestamp_ns_,
            context_.state_authority_id,
            context_.state_authority_class,
            1U,
            context_.freshness_budget_ns,
            true});
  }

  core::Result<core::ObservedStateSnapshotDTO>
  current_state_bounded(std::int64_t now_ns, std::int64_t deadline_ns,
                        std::stop_token cancellation) const override {
    if (cancellation.stop_requested() ||
        (deadline_ns >= 0 && now_ns >= deadline_ns)) {
      return core::Result<core::ObservedStateSnapshotDTO>::Err(
          core::ErrorDTO{"invalid_request", "qualification state unavailable",
                         {{"reason", cancellation.stop_requested()
                                        ? "retained_state_read_cancelled"
                                        : "retained_state_read_deadline_expired"}}});
    }
    return current_state(now_ns);
  }

  core::Result<core::ControlledStateDTO>
  current_controlled_state() const override {
    std::lock_guard lock(mutex_);
    return core::Result<core::ControlledStateDTO>::Ok(state_);
  }

  const core::ObservedStateQueryContextDTO &
  query_context() const noexcept override {
    return context_;
  }

  const core::AddressedObservedStatePublication *
  addressed_publication() const noexcept override {
    return nullptr;
  }

  void set_source_epoch(core::RuntimeEpoch epoch) {
    std::lock_guard lock(mutex_);
    source_epoch_ = std::move(epoch);
  }

  void rebind_context(const core::RuntimeParticipantBinding &binding) {
    std::lock_guard lock(mutex_);
    auto context = core::make_observed_state_query_context(
        binding, context_.freshness_budget_ns);
    if (!context.ok()) {
      throw std::logic_error(context.error().message);
    }
    context_ = std::move(context.value());
    source_epoch_ = context_.runtime_epoch;
  }

  void refresh_source_on_wait(bool enabled) {
    std::lock_guard lock(mutex_);
    refresh_source_on_wait_ = enabled;
  }

  bool wait_for_fresh_state(std::chrono::nanoseconds) const override {
    std::lock_guard lock(mutex_);
    if (!refresh_source_on_wait_) {
      return false;
    }
    source_epoch_ = context_.runtime_epoch;
    ++sequence_;
    if (received_timestamp_ns_ < 101) {
      received_timestamp_ns_ = 101;
    }
    return true;
  }

private:
  [[nodiscard]] static RetainedStateProjectionResult
  failure(std::string reason) {
    return {RetainedStateProjectionStatus::kRejected,
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
                "qualification state rejected",
                {{"reason", std::move(reason)}}}};
  }

  core::ObservedStateQueryContextDTO context_;
  mutable std::mutex mutex_;
  core::ControlledStateDTO state_;
  mutable std::uint64_t sequence_{};
  std::int64_t source_timestamp_ns_{};
  mutable std::int64_t received_timestamp_ns_{};
  mutable core::RuntimeEpoch source_epoch_{context_.runtime_epoch};
  bool refresh_source_on_wait_{};
};

[[nodiscard]] inline core::TrajectoryDTO
trajectory(const core::RuntimeParticipantBinding &binding, double duration_s) {
  const auto &value = binding.value();
  std::vector<core::ResourceId> resources;
  std::vector<std::string> joint_names;
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    joint_names.push_back(resource.compiled_resource_name);
  }
  return core::TrajectoryDTO{
      std::move(resources),
      std::move(joint_names),
      {core::TrajectoryWaypointDTO{0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
       core::TrajectoryWaypointDTO{
           duration_s, {0.2, -0.3}, {0.0, 0.0}, {0.0, 0.0}}}};
}

[[nodiscard]] inline core::MotionInterruptStopProfileDTO
soft_profile(const core::RuntimeParticipantBinding &binding,
             double deceleration_s) {
  core::MotionInterruptStopProfileDTO profile;
  profile.profile_id = "native_stop_qualification_soft";
  profile.stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = deceleration_s;
  profile.allow_natural_completion_on_short_path = false;
  profile.recoverable_substitution_mode =
      core::MotionInterruptStopMode::kSpringBackHold;
  for (const auto &resource : binding.value().resources) {
    if (!resource.source_joint_name.has_value() ||
        resource.coordinate_kind != core::CoordinateKind::kAngular) {
      throw std::invalid_argument(
          "native Stop qualification fixture requires angular source joints");
    }
    profile.per_joint_max_deceleration.emplace(
        *resource.source_joint_name, core::AngularAccelerationDTO{100.0});
  }
  return profile;
}

} // namespace manyforge::roboplan_backend::qualification_fixture
