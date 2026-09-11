#include "manyforge_component_manager/manager_named_apply_coordinator.hpp"

#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <cstddef>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::size_t kMaximumApplyIdMintAttempts = 4U;
constexpr std::size_t kMaximumSelectedBundleAdvances = 2U;

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

[[nodiscard]] ManagerApplyFenceExpectationDTO
FenceExpectation(const ManagerApplyIntentDTO &intent,
                 const deployment::RetainedBundle &predecessor,
                 const deployment::RetainedBundle &candidate,
                 const ManagerActiveRuntimePlanDTO &active_plan,
                 SqliteManagerApplyJournal &journal,
                 core::IManagerKernelApplyFencePort &kernel_fence,
                 const core::RuntimeEpoch &manager_runtime_epoch) {
  if (const auto attempt =
          journal.ReadFenceAttempt(intent.apply_id, intent.apply_generation);
      attempt.has_value()) {
    const auto &fence = attempt->fence_intent;
    return {
        fence.expected_operation_resource_generation,
        fence.expected_fence_generation,
        fence.expected_active_control_configurations,
    };
  }

  const auto scope = deployment::derive_apply_configuration_fence_scope(
      predecessor, candidate);
  const core::ConfigurationFenceAdmissionQueryDTO query{
      scope,
      manager_runtime_epoch,
  };
  Require(query.valid(), "derived Kernel admission query is invalid");

  const auto snapshot = kernel_fence.ObserveAdmission(query);
  Require(snapshot.matches(query),
          "Kernel returned invalid or foreign admission evidence");
  Require(snapshot.active_projection_revision ==
                  intent.expected_active_projection_revision &&
              snapshot.active_public_pointer_digest ==
                  active_plan.active_publication
                      .durable_public_pointer_content_digest &&
              snapshot.execution_host_epoch.publication_generation ==
                  active_plan.active_publication.publication_generation,
          "Kernel admission evidence does not match the current durable "
          "predecessor publication");
  if (snapshot.affected_queued_operation_count != 0U ||
      snapshot.affected_active_operation_count != 0U) {
    throw ManagerNamedApplyBusyError(
        "named Apply rejected because its affected resources are busy");
  }
  return {
      snapshot.operation_resource_generation,
      snapshot.configuration_fence_generation,
      snapshot.active_control_configurations,
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
         intent.expected_admission_generation ==
             request.expected_admission_generation &&
         intent.parent_authoring_claim == request.parent_authoring_claim;
}

[[nodiscard]] core::CellAdmissionClaimIntentDTO
DirectAdmissionIntent(const PersistedManagerApplyIntent &durable) {
  const auto &intent = durable.intent;
  Require(intent.expected_admission_generation.has_value(),
          "direct Manager Apply lacks its admission generation");
  core::CellAdmissionClaimIntentDTO claim{
      core::CellAdmissionClaimKindDTO::kManagerApply,
      core::CellAdmissionOwnerRoleDTO::kManager,
      intent.request_context.request_owner_epoch,
      intent.apply_id.value(),
      intent.request_context.request_id,
      durable.intent_digest,
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
    core::IKernelCellAdmissionPort &cell_admission) {
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

  const auto intent = DirectAdmissionIntent(durable);
  const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(
      {intent, std::nullopt, std::nullopt});
  if (!receipt.ok()) {
    Fail("Kernel Manager-claim lookup failed: " + receipt.error().message);
  }
  if (receipt.value().status ==
          core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact &&
      receipt.value().matched_claim.has_value()) {
    return *receipt.value().matched_claim;
  }
  Require(receipt.value().status ==
              core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
          "Manager Apply admission claim is conflicting or already settled");
  const auto acquired = cell_admission.AcquireCellAdmissionClaim(intent);
  if (!acquired.ok()) {
    Fail("Kernel rejected Manager Apply admission: " +
         acquired.error().message);
  }
  return acquired.value();
}

void ReleaseDirectAdmission(
    const PersistedManagerApplyIntent &durable,
    const PersistedManagerApplyTerminalOutcome &terminal,
    core::IKernelCellAdmissionPort &cell_admission) {
  if (durable.intent.parent_authoring_claim.has_value()) {
    return;
  }
  const auto intent = DirectAdmissionIntent(durable);
  const auto receipt = cell_admission.GetCellAdmissionRecoveryReceipt(
      {intent, std::nullopt, std::nullopt});
  if (!receipt.ok()) {
    Fail("Kernel Manager-claim settlement lookup failed: " +
         receipt.error().message);
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
  const core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
       terminal.terminal_outcome_digest},
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
         !candidate_retained_bundle_id.empty();
}

ManagerNamedApplyCoordinator::ManagerNamedApplyCoordinator(
    SqliteManagerApplyJournal &apply_journal,
    IManagerActiveRuntimePlanSource &active_plan_source,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    core::IActiveBundlePublicationStore &publication_store,
    core::IManagerSceneProjectionHandoffPort &scene_owner,
    core::IManagerSceneProjectionBootstrapPort *bootstrap_scene_owner,
    core::IManagerKernelApplyFencePort &kernel_fence,
    core::IKernelCellAdmissionPort &cell_admission,
    IManagerSelectedRuntimeCompositionPort &runtime_composition,
    IRuntimeUnitProcessPort &process_port,
    IRuntimeUnitProgressPort &progress_port,
    core::RuntimeEpoch manager_runtime_epoch, std::string operating_system,
    std::vector<std::string> launch_environment_contracts,
    RuntimeObservationReducer::Clock clock_ns, ApplyIdFactory apply_id_factory,
    RuntimePreStartGuard pre_start_guard)
    : apply_journal_(apply_journal), active_plan_source_(active_plan_source),
      retained_bundle_source_(retained_bundle_source),
      kernel_fence_(kernel_fence), cell_admission_(cell_admission),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)),
      apply_id_factory_(std::move(apply_id_factory)),
      fence_coordinator_(apply_journal_, kernel_fence_, manager_runtime_epoch_),
      runtime_coordinator_(fence_coordinator_, apply_journal_,
                           publication_store, artifact_source, process_port,
                           progress_port, std::move(operating_system),
                           std::move(launch_environment_contracts),
                           std::move(clock_ns), std::move(pre_start_guard)),
      scene_stage_coordinator_(apply_journal_, retained_bundle_source_,
                               publication_store, scene_owner,
                               bootstrap_scene_owner),
      scene_activation_coordinator_(apply_journal_, scene_owner),
      runtime_composition_coordinator_(apply_journal_, retained_bundle_source_,
                                       runtime_composition,
                                       manager_runtime_epoch_),
      selected_bundle_coordinator_(apply_journal_, retained_bundle_source_,
                                   publication_store, scene_owner,
                                   &artifact_source),
      fence_release_coordinator_(apply_journal_, kernel_fence_,
                                 manager_runtime_epoch_) {
  Require(!manager_runtime_epoch_.empty(),
          "Manager runtime epoch cannot be empty");
  Require(static_cast<bool>(apply_id_factory_), "Apply ID factory is required");
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

ManagerNamedApplyResult ManagerNamedApplyCoordinator::ApplyOrRecover(
    const ManagerNamedApplyRequestDTO &request) {
  std::lock_guard lock(operation_mutex_);
  Require(request.valid(), "named Apply request is invalid");

  if (const auto existing = apply_journal_.ReadRequest(
          request.request_context.request_owner_epoch,
          request.request_context.request_id);
      existing.has_value()) {
    Require(MatchesRequest(existing->intent, request),
            "public request identity is already bound to another Apply");
    return Continue(existing->intent, true);
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

  const auto inventory = apply_journal_.ReadStatusInventory();
  for (const auto &status : inventory) {
    if (!IsTerminal(status)) {
      throw ManagerNamedApplyBusyError(
          "another named Apply remains nonterminal on this Manager strand");
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

  ManagerApplyIntentDTO intent{
      request.request_context,
      MintApplyId(),
      core::ApplyGeneration(1U),
      apply_journal_.cell_id(),
      active_revision,
      candidate->retained_bundle_id(),
      candidate_value.cell_graph.graph_revision,
      request.expected_admission_generation,
      request.parent_authoring_claim,
  };
  Require(intent.valid(), "Manager could not construct a valid Apply intent");
  return Continue(intent, false);
}

ManagerNamedApplyResult
ManagerNamedApplyCoordinator::Continue(const ManagerApplyIntentDTO &intent,
                                       const bool request_replayed) {
  Require(intent.valid() && intent.cell_id == apply_journal_.cell_id(),
          "durable Apply intent is invalid or belongs to another cell");
  const auto durable_intent = apply_journal_.EnsureIntent(intent).record;

  if (const auto terminal = apply_journal_.ReadTerminalOutcome(
          intent.apply_id, intent.apply_generation);
      terminal.has_value()) {
    // The durable terminal proves that both the configuration fence and the
    // direct admission claim were already settled. Exact replay is a read;
    // it must not depend on a surviving Kernel process or issue another
    // settlement call.
    return {intent.apply_id, intent.apply_generation,
            ManagerNamedApplyResultState::kSucceeded, request_replayed,
            terminal->terminal_outcome};
  }

  const auto predecessor =
      LoadBundle(retained_bundle_source_,
                 intent.expected_active_projection_revision.retained_bundle_id,
                 "predecessor");
  const auto candidate =
      LoadBundle(retained_bundle_source_, intent.candidate_retained_bundle_id,
                 "candidate");

  // The Apply intent is durable before this exact Kernel claim is acquired or
  // validated. Every external phase below therefore has one restart-stable
  // recovery identity and can never run outside the cell admission decision.
  static_cast<void>(RequireHeldAdmission(durable_intent, cell_admission_));

  const auto finalized = apply_journal_.ReadSceneFinalized(
      intent.apply_id, intent.apply_generation);
  const auto release_started = apply_journal_.ReadFenceReleaseIntent(
      intent.apply_id, intent.apply_generation);
  if (finalized.has_value() || release_started.has_value()) {
    const auto release = fence_release_coordinator_.ReleaseOrRecover(
        intent.apply_id, intent.apply_generation);
    const auto terminal = apply_journal_.ReadTerminalOutcome(
        intent.apply_id, intent.apply_generation);
    Require(terminal.has_value(),
            "Manager Apply release completed without terminal evidence");
    ReleaseDirectAdmission(durable_intent, *terminal, cell_admission_);
    return {intent.apply_id, intent.apply_generation,
            ManagerNamedApplyResultState::kSucceeded, request_replayed,
            release.durable_terminal_outcome.record.terminal_outcome};
  }

  if (!apply_journal_
           .ReadSceneActivated(intent.apply_id, intent.apply_generation)
           .has_value()) {
    if (!apply_journal_
             .ReadSceneStaged(intent.apply_id, intent.apply_generation)
             .has_value()) {
      if (!apply_journal_
               .ReadLatestRuntimeStagedReady(intent.apply_id,
                                             intent.apply_generation)
               .has_value()) {
        const auto active_plan = active_plan_source_.ReadCurrentPlan();
        Require(active_plan.valid() &&
                    active_plan.active_publication.request
                            .candidate_projection_revision ==
                        intent.expected_active_projection_revision,
                "predecessor publication changed before Apply selection");
        const auto expectation = FenceExpectation(
            intent, *predecessor, *candidate, active_plan, apply_journal_,
            kernel_fence_, manager_runtime_epoch_);
        const auto runtime = runtime_coordinator_.ConvergeOrRecover(
            intent, *predecessor, *candidate, expectation,
            active_plan.active_publication.publication_generation);
        if (runtime.state == ManagerRuntimeConvergenceResultState::kPending) {
          return {intent.apply_id, intent.apply_generation,
                  ManagerNamedApplyResultState::kPending, request_replayed,
                  std::nullopt};
        }
      }
      static_cast<void>(scene_stage_coordinator_.StageOrRecover(
          intent.apply_id, intent.apply_generation));
    }
    static_cast<void>(scene_activation_coordinator_.ActivateOrRecover(
        intent.apply_id, intent.apply_generation));
  }

  const auto composition = runtime_composition_coordinator_.InstallOrRecover(
      intent.apply_id, intent.apply_generation);
  if (composition.state ==
      ManagerSelectedRuntimeCompositionResultState::kPending) {
    return {intent.apply_id, intent.apply_generation,
            ManagerNamedApplyResultState::kPending, request_replayed,
            std::nullopt};
  }

  bool finalized_now = false;
  for (std::size_t advance = 0U;
       advance < kMaximumSelectedBundleAdvances && !finalized_now; ++advance) {
    const auto result = selected_bundle_coordinator_.AdvanceOrRecover(
        intent.apply_id, intent.apply_generation);
    finalized_now = result.durable_finalized.has_value();
  }
  Require(finalized_now,
          "selected-bundle transition exceeded its bounded two effects");

  const auto release = fence_release_coordinator_.ReleaseOrRecover(
      intent.apply_id, intent.apply_generation);
  const auto terminal = apply_journal_.ReadTerminalOutcome(
      intent.apply_id, intent.apply_generation);
  Require(terminal.has_value(),
          "Manager Apply release completed without terminal evidence");
  ReleaseDirectAdmission(durable_intent, *terminal, cell_admission_);
  return {intent.apply_id, intent.apply_generation,
          ManagerNamedApplyResultState::kSucceeded, request_replayed,
          release.durable_terminal_outcome.record.terminal_outcome};
}

std::optional<ManagerNamedApplyResult>
ManagerNamedApplyCoordinator::RecoverIncompleteApply() {
  std::lock_guard lock(operation_mutex_);
  const auto inventory = apply_journal_.ReadStatusInventory();
  const PersistedManagerApplyStatus *pending = nullptr;
  for (const auto &status : inventory) {
    if (IsTerminal(status)) {
      continue;
    }
    Require(pending == nullptr,
            "multiple nonterminal Applies contradict the sole Manager strand");
    pending = &status;
  }
  if (pending == nullptr) {
    return std::nullopt;
  }
  return Continue(pending->intent.intent, true);
}

} // namespace manyforge::component_manager
