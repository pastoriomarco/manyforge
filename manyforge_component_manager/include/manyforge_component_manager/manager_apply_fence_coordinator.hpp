#pragma once

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>

namespace manyforge::component_manager {

class ManagerApplyFenceCoordinatorError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

enum class ManagerApplyFenceUncertaintyPhase {
  kAttemptReconciliationCall,
  kAttemptReconciliationResponse,
  kAcquireCall,
  kAcquireResponse,
  kAcceptancePersistence,
};

/// Programmatically distinct fail-closed result for an Acquire that may
/// already have taken effect, or for a recovered attempt whose exact effect
/// cannot yet be proven.
///
/// The exact durable-attempt query is always retained. A Kernel response or
/// reconciliation result is retained whenever one was observed, so recovery
/// code does not have to infer what happened from an exception string.
class ManagerApplyFenceEffectUncertainError final
    : public ManagerApplyFenceCoordinatorError {
 public:
  ManagerApplyFenceEffectUncertainError(
      std::string detail,
      ManagerApplyFenceUncertaintyPhase phase,
      bool acquire_invoked_in_this_call,
      manyforge::core::ApplyFenceAcquireAttemptQueryDTO recovery_query,
      std::optional<manyforge::core::ConfigurationFenceAcceptedDTO>
          observed_acceptance = std::nullopt,
      std::optional<
          manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO>
          observed_reconciliation = std::nullopt);

  [[nodiscard]] ManagerApplyFenceUncertaintyPhase phase() const noexcept;
  [[nodiscard]] bool acquire_invoked_in_this_call() const noexcept;
  [[nodiscard]] const manyforge::core::ApplyFenceAcquireAttemptQueryDTO&
  recovery_query() const noexcept;
  [[nodiscard]] const std::optional<
      manyforge::core::ConfigurationFenceAcceptedDTO>&
  observed_acceptance() const noexcept;
  [[nodiscard]] const std::optional<
      manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO>&
  observed_reconciliation() const noexcept;

 private:
  ManagerApplyFenceUncertaintyPhase phase_;
  bool acquire_invoked_in_this_call_;
  manyforge::core::ApplyFenceAcquireAttemptQueryDTO recovery_query_;
  std::optional<manyforge::core::ConfigurationFenceAcceptedDTO>
      observed_acceptance_;
  std::optional<
      manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO>
      observed_reconciliation_;
};

enum class ManagerApplyFenceAcquirePath {
  kAcquiredFromNewAttempt,
  kRecoveredAcceptedExact,
  kRetriedAfterAuthoritativeNotSeen,
  kDurableAcceptanceReplay,
};

struct ManagerApplyFenceAcquireResult final {
  ManagerApplyIntentWriteResult durable_intent;
  ManagerApplyFenceAttemptWriteResult durable_attempt;
  ManagerApplyFenceAcceptanceWriteResult durable_acceptance;
  ManagerApplyFenceAcquirePath path;
};

/// Bounded durable pre-effect coordinator for one named Apply fence.
///
/// Every invocation verifies one coherent Apply/fence input, commits the
/// immutable Apply intent and exact fence attempt, and returns only after the
/// complete accepted fence is durable. An existing accepted record is an
/// effect-free replay. An attempt-only restart first performs the read-only
/// Kernel reconciliation query; only authoritative `not_seen` permits an
/// exact Acquire retry, while `accepted_exact` is durably appended directly.
/// Conflict, uncertainty, malformed evidence, or journal failure after a
/// possible effect remains explicitly effect-uncertain.
///
/// This coordinator owns commit-before-each-Acquire it invokes. It does not
/// claim global first-effect ownership, calculate affected scope, implement
/// reject-if-busy, converge processes, stage/activate a projection, publish a
/// bundle, release or rehydrate an accepted fence, or perform later Apply
/// phases.
class ManagerApplyFenceCoordinator final {
 public:
  ManagerApplyFenceCoordinator(
      SqliteManagerApplyJournal& apply_journal,
      manyforge::core::IManagerKernelApplyFenceAcquireRehydratePort&
          kernel_fence,
      manyforge::core::RuntimeEpoch manager_runtime_epoch);

  [[nodiscard]] ManagerApplyFenceAcquireResult AcquireOrRecoverFence(
      const ManagerApplyIntentDTO& apply_intent,
      const manyforge::core::ApplyConfigurationFenceIntentDTO& fence_intent);

  ManagerApplyFenceCoordinator(const ManagerApplyFenceCoordinator&) = delete;
  ManagerApplyFenceCoordinator& operator=(
      const ManagerApplyFenceCoordinator&) = delete;

 private:
  SqliteManagerApplyJournal& apply_journal_;
  manyforge::core::IManagerKernelApplyFenceAcquireRehydratePort& kernel_fence_;
  manyforge::core::RuntimeEpoch manager_runtime_epoch_;
  std::mutex operation_mutex_;
};

}  // namespace manyforge::component_manager
