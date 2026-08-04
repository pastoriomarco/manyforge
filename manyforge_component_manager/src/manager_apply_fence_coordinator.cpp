#include "manyforge_component_manager/manager_apply_fence_coordinator.hpp"

#include <manyforge_deployment/apply_affected_scope.hpp>

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerApplyFenceCoordinatorError(detail);
}

[[nodiscard]] std::string ExceptionDetail(const std::string &prefix,
                                          const std::exception &error) {
  return prefix + ": " + error.what();
}

[[noreturn]] void FailUncertain(
    std::string detail, const ManagerApplyFenceUncertaintyPhase phase,
    const bool acquire_invoked_in_this_call,
    const core::ApplyFenceAcquireAttemptQueryDTO &query,
    std::optional<core::ConfigurationFenceAcceptedDTO> observed_acceptance =
        std::nullopt,
    std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
        observed_reconciliation = std::nullopt,
    std::optional<core::ApplyFenceReconcileResultDTO> observed_rehydration =
        std::nullopt) {
  throw ManagerApplyFenceEffectUncertainError(
      std::move(detail), phase, acquire_invoked_in_this_call, query,
      std::move(observed_acceptance), std::move(observed_reconciliation),
      std::move(observed_rehydration));
}

struct PreparedFence final {
  ManagerApplyAffectedScopeDTO affected_scope;
  core::ApplyConfigurationFenceIntentDTO fence_intent;
};

[[nodiscard]] PreparedFence
PrepareFence(const SqliteManagerApplyJournal &apply_journal,
             const ManagerApplyIntentDTO &apply_intent,
             const manyforge::deployment::RetainedBundle &predecessor,
             const manyforge::deployment::RetainedBundle &candidate,
             const ManagerApplyFenceExpectationDTO &expectation) {
  if (!apply_intent.valid()) {
    Fail("initial Apply intent is invalid");
  }
  if (expectation.expected_fence_generation.value() ==
      std::numeric_limits<std::uint64_t>::max()) {
    Fail("Apply-fence predecessor generation has no valid successor");
  }
  if (apply_intent.cell_id != apply_journal.cell_id()) {
    Fail("initial Apply intent belongs to a different journal cell");
  }
  const auto &old_value = predecessor.value();
  const auto &new_value = candidate.value();
  if (old_value.cell_graph.cell_id != apply_intent.cell_id ||
      new_value.cell_graph.cell_id != apply_intent.cell_id) {
    Fail("sealed Apply bundles do not match the durable intent cell");
  }
  if (predecessor.retained_bundle_id() !=
          apply_intent.expected_active_projection_revision.retained_bundle_id ||
      old_value.cell_graph.graph_revision !=
          apply_intent.expected_active_projection_revision.graph_revision) {
    Fail("sealed predecessor does not match the durable Apply intent");
  }
  if (candidate.retained_bundle_id() !=
          apply_intent.candidate_retained_bundle_id ||
      new_value.cell_graph.graph_revision !=
          apply_intent.candidate_graph_revision) {
    Fail("sealed candidate does not match the durable Apply intent");
  }

  const auto scope =
      manyforge::deployment::derive_apply_configuration_fence_scope(predecessor,
                                                                    candidate);
  ManagerApplyAffectedScopeDTO affected_scope{
      apply_intent.apply_id,
      apply_intent.apply_generation,
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
  };
  core::ApplyConfigurationFenceIntentDTO fence_intent{
      apply_intent.request_context,
      core::ApplyFenceOwnerDTO{apply_intent.apply_id,
                               apply_intent.apply_generation},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      apply_intent.expected_active_projection_revision.retained_bundle_id,
      apply_intent.expected_active_projection_revision.graph_revision,
      expectation.expected_operation_resource_generation,
      expectation.expected_fence_generation,
      expectation.expected_active_control_configurations,
      "component-manager named Apply affected-scope admission",
  };
  if (!affected_scope.valid() || !fence_intent.valid()) {
    Fail("Manager-owned Apply-fence expectation is incoherent with the "
         "derived scope");
  }
  return {std::move(affected_scope), std::move(fence_intent)};
}

[[nodiscard]] core::ApplyFenceAcquireAttemptQueryDTO
RecoveryQuery(const PersistedManagerApplyFenceAttempt &attempt,
              const core::RuntimeEpoch &manager_runtime_epoch) {
  core::ApplyFenceAcquireAttemptQueryDTO query{
      attempt.fence_intent,
      attempt.fence_attempt_digest,
      manager_runtime_epoch,
  };
  if (!query.valid()) {
    Fail("durable Apply-fence attempt cannot form a valid recovery query");
  }
  return query;
}

