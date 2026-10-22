#include <gtest/gtest.h>

#include "native_stop_qualification_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/io_output_participant_engine.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/stop_fault_recovery.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_roboplan/retained_native_cell_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#ifdef MANYFORGE_A1_ROS_GRIPPER_CELL_TEST
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_ros2_control_backend_factory.hpp>
#include <roboplan/core/scene.hpp>
#endif

namespace manyforge::roboplan_backend {
namespace {

namespace core = manyforge::core;
namespace qualification_fixture =
    manyforge::roboplan_backend::qualification_fixture;

[[nodiscard]] std::filesystem::path TemporaryJournalPath() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("manyforge-native-cell-owner-" + std::to_string(nonce) + ".sqlite");
}

void RemoveJournalFiles(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + "-wal", ignored);
  std::filesystem::remove(path.string() + "-shm", ignored);
}

core::ResourceAddressedExecutionKernelConfig EmptyKernelConfig() {
  const auto graph = core::GraphRevision("graph.empty-cell");
  core::ResourceAddressedExecutionKernelConfig config{
      core::RequestOwnerEpoch("owner.empty-cell"),
      core::CellId("cell.empty-cell"),
      32U,
      {},
      nullptr,
      graph};
  config.empty_runtime_retained_bundle_id =
      core::RetainedBundleId("bundle.empty-cell");
  config.empty_runtime_graph_revision = graph;
  return config;
}

RetainedNativeCellRuntimeInput EmptyCellInput() {
  return RetainedNativeCellRuntimeInput{
      .clock = {},
      .scheduler = {},
      .operation_journal_path = {},
      .participant_registrations = {},
      .kernel_config = EmptyKernelConfig(),
      .stop_sources = {}};
}

[[nodiscard]] core::RuntimeParticipantBinding DistinctCellBinding(
    const core::RuntimeParticipantBinding &source, const std::string &suffix) {
  auto value = source.value();
  const auto component_name = "robot." + suffix;
  const auto model_name = "robot.model." + suffix;
  value.program_binding_id = core::ProgramBindingId("program-binding." + suffix);
  value.component_role_id = core::ComponentRoleId("robot." + suffix);
  value.target_role_id = core::TargetRoleId("arm." + suffix);
  value.controllable_component_id =
      core::ControllableComponentId(component_name);
  for (auto &model : value.model_bindings) {
    model.model_instance_id = core::ModelInstanceId(model_name);
  }
  for (auto &group : value.model_groups) {
    group.model_instance_id = core::ModelInstanceId(model_name);
    group.compiled_group_name = "mf2_robot_" + suffix + "_arm_group";
    for (std::size_t index = 0U;
         index < group.expanded_compiled_joint_names.size(); ++index) {
      group.expanded_compiled_joint_names[index] =
          "mf2_robot_" + suffix + "_joint_" + std::to_string(index + 1U);
    }
  }
  for (auto &frame : value.frame_bindings) {
    frame.model_instance_id = core::ModelInstanceId(model_name);
    frame.compiled_frame_name = "mf2_robot_" + suffix + "_" +
                                frame.compiled_frame_name;
  }
  for (std::size_t index = 0U; index < value.resources.size(); ++index) {
    auto &resource = value.resources[index];
    resource.resource_id = core::ResourceId(
        component_name + ".joint." + std::to_string(index + 1U));
    resource.compiled_resource_name =
        "mf2_robot_" + suffix + "_joint_" + std::to_string(index + 1U);
  }
  auto &path = *value.control_path;
  path.control_domain_id = core::ControlDomainId("domain.arm." + suffix);
  path.control_binding_id = core::ControlBindingId("binding.arm." + suffix);
  path.control_route_id = core::ControlRouteId("route.native." + suffix);
  path.activation_set_id = core::ActivationSetId("activation.native." + suffix);
  path.active_control_configuration_id =
      core::ActiveControlConfigurationId("active.native." + suffix);
  path.provider_instance_id = core::ProviderInstanceId("provider.native." + suffix);
  path.state_authority_id = core::StateAuthorityId("state.native." + suffix);
  if (path.runtime_scale_qualification.has_value()) {
    path.runtime_scale_qualification->control_route_id = path.control_route_id;
  }
  for (auto &provider : value.supporting_capability_provider_paths) {
    provider.capability_provider_binding_id =
        core::CapabilityProviderBindingId("planner-binding." + suffix);
    provider.capability_provider_binding_revision =
        core::CapabilityProviderBindingRevision("planner-binding-revision." +
                                                suffix);
    provider.provider_instance_id =
        core::ProviderInstanceId("planner-provider." + suffix);
  }
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::runtime_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] core::RuntimeParticipantAdmissionResolver UnusedResolver() {
  return [](const core::OperationIntentDTO &, const std::size_t) {
    return core::Result<core::RuntimeParticipantExecutionRegistration>::Err(
        {"unused_test_resolver", "cell transition test admits no operations", {}});
  };
}

[[nodiscard]] core::RuntimeParticipantExecutionRegistration TestRegistration(
    const core::RuntimeParticipantBinding &binding,
    core::IResolvedParticipantExecutionEngine *engine) {
  return core::RuntimeParticipantExecutionRegistration{
      .binding = binding,
      .stop_readiness = {},
      .retained_default_stop_limits = {},
      .stop_node_category = core::MotionInterruptNodeCategory::kGeneric,
      .engine = engine,
      .stop_execution_port = nullptr,
      .participant_policy = {},
      .prepared_motion_engine = nullptr,
      .default_operator_stop_profile = {},
      .default_fault_stop_profile = {},
      .deployment_maximum_tcp_linear_speed_mps = {},
      .intrinsic_direct_control = false,
      .direct_control_joint_limits = {}};
}

[[nodiscard]] RetainedStopSurveillanceSourceInput StopSourceFor(
    const core::RuntimeParticipantBinding &binding) {
  auto state_channel =
      std::make_shared<qualification_fixture::StateChannel>(binding);
  RetainedNoActiveGoalEvidenceSource no_active_goal =
      [](const std::int64_t) {
        return core::Result<core::ContentDigest>::Ok(
            core::ContentDigest("sha256:" + std::string(64U, 'a')));
      };
  return {binding, std::move(state_channel), {}, std::move(no_active_goal)};
}

class CellTestExecutionEngine final
    : public core::IResolvedParticipantExecutionEngine {
public:
  [[nodiscard]] core::ResetResultDTO reset_after_certified_containment(
      std::uint64_t, std::optional<std::uint64_t>) override {
    return {};
  }
  [[nodiscard]] core::AcknowledgeCompletedResultDTO acknowledge_completed(
      std::uint64_t, std::optional<std::uint64_t>) override {
    return {};
  }
  [[nodiscard]] core::Result<core::ResolvedRuntimeScaleAcceptedDTO>
  set_runtime_scale(const core::ResolvedRuntimeScaleCommandDTO &) override {
    return unused<core::ResolvedRuntimeScaleAcceptedDTO>();
  }
  [[nodiscard]] core::Result<core::ResolvedRuntimeScaleStatusDTO>
  runtime_scale_status(const core::OperationId &,
                       const core::ControlBindingId &) const override {
    return unused<core::ResolvedRuntimeScaleStatusDTO>();
  }
  [[nodiscard]] core::GripperCommandResultDTO
  command_gripper(const core::GripperCommandRequestDTO &) override {
    return {};
  }
  [[nodiscard]] core::Result<void> validate_gripper_command(
      const core::GripperCommandRequestDTO &) const override {
    return core::Result<void>::Err(
        {"unused_test_engine", "cell transition test admits no commands", {}});
  }
  [[nodiscard]] core::GripperCommandStatusDTO
  get_gripper_status(const std::string &) const override {
    return {};
  }
  [[nodiscard]] core::Result<std::uint64_t>
  stop_gripper(const std::string &, std::uint64_t,
               std::uint64_t) override {
    return unused<std::uint64_t>();
  }
  [[nodiscard]] core::Result<void> reset_gripper(
      const std::string &, std::uint64_t, std::uint64_t) override {
    return core::Result<void>::Err(
        {"unused_test_engine", "cell transition test admits no commands", {}});
  }
  [[nodiscard]] core::ExecutionStatus get_status() const override {
    return {};
  }
  [[nodiscard]] core::SubscriptionHandle
  subscribe_events(core::EventCallback) override {
    ++subscriptions_;
    return {subscriptions_};
  }
  void unsubscribe_events(core::SubscriptionHandle) override {
    if (subscriptions_ != 0U) {
      --subscriptions_;
    }
  }
  [[nodiscard]] std::size_t subscription_count() const noexcept {
    return subscriptions_;
  }

private:
  template <typename T> [[nodiscard]] static core::Result<T> unused() {
    return core::Result<T>::Err(
        {"unused_test_engine", "cell transition test admits no commands", {}});
  }

  std::size_t subscriptions_{};
};

class CellTestAdmissionGate final
    : public core::IResourceOperationAdmissionGate {
public:
  CellTestAdmissionGate()
      : mutex_(std::make_shared<std::recursive_mutex>()) {}

  [[nodiscard]] std::shared_ptr<std::recursive_mutex>
  shared_admission_mutex() const override {
    return mutex_;
  }
  [[nodiscard]] core::Result<void> reserve(
      core::ResourceSlotTable &table,
      const core::ResourceSlotOwnerDTO &owner,
      const std::vector<core::ResourceId> &resources) override {
    return table.reserve(owner, resources);
  }
  [[nodiscard]] core::Result<void> reserve_batch(
      core::ResourceSlotTable &table,
      const std::vector<core::ResourceSlotReservationOwnerDTO> &owners)
      override {
    return table.reserve_batch(owners);
  }
  [[nodiscard]] std::vector<core::ResourceId> current_fenced_resources(
      const std::vector<core::ResourceId> &resources) const override {
    if (!held_fence_id_.has_value()) {
      return {};
    }
    std::vector<core::ResourceId> result;
    std::set_intersection(resources.begin(), resources.end(),
                          held_resources_.begin(), held_resources_.end(),
                          std::back_inserter(result));
    return result;
  }
  [[nodiscard]] bool exact_fence_covers(
      const core::ConfigurationFenceId &fence_id,
      const std::vector<core::ResourceId> &resources) const override {
    return held_fence_id_ == fence_id &&
           std::all_of(resources.begin(), resources.end(), [&](const auto &resource) {
             return std::binary_search(held_resources_.begin(),
                                       held_resources_.end(), resource);
           });
  }
  [[nodiscard]] bool exact_fence_is_held(
      const core::ConfigurationFenceId &fence_id,
      const core::ConfigurationFenceScopeDTO &scope) const override {
    return held_fence_id_ == fence_id && held_scope_ == scope;
  }
  void HoldFence(core::ConfigurationFenceId id,
                 core::ConfigurationFenceScopeDTO scope) {
    held_fence_id_ = std::move(id);
    held_resources_ = scope.resources;
    held_scope_ = std::move(scope);
  }

private:
  std::shared_ptr<std::recursive_mutex> mutex_;
  std::optional<core::ConfigurationFenceId> held_fence_id_;
  std::vector<core::ResourceId> held_resources_;
  std::optional<core::ConfigurationFenceScopeDTO> held_scope_;
};

