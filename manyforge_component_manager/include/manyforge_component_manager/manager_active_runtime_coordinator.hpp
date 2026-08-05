#pragma once

#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_component_manager/runtime_observation.hpp>

#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

namespace manyforge::component_manager {

/// Exact runtime plan selected by the current durable public pointer.
///
/// Bootstrap selects the canonical-empty plan and therefore has neither a
/// runtime-intent digest nor rows. A named Apply carries the exact immutable
/// convergence intent originally committed before its first process effect.
struct ManagerActiveRuntimePlanDTO final {
  manyforge::core::ActiveBundlePublicationReceiptDTO active_publication;
  std::optional<manyforge::core::ContentDigest>
      runtime_convergence_intent_digest;
  std::vector<RuntimeUnitConvergencePlanRowDTO> runtime_units;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerActiveRuntimePlanDTO &,
                         const ManagerActiveRuntimePlanDTO &) = default;
};

class IManagerActiveRuntimePlanSource {
public:
  virtual ~IManagerActiveRuntimePlanSource() = default;

  [[nodiscard]] virtual ManagerActiveRuntimePlanDTO ReadCurrentPlan() const = 0;
};

/// Production resolver from the sole active-publication owner and Apply
/// journal. The integrity read discovers a generation only; the subsequent
/// generation-bound reader call supplies authoritative current-head evidence.
class DurableManagerActiveRuntimePlanSource final
    : public IManagerActiveRuntimePlanSource {
public:
  DurableManagerActiveRuntimePlanSource(
      const SqliteActiveBundlePublicationStore &publication_store,
      const SqliteManagerApplyJournal &apply_journal);

  [[nodiscard]] ManagerActiveRuntimePlanDTO ReadCurrentPlan() const override;

private:
  const SqliteActiveBundlePublicationStore &publication_store_;
  const SqliteManagerApplyJournal &apply_journal_;
};

class ManagerActiveRuntimeCoordinatorError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

enum class ManagerActiveRuntimeProcessEffect {
  kStart,
  kStop,
};

/// A start/stop call may have taken effect even though the Manager did not
/// receive a usable response. Recovery must observe this exact query first.
class ManagerActiveRuntimeProcessEffectUncertainError final
    : public ManagerActiveRuntimeCoordinatorError {
public:
  ManagerActiveRuntimeProcessEffectUncertainError(
      std::string detail, ManagerActiveRuntimeProcessEffect effect,
      RuntimeUnitConvergenceQueryDTO recovery_query);

  [[nodiscard]] ManagerActiveRuntimeProcessEffect effect() const noexcept;
  [[nodiscard]] const RuntimeUnitConvergenceQueryDTO &
  recovery_query() const noexcept;

private:
  ManagerActiveRuntimeProcessEffect effect_;
  RuntimeUnitConvergenceQueryDTO recovery_query_;
};

enum class ManagerActiveRuntimeConvergenceState {
  kPending,
  kConverged,
};

struct ManagerActiveRuntimeConvergenceResult final {
  ManagerActiveRuntimePlanDTO plan;
  RuntimeObservationSnapshot observations;
  ManagerActiveRuntimeConvergenceState state;
};

struct ManagerActiveRuntimeShutdownResult final {
  ManagerActiveRuntimePlanDTO plan;
  std::vector<ProcessPresenceObservation> final_process_observations;
};

/// Steady-state runtime-unit lifecycle owner for the currently selected plan.
///
/// Reconcile is dependency ordered and observe-first. Explicit Shutdown is
/// reverse-dependency ordered and uses only the closed process-port shutdown
/// operation. Destroying this coordinator has no process effect.
class ManagerActiveRuntimeCoordinator final {
public:
  ManagerActiveRuntimeCoordinator(
      const IManagerActiveRuntimePlanSource &plan_source,
      IRuntimeUnitProcessPort &process_port,
      IRuntimeUnitProgressPort &progress_port,
      RuntimeObservationReducer::Clock clock_ns);

  [[nodiscard]] ManagerActiveRuntimeConvergenceResult Reconcile();
  [[nodiscard]] ManagerActiveRuntimeShutdownResult Shutdown();

  ManagerActiveRuntimeCoordinator(const ManagerActiveRuntimeCoordinator &) =
      delete;
  ManagerActiveRuntimeCoordinator &
  operator=(const ManagerActiveRuntimeCoordinator &) = delete;

private:
  const IManagerActiveRuntimePlanSource &plan_source_;
  IRuntimeUnitProcessPort &process_port_;
  IRuntimeUnitProgressPort &progress_port_;
  RuntimeObservationReducer::Clock clock_ns_;
  std::mutex operation_mutex_;
  std::optional<ManagerActiveRuntimePlanDTO> observation_plan_;
  std::optional<RuntimeObservationReducer> observation_reducer_;
};

} // namespace manyforge::component_manager