[[nodiscard]] ManagerApplyFenceAcceptanceWriteResult PersistAcceptance(
    SqliteManagerApplyJournal &journal,
    const PersistedManagerApplyFenceAttempt &attempt,
    const core::ConfigurationFenceAcceptedDTO &accepted_fence,
    const core::ApplyFenceAcquireAttemptQueryDTO &query,
    const bool acquire_invoked_in_this_call,
    const std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
        &observed_reconciliation) {
  try {
    return journal.EnsureFenceAcceptance(
        attempt.fence_intent.owner_transaction.apply_id,
        attempt.fence_intent.owner_transaction.apply_generation,
        attempt.fence_attempt_digest, accepted_fence);
  } catch (const std::exception &error) {
    FailUncertain(
        ExceptionDetail(
            "Kernel fence may be held but its acceptance could not be "
            "persisted",
            error),
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence,
        acquire_invoked_in_this_call, query, accepted_fence,
        observed_reconciliation);
  } catch (...) {
    FailUncertain(
        "Kernel fence may be held but acceptance persistence raised a "
        "non-standard exception",
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence,
        acquire_invoked_in_this_call, query, accepted_fence,
        observed_reconciliation);
  }
}

[[nodiscard]] ManagerApplyFenceAcceptanceWriteResult AcquireAndPersist(
    SqliteManagerApplyJournal &journal,
    core::IManagerKernelApplyFencePort &kernel_fence,
    const PersistedManagerApplyFenceAttempt &attempt,
    const core::ApplyFenceAcquireAttemptQueryDTO &query,
    const std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
        &observed_reconciliation) {
  std::optional<core::ConfigurationFenceAcceptedDTO> accepted_fence;
  try {
    accepted_fence = kernel_fence.Acquire(attempt.fence_intent);
  } catch (const std::exception &error) {
    FailUncertain(
        ExceptionDetail("Kernel Apply-fence Acquire outcome is unknown", error),
        ManagerApplyFenceUncertaintyPhase::kAcquireCall, true, query,
        std::nullopt, observed_reconciliation);
  } catch (...) {
    FailUncertain(
        "Kernel Apply-fence Acquire raised a non-standard exception; its "
        "outcome is unknown",
        ManagerApplyFenceUncertaintyPhase::kAcquireCall, true, query,
        std::nullopt, observed_reconciliation);
  }

  if (!accepted_fence->matches(attempt.fence_intent)) {
    FailUncertain(
        "Kernel returned an invalid or mismatched Apply-fence acceptance",
        ManagerApplyFenceUncertaintyPhase::kAcquireResponse, true, query,
        *accepted_fence, observed_reconciliation);
  }

  return PersistAcceptance(journal, attempt, *accepted_fence, query, true,
                           observed_reconciliation);
}

[[nodiscard]] ManagerApplyFenceAcquireResult RehydrateAndFinish(
    core::IManagerKernelApplyFencePort &kernel_fence,
    const core::RuntimeEpoch &manager_runtime_epoch,
    ManagerApplyIntentWriteResult durable_intent,
    ManagerApplyAffectedScopeWriteResult durable_affected_scope,
    ManagerApplyFenceAttemptWriteResult durable_attempt,
    ManagerApplyFenceAcceptanceWriteResult durable_acceptance,
    const ManagerApplyFenceAcquirePath path,
    const core::ApplyFenceAcquireAttemptQueryDTO &recovery_query,
    const std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
        &observed_attempt_reconciliation) {
  const bool acquire_invoked_in_this_call =
      path == ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt ||
      path == ManagerApplyFenceAcquirePath::kRetriedAfterAuthoritativeNotSeen;
  const auto &intent = durable_intent.record.intent;
  const auto &acceptance = durable_acceptance.record.accepted_fence;
  core::ApplyFenceRehydrateDTO request{
      intent.apply_id,
      intent.apply_generation,
      intent.request_context.request_id,
      intent.request_context.request_owner_epoch,
      durable_intent.record.intent_digest,
      intent.expected_active_projection_revision.retained_bundle_id,
      intent.expected_active_projection_revision.graph_revision,
      intent.candidate_retained_bundle_id,
      intent.candidate_graph_revision,
      acceptance.configuration_fence_id,
      acceptance.configuration_fence_generation,
      acceptance.captured_scope,
      "fence_accepted",
      manager_runtime_epoch,
  };
  if (!request.valid()) {
    FailUncertain(
        "durable Apply-fence records cannot form a valid rehydration request",
        ManagerApplyFenceUncertaintyPhase::kRehydrateResponse,
        acquire_invoked_in_this_call, recovery_query, acceptance,
        observed_attempt_reconciliation);
  }

  std::optional<core::ApplyFenceReconcileResultDTO> rehydration;
  try {
    rehydration = kernel_fence.Rehydrate(request);
  } catch (const std::exception &error) {
    FailUncertain(
        ExceptionDetail("Kernel Apply-fence rehydration failed", error),
        ManagerApplyFenceUncertaintyPhase::kRehydrateCall,
        acquire_invoked_in_this_call, recovery_query, acceptance,
        observed_attempt_reconciliation);
  } catch (...) {
    FailUncertain(
        "Kernel Apply-fence rehydration raised a non-standard exception",
        ManagerApplyFenceUncertaintyPhase::kRehydrateCall,
        acquire_invoked_in_this_call, recovery_query, acceptance,
        observed_attempt_reconciliation);
  }

  const bool reconciled =
      rehydration->matches(request) && !rehydration->admission_fenced &&
      !rehydration->error.has_value() &&
      rehydration->required_compensation_or_operator_direction.empty() &&
      rehydration->fence_results.size() == 1U &&
      (rehydration->fence_results.front().state ==
           core::ApplyFenceReconcileState::kMatched ||
       rehydration->fence_results.front().state ==
           core::ApplyFenceReconcileState::kRecreated);
  if (!reconciled) {
    FailUncertain(
        "Kernel returned invalid, unresolved, or fail-closed Apply-fence "
        "rehydration evidence",
        ManagerApplyFenceUncertaintyPhase::kRehydrateResponse,
        acquire_invoked_in_this_call, recovery_query, acceptance,
        observed_attempt_reconciliation, *rehydration);
  }

  return {
      std::move(durable_intent),
      std::move(durable_affected_scope),
      std::move(durable_attempt),
      std::move(durable_acceptance),
      std::move(request),
      std::move(*rehydration),
      path,
  };
}

} // namespace