[[nodiscard]] core::RuntimeParticipantBinding GripperOnlyBinding() {
  auto value = qualification_fixture::make_binding().value();
  value.program_binding_id = core::ProgramBindingId("program-binding.gripper");
  value.controllable_component_id =
      core::ControllableComponentId("robot.independent-gripper");
  value.target_role_id = core::TargetRoleId("gripper");
  value.command_target_id = core::CommandTargetId("gripper.target");
  value.capability_id = core::CapabilityId("tool.gripper.command");
  value.command_schema = core::ProgramCommandSchemaRefDTO{
      core::CommandSchemaId(
          std::string(core::kGripperPositionCommandSchemaId)),
      core::CommandSchemaRevision(
          std::string(core::kGripperPositionCommandSchemaRevision))};
  for (auto &model : value.model_bindings) {
    model.model_instance_id = core::ModelInstanceId("robot.model.gripper");
  }
  value.model_groups.front().model_instance_id =
      core::ModelInstanceId("robot.model.gripper");
  value.model_groups.front().compiled_group_name = "mf2_gripper_group";
  value.model_groups.front().expanded_compiled_joint_names = {
      "mf2_left_finger_joint", "mf2_right_finger_joint"};
  for (auto &frame : value.frame_bindings) {
    frame.model_instance_id = core::ModelInstanceId("robot.model.gripper");
    frame.compiled_frame_name = "gripper_" + frame.compiled_frame_name;
  }
  value.resources = {
      core::RuntimeResourceBindingDTO{
          core::ResourceRoleId("fingers"),
          core::ResourceId("robot.left_finger"), "left_finger_joint",
          "mf2_left_finger_joint", core::CoordinateKind::kLinear},
      core::RuntimeResourceBindingDTO{
          core::ResourceRoleId("fingers"),
          core::ResourceId("robot.right_finger"), "right_finger_joint",
          "mf2_right_finger_joint", core::CoordinateKind::kLinear}};
  value.supporting_capability_provider_paths.clear();
  auto &path = *value.control_path;
  path.control_domain_id = core::ControlDomainId("domain.gripper");
  path.control_binding_id = core::ControlBindingId("binding.gripper");
  path.control_route_id = core::ControlRouteId("route.gripper");
  path.activation_set_id = core::ActivationSetId("activation.gripper");
  path.active_control_configuration_id =
      core::ActiveControlConfigurationId("active.gripper");
  path.provider_instance_id = core::ProviderInstanceId("provider.gripper");
  path.state_authority_id = core::StateAuthorityId("state.gripper");
  path.runtime_scale_qualification.reset();
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::runtime_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] core::RuntimeParticipantBinding ReplacementGripperBinding(
    const core::RuntimeParticipantBinding &predecessor) {
  auto value = predecessor.value();
  auto &path = *value.control_path;
  path.control_route_id = core::ControlRouteId("route.gripper.replacement");
  path.provider_instance_id =
      core::ProviderInstanceId("provider.gripper.replacement");
  path.state_authority_id =
      core::StateAuthorityId("state.gripper.replacement");
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::runtime_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

[[nodiscard]] std::string StopDigest(const char value) {
  return "sha256:" + std::string(64U, value);
}

[[nodiscard]] core::RuntimeParticipantStopReadiness
StopReadinessFor(const core::RuntimeParticipantBinding &binding,
                 const core::ControllerLifecycleExpectedDomain &domain,
                 const std::string &evidence_suffix = {}) {
  const auto golden =
      std::filesystem::path(__FILE__)
          .parent_path()
          .parent_path()
          .parent_path() /
      "manyforge_core/test/golden/resolved_stop_base_plan.v1.json";
  std::ifstream stream(golden);
  std::string bytes{std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()};
  if (bytes.ends_with('\n'))
    bytes.pop_back();
  auto decoded = core::DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok())
    throw std::logic_error(decoded.error().message);
  auto plan = std::move(decoded.value());
  const auto &value = binding.value();
  const auto &path = *value.control_path;
  plan.retained_bundle_id = value.retained_bundle_id;
  plan.graph_revision = value.graph_revision;
  plan.control_domain_id = path.control_domain_id;
  plan.control_domain_revision = path.control_domain_revision;
  plan.control_binding_id = path.control_binding_id;
  plan.control_binding_revision = path.control_binding_revision;
  plan.controller_topology_id = path.controller_topology_id;
  plan.topology_revision = path.topology_revision;
  plan.control_route_id = path.control_route_id;
  plan.activation_set_id = path.activation_set_id;
  plan.active_control_configuration_id = path.active_control_configuration_id;
  plan.active_control_configuration_generation =
      path.active_control_configuration_generation;
  plan.source_runtime_unit_id = path.provider_runtime_unit_id;
  plan.source_runtime_epoch = path.provider_runtime_epoch;
  plan.stop_evidence_policy_id = path.stop_evidence_policy_id;
  plan.stop_evidence_policy_revision = path.stop_evidence_policy_revision;
  plan.containment_granularity = path.containment_granularity;
  const auto rebind_target = [&](core::ResolvedStopRuntimeTargetDTO &target) {
    target.provider_instance_id = path.provider_instance_id;
    target.provider_manifest_id = path.provider_manifest_id;
    target.provider_manifest_revision = path.provider_manifest_revision;
    target.runtime_unit_id = path.provider_runtime_unit_id;
    target.provider_runtime_epoch = path.provider_runtime_epoch;
    target.runtime_unit_epoch = path.provider_runtime_epoch;
  };
  std::vector<core::ResourceId> resources;
  for (const auto &resource : value.resources)
    resources.push_back(resource.resource_id);
  std::sort(resources.begin(), resources.end());
  for (auto &stage : plan.escalation_stages) {
    rebind_target(stage.command_target);
    stage.containment_granularity = core::ContainmentGranularity::kResource;
    stage.containment_scope = core::ResolvedStopResourceContainmentScopeDTO{
        path.activation_set_id, resources};
    for (auto &evidence : stage.evidence_requirements) {
      if (evidence.requirement.evidence_kind ==
          core::StopEvidenceRequirementKind::kMotionRest) {
        evidence.requirement.evidence_item_id = core::StopEvidenceItemId(
            evidence.requirement.evidence_item_id.value() + evidence_suffix);
      }
      rebind_target(evidence.resolved_source.runtime_target);
      evidence.resolved_source.state_authority_id = path.state_authority_id;
      evidence.resolved_source.state_authority_class =
          path.state_authority_class;
      evidence.requirement.source_requirement.allowed_authority_classes = {
          path.state_authority_class};
    }
  }
  auto sealed = core::MakeResolvedStopBasePlan(std::move(plan));
  if (!sealed.ok())
    throw std::logic_error(sealed.error().message);
  auto readiness = core::verify_runtime_participant_stop_readiness(
      {value.controllable_component_id, core::ComponentLifecycleGeneration(1U),
       domain.value(), std::move(sealed.value())},
      binding, domain);
  if (!readiness.ok())
    throw std::logic_error(readiness.error().message);
  return std::move(readiness.value());
}

[[nodiscard]] core::StopStatusDTO EmptyCapturedStopStatus(
    const core::CellId &cell_id, const core::RetainedBundleId &bundle_id,
    const core::GraphRevision &graph_revision) {
  const core::StopId stop_id("stop.cell-owner");
  const core::StopGeneration stop_generation(1U);
  const auto certificate_result = core::ReduceStopTerminal(
      {core::StopCertificateId("certificate.cell-owner"),
       core::StopCertificateRevision(1U), stop_id, stop_generation,
       {core::StopRequestedSelectorKindDTO::kExplicitFull,
        core::ContentDigest(StopDigest('1')), std::nullopt, std::nullopt},
       core::StopResolutionDispositionDTO::kExact,
       core::StopScopeResolutionReasonDTO::kExplicitFullStop,
       {},
       {core::StopScopeCaptureHistoryDTO{
           1U, core::StopResolutionDispositionDTO::kExact,
           core::StopScopeResolutionReasonDTO::kExplicitFullStop,
           core::ContentDigest(StopDigest('2')), {}, {}, {}}},
       {}, {}, {}, std::nullopt, std::nullopt, std::nullopt, 20,
       std::nullopt});
  if (!certificate_result.ok()) {
    throw std::runtime_error(certificate_result.error().message);
  }
  const auto &certificate = certificate_result.value();
  const core::StopCertificateRefDTO certificate_ref{
      certificate.stop_certificate_id, certificate.stop_certificate_revision,
      certificate.stop_id, certificate.stop_generation};
  const core::StopDurableProjectionStatusDTO recorded{
      core::StopDurableProjectionStateDTO::kRecorded, std::nullopt,
      std::nullopt};
  core::StopStatusDTO status{
      .accepted = {core::RequestId("stop-request.cell-owner"),
                   core::RequestReplayStatusDTO::kAcceptedNew, stop_id,
                   stop_generation, core::ContentDigest(StopDigest('1')),
                   core::StopResolutionDispositionDTO::kExact,
                   core::StopScopeResolutionReasonDTO::kExplicitFullStop,
                   {}, core::ContentDigest(StopDigest('2')), {}, {}, 10},
      .state = core::StopStatusStateDTO::kSucceeded,
      .scope_entries = certificate.scope_entries,
      .current_stages = {},
      .attempted_stage_results = certificate.attempted_stage_results,
      .evidence_items = certificate.evidence_items,
      .unsettled = certificate.unsettled,
      .failed = certificate.failed,
      .acceptance_projection = recorded,
      .terminal_projection = recorded,
      .surveillance_projection =
          {core::StopDurableProjectionStateDTO::kNotApplicable, std::nullopt,
           std::nullopt},
      .stop_certificate = certificate_ref,
      .fault_latch = std::nullopt,
      .surveillance = std::nullopt};
  status.fault_latch = core::StopFaultLatchDTO{
      cell_id,
      stop_id,
      stop_generation,
      certificate_ref,
      core::StopFaultLatchId("latch.cell-owner"),
      core::StopFaultLatchGeneration(1U),
      core::StopFaultTriggerDTO::kPersistenceDegraded,
      true,
      {},
      {},
      bundle_id,
      graph_revision,
      status.terminal_projection,
      {core::StopDurableProjectionStateDTO::kPending, std::nullopt,
       std::nullopt},
      core::StopFaultLatchStateDTO::kLatched,
      1U,
      core::StopFaultRecoveryStatusDTO::kWaitingForObservation,
      "stop_fault_latched",
      {}};
  return status;
}

class NativeNoWriteOutputProvider final : public core::IIoOutputProvider {
public:
  core::IoOutputEvidence StrongestOutputEvidence() const noexcept override {
    return core::IoOutputEvidence::kSourceAccepted;
  }
  core::Result<core::IoOutputObservationDTO>
  SubmitOutput(const core::IoOutputWriteRequestDTO &) override {
    ++submissions;
    return core::Result<core::IoOutputObservationDTO>::Err(
        core::ErrorCode::kUnsupportedCapability);
  }
  core::Result<core::IoOutputObservationDTO>
  ObserveOutputResult(const core::IoOutputCommandIdentityDTO &) const override {
    return core::Result<core::IoOutputObservationDTO>::Err(
        core::ErrorCode::kUnsupportedCapability);
  }
  core::Result<void>
  RetireOutput(const core::IoOutputCommandIdentityDTO &) override {
    return core::Result<void>::Ok();
  }
  std::size_t submissions{};
};

core::RuntimeParticipantBinding NativeLogicalOutputBinding() {
  auto value = qualification_fixture::make_binding().value();
  const auto physical = *value.control_path;
  value.capability_id = core::CapabilityId("io.gpio.set_output");
  const auto *capability =
      core::find_builtin_capability_definition("io.gpio.set_output");
  value.capability_definition_revision = core::CapabilityDefinitionRevision(
      std::string(capability->capability_definition_revision));
  const auto schema = core::builtin_io_signal_write_command_schema();
  value.command_schema = core::ProgramCommandSchemaRefDTO{
      schema.command_schema_id, schema.command_schema_revision};
  value.model_bindings.clear();
  value.model_groups.clear();
  value.frame_bindings.clear();
  value.resources.clear();
  value.collision_body_bindings.clear();
  value.contact_grasp_bindings.clear();
  value.supporting_capability_provider_paths.clear();
  value.control_path.reset();
  const auto digest = [](char c) {
    return std::string("sha256:") + std::string(64U, c);
  };
  core::ResolvedLifecycleProviderOccurrenceDTO provider{
      core::ControlDomainId("domain.output"),
      core::ControlDomainRevision(digest('1')),
      physical.provider_instance_id,
      physical.provider_manifest_id,
      physical.provider_manifest_revision,
      physical.provider_runtime_unit_id,
      physical.provider_runtime_epoch,
      core::ContentDigest(digest('2'))};
  value.capability_provider_path = core::RuntimeCapabilityProviderPathDTO{
      value.capability_id,
      value.capability_definition_revision,
      core::CapabilityProviderBindingId("output.binding"),
      core::CapabilityProviderBindingRevision(digest('3')),
      provider.provider_instance_id,
      provider.provider_manifest_id,
      provider.provider_manifest_revision,
      provider.provider_runtime_unit_id,
      provider.provider_runtime_epoch};
  core::IoSignalDescriptorDTO descriptor{
      {value.controllable_component_id, "lamp"},
      core::InterruptSourceId("outputs"),
      core::IoSignalValueType::kBoolean,
      core::IoSignalDirection::kOutput,
      core::IoSignalUpdateMode::kChangeOnly,
      core::IoTransitionCapability::kSampledOnly,
      {core::IoFreshnessMode::kProviderValidity, {}},
      core::ResourceId("actual.output.channel")};
  const auto descriptor_digest =
      core::ComputeIoSignalDescriptorDigest(descriptor);
  if (!descriptor_digest.ok())
    throw std::logic_error(descriptor_digest.error().message);
  value.io_output_binding = core::RuntimeIoOutputBindingDTO{
      core::ResourceRoleId("lamp"),
      descriptor,
      descriptor_digest.value(),
      core::ComponentLifecycleGeneration(3U),
      provider,
      core::ContentDigest(digest('4')),
      core::IoOutputCompletionPolicyDTO(core::IoOutputImmediateDTO{})};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok())
    throw std::logic_error(sealed.error().message);
  return std::move(sealed.value());
}

