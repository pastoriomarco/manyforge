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
#include <vector>

#include <manyforge_core/stop_fault_recovery.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_roboplan/retained_native_cell_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

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
                   {}, core::ContentDigest(StopDigest('2')), {}, {}, 1'000,
                   100, 900, 10, {}},
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
        {StopSourceFor(retained_binding)}, std::nullopt,
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
        {StopSourceFor(retained_binding)}, std::nullopt,
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
          {StopSourceFor(retained)}, std::nullopt,
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
          resolver, {StopSourceFor(arm), StopSourceFor(gripper)}, std::nullopt,
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
        {StopSourceFor(arm)}, std::nullopt,
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
        resolver, {StopSourceFor(arm), StopSourceFor(new_gripper)},
        std::nullopt, arm.value().retained_bundle_id,
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

}  // namespace
}  // namespace manyforge::roboplan_backend
