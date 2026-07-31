#include "manyforge_component_manager/manager_apply_fence_coordinator.hpp"

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;

[[noreturn]] void Fail(const std::string& detail) {
  throw ManagerApplyFenceCoordinatorError(detail);
}

[[nodiscard]] std::string ExceptionDetail(
    const std::string& prefix,
    const std::exception& error) {
  return prefix + ": " + error.what();
}

[[noreturn]] void FailUncertain(
    std::string detail,
    const ManagerApplyFenceUncertaintyPhase phase,
    const bool acquire_invoked_in_this_call,
    const core::ApplyFenceAcquireAttemptQueryDTO& query,
    std::optional<core::ConfigurationFenceAcceptedDTO> observed_acceptance =
        std::nullopt,
    std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
        observed_reconciliation = std::nullopt) {
  throw ManagerApplyFenceEffectUncertainError(
      std::move(detail),
      phase,
      acquire_invoked_in_this_call,
      query,
      std::move(observed_acceptance),
      std::move(observed_reconciliation));
}

void ValidateCoherentInputs(
    const SqliteManagerApplyJournal& apply_journal,
    const ManagerApplyIntentDTO& apply_intent,
    const core::ApplyConfigurationFenceIntentDTO& fence_intent) {
  if (!apply_intent.valid()) {
    Fail("initial Apply intent is invalid");
  }
  if (!fence_intent.valid()) {
    Fail("Apply-fence intent is invalid");
  }
  if (fence_intent.expected_fence_generation.value() ==
      std::numeric_limits<std::uint64_t>::max()) {
    Fail("Apply-fence predecessor generation has no valid successor");
  }
  if (apply_intent.cell_id != apply_journal.cell_id()) {
    Fail("initial Apply intent belongs to a different journal cell");
  }
  if (fence_intent.owner_request_context != apply_intent.request_context) {
    Fail("Apply-fence request identity does not match the durable intent");
  }
  if (fence_intent.owner_transaction.apply_id != apply_intent.apply_id ||
      fence_intent.owner_transaction.apply_generation !=
          apply_intent.apply_generation) {
    Fail("Apply-fence owner does not match the durable Apply generation");
  }
  if (fence_intent.expected_retained_bundle_id !=
          apply_intent.expected_active_projection_revision
              .retained_bundle_id ||
      fence_intent.expected_graph_revision !=
          apply_intent.expected_active_projection_revision.graph_revision) {
    Fail("Apply-fence predecessor does not match the durable intent");
  }
}

[[nodiscard]] core::ApplyFenceAcquireAttemptQueryDTO RecoveryQuery(
    const PersistedManagerApplyFenceAttempt& attempt,
    const core::RuntimeEpoch& manager_runtime_epoch) {
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

[[nodiscard]] ManagerApplyFenceAcquireResult Result(
    ManagerApplyIntentWriteResult durable_intent,
    ManagerApplyFenceAttemptWriteResult durable_attempt,
    ManagerApplyFenceAcceptanceWriteResult durable_acceptance,
    const ManagerApplyFenceAcquirePath path) {
  return {
      std::move(durable_intent),
      std::move(durable_attempt),
      std::move(durable_acceptance),
      path,
  };
}

[[nodiscard]] ManagerApplyFenceAcceptanceWriteResult PersistAcceptance(
    SqliteManagerApplyJournal& journal,
    const PersistedManagerApplyFenceAttempt& attempt,
    const core::ConfigurationFenceAcceptedDTO& accepted_fence,
    const core::ApplyFenceAcquireAttemptQueryDTO& query,
    const bool acquire_invoked_in_this_call,
    const std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>&
        observed_reconciliation) {
  try {
    return journal.EnsureFenceAcceptance(
        attempt.fence_intent.owner_transaction.apply_id,
        attempt.fence_intent.owner_transaction.apply_generation,
        attempt.fence_attempt_digest,
        accepted_fence);
  } catch (const std::exception& error) {
    FailUncertain(
        ExceptionDetail(
            "Kernel fence may be held but its acceptance could not be "
            "persisted",
            error),
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence,
        acquire_invoked_in_this_call,
        query,
        accepted_fence,
        observed_reconciliation);
  } catch (...) {
    FailUncertain(
        "Kernel fence may be held but acceptance persistence raised a "
        "non-standard exception",
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence,
        acquire_invoked_in_this_call,
        query,
        accepted_fence,
        observed_reconciliation);
  }
}

[[nodiscard]] ManagerApplyFenceAcceptanceWriteResult AcquireAndPersist(
    SqliteManagerApplyJournal& journal,
    core::IManagerKernelApplyFenceAcquireRehydratePort& kernel_fence,
    const PersistedManagerApplyFenceAttempt& attempt,
    const core::ApplyFenceAcquireAttemptQueryDTO& query,
    const std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>&
        observed_reconciliation) {
  std::optional<core::ConfigurationFenceAcceptedDTO> accepted_fence;
  try {
    accepted_fence = kernel_fence.Acquire(attempt.fence_intent);
  } catch (const std::exception& error) {
    FailUncertain(
        ExceptionDetail("Kernel Apply-fence Acquire outcome is unknown", error),
        ManagerApplyFenceUncertaintyPhase::kAcquireCall,
        true,
        query,
        std::nullopt,
        observed_reconciliation);
  } catch (...) {
    FailUncertain(
        "Kernel Apply-fence Acquire raised a non-standard exception; its "
        "outcome is unknown",
        ManagerApplyFenceUncertaintyPhase::kAcquireCall,
        true,
        query,
        std::nullopt,
        observed_reconciliation);
  }

  if (!accepted_fence->matches(attempt.fence_intent)) {
    FailUncertain(
        "Kernel returned an invalid or mismatched Apply-fence acceptance",
        ManagerApplyFenceUncertaintyPhase::kAcquireResponse,
        true,
        query,
        *accepted_fence,
        observed_reconciliation);
  }

  return PersistAcceptance(
      journal,
      attempt,
      *accepted_fence,
      query,
      true,
      observed_reconciliation);
}

}  // namespace