TEST(RetainedNativeCellRuntimeTest,
     LogicalOutputRegistrationUsesActualChannelWithoutPhysicalStopSource) {
  const auto binding = NativeLogicalOutputBinding();
  auto clock = std::make_shared<WallClock>();
  NativeNoWriteOutputProvider provider;
  core::IoOutputParticipantEngine engine(provider, *clock);
  auto registration = TestRegistration(binding, &engine);
  registration.prepared_motion_engine = &engine;
  auto input = EmptyCellInput();
  input.clock = clock;
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  const auto journal_path = input.operation_journal_path;
  input.participant_registrations = {registration};
  input.kernel_config.graph_revision = binding.value().graph_revision;
  input.kernel_config.participant_resolver = UnusedResolver();
  {
    RetainedNativeCellRuntime cell(std::move(input));
    ASSERT_EQ(cell.participant_registrations().size(), 1U);
    EXPECT_TRUE(cell.participant_registrations()
                    .front()
                    .binding.value()
                    .resources.empty());
    EXPECT_FALSE(cell.participant_registrations().front().stop_readiness);
    EXPECT_TRUE(cell.stop_surveillance_owner()->SnapshotSources().empty());
    const auto slots = cell.addressed_kernel().resource_snapshot();
    ASSERT_EQ(slots.size(), 1U);
    EXPECT_EQ(slots.front().resource_id,
              core::ResourceId("actual.output.channel"));
    const auto readiness = cell.addressed_kernel().component_command_readiness(
        binding.value().controllable_component_id);
    ASSERT_TRUE(readiness.ok());
    EXPECT_FALSE(readiness.value().command_eligible);
    EXPECT_EQ(provider.submissions, 0U);
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     LogicalOutputDoesNotWaivePhysicalOrExplicitStopSourceRequirements) {
  NativeNoWriteOutputProvider provider;
  WallClock clock;
  core::IoOutputParticipantEngine engine(provider, clock);
  const auto output = TestRegistration(NativeLogicalOutputBinding(), &engine);
  CellTestExecutionEngine physical_engine;
  const auto physical =
      TestRegistration(qualification_fixture::make_binding(), &physical_engine);
  const auto bootstrap =
      RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty;
  EXPECT_TRUE(RetainedNativeCellRuntime::ValidateStopSourceCoverage(
                  {}, bootstrap, {output})
                  .ok());
  EXPECT_FALSE(RetainedNativeCellRuntime::ValidateStopSourceCoverage(
                   {}, bootstrap, {physical})
                   .ok());
  EXPECT_FALSE(RetainedNativeCellRuntime::ValidateStopSourceCoverage(
                   {}, bootstrap, {output, physical})
                   .ok());
  EXPECT_FALSE(RetainedNativeCellRuntime::ValidateStopSourceCoverage(
                   {},
                   RetainedStopSurveillanceEmptySourceMode::kRequireNonEmpty,
                   {output})
                   .ok());
  auto invalid = output;
  invalid.engine = nullptr;
  EXPECT_FALSE(RetainedNativeCellRuntime::ValidateStopSourceCoverage(
                   {}, bootstrap, {invalid})
                   .ok());
  EXPECT_EQ(provider.submissions, 0U);
}

TEST(
    RetainedNativeCellRuntimeTest,
    LogicalOutputComponentSetRequiresExactCandidateAndPredecessorProviderFence) {
  NativeNoWriteOutputProvider provider;
  auto clock = std::make_shared<WallClock>();
  core::IoOutputParticipantEngine engine(provider, *clock);
  const auto binding = NativeLogicalOutputBinding();
  auto registration = TestRegistration(binding, &engine);
  registration.prepared_motion_engine = &engine;
  const auto &output = *binding.value().io_output_binding;
  const auto resource = *output.signal.output_resource;
  const auto scope_for = [&](bool candidate) {
    core::ConfigurationFenceDomainScopeDTO domain{
        output.provider.control_domain_id,
        {},
        {},
        core::ProviderConfigurationFenceDomainDTO{}};
    auto &proof =
        std::get<core::ProviderConfigurationFenceDomainDTO>(domain.payload);
    const core::ProviderConfigurationFenceOccurrenceDTO occurrence{
        binding.value().controllable_component_id,
        output.provider.provider_instance_id,
        output.signal_configuration_digest,
        {resource}};
    if (candidate) {
      domain.candidate_control_domain_revision =
          output.provider.control_domain_revision;
      proof.candidate_provider = occurrence;
    } else {
      domain.predecessor_control_domain_revision =
          output.provider.control_domain_revision;
      proof.predecessor_provider = occurrence;
    }
    return core::ConfigurationFenceScopeDTO{
        {binding.value().controllable_component_id}, {domain}, {}, {resource}};
  };
  const auto candidate_scope = scope_for(true);
  const auto predecessor_scope = scope_for(false);
  ASSERT_TRUE(candidate_scope.valid());
  ASSERT_TRUE(predecessor_scope.valid());
  const core::ConfigurationFenceId fence_id("fence.logical-output");
  CellTestAdmissionGate gate;
  auto input = EmptyCellInput();
  input.clock = clock;
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config.operation_admission_gate = &gate;
  const auto journal_path = input.operation_journal_path;
  {
    RetainedNativeCellRuntime cell(std::move(input));
    const auto transition = [&](const auto &scope, bool add) {
      gate.HoldFence(fence_id, scope);
      return cell.reconfigure_runtime_component_set(
          add ? std::vector<
                    core::RuntimeParticipantExecutionRegistration>{registration}
              : std::vector<core::RuntimeParticipantExecutionRegistration>{},
          add ? UnusedResolver() : core::RuntimeParticipantAdmissionResolver{},
          {}, binding.value().retained_bundle_id,
          binding.value().graph_revision, fence_id, scope, {});
    };
    // Exact held-fence permission alone does not replace provider-side proof.
    for (unsigned mutation = 0; mutation != 5U; ++mutation) {
      auto foreign = candidate_scope;
      auto &domain = foreign.domains.front();
      auto &proof =
          std::get<core::ProviderConfigurationFenceDomainDTO>(domain.payload);
      switch (mutation) {
      case 0:
        proof.candidate_provider->provider_instance_id =
            core::ProviderInstanceId("foreign.provider");
        break;
      case 1:
        proof.candidate_provider->signal_configuration_digest =
            core::ContentDigest(std::string("sha256:") + std::string(64U, 'f'));
        break;
      case 2:
        domain.candidate_control_domain_revision = core::ControlDomainRevision(
            std::string("sha256:") + std::string(64U, 'e'));
        break;
      case 3:
        proof.candidate_provider->output_resources.clear();
        foreign.resources.clear();
        break;
      case 4:
        foreign = predecessor_scope;
        break;
      }
      ASSERT_TRUE(foreign.valid());
      const auto rejected = transition(foreign, true);
      ASSERT_FALSE(rejected.ok());
      EXPECT_EQ(rejected.error().details.at("reason"),
                "runtime_component_set_scope_incomplete");
      EXPECT_TRUE(cell.participant_registrations().empty());
      EXPECT_TRUE(cell.addressed_kernel().resource_snapshot().empty());
    }
    const auto installed = transition(candidate_scope, true);
    ASSERT_TRUE(installed.ok()) << installed.error().message;
    ASSERT_EQ(cell.participant_registrations().size(), 1U);
    ASSERT_EQ(cell.addressed_kernel().resource_snapshot().size(), 1U);
    EXPECT_EQ(cell.addressed_kernel().resource_snapshot().front().resource_id,
              resource);
    EXPECT_TRUE(cell.stop_surveillance_owner()->SnapshotSources().empty());
    const auto readback =
        cell.addressed_kernel().current_runtime_provider_configuration(
            output.provider.control_domain_id);
    ASSERT_TRUE(readback.ok());
    EXPECT_FALSE(readback.value());
    const auto readiness = cell.addressed_kernel().component_command_readiness(
        binding.value().controllable_component_id);
    ASSERT_TRUE(readiness.ok());
    EXPECT_FALSE(readiness.value().command_eligible);
    const auto wrong_side = transition(candidate_scope, false);
    ASSERT_FALSE(wrong_side.ok());
    EXPECT_EQ(wrong_side.error().details.at("reason"),
              "runtime_component_set_scope_incomplete");
    EXPECT_EQ(cell.participant_registrations().size(), 1U);
    const auto removed = transition(predecessor_scope, false);
    ASSERT_TRUE(removed.ok()) << removed.error().message;
    EXPECT_TRUE(cell.participant_registrations().empty());
    EXPECT_TRUE(cell.addressed_kernel().resource_snapshot().empty());
    EXPECT_EQ(provider.submissions, 0U);
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     OwnsStableCellServicesWithoutAMotionRuntime) {
  auto clock = std::make_shared<WallClock>();
  const std::weak_ptr<WallClock> clock_lifetime = clock;
  auto scheduler = std::make_shared<SimpleScheduler>();
  std::shared_ptr<RetainedStopSurveillanceOwner> observer_handle;
  const auto journal_path = TemporaryJournalPath();

  auto input = EmptyCellInput();
  input.clock = clock;
  input.scheduler = scheduler;
  input.operation_journal_path = journal_path;
  input.kernel_config = EmptyKernelConfig();
  input.stop_source_mode =
      RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    EXPECT_EQ(cell.clock(), clock);
    EXPECT_EQ(cell.scheduler(), scheduler);
    EXPECT_NE(cell.stop_surveillance_owner(), nullptr);
    observer_handle = cell.stop_surveillance_owner();
    EXPECT_EQ(&cell.operation_journal(), &cell.operation_journal());
    EXPECT_EQ(&cell.addressed_kernel(), &cell.addressed_kernel());
    EXPECT_TRUE(cell.participant_registrations().empty());
    EXPECT_FALSE(cell.participant_resolver());
    EXPECT_TRUE(cell.stop_surveillance_owner()->SnapshotSources().empty());
    clock.reset();
  }
  EXPECT_FALSE(clock_lifetime.expired());
  observer_handle.reset();
  EXPECT_TRUE(clock_lifetime.expired());

  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     RecoveryFactsUseCellAuthorityWithoutAMotionRuntime) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    auto status = EmptyCapturedStopStatus(
        core::CellId("cell.empty-cell"),
        core::RetainedBundleId("bundle.empty-cell"),
        core::GraphRevision("graph.empty-cell"));
    const auto facts = cell.BuildStopRecoveryFacts(status, -1);
    ASSERT_FALSE(facts.ok());
    EXPECT_EQ(facts.error().message,
              "native Stop recovery requires exact terminal status and latch");
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     EmptyStopSourcesRequireManagedBootstrapMode) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  input.stop_source_mode =
      RetainedStopSurveillanceEmptySourceMode::kRequireNonEmpty;
  const auto journal_path = input.operation_journal_path;

  EXPECT_THROW(RetainedNativeCellRuntime(std::move(input)),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(journal_path));
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     ProductResealPreservesExactPhysicalProviderFactsAndStrictAliasOwnership) {
  const auto before = qualification_fixture::make_binding();
  auto value = before.value();
  value.program_id = core::ProgramId("candidate.program");
  value.program_revision = core::ProgramRevision("candidate.program-revision");
  value.program_binding_id =
      core::ProgramBindingId("candidate.program-binding");
  value.retained_bundle_id = core::RetainedBundleId("candidate.bundle");
  value.graph_revision = core::GraphRevision("candidate.graph");
  value.component_role_id = core::ComponentRoleId("candidate.robot");
  value.target_role_id = core::TargetRoleId("candidate.arm");
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(sealed.ok()) << sealed.error().message;
  const auto candidate = std::move(sealed.value());
  EXPECT_TRUE(retained_bindings_preserve_physical_owner_after_product_reseal(
      before, candidate));
  EXPECT_TRUE(retained_bindings_preserve_physical_owner_after_product_reseal(
      candidate, before));
  // The capability-alias classifier still requires the same product seal.
  EXPECT_FALSE(retained_bindings_share_backend_owner(before, candidate));

  const std::vector<std::function<void(core::RuntimeParticipantBindingDTO &)>>
      foreign_facts{[](auto &v) {
                      v.resources.front().resource_id =
                          core::ResourceId("foreign.joint");
                    },
                    [](auto &v) {
                      v.frame_bindings.front().compiled_frame_name =
                          "foreign.base";
                    },
                    [](auto &v) {
                      v.control_path->control_binding_id =
                          core::ControlBindingId("foreign.binding");
                    },
                    [](auto &v) {
                      v.control_path->provider_instance_id =
                          core::ProviderInstanceId("foreign.provider");
                    },
                    [](auto &v) {
                      v.capability_definition_revision =
                          core::CapabilityDefinitionRevision(
                              "foreign.capability-revision");
                    },
                    [](auto &v) {
                      v.supporting_capability_provider_paths.front()
                          .provider_instance_id =
                          core::ProviderInstanceId("foreign.planner");
                    },
                    [](auto &v) {
                      const core::RuntimeEpoch epoch("foreign.epoch");
                      v.control_path->provider_runtime_epoch = epoch;
                      v.supporting_capability_provider_paths.front()
                          .provider_runtime_epoch = epoch;
                      v.runtime_unit_epochs.front().runtime_epoch = epoch;
                    }};
  for (std::size_t index = 0; index < foreign_facts.size(); ++index) {
    SCOPED_TRACE(index);
    auto foreign_value = candidate.value();
    foreign_facts[index](foreign_value);
    auto foreign =
        core::verify_runtime_participant_binding(std::move(foreign_value));
    ASSERT_TRUE(foreign.ok()) << foreign.error().message;
    EXPECT_FALSE(retained_bindings_preserve_physical_owner_after_product_reseal(
        before, foreign.value()));
  }
}

