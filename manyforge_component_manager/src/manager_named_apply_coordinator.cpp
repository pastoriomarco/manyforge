#include "manyforge_component_manager/manager_named_apply_coordinator.hpp"

#include "native_projection_transition_internal.hpp"

#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/native_domain_topology.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <set>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::size_t kMaximumApplyIdMintAttempts = 4U;
constexpr std::size_t kMaximumSelectedBundleAdvances = 2U;

[[nodiscard]] bool SameDeploymentRevision(
    const core::ProjectionRevisionDTO &left,
    const core::ProjectionRevisionDTO &right) noexcept {
  return left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision;
}

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerNamedApplyCoordinatorError(detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

[[nodiscard]] bool IsTerminal(const PersistedManagerApplyStatus &status) {
  return status.terminal_outcome.has_value();
}

/// Map one durable terminal outcome to its public result state.
///
/// A terminal failure or unknown-effect operation is never reported as success
/// and never resumes. Published success settles admission; the only failure
/// allowed to settle a transferred parent claim is a known refusal retained at
/// the pre-effect `accepted` phase.
[[nodiscard]] ManagerNamedApplyResultState ResultStateFromTerminal(
    const ManagerApplyTerminalOutcomeDTO &terminal) {
  switch (terminal.state) {
  case ManagerApplyTerminalOutcomeState::kSucceeded:
    return ManagerNamedApplyResultState::kSucceeded;
  case ManagerApplyTerminalOutcomeState::kFailed:
    return ManagerNamedApplyResultState::kFailed;
  case ManagerApplyTerminalOutcomeState::kUnknownEffect:
    return ManagerNamedApplyResultState::kUnknownEffect;
  }
  return ManagerNamedApplyResultState::kUnknownEffect;
}

[[nodiscard]] ManagerNamedApplyResult RetainTerminalFailure(
    SqliteManagerApplyJournal &journal,
    const PersistedManagerApplyIntent &durable_intent,
    const std::string &failure_code, const std::string &failure_message,
    const ManagerApplyEffectDisposition disposition,
    const bool request_replayed) {
  const auto status = journal.ReadStatus(
      durable_intent.intent.apply_id, durable_intent.intent.apply_generation);
  Require(status.has_value(),
          "Manager Apply disappeared while retaining terminal failure");
  const auto bounded_code =
      !failure_code.empty() &&
              failure_code.size() <= kMaximumManagerApplyFailureCodeBytes
          ? failure_code
          : std::string("manager_apply_failure");
  const auto bounded_message =
      failure_message.size() <= kMaximumManagerApplyFailureMessageBytes
          ? failure_message
          : std::string("failure detail exceeded the retained message bound");
  const ManagerApplyTerminalOutcomeDTO outcome{
      durable_intent.intent.apply_id,
      durable_intent.intent.apply_generation,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      disposition == ManagerApplyEffectDisposition::kKnown
          ? ManagerApplyTerminalOutcomeState::kFailed
          : ManagerApplyTerminalOutcomeState::kUnknownEffect,
      ManagerApplyFailureEvidenceDTO{
          durable_intent.intent_digest,
          DeriveManagerApplyProgress(*status).phase,
          bounded_code,
          bounded_message,
          disposition},
  };
  Require(outcome.valid(), "Manager Apply terminal failure is invalid");
  const auto retained = journal.EnsureTerminalOutcome(outcome);
  return {durable_intent.intent.apply_id,
          durable_intent.intent.apply_generation,
          ResultStateFromTerminal(retained.record.terminal_outcome),
          request_replayed,
          retained.record.terminal_outcome};
}

[[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
LoadBundle(const deployment::IVerifiedRetainedBundleSource &source,
           const core::RetainedBundleId &bundle_id,
           const std::string_view label) {
  auto bundle = source.get_verified(bundle_id);
  Require(bundle != nullptr &&
              deployment::verify_retained_bundle_identity(*bundle) &&
              bundle->retained_bundle_id() == bundle_id,
          std::string(label) +
              " retained bundle is absent or fails independent verification");
  return bundle;
}

[[nodiscard]] ManagerApplyFenceExpectationDTO FenceExpectation(
    const ManagerApplyIntentDTO &intent,
    const deployment::RetainedBundle &predecessor,
    const deployment::RetainedBundle &candidate,
    const ManagerActiveRuntimePlanDTO &active_plan,
    SqliteManagerApplyJournal &journal,
    core::IManagerKernelApplyFencePort &kernel_fence,
    const core::RuntimeEpoch &manager_runtime_epoch,
    IManagerSelectedRuntimeCompositionPort *runtime_composition,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source) {
  if (const auto attempt =
          journal.ReadFenceAttempt(intent.apply_id, intent.apply_generation);
      attempt.has_value()) {
    const auto &fence = attempt->fence_intent;
    return {
        fence.expected_operation_resource_generation,
        fence.expected_fence_generation,
        fence.expected_active_control_configurations,
        fence.scope,
    };
  }

  auto scope = deployment::derive_apply_configuration_fence_scope(predecessor,
                                                                  candidate);
  core::ConfigurationFenceAdmissionQueryDTO query{scope, manager_runtime_epoch};
  Require(query.valid(), "derived Kernel admission query is invalid");

  auto snapshot = kernel_fence.ObserveAdmission(query);
  Require(snapshot.matches(query),
          "Kernel returned invalid or foreign admission evidence");
  Require(SameDeploymentRevision(snapshot.active_projection_revision,
                                 intent.expected_active_projection_revision) &&
              snapshot.active_public_pointer_digest ==
                  active_plan.active_publication
                      .durable_public_pointer_content_digest &&
              snapshot.execution_host_epoch.publication_generation ==
                  active_plan.active_publication.publication_generation,
          "Kernel admission evidence does not match the current durable "
          "predecessor publication");
  if (runtime_composition != nullptr) {
    const ManagerComponentProcessInventoryRequestDTO inventory_request{
        active_plan.active_publication.request.candidate_projection_revision,
        active_plan.active_publication.publication_generation,
        active_plan.active_publication.durable_public_pointer_content_digest,
        snapshot.execution_host_epoch.runtime_unit_id,
        snapshot.execution_host_epoch.runtime_epoch,
        manager_runtime_epoch};
    const auto inventory =
        runtime_composition->InspectInstalledComponentProcesses(
            inventory_request);
    if (!inventory.ok())
      Fail("actual_installed_state_unknown: " + inventory.error().message +
           "; restart the workbench container");
    Require(inventory.value().valid() &&
                inventory.value().request == inventory_request,
            "actual_installed_state_unknown: installed process inventory is "
            "foreign");
    if (inventory.value().error)
      Fail(inventory.value().error->code + ": " +
           inventory.value().error->message +
           "; restart the workbench container");
    std::set<core::ControllableComponentId> restart;
    for (const auto &row : inventory.value().process_occurrences) {
      Require(row.process_presence != ProcessPresenceState::kUnknown &&
                  row.installed_runtime_identity,
              "actual_installed_state_unknown: installed process identity or "
              "presence is unknown; restart the workbench container");
      const auto wanted = deployment::derive_component_runtime_identity(
          candidate, row.controllable_component_id);
      if (row.process_presence != ProcessPresenceState::kPresent ||
          wanted != row.installed_runtime_identity)
        restart.insert(row.controllable_component_id);
    }
    for (const auto &component :
         candidate.value().cell_graph.controllable_components) {
      const auto wanted = deployment::derive_component_runtime_identity(
          candidate, component.controllable_component_id);
      const auto count = std::count_if(
          inventory.value().process_occurrences.begin(),
          inventory.value().process_occurrences.end(), [&](const auto &row) {
            return row.controllable_component_id ==
                       component.controllable_component_id &&
                   row.process_presence == ProcessPresenceState::kPresent &&
                   row.installed_runtime_identity == wanted;
          });
      Require(count <= 1, "actual_installed_state_unknown: component has "
                          "multiple current process owners");
      if (count == 0)
        restart.insert(component.controllable_component_id);
    }
    std::vector<std::shared_ptr<const deployment::RetainedBundle>>
        terminal_products;
    std::vector<const deployment::RetainedBundle *> source_products;
    const auto old_statuses = journal.ReadStatusInventory();
    for (const auto &row : inventory.value().process_occurrences) {
      if (!restart.contains(row.controllable_component_id))
        continue;
      const auto old_identity = deployment::derive_component_runtime_identity(
          predecessor, row.controllable_component_id);
      const auto new_identity = deployment::derive_component_runtime_identity(
          candidate, row.controllable_component_id);
      if (old_identity == row.installed_runtime_identity ||
          new_identity == row.installed_runtime_identity)
        continue;
      std::shared_ptr<const deployment::RetainedBundle> source;
      for (const auto &status : old_statuses) {
        if (!status.terminal_outcome || status.bring_up_plan ||
            status.terminal_outcome->terminal_outcome.succeeded())
          continue;
        const auto product = retained_bundle_source.get_verified(
            status.intent.intent.candidate_retained_bundle_id);
        if (!product ||
            !deployment::verify_retained_bundle_identity(*product) ||
            product->value().cell_graph.cell_id != intent.cell_id ||
            product->value().cell_graph.graph_revision !=
                status.intent.intent.candidate_graph_revision)
          continue;
        if (deployment::derive_component_runtime_identity(
                *product, row.controllable_component_id) ==
            row.installed_runtime_identity) {
          source = product;
          break;
        }
      }
      Require(
          source != nullptr,
          "actual_installed_state_unknown: installed failed candidate "
          "lacks its exact terminal product; restart the workbench container");
      if (std::none_of(terminal_products.begin(), terminal_products.end(),
                       [&](const auto &known) {
                         return known->retained_bundle_id() ==
                                source->retained_bundle_id();
                       })) {
        source_products.push_back(source.get());
        terminal_products.push_back(std::move(source));
      }
    }
    const std::vector<core::ControllableComponentId> components(restart.begin(),
                                                                restart.end());
    const auto actual_scope =
        deployment::derive_apply_configuration_fence_scope(
            predecessor, candidate, components, source_products);
    if (actual_scope != scope) {
      query.affected_scope = actual_scope;
      const auto expanded = kernel_fence.ObserveAdmission(query);
      Require(expanded.matches(query) &&
                  expanded.execution_host_epoch ==
                      snapshot.execution_host_epoch &&
                  expanded.active_projection_revision ==
                      snapshot.active_projection_revision &&
                  expanded.active_public_pointer_digest ==
                      snapshot.active_public_pointer_digest,
              "actual_installed_state_unknown: original host or publication "
              "changed during scope observation");
      snapshot = expanded;
      scope = actual_scope;
    }
  }
  if (snapshot.affected_queued_operation_count != 0U ||
      snapshot.affected_active_operation_count != 0U) {
    throw ManagerNamedApplyBusyError(
        "named Apply rejected because its affected resources are busy");
  }
  return {
      snapshot.operation_resource_generation,
      snapshot.configuration_fence_generation,
      snapshot.active_control_configurations,
      scope,
  };
}

[[nodiscard]] bool MatchesRequest(const ManagerApplyIntentDTO &intent,
                                  const ManagerNamedApplyRequestDTO &request) {
  return intent.request_context == request.request_context &&
         intent.expected_active_projection_revision.retained_bundle_id ==
             request.expected_current_retained_bundle_id &&
         intent.expected_active_projection_revision.graph_revision ==
             request.expected_current_graph_revision &&
         intent.candidate_retained_bundle_id ==
             request.candidate_retained_bundle_id &&
         intent.prepared_activation == request.prepared_activation &&
         intent.expected_admission_generation ==
             request.expected_admission_generation &&
         intent.parent_authoring_claim == request.parent_authoring_claim &&
         intent.isaac_pose_observer_launch_facts ==
             request.isaac_pose_observer_launch_facts;
}

[[nodiscard]] core::CellAdmissionClaimIntentDTO
DirectAdmissionIntent(
    const PersistedManagerApplyIntent &durable,
    const std::optional<core::ContentDigest> &bring_up_plan_digest = {}) {
  const auto &intent = durable.intent;
  Require(intent.expected_admission_generation.has_value(),
          "direct Manager Apply lacks its admission generation");
  core::CellAdmissionClaimIntentDTO claim{
      core::CellAdmissionClaimKindDTO::kManagerApply,
      core::CellAdmissionOwnerRoleDTO::kManager,
      intent.request_context.request_owner_epoch,
      intent.apply_id.value(),
      intent.request_context.request_id,
      bring_up_plan_digest.value_or(durable.intent_digest),
      *intent.expected_admission_generation,
      {},
      std::nullopt,
      std::nullopt,
  };
  Require(core::ValidateCellAdmissionClaimIntent(claim).ok(),
          "durable Manager Apply formed an invalid admission claim");
  return claim;
}

[[nodiscard]] core::CellAdmissionClaimDTO RequireHeldAdmission(
    const PersistedManagerApplyIntent &durable,
    core::IKernelCellAdmissionPort &cell_admission,
    const std::optional<core::ContentDigest> &bring_up_plan_digest = {},
    const std::optional<core::CellAdmissionClaimDTO> &original_parent_claim = {}) {
  if (original_parent_claim) {
    Require(bring_up_plan_digest && !durable.intent.parent_authoring_claim &&
                original_parent_claim->intent == DirectAdmissionIntent(durable, bring_up_plan_digest),
            "original configured parent claim differs from its sealed plan");
    const core::CellAdmissionRecoveryReceiptQueryDTO query{
        original_parent_claim->intent, original_parent_claim, std::nullopt};
    const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(query);
    Require(core::ValidateCellAdmissionRecoveryReceiptQuery(query).ok() && receipt.ok() &&
                core::ValidateCellAdmissionRecoveryReceipt(receipt.value()).ok() &&
                receipt.value().status == core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
                receipt.value().matched_claim == original_parent_claim,
            "original configured parent claim is no longer held exactly");
    return *original_parent_claim;
  }
  if (durable.intent.parent_authoring_claim.has_value()) {
    const auto &parent = *durable.intent.parent_authoring_claim;
    const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(
        {parent.intent, parent, std::nullopt});
    if (!receipt.ok()) {
      Fail("Kernel parent-claim lookup failed: " + receipt.error().message);
    }
    Require(receipt.value().status ==
                    core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
                receipt.value().matched_claim == parent,
            "Program Apply parent authoring claim is not held exactly");
    return parent;
  }

  const auto intent = DirectAdmissionIntent(durable, bring_up_plan_digest);
  const core::CellAdmissionRecoveryReceiptQueryDTO query{
      intent, std::nullopt, std::nullopt};
  const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(query);
  if (!receipt.ok()) {
    Fail("Kernel Manager-claim lookup failed: " + receipt.error().message);
  }
  if (bring_up_plan_digest) {
    Require(core::ValidateCellAdmissionRecoveryReceipt(receipt.value()).ok() &&
                (!receipt.value().matched_claim ||
                 receipt.value().matched_claim->intent == intent),
            "configured bring-up received invalid or foreign parent-claim evidence");
  }
  if (receipt.value().status ==
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
      receipt.value().matched_claim.has_value()) {
    if (bring_up_plan_digest) {
      Require(core::ValidateCellAdmissionRecoveryReceiptQuery(
                  {intent, receipt.value().matched_claim, std::nullopt}).ok(),
              "configured bring-up retained claim differs from original admission generation");
    }
    return *receipt.value().matched_claim;
  }
  Require(receipt.value().status ==
              core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
          "Manager Apply admission claim is conflicting or already settled");
  const auto acquired = cell_admission.AcquireCellAdmissionClaim(intent);
  if (!acquired.ok()) {
    const auto &error = acquired.error();
    const auto reason = error.details.find("reason");
    Fail("Kernel rejected Manager Apply admission: " +
         error.code + ": " + error.message +
         (reason == error.details.end() ? std::string{}
                                        : " (" + reason->second + ")"));
  }
  if (bring_up_plan_digest) {
    Require(acquired.value().intent == intent &&
                core::ValidateCellAdmissionRecoveryReceiptQuery(
                    {intent, acquired.value(), std::nullopt}).ok(),
            "configured bring-up acquired a foreign or invalid original parent claim");
  }
  return acquired.value();
}

void ReleaseAdmission(
    const PersistedManagerApplyIntent &durable,
    const PersistedManagerApplyTerminalOutcome &terminal,
    core::IKernelCellAdmissionPort &cell_admission,
    const std::optional<core::ContentDigest> &bring_up_plan_digest = {},
    const std::optional<core::CellAdmissionClaimDTO> &original_parent_claim = {}) {
  const auto intent = durable.intent.parent_authoring_claim.has_value()
                          ? durable.intent.parent_authoring_claim->intent
                          : DirectAdmissionIntent(durable, bring_up_plan_digest);
  const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(
      {intent, original_parent_claim, std::nullopt});
  if (!receipt.ok()) {
    Fail("Kernel Manager-claim settlement lookup failed: " +
         receipt.error().message);
  }
  if (bring_up_plan_digest) {
    Require(core::ValidateCellAdmissionRecoveryReceipt(receipt.value()).ok() &&
                (!receipt.value().matched_claim || receipt.value().matched_claim->intent == intent) &&
                (!original_parent_claim || receipt.value().matched_claim == original_parent_claim),
            "configured parent settlement lookup returned foreign claim evidence");
  }
  if (receipt.value().status ==
      core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact) {
    return;
  }
  Require(receipt.value().status ==
                  core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
              receipt.value().matched_claim.has_value(),
          "terminal Manager Apply has no exact held admission claim");
  const auto &claim = *receipt.value().matched_claim;
  const auto settlement_kind = durable.intent.parent_authoring_claim.has_value()
                                   ? core::CellAdmissionSettlementKindDTO::kOwnerCommit
                                   : core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled;
  const core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {settlement_kind, terminal.terminal_outcome_digest},
  };
  const auto released = cell_admission.ReleaseCellAdmissionClaim(release);
  if (!released.ok()) {
    const auto reason = released.error().details.find("reason");
    Fail("Kernel rejected Manager Apply settlement: " +
         released.error().message +
         (reason == released.error().details.end()
              ? std::string{}
              : " [" + reason->second + "]"));
  }
}

} // namespace

bool ManagerNamedApplyRequestDTO::valid() const noexcept {
  const bool direct = expected_admission_generation.has_value() &&
                      expected_admission_generation->nonzero() &&
                      !parent_authoring_claim.has_value();
  const bool parent = !expected_admission_generation.has_value() &&
                      parent_authoring_claim.has_value() &&
                      core::ValidateCellAdmissionClaimIntent(
                          parent_authoring_claim->intent)
                          .ok() &&
                      parent_authoring_claim->intent.claim_kind ==
                          core::CellAdmissionClaimKindDTO::kAuthoringMutation &&
                      parent_authoring_claim->intent.owner_role ==
                          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner;
  return (direct || parent) && !request_context.request_id.empty() &&
         !request_context.request_owner_epoch.empty() &&
         !request_context.source_label.empty() &&
         (!request_context.correlation_id.has_value() ||
          !request_context.correlation_id->empty()) &&
         !deployment_artifact_id.empty() &&
         core::IsExactLowercaseSha256Digest(deployment_revision.value()) &&
         !expected_current_retained_bundle_id.empty() &&
         !expected_current_graph_revision.empty() &&
         !candidate_retained_bundle_id.empty() &&
         prepared_activation.valid() &&
         (!isaac_pose_observer_launch_facts ||
          isaac_pose_observer_launch_facts->valid());
}

ManagerNamedApplyCoordinator::ManagerNamedApplyCoordinator(
    SqliteManagerApplyJournal &apply_journal,
    IManagerActiveRuntimePlanSource &active_plan_source,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    core::IActiveBundlePublicationStore &publication_store,
    core::IManagerSceneProjectionMaintenancePort &scene_owner,
    core::IManagerSceneProjectionBootstrapPort *bootstrap_scene_owner,
    core::IManagerKernelApplyFencePort &kernel_fence,
    core::IKernelCellAdmissionPort &cell_admission,
    IManagerSelectedRuntimeCompositionPort &runtime_composition,
    IRuntimeUnitProcessPort &process_port,
    IRuntimeUnitProgressPort &progress_port,
    ManagerPlannedMaintenanceCoordinator &planned_maintenance,
    core::RuntimeEpoch manager_runtime_epoch, std::string operating_system,
    std::vector<std::string> launch_environment_contracts,
    RuntimeObservationReducer::Clock clock_ns, ApplyIdFactory apply_id_factory,
    InitialSceneSource initial_scene_source,
    RuntimePreStartGuard pre_start_guard,
    ApplyProgressObserver apply_progress_observer)
    : apply_journal_(apply_journal), active_plan_source_(active_plan_source),
      retained_bundle_source_(retained_bundle_source),
      artifact_source_(artifact_source), publication_reader_(publication_store),
      scene_owner_(scene_owner), kernel_fence_(kernel_fence),
      cell_admission_(cell_admission),
      runtime_composition_port_(runtime_composition),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)),
      apply_id_factory_(std::move(apply_id_factory)),
      initial_scene_source_(std::move(initial_scene_source)),
      apply_progress_observer_(std::move(apply_progress_observer)),
      fence_coordinator_(apply_journal_, kernel_fence_, manager_runtime_epoch_),
      planned_maintenance_(planned_maintenance),
      active_runtime_coordinator_(active_plan_source, process_port,
                                  progress_port, clock_ns, pre_start_guard),
      runtime_coordinator_(apply_journal_, publication_store, artifact_source,
                           process_port, progress_port,
                           std::move(operating_system),
                           std::move(launch_environment_contracts),
                           std::move(clock_ns), std::move(pre_start_guard)),
      scene_initialization_coordinator_(apply_journal_, scene_owner),
      runtime_composition_coordinator_(apply_journal_, scene_owner,
                                       runtime_composition,
                                       manager_runtime_epoch_),
      selected_bundle_coordinator_(apply_journal_, publication_store,
                                   scene_owner),
      fence_release_coordinator_(apply_journal_, kernel_fence_, scene_owner,
                                 manager_runtime_epoch_) {
  static_cast<void>(bootstrap_scene_owner);
  Require(!manager_runtime_epoch_.empty(),
          "Manager runtime epoch cannot be empty");
  Require(static_cast<bool>(apply_id_factory_), "Apply ID factory is required");
  Require(static_cast<bool>(initial_scene_source_),
          "Apply initial-Scene source is required");
}