ManagerApplyFenceEffectUncertainError::
    ManagerApplyFenceEffectUncertainError(
        std::string detail,
        const ManagerApplyFenceUncertaintyPhase phase,
        const bool acquire_invoked_in_this_call,
        core::ApplyFenceAcquireAttemptQueryDTO recovery_query,
        std::optional<core::ConfigurationFenceAcceptedDTO>
            observed_acceptance,
        std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
            observed_reconciliation)
    : ManagerApplyFenceCoordinatorError(std::move(detail)),
      phase_(phase),
      acquire_invoked_in_this_call_(acquire_invoked_in_this_call),
      recovery_query_(std::move(recovery_query)),
      observed_acceptance_(std::move(observed_acceptance)),
      observed_reconciliation_(std::move(observed_reconciliation)) {}

ManagerApplyFenceUncertaintyPhase
ManagerApplyFenceEffectUncertainError::phase() const noexcept {
  return phase_;
}

bool ManagerApplyFenceEffectUncertainError::acquire_invoked_in_this_call()
    const noexcept {
  return acquire_invoked_in_this_call_;
}

const core::ApplyFenceAcquireAttemptQueryDTO&
ManagerApplyFenceEffectUncertainError::recovery_query() const noexcept {
  return recovery_query_;
}

const std::optional<core::ConfigurationFenceAcceptedDTO>&
ManagerApplyFenceEffectUncertainError::observed_acceptance() const noexcept {
  return observed_acceptance_;
}

const std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>&
ManagerApplyFenceEffectUncertainError::observed_reconciliation()
    const noexcept {
  return observed_reconciliation_;
}

ManagerApplyFenceCoordinator::ManagerApplyFenceCoordinator(
    SqliteManagerApplyJournal& apply_journal,
    core::IManagerKernelApplyFenceAcquireRehydratePort& kernel_fence,
    core::RuntimeEpoch manager_runtime_epoch)
    : apply_journal_(apply_journal),
      kernel_fence_(kernel_fence),
      manager_runtime_epoch_(std::move(manager_runtime_epoch)) {
  if (manager_runtime_epoch_.empty()) {
    Fail("Manager runtime epoch is empty");
  }
}