ManagerApplyFenceEffectUncertainError::ManagerApplyFenceEffectUncertainError(
    std::string detail, const ManagerApplyFenceUncertaintyPhase phase,
    const bool acquire_invoked_in_this_call,
    core::ApplyFenceAcquireAttemptQueryDTO recovery_query,
    std::optional<core::ConfigurationFenceAcceptedDTO> observed_acceptance,
    std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
        observed_reconciliation,
    std::optional<core::ApplyFenceReconcileResultDTO> observed_rehydration)
    : ManagerApplyFenceCoordinatorError(std::move(detail)), phase_(phase),
      acquire_invoked_in_this_call_(acquire_invoked_in_this_call),
      recovery_query_(std::move(recovery_query)),
      observed_acceptance_(std::move(observed_acceptance)),
      observed_reconciliation_(std::move(observed_reconciliation)),
      observed_rehydration_(std::move(observed_rehydration)) {}

ManagerApplyFenceUncertaintyPhase
ManagerApplyFenceEffectUncertainError::phase() const noexcept {
  return phase_;
}

bool ManagerApplyFenceEffectUncertainError::acquire_invoked_in_this_call()
    const noexcept {
  return acquire_invoked_in_this_call_;
}

const core::ApplyFenceAcquireAttemptQueryDTO &
ManagerApplyFenceEffectUncertainError::recovery_query() const noexcept {
  return recovery_query_;
}

const std::optional<core::ConfigurationFenceAcceptedDTO> &
ManagerApplyFenceEffectUncertainError::observed_acceptance() const noexcept {
  return observed_acceptance_;
}

const std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO> &
ManagerApplyFenceEffectUncertainError::observed_reconciliation()
    const noexcept {
  return observed_reconciliation_;
}

const std::optional<core::ApplyFenceReconcileResultDTO> &
ManagerApplyFenceEffectUncertainError::observed_rehydration() const noexcept {
  return observed_rehydration_;
}

ManagerApplyFenceCoordinator::ManagerApplyFenceCoordinator(
    SqliteManagerApplyJournal &apply_journal,
    core::IManagerKernelApplyFencePort &kernel_fence,
    core::RuntimeEpoch manager_runtime_epoch)
    : apply_journal_(apply_journal), kernel_fence_(kernel_fence),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)) {
  if (manager_runtime_epoch_.empty()) {
    Fail("Manager runtime epoch is empty");
  }
}

