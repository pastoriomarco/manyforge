// Kernel-to-ros2_control Stop integration.
//
// Joins the real ResourceAddressedExecutionKernel (with its real SQLite
// operation journal and real StopControl/stage-attempt machinery) to a real
// Ros2ControlExecutionBackend whose FollowJointTrajectory goal is held active
// by an in-process ROS action server. The only test-owned seam is the thin
// prepared-motion engine below: it replaces planning/validation (it seals a
// fixed trajectory into the Kernel-minted prepared participant) and then calls
// the real backend Prepare/Dispatch. The Kernel's Stop-stage port is the real
// backend, observed through a forwarding recorder.
#include "ros2_control_stop_qualification_fixture.hpp"

#include <gtest/gtest.h>

#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/prepared_execution.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using namespace std::chrono_literals;
namespace fixture = manyforge::ros2_control_execution::qualification_fixture;
using manyforge::ros2_control_execution::Ros2ControlExecutionBackend;

constexpr std::uint64_t kExecutionId = 1U;
constexpr const char *kOwnerEpoch = "kernel.ros2_control.stop.1";
constexpr const char *kCellId = "cell.ros2_control.stop";

[[nodiscard]] std::string Sha(const char value) {
  return std::string("sha256:") + std::string(64U, value);
}

template <typename Predicate>
[[nodiscard]] bool WaitUntil(Predicate predicate,
                             std::chrono::milliseconds timeout = 5s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

// The ROS fixture binding, completed with the content-addressed revisions a
// resolved Stop plan requires and the base frame / joint-goal planner path
// that ordinary motion admission requires.
[[nodiscard]] core::RuntimeParticipantBinding KernelBinding() {
  auto value = fixture::make_binding().value();
  value.program_revision = core::ProgramRevision(Sha('0'));
  value.program_binding_id = core::ProgramBindingId(Sha('1'));
  value.retained_bundle_id = core::RetainedBundleId(Sha('6'));
  value.graph_revision = core::GraphRevision(Sha('7'));
  value.frame_bindings.insert(
      value.frame_bindings.begin(),
      core::ProgramFrameRoleBindingDTO{
          core::FrameRoleId("base"), core::ModelInstanceId("robot.model"),
          core::ModelRevision("model-revision"), "base"});
  auto &path = *value.control_path;
  path.control_domain_revision = core::ControlDomainRevision(Sha('8'));
  path.control_binding_revision = core::ControlBindingRevision(Sha('9'));
  path.topology_revision = core::TopologyRevision(Sha('a'));
  path.provider_manifest_revision = core::ProviderManifestRevision(Sha('b'));
  path.stop_evidence_policy_revision =
      core::StopEvidencePolicyRevision(Sha('c'));
  value.supporting_capability_provider_paths = {
      core::RuntimeCapabilityProviderPathDTO{
          core::CapabilityId("planning.motion.joint_goal"),
          core::CapabilityDefinitionRevision(
              "sha256:"
              "0f6cb2fda7e1d05f20a5867203b84c0e0427440f4b11657941a907c2e2a23dce"),
          core::CapabilityProviderBindingId("planner-binding"),
          core::CapabilityProviderBindingRevision("planner-binding-revision"),
          core::ProviderInstanceId("planner-provider"),
          core::ProviderManifestId("planner-provider-manifest"),
          core::ProviderManifestRevision(Sha('e')),
          path.provider_runtime_unit_id, path.provider_runtime_epoch}};
  auto sealed = core::verify_runtime_participant_binding(std::move(value));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  return std::move(sealed.value());
}

// Same single-binding domain shape as the retained native cell runtime test.
[[nodiscard]] core::ControllerLifecycleExpectedDomain
ExpectedDomain(const core::RuntimeParticipantBinding &binding) {
  const auto &path = *binding.value().control_path;
  const core::ControllerRoleId controller("controller.arm");
  std::vector<core::ControllerInterfaceInstanceDTO> interfaces;
  for (const auto &resource : binding.value().resources) {
    interfaces.push_back({resource.resource_id, "position"});
  }
  core::ControllerLifecycleExpectedDomainDTO expected{
      path.control_domain_id,
      path.control_domain_revision,
      path.controller_topology_id,
      path.topology_revision,
      core::ProductId("product.control"),
      core::ProductContractId("control.contract"),
      core::ProductContractRevision(Sha('1')),
      core::ContentDigest(Sha('2')),
      core::ContentDigest(Sha('3')),
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
       {{path.control_binding_id, path.control_binding_revision,
         path.control_route_id}}},
      {{path.control_binding_id, path.control_binding_revision,
        path.control_interface_contract_id, path.control_interface_revision,
        interfaces, interfaces}},
      {{controller,
        core::ControllerImplementationId("controller.ros2_control"),
        core::ControllerImplementationRevision(Sha('4')),
        "controller_interface.ControllerInterface",
        core::ContentDigest(Sha('5')),
        interfaces,
        interfaces,
        {},
        {}}},
      {{path.control_binding_id, path.control_binding_revision,
        path.control_route_id, core::EndpointRoleId("command.stop"),
        "command.stop", true, false}}};
  auto domain =
      core::verify_controller_lifecycle_expected_domain(std::move(expected));
  if (!domain.ok()) {
    throw std::logic_error(domain.error().message);
  }
  return std::move(domain.value());
}