core::ApplyId ManagerNamedApplyCoordinator::MintApplyId() {
  for (std::size_t attempt = 0U; attempt < kMaximumApplyIdMintAttempts;
       ++attempt) {
    auto apply_id = apply_id_factory_();
    if (!apply_id.empty() &&
        !apply_journal_.ReadApplyGenerationHighWater(apply_id).has_value()) {
      return apply_id;
    }
  }
  Fail("Apply ID factory did not produce a fresh bounded identity");
}

bool ManagerNamedApplyCoordinator::HasOriginalBringUpCapture(
    const ManagerApplyIntentDTO &intent) const {
  if (!captured_apply_inputs_ || !captured_apply_inputs_->bring_up_plan ||
      !captured_apply_inputs_->original_parent_claim ||
      !captured_apply_inputs_->original_predecessor_publication ||
      captured_apply_inputs_->apply_id != intent.apply_id ||
      captured_apply_inputs_->apply_generation != intent.apply_generation ||
      !captured_apply_inputs_->predecessor || !captured_apply_inputs_->candidate) return false;
  const auto &captured = *captured_apply_inputs_;
  const auto stored = apply_journal_.ReadIntent(intent.apply_id, intent.apply_generation);
  const auto plan = apply_journal_.ReadBringUpPlan(intent.apply_id, intent.apply_generation);
  Require(stored && stored->intent == intent && plan == captured.bring_up_plan &&
              plan->plan.parent_intent_digest == stored->intent_digest &&
              captured.predecessor->retained_bundle_id() == intent.expected_active_projection_revision.retained_bundle_id &&
              captured.candidate->retained_bundle_id() == intent.candidate_retained_bundle_id &&
              captured.original_parent_claim->intent == DirectAdmissionIntent(*stored, plan->plan_digest) &&
              captured.original_predecessor_publication->valid() &&
              core::SameActiveDeployment(captured.original_predecessor_publication->request.candidate_projection_revision,
                                         intent.expected_active_projection_revision) &&
              captured.original_predecessor_publication->publication_generation ==
                  plan->plan.expected_publication.expected_previous_publication_generation &&
              captured.original_predecessor_publication->durable_public_pointer_content_digest ==
                  plan->plan.predecessor_public_pointer_digest,
          "original configured parent capture differs from its immutable request/plan");
  return true;
}