TEST(RetainedNativeCellRuntimeTest,
     QualifiedSecondaryCapabilityRequiresExactPhysicalStopOwnerAndEvidence) {
  // The shared engine fixture uses placeholder revisions. A resolved Stop
  // plan requires content-addressed identities even for this synthetic test.
  auto primary_value = qualification_fixture::make_binding().value();
  primary_value.retained_bundle_id = core::RetainedBundleId(StopDigest('6'));
  primary_value.graph_revision = core::GraphRevision(StopDigest('7'));
  auto &primary_path = *primary_value.control_path;
  primary_path.control_domain_revision =
      core::ControlDomainRevision(StopDigest('8'));
  primary_path.control_binding_revision =
      core::ControlBindingRevision(StopDigest('9'));
  primary_path.topology_revision = core::TopologyRevision(StopDigest('a'));
  primary_path.provider_manifest_revision =
      core::ProviderManifestRevision(StopDigest('b'));
  primary_path.stop_evidence_policy_revision =
      core::StopEvidencePolicyRevision(StopDigest('c'));
  auto sealed_primary =
      core::verify_runtime_participant_binding(std::move(primary_value));
  ASSERT_TRUE(sealed_primary.ok()) << sealed_primary.error().message;
  const auto primary = std::move(sealed_primary.value());
  auto secondary_value = primary.value();
  const auto *feedback = core::find_builtin_capability_definition(
      "execution.feedback.state_snapshots");
  ASSERT_NE(feedback, nullptr);
  secondary_value.capability_id =
      core::CapabilityId(std::string(feedback->capability_id));
  secondary_value.capability_definition_revision =
      core::CapabilityDefinitionRevision(
          std::string(feedback->capability_definition_revision));
  secondary_value.control_path->control_binding_id =
      core::ControlBindingId("binding.arm.feedback");
  const auto secondary =
      core::verify_runtime_participant_binding(std::move(secondary_value));
  ASSERT_TRUE(secondary.ok()) << secondary.error().message;
  ASSERT_FALSE(core::runtime_participant_binding_physically_contains(
      primary, secondary.value()));
  ASSERT_TRUE(
      retained_bindings_share_backend_owner(primary, secondary.value()));
  const auto &path = *primary.value().control_path;
  const core::ControllerRoleId controller("controller.arm");
  std::vector<core::ControllerInterfaceInstanceDTO> interfaces;
  for (const auto &resource : primary.value().resources)
    interfaces.push_back({resource.resource_id, "position"});
  core::ControllerLifecycleExpectedDomainDTO expected{
      path.control_domain_id,
      path.control_domain_revision,
      path.controller_topology_id,
      path.topology_revision,
      core::ProductId("product.control"),
      core::ProductContractId("control.contract"),
      core::ProductContractRevision(StopDigest('1')),
      core::ContentDigest(StopDigest('2')),
      core::ContentDigest(StopDigest('3')),
      path.provider_runtime_unit_id,
      path.provider_runtime_epoch,
      {path.controller_topology_id,
       path.topology_revision,
       {{controller,
         {{path.control_interface_contract_id,
           path.control_interface_revision,
           {core::ControllerClaimDirection::kCommand,
            core::ControllerClaimDirection::kState}}},
         {},
         {}}},
       {},
       {{path.activation_set_id, {controller}, {}}},
       {{path.control_route_id,
         path.control_interface_contract_id,
         path.control_interface_revision,
         {controller},
         {path.activation_set_id}}}},
      {path.control_domain_id,
       path.control_domain_revision,
       path.controller_topology_id,
       path.topology_revision,
       path.activation_set_id,
       {}},
      {},
      {{controller,
        core::ControllerImplementationId("controller.native"),
        core::ControllerImplementationRevision(StopDigest('4')),
        "native.controller",
        core::ContentDigest(StopDigest('5')),
        interfaces,
        interfaces,
        {},
        {}}},
      {}};
  for (const auto &binding : {primary, secondary.value()}) {
    const auto &control = *binding.value().control_path;
    expected.desired_configuration.binding_routes.push_back(
        {control.control_binding_id, control.control_binding_revision,
         control.control_route_id});
    expected.expected_bindings.push_back(
        {control.control_binding_id, control.control_binding_revision,
         control.control_interface_contract_id,
         control.control_interface_revision, interfaces, interfaces});
    expected.expected_endpoints.push_back(
        {control.control_binding_id, control.control_binding_revision,
         control.control_route_id, core::EndpointRoleId("command.stop"),
         "command.stop", true, false});
  }
  const auto domain =
      core::verify_controller_lifecycle_expected_domain(std::move(expected));
  ASSERT_TRUE(domain.ok()) << domain.error().message;
  CellTestExecutionEngine engine, other_engine;
  class StopPort final : public core::IControlDomainExecutionPort {
    core::ParticipantStopStageResultDTO
    TranslateStopStage(const core::ResolvedParticipantStopStageDTO &,
                       const core::RemainingStopBudgetDTO &,
                       std::stop_token) override {
      throw std::logic_error("coverage test must not dispatch a Stop stage");
    }
  } port, other_port;
  auto source_registration = TestRegistration(primary, &engine);
  source_registration.stop_readiness =
      StopReadinessFor(primary, domain.value());
  source_registration.stop_execution_port = &port;
  auto covered = TestRegistration(secondary.value(), &engine);
  covered.stop_readiness =
      StopReadinessFor(secondary.value(), domain.value(), ".feedback");
  covered.stop_execution_port = &port;
  auto source = StopSourceFor(primary);
  const auto add_observations = [&](const auto &readiness) {
    for (const auto &stage : readiness.value()
                                 .resolved_stop_base_plan.escalation_stages)
      for (const auto &evidence : stage.evidence_requirements)
        source.observation_sources.push_back(
            {evidence.requirement, evidence.resolved_source,
             std::get<core::ResolvedStopResourceContainmentScopeDTO>(
                 stage.containment_scope)
                 .resource_ids});
  };
  add_observations(*source_registration.stop_readiness);
  const auto primary_only_source = source;
  add_observations(*covered.stop_readiness);
  const auto validate = [&](const auto &sources, const auto &rows) {
    return RetainedNativeCellRuntime::ValidateStopSourceCoverage(
        sources,
        RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty,
        rows);
  };
  const std::vector<core::RuntimeParticipantExecutionRegistration>
      registrations{source_registration, covered};
  const std::vector<RetainedStopSurveillanceSourceInput> sources{source};
  const auto accepted = validate(sources, registrations);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto primary_only = validate(
      std::vector<RetainedStopSurveillanceSourceInput>{primary_only_source},
      registrations);
  ASSERT_FALSE(primary_only.ok());
  EXPECT_EQ(primary_only.error().details.at("reason"),
            "native_cell_stop_source_evidence_missing");
  for (const bool change_engine : {true, false}) {
    auto wrong_owner = registrations;
    if (change_engine)
      wrong_owner.back().engine = &other_engine;
    else
      wrong_owner.back().stop_execution_port = &other_port;
    const auto rejected = validate(sources, wrong_owner);
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().details.at("reason"),
              "native_cell_stop_source_owner_mismatch");
  }
  auto unqualified_source = registrations;
  unqualified_source.front().stop_readiness.reset();
  EXPECT_FALSE(validate(sources, unqualified_source).ok());
  auto changed_generation = registrations;
  const auto newer = core::rebind_runtime_participant_stop_readiness_generation(
      *covered.stop_readiness, core::ComponentLifecycleGeneration(2U));
  ASSERT_TRUE(newer.ok());
  changed_generation.back().stop_readiness = newer.value();
  EXPECT_FALSE(validate(sources, changed_generation).ok());
  auto authored_alias = registrations;
  auto alias_value = secondary.value().value();
  alias_value.program_binding_id =
      core::ProgramBindingId("authored.program-binding");
  const auto alias =
      core::verify_runtime_participant_binding(std::move(alias_value));
  ASSERT_TRUE(alias.ok());
  authored_alias.back().binding = alias.value();
  EXPECT_FALSE(validate(sources, authored_alias).ok());
  EXPECT_FALSE(validate(std::vector<RetainedStopSurveillanceSourceInput>{},
                        registrations)
                   .ok());
  EXPECT_FALSE(
      validate(
          sources,
          std::vector<core::RuntimeParticipantExecutionRegistration>{covered})
          .ok());
  auto missing_evidence = sources;
  missing_evidence.front().observation_sources.pop_back();
  const auto incomplete = validate(missing_evidence, registrations);
  ASSERT_FALSE(incomplete.ok());
  EXPECT_EQ(incomplete.error().details.at("reason"),
            "native_cell_stop_source_evidence_missing");
}