// The codec golden plan rebound to this binding. Its nominal stage becomes
// the exact controller-goal cancel the ros2_control Stop port realizes
// (FollowJointTrajectory cancel + goal-terminal evidence); the retained
// software-hard stage remains the escalation floor.
[[nodiscard]] core::RuntimeParticipantStopReadiness
StopReadiness(const core::RuntimeParticipantBinding &binding,
              const core::ControllerLifecycleExpectedDomain &domain) {
  const auto golden =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
      "manyforge_core/test/golden/resolved_stop_base_plan.v1.json";
  std::ifstream stream(golden);
  std::string bytes{std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()};
  if (bytes.ends_with('\n')) {
    bytes.pop_back();
  }
  auto decoded = core::DecodeResolvedStopBasePlan(bytes);
  if (!decoded.ok()) {
    throw std::logic_error(decoded.error().message);
  }
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
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
  }
  std::sort(resources.begin(), resources.end());
  for (auto &stage : plan.escalation_stages) {
    rebind_target(stage.command_target);
    stage.containment_granularity = core::ContainmentGranularity::kResource;
    stage.containment_scope = core::ResolvedStopResourceContainmentScopeDTO{
        path.activation_set_id, resources};
    for (auto &evidence : stage.evidence_requirements) {
      rebind_target(evidence.resolved_source.runtime_target);
      evidence.resolved_source.state_authority_id = path.state_authority_id;
      evidence.resolved_source.state_authority_class =
          path.state_authority_class;
      evidence.requirement.source_requirement.allowed_authority_classes = {
          path.state_authority_class};
    }
  }

  auto &nominal = plan.escalation_stages.front();
  nominal.containment_command.command_kind =
      core::StopContainmentCommandKind::kControllerGoalCancel;
  nominal.containment_command.provider_contract_ref.reset();
  nominal.containment_command.registered_command_kind_id.reset();
  nominal.default_contract_parameters.reset();
  nominal.command_qualification.controlled_substitution_contract.reset();
  auto &goal_terminal = nominal.evidence_requirements.front();
  goal_terminal.requirement.evidence_item_id =
      core::StopEvidenceItemId("evidence.nominal.goal_terminal");
  goal_terminal.requirement.evidence_kind =
      core::StopEvidenceRequirementKind::kControllerGoalTerminal;
  goal_terminal.requirement.provider_contract_ref.reset();
  goal_terminal.requirement.registered_evidence_kind_id.reset();
  goal_terminal.requirement.source_requirement.source_kind =
      core::StopEvidenceSourceKind::kControllerGoalStatus;
  goal_terminal.requirement.source_requirement.endpoint_role_id.reset();
  goal_terminal.requirement.source_requirement.allowed_authority_classes
      .clear();
  goal_terminal.requirement.observation_semantics.observation_kind =
      core::StopEvidenceObservationKind::kPostCommandSample;
  goal_terminal.requirement.observation_semantics.minimum_dwell_ns.reset();
  goal_terminal.resolved_source.source_kind =
      core::StopEvidenceSourceKind::kControllerGoalStatus;
  goal_terminal.resolved_source.state_authority_id.reset();
  goal_terminal.resolved_source.state_authority_class.reset();
  goal_terminal.resolved_source.provider_contract_ref.reset();

  auto sealed = core::MakeResolvedStopBasePlan(std::move(plan));
  if (!sealed.ok()) {
    throw std::logic_error(sealed.error().message);
  }
  auto readiness = core::verify_runtime_participant_stop_readiness(
      {value.controllable_component_id, core::ComponentLifecycleGeneration(1U),
       domain.value(), std::move(sealed.value())},
      binding, domain);
  if (!readiness.ok()) {
    throw std::logic_error(readiness.error().message);
  }
  return std::move(readiness.value());
}

// Forwards every Kernel Stop stage to the real ros2_control backend port and
// records what the backend returned.
class RecordingRos2ControlStopPort final
    : public core::IControlDomainExecutionPort {
public:
  explicit RecordingRos2ControlStopPort(Ros2ControlExecutionBackend &backend)
      : backend_(backend) {}

  [[nodiscard]] core::ParticipantStopStageResultDTO
  TranslateStopStage(const core::ResolvedParticipantStopStageDTO &stage,
                     const core::RemainingStopBudgetDTO &budget,
                     std::stop_token cancellation) override {
    auto result = backend_.TranslateStopStage(stage, budget, cancellation);
    std::lock_guard lock(mutex_);
    results_.push_back(result);
    return result;
  }

  [[nodiscard]] std::vector<core::ParticipantStopStageResultDTO>
  results() const {
    std::lock_guard lock(mutex_);
    return results_;
  }

private:
  Ros2ControlExecutionBackend &backend_;
  mutable std::mutex mutex_;
  std::vector<core::ParticipantStopStageResultDTO> results_;
};

