#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

/// Exact already-read facts used by the CCLC admission commit path.
///
/// This is an in-process construction value, not a transport DTO or an
/// authority token. The coordinator obtains every field from its injected
/// sources before calling the pure preparation function below.
struct ComponentLifecycleAdmissionCandidate final {
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  /// Exact current durable per-component predecessor selected by CCLC.
  ///
  /// It comes from the recovered journal watermark, or from the one permitted
  /// first-admission seed. Caller expectations never populate this field.
  ComponentLifecycleGeneration durable_component_generation_predecessor;
  std::int64_t accepted_at_ns;
  std::int64_t containment_only_disable_timeout_duration_ns;
  std::optional<ComponentLifecycleCurrentSnapshotDTO> current_snapshot;
  std::vector<BrandLifecyclePlanCatalogDTO> lifecycle_plan_catalogs;
  std::optional<DisableHandoffAdmissionSnapshotDTO> disable_handoff_admission;
};

/// Fully closed values ready for atomic reservation plus journal admission.
struct PreparedComponentLifecycleAdmission final {
  ComponentLifecycleAcceptedDTO acceptance;
  ComponentLifecycleStatusDTO initial_status;
  std::vector<ControlDomainId> reserved_control_domains;
  /// Present only when the complete exact provider-catalog closure was
  /// qualified for this accepted intent.  Containment-only Disable admission
  /// deliberately carries no executable vendor plan.
  std::optional<std::vector<BrandLifecyclePlanCatalogDTO>> qualified_lifecycle_plan_catalogs;
  /// True only when the accepted request's original component scope and
  /// caller expectations permit vendor lifecycle mutation after a ready
  /// Kernel handoff.  The exact provider/catalog closure is independently
  /// requalified under that fence; it may have been unavailable at admission.
  bool lifecycle_mutation_eligible;
  std::optional<DisableHandoffAdmissionSnapshotDTO> disable_handoff_admission;
};

/// Derive the exact component-shaped read-only Kernel admission expectation.
[[nodiscard]] Result<DisableHandoffComponentScopeExpectationDTO>
BuildDisableHandoffComponentScopeExpectation(const ComponentLifecycleCurrentSnapshotDTO& snapshot);

/// Pure closed admission construction after replay classification and source
/// reads, but before reservation or durable journal CAS.
[[nodiscard]] Result<PreparedComponentLifecycleAdmission> PrepareComponentLifecycleAdmission(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAdmissionCandidate& candidate);

}  // namespace manyforge::core