TEST(RetainedNativeCellRuntimeTest,
     RejectsMissingStableTimeOwnersBeforeOpeningTheJournal) {
  auto input = EmptyCellInput();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  EXPECT_THROW(RetainedNativeCellRuntime(std::move(input)),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(journal_path));
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     AdoptsPreverifiedReadinessSnapshotWithNoThrowSwaps) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
    core::RuntimeParticipantAdmissionResolver resolver =
        [](const core::OperationIntentDTO &, const std::size_t) {
          return core::Result<
              core::RuntimeParticipantExecutionRegistration>::Err(
              {"test_resolver", "test resolver is not invoked", {}});
        };
    static_assert(noexcept(cell.AdoptVerifiedComponentReadinessSnapshot(
        std::move(registrations), std::move(resolver))));

    cell.AdoptVerifiedComponentReadinessSnapshot(std::move(registrations),
                                                 std::move(resolver));

    EXPECT_TRUE(cell.participant_registrations().empty());
    EXPECT_TRUE(static_cast<bool>(cell.participant_resolver()));
    EXPECT_FALSE(static_cast<bool>(resolver));
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     CCLCReadinessAdapterForwardsCloseAndReadAndRequiresPublicationAuthority) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    bool candidate_builder_called = false;
    RetainedNativeCellCommandReadinessPort readiness_port(
        cell, [&candidate_builder_called](
                  const core::ComponentCommandReadinessPublicationDTO&,
                  const std::vector<core::RuntimeParticipantExecutionRegistration>&
                      current) {
          candidate_builder_called = true;
          RetainedNativeCellReadinessCandidate candidate;
          candidate.registrations = current;
          candidate.adopt_component_owners = []() noexcept {};
          return core::Result<RetainedNativeCellReadinessCandidate>::Ok(
              std::move(candidate));
        });
    const core::ControllableComponentId component("gripper.component");

    const auto initial =
        readiness_port.ReadComponentCommandReadiness(component);
    ASSERT_TRUE(initial.ok()) << initial.error().message;
    EXPECT_EQ(initial.value().controllable_component_id, component);
    EXPECT_FALSE(initial.value().component_lifecycle_generation.has_value());
    EXPECT_FALSE(initial.value().command_eligible);

    const auto close = readiness_port.CloseComponentCommandEligibility(
        core::LifecycleTransactionId("lifecycle.not-held"), component);
    ASSERT_FALSE(close.ok());
    ASSERT_TRUE(close.error().details.contains("reason"));
    EXPECT_EQ(close.error().details.at("reason"),
              "component_readiness_claim_not_held");

    const auto publish = readiness_port.PublishComponentCommandReadiness(
        {core::ComponentReadinessAuthorityDTO::kRestartExclusion,
         std::nullopt, std::nullopt, component,
         core::ComponentLifecycleGeneration(1U), true});
    ASSERT_FALSE(publish.ok());
    EXPECT_TRUE(candidate_builder_called);
    ASSERT_TRUE(publish.error().details.contains("reason"));
    EXPECT_EQ(publish.error().details.at("reason"),
              "component_readiness_restart_exclusion_not_held");

    const auto after = readiness_port.ReadComponentCommandReadiness(component);
    ASSERT_TRUE(after.ok()) << after.error().message;
    EXPECT_FALSE(after.value().component_lifecycle_generation.has_value());
    EXPECT_FALSE(after.value().command_eligible);
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     GenericCellPublishesClosedReadinessWithoutAMotionOwner) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    bool candidate_built = false;
    bool owners_adopted = false;
    RetainedNativeCellCommandReadinessPort readiness_port(
        cell, [&](const core::ComponentCommandReadinessPublicationDTO &,
                  const std::vector<
                      core::RuntimeParticipantExecutionRegistration> &current) {
          candidate_built = true;
          RetainedNativeCellReadinessCandidate candidate;
          candidate.registrations = current;
          candidate.adopt_component_owners = [&owners_adopted]() noexcept {
            owners_adopted = true;
          };
          return core::Result<RetainedNativeCellReadinessCandidate>::Ok(
              std::move(candidate));
        });
    const core::ControllableComponentId gripper_component(
        "cell.gripper.component");

    ASSERT_TRUE(cell.addressed_kernel().SetRouteRecoveryPending(true).ok());
    const auto published = readiness_port.PublishComponentCommandReadiness(
        {core::ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt,
         std::nullopt, gripper_component,
         core::ComponentLifecycleGeneration(3U), false});
    ASSERT_TRUE(published.ok()) << published.error().message;
    EXPECT_TRUE(candidate_built);
    EXPECT_TRUE(owners_adopted);
    EXPECT_EQ(published.value(),
              (core::ComponentCommandReadinessDTO{
                  gripper_component, core::ComponentLifecycleGeneration(3U),
                  false}));
    const auto readback =
        readiness_port.ReadComponentCommandReadiness(gripper_component);
    ASSERT_TRUE(readback.ok()) << readback.error().message;
    EXPECT_EQ(readback.value(), published.value());
    EXPECT_TRUE(cell.participant_registrations().empty());
    EXPECT_FALSE(cell.participant_resolver());
    ASSERT_TRUE(cell.addressed_kernel().SetRouteRecoveryPending(false).ok());
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     CandidateFailureLeavesGenericReadinessClosedAndUnpublished) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    RetainedNativeCellCommandReadinessPort readiness_port(
        cell, [](const core::ComponentCommandReadinessPublicationDTO &,
                 const std::vector<
                     core::RuntimeParticipantExecutionRegistration> &) {
          return core::Result<RetainedNativeCellReadinessCandidate>::Err(
              {"candidate_stale", "candidate snapshot is stale",
               {{"reason", "component_readiness_candidate_stale"}}});
        });
    const core::ControllableComponentId component("cell.gripper.stale");

    ASSERT_TRUE(cell.addressed_kernel().SetRouteRecoveryPending(true).ok());
    const auto rejected = readiness_port.PublishComponentCommandReadiness(
        {core::ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt,
         std::nullopt, component, core::ComponentLifecycleGeneration(4U),
         false});
    ASSERT_FALSE(rejected.ok());
    EXPECT_EQ(rejected.error().details.at("reason"),
              "component_readiness_candidate_stale");
    const auto readback =
        readiness_port.ReadComponentCommandReadiness(component);
    ASSERT_TRUE(readback.ok()) << readback.error().message;
    EXPECT_FALSE(readback.value().component_lifecycle_generation.has_value());
    EXPECT_FALSE(readback.value().command_eligible);
    EXPECT_TRUE(cell.participant_registrations().empty());
    ASSERT_TRUE(cell.addressed_kernel().SetRouteRecoveryPending(false).ok());
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     MixedRetainRemovePublishesMatchingRegistrationAndStopSourceSets) {
  const auto retained_binding = qualification_fixture::make_binding();
  const auto removed_binding = DistinctCellBinding(retained_binding, "peer");
  CellTestExecutionEngine retained_engine;
  CellTestExecutionEngine removed_engine;
  auto resolver = UnusedResolver();
  CellTestAdmissionGate admission_gate;
  const auto fence_id =
      core::ConfigurationFenceId("fence.native-cell.remove-peer");
  const auto &removed_path = *removed_binding.value().control_path;
  auto removed_resources = std::vector<core::ResourceId>{};
  for (const auto &resource : removed_binding.value().resources) {
    removed_resources.push_back(resource.resource_id);
  }
  std::sort(removed_resources.begin(), removed_resources.end());
  const core::ConfigurationFenceScopeDTO scope{
      {removed_binding.value().controllable_component_id},
      {{removed_path.control_domain_id,
        removed_path.control_domain_revision, std::nullopt}},
      {{removed_path.control_binding_id,
        removed_path.control_binding_revision, std::nullopt}},
      removed_resources};
  ASSERT_TRUE(scope.valid());
  admission_gate.HoldFence(fence_id, scope);

  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.participant_registrations = {
      TestRegistration(retained_binding, &retained_engine),
      TestRegistration(removed_binding, &removed_engine)};
  input.kernel_config = {
      core::RequestOwnerEpoch("owner.native-cell.mixed"),
      core::CellId("cell.native-cell.mixed"), 32U, resolver, &admission_gate,
      retained_binding.value().graph_revision};
  input.stop_sources = {StopSourceFor(retained_binding),
                        StopSourceFor(removed_binding)};
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    ASSERT_EQ(cell.participant_registrations().size(), 2U);
    ASSERT_EQ(cell.stop_surveillance_owner()->SnapshotSources().size(), 2U);
    ASSERT_TRUE(cell.addressed_kernel().participant_engine_in_use(
        &retained_engine));
    ASSERT_TRUE(cell.addressed_kernel().participant_engine_in_use(
        &removed_engine));

    int commit_calls = 0;
    const std::vector<core::ControllableComponentId> retained_components{
        retained_binding.value().controllable_component_id};
    const auto wrong_fence = cell.reconfigure_runtime_component_set(
        {TestRegistration(retained_binding, &retained_engine)}, resolver,
        {StopSourceFor(retained_binding)},
        retained_binding.value().retained_bundle_id,
        retained_binding.value().graph_revision,
        core::ConfigurationFenceId("fence.native-cell.wrong"), scope,
        retained_components, [&commit_calls] {
          ++commit_calls;
          return core::Result<void>::Ok();
        });
    ASSERT_FALSE(wrong_fence.ok());
    ASSERT_TRUE(wrong_fence.error().details.contains("reason"));
    EXPECT_EQ(wrong_fence.error().details.at("reason"),
              "runtime_component_set_fence_missing");
    EXPECT_EQ(commit_calls, 0);
    EXPECT_EQ(cell.participant_registrations().size(), 2U);
    EXPECT_EQ(cell.stop_surveillance_owner()->SnapshotSources().size(), 2U);
    const auto changed = cell.reconfigure_runtime_component_set(
        {TestRegistration(retained_binding, &retained_engine)}, resolver,
        {StopSourceFor(retained_binding)},
        retained_binding.value().retained_bundle_id,
        retained_binding.value().graph_revision, fence_id, scope,
        retained_components, [&commit_calls] {
          ++commit_calls;
          return core::Result<void>::Ok();
        });
    ASSERT_TRUE(changed.ok()) << changed.error().message;
    EXPECT_EQ(commit_calls, 1);

    ASSERT_EQ(cell.participant_registrations().size(), 1U);
    EXPECT_EQ(cell.participant_registrations().front().binding.value(),
              retained_binding.value());
    const auto stop_sources = cell.stop_surveillance_owner()->SnapshotSources();
    ASSERT_EQ(stop_sources.size(), 1U);
    EXPECT_EQ(stop_sources.front().binding.value(), retained_binding.value());
    const auto slots = cell.addressed_kernel().resource_table_snapshot();
    ASSERT_EQ(slots.slots.size(), retained_binding.value().resources.size());
    for (const auto &slot : slots.slots) {
      EXPECT_TRUE(std::any_of(
          retained_binding.value().resources.begin(),
          retained_binding.value().resources.end(), [&](const auto &resource) {
            return resource.resource_id == slot.resource_id;
          }));
    }
    EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(
        &retained_engine));
    EXPECT_FALSE(cell.addressed_kernel().participant_engine_in_use(
        &removed_engine));
    ASSERT_TRUE(
        cell.addressed_kernel().release_retired_engine(&removed_engine).ok());
    EXPECT_EQ(removed_engine.subscription_count(), 0U);
    EXPECT_EQ(retained_engine.subscription_count(), 1U);
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     GripperOnlyOwnerAssessesEmptyCaptureStopRecoveryWithoutMotionRuntime) {
  const auto binding = GripperOnlyBinding();
  CellTestExecutionEngine engine;
  auto resolver = UnusedResolver();
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.participant_registrations = {TestRegistration(binding, &engine)};
  input.kernel_config = {
      core::RequestOwnerEpoch("owner.native-cell.gripper"),
      core::CellId("cell.native-cell.gripper"), 32U, resolver, nullptr,
      binding.value().graph_revision};
  input.stop_sources = {StopSourceFor(binding)};
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    const auto status = EmptyCapturedStopStatus(
        core::CellId("cell.native-cell.gripper"),
        binding.value().retained_bundle_id, binding.value().graph_revision);
    // A latch that captured no resource has no physical rest obligation:
    // its exact facts are resource-free yet still name the owner's exact
    // control configuration, and are never rejected as incomplete.
    const auto facts = cell.BuildStopRecoveryFacts(status, 1);
    ASSERT_TRUE(facts.ok()) << facts.error().message;
    EXPECT_TRUE(facts.value().resource_facts.empty());
    ASSERT_FALSE(facts.value().configuration_facts.empty());
    EXPECT_TRUE(core::AssessStopFaultRecovery(status, facts.value()).ok());
    ASSERT_EQ(cell.participant_registrations().size(), 1U);
    EXPECT_EQ(cell.participant_registrations().front().binding.value().capability_id,
              core::CapabilityId("tool.gripper.command"));
    EXPECT_EQ(cell.stop_surveillance_owner()->SnapshotSources().size(), 1U);
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     RemovingEitherIndependentCapabilityRetainsTheOtherOwnerAndStopSource) {
  const auto arm = qualification_fixture::make_binding();
  const auto gripper = GripperOnlyBinding();
  ASSERT_NE(arm.value().controllable_component_id,
            gripper.value().controllable_component_id);

  for (const bool remove_arm : {false, true}) {
    const auto &retained = remove_arm ? gripper : arm;
    const auto &removed = remove_arm ? arm : gripper;
    CellTestExecutionEngine arm_engine;
    CellTestExecutionEngine gripper_engine;
    auto *retained_engine = remove_arm ? &gripper_engine : &arm_engine;
    auto *removed_engine = remove_arm ? &arm_engine : &gripper_engine;
    auto resolver = UnusedResolver();
    CellTestAdmissionGate admission_gate;
    const auto fence_id = core::ConfigurationFenceId(
        remove_arm ? "fence.native-cell.remove-arm"
                   : "fence.native-cell.remove-gripper");
    const auto &removed_path = *removed.value().control_path;
    std::vector<core::ResourceId> removed_resources;
    for (const auto &resource : removed.value().resources) {
      removed_resources.push_back(resource.resource_id);
    }
    std::sort(removed_resources.begin(), removed_resources.end());
    const core::ConfigurationFenceScopeDTO scope{
        {removed.value().controllable_component_id},
        {{removed_path.control_domain_id,
          removed_path.control_domain_revision, std::nullopt}},
        {{removed_path.control_binding_id,
          removed_path.control_binding_revision, std::nullopt}},
        removed_resources};
    ASSERT_TRUE(scope.valid());
    admission_gate.HoldFence(fence_id, scope);

    auto input = EmptyCellInput();
    input.clock = std::make_shared<WallClock>();
    input.scheduler = std::make_shared<SimpleScheduler>();
    input.operation_journal_path = TemporaryJournalPath();
    input.participant_registrations = {
        TestRegistration(arm, &arm_engine),
        TestRegistration(gripper, &gripper_engine)};
    input.kernel_config = {
        core::RequestOwnerEpoch("owner.native-cell.independent"),
        core::CellId("cell.native-cell.independent"), 32U, resolver,
        &admission_gate, arm.value().graph_revision};
    input.stop_sources = {StopSourceFor(arm), StopSourceFor(gripper)};
    const auto journal_path = input.operation_journal_path;

    {
      RetainedNativeCellRuntime cell(std::move(input));
      const auto retained_observation_wake = cell.observation_wake();
      ASSERT_NE(retained_observation_wake, nullptr);
      const auto observation_incarnation =
          retained_observation_wake->incarnation();
      ASSERT_TRUE(cell.addressed_kernel().participant_engine_in_use(
          retained_engine));
      ASSERT_TRUE(cell.addressed_kernel().participant_engine_in_use(
          removed_engine));

      const std::vector<core::ControllableComponentId> retained_components{
          retained.value().controllable_component_id};
      int commit_calls = 0;
      const auto changed = cell.reconfigure_runtime_component_set(
          {TestRegistration(retained, retained_engine)}, resolver,
          {StopSourceFor(retained)},
          retained.value().retained_bundle_id,
          retained.value().graph_revision, fence_id, scope,
          retained_components, [&commit_calls] {
            ++commit_calls;
            return core::Result<void>::Ok();
          });
      ASSERT_TRUE(changed.ok()) << changed.error().message;
      EXPECT_EQ(cell.observation_wake(), retained_observation_wake);
      EXPECT_EQ(cell.observation_wake()->incarnation(), observation_incarnation);
      EXPECT_EQ(commit_calls, 1);
      ASSERT_EQ(cell.participant_registrations().size(), 1U);
      EXPECT_EQ(cell.participant_registrations().front().engine,
                retained_engine);
      EXPECT_EQ(cell.participant_registrations().front().binding.value(),
                retained.value());
      const auto stop_sources =
          cell.stop_surveillance_owner()->SnapshotSources();
      ASSERT_EQ(stop_sources.size(), 1U);
      EXPECT_EQ(stop_sources.front().binding.value(), retained.value());
      EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(
          retained_engine));
      EXPECT_FALSE(cell.addressed_kernel().participant_engine_in_use(
          removed_engine));
      const auto slots = cell.addressed_kernel().resource_table_snapshot();
      EXPECT_EQ(slots.slots.size(), retained.value().resources.size());
      for (const auto &slot : slots.slots) {
        EXPECT_TRUE(std::any_of(
            retained.value().resources.begin(),
            retained.value().resources.end(), [&](const auto &resource) {
              return resource.resource_id == slot.resource_id;
            }));
      }
      ASSERT_TRUE(cell.addressed_kernel()
                      .release_retired_engine(removed_engine)
                      .ok());
      EXPECT_EQ(retained_engine->subscription_count(), 1U);
      EXPECT_EQ(removed_engine->subscription_count(), 0U);

      // Re-add only the removed component. The unchanged controller must be
      // the same engine across A -> B -> A, not a rebuilt replacement.
      const auto add_fence_id = core::ConfigurationFenceId(
          remove_arm ? "fence.native-cell.readd-arm"
                     : "fence.native-cell.readd-gripper");
      const core::ConfigurationFenceScopeDTO add_scope{
          {removed.value().controllable_component_id},
          {{removed_path.control_domain_id, std::nullopt,
            removed_path.control_domain_revision}},
          {{removed_path.control_binding_id, std::nullopt,
            removed_path.control_binding_revision}},
          removed_resources};
      ASSERT_TRUE(add_scope.valid());
      admission_gate.HoldFence(add_fence_id, add_scope);
      const auto restored = cell.reconfigure_runtime_component_set(
          {TestRegistration(arm, &arm_engine),
           TestRegistration(gripper, &gripper_engine)},
          resolver, {StopSourceFor(arm), StopSourceFor(gripper)},
          retained.value().retained_bundle_id,
          retained.value().graph_revision, add_fence_id, add_scope,
          {removed.value().controllable_component_id},
          [] { return core::Result<void>::Ok(); });
      ASSERT_TRUE(restored.ok()) << restored.error().message;
      // A newly constructed participant gets the same stream as the retained
      // participant. Either publisher wakes the common reader without a
      // reset, timeout, or new stream incarnation during a mixed Apply.
      const auto added_observation_wake = cell.observation_wake();
      EXPECT_EQ(added_observation_wake, retained_observation_wake);
      auto generation = retained_observation_wake->generation();
      added_observation_wake->notify();
      EXPECT_GT(retained_observation_wake->generation(), generation);
      EXPECT_TRUE(retained_observation_wake->wait_after(
          generation, std::chrono::nanoseconds::zero()));
      generation = added_observation_wake->generation();
      retained_observation_wake->notify();
      EXPECT_GT(added_observation_wake->generation(), generation);
      EXPECT_TRUE(added_observation_wake->wait_after(
          generation, std::chrono::nanoseconds::zero()));
      ASSERT_EQ(cell.participant_registrations().size(), 2U);
      EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(
          retained_engine));
      EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(
          removed_engine));
      EXPECT_EQ(retained_engine->subscription_count(), 1U);
      EXPECT_EQ(removed_engine->subscription_count(), 1U);
      EXPECT_EQ(cell.stop_surveillance_owner()->SnapshotSources().size(), 2U);
    }
    RemoveJournalFiles(journal_path);
  }
}

