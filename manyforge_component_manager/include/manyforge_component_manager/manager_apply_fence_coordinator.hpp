#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

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
  kRehydrateCall,
  kRehydrateResponse,
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
      std::string detail, ManagerApplyFenceUncertaintyPhase phase,
      bool acquire_invoked_in_this_call,
      manyforge::core::ApplyFenceAcquireAttemptQueryDTO recovery_query,
      std::optional<manyforge::core::ConfigurationFenceAcceptedDTO>
          observed_acceptance = std::nullopt,
      std::optional<manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO>
          observed_reconciliation = std::nullopt,
      std::optional<manyforge::core::ApplyFenceReconcileResultDTO>
          observed_rehydration = std::nullopt);

  [[nodiscard]] ManagerApplyFenceUncertaintyPhase phase() const noexcept;
  [[nodiscard]] bool acquire_invoked_in_this_call() const noexcept;
  [[nodiscard]] const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &
  recovery_query() const noexcept;
  [[nodiscard]] const std::optional<
      manyforge::core::ConfigurationFenceAcceptedDTO> &
  observed_acceptance() const noexcept;
  [[nodiscard]] const std::optional<
      manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO> &
  observed_reconciliation() const noexcept;
  [[nodiscard]] const std::optional<
      manyforge::core::ApplyFenceReconcileResultDTO> &
  observed_rehydration() const noexcept;

private:
  ManagerApplyFenceUncertaintyPhase phase_;
  bool acquire_invoked_in_this_call_;
  manyforge::core::ApplyFenceAcquireAttemptQueryDTO recovery_query_;
  std::optional<manyforge::core::ConfigurationFenceAcceptedDTO>
      observed_acceptance_;
  std::optional<manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO>
      observed_reconciliation_;
  std::optional<manyforge::core::ApplyFenceReconcileResultDTO>
      observed_rehydration_;
};

enum class ManagerApplyFenceAcquirePath {
  kAcquiredFromNewAttempt,
  kRecoveredAcceptedExact,
  kRetriedAfterAuthoritativeNotSeen,
  kDurableAcceptanceReplay,
};

struct ManagerApplyFenceAcquireResult final {
  ManagerApplyIntentWriteResult durable_intent;
  ManagerApplyAffectedScopeWriteResult durable_affected_scope;
  ManagerApplyFenceAttemptWriteResult durable_attempt;
  ManagerApplyFenceAcceptanceWriteResult durable_acceptance;
  manyforge::core::ApplyFenceRehydrateDTO rehydrate_request;
  manyforge::core::ApplyFenceReconcileResultDTO rehydration;
  ManagerApplyFenceAcquirePath path;
};

/// Manager-owned exact Kernel generation/configuration observation used to
/// construct the fixed fence request. The optional actual scope is the trusted
/// current-owner convergence closure, captured before the first fence attempt.
/// Disposition, request identity, graph and bundle remain fixed by the Apply.
struct ManagerApplyFenceExpectationDTO final {
  std::uint64_t expected_operation_resource_generation;
  manyforge::core::ConfigurationFenceGeneration expected_fence_generation;
  std::vector<manyforge::core::DomainActiveControlConfigurationDTO>
      expected_active_control_configurations;
  std::optional<manyforge::core::ConfigurationFenceScopeDTO> actual_scope{};
  std::vector<manyforge::core::ProviderConfigurationFenceLineageDTO>
      expected_provider_lineages{};
};

/// Bounded durable pre-effect coordinator for one named Apply fence.
///
/// Every invocation verifies the exact sealed predecessor/candidate bundles,
/// derives and commits one immutable whole-Apply scope with the fixed busy
/// disposition, commits the exact fence attempt, and returns only after the
/// complete accepted fence is durable and rehydrated. An existing accepted
/// record never repeats Acquire. An attempt-only restart first performs the
/// read-only Kernel reconciliation query; only authoritative `not_seen` permits
/// an exact Acquire retry, while `accepted_exact` is durably appended directly.
/// Conflict, uncertainty, malformed evidence, or journal failure after a
/// possible effect remains explicitly effect-uncertain.
///
/// This coordinator owns the complete pre-effect admission boundary through
/// rehydration. It does not converge processes, stage/activate a projection,
/// publish a bundle, release a fence, or perform later Apply phases.
class ManagerApplyFenceCoordinator final {
public:
  ManagerApplyFenceCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      manyforge::core::IManagerKernelApplyFencePort
          &kernel_fence,
      manyforge::core::RuntimeEpoch manager_runtime_epoch);

  [[nodiscard]] ManagerApplyFenceAcquireResult AcquireOrRecoverFence(
      const ManagerApplyIntentDTO &apply_intent,
      const manyforge::deployment::RetainedBundle &predecessor,
      const manyforge::deployment::RetainedBundle &candidate,
      const ManagerApplyFenceExpectationDTO &expectation);

  ManagerApplyFenceCoordinator(const ManagerApplyFenceCoordinator &) = delete;
  ManagerApplyFenceCoordinator &
  operator=(const ManagerApplyFenceCoordinator &) = delete;

private:
  SqliteManagerApplyJournal &apply_journal_;
  manyforge::core::IManagerKernelApplyFencePort &kernel_fence_;
  manyforge::core::RuntimeEpoch manager_runtime_epoch_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