ManagerApplyFenceAcquireResult
ManagerApplyFenceCoordinator::AcquireOrRecoverFence(
    const ManagerApplyIntentDTO &apply_intent,
    const manyforge::deployment::RetainedBundle &predecessor,
    const manyforge::deployment::RetainedBundle &candidate,
    const ManagerApplyFenceExpectationDTO &expectation) {
  std::lock_guard lock(operation_mutex_);
  auto prepared = PrepareFence(apply_journal_, apply_intent, predecessor,
                               candidate, expectation);

  // All three immutable commits complete before this coordinator can invoke
  // Acquire: public intent, derived whole-Apply scope/fixed disposition, and
  // the byte-exact request.
  auto durable_intent = apply_journal_.EnsureIntent(apply_intent);
  auto durable_affected_scope = apply_journal_.EnsureAffectedScope(
      durable_intent.record.intent_digest, prepared.affected_scope);
  auto durable_attempt = apply_journal_.EnsureFenceAttempt(
      durable_intent.record.intent_digest,
      durable_affected_scope.record.affected_scope_digest,
      prepared.fence_intent);
  const auto query =
      RecoveryQuery(durable_attempt.record, manager_runtime_epoch_);

  std::optional<PersistedManagerApplyFenceAcceptance> existing_acceptance;
  try {
    existing_acceptance = apply_journal_.ReadFenceAcceptance(
        apply_intent.apply_id, apply_intent.apply_generation);
  } catch (const std::exception &error) {
    if (durable_attempt.inserted) {
      throw;
    }
    FailUncertain(
        ExceptionDetail(
            "durable attempt exists but its acceptance could not be read",
            error),
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence, false,
        query);
  } catch (...) {
    if (durable_attempt.inserted) {
      throw;
    }
    FailUncertain(
        "durable attempt exists but acceptance read raised a non-standard "
        "exception",
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence, false,
        query);
  }

  if (existing_acceptance.has_value()) {
    if (existing_acceptance->fence_attempt_digest !=
            durable_attempt.record.fence_attempt_digest ||
        !existing_acceptance->accepted_fence.matches(
            durable_attempt.record.fence_intent)) {
      FailUncertain(
          "durable Apply-fence acceptance contradicts its exact attempt",
          ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence, false,
          query, existing_acceptance->accepted_fence);
    }
    return RehydrateAndFinish(
        kernel_fence_, manager_runtime_epoch_, std::move(durable_intent),
        std::move(durable_affected_scope), std::move(durable_attempt),
        ManagerApplyFenceAcceptanceWriteResult{std::move(*existing_acceptance),
                                               false},
        ManagerApplyFenceAcquirePath::kDurableAcceptanceReplay, query,
        std::nullopt);
  }

  if (durable_attempt.inserted) {
    auto durable_acceptance =
        AcquireAndPersist(apply_journal_, kernel_fence_, durable_attempt.record,
                          query, std::nullopt);
    return RehydrateAndFinish(
        kernel_fence_, manager_runtime_epoch_, std::move(durable_intent),
        std::move(durable_affected_scope), std::move(durable_attempt),
        std::move(durable_acceptance),
        ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt, query,
        std::nullopt);
  }

  std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
      reconciliation;
  try {
    reconciliation = kernel_fence_.ReconcileAcquireAttempt(query);
  } catch (const std::exception &error) {
    FailUncertain(
        ExceptionDetail("durable Apply-fence attempt reconciliation failed",
                        error),
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationCall, false,
        query);
  } catch (...) {
    FailUncertain(
        "durable Apply-fence attempt reconciliation raised a non-standard "
        "exception",
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationCall, false,
        query);
  }

  if (!reconciliation->matches(query)) {
    FailUncertain(
        "Kernel returned invalid or mismatched attempt-reconciliation "
        "evidence",
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse,
        false, query, std::nullopt, *reconciliation);
  }

  switch (reconciliation->state) {
  case core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact: {
    auto durable_acceptance = PersistAcceptance(
        apply_journal_, durable_attempt.record, *reconciliation->accepted_fence,
        query, false, reconciliation);
    return RehydrateAndFinish(
        kernel_fence_, manager_runtime_epoch_, std::move(durable_intent),
        std::move(durable_affected_scope), std::move(durable_attempt),
        std::move(durable_acceptance),
        ManagerApplyFenceAcquirePath::kRecoveredAcceptedExact, query,
        reconciliation);
  }
  case core::ApplyFenceAcquireAttemptReconcileState::kNotSeen: {
    auto durable_acceptance =
        AcquireAndPersist(apply_journal_, kernel_fence_, durable_attempt.record,
                          query, reconciliation);
    return RehydrateAndFinish(
        kernel_fence_, manager_runtime_epoch_, std::move(durable_intent),
        std::move(durable_affected_scope), std::move(durable_attempt),
        std::move(durable_acceptance),
        ManagerApplyFenceAcquirePath::kRetriedAfterAuthoritativeNotSeen, query,
        reconciliation);
  }
  case core::ApplyFenceAcquireAttemptReconcileState::kConflicting:
    FailUncertain(
        "Kernel reports a conflicting durable Acquire attempt",
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse,
        false, query, std::nullopt, *reconciliation);
  case core::ApplyFenceAcquireAttemptReconcileState::kUncertain:
    FailUncertain(
        "Kernel cannot prove the durable Acquire attempt outcome",
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse,
        false, query, std::nullopt, *reconciliation);
  }

  FailUncertain(
      "Kernel returned an unknown attempt-reconciliation state",
      ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse, false,
      query, std::nullopt, *reconciliation);
}

} // namespace manyforge::component_manager