TEST(RetainedNativeCellRuntimeTest,
     FailedPublicationDoesNotInstallAMixedComponentSetOrRetireAnEngine) {
  const auto arm = qualification_fixture::make_binding();
  const auto gripper = GripperOnlyBinding();
  CellTestExecutionEngine arm_engine;
  CellTestExecutionEngine gripper_engine;
  auto resolver = UnusedResolver();
  CellTestAdmissionGate admission_gate;
  const auto fence_id =
      core::ConfigurationFenceId("fence.native-cell.failed-publication");
  const auto &gripper_path = *gripper.value().control_path;
  std::vector<core::ResourceId> gripper_resources;
  for (const auto &resource : gripper.value().resources) {
    gripper_resources.push_back(resource.resource_id);
  }
  std::sort(gripper_resources.begin(), gripper_resources.end());
  const core::ConfigurationFenceScopeDTO scope{
      {gripper.value().controllable_component_id},
      {{gripper_path.control_domain_id,
        gripper_path.control_domain_revision, std::nullopt}},
      {{gripper_path.control_binding_id,
        gripper_path.control_binding_revision, std::nullopt}},
      gripper_resources};
  ASSERT_TRUE(scope.valid());
  admission_gate.HoldFence(fence_id, scope);

  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.participant_registrations = {
      TestRegistration(arm, &arm_engine),
      TestRegistration(gripper, &gripper_engine)};
  input.kernel_config = {
      core::RequestOwnerEpoch("owner.native-cell.failed-publication"),
      core::CellId("cell.native-cell.failed-publication"), 32U, resolver,
      &admission_gate, arm.value().graph_revision};
  input.stop_sources = {StopSourceFor(arm), StopSourceFor(gripper)};
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    int commit_calls = 0;
    const auto failed = cell.reconfigure_runtime_component_set(
        {TestRegistration(arm, &arm_engine)}, resolver,
        {StopSourceFor(arm)},
        arm.value().retained_bundle_id, arm.value().graph_revision,
        fence_id, scope, {arm.value().controllable_component_id},
        [&commit_calls] {
          ++commit_calls;
          return core::Result<void>::Err(
              {"test_publication_failed", "publication was refused", {}});
        });
    ASSERT_FALSE(failed.ok());
    ASSERT_TRUE(failed.error().details.contains("reason"));
    EXPECT_EQ(failed.error().details.at("reason"),
              "runtime_component_set_publication_commit_failed");
    EXPECT_EQ(commit_calls, 1);
    ASSERT_EQ(cell.participant_registrations().size(), 2U);
    ASSERT_EQ(cell.stop_surveillance_owner()->SnapshotSources().size(), 2U);
    EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(&arm_engine));
    EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(
        &gripper_engine));
    EXPECT_EQ(arm_engine.subscription_count(), 1U);
    EXPECT_EQ(gripper_engine.subscription_count(), 1U);
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     ReplacingIndependentGripperDoesNotReplaceTheArmEngine) {
  const auto arm = qualification_fixture::make_binding();
  const auto old_gripper = GripperOnlyBinding();
  const auto new_gripper = ReplacementGripperBinding(old_gripper);
  CellTestExecutionEngine arm_engine;
  CellTestExecutionEngine old_gripper_engine;
  CellTestExecutionEngine new_gripper_engine;
  auto resolver = UnusedResolver();
  CellTestAdmissionGate admission_gate;
  const auto fence_id =
      core::ConfigurationFenceId("fence.native-cell.replace-gripper");
  const auto &old_path = *old_gripper.value().control_path;
  const auto &new_path = *new_gripper.value().control_path;
  std::vector<core::ResourceId> gripper_resources;
  for (const auto &resource : old_gripper.value().resources) {
    gripper_resources.push_back(resource.resource_id);
  }
  std::sort(gripper_resources.begin(), gripper_resources.end());
  const core::ConfigurationFenceScopeDTO scope{
      {old_gripper.value().controllable_component_id},
      {{old_path.control_domain_id,
        old_path.control_domain_revision,
        new_path.control_domain_revision}},
      {{old_path.control_binding_id,
        old_path.control_binding_revision,
        new_path.control_binding_revision}},
      gripper_resources};
  ASSERT_TRUE(scope.valid());
  admission_gate.HoldFence(fence_id, scope);

  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.participant_registrations = {
      TestRegistration(arm, &arm_engine),
      TestRegistration(old_gripper, &old_gripper_engine)};
  input.kernel_config = {
      core::RequestOwnerEpoch("owner.native-cell.replace-gripper"),
      core::CellId("cell.native-cell.replace-gripper"), 32U, resolver,
      &admission_gate, arm.value().graph_revision};
  input.stop_sources = {StopSourceFor(arm), StopSourceFor(old_gripper)};
  const auto journal_path = input.operation_journal_path;

  {
    RetainedNativeCellRuntime cell(std::move(input));
    int commit_calls = 0;
    const auto changed = cell.reconfigure_runtime_component_set(
        {TestRegistration(arm, &arm_engine),
         TestRegistration(new_gripper, &new_gripper_engine)},
        resolver, {StopSourceFor(arm), StopSourceFor(new_gripper)}, arm.value().retained_bundle_id,
        arm.value().graph_revision, fence_id, scope,
        {arm.value().controllable_component_id}, [&commit_calls] {
          ++commit_calls;
          return core::Result<void>::Ok();
        });
    ASSERT_TRUE(changed.ok()) << changed.error().message;
    EXPECT_EQ(commit_calls, 1);
    ASSERT_EQ(cell.participant_registrations().size(), 2U);
    EXPECT_TRUE(std::any_of(
        cell.participant_registrations().begin(),
        cell.participant_registrations().end(), [&](const auto &registration) {
          return registration.engine == &arm_engine &&
                 registration.binding.value() == arm.value();
        }));
    EXPECT_TRUE(std::any_of(
        cell.participant_registrations().begin(),
        cell.participant_registrations().end(), [&](const auto &registration) {
          return registration.engine == &new_gripper_engine &&
                 registration.binding.value() == new_gripper.value();
        }));
    EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(&arm_engine));
    EXPECT_TRUE(cell.addressed_kernel().participant_engine_in_use(
        &new_gripper_engine));
    EXPECT_FALSE(cell.addressed_kernel().participant_engine_in_use(
        &old_gripper_engine));
    const auto stop_sources =
        cell.stop_surveillance_owner()->SnapshotSources();
    ASSERT_EQ(stop_sources.size(), 2U);
    EXPECT_TRUE(std::any_of(
        stop_sources.begin(), stop_sources.end(), [&](const auto &source) {
          return source.binding.value() == arm.value();
        }));
    EXPECT_TRUE(std::any_of(
        stop_sources.begin(), stop_sources.end(), [&](const auto &source) {
          return source.binding.value() == new_gripper.value();
        }));
    ASSERT_TRUE(cell.addressed_kernel()
                    .release_retired_engine(&old_gripper_engine)
                    .ok());
    EXPECT_EQ(arm_engine.subscription_count(), 1U);
    EXPECT_EQ(old_gripper_engine.subscription_count(), 0U);
    EXPECT_EQ(new_gripper_engine.subscription_count(), 1U);
  }
  RemoveJournalFiles(journal_path);
}