ManagerApplyFenceAcquireResult
ManagerApplyFenceCoordinator::AcquireOrRecoverFence(
    const ManagerApplyIntentDTO& apply_intent,
    const core::ApplyConfigurationFenceIntentDTO& fence_intent) {
  std::lock_guard lock(operation_mutex_);
  ValidateCoherentInputs(apply_journal_, apply_intent, fence_intent);

  // Both commits complete before this coordinator can invoke Acquire. This is
  // the exact ordering guarantee owned here; no claim is made about effects
  // outside this coordinator.
  auto durable_intent = apply_journal_.EnsureIntent(apply_intent);
  auto durable_attempt = apply_journal_.EnsureFenceAttempt(
      durable_intent.record.intent_digest, fence_intent);
  const auto query =
      RecoveryQuery(durable_attempt.record, manager_runtime_epoch_);

  std::optional<PersistedManagerApplyFenceAcceptance> existing_acceptance;
  try {
    existing_acceptance = apply_journal_.ReadFenceAcceptance(
        apply_intent.apply_id, apply_intent.apply_generation);
  } catch (const std::exception& error) {
    if (durable_attempt.inserted) {
      throw;
    }
    FailUncertain(
        ExceptionDetail(
            "durable attempt exists but its acceptance could not be read",
            error),
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence,
        false,
        query);
  } catch (...) {
    if (durable_attempt.inserted) {
      throw;
    }
    FailUncertain(
        "durable attempt exists but acceptance read raised a non-standard "
        "exception",
        ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence,
        false,
        query);
  }

  if (existing_acceptance.has_value()) {
    if (existing_acceptance->fence_attempt_digest !=
            durable_attempt.record.fence_attempt_digest ||
        !existing_acceptance->accepted_fence.matches(
            durable_attempt.record.fence_intent)) {
      FailUncertain(
          "durable Apply-fence acceptance contradicts its exact attempt",
          ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence,
          false,
          query,
          existing_acceptance->accepted_fence);
    }
    return Result(
        std::move(durable_intent),
        std::move(durable_attempt),
        ManagerApplyFenceAcceptanceWriteResult{
            std::move(*existing_acceptance), false},
        ManagerApplyFenceAcquirePath::kDurableAcceptanceReplay);
  }

  if (durable_attempt.inserted) {
    auto durable_acceptance = AcquireAndPersist(
        apply_journal_,
        kernel_fence_,
        durable_attempt.record,
        query,
        std::nullopt);
    return Result(
        std::move(durable_intent),
        std::move(durable_attempt),
        std::move(durable_acceptance),
        ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt);
  }

  std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO>
      reconciliation;
  try {
    reconciliation = kernel_fence_.ReconcileAcquireAttempt(query);
  } catch (const std::exception& error) {
    FailUncertain(
        ExceptionDetail(
            "durable Apply-fence attempt reconciliation failed", error),
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationCall,
        false,
        query);
  } catch (...) {
    FailUncertain(
        "durable Apply-fence attempt reconciliation raised a non-standard "
        "exception",
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationCall,
        false,
        query);
  }

  if (!reconciliation->matches(query)) {
    FailUncertain(
        "Kernel returned invalid or mismatched attempt-reconciliation "
        "evidence",
        ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse,
        false,
        query,
        std::nullopt,
        *reconciliation);
  }

  switch (reconciliation->state) {
    case core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact: {
      auto durable_acceptance = PersistAcceptance(
          apply_journal_,
          durable_attempt.record,
          *reconciliation->accepted_fence,
          query,
          false,
          reconciliation);
      return Result(
          std::move(durable_intent),
          std::move(durable_attempt),
          std::move(durable_acceptance),
          ManagerApplyFenceAcquirePath::kRecoveredAcceptedExact);
    }
    case core::ApplyFenceAcquireAttemptReconcileState::kNotSeen: {
      auto durable_acceptance = AcquireAndPersist(
          apply_journal_,
          kernel_fence_,
          durable_attempt.record,
          query,
          reconciliation);
      return Result(
          std::move(durable_intent),
          std::move(durable_attempt),
          std::move(durable_acceptance),
          ManagerApplyFenceAcquirePath::kRetriedAfterAuthoritativeNotSeen);
    }
    case core::ApplyFenceAcquireAttemptReconcileState::kConflicting:
      FailUncertain(
          "Kernel reports a conflicting durable Acquire attempt",
          ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse,
          false,
          query,
          std::nullopt,
          *reconciliation);
    case core::ApplyFenceAcquireAttemptReconcileState::kUncertain:
      FailUncertain(
          "Kernel cannot prove the durable Acquire attempt outcome",
          ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse,
          false,
          query,
          std::nullopt,
          *reconciliation);
  }

  FailUncertain(
      "Kernel returned an unknown attempt-reconciliation state",
      ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse,
      false,
      query,
      std::nullopt,
      *reconciliation);
}

}  // namespace manyforge::component_manager