core::ApplyFenceAcquireAttemptQueryDTO
ManagerNamedApplyCoordinator::CaptureOriginalSceneStagingAttempt(const ManagerApplyIntentDTO &intent) {
  Require(HasOriginalBringUpCapture(intent), "Scene staging requires the original linked capture");
  const auto status = apply_journal_.ReadStatus(intent.apply_id, intent.apply_generation);
  Require(status && status->fence_attempt && status->fence_acceptance &&
              status->fence_acceptance->fence_attempt_digest == status->fence_attempt->fence_attempt_digest,
          "Scene staging lacks the exact persisted original fence attempt/acceptance");
  const core::ApplyFenceAcquireAttemptQueryDTO query{
      status->fence_attempt->fence_intent, status->fence_attempt->fence_attempt_digest,
      manager_runtime_epoch_};
  Require(query.valid(), "Scene staging has an invalid original acquire-attempt query");
  return query;
}

core::QualifiedSceneProductAdoptionRequestDTO
ManagerNamedApplyCoordinator::CaptureOriginalSceneAdoption(
    const ManagerApplyIntentDTO &intent,
    const std::optional<core::ApplyFenceAcquireAttemptQueryDTO> &staging_attempt) {
  Require(HasOriginalBringUpCapture(intent), "Scene adoption requires the original linked capture");
  const auto &captured = *captured_apply_inputs_;
  const auto status = apply_journal_.ReadStatus(intent.apply_id, intent.apply_generation);
  Require(status && status->fence_acceptance && status->latest_runtime_staged_ready &&
              status->bring_up_plan == captured.bring_up_plan,
          "Scene adoption lacks exact original parent, fence and staged host evidence");
  const auto claim = RequireHeldAdmission(status->intent, cell_admission_,
      captured.bring_up_plan->plan_digest, captured.original_parent_claim);
  const auto &fence = status->fence_acceptance->accepted_fence;
  const core::ConfigurationFenceStatusQueryDTO query{
      {intent.apply_id, intent.apply_generation}, fence.configuration_fence_id,
      fence.configuration_fence_generation, fence.captured_scope, manager_runtime_epoch_};
  if (staging_attempt) {
    Require(status->fence_attempt &&
                staging_attempt->persisted_intent == status->fence_attempt->fence_intent &&
                staging_attempt->persisted_attempt_digest == status->fence_attempt->fence_attempt_digest &&
                status->fence_acceptance->fence_attempt_digest == staging_attempt->persisted_attempt_digest &&
                staging_attempt->manager_runtime_epoch == manager_runtime_epoch_ &&
                staging_attempt->persisted_intent.owner_request_context == intent.request_context &&
                staging_attempt->persisted_intent.owner_transaction == query.owner_transaction &&
                staging_attempt->persisted_intent.scope == query.expected_scope &&
                staging_attempt->persisted_intent.expected_retained_bundle_id ==
                    intent.expected_active_projection_revision.retained_bundle_id &&
                staging_attempt->persisted_intent.expected_graph_revision ==
                    intent.expected_active_projection_revision.graph_revision,
            "Scene staging acquire-attempt query differs from the original persisted parent");
    // Read the existing producer directly. This never enters Manager's lost
    // Acquire wrapper, retries effects, appends acceptance or rehydrates.
    const auto held = kernel_fence_.ReconcileAcquireAttempt(*staging_attempt);
    Require(held.matches(*staging_attempt) &&
                held.state == core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact &&
                held.admission_fenced && held.accepted_fence == fence &&
                held.execution_host_epoch.publication_generation ==
                    captured.original_predecessor_publication->publication_generation &&
                std::any_of(status->latest_runtime_staged_ready->staged_ready.runtime_units.begin(),
                            status->latest_runtime_staged_ready->staged_ready.runtime_units.end(),
                            [&](const auto &runtime) {
                              return runtime.valid() && runtime.runtime_unit_id == held.execution_host_epoch.runtime_unit_id &&
                                  runtime.runtime_epoch == held.execution_host_epoch.runtime_epoch;
                            }),
            "Scene staging original acquire attempt is not actually held by the staged host");
  } else {
    const auto held = kernel_fence_.QueryStatus(query);
    Require(held.matches(query) && held.state == core::ConfigurationFenceState::kHeld && held.admission_fenced,
            "Scene adoption parent fence is not actually held exactly");
  }
  const core::ApplyComponentBringUpAuthorityDTO authority{
      {{intent.apply_id, intent.apply_generation}, captured.bring_up_plan->plan_digest}, claim, query};
  Require(authority.valid(), "Scene adoption has invalid original parent authority");
  return {authority, *captured.original_predecessor_publication, intent.expected_active_projection_revision,
      {intent.expected_active_projection_revision.scene_revision, intent.cell_id}, captured.candidate->value(),
      detail::DeriveCandidateNativeProjectionBindings(captured.candidate->value(),
          status->latest_runtime_staged_ready->staged_ready)};
}

bool ManagerNamedApplyCoordinator::HasOriginalConfigurationCapture(
    const ManagerApplyIntentDTO &intent) const {
  if (!captured_apply_inputs_ || captured_apply_inputs_->bring_up_plan ||
      !captured_apply_inputs_->original_parent_claim ||
      !captured_apply_inputs_->original_predecessor_publication ||
      captured_apply_inputs_->apply_id != intent.apply_id ||
      captured_apply_inputs_->apply_generation != intent.apply_generation ||
      !captured_apply_inputs_->predecessor || !captured_apply_inputs_->candidate)
    return false;
  const auto stored = apply_journal_.ReadIntent(intent.apply_id, intent.apply_generation);
  const auto &captured = *captured_apply_inputs_;
  Require(stored && stored->intent == intent &&
              !apply_journal_.ReadBringUpPlan(intent.apply_id, intent.apply_generation) &&
              captured.predecessor->retained_bundle_id() == intent.expected_active_projection_revision.retained_bundle_id &&
              captured.candidate->retained_bundle_id() == intent.candidate_retained_bundle_id &&
              captured.original_predecessor_publication->valid() &&
              core::SameActiveDeployment(captured.original_predecessor_publication->request.candidate_projection_revision,
                                         intent.expected_active_projection_revision),
          "configuration capture differs from its original immutable request/head");
  if (intent.parent_authoring_claim) {
    Require(captured.original_parent_claim == intent.parent_authoring_claim,
            "configuration capture differs from its original authoring parent");
  } else {
    Require(captured.original_parent_claim->intent == DirectAdmissionIntent(*stored),
            "configuration capture differs from its original direct Manager claim");
  }
  return true;
}