#ifdef MANYFORGE_A1_ROS_GRIPPER_CELL_TEST
TEST(RetainedNativeCellRuntimeTest,
     EmptyGripperOnlyEmptyPreservesStableServicesAndDrainsActualOwner) {
  auto value = GripperOnlyBinding().value();
  // One scalar ROS gripper target projected onto its actual retained joint.
  value.control_path->state_authority_class = core::StateAuthorityClass::kCommanded;
  value.resources.erase(value.resources.begin() + 1, value.resources.end());
  value.model_groups.front().expanded_compiled_joint_names = {
      value.resources.front().compiled_resource_name};
  auto verified = core::verify_runtime_participant_binding(std::move(value));
  ASSERT_TRUE(verified.ok()) << verified.error().message;
  const auto binding = std::move(verified.value());
  auto clock = std::make_shared<WallClock>();
  auto scheduler = std::make_shared<SimpleScheduler>();
  auto input = EmptyCellInput();
  input.clock = clock;
  input.scheduler = scheduler;
  input.operation_journal_path = TemporaryJournalPath();
  CellTestAdmissionGate gate;
  input.kernel_config.operation_admission_gate = &gate;
  const auto journal_path = input.operation_journal_path;
  const auto &resource = binding.value().resources.front();
  const std::string joint = resource.compiled_resource_name;
  const auto scene = std::make_shared<roboplan::Scene>(
      "gripper_cell_fixture", roboplan::loadUrdfSceneDescriptionFromXml(
          "<robot name='gripper_cell_fixture'><link name='base'/><link name='tip'/>"
          "<joint name='" + joint + "' type='prismatic'><parent link='base'/>"
          "<child link='tip'/><axis xyz='0 0 1'/>"
          "<limit lower='0' upper='0.1' effort='5' velocity='1'/></joint></robot>", {}));
  const auto channel_config = retained_ros2_control_gripper_state_config(binding);
  RetainedKinematicModelMapInput map{
      binding.value().retained_bundle_id, binding.value().graph_revision,
      {{binding.value().control_path->control_binding_id,
        binding.value().controllable_component_id, binding.value().command_target_id,
        {{resource.resource_id, {{binding.value().model_groups.front().model_instance_id,
                                 joint, resource.coordinate_kind}}}}}}};
  auto projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create(scene, std::make_shared<std::mutex>(),
                                            std::move(map), {binding},
                                            channel_config.freshness_budget_ns));
  auto channel = std::make_shared<RetainedObservedStateChannel>(
      binding, projector, channel_config, clock.get());
  auto handle = std::make_shared<RetainedRos2ControlRuntimeHandle>();
  RetainedRos2ControlBackendConfig config;
  config.node_name = "manyforge_gripper_cell_fixture";
  config.node_namespace = "/manyforge/gripper_cell_fixture";
  config.executor_thread_count = 1U;
  config.defer_ros_start = true;
  config.measured_receiver_clock = clock;
  config.arm.gripper_controller = RetainedRos2ControlArmController{
      config.node_namespace + "/controller_manager", "gripper_controller", "TestSystem"};
  ros2_control_execution::Ros2GripperActionConfig transport;
  transport.action_name = config.node_namespace + "/gripper_controller/gripper_cmd";
  transport.group_id = binding.value().model_groups.front().compiled_group_name;
  transport.joint_name = joint;
  config.gripper = transport;
  auto backend = make_retained_ros2_control_gripper_backend(
      config, binding, channel, *clock, *scheduler, handle);
  auto gripper = std::make_unique<RetainedNativeGripperRuntime>(
      RetainedNativeGripperRuntimeInput{
          .binding = binding, .backend = backend, .clock = clock,
          .state_channel = channel, .stop_readiness = {}, .rebind_route = {},
          .scheduler = scheduler, .position_limits = {}});
  const auto registration = gripper->registration();
  EXPECT_EQ(registration.engine, gripper->participant_engine());
  EXPECT_EQ(registration.stop_execution_port,
            static_cast<core::IControlDomainExecutionPort *>(gripper->participant_engine()));
  EXPECT_FALSE(handle->participant());
  EXPECT_FALSE(handle->current_port());
  // This transition case does not qualify recovery evidence. The actual
  // source refuses that request rather than manufacturing a no-goal digest.
  const RetainedStopSurveillanceSourceInput source{
      binding, channel, {}, [](std::int64_t) {
        return core::Result<core::ContentDigest>::Err(
            {"recovery_not_qualified", "ownership fixture supplies no recovery evidence", {}});
      }};
  const auto &path = *binding.value().control_path;
  const core::ConfigurationFenceId fence("fence.gripper-cell");
  const core::ConfigurationFenceScopeDTO add_scope{
      {binding.value().controllable_component_id},
      {{path.control_domain_id, std::nullopt, path.control_domain_revision}},
      {{path.control_binding_id, std::nullopt, path.control_binding_revision}},
      {resource.resource_id}};
  const core::ConfigurationFenceScopeDTO remove_scope{
      {binding.value().controllable_component_id},
      {{path.control_domain_id, path.control_domain_revision, std::nullopt}},
      {{path.control_binding_id, path.control_binding_revision, std::nullopt}},
      {resource.resource_id}};
  ASSERT_TRUE(add_scope.valid());
  ASSERT_TRUE(remove_scope.valid());
  const std::weak_ptr<RetainedRos2ControlRuntimeHandle> transport_owner = handle;
  {
    RetainedNativeCellRuntime cell(std::move(input));
    auto *kernel = &cell.addressed_kernel();
    auto *journal = &cell.operation_journal();
    const auto observer = cell.stop_surveillance_owner();
    const auto wake = cell.observation_wake();
    gate.HoldFence(fence, add_scope);
    int commit_calls = 0;
    const auto missing_source = cell.reconfigure_runtime_component_set(
        {registration}, UnusedResolver(), {}, binding.value().retained_bundle_id,
        binding.value().graph_revision, fence, add_scope, {}, [&] {
          ++commit_calls;
          return core::Result<void>::Ok();
        });
    ASSERT_FALSE(missing_source.ok());
    EXPECT_EQ(commit_calls, 0);
    EXPECT_TRUE(cell.participant_registrations().empty());
    EXPECT_TRUE(observer->SnapshotSources().empty());
    EXPECT_FALSE(kernel->participant_engine_in_use(gripper->participant_engine()));
    auto added = cell.reconfigure_runtime_component_set(
        {registration}, UnusedResolver(), {source}, binding.value().retained_bundle_id,
        binding.value().graph_revision, fence, add_scope, {}, [] { return core::Result<void>::Ok(); });
    ASSERT_TRUE(added.ok()) << added.error().message;
    ASSERT_EQ(cell.participant_registrations().size(), 1U);
    EXPECT_EQ(cell.participant_registrations().front().engine, gripper->participant_engine());
    ASSERT_EQ(observer->SnapshotSources().size(), 1U);
    EXPECT_EQ(observer->SnapshotSources().front().state_channel, channel);
    EXPECT_TRUE(kernel->participant_engine_in_use(gripper->participant_engine()));
    gate.HoldFence(fence, remove_scope);
    auto removed = cell.reconfigure_runtime_component_set(
        {}, UnusedResolver(), {}, core::RetainedBundleId("bundle.empty-cell"),
        core::GraphRevision("graph.empty-cell"), fence, remove_scope, {},
        [] { return core::Result<void>::Ok(); });
    ASSERT_TRUE(removed.ok()) << removed.error().message;
    EXPECT_TRUE(cell.participant_registrations().empty());
    EXPECT_TRUE(observer->SnapshotSources().empty());
    EXPECT_FALSE(kernel->participant_engine_in_use(gripper->participant_engine()));
    ASSERT_TRUE(kernel->release_retired_engine(gripper->participant_engine()).ok());
    EXPECT_EQ(&cell.addressed_kernel(), kernel);
    EXPECT_EQ(&cell.operation_journal(), journal);
    EXPECT_EQ(cell.clock(), clock);
    EXPECT_EQ(cell.scheduler(), scheduler);
    EXPECT_EQ(cell.stop_surveillance_owner(), observer);
    EXPECT_EQ(cell.observation_wake(), wake);
    backend.reset();
    handle.reset();
    EXPECT_FALSE(transport_owner.expired());
    gripper.reset();
    EXPECT_TRUE(transport_owner.expired());
  }
  RemoveJournalFiles(journal_path);
}
#endif

