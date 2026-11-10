#include <gtest/gtest.h>

#include "native_stop_qualification_fixture.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/cell_kinematic_index.hpp>
#include <manyforge_roboplan/retained_cell_runtime.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#ifdef MANYFORGE_ROS_GRIPPER_CELL_TEST
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
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

RetainedCellRuntimeInput EmptyCellInput() {
  return RetainedCellRuntimeInput{
      .clock = {},
      .scheduler = {},
      .operation_journal_path = {},
      .participant_registrations = {},
      .kernel_config = EmptyKernelConfig()};
}

[[nodiscard]] core::RuntimeParticipantBinding DistinctCellBinding(
    const core::RuntimeParticipantBinding &source, const std::string &suffix) {
  auto value = source.value();
  const auto component_name = "robot." + suffix;
  const auto model_name = "robot.model." + suffix;
  value.program_binding_id = core::ProgramBindingId("program-binding." + suffix);
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
    frame.reference.component_id = value.controllable_component_id;
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

TEST(RetainedCellRuntimeTest,
     ThrowingStopControlCallbackIsContainedByTheCellKernelInsteadOfTerminating) {
  auto scheduler = std::make_shared<SimpleScheduler>();
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = scheduler;
  input.operation_journal_path = TemporaryJournalPath();
  const auto journal_path = input.operation_journal_path;

  {
    RetainedCellRuntime cell(std::move(input));
    auto followup = std::make_shared<std::promise<void>>();
    auto followed = followup->get_future();
    testing::internal::CaptureStderr();
    const auto now = cell.clock()->now();
    scheduler->schedule(
        now, [] { throw std::runtime_error("stop stage failed"); },
        core::SchedulerPriority::StopControl);
    scheduler->schedule(
        now + 1'000'000, [followup] { followup->set_value(); },
        core::SchedulerPriority::StopControl);
    const bool lane_continued =
        followed.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const auto output = testing::internal::GetCapturedStderr();
    ASSERT_TRUE(lane_continued);
    // The cell Kernel contained it: the idle cell accepted the internal Full
    // Stop, and the typed cause holds motion admission closed.
    EXPECT_NE(output.find("manyforge_fault code=scheduler_stop_control_callback_exception "
                          "state=contained\n"),
              std::string::npos)
        << output;
    const auto fault = cell.addressed_kernel().get_internal_fault();
    ASSERT_TRUE(fault.has_value());
    EXPECT_EQ(fault->reason_code, "scheduler_stop_control_callback_exception");
    EXPECT_TRUE(fault->stop_accepted);
    EXPECT_EQ(fault->later_count, 0U);
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedCellRuntimeTest,
     ProductResealPreservesExactPhysicalProviderFactsAndStrictAliasOwnership) {
  const auto before = qualification_fixture::make_binding();
  auto value = before.value();
  value.program_id = core::ProgramId("candidate.program");
  value.program_revision = core::ProgramRevision("candidate.program-revision");
  value.program_binding_id =
      core::ProgramBindingId("candidate.program-binding");
  value.retained_bundle_id = core::RetainedBundleId("candidate.bundle");
  value.graph_revision = core::GraphRevision("candidate.graph");
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

// A retained gripper passes the same check as a retained arm
// (BuildRetainedProductComposition): a Deployment Apply re-seals its binding
// into the candidate bundle, and only a changed physical controller or
// capability refuses it.
TEST(RetainedCellRuntimeTest,
     RetainedGripperResealKeepsItsPhysicalOwnerAcrossANewBundle) {
  auto gripper_value = qualification_fixture::make_binding().value();
  gripper_value.controllable_component_id =
      core::ControllableComponentId("gripper.1");
  for (auto &frame : gripper_value.frame_bindings) frame.reference.component_id = gripper_value.controllable_component_id;
  gripper_value.component_type_id = core::ComponentTypeId("aggregate.gripper");
  gripper_value.command_target_id = core::CommandTargetId("gripper.target");
  gripper_value.capability_id = core::CapabilityId("tool.gripper.command");
  gripper_value.model_groups.clear();
  gripper_value.resources = {core::RuntimeResourceBindingDTO{
      core::ResourceRoleId("gripper_joint"), core::ResourceId("gripper.joint"),
      "finger_joint", "finger_joint", core::CoordinateKind::kLinear}};
  gripper_value.control_path->control_domain_id =
      core::ControlDomainId("domain.gripper");
  gripper_value.control_path->control_binding_id =
      core::ControlBindingId("binding.gripper");
  gripper_value.control_path->control_interface_contract_id =
      core::ControlInterfaceContractId("gripper.position");
  gripper_value.control_path->provider_instance_id =
      core::ProviderInstanceId("provider.gripper");
  gripper_value.control_path->state_authority_id =
      core::StateAuthorityId("state.gripper");
  gripper_value.control_path->runtime_scale_qualification = std::nullopt;
  gripper_value.supporting_capability_provider_paths.clear();
  auto gripper = core::verify_runtime_participant_binding(gripper_value);
  ASSERT_TRUE(gripper.ok()) << gripper.error().message;
  const auto before = std::move(gripper.value());

  // Every Deployment mints a new bundle and graph revision.
  auto resealed_value = before.value();
  resealed_value.retained_bundle_id = core::RetainedBundleId("candidate.bundle");
  resealed_value.graph_revision = core::GraphRevision("candidate.graph");
  resealed_value.program_binding_id =
      core::ProgramBindingId("candidate.program-binding");
  auto resealed = core::verify_runtime_participant_binding(resealed_value);
  ASSERT_TRUE(resealed.ok()) << resealed.error().message;
  EXPECT_TRUE(retained_bindings_preserve_physical_owner_after_product_reseal(
      before, resealed.value()));
  // The whole-binding check the gripper branch used before refuses it.
  EXPECT_FALSE(retained_bindings_share_backend_owner(before, resealed.value()));

  const std::vector<
      std::pair<std::string_view,
                std::function<void(core::RuntimeParticipantBindingDTO &)>>>
      changed{{"capability_id",
               [](auto &v) {
                 v.capability_id = core::CapabilityId("tool.gripper.feedback");
               }},
              {"capability_definition_revision",
               [](auto &v) {
                 v.capability_definition_revision =
                     core::CapabilityDefinitionRevision("foreign.revision");
               }},
              {"resources",
               [](auto &v) {
                 v.resources.front().resource_id =
                     core::ResourceId("foreign.joint");
               }},
              {"control_path.control_binding_id",
               [](auto &v) {
                 v.control_path->control_binding_id =
                     core::ControlBindingId("foreign.binding");
               }},
              {"control_path.control_interface_contract_id",
               [](auto &v) {
                 v.control_path->control_interface_contract_id =
                     core::ControlInterfaceContractId("foreign.interface");
               }},
              {"control_path.controller_topology_id",
               [](auto &v) {
                 v.control_path->controller_topology_id =
                     core::ControllerTopologyId("foreign.topology");
               }},
              {"control_path.provider_instance_id",
               [](auto &v) {
                 v.control_path->provider_instance_id =
                     core::ProviderInstanceId("foreign.provider");
               }}};
  for (const auto &[field, change] : changed) {
    SCOPED_TRACE(std::string(field));
    auto foreign_value = resealed.value().value();
    change(foreign_value);
    auto foreign =
        core::verify_runtime_participant_binding(std::move(foreign_value));
    ASSERT_TRUE(foreign.ok()) << foreign.error().message;
    EXPECT_EQ(retained_product_reseal_physical_owner_difference(
                  before, foreign.value()),
              field);
    EXPECT_FALSE(retained_bindings_preserve_physical_owner_after_product_reseal(
        before, foreign.value()));
  }
}

TEST(RetainedCellRuntimeTest,
     RejectsMissingStableTimeOwnersBeforeOpeningTheJournal) {
  auto input = EmptyCellInput();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  EXPECT_THROW(RetainedCellRuntime(std::move(input)),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(journal_path));
  RemoveJournalFiles(journal_path);
}

TEST(RetainedCellRuntimeTest,
     AdoptsPreverifiedReadinessSnapshotWithNoThrowSwaps) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  input.kernel_config = EmptyKernelConfig();
  const auto journal_path = input.operation_journal_path;

  {
    RetainedCellRuntime cell(std::move(input));
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

#ifdef MANYFORGE_ROS_GRIPPER_CELL_TEST
#endif

TEST(RetainedCellRuntimeTest,
     RuntimeRetirementClearsPeerRootAndKeepsTheBorrowedCellKernel) {
  auto input = EmptyCellInput();
  input.clock = std::make_shared<WallClock>();
  input.scheduler = std::make_shared<SimpleScheduler>();
  input.operation_journal_path = TemporaryJournalPath();
  const auto journal_path = input.operation_journal_path;
  {
    RetainedCellRuntime cell(std::move(input));
    const auto binding = qualification_fixture::make_binding();
    auto root = std::make_shared<RetainedExecutionHost>(binding);
    root->clock = cell.clock();
    root->scheduler = cell.scheduler();
    root->cell_runtime_root = root.get();
    auto peer = std::make_shared<RetainedExecutionHost>(
        DistinctCellBinding(binding, "local-peer"));
    root->participant_peers = {peer};
    peer->cell_runtime_root = root.get();
    root->BindCellRuntimeOwner(cell, "owner.empty-cell", "cell.empty-cell");
    EXPECT_EQ(root->cell_addressed_kernel, &cell.addressed_kernel());
    EXPECT_EQ(peer->cell_addressed_kernel, &cell.addressed_kernel());
    EXPECT_EQ(peer->cell_operation_journal, &cell.operation_journal());
    const std::weak_ptr<RetainedExecutionHost> root_lifetime = root;
    const std::weak_ptr<RetainedExecutionHost> peer_lifetime = peer;
    auto strong_borrow = peer;
    peer.reset();
    root.reset();
    EXPECT_TRUE(root_lifetime.expired());
    EXPECT_FALSE(peer_lifetime.expired());
    // The process-stable cell Kernel outlives the retired root.
    EXPECT_EQ(strong_borrow->cell_addressed_kernel, &cell.addressed_kernel());
    EXPECT_EQ(strong_borrow->cell_runtime_root, nullptr);
    strong_borrow.reset();
    EXPECT_TRUE(peer_lifetime.expired());
  }
  RemoveJournalFiles(journal_path);
}

TEST(RetainedCellRuntimeTest,
     ProgramResolverPreservesContactPoliciesAndFencesThePhysicalOwner) {
  auto physical_value = qualification_fixture::make_binding().value();
  physical_value.collision_body_bindings = {
      {{physical_value.controllable_component_id, core::CollisionBodyRoleId("tool_collision")},
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
      {{{program_value.controllable_component_id, core::CollisionBodyRoleId("tool_collision")},
        core::ModelRoleId("body"),
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

TEST(RetainedCellRuntimeTest,
     ProgramResolverAcceptsAProgramViewWithFewerCollisionBodiesThanItsOwner) {
  const auto body = [](const char *role, const char *name) {
    return core::ProgramCollisionBodyBindingDTO{
        {core::ControllableComponentId("robot.1"), core::CollisionBodyRoleId(role)}, core::ModelInstanceId("robot.model"),
        core::ModelRevision("model-revision"), {name}};
  };
  auto physical_value = qualification_fixture::make_binding().value();
  physical_value.collision_body_bindings = {
      body("arm_collision", "arm_body"), body("tool_collision", "tool_body")};
  const auto physical_binding =
      core::verify_runtime_participant_binding(std::move(physical_value));
  ASSERT_TRUE(physical_binding.ok()) << physical_binding.error().message;
  auto program_value = physical_binding.value().value();
  program_value.program_id = core::ProgramId("program.arm-only");
  program_value.program_revision = core::ProgramRevision("arm-only-revision");
  program_value.program_binding_id =
      core::ProgramBindingId("arm-only-program-binding");
  program_value.collision_body_bindings = {body("arm_collision", "arm_body")};
  const auto program_binding =
      core::verify_runtime_participant_binding(std::move(program_value));
  ASSERT_TRUE(program_binding.ok()) << program_binding.error().message;
  CellTestExecutionEngine engine;
  const auto physical = TestRegistration(physical_binding.value(), &engine);
  const core::OperationIntentDTO intent{
      {core::RequestId("request.arm-only"),
       core::RequestOwnerEpoch("owner.resolver"), "test", std::nullopt},
      program_binding.value().value().program_id,
      program_binding.value().value().program_revision,
      program_binding.value().value().program_binding_id,
      core::ProgramRunId("run.arm-only"),
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
  const auto resolved = resolve(program_binding.value());
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().engine, &engine);
  EXPECT_EQ(resolved.value().binding.value(), program_binding.value().value());

  // A body the owner does not declare widens the view: no owner matches.
  auto widened = program_binding.value().value();
  widened.collision_body_bindings = {body("arm_collision", "another_body")};
  const auto widened_binding =
      core::verify_runtime_participant_binding(std::move(widened));
  ASSERT_TRUE(widened_binding.ok()) << widened_binding.error().message;
  const auto rejected = resolve(widened_binding.value());
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "runtime_participant_stop_port_missing");
}

// mock_lite6_pick_and_place (merge 6 tool split): the robot's direct-control
// binding holds every TCP of its arm target (`base`, `flange` and the mounted
// tool's default `tcp`); the Program names only `base` and `tcp`. Its first
// arm move failed with runtime_participant_stop_port_missing.
TEST(RetainedCellRuntimeTest,
     ProgramResolverAcceptsAProgramViewWithFewerFramesThanItsOwner) {
  const auto program_value = qualification_fixture::make_binding().value();
  auto physical_value = program_value;
  physical_value.frame_bindings.insert(
      std::next(physical_value.frame_bindings.begin()),
      core::ProgramFrameBindingDTO{
          {core::ControllableComponentId("robot.1"), core::FrameRoleId("flange")}, core::ModelInstanceId("robot.model"),
          core::ModelRevision("model-revision"), "link_eef"});
  const auto physical_binding =
      core::verify_runtime_participant_binding(std::move(physical_value));
  ASSERT_TRUE(physical_binding.ok()) << physical_binding.error().message;
  const auto program_binding =
      core::verify_runtime_participant_binding(core::RuntimeParticipantBindingDTO(program_value));
  ASSERT_TRUE(program_binding.ok()) << program_binding.error().message;
  CellTestExecutionEngine engine;
  const auto physical = TestRegistration(physical_binding.value(), &engine);
  const core::OperationIntentDTO intent{
      {core::RequestId("request.fewer-frames"),
       core::RequestOwnerEpoch("owner.resolver"), "test", std::nullopt},
      program_value.program_id,
      program_value.program_revision,
      program_value.program_binding_id,
      core::ProgramRunId("run.fewer-frames"),
      program_value.graph_revision,
      program_value.retained_bundle_id,
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
  const auto resolved = resolve(program_binding.value());
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_EQ(resolved.value().engine, &engine);
  EXPECT_EQ(resolved.value().binding.value(), program_binding.value().value());

  // A frame the owner does not hold still finds no owner.
  auto foreign = program_value;
  foreign.frame_bindings.back().compiled_frame_name = "another_tcp";
  const auto foreign_binding =
      core::verify_runtime_participant_binding(std::move(foreign));
  ASSERT_TRUE(foreign_binding.ok()) << foreign_binding.error().message;
  const auto rejected = resolve(foreign_binding.value());
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "runtime_participant_stop_port_missing");
}

// mock_lite6_pick_and_place (merge 6 tool split): the robot's Program `tcp` is the
// mounted tool's frame, whose retained frame path keeps the tool's component.
// A relative move from `tcp` (pick_retreat) failed with
// program_reference_frame_path_missing.
TEST(RetainedCellRuntimeTest,
     ProgramBindingFrameOfAMountedToolKeepsTheToolComponent) {
  const auto frame = [](const char *component, const char *model,
                        const char *role, const char *name) {
    return core::ResolvedComponentModelFrameDTO{
        core::ControllableComponentId(component), core::ModelInstanceId(model),
        core::FrameRoleId(role), name, core::ModelRevision(std::string(model) + "-rev")};
  };
  const std::vector<core::KinematicFramePathDTO> paths{
      {core::ResolvedCellFrameDTO{core::CellId("cell"), "cell_frame"}, {}},
      {frame("lite6_robot", "lite6_robot.model", "base", "link_base"), {}},
      {frame("lite_gripper", "lite_gripper.model", "tcp", "link_tcp"), {}}};
  const auto binding = [](const char *model, const char *role, const char *name) {
    return core::ProgramFrameBindingDTO{
        {core::ControllableComponentId(std::string(model) == "lite_gripper.model" ? "lite_gripper" : "lite6_robot"), core::FrameRoleId(role)}, core::ModelInstanceId(model),
        core::ModelRevision(std::string(model) + "-rev"), name};
  };
  const core::ControllableComponentId robot("lite6_robot");
  // The explicit frame reference carries its own component owner.
  const core::ResolvedFrameDTO tool_tcp = program_binding_frame(
      binding("lite_gripper.model", "tcp", "link_tcp"));
  EXPECT_EQ(tool_tcp, core::ResolvedFrameDTO(paths[2].terminal_frame));
  const core::ResolvedFrameDTO base = program_binding_frame(
      binding("lite6_robot.model", "base", "link_base"));
  EXPECT_EQ(base, core::ResolvedFrameDTO(paths[1].terminal_frame));
  // A frame reference keeps its owner even without a retained path.
  EXPECT_EQ(program_binding_frame(binding("other.model", "tcp", "tool0"))
                .controllable_component_id,
            robot);
}

// Direct frame references retain their component owner independently of model and target names.
namespace path_scan_oracle {
core::ResolvedComponentModelFrameDTO program_binding_frame(
    const std::vector<core::KinematicFramePathDTO> &frame_paths,
    const core::ControllableComponentId &binding_component,
    const core::ProgramFrameBindingDTO &frame) {
  static_cast<void>(frame_paths);
  static_cast<void>(binding_component);
  return core::ResolvedComponentModelFrameDTO{
      frame.reference.component_id, frame.model_instance_id, frame.reference.frame_id,
      frame.compiled_frame_name, frame.model_revision};
}
} // namespace path_scan_oracle

TEST(RetainedCellRuntimeTest,
     ProgramBindingFramePreservesDirectComponentOwner) {
  const auto component = [](const char *id, std::vector<const char *> models) {
    core::ControllableComponentDTO row{core::ControllableComponentId(id),
                                       core::ComponentTypeId(std::string(id) + "_type"),
                                       core::ComponentTypeRevision("1"),
                                       {},
                                       {},
                                       std::nullopt};
    for (const auto *model : models)
      row.model_role_bindings.push_back(
          {core::ModelRoleId("model"), core::ModelInstanceId(model)});
    return row;
  };
  const auto mount = [](const char *id, const char *parent, const char *child) {
    return core::ComponentAttachmentEdgeDTO{
        core::ComponentAttachmentId(id), core::ComponentAttachmentRevision("1"),
        {core::ControllableComponentId(parent), core::AttachmentInterfaceId("flange")},
        {core::ControllableComponentId(child), core::AttachmentInterfaceId("mount")},
        core::PlacementPoseDTO{}};
  };
  // `arm.tool` carries `arm.tool.nozzle`; `ur5` has no retained path;
  // `io_box` owns no model; `fixture.model` is passive.
  const core::CellGraphDTO graph{
      "manyforge.cell_graph.v1",
      core::GraphRevision("graph-1"),
      core::CellId("cell"),
      {},
      {},
      {component("arm", {"arm.model"}),
       component("arm.tool", {"arm.tool.model"}),
       component("arm.tool.nozzle", {"arm.tool.nozzle.model"}),
       component("io_box", {}),
       component("ur5", {"ur5.model"})},
      {},
      {},
      {mount("z_mount", "arm", "arm.tool"),
       mount("a_mount", "arm.tool", "arm.tool.nozzle")}};
  const auto frame = [](const char *component, const char *model,
                        const char *role, const char *name) {
    return core::ResolvedComponentModelFrameDTO{
        core::ControllableComponentId(component), core::ModelInstanceId(model),
        core::FrameRoleId(role), name, core::ModelRevision(std::string(model) + "-rev")};
  };
  const std::vector<core::KinematicFramePathDTO> paths{
      {core::ResolvedCellFrameDTO{core::CellId("cell"), "cell_frame"}, {}},
      {core::ResolvedPassiveModelFrameDTO{core::ModelInstanceId("fixture.model"),
                                          core::FrameRoleId("top"), "fixture_top",
                                          core::ModelRevision("fixture.model-rev")},
       {}},
      {frame("arm", "arm.model", "base", "link_base"), {}},
      {frame("arm", "arm.model", "flange", "link_eef"), {}},
      {frame("arm.tool", "arm.tool.model", "tcp", "link_tcp"), {}},
      {frame("arm.tool.nozzle", "arm.tool.nozzle.model", "tip", "nozzle_tip"), {}}};
  const deployment::CellKinematicIndex index(graph, paths);

  const std::vector<const char *> models{
      "arm.model", "arm.tool.model", "arm.tool.nozzle.model", "ur5.model",
      "fixture.model", "absent.model"};
  const std::vector<const char *> components{"arm", "arm.tool", "arm.tool.nozzle",
                                             "io_box", "ur5"};
  for (const auto *binding_component : components) {
    for (const auto *model : models) {
      const core::ProgramFrameBindingDTO binding{
          {core::ControllableComponentId(binding_component), core::FrameRoleId("tcp")}, core::ModelInstanceId(model),
          core::ModelRevision(std::string(model) + "-rev"), "link_tcp"};
      const core::ControllableComponentId bound(binding_component);
      EXPECT_EQ(program_binding_frame(binding),
                path_scan_oracle::program_binding_frame(paths, bound, binding))
          << binding_component << " / " << model;
    }
  }
  // Graph owner == the path's owner stamp, for every component-model path.
  for (const auto &path : paths) {
    if (const auto *model = std::get_if<core::ResolvedComponentModelFrameDTO>(
            &path.terminal_frame)) {
      const auto *owner = index.owner_of(model->model_instance_id);
      ASSERT_NE(owner, nullptr);
      EXPECT_EQ(*owner, model->controllable_component_id);
    }
  }
}

// A component without its own live state source is local when
// anything it is mounted on is local: lite6 + a pneumatic gripper (no
// coordinate publication) resolves the gripper's own frames and the objects it
// carries (before: program_reference_robot_not_local). A tool on a robot
// hosted elsewhere, and that robot, stay refused.
TEST(RetainedCellRuntimeTest,
     ComponentWithoutItsOwnStateSourceIsLocalWhenItsMountIsLocal) {
  const auto component = [](const char *id) {
    return core::ControllableComponentDTO{core::ControllableComponentId(id),
                                          core::ComponentTypeId("type"),
                                          core::ComponentTypeRevision("1"),
                                          {},
                                          {},
                                          std::nullopt};
  };
  const auto mount = [](const char *id, const char *parent, const char *child) {
    return core::ComponentAttachmentEdgeDTO{
        core::ComponentAttachmentId(id), core::ComponentAttachmentRevision("1"),
        core::ComponentAttachmentEndpointDTO{core::ControllableComponentId(parent),
                                             core::AttachmentInterfaceId("flange")},
        core::ComponentAttachmentEndpointDTO{core::ControllableComponentId(child),
                                             core::AttachmentInterfaceId("mount")},
        core::PlacementPoseDTO{}};
  };
  const core::CellGraphDTO graph{
      "manyforge.cell_graph.v1",
      core::GraphRevision("graph-1"),
      core::CellId("cell"),
      {},
      {},
      {component("lite6_robot"), component("lite_gripper"), component("gripper_io"),
       component("ur5_robot"), component("ur5_gripper")},
      {},
      {},
      {mount("lite6_flange", "lite6_robot", "lite_gripper"),
       mount("ur5_flange", "ur5_robot", "ur5_gripper")}};
  const deployment::CellKinematicIndex index(graph);
  const auto mounts = retained_scene_frame_component_mounts(graph, index);
  // Only lite6_robot has a live state source in this resolver.
  const auto has_source = [](const core::ControllableComponentId &id) {
    return id.value() == "lite6_robot";
  };
  const auto local = [&](const char *id) {
    return retained_scene_frame_component_local(
        mounts, core::ControllableComponentId(id), has_source);
  };
  EXPECT_TRUE(local("lite6_robot"));
  EXPECT_TRUE(local("lite_gripper"));
  EXPECT_FALSE(local("gripper_io"));
  EXPECT_FALSE(local("ur5_robot"));
  EXPECT_FALSE(local("ur5_gripper"));
  // A tool with its own source is local by it, mounted or not.
  const auto tool_has_source = [](const core::ControllableComponentId &id) {
    return id.value() == "ur5_gripper";
  };
  EXPECT_TRUE(retained_scene_frame_component_local(
      mounts, core::ControllableComponentId("ur5_gripper"), tool_has_source));
  EXPECT_FALSE(retained_scene_frame_component_local(
      mounts, core::ControllableComponentId("ur5_robot"), tool_has_source));
}

TEST(RetainedCellRuntimeTest,
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

}  // namespace
}  // namespace manyforge::roboplan_backend
