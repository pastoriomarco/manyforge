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

/// Exact effect-free runtime plan bound to the selected canonical predecessor.
/// Its digest is the sealed admission digest, not a fabricated Apply intent.
struct ManagerBootstrapRuntimePlanDTO final {
  manyforge::core::RetainedBundleId retained_bundle_id;
  manyforge::core::GraphRevision graph_revision;
  manyforge::core::ContentDigest admission_digest;
  std::vector<RuntimeUnitConvergencePlanRowDTO> runtime_units;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerBootstrapRuntimePlanDTO &,
                         const ManagerBootstrapRuntimePlanDTO &) = default;
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
      const SqliteManagerApplyJournal &apply_journal,
      std::optional<ManagerBootstrapRuntimePlanDTO> bootstrap_plan =
          std::nullopt);

  [[nodiscard]] ManagerActiveRuntimePlanDTO ReadCurrentPlan() const override;

private:
  const SqliteActiveBundlePublicationStore &publication_store_;
  const SqliteManagerApplyJournal &apply_journal_;
  std::optional<ManagerBootstrapRuntimePlanDTO> bootstrap_plan_;
  /// Last named-Apply plan accepted by ReadCurrentPlan.
  mutable std::mutex verified_plan_mutex_;
  mutable std::optional<ManagerActiveRuntimePlanDTO> verified_plan_;
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
/// Observation and process mutation are two explicit surfaces, both dependency
/// ordered and observe-first:
///
/// - Observe() calls only ObserveProcess/ObserveProgress. A missing
///   Manager-managed runtime unit is reported as non-converged fault evidence
///   and is never started. This is the surface for periodic runtime/readiness
///   observation and the public reconcile command; independent liveness
///   renewal may consume its snapshot but performs no lifecycle effect.
/// - Initialize() may additionally call EnsureRunning for absent
///   Manager-managed runtime units in dependency order. It is reserved for
///   bootstrap and for an admitted maintenance operation that already holds
///   the exact operation admission.
///
/// There is deliberately no boolean selector and no background repair loop,
/// retry timer or replacement policy: a caller chooses behaviour by naming the
/// surface it needs. Explicit Shutdown is reverse-dependency ordered and uses
/// only the closed process-port shutdown operation. Destroying this
/// coordinator has no process effect.
class ManagerActiveRuntimeCoordinator final {
public:
  ManagerActiveRuntimeCoordinator(
      const IManagerActiveRuntimePlanSource &plan_source,
      IRuntimeUnitProcessPort &process_port,
      IRuntimeUnitProgressPort &progress_port,
      RuntimeObservationReducer::Clock clock_ns,
      RuntimePreStartGuard pre_start_guard = {});

  /// Effect-free evaluation of the current plan. Never starts a process.
  [[nodiscard]] ManagerActiveRuntimeConvergenceResult Observe();

  /// Effect-permitted evaluation. Starts absent Manager-managed runtime units
  /// in dependency order. Only bootstrap and an admitted maintenance operation
  /// may call this.
  [[nodiscard]] ManagerActiveRuntimeConvergenceResult Initialize();

  [[nodiscard]] ManagerActiveRuntimeShutdownResult Shutdown();

  ManagerActiveRuntimeCoordinator(const ManagerActiveRuntimeCoordinator &) =
      delete;
  ManagerActiveRuntimeCoordinator &
  operator=(const ManagerActiveRuntimeCoordinator &) = delete;

private:
  enum class ConvergenceMode {
    kObserveOnly,
    kInitialize,
  };

  [[nodiscard]] ManagerActiveRuntimeConvergenceResult
  Converge(ConvergenceMode mode);

  const IManagerActiveRuntimePlanSource &plan_source_;
  IRuntimeUnitProcessPort &process_port_;
  IRuntimeUnitProgressPort &progress_port_;
  RuntimeObservationReducer::Clock clock_ns_;
  RuntimePreStartGuard pre_start_guard_;
  std::mutex operation_mutex_;
  std::optional<ManagerActiveRuntimePlanDTO> observation_plan_;
  std::optional<RuntimeObservationReducer> observation_reducer_;
};

} // namespace manyforge::component_manager
