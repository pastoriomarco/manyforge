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
  Require(SameDeploymentRevision(snapshot.active_projection_revision,
                                 intent.expected_active_projection_revision) &&
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
         intent.prepared_activation == request.prepared_activation &&
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

void ReleaseAdmission(
    const PersistedManagerApplyIntent &durable,
    const PersistedManagerApplyTerminalOutcome &terminal,
    core::IKernelCellAdmissionPort &cell_admission) {
  const auto intent = durable.intent.parent_authoring_claim.has_value()
                          ? durable.intent.parent_authoring_claim->intent
                          : DirectAdmissionIntent(durable);
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
         !candidate_retained_bundle_id.empty() &&
         prepared_activation.valid();
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
    ManagerPlannedMaintenanceCoordinator &planned_maintenance,
    core::RuntimeEpoch manager_runtime_epoch, std::string operating_system,
    std::vector<std::string> launch_environment_contracts,
    RuntimeObservationReducer::Clock clock_ns, ApplyIdFactory apply_id_factory,
    RuntimePreStartGuard pre_start_guard,
    ApplyProgressObserver apply_progress_observer)
    : apply_journal_(apply_journal), active_plan_source_(active_plan_source),
      retained_bundle_source_(retained_bundle_source),
      scene_owner_(scene_owner),
      kernel_fence_(kernel_fence), cell_admission_(cell_admission),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)),
      apply_id_factory_(std::move(apply_id_factory)),
      apply_progress_observer_(std::move(apply_progress_observer)),
      fence_coordinator_(apply_journal_, kernel_fence_, manager_runtime_epoch_),
      planned_maintenance_(planned_maintenance),
      runtime_coordinator_(fence_coordinator_, apply_journal_,
                           publication_store, artifact_source, process_port,
                           progress_port, std::move(operating_system),
                           std::move(launch_environment_contracts),
                           std::move(clock_ns), std::move(pre_start_guard)),
      scene_stage_coordinator_(apply_journal_, retained_bundle_source_,
                               publication_store, scene_owner,
                               bootstrap_scene_owner, &process_port),
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