core::QualifiedSceneConfigurationAdoptionRequestDTO
ManagerNamedApplyCoordinator::CaptureOriginalConfigurationAdoption(
    const ManagerApplyIntentDTO &intent,
    const std::optional<core::ApplyFenceAcquireAttemptQueryDTO> &staging_attempt) {
  Require(HasOriginalConfigurationCapture(intent),
          "configuration Scene adoption requires the original process capture");
  const auto &captured = *captured_apply_inputs_;
  const auto status = apply_journal_.ReadStatus(intent.apply_id, intent.apply_generation);
  Require(status && status->fence_attempt && status->fence_acceptance &&
              status->latest_runtime_staged_ready && !status->bring_up_plan &&
              status->intent.intent == intent &&
              status->fence_acceptance->fence_attempt_digest == status->fence_attempt->fence_attempt_digest,
          "configuration Scene adoption lacks exact stored parent/fence/host evidence");
  const auto &claim = *captured.original_parent_claim;
  const core::CellAdmissionRecoveryReceiptQueryDTO claim_query{claim.intent, claim, std::nullopt};
  const auto claim_receipt = cell_admission_.GetCellAdmissionRecoveryReceipt(claim_query);
  Require(core::ValidateCellAdmissionRecoveryReceiptQuery(claim_query).ok() && claim_receipt.ok() &&
              core::ValidateCellAdmissionRecoveryReceipt(claim_receipt.value()).ok() &&
              claim_receipt.value().status == core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
              claim_receipt.value().matched_claim == claim &&
              !claim_receipt.value().matched_release && !claim_receipt.value().matched_reconcile_settlement,
          "configuration original claim is no longer held exactly");
  const auto &fence = status->fence_acceptance->accepted_fence;
  const core::ConfigurationFenceStatusQueryDTO query{
      {intent.apply_id, intent.apply_generation}, fence.configuration_fence_id,
      fence.configuration_fence_generation, fence.captured_scope, manager_runtime_epoch_};
  Require(query.valid(), "configuration parent fence query is invalid");
  if (staging_attempt) {
    Require(staging_attempt->valid() &&
                staging_attempt->persisted_intent == status->fence_attempt->fence_intent &&
                staging_attempt->persisted_attempt_digest == status->fence_attempt->fence_attempt_digest &&
                staging_attempt->manager_runtime_epoch == manager_runtime_epoch_ &&
                staging_attempt->persisted_intent.owner_request_context == intent.request_context &&
                staging_attempt->persisted_intent.owner_transaction == query.owner_transaction &&
                staging_attempt->persisted_intent.scope == query.expected_scope,
            "configuration staging query differs from its original stored attempt");
    // Observe the real ledger directly, without Acquire retry/persistence or
    // candidate-installed release reconciliation.
    const auto held = kernel_fence_.ReconcileAcquireAttempt(*staging_attempt);
    Require(held.matches(*staging_attempt) &&
                held.state == core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact &&
                held.admission_fenced && held.accepted_fence == fence &&
                held.execution_host_epoch.publication_generation ==
                    captured.original_predecessor_publication->publication_generation &&
                std::any_of(status->latest_runtime_staged_ready->staged_ready.runtime_units.begin(),
                            status->latest_runtime_staged_ready->staged_ready.runtime_units.end(),
                            [&](const auto &runtime) {
                              return runtime.valid() && runtime.runtime_unit_id == held.execution_host_epoch.runtime_unit_id &&
                                  runtime.runtime_epoch == held.execution_host_epoch.runtime_epoch;
                            }),
            "configuration staging fence is not actually held by the staged host");
  } else {
    const auto held = kernel_fence_.QueryStatus(query);
    Require(held.matches(query) && held.state == core::ConfigurationFenceState::kHeld && held.admission_fenced,
            "configuration Scene adoption fence is no longer held exactly");
  }
  return {{{intent.apply_id, intent.apply_generation}, status->intent.intent_digest, claim, query},
      *captured.original_predecessor_publication, intent.expected_active_projection_revision,
      {intent.expected_active_projection_revision.scene_revision, intent.cell_id}, captured.candidate->value(),
      detail::DeriveCandidateNativeProjectionBindings(captured.candidate->value(),
          status->latest_runtime_staged_ready->staged_ready)};
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::ContinueOriginalConfiguration(
    const PersistedManagerApplyIntent &durable, const bool request_replayed) {
  const auto &intent = durable.intent;
  Require(HasOriginalConfigurationCapture(intent), "configuration lost its original capture");
  auto &captured = *captured_apply_inputs_;
  const auto pending = [&] { return ManagerNamedApplyResult{intent.apply_id, intent.apply_generation,
      ManagerNamedApplyResultState::kPending, request_replayed, std::nullopt}; };
  try {
    const auto status = apply_journal_.ReadStatus(intent.apply_id, intent.apply_generation);
    Require(status && status->fence_attempt && status->fence_acceptance,
            "configuration staging lacks original fence acceptance");
    const core::ApplyFenceAcquireAttemptQueryDTO attempt{
        status->fence_attempt->fence_intent, status->fence_attempt->fence_attempt_digest, manager_runtime_epoch_};
    const core::ProjectionRevisionDTO target{intent.candidate_retained_bundle_id,
        intent.candidate_graph_revision, intent.expected_active_projection_revision.scene_revision};
    if (!captured.original_preparation_completed) {
      const auto request = CaptureOriginalConfigurationAdoption(intent, attempt);
      const auto before = publication_reader_.ReadActiveBundlePublication(
          request.expected_predecessor_publication.publication_generation);
      Require(before.ok() && before.value() == request.expected_predecessor_publication,
              "configuration staging predecessor is not the current head");
      std::vector<core::ProjectionHealthDTO> health;
      if (!captured.original_scene_stage_consumed) {
        captured.original_configuration_adoption = request;
        captured.original_scene_stage_consumed = true;
        health = scene_owner_.StageOriginalManagerConfigurationAdoption(request, attempt);
        captured.original_scene_stage_completed = true;
      } else {
        Require(captured.original_scene_stage_completed && captured.original_configuration_adoption == request,
                "configuration staging has an uncertain outcome or changed identity");
        health = scene_owner_.ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, attempt);
      }
      Require(CaptureOriginalConfigurationAdoption(intent, attempt) == request &&
                  health.size() == request.native_runtime_bindings.size(),
              "configuration staging changed identity or returned incomplete participants");
      const auto after = publication_reader_.ReadActiveBundlePublication(
          request.expected_predecessor_publication.publication_generation);
      Require(after.ok() && after.value() == request.expected_predecessor_publication,
              "configuration staging changed the public head");
      bool ready = true;
      for (std::size_t index = 0; index < health.size(); ++index) {
        const auto &row = health[index];
        Require(row.valid() && row.native_runtime_binding == request.native_runtime_bindings[index] && !row.error,
                "configuration staging returned foreign or failed native evidence");
        if (!row.healthy_for_target(target)) {
          Require(row.state == core::ProjectionHealthState::kRebuilding ||
                      row.state == core::ProjectionHealthState::kUninitialized,
                  "configuration staging returned unavailable native evidence");
          ready = false;
        }
      }
      if (!ready) return pending();
      const auto composition = runtime_composition_coordinator_.PrepareOriginalConfiguration(
          request, *captured.candidate);
      if (composition.state == ManagerSelectedRuntimeCompositionResultState::kPending) return pending();
      if (captured.original_prepare_acknowledgement && !captured.prepare_acknowledgement_consumed) {
        captured.prepare_acknowledgement_consumed = true;
        captured.original_prepare_acknowledgement(intent, *captured.candidate, composition.owner_result);
      }
      captured.original_preparation_completed = true;
    }
    if (!captured.original_scene_begin_consumed) {
      const auto request = CaptureOriginalConfigurationAdoption(intent);
      Require(captured.original_scene_stage_completed && captured.original_configuration_adoption == request,
              "configuration Begin differs from its original staged identity");
      captured.original_scene_begin_consumed = true;
      const auto begun = scene_owner_.BeginOriginalManagerConfigurationAdoption(request);
      Require(begun.valid() && begun.apply_id == intent.apply_id && begun.apply_generation == intent.apply_generation &&
                  begun.state == core::ProjectionMaintenanceInitializationState::kInitialized &&
                  begun.ready_for_publication() && !begun.publication_receipt && !begun.effects_unknown && !begun.first_error &&
                  begun.predecessor_projection_revision == intent.expected_active_projection_revision &&
                  begun.target_projection_revision == target &&
                  begun.authoritative_snapshot_identity == request.expected_snapshot_identity &&
                  begun.native_runtime_bindings == request.native_runtime_bindings &&
                  CaptureOriginalConfigurationAdoption(intent) == request,
              "configuration Begin returned foreign or incomplete preservation evidence");
    }
    const auto composition = runtime_composition_coordinator_.InstallOrRecover(
        intent.apply_id, intent.apply_generation, *captured.candidate);
    if (composition.state == ManagerSelectedRuntimeCompositionResultState::kPending) return pending();
    static_cast<void>(selected_bundle_coordinator_.PublishOriginalConfiguration(
        intent.apply_id, intent.apply_generation, *captured.predecessor, *captured.candidate));
    return FinishOriginalConfigurationPublication(durable, request_replayed);
  } catch (const std::exception &error) {
    if (apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation)) throw;
    return RetainTerminalFailure(apply_journal_, durable, "configuration_preservation_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::FinishOriginalConfigurationPublication(
    const PersistedManagerApplyIntent &durable, const bool request_replayed) {
  const auto &intent = durable.intent;
  Require(HasOriginalConfigurationCapture(intent), "published configuration lost its original capture");
  try {
    auto &captured = *captured_apply_inputs_;
    Require(captured.original_scene_begin_consumed && captured.original_configuration_adoption,
            "configuration publication has no original preserved Scene operation");
    const auto publication = apply_journal_.ReadPublicationReceipt(intent.apply_id, intent.apply_generation);
    Require(publication.has_value(), "configuration publication has no durable receipt");
    const auto &receipt = publication->publication.publication_receipt;
    const core::ProjectionRevisionDTO target{intent.candidate_retained_bundle_id,
        intent.candidate_graph_revision, intent.expected_active_projection_revision.scene_revision};
    Require(receipt.matches_named_apply(intent.apply_id, intent.apply_generation, target) &&
                receipt.request.expected_previous_publication_generation ==
                    captured.original_predecessor_publication->publication_generation,
            "configuration publication differs from the original candidate/head");
    const auto head = publication_reader_.ReadActiveBundlePublication(receipt.publication_generation);
    Require(head.ok() && head.value() == receipt, "configuration receipt is not the actual current head");
    if (!captured.original_scene_complete_consumed) {
      Require(CaptureOriginalConfigurationAdoption(intent) == *captured.original_configuration_adoption,
              "configuration identity changed before Scene Complete");
      captured.original_scene_complete_consumed = true;
      const auto completed = scene_owner_.CompleteOriginalManagerConfigurationAdoption(
          *captured.original_configuration_adoption, receipt);
      Require(completed.valid() && completed.apply_id == intent.apply_id && completed.apply_generation == intent.apply_generation &&
                  completed.state == core::ProjectionMaintenanceInitializationState::kCompleted &&
                  completed.publication_receipt == receipt && completed.initial_scene_installed &&
                  !completed.effects_unknown && !completed.first_error &&
                  completed.predecessor_projection_revision == intent.expected_active_projection_revision &&
                  completed.target_projection_revision == target &&
                  completed.authoritative_snapshot_identity == captured.original_configuration_adoption->expected_snapshot_identity &&
                  completed.native_runtime_bindings == captured.original_configuration_adoption->native_runtime_bindings &&
                  CaptureOriginalConfigurationAdoption(intent) == *captured.original_configuration_adoption,
              "configuration Scene Complete returned foreign or incomplete preservation evidence");
      const auto after = publication_reader_.ReadActiveBundlePublication(receipt.publication_generation);
      Require(after.ok() && after.value() == receipt, "configuration head changed across Scene Complete");
      captured.original_scene_completed = completed;
    }
    Require(captured.original_scene_completed.has_value(), "configuration Scene completion outcome is uncertain");
    const auto composition = runtime_composition_coordinator_.InstallOrRecover(
        intent.apply_id, intent.apply_generation, *captured.candidate);
    if (composition.state == ManagerSelectedRuntimeCompositionResultState::kPending)
      return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending, request_replayed, std::nullopt};
    const auto released = fence_release_coordinator_.ReleaseOrRecover(intent.apply_id, intent.apply_generation);
    const auto terminal = apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation);
    Require(terminal.has_value(), "configuration release lacks its actual terminal evidence");
    ReleaseAdmission(durable, *terminal, cell_admission_);
    return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kSucceeded,
        request_replayed, released.durable_terminal_outcome.record.terminal_outcome};
  } catch (const std::exception &error) {
    if (apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation)) throw;
    return RetainTerminalFailure(apply_journal_, durable, "configuration_settlement_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }
}