// Test replacement for the planner/validator-backed ExecutionKernel only. The
// prepared product is sealed with the Kernel's authority exactly as the
// retained CCLC admission fake does; Prepare and Dispatch are the real
// ros2_control backend.
class Ros2ControlPreparedMotionEngine final
    : public core::IResolvedParticipantExecutionEngine,
      public core::IResolvedPreparedParticipantExecutionEngine {
public:
  explicit Ros2ControlPreparedMotionEngine(
      Ros2ControlExecutionBackend &backend)
      : backend_(backend) {}

  [[nodiscard]] core::Result<std::uint64_t> PrepareMotion(
      core::ResolvedPreparedMotionExecutionRequestDTO request) override {
    auto participant = MakePreparedMotionParticipant(request);
    if (!participant.ok()) {
      return core::Result<std::uint64_t>::Err(participant.error());
    }
    auto retained =
        std::make_shared<const core::PreparedOperationParticipantDTO>(
            std::move(participant.value()));
    auto backend_prepared = backend_.Prepare(*retained, kExecutionId);
    if (backend_prepared.state !=
        core::PrepareParticipantResultStateDTO::kPrepared) {
      return core::Result<std::uint64_t>::Err(backend_prepared.error.value_or(
          core::ErrorDTO{"ros2_control_prepare_rejected",
                         "ros2_control backend rejected the prepared motion",
                         {}}));
    }
    {
      std::lock_guard lock(mutex_);
      request_ = std::move(request);
      snapshot_ = core::ResolvedPreparedMotionExecutionSnapshotDTO{
          kExecutionId, core::ResolvedPreparedMotionExecutionStateDTO::kPrepared,
          retained, std::move(backend_prepared), std::nullopt};
    }
    Deliver(core::KernelEvent{core::KernelEventType::kParticipantPrepared,
                              kExecutionId,
                              0,
                              {},
                              {},
                              std::nullopt,
                              {{"prepared_contract_digest",
                                retained->prepared_contract_digest.value()}}});
    return core::Result<std::uint64_t>::Ok(kExecutionId);
  }

  [[nodiscard]] core::Result<core::ResolvedPreparedMotionExecutionSnapshotDTO>
  ObservePreparedMotion(std::uint64_t execution_id) const override {
    std::lock_guard lock(mutex_);
    if (!snapshot_.has_value() || snapshot_->execution_id != execution_id) {
      return core::Result<core::ResolvedPreparedMotionExecutionSnapshotDTO>::
          Err({"prepared_motion_missing",
               "ros2_control prepared motion is unavailable",
               {{"reason", "prepared_motion_missing"}}});
    }
    return core::Result<core::ResolvedPreparedMotionExecutionSnapshotDTO>::Ok(
        *snapshot_);
  }

  [[nodiscard]] core::Result<void> DispatchPreparedMotion(
      std::uint64_t execution_id,
      const core::ContentDigest &prepared_contract_digest) override {
    std::shared_ptr<const core::PreparedOperationParticipantDTO> participant;
    std::optional<core::ControllerGoalId> goal_id;
    std::optional<core::ControllerGoalGeneration> goal_generation;
    {
      std::lock_guard lock(mutex_);
      if (!request_.has_value() || !snapshot_.has_value() ||
          !snapshot_->participant || snapshot_->execution_id != execution_id ||
          snapshot_->participant->prepared_contract_digest !=
              prepared_contract_digest) {
        return core::Result<void>::Err(
            {"prepared_motion_dispatch_mismatch",
             "ros2_control prepared dispatch identity differs",
             {{"reason", "prepared_dispatch_mismatch"}}});
      }
      participant = snapshot_->participant;
      goal_id = request_->execution_request.binding.controller_goal_id;
      goal_generation =
          request_->execution_request.binding.controller_goal_generation;
    }
    auto handle = backend_.Dispatch(*participant, *goal_id, *goal_generation);
    if (const auto invalid = core::prepared_execution_handle_field_error(
            handle, *participant, *goal_id, *goal_generation);
        invalid.has_value()) {
      return core::Result<void>::Err(
          {"ros2_control_dispatch_rejected",
           "ros2_control backend rejected the prepared dispatch",
           {{"reason", std::string(*invalid)}}});
    }
    const auto dispatched = handle.get_snapshot();
    if (dispatched.state == core::ExecutionState::kFailed) {
      return core::Result<void>::Err(dispatched.error.value_or(
          core::ErrorDTO{"ros2_control_dispatch_failed",
                         "ros2_control prepared dispatch failed",
                         {}}));
    }
    {
      std::lock_guard lock(mutex_);
      snapshot_->state =
          core::ResolvedPreparedMotionExecutionStateDTO::kDispatched;
      snapshot_->last_dispatched_participant = participant;
      status_.state = core::ExecutionState::kPlanning;
      status_.execution_id = kExecutionId;
    }
    dispatched_.store(true, std::memory_order_release);
    return core::Result<void>::Ok();
  }

  [[nodiscard]] core::Result<void>
  CancelPreparedMotion(std::uint64_t execution_id) override {
    std::lock_guard lock(mutex_);
    if (!snapshot_.has_value() || snapshot_->execution_id != execution_id) {
      return core::Result<void>::Err(
          {"prepared_motion_missing",
           "ros2_control prepared motion is unavailable",
           {{"reason", "prepared_motion_missing"}}});
    }
    if (snapshot_->state ==
        core::ResolvedPreparedMotionExecutionStateDTO::kDispatched) {
      return core::Result<void>::Err(
          {std::string(core::to_string(
               core::ErrorCode::kInvalidStateTransition)),
           "ros2_control prepared motion already dispatched",
           {{"reason", "prepared_motion_already_dispatched"}}});
    }
    snapshot_->state =
        core::ResolvedPreparedMotionExecutionStateDTO::kCancelledPreDispatch;
    return core::Result<void>::Ok();
  }

  [[nodiscard]] core::Result<std::uint64_t> PrepareCapabilityCommand(
      core::ResolvedPreparedCapabilityCommandExecutionRequestDTO) override {
    return Unsupported<std::uint64_t>();
  }
  [[nodiscard]]
  core::Result<core::ResolvedPreparedCapabilityCommandSnapshotDTO>
  ObservePreparedCapabilityCommand(std::uint64_t) const override {
    return Unsupported<core::ResolvedPreparedCapabilityCommandSnapshotDTO>();
  }
  [[nodiscard]] core::Result<core::GripperCommandResultDTO>
  DispatchPreparedCapabilityCommand(std::uint64_t, const core::ContentDigest &,
                                    const core::ControllerGoalId &,
                                    core::ControllerGoalGeneration) override {
    return Unsupported<core::GripperCommandResultDTO>();
  }
  [[nodiscard]] core::Result<void>
  CancelPreparedCapabilityCommand(std::uint64_t) override {
    return Unsupported<void>();
  }

  core::AddressedParticipantStopFenceDisposition
  fence_addressed_participant_stop(std::uint64_t execution_id) noexcept override {
    fence_calls_.fetch_add(1U, std::memory_order_relaxed);
    return execution_id == kExecutionId &&
                   dispatched_.load(std::memory_order_acquire)
               ? core::AddressedParticipantStopFenceDisposition::
                     kDispatchCommitted
               : core::AddressedParticipantStopFenceDisposition::kUnknown;
  }

  // Same exact-identity settlement contract as the Kernel tests' fake engine.
  [[nodiscard]] core::Result<core::AddressedParticipantStopSettlementDTO>
  settle_addressed_participant_stop(
      const core::AddressedParticipantStopSettlementRequestDTO &request)
      override {
    const auto &certificate = request.aggregate_stop_certificate;
    const auto &stage = request.containment.request;
    const bool expected_disposition =
        request.effect_prevented_before_dispatch
            ? request.containment.attempt_disposition ==
                  core::StopStageAttemptDispositionDTO::kNotDispatched
            : request.containment.attempt_disposition !=
                  core::StopStageAttemptDispositionDTO::kNotDispatched;
    if (request.execution_id != kExecutionId || !certificate.locally_valid() ||
        request.containment.outcome !=
            core::ParticipantStopStageOutcomeDTO::kSucceeded ||
        !expected_disposition || stage.stop_id != certificate.stop_id ||
        stage.stop_generation != certificate.stop_generation) {
      return core::Result<core::AddressedParticipantStopSettlementDTO>::Err(
          {"addressed_motion_stop_execution_mismatch",
           "ros2_control addressed Stop identity is stale",
           {}});
    }
    std::lock_guard lock(mutex_);
    ++settlements_;
    status_.state = core::ExecutionState::kStopped;
    status_.execution_id = request.execution_id;
    recovery_.assessment_revision += 1U;
    recovery_.control_command_epoch += 1U;
    recovery_.execution_id = request.execution_id;
    recovery_.execution_state = core::ExecutionState::kStopped;
    recovery_.recovery_class = core::RecoveryClass::kOrdinaryOperatorStop;
    recovery_.status = core::RecoveryAssessmentStatus::kRetryReady;
    recovery_.retry_admissible = true;
    recovery_.stop_certificate_id = certificate.stop_certificate_id.value();
    recovery_.stop_certificate_revision =
        certificate.stop_certificate_revision.value();
    recovery_.stop_owner_generation = certificate.stop_generation.value();
    recovery_.backend_stop_confirmed = true;
    recovery_.allowed_actions = {core::RecoveryAction::kReplanFromStoppedState,
                                 core::RecoveryAction::kResetCycle};
    return core::Result<core::AddressedParticipantStopSettlementDTO>::Ok(
        core::AddressedParticipantStopSettlementDTO{
            request.execution_id, certificate, stage.operation_id,
            stage.participant_id, stage.controller_goal_id,
            stage.controller_goal_generation, core::ExecutionState::kStopped,
            recovery_});
  }

  [[nodiscard]] core::ResetResultDTO
  reset_after_certified_containment(std::uint64_t,
                                    std::optional<std::uint64_t>) override {
    return {true, std::nullopt};
  }
  [[nodiscard]] core::AcknowledgeCompletedResultDTO
  acknowledge_completed(std::uint64_t, std::optional<std::uint64_t>) override {
    return {true, std::nullopt};
  }
  [[nodiscard]] core::Result<core::ResolvedRuntimeScaleAcceptedDTO>
  set_runtime_scale(const core::ResolvedRuntimeScaleCommandDTO &) override {
    return Unsupported<core::ResolvedRuntimeScaleAcceptedDTO>();
  }
  [[nodiscard]] core::Result<core::ResolvedRuntimeScaleStatusDTO>
  runtime_scale_status(const core::OperationId &,
                       const core::ControlBindingId &) const override {
    return Unsupported<core::ResolvedRuntimeScaleStatusDTO>();
  }
  [[nodiscard]] core::GripperCommandResultDTO
  command_gripper(const core::GripperCommandRequestDTO &) override {
    return {false, 0U, 0U,
            core::ErrorDTO{
                "unsupported_capability", "test engine has no gripper", {}}};
  }
  [[nodiscard]] core::Result<void> validate_gripper_command(
      const core::GripperCommandRequestDTO &) const override {
    return Unsupported<void>();
  }
  [[nodiscard]] core::GripperCommandStatusDTO
  get_gripper_status(const std::string &) const override {
    return {};
  }
  [[nodiscard]] core::Result<std::uint64_t>
  stop_gripper(const std::string &, std::uint64_t, std::uint64_t) override {
    return Unsupported<std::uint64_t>();
  }
  [[nodiscard]] core::Result<void>
  reset_gripper(const std::string &, std::uint64_t, std::uint64_t) override {
    return Unsupported<void>();
  }
  [[nodiscard]] core::ExecutionStatus get_status() const override {
    std::lock_guard lock(mutex_);
    return status_;
  }
  [[nodiscard]] core::KernelRecoveryAssessmentDTO
  get_recovery_assessment() const override {
    std::lock_guard lock(mutex_);
    return recovery_;
  }
  [[nodiscard]] core::SubscriptionHandle
  subscribe_events(core::EventCallback callback) override {
    std::lock_guard lock(callbacks_mutex_);
    const auto id = next_subscription_id_++;
    callbacks_.emplace(id, std::move(callback));
    return {id};
  }
  void unsubscribe_events(const core::SubscriptionHandle handle) override {
    std::lock_guard lock(callbacks_mutex_);
    callbacks_.erase(handle.id);
  }

  void EmitTransition(core::ExecutionState from, core::ExecutionState to) {
    {
      std::lock_guard lock(mutex_);
      status_.state = to;
      status_.execution_id = kExecutionId;
      status_.progress = 0.1;
    }
    Deliver(core::KernelEvent{core::KernelEventType::kStateTransition,
                              kExecutionId,
                              0,
                              from,
                              to,
                              std::nullopt,
                              {}});
  }

  [[nodiscard]] std::size_t settlements() const {
    std::lock_guard lock(mutex_);
    return settlements_;
  }
  [[nodiscard]] std::uint64_t fence_calls() const noexcept {
    return fence_calls_.load(std::memory_order_relaxed);
  }

private:
  template <typename Value>
  [[nodiscard]] static core::Result<Value> Unsupported() {
    return core::Result<Value>::Err(
        {"unsupported_capability", "test engine path is unsupported", {}});
  }

  void Deliver(const core::KernelEvent &event) {
    std::map<std::uint64_t, core::EventCallback> callbacks;
    {
      std::lock_guard lock(callbacks_mutex_);
      callbacks = callbacks_;
    }
    for (const auto &[id, callback] : callbacks) {
      (void)id;
      callback(event);
    }
  }

  // Adapted from the retained native CCLC admission fake. The only semantic
  // change is a trajectory long enough to remain active on the controller
  // until the test stops it.
  [[nodiscard]] static core::Result<core::PreparedOperationParticipantDTO>
  MakePreparedMotionParticipant(
      const core::ResolvedPreparedMotionExecutionRequestDTO &authority) {
    using namespace manyforge::core;
    const auto &request = authority.execution_request;
    const auto &binding = authority.binding.value();
    const auto &control = *binding.control_path;
    if (!request.motion_interrupt_scopes.empty()) {
      return core::Result<core::PreparedOperationParticipantDTO>::Err(
          {"motion_interrupt_policy_not_admitted",
           "test prepared motion admits no interrupt scopes",
           {}});
    }
    std::vector<CoordinatePositionDTO> positions;
    positions.reserve(binding.resources.size());
    for (const auto &resource : binding.resources) {
      positions.emplace_back(
          resource.coordinate_kind == CoordinateKind::kAngular
              ? CoordinatePositionDTO{AngularPositionDTO{0.0}}
              : CoordinatePositionDTO{LinearPositionDTO{0.0}});
    }
    ControlledStateDTO start{
        binding.controllable_component_id,
        binding.command_target_id,
        control.control_domain_id,
        control.control_domain_revision,
        control.control_binding_id,
        control.control_binding_revision,
        control.controller_topology_id,
        control.topology_revision,
        control.control_route_id,
        control.active_control_configuration_id,
        control.active_control_configuration_generation,
        request.binding.resources,
        request.binding.model_group_binding.compiled_joint_names,
        std::move(positions),
        std::nullopt};
    ProjectionRevisionDTO projection{
        binding.retained_bundle_id, binding.graph_revision,
        SceneRevision{SceneId(Sha('a')), 1U, ContentDigest(Sha('b'))}};
    PlanRequestDTO plan_request{
        request.binding.operation_id,
        request.binding.participant_id,
        request.binding.program_id,
        request.binding.program_revision,
        request.binding.program_binding_id,
        request.binding.program_run_id,
        MoveId(request.move_id),
        request.binding.controllable_component_id,
        request.binding.component_type_id,
        request.binding.component_type_revision,
        request.binding.command_target_id,
        request.binding.capability_id,
        request.binding.capability_definition_revision,
        request.binding.planner_capability_binding,
        request.binding.model_bindings,
        request.binding.model_group_binding,
        request.binding.control_domain_id,
        request.binding.control_domain_revision,
        request.binding.control_binding_id,
        request.binding.control_binding_revision,
        request.binding.control_interface_contract_id,
        request.binding.control_interface_revision,
        request.binding.controller_topology_id,
        request.binding.topology_revision,
        request.binding.control_route_id,
        request.binding.activation_set_id,
        request.binding.active_control_configuration_id,
        request.binding.active_control_configuration_generation,
        request.binding.provider_instance_id,
        request.binding.provider_manifest_id,
        request.binding.provider_manifest_revision,
        request.binding.provider_runtime_epoch,
        request.binding.runtime_unit_bindings,
        request.binding.resources,
        request.action_span,
        request.binding.base_frame,
        request.binding.tcp_frame,
        start,
        request.motion,
        request.tolerances,
        request.speed_profile,
        request.planning_profile,
        request.contact_policy_id.empty()
            ? std::nullopt
            : std::optional<std::string>{request.contact_policy_id},
        request.per_resource_dynamics_limits,
        request.timeouts.planning_timeout_s,
        0U,
        ProjectionLeaseBindingDTO{projection,
                                  ProjectionLeaseId("lease.ros2_control.stop")},
        request.binding.component_attachment_revisions,
        request.binding.carried_object_attachment_revisions,
        {},
        {}};
    // Stationary but long-lived: the FollowJointTrajectory goal stays active
    // on the fake controller until Stop cancels it.
    const std::vector<double> zeros(request.binding.resources.size(), 0.0);
    TrajectoryDTO trajectory{
        request.binding.resources,
        request.binding.model_group_binding.compiled_joint_names,
        {{0.0, zeros, zeros, zeros}, {8.0, zeros, zeros, zeros}}};
    // Production planners always attach runtime-scale bounds.
    trajectory.runtime_scale_bounds =
        core::TrajectoryRuntimeScaleBoundsDTO{4.0, 16.0, 1.0, std::nullopt};
    PlanResultDTO planning_result{trajectory, std::nullopt, projection};
    CollisionWorldRevision world{1U, Sha('c')};
    CollisionDecision decision;
    decision.accepted = true;
    decision.severity = CollisionDecisionSeverity::kNone;
    decision.action = CollisionDecisionAction::kDispatch;
    decision.collision_world_revision = world;

    std::optional<PreparedContactPolicyRefDTO> contact_policy;
    if (request.resolved_contact_grasp) {
      contact_policy = PreparedContactPolicyRefDTO{
          request.resolved_contact_grasp->contact_policy_id,
          request.resolved_contact_grasp->contact_policy_revision};
    }
    PreparedMotionExecutionDTO payload{
        MoveId(request.move_id),
        plan_request,
        trajectory,
        start,
        {std::move(planning_result), 0U,
         request.binding.planner_capability_binding},
        {ValidationRevision("validation.ros2_control.stop"),
         ValidationResultDTO{
             true, std::nullopt,
             TrajectoryValidationMetricsDTO{
                 trajectory.waypoints.back().time_from_start_s,
                 std::vector<double>(trajectory.joint_names.size(), 0.5),
                 std::vector<double>(trajectory.joint_names.size(), 0.0), 0.1}},
         world, CollisionPolicyRevision{1U, Sha('d')}, decision},
        std::move(contact_policy),
        request.speed_profile.max_tcp_linear_speed_mps,
        authority.deployment_maximum_tcp_linear_speed_mps,
        {},
        request.timeouts.execution_timeout_s};

    std::vector<ResourceId> resources = request.binding.resources;
    std::sort(resources.begin(), resources.end());
    std::optional<PreparedCapabilityProviderRuntimeOccurrenceDTO>
        capability_provider;
    if (binding.capability_provider_path) {
      const auto &provider = *binding.capability_provider_path;
      capability_provider = PreparedCapabilityProviderRuntimeOccurrenceDTO{
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision,
          {provider.provider_instance_id, provider.provider_manifest_id,
           provider.provider_manifest_revision,
           provider.provider_runtime_unit_id, provider.provider_runtime_epoch}};
    }
    PreparedOperationParticipantDTO candidate{
        ContentDigest("pending"),
        request.binding.operation_id,
        authority.operation_revision,
        request.binding.participant_id,
        binding.program_id,
        binding.program_revision,
        binding.program_binding_id,
        request.binding.program_run_id,
        binding.retained_bundle_id,
        binding.graph_revision,
        binding.controllable_component_id,
        authority.stop_readiness.value().component_lifecycle_generation,
        binding.component_type_id,
        binding.component_type_revision,
        binding.command_target_id,
        binding.capability_id,
        binding.capability_definition_revision,
        binding.model_bindings,
        binding.frame_bindings,
        control.control_domain_id,
        control.control_domain_revision,
        control.control_binding_id,
        control.control_binding_revision,
        control.control_interface_contract_id,
        control.control_interface_revision,
        control.controller_topology_id,
        control.topology_revision,
        control.control_route_id,
        control.activation_set_id,
        {control.provider_instance_id, control.provider_manifest_id,
         control.provider_manifest_revision, control.provider_runtime_unit_id,
         control.provider_runtime_epoch},
        std::move(capability_provider),
        std::move(resources),
        PreparedParticipantFocusedPortDTO{control.provider_runtime_unit_id},
        {},
        {},
        control.active_control_configuration_id,
        control.active_control_configuration_generation,
        binding.runtime_unit_epochs,
        control.stop_evidence_policy_id,
        control.stop_evidence_policy_revision,
        *authority.ordinary_stop_plan,
        std::move(payload),
        authority.fault_stop_plan
            ? std::optional<
                  PreparedParticipantStopPlanDTO>{*authority.fault_stop_plan}
            : std::nullopt};
    return MakePreparedOperationParticipant(
        std::move(candidate), authority.binding, authority.stop_readiness,
        authority.retained_default_stop_limits, authority.stop_node_category);
  }

  Ros2ControlExecutionBackend &backend_;
  mutable std::mutex mutex_;
  std::optional<core::ResolvedPreparedMotionExecutionRequestDTO> request_;
  std::optional<core::ResolvedPreparedMotionExecutionSnapshotDTO> snapshot_;
  core::ExecutionStatus status_{};
  core::KernelRecoveryAssessmentDTO recovery_{};
  std::size_t settlements_{};
  std::atomic<bool> dispatched_{false};
  std::atomic<std::uint64_t> fence_calls_{0U};
  mutable std::mutex callbacks_mutex_;
  std::uint64_t next_subscription_id_{1U};
  std::map<std::uint64_t, core::EventCallback> callbacks_;
};