TEST(RetainedNativeCellRuntimeTest,
     HostBindingPreservesStableBorrowAcrossRetainedAndReplacedRuntimeRoots) {
  const auto journal_path = TemporaryJournalPath();
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = journal_path;
  input.kernel_config.request_owner_epoch = core::RequestOwnerEpoch("kernel-owner-epoch.h1");
  input.kernel_config.cell_id = core::CellId("cell.h1");
  input.stop_source_mode = RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty;
  {
    RetainedNativeCellRuntime cell(std::move(input));
    auto *const kernel = &cell.addressed_kernel();
    auto *const journal = &cell.operation_journal();
    const auto binding = qualification_fixture::make_binding();
    auto root = std::make_shared<RetainedNativeRuntime>(binding, std::nullopt);
    auto peer = std::make_shared<RetainedNativeRuntime>(
        DistinctCellBinding(binding, "peer"), std::nullopt);
    auto selected_peer = std::make_shared<RetainedNativeRuntime>(
        DistinctCellBinding(binding, "selected-peer"), std::nullopt);
    root->participant_peers = {peer, selected_peer};
    // Existing predecessor association only: BindCellRuntimeOwner does not
    // adopt a route root or emulate the host's separate membership join.
    peer->cell_runtime_root = root.get();
    root->BindCellRuntimeOwner(cell, "epoch.h1", "cell.h1");
    const auto check_borrow = [&](const RetainedNativeRuntime &runtime) {
      EXPECT_EQ(runtime.active_addressed_kernel(), kernel);
      EXPECT_EQ(runtime.active_operation_journal(), journal);
      EXPECT_FALSE(runtime.addressed_kernel);
      EXPECT_FALSE(runtime.operation_journal);
      EXPECT_EQ(runtime.addressed_request_owner_epoch,
                core::RequestOwnerEpoch("kernel-owner-epoch.h1"));
      EXPECT_EQ(runtime.addressed_cell_id, core::CellId("cell.h1"));
    };
    check_borrow(*root);
    check_borrow(*peer);
    check_borrow(*selected_peer);
    auto *const retained_root = root.get();
    root->BindCellRuntimeOwner(cell, "epoch.h1", "cell.h1");
    EXPECT_EQ(root.get(), retained_root);
    check_borrow(*root);
    check_borrow(*peer);

    auto replacement = std::make_shared<RetainedNativeRuntime>(
        DistinctCellBinding(binding, "replacement"), std::nullopt);
    replacement->participant_peers = {peer, selected_peer};
    replacement->BindCellRuntimeOwner(cell, "epoch.h1", "cell.h1");
    // A peer may already name another live root through the separate join.
    // This fixture state is not a handover performed by the binding method.
    selected_peer->cell_runtime_root = replacement.get();
    check_borrow(*replacement);
    EXPECT_EQ(peer->cell_runtime_root, root.get());
    const std::weak_ptr<RetainedNativeRuntime> retired_root = root;
    root.reset();
    EXPECT_TRUE(retired_root.expired());
    EXPECT_EQ(peer->cell_runtime_root, nullptr);
    EXPECT_EQ(selected_peer->cell_runtime_root, replacement.get());
    check_borrow(*peer);
    check_borrow(*selected_peer);
    check_borrow(*replacement);

    const std::weak_ptr<RetainedNativeRuntime> peer_lifetime = peer;
    auto strong_borrow = peer;
    peer.reset();
    replacement.reset();
    EXPECT_FALSE(peer_lifetime.expired());
    EXPECT_EQ(selected_peer->cell_runtime_root, nullptr);
    check_borrow(*selected_peer);
    check_borrow(*strong_borrow);
    EXPECT_EQ(strong_borrow->cell_runtime_root, nullptr);
    ASSERT_EQ(strong_borrow->active_addressed_kernel(), kernel);
    ASSERT_EQ(strong_borrow->active_operation_journal(), journal);
    ASSERT_TRUE(strong_borrow->addressed_request_owner_epoch.has_value());
    EXPECT_TRUE(strong_borrow->active_addressed_kernel()->resource_table_snapshot().slots.empty());
    const auto events = strong_borrow->active_operation_journal()->ReadEvents(
        *strong_borrow->addressed_request_owner_epoch, 0U);
    EXPECT_TRUE(events.ok());
    EXPECT_EQ(&cell.addressed_kernel(), kernel);
    EXPECT_EQ(&cell.operation_journal(), journal);
    strong_borrow.reset();
    EXPECT_TRUE(peer_lifetime.expired());
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     RuntimeRetirementClearsOnlyItsLocallyOwnedKernelFromStrongPeerBorrow) {
  const auto journal_path = TemporaryJournalPath();
  {
    const auto binding = qualification_fixture::make_binding();
    auto root = std::make_shared<RetainedNativeRuntime>(binding, std::nullopt);
    root->clock = std::make_shared<WallClock>();
    root->scheduler = std::make_shared<SimpleScheduler>();
    auto config = EmptyKernelConfig();
    root->operation_journal = std::make_unique<core::SqliteOperationJournal>(
        journal_path, config.cell_id);
    root->addressed_kernel = std::make_unique<core::ResourceAddressedExecutionKernel>(
        std::vector<core::RuntimeParticipantExecutionRegistration>{},
        *root->clock, *root->scheduler, *root->operation_journal, std::move(config));
    root->cell_addressed_kernel = root->addressed_kernel.get();
    root->cell_runtime_root = root.get();
    auto peer = std::make_shared<RetainedNativeRuntime>(
        DistinctCellBinding(binding, "local-peer"), std::nullopt);
    root->participant_peers = {peer};
    peer->cell_addressed_kernel = root->addressed_kernel.get();
    peer->cell_runtime_root = root.get();
    EXPECT_EQ(peer->active_addressed_kernel(), root->addressed_kernel.get());
    const std::weak_ptr<RetainedNativeRuntime> root_lifetime = root;
    const std::weak_ptr<RetainedNativeRuntime> peer_lifetime = peer;
    auto strong_borrow = peer;
    peer.reset();
    root.reset();
    EXPECT_TRUE(root_lifetime.expired());
    EXPECT_FALSE(peer_lifetime.expired());
    EXPECT_EQ(strong_borrow->cell_addressed_kernel, nullptr);
    EXPECT_EQ(strong_borrow->active_addressed_kernel(), nullptr);
    EXPECT_EQ(strong_borrow->cell_runtime_root, nullptr);
    // No retired local journal is borrowed or dereferenced by this case.
    strong_borrow.reset();
    EXPECT_TRUE(peer_lifetime.expired());
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedNativeCellRuntimeTest,
     ProgramResolverPreservesContactPoliciesAndFencesThePhysicalOwner) {
  auto physical_value = qualification_fixture::make_binding().value();
  physical_value.collision_body_bindings = {
      {core::CollisionBodyRoleId("tool_collision"),
       core::ModelInstanceId("robot.model"),
       core::ModelRevision("model-revision"),
       {"tool_body"}}};
  const auto physical_binding =
      core::verify_runtime_participant_binding(std::move(physical_value));
  ASSERT_TRUE(physical_binding.ok()) << physical_binding.error().message;
  auto program_value = physical_binding.value().value();
  program_value.program_id = core::ProgramId("program.with-contact-policy");
  program_value.program_revision =
      core::ProgramRevision("contact-program-revision");
  program_value.program_binding_id =
      core::ProgramBindingId("contact-program-binding");
  program_value.contact_grasp_bindings = {core::ProgramContactGraspBindingDTO{
      core::ContactPolicyId("probe_contact_policy"),
      core::ContactPolicyRevision("contact-policy-revision"),
      core::RuntimeObjectId("trial.probe_object"),
      {{program_value.component_role_id,
        program_value.target_role_id,
        program_value.controllable_component_id,
        program_value.command_target_id,
        core::ModelRoleId("body"),
        core::CollisionBodyRoleId("tool_collision"),
        core::ModelInstanceId("robot.model"),
        core::ModelRevision("model-revision"),
        {core::make_component_collision_body_id(
            program_value.controllable_component_id,
            core::ModelInstanceId("robot.model"),
            core::CollisionBodyRoleId("tool_collision"), 0U)}}},
      core::ContactGraspEnvelopeDTO{0.002, 0.8}}};
  const auto program_binding =
      core::verify_runtime_participant_binding(std::move(program_value));
  ASSERT_TRUE(program_binding.ok()) << program_binding.error().message;
  CellTestExecutionEngine engine;
  const auto physical = TestRegistration(physical_binding.value(), &engine);
  const core::OperationIntentDTO intent{
      {core::RequestId("request.contact-policy"),
       core::RequestOwnerEpoch("owner.resolver"), "test", std::nullopt},
      program_binding.value().value().program_id,
      program_binding.value().value().program_revision,
      program_binding.value().value().program_binding_id,
      core::ProgramRunId("run.resolver"),
      physical_binding.value().value().graph_revision,
      physical_binding.value().value().retained_bundle_id,
      std::nullopt,
      {}};
  const auto resolve = [&](const core::RuntimeParticipantBinding &selected) {
    auto resolver = make_retained_cell_kernel_participant_resolver(
        [selected](const core::OperationIntentDTO &, std::size_t) {
          return core::Result<RetainedResolvedParticipant>::Ok(
              {selected, false});
        },
        {physical});
    return resolver(intent, 0U);
  };
  // The ordinary first node carries the Program's policy catalog even when
  // that node does not select a contact policy. It must keep the same owner.
  const auto resolved = resolve(program_binding.value());
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().engine, &engine);
  EXPECT_EQ(resolved.value().binding.value(), program_binding.value().value());

  auto crossed = program_binding.value().value();
  crossed.resources.front().compiled_resource_name = "another_robot_joint";
  const auto crossed_binding =
      core::verify_runtime_participant_binding(std::move(crossed));
  ASSERT_TRUE(crossed_binding.ok()) << crossed_binding.error().message;
  const auto rejected = resolve(crossed_binding.value());
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "runtime_participant_stop_port_missing");
}

TEST(RetainedNativeCellRuntimeTest,
     ProgramResolverPreservesTheExactPhysicalMovingTakeoverBudget) {
  const auto binding = qualification_fixture::make_binding();
  for (const auto budget :
       {std::optional<double>{}, std::optional<double>{3.5}}) {
    auto physical = TestRegistration(binding, nullptr);
    physical.motion_takeover_preparation_budget_s = budget;
    physical.motion_takeover_planning_budget_s =
        budget ? std::optional<double>(1.25) : std::nullopt;
    auto resolver = make_retained_cell_kernel_participant_resolver(
        [binding](const core::OperationIntentDTO &, std::size_t) {
          return core::Result<RetainedResolvedParticipant>::Ok(
              {binding, false});
        },
        {physical});
    const core::OperationIntentDTO intent{
        {core::RequestId("request.resolver"), core::RequestOwnerEpoch("owner.resolver"),
         "test", std::nullopt},
        binding.value().program_id,
        binding.value().program_revision,
        binding.value().program_binding_id,
        core::ProgramRunId("run.resolver"),
        binding.value().graph_revision,
        binding.value().retained_bundle_id,
        std::nullopt,
        {}};
    const auto resolved = resolver(intent, 0U);
    ASSERT_TRUE(resolved.ok()) << resolved.error().message;
    EXPECT_EQ(resolved.value().motion_takeover_preparation_budget_s, budget);
    EXPECT_EQ(resolved.value().motion_takeover_planning_budget_s,
              physical.motion_takeover_planning_budget_s);
    EXPECT_EQ(resolved.value().binding.value(), binding.value());
  }
}

TEST(
    RetainedNativeCellRuntimeTest,
    BoundMotionCachesAndReadinessKeepTheCurrentCellBudgetAndScalarsStayAbsent) {
  const auto motion = qualification_fixture::make_binding();
  const auto gripper = GripperOnlyBinding();
  CellTestExecutionEngine engine;
  auto motion_row = TestRegistration(motion, &engine);
  motion_row.motion_takeover_preparation_budget_s = 3.5;
  const auto gripper_row = TestRegistration(gripper, &engine);
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  input.kernel_config.graph_revision = motion.value().graph_revision;
  input.kernel_config.participant_resolver = UnusedResolver();
  input.participant_registrations = {motion_row, gripper_row};
  input.stop_sources = {StopSourceFor(motion), StopSourceFor(gripper)};
  const auto path = input.operation_journal_path;
  {
    RetainedNativeCellRuntime cell(std::move(input));
    auto runtime = std::make_shared<RetainedNativeRuntime>(motion, gripper);
    runtime->participant_registrations = {TestRegistration(motion, &engine),
                                          gripper_row};
    runtime->cell_runtime_owner = &cell;
    runtime->BindCellRuntimeOwner(cell, "budget-epoch", "budget-cell");
    EXPECT_EQ(runtime->participant_registrations.front()
                  .motion_takeover_preparation_budget_s,
              std::optional<double>(3.5));
    EXPECT_FALSE(runtime->participant_registrations.back()
                     .motion_takeover_preparation_budget_s);
    ASSERT_TRUE(cell.addressed_kernel().SetRouteRecoveryPending(true).ok());
    const auto published = runtime->PublishComponentCommandReadiness(
        {core::ComponentReadinessAuthorityDTO::kRestartExclusion, std::nullopt,
         std::nullopt, motion.value().controllable_component_id,
         core::ComponentLifecycleGeneration(1U), false});
    ASSERT_TRUE(published.ok()) << published.error().message;
    EXPECT_EQ(runtime->participant_registrations.front()
                  .motion_takeover_preparation_budget_s,
              std::optional<double>(3.5));
    EXPECT_EQ(cell.participant_registrations()
                  .front()
                  .motion_takeover_preparation_budget_s,
              std::optional<double>(3.5));
    EXPECT_FALSE(cell.participant_registrations()
                     .back()
                     .motion_takeover_preparation_budget_s);
  }
  RemoveJournalFiles(path);
}

}  // namespace
}  // namespace manyforge::roboplan_backend