ManagerNamedApplyResult ManagerNamedApplyCoordinator::AcceptOrReplay(
    const ManagerNamedApplyRequestDTO &request) {
  std::lock_guard lock(operation_mutex_);
  Require(request.valid(), "named Apply request is invalid");

  if (const auto existing = apply_journal_.ReadRequest(
          request.request_context.request_owner_epoch,
          request.request_context.request_id);
      existing.has_value()) {
    Require(MatchesRequest(existing->intent, request),
            "public request identity is already bound to another Apply");
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
  };
  Require(intent.valid(), "Manager could not construct a valid Apply intent");
  const auto durable_intent = apply_journal_.EnsureIntent(intent).record;
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
  const auto durable_intent = apply_journal_.EnsureIntent(intent).record;
  if (apply_progress_observer_) {
    apply_progress_observer_(durable_intent.intent);
  }

  if (const auto terminal = apply_journal_.ReadTerminalOutcome(
          intent.apply_id, intent.apply_generation);
      terminal.has_value()) {
    // Exact terminal replay is observation-only. Success proves settlement;
    // failure may deliberately retain closed admission, especially when an
    // effect is unknown, and replay must never release or repeat it.
    return {intent.apply_id, intent.apply_generation,
            ResultStateFromTerminal(terminal->terminal_outcome), request_replayed,
            terminal->terminal_outcome};
  }

  const auto retain_pre_effect_failure =
      [&](const std::string &code, const std::string &message,
          const ManagerApplyEffectDisposition disposition) {
        auto result = RetainTerminalFailure(apply_journal_, durable_intent, code,
                                            message, disposition,
                                            request_replayed);
        // Composer transferred a real, already-held parent claim when this
        // operation was accepted. A rejection before the maintenance fence
        // has any effect must not strand authoring. Manager owns the terminal
        // release because Composer deliberately stopped tracking the claim at
        // acceptance.
        if (durable_intent.intent.parent_authoring_claim.has_value() &&
            disposition == ManagerApplyEffectDisposition::kKnown) {
          const auto terminal = apply_journal_.ReadTerminalOutcome(
              intent.apply_id, intent.apply_generation);
          Require(terminal.has_value(),
                  "pre-effect failure lacks terminal evidence");
          ReleaseAdmission(durable_intent, *terminal, cell_admission_);
        }
        return result;
      };

  std::shared_ptr<const deployment::RetainedBundle> predecessor;
  std::shared_ptr<const deployment::RetainedBundle> candidate;
  try {
    predecessor = LoadBundle(
        retained_bundle_source_,
        intent.expected_active_projection_revision.retained_bundle_id,
        "predecessor");
    candidate = LoadBundle(retained_bundle_source_,
                           intent.candidate_retained_bundle_id, "candidate");
  } catch (const std::exception &error) {
    return retain_pre_effect_failure("apply_bundle_validation_failed",
                                     error.what(),
                                     ManagerApplyEffectDisposition::kKnown);
  }

  // A prior accepted fence without the first candidate-initialization intent
  // straddles the teardown crash window. Resolve it before admission recovery:
  // recovery is observation-only and may not reacquire a lost claim or repeat
  // predecessor effects.
  if (apply_journal_
          .ReadFenceAcceptance(intent.apply_id, intent.apply_generation)
          .has_value() &&
      !apply_journal_
           .ReadRuntimeConvergenceIntent(intent.apply_id,
                                         intent.apply_generation)
           .has_value()) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "planned_maintenance_interrupted",
        "maintenance admission was accepted but candidate initialization was "
        "not durably reached; recovery will not reacquire admission or repeat "
        "teardown effects",
        ManagerApplyEffectDisposition::kUnknown, request_replayed);
  }

  // Ordinary idle/stale refusals are decided before the configuration fence or
  // any process effect. A transferred parent authoring claim may already be
  // held, but the failure path releases it from durable pre-effect evidence so
  // the predecessor remains authoritative and controls remain usable. The
  // maintenance owner repeats this check after admission closes to catch races;
  // a refusal at that later boundary remains contained.
  if (!apply_journal_
           .ReadFenceAttempt(intent.apply_id, intent.apply_generation)
           .has_value()) {
    const auto refusal = planned_maintenance_.Preflight(
        intent, intent.expected_active_projection_revision);
    if (refusal.has_value()) {
      Require(refusal->valid() && refusal->terminal_failure(),
              "planned maintenance preflight returned an invalid refusal");
      return retain_pre_effect_failure(
          refusal->failure_code, refusal->failure_message,
          refusal->effect_disposition);
    }
  }

  // The Apply intent is durable before this exact Kernel claim is acquired or
  // validated. Every external phase below therefore has one restart-stable
  // recovery identity and can never run outside the cell admission decision.
  try {
    static_cast<void>(RequireHeldAdmission(durable_intent, cell_admission_));
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

  const auto finalized = apply_journal_.ReadSceneFinalized(
      intent.apply_id, intent.apply_generation);
  const auto release_started = apply_journal_.ReadFenceReleaseIntent(
      intent.apply_id, intent.apply_generation);
  if (finalized.has_value() || release_started.has_value()) {
    // Scene Finalize and the public successor can be durable while the
    // Manager/native child is replaced.  The Scene activation row is the
    // durable owner of native projection membership, so replay it before
    // composing the successor.  Otherwise BuildManagedCompositionInput sees
    // a live child whose candidate projections are still only prepared and
    // correctly rejects the composition as not active at the candidate
    // revision.  Only after that replay do we install the exact composition
    // under the still-held Apply fence and query/release the fence.
    if (apply_journal_.ReadSceneActivated(intent.apply_id,
                                          intent.apply_generation)
            .has_value()) {
      try {
        static_cast<void>(scene_activation_coordinator_.ActivateOrRecover(
            intent.apply_id, intent.apply_generation));
      } catch (const ManagerSceneActivationPending &) {
        return {intent.apply_id, intent.apply_generation,
                ManagerNamedApplyResultState::kPending, request_replayed,
                std::nullopt};
      } catch (const std::exception &error) {
        return RetainTerminalFailure(
            apply_journal_, durable_intent, "scene_activation_failed",
            error.what(), ManagerApplyEffectDisposition::kUnknown,
            request_replayed);
      }
    }
    std::optional<ManagerSelectedRuntimeCompositionCoordinatorResult>
        composition;
    try {
      composition.emplace(runtime_composition_coordinator_.InstallOrRecover(
          intent.apply_id, intent.apply_generation));
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "runtime_composition_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    if (composition->state ==
        ManagerSelectedRuntimeCompositionResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
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
    ReleaseAdmission(durable_intent, *terminal, cell_admission_);
    return {intent.apply_id, intent.apply_generation,
            ManagerNamedApplyResultState::kSucceeded, request_replayed,
            release->durable_terminal_outcome.record.terminal_outcome};
  }

  // Runtime convergence and scene projection activation form one ordered
  // hand-off.  The candidate native child may be launched and prepared before
  // staging, but its candidate projection is not usable for composition until
  // Scene activates it. On restart, an already-activated scene must still
  // verify the exact retained child epoch before authority can advance.
  bool scene_was_activated =
      apply_journal_.ReadSceneActivated(intent.apply_id,
                                        intent.apply_generation)
          .has_value();
  if (scene_was_activated) {
    // Scene activation is durable, but it cannot substitute for current
    // readiness of the exact retained component epochs. Convergence is an
    // observe-only replay once staged-ready evidence exists and fails closed
    // on loss or epoch drift; it never recreates a child for this Apply.
    std::optional<ManagerActiveRuntimePlanDTO> active_plan;
    try {
      active_plan.emplace(active_plan_source_.ReadCurrentPlan());
      Require(active_plan->valid() &&
                  SameDeploymentRevision(
                      active_plan->active_publication.request
                          .candidate_projection_revision,
                      intent.expected_active_projection_revision),
              "predecessor publication changed before runtime convergence");
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "active_plan_recheck_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    std::optional<ManagerApplyFenceExpectationDTO> expectation;
    try {
      expectation.emplace(FenceExpectation(
          intent, *predecessor, *candidate, *active_plan, apply_journal_,
          kernel_fence_, manager_runtime_epoch_));
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_evidence_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    std::optional<ManagerApplyRuntimeConvergenceResult> runtime;
    try {
      runtime.emplace(runtime_coordinator_.ConvergeOrRecover(
          intent, *predecessor, *candidate, *expectation,
          active_plan->active_publication.publication_generation));
    } catch (const ManagerRuntimeProcessEffectUncertainError &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent,
          "planned_maintenance_initialization_effect_unknown", error.what(),
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent,
          "planned_maintenance_initialization_failed", error.what(),
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    if (runtime->state == ManagerRuntimeConvergenceResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    }
    try {
      static_cast<void>(scene_activation_coordinator_.ActivateOrRecover(
          intent.apply_id, intent.apply_generation));
    } catch (const ManagerSceneActivationPending &) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "scene_activation_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
  }
  if (!scene_was_activated) {
    // Durable Scene Stage success only records what an earlier runtime process
    // prepared.  It does not prove that the intended component epochs are
    // currently ready, so converge before either staging or activating.  The
    // coordinator is idempotent when the exact current child still matches.
    std::optional<ManagerActiveRuntimePlanDTO> active_plan;
    try {
      active_plan.emplace(active_plan_source_.ReadCurrentPlan());
      Require(active_plan->valid() &&
                  SameDeploymentRevision(
                      active_plan->active_publication.request
                          .candidate_projection_revision,
                      intent.expected_active_projection_revision),
              "predecessor publication changed before runtime convergence");
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "active_plan_recheck_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    std::optional<ManagerApplyFenceExpectationDTO> expectation;
    try {
      expectation.emplace(FenceExpectation(
          intent, *predecessor, *candidate, *active_plan, apply_journal_,
          kernel_fence_, manager_runtime_epoch_));
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_evidence_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    // The runtime convergence intent is committed before the first candidate
    // start effect. Its presence therefore proves that predecessor teardown
    // already completed for this operation and prevents a pending candidate
    // initialization from being mistaken for a predecessor on the next poll.
    if (!apply_journal_
             .ReadRuntimeConvergenceIntent(intent.apply_id,
                                           intent.apply_generation)
             .has_value()) {
      std::optional<ManagerApplyFenceAcquireResult> acquired_fence;
      try {
        acquired_fence.emplace(fence_coordinator_.AcquireOrRecoverFence(
            intent, *predecessor, *candidate, *expectation));
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
            apply_journal_, durable_intent,
            "planned_maintenance_interrupted",
            "a prior Manager incarnation accepted the maintenance fence but "
            "did not durably reach candidate initialization; teardown effects "
            "will not be repeated",
            ManagerApplyEffectDisposition::kUnknown, request_replayed);
      }
      std::optional<ManagerPlannedMaintenanceResult> maintenance;
      try {
        maintenance.emplace(planned_maintenance_.EnterIdleMaintenance(
            intent, intent.expected_active_projection_revision));
      } catch (const std::exception &error) {
        return RetainTerminalFailure(
            apply_journal_, durable_intent,
            "planned_maintenance_boundary_failed", error.what(),
            ManagerApplyEffectDisposition::kUnknown, request_replayed);
      }
      if (!maintenance->valid()) {
        return RetainTerminalFailure(
            apply_journal_, durable_intent,
            "planned_maintenance_result_invalid",
            "planned maintenance returned an invalid result",
            ManagerApplyEffectDisposition::kUnknown, request_replayed);
      }
      if (maintenance->terminal_failure()) {
        const auto disposition =
            maintenance->effect_disposition ==
                    ManagerApplyEffectDisposition::kKnown
                ? ManagerApplyEffectDisposition::kUnknown
                : maintenance->effect_disposition;
        return RetainTerminalFailure(
            apply_journal_, durable_intent, maintenance->failure_code,
            maintenance->failure_message, disposition,
            request_replayed);
      }
    }
    std::optional<ManagerApplyRuntimeConvergenceResult> runtime;
    try {
      runtime.emplace(runtime_coordinator_.ConvergeOrRecover(
          intent, *predecessor, *candidate, *expectation,
          active_plan->active_publication.publication_generation));
    } catch (const ManagerRuntimeProcessEffectUncertainError &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent,
          "planned_maintenance_initialization_effect_unknown", error.what(),
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent,
          "planned_maintenance_initialization_failed", error.what(),
          ManagerApplyEffectDisposition::kUnknown, request_replayed);
    }
    if (runtime->state == ManagerRuntimeConvergenceResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    }
    if (!apply_journal_
             .ReadSceneStaged(intent.apply_id, intent.apply_generation)
             .has_value()) {
      try {
        static_cast<void>(scene_stage_coordinator_.StageOrRecover(
            intent.apply_id, intent.apply_generation));
      } catch (const ManagerSceneStagePending &) {
        return {intent.apply_id, intent.apply_generation,
                ManagerNamedApplyResultState::kPending, request_replayed,
                std::nullopt};
      } catch (const std::exception &error) {
        return RetainTerminalFailure(
            apply_journal_, durable_intent, "scene_stage_failed",
            error.what(), ManagerApplyEffectDisposition::kUnknown,
            request_replayed);
      }
    }
    try {
      static_cast<void>(scene_activation_coordinator_.ActivateOrRecover(
          intent.apply_id, intent.apply_generation));
    } catch (const ManagerSceneActivationPending &) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "scene_activation_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    scene_was_activated = true;
  }

  // Once Scene has activated the candidate, recheck aggregate readiness of
  // the same retained epochs before installation. Pending or failed current
  // readiness cannot switch runtime authority.
  if (scene_was_activated) {
    std::optional<ManagerActiveRuntimePlanDTO> active_plan;
    try {
      active_plan.emplace(active_plan_source_.ReadCurrentPlan());
      Require(active_plan->valid() &&
                  SameDeploymentRevision(
                      active_plan->active_publication.request
                          .candidate_projection_revision,
                      intent.expected_active_projection_revision),
              "predecessor publication changed before runtime convergence");
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "active_plan_recheck_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    std::optional<ManagerApplyFenceExpectationDTO> expectation;
    try {
      expectation.emplace(FenceExpectation(
          intent, *predecessor, *candidate, *active_plan, apply_journal_,
          kernel_fence_, manager_runtime_epoch_));
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "apply_fence_evidence_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    std::optional<ManagerApplyRuntimeConvergenceResult> runtime;
    try {
      runtime.emplace(runtime_coordinator_.ConvergeOrRecover(
          intent, *predecessor, *candidate, *expectation,
          active_plan->active_publication.publication_generation));
    } catch (const std::exception &error) {
      return RetainTerminalFailure(
          apply_journal_, durable_intent, "runtime_readiness_recheck_failed",
          error.what(), ManagerApplyEffectDisposition::kUnknown,
          request_replayed);
    }
    if (runtime->state == ManagerRuntimeConvergenceResultState::kPending) {
      return {intent.apply_id, intent.apply_generation,
              ManagerNamedApplyResultState::kPending, request_replayed,
              std::nullopt};
    }
  }

  std::optional<ManagerSelectedRuntimeCompositionCoordinatorResult>
      composition;
  try {
    composition.emplace(runtime_composition_coordinator_.InstallOrRecover(
        intent.apply_id, intent.apply_generation));
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "runtime_composition_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }
  if (composition->state ==
      ManagerSelectedRuntimeCompositionResultState::kPending) {
    return {intent.apply_id, intent.apply_generation,
            ManagerNamedApplyResultState::kPending, request_replayed,
            std::nullopt};
  }

  bool finalized_now = false;
  try {
    for (std::size_t advance = 0U;
         advance < kMaximumSelectedBundleAdvances && !finalized_now;
         ++advance) {
      const auto result = selected_bundle_coordinator_.AdvanceOrRecover(
          intent.apply_id, intent.apply_generation);
      finalized_now = result.durable_finalized.has_value();
    }
  } catch (const std::exception &error) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent, "configuration_publication_failed",
        error.what(), ManagerApplyEffectDisposition::kUnknown,
        request_replayed);
  }
  if (!finalized_now) {
    return RetainTerminalFailure(
        apply_journal_, durable_intent,
        "configuration_publication_incomplete",
        "selected-bundle transition exceeded its bounded two effects",
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
  ReleaseAdmission(durable_intent, *terminal, cell_admission_);
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
    if (IsTerminal(status)) {
      continue;
    }
    Require(
        pending == nullptr,
        "multiple nonterminal Applies contradict the sole progression owner");
    pending = &status;
  }
  if (pending == nullptr) {
    return std::nullopt;
  }
  return Continue(pending->intent.intent, true);
}

} // namespace manyforge::component_manager
