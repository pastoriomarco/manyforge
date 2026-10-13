#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <vector>

namespace manyforge::core {

/// Private CCLC offer for one explicitly requested failed-component bring-up.
/// The coordinator reads the predecessor/incident; the Manager qualifies the
/// candidate product. This is not public lifecycle or observation-only route
/// recovery authority. Kernel admission verifies its exact current facts.
struct SelectedComponentBringUpOfferDTO final {
  PublicRequestContextDTO request_context;
  LifecycleTransactionId lifecycle_transaction_id;
  LifecycleTransactionGeneration lifecycle_transaction_generation;
  ComponentLifecycleCurrentSnapshotDTO predecessor;
  std::string incident_id;
  ComponentIncidentContainmentReceiptDTO containment;
  ProjectionRevisionDTO candidate_projection_revision;
  ContentDigest candidate_retained_bundle_digest;
  std::uint64_t candidate_retained_bundle_bytes{};
  std::vector<RuntimeParticipantBindingDTO> candidate_participants;
  ConfigurationFenceAdmissionSnapshotDTO fence_admission;
  ContentDigest original_request_payload_digest;

  friend bool operator==(const SelectedComponentBringUpOfferDTO&,
                         const SelectedComponentBringUpOfferDTO&) = default;
};

/// Private Q91 input from the original live Manager continuation. Manager
/// retains and revalidates the sealed full plan/head/acknowledged needed set;
/// these current selected facts do not recreate that continuation or authority.
enum class ApplyBringUpAdmissionComponentKindDTO { kInitial, kContainedRecovery };

struct ApplyBringUpAdmissionRecoveryDTO final {
  std::string incident_id;
  ComponentIncidentContainmentReceiptDTO containment;
};

struct ApplyBringUpAdmissionComponentDTO final {
  ControllableComponentId controllable_component_id;
  ApplyBringUpAdmissionComponentKindDTO kind;
  std::optional<ComponentLifecycleCurrentSnapshotDTO> predecessor;
  std::vector<RuntimeParticipantBindingDTO> candidate_participants;
  std::optional<ApplyBringUpAdmissionRecoveryDTO> recovery;
};

struct ApplyBringUpAdmissionOfferDTO final {
  ApplyComponentBringUpReferenceDTO parent;
  ConfigurationFenceAdmissionSnapshotDTO admission;
  std::vector<ControllableComponentId> configured_components;
  std::vector<ApplyBringUpAdmissionComponentDTO> needed_components;
};

/// Derive the exact predecessor/candidate selected physical closure. Removed
/// domains retain their predecessor scope; new domains have no invented
/// predecessor active configuration. This pure function grants no authority.
[[nodiscard]] Result<ConfigurationFenceScopeDTO> SelectedComponentBringUpFenceScope(
    const SelectedComponentBringUpOfferDTO& offer);

[[nodiscard]] Result<CellAdmissionScopeDTO> SelectedComponentBringUpAdmissionScope(
    const SelectedComponentBringUpOfferDTO& offer);

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
  /// Ordinary work uses the recovered watermark or its permitted snapshot
  /// seed. Q97 uses the exact owner selection below; zero represents its
  /// explicit absence. Caller expectations never populate this field.
  ComponentLifecycleGeneration durable_component_generation_predecessor;
  std::int64_t accepted_at_ns;
  std::optional<ComponentLifecycleCurrentSnapshotDTO> current_snapshot;
  std::vector<BrandLifecyclePlanCatalogDTO> lifecycle_plan_catalogs;
  /// Q97 owner-read generation selection. Present only for private Apply
  /// admission; ordinary admission never consumes this value.
  std::optional<ApplyComponentGenerationSelectionDTO> apply_generation_selection{};
};

/// Fully closed values ready for atomic reservation plus journal admission.
struct PreparedComponentLifecycleAdmission final {
  ComponentLifecycleAcceptedDTO acceptance;
  ComponentLifecycleStatusDTO initial_status;
  std::vector<ControlDomainId> reserved_control_domains;
  /// Present when the complete exact provider-catalog closure was qualified
  /// for this accepted ordinary lifecycle intent.
  std::optional<std::vector<BrandLifecyclePlanCatalogDTO>> qualified_lifecycle_plan_catalogs;
};

/// Pure closed admission construction after replay classification and source
/// reads, but before reservation or durable journal CAS.
[[nodiscard]] Result<PreparedComponentLifecycleAdmission> PrepareComponentLifecycleAdmission(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAdmissionCandidate& candidate);

/// Private pure Q91 construction. Candidate qualification is retained for the
/// later installed successor; only genuine predecessor observations populate
/// Accepted status. This value does not verify live held authority or effects.
[[nodiscard]] Result<PreparedComponentLifecycleAdmission> PrepareApplyComponentBringUpAdmission(
    const ComponentLifecycleIntentDTO& intent,
    const ComponentLifecycleAdmissionCandidate& candidate,
    const ApplyComponentBringUpEvidenceDTO& evidence,
    const std::vector<ResolvedLifecycleControlDomainScopeDTO>& qualified_candidate_domains,
    const std::vector<ResolvedLifecycleProviderOccurrenceDTO>& candidate_provider_occurrences);

}  // namespace manyforge::core