[[nodiscard]] core::RuntimeParticipantExecutionRegistration
Registration(const core::RuntimeParticipantBinding &binding,
             core::RuntimeParticipantStopReadiness readiness,
             Ros2ControlPreparedMotionEngine &engine,
             RecordingRos2ControlStopPort &port) {
  std::vector<core::HardStopResourceLimitDTO> limits;
  for (const auto &resource : binding.value().resources) {
    limits.push_back(core::HardStopResourceLimitDTO{
        resource.resource_id,
        core::CoordinateVelocityDTO{core::AngularVelocityDTO{0.02}}});
  }
  return core::RuntimeParticipantExecutionRegistration{
      .binding = binding,
      .stop_readiness = std::move(readiness),
      .retained_default_stop_limits = std::move(limits),
      .stop_node_category = core::MotionInterruptNodeCategory::kGeneric,
      .engine = &engine,
      .stop_execution_port = &port,
      .participant_policy = core::select_runtime_participant_policy(binding),
      .prepared_motion_engine = &engine,
      .default_operator_stop_profile = std::nullopt,
      .default_fault_stop_profile = std::nullopt,
      .deployment_maximum_tcp_linear_speed_mps = 0.5,
      .intrinsic_direct_control = false,
      .direct_control_joint_limits = {}};
}