bool ManagerNamedApplyCoordinator::AdvanceOriginalBringUpChildren(
    const ManagerApplyIntentDTO &intent, const bool after_publication) {
  Require(HasOriginalBringUpCapture(intent), "configured children require this process's original capture");
  auto &captured = *captured_apply_inputs_;
  const auto &plan = *captured.bring_up_plan;
  const auto durable = apply_journal_.ReadIntent(intent.apply_id, intent.apply_generation);
  const auto claim = RequireHeldAdmission(*durable, cell_admission_, plan.plan_digest,
                                         captured.original_parent_claim);
  const auto fence = apply_journal_.ReadFenceAcceptance(intent.apply_id, intent.apply_generation);
  Require(fence.has_value(), "configured children lack actual parent fence acceptance");
  const core::ConfigurationFenceStatusQueryDTO query{
      {intent.apply_id, intent.apply_generation}, fence->accepted_fence.configuration_fence_id,
      fence->accepted_fence.configuration_fence_generation, fence->accepted_fence.captured_scope,
      manager_runtime_epoch_};
  const auto held = kernel_fence_.QueryStatus(query);
  Require(held.matches(query) && held.state == core::ConfigurationFenceState::kHeld && held.admission_fenced,
          "configured children parent fence is not actually held exactly");
  const core::ApplyComponentBringUpAuthorityDTO authority{
      {{intent.apply_id, intent.apply_generation}, plan.plan_digest}, claim, query};
  Require(authority.valid(), "configured children lack exact full parent authority");
  const auto request_for = [&](const ManagerApplyBringUpChildRequestDTO &child) {
    ManagerApplyComponentBringUpRequestDTO request{child.intent, child.candidate_projection_revision,
        child.candidate_retained_bundle_digest, child.candidate_retained_bundle_bytes, child.runtime_unit_id,
        child.runtime_epoch, child.manager_runtime_epoch, authority};
    Require(request.valid(), "original child cannot form its exact typed request");
    return request;
  };
  const auto read = [&](const ManagerApplyComponentBringUpRequestDTO &request) {
    auto value = runtime_composition_coordinator_.ReadApplyComponentBringUp({request});
    if (!value.ok()) Fail("configured child READ failed: " + value.error().message);
    return value.value().result;
  };
  const auto record = [&](const ManagerApplyBringUpChildRequestDTO &child,
                          const ManagerApplyBringUpChildPhase phase,
                          const core::CCLCLifecycleRequestResultDTO &result) {
    static_cast<void>(apply_journal_.EnsureBringUpChildProof({intent.apply_id, intent.apply_generation,
        plan.plan_digest, child.intent.controllable_component_id, phase, result}));
  };
  const auto require_healthy = [&](const ManagerApplyBringUpChildRequestDTO &child,
                                   const core::CCLCLifecycleRequestResultDTO &result) {
    Require(result.locally_valid(), "configured child returned invalid owner data");
    if (result.rejection || !result.status) Fail("configured child has no accepted owner status");
    const auto &status = *result.status;
    if (status.reconciliation_required || status.error ||
        status.phase == core::ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown ||
        (status.terminal_result && *status.terminal_result != core::ComponentLifecycleTerminalResultDTO::kSucceeded)) {
      if (status.terminal_result || status.phase == core::ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown) {
        if (!apply_journal_.ReadBringUpChildProof(intent.apply_id, intent.apply_generation,
                child.intent.controllable_component_id, ManagerApplyBringUpChildPhase::kTerminal))
          record(child, ManagerApplyBringUpChildPhase::kTerminal, result);
      }
      Fail("configured child failed or requires owner reconciliation");
    }
    Require(status.status_owner_epoch == child.intent.request_context.request_owner_epoch,
            "configured child observation belongs to a replacement owner");
    Require(std::none_of(status.domains.begin(), status.domains.end(), [](const auto &domain) {
              return domain.error.has_value(); }), "configured child domain failed");
  };
  const auto holding = [](const core::ComponentLifecycleStatusDTO &status) {
    return status.phase == core::ComponentLifecycleTransactionPhaseDTO::kVerifying &&
        !status.terminal_result && !status.reconciliation_required && !status.error &&
        status.requested_state.presence == core::ComponentLifecycleRequestedStatePresenceDTO::kResolved &&
        status.requested_state.state && status.current_state == status.requested_state.state &&
        std::all_of(status.domains.begin(), status.domains.end(), [](const auto &domain) {
          if (domain.error) return false;
          if (!domain.expected_domain) return domain.effect == core::ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
          return domain.action_state == core::ComponentLifecycleDomainActionStateDTO::kSucceeded &&
              domain.current_scope && domain.resolved_provider_occurrence &&
              domain.requested_state.presence == core::ComponentLifecycleRequestedStatePresenceDTO::kResolved &&
              domain.requested_state.state && domain.current_state == domain.requested_state.state;
        });
  };
  // Preserve real owner acceptance for every needed row before any SUBMIT.
  for (const auto &child : plan.plan.children) {
    if (apply_journal_.ReadBringUpChildProof(intent.apply_id, intent.apply_generation,
            child.intent.controllable_component_id, ManagerApplyBringUpChildPhase::kAccepted)) continue;
    Require(!after_publication, "published configured child has no original accepted proof");
    const auto result = read(request_for(child));
    if (!result) return false;
    record(child, ManagerApplyBringUpChildPhase::kAccepted, *result);
    require_healthy(child, *result);
  }
  for (const auto &child : plan.plan.children) {
    const auto request = request_for(child);
    std::optional<core::CCLCLifecycleRequestResultDTO> result;
    const auto submitted = std::find(captured.submitted_children.begin(), captured.submitted_children.end(),
                                     child.intent.controllable_component_id);
    if (!after_publication && submitted == captured.submitted_children.end()) {
      // Consume before I/O. Unknown sends/replies cannot authorize a resend.
      captured.submitted_children.push_back(child.intent.controllable_component_id);
      auto value = runtime_composition_coordinator_.SubmitApplyComponentBringUp(request);
      if (!value.ok()) Fail("configured child SUBMIT failed: " + value.error().message);
      result = value.value().result;
    } else {
      result = read(request);
    }
    if (!result) return false;
    require_healthy(child, *result);
    const auto &status = *result->status;
    if (after_publication) {
      if (status.phase != core::ComponentLifecycleTransactionPhaseDTO::kSucceeded ||
          status.terminal_result != core::ComponentLifecycleTerminalResultDTO::kSucceeded) return false;
      if (!apply_journal_.ReadBringUpChildProof(intent.apply_id, intent.apply_generation,
              child.intent.controllable_component_id, ManagerApplyBringUpChildPhase::kTerminal))
        record(child, ManagerApplyBringUpChildPhase::kTerminal, *result);
    } else {
      Require(!status.terminal_result, "configured child reported terminal success before parent publication");
      if (!holding(status)) return false;
      if (!apply_journal_.ReadBringUpChildProof(intent.apply_id, intent.apply_generation,
              child.intent.controllable_component_id, ManagerApplyBringUpChildPhase::kHolding))
        record(child, ManagerApplyBringUpChildPhase::kHolding, *result);
    }
  }
  return true;
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::FinishOriginalBringUpPublication(
    const PersistedManagerApplyIntent &durable, const bool request_replayed) {
  const auto &intent = durable.intent;
  Require(HasOriginalBringUpCapture(intent), "published configured parent lost original capture");
  try {
    auto &captured = *captured_apply_inputs_;
    Require(captured.original_scene_begin_consumed && captured.original_scene_adoption,
            "published parent has no original Scene operation");
    const auto publication = apply_journal_.ReadPublicationReceipt(intent.apply_id, intent.apply_generation);
    Require(publication && publication->publication.publication_receipt.request ==
                captured.bring_up_plan->plan.expected_publication,
            "original Scene completion lacks the exact sealed parent publication");
    const auto &receipt = publication->publication.publication_receipt;
    const auto head = publication_reader_.ReadActiveBundlePublication(receipt.publication_generation);
    Require(head.ok() && head.value() == receipt,
            "original Scene completion publication is not the actual current head");
    if (!captured.original_scene_complete_consumed) {
      Require(CaptureOriginalSceneAdoption(intent) == *captured.original_scene_adoption,
              "original parent identity changed before Scene Complete");
      captured.original_scene_complete_consumed = true;
      const auto completed = scene_owner_.CompleteOriginalManagerProductAdoption(
          *captured.original_scene_adoption, receipt);
      Require(completed.valid() && completed.apply_id == intent.apply_id &&
                  completed.apply_generation == intent.apply_generation && completed.state == core::ProjectionMaintenanceInitializationState::kCompleted &&
                  completed.publication_receipt == receipt && completed.initial_scene_installed &&
                  !completed.effects_unknown && !completed.first_error &&
                  completed.predecessor_projection_revision == intent.expected_active_projection_revision &&
                  completed.target_projection_revision == receipt.request.candidate_projection_revision &&
                  completed.authoritative_snapshot_identity == captured.original_scene_adoption->expected_snapshot_identity &&
                  completed.native_runtime_bindings == captured.original_scene_adoption->native_runtime_bindings,
              "original Scene Complete returned missing or foreign evidence");
      Require(CaptureOriginalSceneAdoption(intent) == *captured.original_scene_adoption,
              "original parent identity changed across Scene Complete");
      const auto after = publication_reader_.ReadActiveBundlePublication(receipt.publication_generation);
      Require(after.ok() && after.value() == receipt,
              "original publication identity changed across Scene Complete");
      captured.original_scene_completed = completed;
    }
    Require(captured.original_scene_completed.has_value(), "original Scene completion outcome is uncertain");
    if (!AdvanceOriginalBringUpChildren(intent, true))
      return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
              request_replayed, std::nullopt};
    const auto composition = runtime_composition_coordinator_.InstallOrRecover(
        intent.apply_id, intent.apply_generation, *captured_apply_inputs_->candidate);
    if (composition.state == ManagerSelectedRuntimeCompositionResultState::kPending)
      return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
              request_replayed, std::nullopt};
    const auto released = fence_release_coordinator_.ReleaseOrRecover(intent.apply_id, intent.apply_generation);
    const auto terminal = apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation);
    Require(terminal.has_value(), "configured parent fence release lacks actual terminal proof");
    ReleaseAdmission(durable, *terminal, cell_admission_, captured_apply_inputs_->bring_up_plan->plan_digest,
                     captured_apply_inputs_->original_parent_claim);
    return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kSucceeded,
            request_replayed, released.durable_terminal_outcome.record.terminal_outcome};
  } catch (const std::exception &error) {
    // Fence release can already have recorded the actual successful parent
    // terminal. A later claim-release refusal must not overwrite that fact.
    if (apply_journal_.ReadTerminalOutcome(intent.apply_id, intent.apply_generation)) throw;
    return RetainTerminalFailure(apply_journal_, durable, "apply_component_bring_up_settlement_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }
}

std::optional<ManagerNamedApplyResult>
ManagerNamedApplyCoordinator::ObserveBringUpParent(
    const ManagerApplyIntentDTO &intent, const bool request_replayed) const {
  const auto plan =
      apply_journal_.ReadBringUpPlan(intent.apply_id, intent.apply_generation);
  if (!plan.has_value())
    return std::nullopt;
  if (HasOriginalBringUpCapture(intent)) return std::nullopt;
  // The journal's complete status read verifies every retained child proof
  // against this exact immutable plan and the original request/fence facts.
  // Those facts are observations, never reconstructed current authority.
  const auto status =
      apply_journal_.ReadStatus(intent.apply_id, intent.apply_generation);
  Require(status.has_value() && status->intent.intent == intent &&
              status->bring_up_plan == plan &&
              plan->plan.parent_intent_digest == status->intent.intent_digest,
          "linked bring-up parent does not match its exact stored request");
  const auto &terminal = status->terminal_outcome;
  // A child failure/unknown proof does not manufacture a parent terminal.
  // Until the real parent result exists, preserve the unresolved observation
  // and all exclusions for explicit owner reconciliation.
  return ManagerNamedApplyResult{
      intent.apply_id, intent.apply_generation,
      terminal.has_value() ? ResultStateFromTerminal(terminal->terminal_outcome)
                           : ManagerNamedApplyResultState::kPending,
      request_replayed,
      terminal.has_value() ? std::optional(terminal->terminal_outcome)
                           : std::nullopt};
}

ManagerNamedApplyResult ManagerNamedApplyCoordinator::AcceptOrReplay(
    const ManagerNamedApplyRequestDTO &request,
    OriginalBringUpPlanSource original_bring_up_plan_source,
    OriginalPrepareAcknowledgement original_prepare_acknowledgement) {
  std::lock_guard lock(operation_mutex_);
  Require(request.valid(), "named Apply request is invalid");

  if (const auto existing = apply_journal_.ReadRequest(
          request.request_context.request_owner_epoch,
          request.request_context.request_id);
      existing.has_value()) {
    Require(MatchesRequest(existing->intent, request),
            "public request identity is already bound to another Apply");
    if (const auto observation = ObserveBringUpParent(existing->intent, true);
        observation.has_value())
      return *observation;
    const auto terminal = apply_journal_.ReadTerminalOutcome(
        existing->intent.apply_id, existing->intent.apply_generation);
    return {existing->intent.apply_id, existing->intent.apply_generation,
            terminal.has_value()
                ? ResultStateFromTerminal(terminal->terminal_outcome)
                : ManagerNamedApplyResultState::kPending,
            true,
            terminal.has_value() ? std::optional(terminal->terminal_outcome)
                                 : std::nullopt};
  }

  const auto candidate =
      LoadBundle(retained_bundle_source_, request.candidate_retained_bundle_id,
                 "candidate");
  const auto &candidate_value = candidate->value();
  Require(candidate_value.deployment_artifact_id ==
                  request.deployment_artifact_id &&
              candidate_value.deployment_revision ==
                  request.deployment_revision,
          "selected candidate does not match the requested deployment "
          "identity and revision");
  Require(candidate_value.cell_graph.cell_id == apply_journal_.cell_id(),
          "selected candidate belongs to another cell");
  // Configuration Apply preserves the current logical Scene. Program Load
  // owns authored scene changes; this operation cannot install a baseline.
  auto initial_scene = std::vector<core::AddObjectRequestDTO>{};
  Require(core::ValidInitialSceneObjects(initial_scene),
          "captured initial Scene has invalid geometry, pose, or ordering");

  const auto inventory = apply_journal_.ReadStatusInventory();
  for (const auto &status : inventory) {
    if (!IsTerminal(status)) {
      throw ManagerNamedApplyBusyError(
          "another named Apply remains nonterminal on this Manager lane");
    }
  }

  const auto active_plan = active_plan_source_.ReadCurrentPlan();
  Require(active_plan.valid(), "current active runtime plan is invalid");
  const auto &active_revision =
      active_plan.active_publication.request.candidate_projection_revision;
  Require(active_revision.retained_bundle_id ==
                  request.expected_current_retained_bundle_id &&
              active_revision.graph_revision ==
                  request.expected_current_graph_revision,
          "named Apply expected_current does not match the active pointer");

  const auto predecessor =
      LoadBundle(retained_bundle_source_, active_revision.retained_bundle_id,
                 "predecessor");
  Require(predecessor->value().cell_graph.cell_id == apply_journal_.cell_id() &&
              predecessor->value().cell_graph.graph_revision ==
                  active_revision.graph_revision,
          "selected predecessor does not match the active cell graph");

  const auto scene_revision = scene_owner_.CurrentProjectionRevision();
  Require(scene_revision.valid() &&
              SameDeploymentRevision(scene_revision, active_revision),
          "logical-scene predecessor does not match the active Deployment");

  ManagerApplyIntentDTO intent{
      request.request_context,
      MintApplyId(),
      core::ApplyGeneration(1U),
      apply_journal_.cell_id(),
      scene_revision,
      candidate->retained_bundle_id(),
      candidate_value.cell_graph.graph_revision,
      request.prepared_activation,
      request.expected_admission_generation,
      request.parent_authoring_claim,
      request.isaac_pose_observer_launch_facts,
  };
  Require(intent.valid(), "Manager could not construct a valid Apply intent");
  std::optional<ManagerApplyBringUpPlanDTO> original_plan;
  if (original_bring_up_plan_source) {
    Require(!intent.parent_authoring_claim &&
                intent.expected_admission_generation.has_value(),
            "configured bring-up requires the original direct Apply parent");
    original_plan = original_bring_up_plan_source(
        intent, *predecessor, *candidate, active_plan);
    Require(original_plan->valid() && original_plan->apply_id == intent.apply_id &&
                original_plan->apply_generation == intent.apply_generation &&
                original_plan->parent_intent_digest == ComputeManagerApplyIntentDigest(intent) &&
                original_plan->expected_publication.expected_previous_publication_generation ==
                    active_plan.active_publication.publication_generation &&
                original_plan->predecessor_public_pointer_digest ==
                    active_plan.active_publication.durable_public_pointer_content_digest,
            "original configured bring-up plan differs from its captured parent/head");
    std::vector<core::ControllableComponentId> configured;
    for (const auto &component : candidate_value.cell_graph.controllable_components)
      configured.push_back(component.controllable_component_id);
    std::sort(configured.begin(), configured.end());
    Require(original_plan->configured_components == configured,
            "original configured bring-up plan omits or adds candidate components");
    const auto candidate_digest = core::ContentDigest(
        deployment::sha256_digest(candidate->canonical_bytes()));
    for (const auto &child : original_plan->children) {
      const auto component = std::find_if(
          candidate_value.cell_graph.controllable_components.begin(),
          candidate_value.cell_graph.controllable_components.end(),
          [&](const auto &row) {
            return row.controllable_component_id == child.intent.controllable_component_id;
          });
      Require(component != candidate_value.cell_graph.controllable_components.end() &&
                  std::find(component->runtime_unit_ids.begin(), component->runtime_unit_ids.end(),
                            child.runtime_unit_id) != component->runtime_unit_ids.end() &&
                  child.intent.expected_graph_revision == scene_revision.graph_revision &&
                  child.candidate_projection_revision ==
                      core::ProjectionRevisionDTO{candidate->retained_bundle_id(),
                                                  candidate_value.cell_graph.graph_revision,
                                                  scene_revision.scene_revision} &&
                  child.candidate_retained_bundle_digest == candidate_digest &&
                  child.candidate_retained_bundle_bytes == candidate->canonical_bytes().size() &&
                  child.manager_runtime_epoch == manager_runtime_epoch_ &&
                  child.intent.expected_admission_generation ==
                      *intent.expected_admission_generation,
              "original configured bring-up child differs from its candidate/current Manager");
    }
    Require(active_plan_source_.ReadCurrentPlan().active_publication ==
                active_plan.active_publication &&
                scene_owner_.CurrentProjectionRevision() == scene_revision,
            "original configured bring-up predecessor changed before acceptance");
  }
  std::optional<PersistedManagerApplyBringUpPlan> durable_plan;
  const auto durable_intent = [&] {
    if (!original_plan) return apply_journal_.EnsureIntent(intent).record;
    auto accepted = apply_journal_.EnsureBringUpIntent(intent, *original_plan);
    Require(accepted.inserted,
            "original configured bring-up acceptance lost its fresh parent identity");
    durable_plan = std::move(accepted.plan);
    return std::move(accepted.intent);
  }();
  // Capture before synchronous Acquire; only this fresh call owns these
  // verified inputs. Neither a stored plan nor a recovered claim recreates it.
  if (durable_plan) {
    captured_apply_inputs_.emplace(CapturedApplyInputs{
        intent.apply_id, intent.apply_generation, predecessor, candidate,
        std::move(initial_scene), durable_plan,
        std::move(original_prepare_acknowledgement)});
    captured_apply_inputs_->original_predecessor_publication = active_plan.active_publication;
  }
  // Claim the exact idle admission before exposing this Apply as pending.
  // Publishing pending closes readiness; doing that first invalidates the
  // admission generation carried by the accepted request itself.
  std::optional<core::CellAdmissionClaimDTO> accepted_claim;
  try {
    accepted_claim = RequireHeldAdmission(
        durable_intent, cell_admission_,
        durable_plan ? std::optional(durable_plan->plan_digest) : std::nullopt);
    if (durable_plan) captured_apply_inputs_->original_parent_claim = accepted_claim;
  } catch (const std::exception &error) {
    if (durable_plan) captured_apply_inputs_.reset();
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "apply_admission_failed", error.what(),
        ManagerApplyEffectDisposition::kKnown, false);
  }
  if (!durable_plan) {
    captured_apply_inputs_.emplace(CapturedApplyInputs{
        intent.apply_id, intent.apply_generation, predecessor, candidate,
        std::move(initial_scene), std::nullopt,
        std::move(original_prepare_acknowledgement)});
    captured_apply_inputs_->original_parent_claim = accepted_claim;
    captured_apply_inputs_->original_predecessor_publication = active_plan.active_publication;
  }
  if (apply_progress_observer_) {
    apply_progress_observer_(durable_intent.intent);
  }
  return {intent.apply_id, intent.apply_generation,
          ManagerNamedApplyResultState::kPending, false, std::nullopt};
}

