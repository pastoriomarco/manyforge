#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/runtime_profile.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::size_t kMaximumRuntimeStagedReadySets = 4096U;
inline constexpr std::size_t kMaximumRuntimeStagedReadySetsPerApply = 16U;

enum class RuntimeUnitCandidateDisposition {
  kAdded,
  kRetainedExact,
};

/// One exact, independently verified candidate runtime-unit plan row.
///
/// The row contains no executable path, arbitrary argument/environment bag,
/// process handle, or caller-selected command. A concrete adapter is bound to
/// the sealed retained admission separately and must match this complete row.
struct RuntimeUnitConvergencePlanRowDTO final {
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeProfileId runtime_profile_id;
  manyforge::core::RuntimeProfileRevision runtime_profile_revision;
  manyforge::core::RuntimeLifecycleControl lifecycle_control;
  RuntimeUnitCandidateDisposition candidate_disposition;
  manyforge::core::ProductId product_id;
  manyforge::core::ProductContractId product_contract_id;
  manyforge::core::ProductContractRevision product_contract_revision;
  manyforge::core::ContentDigest product_identity;
  manyforge::core::ContentDigest configuration_identity;
  std::vector<manyforge::core::RuntimeUnitId> dependency_runtime_unit_ids;
  std::vector<manyforge::core::RuntimeRoleId> required_runtime_role_ids;
  manyforge::core::EndpointRoleId status_endpoint_role_id;
  std::string status_endpoint_contract_id;
  std::string status_endpoint_contract_revision;
  std::string status_transport;
  std::string process_adapter_contract_id;
  std::string process_adapter_contract_revision;
  std::string retrieval_contract_id;
  std::string retrieval_contract_revision;
  std::optional<manyforge::core::ContentDigest> launchable_content_digest;
  std::uint64_t launchable_byte_size;
  std::string operating_system;
  std::string shell_contract;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const RuntimeUnitConvergencePlanRowDTO &,
                         const RuntimeUnitConvergencePlanRowDTO &) = default;
};

/// Complete immutable candidate-runtime convergence intent.
///
/// This aggregate is persisted after fence acceptance/rehydration and before
/// the first process effect. Empty runtime-unit sets are explicit and legal.
struct ManagerApplyRuntimeConvergenceIntentDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;
  manyforge::core::ContentDigest fence_acceptance_digest;
  manyforge::core::ActiveBundlePublicationGeneration
      predecessor_publication_generation;
  manyforge::core::ContentDigest predecessor_public_pointer_digest;
  std::vector<RuntimeUnitConvergencePlanRowDTO> runtime_units;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerApplyRuntimeConvergenceIntentDTO &,
             const ManagerApplyRuntimeConvergenceIntentDTO &) = default;
};

struct RuntimeUnitConvergenceApplyOwnerDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero();
  }

  friend bool operator==(const RuntimeUnitConvergenceApplyOwnerDTO &,
                         const RuntimeUnitConvergenceApplyOwnerDTO &) = default;
};

struct RuntimeUnitConvergenceBootstrapOwnerDTO final {
  manyforge::core::BootstrapId bootstrap_id;
  manyforge::core::BootstrapGeneration bootstrap_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero();
  }

  friend bool
  operator==(const RuntimeUnitConvergenceBootstrapOwnerDTO &,
             const RuntimeUnitConvergenceBootstrapOwnerDTO &) = default;
};

using RuntimeUnitConvergenceOwnerDTO =
    std::variant<RuntimeUnitConvergenceApplyOwnerDTO,
                 RuntimeUnitConvergenceBootstrapOwnerDTO>;

/// Exact adapter query bound to one durable Apply intent or admitted bootstrap
/// plan and one plan row.
struct RuntimeUnitConvergenceQueryDTO final {
  RuntimeUnitConvergenceOwnerDTO owner;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;
  manyforge::core::ContentDigest runtime_convergence_intent_digest;
  RuntimeUnitConvergencePlanRowDTO plan;

  RuntimeUnitConvergenceQueryDTO(
      manyforge::core::ApplyId apply_id,
      manyforge::core::ApplyGeneration apply_generation,
      manyforge::core::RetainedBundleId candidate_retained_bundle_id_in,
      manyforge::core::GraphRevision candidate_graph_revision_in,
      manyforge::core::ContentDigest runtime_convergence_intent_digest_in,
      RuntimeUnitConvergencePlanRowDTO plan_in)
      : owner(RuntimeUnitConvergenceApplyOwnerDTO{std::move(apply_id),
                                                  apply_generation}),
        candidate_retained_bundle_id(
            std::move(candidate_retained_bundle_id_in)),
        candidate_graph_revision(std::move(candidate_graph_revision_in)),
        runtime_convergence_intent_digest(
            std::move(runtime_convergence_intent_digest_in)),
        plan(std::move(plan_in)) {}

