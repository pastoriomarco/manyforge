#pragma once

#include <cstdint>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::uint64_t kManagerRuntimeMaximumPresenceAgeNs =
    5'000'000'000ULL;
inline constexpr std::uint64_t kManagerRuntimeMaximumReportAgeNs =
    5'000'000'000ULL;

class ManagerRuntimeConvergenceCoordinatorError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Fail-closed result for an exact managed-process Ensure whose effect may
/// already have happened even though no usable response reached the Manager.
///
/// The durable convergence intent and complete adapter query are retained.
/// Recovery is therefore observe-first and never retries from exception text.
class ManagerRuntimeProcessEffectUncertainError final
    : public ManagerRuntimeConvergenceCoordinatorError {
public:
  ManagerRuntimeProcessEffectUncertainError(
      std::string detail, RuntimeUnitConvergenceQueryDTO recovery_query);

  [[nodiscard]] const RuntimeUnitConvergenceQueryDTO &
  recovery_query() const noexcept;

private:
  RuntimeUnitConvergenceQueryDTO recovery_query_;
};

enum class ManagerRuntimeConvergenceResultState {
  kPending,
  kStagedReady,
};

struct ManagerApplyRuntimeConvergenceResult final {
  ManagerApplyRuntimeConvergenceIntentWriteResult durable_runtime_intent;
  RuntimeObservationSnapshot observations;
  std::optional<ManagerApplyRuntimeStagedReadyWriteResult> durable_staged_ready;
  ManagerRuntimeConvergenceResultState state;
};

/// Effect-free verification and canonical plan projection for every retained
/// runtime unit in one exact bundle. Bootstrap and named Apply use the same
/// product/host compatibility rules; only their durable transaction owners
/// differ.
[[nodiscard]] std::vector<RuntimeUnitConvergencePlanRowDTO>
BuildVerifiedRuntimeConvergencePlan(
    const manyforge::deployment::RetainedBundle &bundle,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string_view operating_system,
    std::vector<std::string> launch_environment_contracts);

/// Bounded named-Apply runtime convergence coordinator.
///
/// The complete candidate and predecessor runtime products are verified and
/// the current predecessor publication is read before the fence coordinator
/// may perform an effect. After accepted-fence rehydration, one immutable
/// aggregate convergence intent is committed before any managed-process
/// Ensure. Every unit is observed first and evaluated through the sole C++
/// runtime reducer in dependency order. A complete staged-ready set is
/// persisted only after the exact predecessor public pointer is independently
/// re-read as the current head.
///
/// Product verification is generic only across the closed build-qualified
/// runtime-product registry. The selected process/progress ports still own the
/// concrete adapter and may never interpret a retained product as an arbitrary
/// executable tuple. This coordinator does not provide an external-runtime
/// fallback, process retirement, scene mutation, activation, publication, or
/// fence release.
class ManagerApplyRuntimeConvergenceCoordinator final {
public:
  ManagerApplyRuntimeConvergenceCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      IRuntimeUnitProcessPort &process_port,
      IRuntimeUnitProgressPort &progress_port, std::string operating_system,
      std::string shell_contract, RuntimeObservationReducer::Clock clock_ns,
      RuntimePreStartGuard pre_start_guard = {});
  ManagerApplyRuntimeConvergenceCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      IRuntimeUnitProcessPort &process_port,
      IRuntimeUnitProgressPort &progress_port, std::string operating_system,
      std::vector<std::string> launch_environment_contracts,
      RuntimeObservationReducer::Clock clock_ns,
      RuntimePreStartGuard pre_start_guard = {});

  [[nodiscard]] ManagerApplyRuntimeConvergenceResult
  ConvergeOrRecover(const ManagerApplyIntentDTO &apply_intent,
                    const manyforge::deployment::RetainedBundle &predecessor,
                    const manyforge::deployment::RetainedBundle &candidate,
                    manyforge::core::ActiveBundlePublicationGeneration
                        predecessor_publication_generation);

  ManagerApplyRuntimeConvergenceCoordinator(
      const ManagerApplyRuntimeConvergenceCoordinator &) = delete;
  ManagerApplyRuntimeConvergenceCoordinator &
  operator=(const ManagerApplyRuntimeConvergenceCoordinator &) = delete;

private:
  SqliteManagerApplyJournal &apply_journal_;
  const manyforge::core::IActiveBundlePublicationReader &publication_reader_;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource
      &artifact_source_;
  IRuntimeUnitProcessPort &process_port_;
  IRuntimeUnitProgressPort &progress_port_;
  std::string operating_system_;
  std::vector<std::string> launch_environment_contracts_;
  RuntimeObservationReducer::Clock clock_ns_;
  RuntimePreStartGuard pre_start_guard_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