ManagerNamedApplyResult
ManagerNamedApplyCoordinator::Continue(const ManagerApplyIntentDTO &intent,
                                       const bool request_replayed) {
  Require(intent.valid() && intent.cell_id == apply_journal_.cell_id(),
          "durable Apply intent is invalid or belongs to another cell");
  if (const auto observation = ObserveBringUpParent(intent, request_replayed);
      observation.has_value())
    return *observation;
  const bool original_linked = HasOriginalBringUpCapture(intent);
  const bool original_configuration = HasOriginalConfigurationCapture(intent);
  const auto durable_intent = original_linked
      ? *apply_journal_.ReadIntent(intent.apply_id, intent.apply_generation)
      : apply_journal_.EnsureIntent(intent).record;
  const auto plan_digest = original_linked
      ? std::optional(captured_apply_inputs_->bring_up_plan->plan_digest) : std::nullopt;
  const auto original_claim = original_linked
      ? captured_apply_inputs_->original_parent_claim : std::nullopt;
  if (apply_progress_observer_)
    apply_progress_observer_(durable_intent.intent);

  if (const auto terminal = apply_journal_.ReadTerminalOutcome(
          intent.apply_id, intent.apply_generation);
      terminal.has_value()) {
    return {intent.apply_id, intent.apply_generation,
            ResultStateFromTerminal(terminal->terminal_outcome),
            request_replayed, terminal->terminal_outcome};
  }

  if (original_linked && apply_journal_.ReadPublicationReceipt(intent.apply_id, intent.apply_generation)) {
    Require(captured_apply_inputs_->original_scene_begin_consumed && captured_apply_inputs_->original_scene_adoption,
            "publication observation cannot reconstruct an original Scene adoption");
    return FinishOriginalBringUpPublication(durable_intent, request_replayed);
  }

  if (original_configuration &&
      apply_journal_.ReadPublicationReceipt(intent.apply_id, intent.apply_generation))
    return FinishOriginalConfigurationPublication(durable_intent, request_replayed);

  const auto retain_pre_effect_failure =
      [&](const std::string &code, const std::string &message,
          const ManagerApplyEffectDisposition disposition) {
        auto result = RetainTerminalFailure(apply_journal_, durable_intent,
                                            code, message, disposition,
                                            request_replayed);
        if (disposition == ManagerApplyEffectDisposition::kKnown) {
          const auto terminal = apply_journal_.ReadTerminalOutcome(
              intent.apply_id, intent.apply_generation);
          Require(terminal.has_value(),
                  "pre-effect failure lacks terminal evidence");
          ReleaseAdmission(durable_intent, *terminal, cell_admission_, plan_digest, original_claim);
        }
        return result;
      };

  Require(captured_apply_inputs_.has_value() &&
              captured_apply_inputs_->apply_id == intent.apply_id &&
              captured_apply_inputs_->apply_generation ==
                  intent.apply_generation &&
              captured_apply_inputs_->predecessor != nullptr &&
              captured_apply_inputs_->candidate != nullptr,
          "named Apply continuation lacks its process-local captured inputs");
  const auto &predecessor = *captured_apply_inputs_->predecessor;
  const auto &candidate = *captured_apply_inputs_->candidate;
  const auto &initial_scene = captured_apply_inputs_->initial_scene;

  if (!apply_journal_
           .ReadFenceAttempt(intent.apply_id, intent.apply_generation)
           .has_value()) {
    // Refuse an unsupported native layout before any predecessor teardown:
    // this is a known pre-effect failure, never a candidate-initialization
    // surprise after the predecessor is gone.
    if (const auto topology =
            deployment::VerifySupportedNativeDomainTopology(
                candidate.value().cell_graph);
        !topology.ok()) {
      return retain_pre_effect_failure(topology.error().code,
                                       topology.error().message,
                                       ManagerApplyEffectDisposition::kKnown);
    }
    const auto refusal = planned_maintenance_.Preflight(
        intent, intent.expected_active_projection_revision);
    if (refusal.has_value()) {
      Require(refusal->valid() && refusal->terminal_failure(),
              "planned maintenance preflight returned an invalid refusal");
      return retain_pre_effect_failure(refusal->failure_code,
                                       refusal->failure_message,
                                       refusal->effect_disposition);
    }
  }

  try {
    static_cast<void>(RequireHeldAdmission(durable_intent, cell_admission_, plan_digest, original_claim));
  } catch (const std::exception &error) {
    const auto disposition =
        apply_journal_
                .ReadFenceAttempt(intent.apply_id, intent.apply_generation)
                .has_value()
            ? ManagerApplyEffectDisposition::kUnknown
            : ManagerApplyEffectDisposition::kKnown;
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "apply_admission_failed", error.what(),
        disposition, request_replayed);
  }

  std::optional<ManagerActiveRuntimePlanDTO> active_plan;
  try {
    active_plan.emplace(active_plan_source_.ReadCurrentPlan());
    Require(active_plan->valid() &&
                SameDeploymentRevision(
                    active_plan->active_publication.request
                        .candidate_projection_revision,
                    intent.expected_active_projection_revision),
            "predecessor publication changed before initialization");
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "apply_fence_evidence_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }

  const auto host_continuity = VerifyNativeExecutionHostContinuity(
      predecessor, candidate, artifact_source_);
  if (!host_continuity.ok()) {
    return retain_pre_effect_failure(
        host_continuity.error().code, host_continuity.error().message,
        ManagerApplyEffectDisposition::kKnown);
  }

  std::optional<ManagerApplyFenceExpectationDTO> expectation;
  try {
    expectation.emplace(FenceExpectation(
        intent, predecessor, candidate, *active_plan, apply_journal_,
        kernel_fence_, manager_runtime_epoch_,
        !original_linked &&
                std::any_of(
                    active_plan->runtime_units.begin(),
                    active_plan->runtime_units.end(),
                    [](const auto &unit) {
                      return unit.runtime_profile_id.value() ==
                             deployment::
                                 ros2_control_execution_runtime_contract::
                                     kRuntimeProfileId;
                    })
            ? &runtime_composition_port_
            : nullptr,
        retained_bundle_source_));
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "apply_fence_evidence_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }

  if (!apply_journal_
           .ReadRuntimeConvergenceIntent(intent.apply_id,
                                         intent.apply_generation)
           .has_value()) {
    std::optional<ManagerApplyFenceAcquireResult> acquired_fence;
    try {
      acquired_fence.emplace(fence_coordinator_.AcquireOrRecoverFence(
          intent, predecessor, candidate, *expectation));
    } catch (const ManagerApplyFenceEffectUncertainError &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_effect_unknown",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_acquire_failed",
          error.what(), ManagerApplyEffectDisposition::kKnown,
          request_replayed);
    }
    if (acquired_fence->path !=
            ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt &&
        acquired_fence->path !=
            ManagerApplyFenceAcquirePath::kRetriedAfterAuthoritativeNotSeen) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "planned_maintenance_interrupted",
          "maintenance admission belongs to an earlier Manager incarnation; "
          "effects will not be resumed or repeated",
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }

    const auto maintenance = planned_maintenance_.EnterIdleMaintenance(
        intent, intent.expected_active_projection_revision);
    if (!maintenance.valid()) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent,
          "planned_maintenance_result_invalid",
          "planned maintenance returned invalid evidence",
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    if (maintenance.terminal_failure()) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, maintenance.failure_code,
          maintenance.failure_message,
          maintenance.effect_disposition, request_replayed);
    }
  }

  try {
    const auto runtime = runtime_coordinator_.ConvergeOrRecover(
        intent, predecessor, candidate,
        active_plan->active_publication.publication_generation);
    if (runtime.state == ManagerRuntimeConvergenceResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    }
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent,
        "planned_maintenance_initialization_failed", error.what(),
        ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }

  if (original_linked) {
    if (!captured_apply_inputs_->original_preparation_completed) {
      try {
        auto &captured = *captured_apply_inputs_;
        const auto attempt_query = CaptureOriginalSceneStagingAttempt(intent);
        const auto request = CaptureOriginalSceneAdoption(intent, attempt_query);
        const auto before = publication_reader_.ReadActiveBundlePublication(
            request.expected_predecessor_publication.publication_generation);
        Require(before.ok() && before.value() == request.expected_predecessor_publication,
                "original Scene staging predecessor is not the current publication");
        std::vector<core::ProjectionHealthDTO> health;
        if (!captured.original_scene_stage_consumed) {
          captured.original_scene_adoption = request;
          captured.original_scene_stage_consumed = true;
          health = scene_owner_.StageOriginalManagerProductAdoption(request, attempt_query);
          captured.original_scene_stage_completed = true;
        } else {
          Require(captured.original_scene_stage_completed && captured.original_scene_adoption == request,
                  "original Scene staging outcome or identity is uncertain");
          health = scene_owner_.ReadOriginalManagerProductAdoptionProjectionHealth(request, attempt_query);
        }
        Require(CaptureOriginalSceneStagingAttempt(intent) == attempt_query &&
                    CaptureOriginalSceneAdoption(intent, attempt_query) == request,
                "original parent identity changed across Scene projection staging/read");
        const auto after = publication_reader_.ReadActiveBundlePublication(
            request.expected_predecessor_publication.publication_generation);
        Require(after.ok() && after.value() == request.expected_predecessor_publication,
                "original publication identity changed across Scene projection staging/read");
        Require(health.size() == request.native_runtime_bindings.size(),
                "original Scene staging has an incomplete native participant set");
        bool ready = true;
        const auto &target = captured.bring_up_plan->plan.expected_publication.candidate_projection_revision;
        for (std::size_t index = 0; index < health.size(); ++index) {
          const auto &row = health[index];
          Require(row.valid() && row.native_runtime_binding == request.native_runtime_bindings[index] && !row.error,
                  "original Scene staging returned foreign or failed native evidence");
          if (!row.healthy_for_target(target)) {
            Require(row.state == core::ProjectionHealthState::kRebuilding ||
                        row.state == core::ProjectionHealthState::kUninitialized,
                    "original Scene staging lacks healthy or genuinely pending native evidence");
            ready = false;
          }
        }
        if (!ready)
          return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
                  request_replayed, std::nullopt};
      } catch (const std::exception &error) {
        return RetainTerminalFailure(apply_journal_, durable_intent, "scene_product_projection_staging_failed",
            error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
      }
    }
    if (!captured_apply_inputs_->original_preparation_completed) {
      try {
        const auto claim = RequireHeldAdmission(durable_intent, cell_admission_, plan_digest, original_claim);
        const auto composition = runtime_composition_coordinator_.PrepareOriginalApplyComponentBringUp(
            *captured_apply_inputs_->bring_up_plan,
            {intent.expected_active_projection_revision.scene_revision, intent.cell_id}, claim, candidate);
        if (composition.state == ManagerSelectedRuntimeCompositionResultState::kPending)
          return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
                  request_replayed, std::nullopt};
        if (captured_apply_inputs_->original_prepare_acknowledgement &&
            !captured_apply_inputs_->prepare_acknowledgement_consumed) {
          captured_apply_inputs_->prepare_acknowledgement_consumed = true;
          captured_apply_inputs_->original_prepare_acknowledgement(intent, candidate, composition.owner_result);
        }
        captured_apply_inputs_->original_preparation_completed = true;
      } catch (const std::exception &error) {
        return RetainTerminalFailure(apply_journal_, durable_intent,
            "runtime_composition_preparation_failed", error.what(),
            ManagerApplyEffectDisposition::kUnknown, request_replayed);
      }
    }
    try {
      if (!AdvanceOriginalBringUpChildren(intent, false))
        return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
                request_replayed, std::nullopt};
    } catch (const std::exception &error) {
      return RetainTerminalFailure(apply_journal_, durable_intent, "apply_component_bring_up_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    try {
      if (!captured_apply_inputs_->original_scene_begin_consumed) {
        auto request = CaptureOriginalSceneAdoption(intent);
        const auto head = publication_reader_.ReadActiveBundlePublication(
            request.expected_predecessor_publication.publication_generation);
        Require(head.ok() && head.value() == request.expected_predecessor_publication,
                "original Scene Begin predecessor is not the current publication");
        Require(captured_apply_inputs_->original_scene_stage_completed &&
                    captured_apply_inputs_->original_scene_adoption == request,
                "Scene Begin differs from its original native staging request");
        captured_apply_inputs_->original_scene_begin_consumed = true;
        const auto begun = scene_owner_.BeginOriginalManagerProductAdoption(request);
        Require(begun.valid() && begun.apply_id == intent.apply_id &&
                    begun.apply_generation == intent.apply_generation && begun.state == core::ProjectionMaintenanceInitializationState::kInitialized &&
                    begun.ready_for_publication() && !begun.publication_receipt && !begun.effects_unknown && !begun.first_error &&
                    begun.predecessor_projection_revision == intent.expected_active_projection_revision &&
                    begun.target_projection_revision == captured_apply_inputs_->bring_up_plan->plan.expected_publication.candidate_projection_revision &&
                    begun.authoritative_snapshot_identity == request.expected_snapshot_identity &&
                    begun.native_runtime_bindings == request.native_runtime_bindings,
                "original Scene Begin returned missing or foreign adoption evidence");
        Require(CaptureOriginalSceneAdoption(intent) == request,
                "original parent identity changed across Scene Begin");
        const auto after = publication_reader_.ReadActiveBundlePublication(
            request.expected_predecessor_publication.publication_generation);
        Require(after.ok() && after.value() == request.expected_predecessor_publication,
                "original publication identity changed across Scene Begin");
      }
    } catch (const std::exception &error) {
      return RetainTerminalFailure(apply_journal_, durable_intent, "scene_product_adoption_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    try {
      // Holding is re-read after Begin; exact parent identity alone does not
      // promise that a child's deadline or health stayed unchanged.
      if (!AdvanceOriginalBringUpChildren(intent, false))
        return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
                request_replayed, std::nullopt};
      const auto composition = runtime_composition_coordinator_.InstallOrRecover(
          intent.apply_id, intent.apply_generation, candidate);
      if (composition.state == ManagerSelectedRuntimeCompositionResultState::kPending)
        return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
                request_replayed, std::nullopt};
    } catch (const std::exception &error) {
      return RetainTerminalFailure(apply_journal_, durable_intent, "runtime_composition_finalization_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    try {
      // Its first call publishes the exact head without the ordinary Scene
      // completion call. The original completion below owns the second step.
      static_cast<void>(selected_bundle_coordinator_.AdvanceOrRecover(
          intent.apply_id, intent.apply_generation, predecessor, candidate));
    } catch (const std::exception &error) {
      return RetainTerminalFailure(apply_journal_, durable_intent, "configuration_publication_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    return FinishOriginalBringUpPublication(durable_intent, request_replayed);
  }

  if (original_configuration)
    return ContinueOriginalConfiguration(durable_intent, request_replayed);

  Require(original_linked,
          "configuration Apply lacks its original preserved-Scene capture");
  bool scene_is_installed = false;
  try {
    const auto initialized = scene_initialization_coordinator_.Initialize(
        intent.apply_id, intent.apply_generation, candidate, initial_scene);
    scene_is_installed = initialized.ready_for_publication();
  } catch (const ManagerSceneInitializationPending &) {
    return {intent.apply_id, intent.apply_generation,
            ManagerNamedApplyResultState::kPending, request_replayed,
            std::nullopt};
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "scene_initialization_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }

  if (!scene_is_installed &&
      (!original_linked || !captured_apply_inputs_->original_preparation_completed)) {
    try {
      const auto composition = runtime_composition_coordinator_.PrepareOrObserve(
          intent.apply_id, intent.apply_generation, candidate);
      if (composition.state ==
          ManagerSelectedRuntimeCompositionResultState::kPending) {
        return {intent.apply_id, intent.apply_generation,
                ManagerNamedApplyResultState::kPending, request_replayed,
                std::nullopt};
      }
      if (captured_apply_inputs_->original_prepare_acknowledgement &&
          !captured_apply_inputs_->prepare_acknowledgement_consumed) {
        // Consume before invoking: failure is retained as unknown by this
        // existing preparation boundary and never replays a source transition.
        captured_apply_inputs_->prepare_acknowledgement_consumed = true;
        captured_apply_inputs_->original_prepare_acknowledgement(
            intent, candidate, composition.owner_result);
      }
      if (original_linked) captured_apply_inputs_->original_preparation_completed = true;
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent,
          "runtime_composition_preparation_failed", error.what(),
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
  }

  if (original_linked) {
    try {
      if (!AdvanceOriginalBringUpChildren(intent, false)) {
        return {intent.apply_id, intent.apply_generation, ManagerNamedApplyResultState::kPending,
                request_replayed, std::nullopt};
      }
    } catch (const std::exception &error) {
      return RetainTerminalFailure(apply_journal_, durable_intent, "apply_component_bring_up_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
  }

  try {
    static_cast<void>(scene_initialization_coordinator_.InstallInitialScene(
        intent.apply_id, intent.apply_generation, candidate, initial_scene));
  } catch (const ManagerSceneInitializationPending &) {
    return {intent.apply_id, intent.apply_generation,
            ManagerNamedApplyResultState::kPending, request_replayed,
            std::nullopt};
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "initial_scene_installation_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }

  try {
    const auto composition = runtime_composition_coordinator_.InstallOrRecover(
        intent.apply_id, intent.apply_generation, candidate);
    if (composition.state ==
        ManagerSelectedRuntimeCompositionResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    }
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "runtime_composition_finalization_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }

  bool completed = false;
  try {
    for (std::size_t advance = 0U;
         advance < kMaximumSelectedBundleAdvances && !completed; ++advance) {
      const auto result = selected_bundle_coordinator_.AdvanceOrRecover(
          intent.apply_id, intent.apply_generation, predecessor, candidate);
      completed = result.scene_completed;
    }
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "configuration_publication_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }
  if (!completed) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent,
        "configuration_publication_incomplete",
        "coherent publication exceeded its bounded two effects",
        ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }

  if (original_linked) return FinishOriginalBringUpPublication(durable_intent, request_replayed);

  // The candidate is public only now. Its lifecycle/route owner can verify
  // that exact publication while the parent fence is still held. Do not
  // release admission merely because the pointer swap succeeded.
  try {
    const auto composition = runtime_composition_coordinator_.InstallOrRecover(
        intent.apply_id, intent.apply_generation, candidate);
    if (composition.state ==
        ManagerSelectedRuntimeCompositionResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    }
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent,
        "runtime_composition_publication_verification_failed", error.what(),
        ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }

  std::optional<ManagerApplyFenceReleaseResult> release;
  try {
    release.emplace(fence_release_coordinator_.ReleaseOrRecover(
        intent.apply_id, intent.apply_generation));
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "apply_fence_release_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }
  const auto terminal = apply_journal_.ReadTerminalOutcome(
      intent.apply_id, intent.apply_generation);
  Require(terminal.has_value(),
          "Manager Apply release completed without terminal evidence");
  ReleaseAdmission(durable_intent, *terminal, cell_admission_, plan_digest, original_claim);
  return {intent.apply_id, intent.apply_generation,
          ManagerNamedApplyResultState::kSucceeded, request_replayed,
          release->durable_terminal_outcome.record.terminal_outcome};
}

std::optional<ManagerNamedApplyResult>
ManagerNamedApplyCoordinator::RecoverIncompleteApply() {
  std::lock_guard lock(operation_mutex_);
  const auto inventory = apply_journal_.ReadStatusInventory();
  const PersistedManagerApplyStatus *pending = nullptr;
  for (const auto &status : inventory) {
    if (IsTerminal(status))
      continue;
    Require(pending == nullptr,
            "multiple nonterminal Applies contradict the sole progression owner");
    pending = &status;
  }
  if (pending == nullptr) {
    captured_apply_inputs_.reset();
    return std::nullopt;
  }

  if (const auto observation =
          ObserveBringUpParent(pending->intent.intent, true);
      observation.has_value())
    return *observation;

  if (captured_apply_inputs_.has_value() &&
      captured_apply_inputs_->apply_id == pending->intent.intent.apply_id &&
      captured_apply_inputs_->apply_generation ==
          pending->intent.intent.apply_generation) {
    auto result = Continue(pending->intent.intent, true);
    if (result.state != ManagerNamedApplyResultState::kPending) {
      captured_apply_inputs_.reset();
    }
    return result;
  }

  return RetainTerminalFailure(
      apply_journal_, pending->intent, "planned_maintenance_interrupted",
      "Manager reopened a nonterminal maintenance operation; effects remain "
      "unknown and admission stays closed until explicit diagnosis and a "
      "later authorized operation",
      ManagerApplyEffectDisposition::kUnknown, true);
}

} // namespace manyforge::component_manager