[[nodiscard]] core::PublicRequestContextDTO Context(std::string request_id) {
  return core::PublicRequestContextDTO{core::RequestId(std::move(request_id)),
                                       core::RequestOwnerEpoch(kOwnerEpoch),
                                       "test", std::nullopt};
}

[[nodiscard]] core::OperationIntentDTO
MotionOperation(const core::RuntimeParticipantBinding &binding) {
  const auto &value = binding.value();
  std::vector<std::string> source_joint_names;
  std::vector<double> goal_positions;
  std::map<std::string, core::JointDynamicsLimitDTO> dynamics;
  for (const auto &resource : value.resources) {
    source_joint_names.push_back(*resource.source_joint_name);
    goal_positions.push_back(0.01);
    dynamics.emplace(*resource.source_joint_name,
                     core::JointDynamicsLimitDTO{1.0, 2.0, std::nullopt});
  }
  const core::JointGoalDTO joint_goal{std::move(source_joint_names),
                                      std::move(goal_positions)};
  const core::MotionParticipantIntentDTO motion{
      core::MoveId("move.ros2_control.stop.1"),
      core::FrameRoleId("base"),
      core::FrameRoleId("tcp"),
      core::TolerancesDTO{0.01, 0.001, 0.01},
      core::SpeedProfileDTO{},
      core::TimeoutsDTO{},
      core::CacheMode::kDefault,
      core::PlanningProfileDTO::kBalanced,
      core::MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      joint_goal,
      std::nullopt,
      std::move(dynamics),
      {},
      {}};
  const core::OperationParticipantIntentDTO participant{
      value.component_role_id, value.target_role_id, value.capability_id,
      std::nullopt, motion};
  std::vector<core::OperationParticipantIntentDTO> participants{participant};
  return {Context("execute.ros2_control.stop"),
          value.program_id,
          value.program_revision,
          value.program_binding_id,
          core::ProgramRunId("run.ros2_control.stop.1"),
          value.graph_revision,
          value.retained_bundle_id,
          std::nullopt,
          std::move(participants),
          core::OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] bool FinishCancelledGoal(fixture::FakeFjtServer &server) {
  return WaitUntil([&] { return server.cancel_current_goal(); });
}

[[nodiscard]] std::optional<core::StopStatusDTO>
WaitForTerminalStop(const core::ResourceAddressedExecutionKernel &kernel,
                    const core::StopId &stop_id) {
  std::optional<core::StopStatusDTO> last;
  const bool terminal = WaitUntil(
      [&] {
        auto status = kernel.get_stop_status(stop_id);
        if (!status.ok()) {
          return false;
        }
        last = std::move(status.value());
        return last->state == core::StopStatusStateDTO::kSucceeded ||
               last->state == core::StopStatusStateDTO::kPartial ||
               last->state == core::StopStatusStateDTO::kFailed;
      },
      10s);
  return terminal ? last : std::nullopt;
}

[[nodiscard]] std::string StageReason(
    const core::ParticipantStopStageResultDTO &result) {
  if (!result.error.has_value()) {
    return "no diagnostic";
  }
  const auto reason = result.error->details.find("reason");
  return reason == result.error->details.end()
             ? result.error->message
             : reason->second + ": " + result.error->message;
}

void RemoveJournalFiles(const std::filesystem::path &path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + "-wal", ignored);
  std::filesystem::remove(path.string() + "-shm", ignored);
}