  RuntimeUnitConvergenceQueryDTO(
      manyforge::core::BootstrapId bootstrap_id,
      manyforge::core::BootstrapGeneration bootstrap_generation,
      manyforge::core::RetainedBundleId candidate_retained_bundle_id_in,
      manyforge::core::GraphRevision candidate_graph_revision_in,
      manyforge::core::ContentDigest runtime_convergence_intent_digest_in,
      RuntimeUnitConvergencePlanRowDTO plan_in)
      : owner(RuntimeUnitConvergenceBootstrapOwnerDTO{std::move(bootstrap_id),
                                                      bootstrap_generation}),
        candidate_retained_bundle_id(
            std::move(candidate_retained_bundle_id_in)),
        candidate_graph_revision(std::move(candidate_graph_revision_in)),
        runtime_convergence_intent_digest(
            std::move(runtime_convergence_intent_digest_in)),
        plan(std::move(plan_in)) {}

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const RuntimeUnitConvergenceQueryDTO &,
                         const RuntimeUnitConvergenceQueryDTO &) = default;
};

/// Process presence/lifecycle adapter used only by the Manager owner.
///
/// Every implementation observes first. `EnsureRunning` and `EnsureStopped`
/// are idempotent for the exact query, may perform an effect only for
/// `manager_managed`, and must reconcile a surviving exact process or an
/// already-absent process rather than duplicate an effect.
class IRuntimeUnitProcessPort {
public:
  virtual ~IRuntimeUnitProcessPort() = default;

  [[nodiscard]] virtual ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) = 0;
  [[nodiscard]] virtual ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) = 0;
  [[nodiscard]] virtual ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) = 0;
};

/// Direct source-attested runtime progress adapter.
///
/// Absence means the exact process has not produced a current report. The
/// adapter does not derive freshness, dependency epochs, or convergence. It
/// must return a newly received direct report for this observation or absence;
/// replaying cached bytes as a fresh receipt is forbidden.
class IRuntimeUnitProgressPort {
public:
  virtual ~IRuntimeUnitProgressPort() = default;

  [[nodiscard]] virtual std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const manyforge::core::RuntimeEpoch &runtime_epoch) = 0;
};

struct RuntimeUnitDependencyEpochDTO final {
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;

  [[nodiscard]] bool valid() const noexcept {
    return !runtime_unit_id.empty() && !runtime_epoch.empty();
  }

  friend bool operator==(const RuntimeUnitDependencyEpochDTO &,
                         const RuntimeUnitDependencyEpochDTO &) = default;
};

/// Complete Manager-derived staged-ready evidence for one runtime unit.
struct RuntimeUnitStagedReadyEvidenceDTO final {
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeLifecycleControl lifecycle_control;
  RuntimeUnitCandidateDisposition candidate_disposition;
  manyforge::core::RuntimeEpoch runtime_epoch;
  std::string process_observer_instance_id;
  std::uint64_t process_observation_sequence;
  std::uint64_t source_report_sequence;
  std::uint64_t critical_progress_sequence;
  manyforge::core::ContentDigest observed_product_identity;
  manyforge::core::ContentDigest observed_configuration_identity;
  RuntimeSourcePhase source_phase;
  RuntimeSourceHealth source_health;
  std::vector<manyforge::core::RuntimeRoleId> ready_runtime_role_ids;
  ManagedCommandSurfaceState command_surface_state;
  std::vector<RuntimeUnitDependencyEpochDTO> dependency_runtime_epochs;
  std::uint64_t presence_age_ns;
  std::uint64_t report_age_ns;
  std::uint64_t progress_age_ns;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const RuntimeUnitStagedReadyEvidenceDTO &,
                         const RuntimeUnitStagedReadyEvidenceDTO &) = default;
};

/// One complete point-in-time candidate staged-ready set.
struct ManagerApplyRuntimeStagedReadyDTO final {
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  std::uint64_t staged_ready_generation;
  manyforge::core::ContentDigest runtime_convergence_intent_digest;
  manyforge::core::ActiveBundlePublicationGeneration
      predecessor_publication_generation;
  manyforge::core::ContentDigest predecessor_public_pointer_digest;
  std::uint64_t evaluated_at_ns;
  std::vector<RuntimeUnitStagedReadyEvidenceDTO> runtime_units;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyRuntimeStagedReadyDTO &,
                         const ManagerApplyRuntimeStagedReadyDTO &) = default;
};

[[nodiscard]] std::string_view ToString(RuntimeUnitCandidateDisposition value);

} // namespace manyforge::component_manager