enum class StopKind { kFullStop, kOperationStop };

void RunStopDuringActiveRos2ControlMotion(const StopKind kind) {
  WallClock clock;
  SimpleScheduler scheduler;
  const auto binding = KernelBinding();
  const auto &path = *binding.value().control_path;
  const auto domain = ExpectedDomain(binding);
  auto readiness = StopReadiness(binding, domain);

  const std::string action_name =
      "/manyforge_kernel_ros2_control_stop_fjt_" + fixture::unique_suffix();
  fixture::FakeFjtServer server(action_name);
  auto sink = std::make_shared<fixture::StateSink>();
  // As in the Isaac commanded-simulation profile: no controller
  // speed-scaling input, so bounded trajectories run at nominal timing.
  auto config = fixture::backend_config(binding, action_name);
  config.enable_jtc_speed_scaling_input = false;
  ros2_control_execution::Ros2TransportResources resources(
      config.node_name, config.node_namespace, config.executor_thread_count,
      config.command_gate_topic, sink, "arm", clock);
  Ros2ControlExecutionBackend backend(resources, sink, "arm", clock, scheduler,
                                      config);
  RecordingRos2ControlStopPort port(backend);
  Ros2ControlPreparedMotionEngine engine(backend);
  // Quiesce callbacks while the adapter and its test ports are still alive,
  // including fatal assertion returns. Resources outlive the adapter.
  struct TransportLifetime final {
    ros2_control_execution::Ros2TransportResources &resources;
    Ros2ControlExecutionBackend &backend;
    ~TransportLifetime() {
      backend.shutdown();
      resources.quiesce();
    }
  } transport_lifetime{resources, backend};
  resources.start();

  const auto journal_path =
      std::filesystem::temp_directory_path() /
      ("manyforge-ros2-control-kernel-stop-" + fixture::unique_suffix() + "-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".sqlite");
  {
    const core::CellId cell_id(kCellId);
    core::SqliteOperationJournal journal(journal_path, cell_id, 16U);
    core::ResourceAddressedExecutionKernel kernel(
        {Registration(binding, std::move(readiness), engine, port)}, clock,
        scheduler, journal,
        core::ResourceAddressedExecutionKernelConfig{
            core::RequestOwnerEpoch(kOwnerEpoch), cell_id, 16U});

    // 1. A real Kernel motion dispatches a live FollowJointTrajectory goal.
    const auto operation = kernel.execute(MotionOperation(binding));
    ASSERT_TRUE(operation.ok()) << operation.error().message;
    const auto &operation_id = operation.value().operation_id;
    ASSERT_TRUE(server.wait_for_goal_count(1U, 5s))
        << "Kernel dispatch did not reach the ros2_control action server";
    ASSERT_TRUE(WaitUntil([&] {
      return backend.get_execution_snapshot().state ==
             core::ExecutionState::kExecuting;
    }));
    ASSERT_TRUE(server.publish_feedback());
    engine.EmitTransition(core::ExecutionState::kPlanning,
                          core::ExecutionState::kExecuting);
    std::optional<core::OperationStateDTO> before;
    ASSERT_TRUE(WaitUntil([&] {
      auto current = kernel.get_operation_state(operation_id);
      if (!current.ok()) {
        return false;
      }
      before = std::move(current.value());
      return before->state == core::OperationStateKind::kExecuting;
    })) << "Kernel operation never observed the active ros2_control goal";
    ASSERT_EQ(before->participants.size(), 1U);
    const auto goal_id = before->participants.front().controller_goal_id;
    const auto goal_generation =
        before->participants.front().controller_goal_generation;
    EXPECT_EQ(server.cancel_requests(), 0U);
    EXPECT_TRUE(port.results().empty());

    // 2. Stop is accepted while that goal is active.
    auto stopping = std::async(std::launch::async, [&] {
      if (kind == StopKind::kFullStop) {
        return kernel.full_stop(core::FullStopIntentDTO{
            Context("request.ros2_control.full-stop"), core::StopGeneration(0U),
            {}});
      }
      return kernel.stop(core::StopIntentDTO{
          Context("request.ros2_control.operation-stop"),
          std::vector<core::StopOperationTargetDTO>{core::StopOperationTargetDTO{
              operation_id, before->operation_revision,
              core::StopGeneration(0U)}},
          std::nullopt});
    });
    // The Kernel's Stop stage must reach the ROS controller as a goal cancel.
    const bool cancel_requested = server.wait_for_cancel_requests(1U, 5s);
    const bool cancel_completed = cancel_requested && FinishCancelledGoal(server);
    const auto accepted = stopping.get();
    ASSERT_TRUE(accepted.ok()) << accepted.error().message;
    ASSERT_TRUE(cancel_requested)
        << "Kernel Stop never cancelled the ros2_control goal";
    ASSERT_TRUE(cancel_completed);
    const auto &stop_id = accepted.value().stop_id;

    // 3. Captured scope: the active operation and its exact revision.
    if (kind == StopKind::kFullStop) {
      EXPECT_EQ(accepted.value().resolution_reason,
                core::StopScopeResolutionReasonDTO::kExplicitFullStop);
    }
    ASSERT_EQ(accepted.value().affected_operation_revisions.size(), 1U);
    EXPECT_EQ(accepted.value().affected_operation_revisions.front().operation_id,
              operation_id);

    const auto status = WaitForTerminalStop(kernel, stop_id);
    ASSERT_TRUE(status.has_value()) << "Stop did not reach a terminal state";

    // 4. The ros2_control port realized exactly the nominal stage.
    const auto results = port.results();
    ASSERT_EQ(results.size(), 1U)
        << "unexpected escalation: "
        << (results.empty() ? std::string("none") : StageReason(results.front()));
    const auto &stage_result = results.front();
    EXPECT_EQ(stage_result.attempt_disposition,
              core::StopStageAttemptDispositionDTO::kDispatched);
    ASSERT_EQ(stage_result.outcome,
              core::ParticipantStopStageOutcomeDTO::kSucceeded)
        << StageReason(stage_result);
    const auto &stage = stage_result.request;
    EXPECT_EQ(stage.stop_id, stop_id);
    EXPECT_EQ(stage.operation_id, operation_id);
    EXPECT_EQ(stage.controller_goal_id, goal_id);
    EXPECT_EQ(stage.controller_goal_generation, goal_generation);
    EXPECT_EQ(stage.control_domain_id, path.control_domain_id);
    EXPECT_EQ(stage.control_binding_id, path.control_binding_id);
    EXPECT_EQ(stage.stage_index, 0U);
    ASSERT_LT(stage.stage_index,
              stage.participant_stop_plan.stage_commands.size());
    const auto &resolved_stage =
        stage.participant_stop_plan.stage_commands[stage.stage_index]
            .resolved_stage;
    EXPECT_EQ(resolved_stage.stage_kind, core::StopEscalationStageKind::kNominal);
    EXPECT_EQ(resolved_stage.containment_command.command_kind,
              core::StopContainmentCommandKind::kControllerGoalCancel);
    ASSERT_EQ(stage_result.evidence_items.size(), 1U);
    EXPECT_EQ(stage_result.evidence_items.front().required_class,
              core::StopEvidenceRequirementKind::kControllerGoalTerminal);
    EXPECT_EQ(stage_result.evidence_items.front().state,
              core::StopEvidenceItemStateDTO::kSatisfied);
    EXPECT_GE(server.cancel_requests(), 1U);
    EXPECT_EQ(server.goal_count(), 1U) << "Stop dispatched a second goal";
    EXPECT_EQ(backend.get_execution_snapshot().state,
              core::ExecutionState::kStopped);
    EXPECT_GE(engine.fence_calls(), 1U);
    EXPECT_EQ(engine.settlements(), 1U);

    // 5. Terminal status and certificate name the ros2_control participant.
    EXPECT_EQ(status->state, core::StopStatusStateDTO::kSucceeded);
    EXPECT_FALSE(status->fault_latch.has_value());
    EXPECT_FALSE(kernel.get_stop_fault_latch().has_value());
    EXPECT_TRUE(status->failed.empty());
    EXPECT_TRUE(status->unsettled.empty());
    const auto expected_key = core::StopScopeEntryKeyDTO{
        stop_id, accepted.value().stop_generation,
        core::StopParticipantScopeKeyDTO{operation_id, stage.participant_id}};
    const auto entry = std::find_if(
        status->scope_entries.begin(), status->scope_entries.end(),
        [&](const auto &row) { return row.key == expected_key; });
    ASSERT_NE(entry, status->scope_entries.end())
        << "terminal Stop status does not contain the ros2_control participant";
    EXPECT_EQ(entry->dispatch_state, core::StopScopeDispatchStateDTO::kDispatched);
    const auto *facts =
        std::get_if<core::StopParticipantScopeFactsDTO>(&entry->facts);
    ASSERT_NE(facts, nullptr);
    EXPECT_EQ(facts->controllable_component_id,
              binding.value().controllable_component_id);
    EXPECT_EQ(facts->control_domain_id, path.control_domain_id);
    EXPECT_EQ(facts->control_binding_id, path.control_binding_id);
    EXPECT_EQ(facts->control_route_id, path.control_route_id);
    EXPECT_EQ(facts->provider_runtime_epoch, path.provider_runtime_epoch);
    EXPECT_EQ(facts->controller_goal_id, goal_id);
    EXPECT_EQ(facts->terminal_settlement,
              core::StopParticipantScopeFactsDTO::TerminalSettlementState::
                  kSettled);

    ASSERT_TRUE(status->stop_certificate.has_value());
    const auto certificate =
        kernel.get_stop_certificate(status->stop_certificate->stop_certificate_id);
    ASSERT_TRUE(certificate.ok()) << certificate.error().message;
    EXPECT_EQ(certificate.value().stop_id, stop_id);
    EXPECT_EQ(certificate.value().result,
              core::StopCertificateResultDTO::kSucceededNominal);
    EXPECT_NE(std::find_if(certificate.value().scope_entries.begin(),
                           certificate.value().scope_entries.end(),
                           [&](const auto &row) { return row.key == expected_key; }),
              certificate.value().scope_entries.end());
    EXPECT_NE(std::find(certificate.value().dispatched_stopped.begin(),
                        certificate.value().dispatched_stopped.end(),
                        expected_key),
              certificate.value().dispatched_stopped.end());
    EXPECT_TRUE(certificate.value().failed.empty());
    EXPECT_TRUE(certificate.value().unsettled.empty());
    ASSERT_EQ(certificate.value().attempted_stage_results.size(), 1U);
    const auto &attempt = certificate.value().attempted_stage_results.front();
    EXPECT_EQ(attempt.scope_key, expected_key);
    EXPECT_EQ(attempt.authority, core::StopStageResultAuthorityDTO::kAuthoritative);
    EXPECT_EQ(attempt.result, stage_result);

    // 6. The operation itself is terminally stopped by this Stop.
    const auto after = kernel.get_operation_state(operation_id);
    ASSERT_TRUE(after.ok()) << after.error().message;
    EXPECT_EQ(after.value().state, core::OperationStateKind::kStopped);
    EXPECT_EQ(after.value().active_stop_id, stop_id);
  }
  RemoveJournalFiles(journal_path);
}

TEST(Ros2ControlKernelStopIntegrationTest,
     FullStopDuringActiveRos2ControlMotionCancelsGoalAndCertifiesParticipant) {
  RunStopDuringActiveRos2ControlMotion(StopKind::kFullStop);
}

TEST(Ros2ControlKernelStopIntegrationTest,
     OperationStopDuringActiveRos2ControlMotionCancelsGoalAndCertifiesParticipant) {
  RunStopDuringActiveRos2ControlMotion(StopKind::kOperationStop);
}

} // namespace
} // namespace manyforge::roboplan_backend
