#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

/// Component incidents (maintainer decisions 2026-10-08, addendum 2).
///
/// After a component first reached its requested target in the session, any
/// unexpected loss or change is an incident: CCLC opens it once, requests a
/// system-attributed cell containment Stop that latches the cell, blocks
/// lifecycle progress and keeps it open until the operator's acknowledgement
/// (an accepted operator `recover` request) is recorded.

inline constexpr std::size_t kMaxComponentIncidentRecordBytes = 512U * 1024U;
inline constexpr std::size_t kMaxComponentIncidentIdBytes = 128U;
inline constexpr std::size_t kMaxComponentIncidentVendorCodes = 16U;
inline constexpr std::size_t kMaxComponentIncidentVendorCodeBytes = 64U;
inline constexpr std::size_t kMaxComponentIncidentSummaryBytes = 1024U;
inline constexpr std::size_t kMaxComponentIncidentTimelineEntries = 32U;

/// Q96 storage target only. It grants no component lifecycle, recovery
/// generation or Kernel containment authority. Values are the checked cell
/// observer identities; source bytes (including NUL) are preserved.
struct CellObserverIncidentTargetDTO final {
  CellId cell_id;
  std::string source_id;
  std::string source_epoch_base;

  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CellObserverIncidentTargetDTO&,
                         const CellObserverIncidentTargetDTO&) = default;
};

/// Initial observer-loss evidence in the same bounded incident table. This
/// codec intentionally supplies no acknowledgement/closing operation.
struct CellObserverIncidentRecordDTO final {
  std::string incident_id;
  CellObserverIncidentTargetDTO target;
  std::int64_t detected_at_ns{};
  std::string summary;
  content_identity::CanonicalValue diagnostic_snapshot;

  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CellObserverIncidentRecordDTO&,
                         const CellObserverIncidentRecordDTO&) = default;
};

/// Closed initial record, capped by kMaxComponentIncidentRecordBytes. The
/// host owns capture of context/settings/generated maps; this ROS-free value
/// codec preserves them and validates actual exit and bounded output shape.
[[nodiscard]] std::string EncodeCellObserverIncidentRecord(
    const CellObserverIncidentRecordDTO& record);
[[nodiscard]] CellObserverIncidentRecordDTO DecodeCellObserverIncidentRecord(
    std::string_view record_json);
/// Existing incident_events envelope payload: byte-identical to the stored
/// record, including event_kind under the same single 512 KiB bound.
[[nodiscard]] std::string CellObserverIncidentEventPayload(
    const CellObserverIncidentRecordDTO& record);

/// The existing selected-runtime command packet is capped at 256 KiB. The
/// checked launch path cannot exceed that raw bound; the complete durable
/// record still has the existing 512 KiB incident bound after JSON escaping.
inline constexpr std::size_t kMaxObserverBringUpSnapshotPathBytes = 256U * 1024U;

/// Actual association from the existing open observer incident row. The
/// original incident is immutable while each genuine later exit advances
/// its existing latest-exit identity and digest.
struct CellObserverLossAssociationDTO final {
  std::string incident_id;
  std::string latest_exit_id;
  std::string latest_exit_digest;
  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CellObserverLossAssociationDTO&,
                         const CellObserverLossAssociationDTO&) = default;
};

/// One explicit observer bring-up, retained in this cell's existing journal.
/// These values are evidence from the actual owners, never process, Scene or
/// lifecycle authority. Only the latest attempt is retained; reads do no work.
struct CellObserverBringUpIntentDTO final {
  PublicRequestContextDTO request_context;
  RequestAttributionDTO attribution;
  ProjectionRevisionDTO projection_revision;
  ActiveBundlePublicationGeneration publication_generation;
  ContentDigest publication_digest;
  RuntimeUnitId runtime_unit_id;
  RuntimeEpoch host_epoch;
  std::string manager_epoch;
  std::string scene_owner_epoch;
  /// Canonical closed Scene identity supplied and checked by Manager.
  std::string scene_identity_json;
  CellObserverIncidentTargetDTO predecessor;
  CellObserverIncidentTargetDTO replacement;
  std::string retained_snapshot_path;
  std::optional<std::string> predecessor_incident_id;

  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CellObserverBringUpIntentDTO&,
                         const CellObserverBringUpIntentDTO&) = default;
};

struct CellObserverBringUpProcessDTO final {
  std::int64_t leader_pid{};
  std::string launch_identity;
  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CellObserverBringUpProcessDTO&,
                         const CellObserverBringUpProcessDTO&) = default;
};

enum class CellObserverBringUpPhaseDTO : std::uint8_t {
  kAdmitted, kRetiring, kStarted, kOpenAccepted, kFailed, kUnknown,
};

/// The first accepted Open is captured by Manager under its existing checks.
/// A reconnect cannot mint this evidence or stand in for that first Open.
struct CellObserverBringUpOpenDTO final {
  std::string source_epoch;
  std::uint64_t source_epoch_order{};
  std::int64_t accepted_at_ns{};
  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CellObserverBringUpOpenDTO&,
                         const CellObserverBringUpOpenDTO&) = default;
};

struct CellObserverBringUpRecordDTO final {
  CellObserverBringUpIntentDTO intent;
  CellObserverBringUpPhaseDTO phase{CellObserverBringUpPhaseDTO::kAdmitted};
  std::int64_t admitted_at_ns{};
  std::int64_t updated_at_ns{};
  std::optional<CellObserverBringUpProcessDTO> predecessor_process;
  /// Historical positive drain/absence from the actual owner, not an inference
  /// from epoch change, timeout, missing callback or missing process handle.
  bool predecessor_drained_or_absent{};
  std::optional<CellObserverBringUpProcessDTO> started_process;
  std::optional<CellObserverBringUpOpenDTO> first_open;
  std::optional<ErrorDTO> diagnostic;
  /// Captured admission evidence, preserved on replay and outcome updates.
  /// Null names a genuinely checked absent slot, never a new owner epoch.
  std::optional<CellObserverLossAssociationDTO> predecessor_loss{};
  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const CellObserverBringUpRecordDTO&,
                         const CellObserverBringUpRecordDTO&) = default;
};

struct CellObserverBringUpAdmissionDTO final {
  CellObserverBringUpRecordDTO record;
  /// Only accepted-new permits the caller's one existing scheduler callback.
  bool accepted_new{};
};

/// Closed canonical record, capped at kMaxComponentIncidentRecordBytes.
[[nodiscard]] std::string EncodeCellObserverBringUpRecord(const CellObserverBringUpRecordDTO&);
[[nodiscard]] CellObserverBringUpRecordDTO DecodeCellObserverBringUpRecord(std::string_view);
/// Every incident time is in the CCLC owner clock domain (the one of
/// `accepted_at_ns`): a full-width nonnegative int64 (spec 210 §1.1). JSON
/// carries it through `content_identity::nonnegative_int64_value` (a number
/// up to 2^53-1, a canonical decimal string above); a negative time is
/// invalid, never clamped.

/// Durable history event kinds of the incident record (contract item 5).
inline constexpr std::string_view kComponentIncidentEventKind = "component_incident";
inline constexpr std::string_view kComponentIncidentAcknowledgedEventKind =
    "component_incident_acknowledged";

/// What CCLC asks the Kernel to contain for one incident. The Kernel mints
/// the request identity from its own owner epoch and the incident id, so a
/// repeated request for the same incident is an exact replay.
struct ComponentIncidentContainmentIntentDTO final {
  std::string incident_id;
  ControllableComponentId controllable_component_id;
  LifecycleBlockingCauseClassDTO cause_class{LifecycleBlockingCauseClassDTO::kUnknown};

  [[nodiscard]] bool locally_valid() const noexcept {
    return !incident_id.empty() && incident_id.size() <= kMaxComponentIncidentIdBytes &&
           !controllable_component_id.empty() &&
           controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
           lifecycle_cause_requires_intervention(cause_class);
  }

  friend bool operator==(const ComponentIncidentContainmentIntentDTO&,
                         const ComponentIncidentContainmentIntentDTO&) = default;
};

/// The accepted containment Stop. Acceptance only: the physical outcome is
/// the Stop's own status/certificate, read through the Stop reference.
struct ComponentIncidentContainmentReceiptDTO final {
  StopId stop_id;
  StopGeneration stop_generation;
  /// Operations the Stop captured when it was accepted.
  std::vector<OperationId> affected_operation_ids;
  /// The Program run holding the cell cycle claim at acceptance, if any.
  std::optional<ProgramRunId> program_run_id;

  friend bool operator==(const ComponentIncidentContainmentReceiptDTO&,
                         const ComponentIncidentContainmentReceiptDTO&) = default;
};

enum class ComponentIncidentContainmentStateDTO : std::uint8_t {
  /// Recorded with the incident; the Kernel request has not completed yet.
  kPending = 0,
  kRequested,
  /// The Kernel refused or was unreachable; the record keeps the reason.
  kFailed,
};

[[nodiscard]] constexpr std::string_view to_string(
    const ComponentIncidentContainmentStateDTO value) noexcept {
  switch (value) {
    case ComponentIncidentContainmentStateDTO::kPending:
      return "pending";
    case ComponentIncidentContainmentStateDTO::kRequested:
      return "requested";
    case ComponentIncidentContainmentStateDTO::kFailed:
      return "failed";
  }
  return "pending";
}

/// Last fresh observation before the incident, for debugging.
struct ComponentIncidentObservationDTO final {
  ControlDomainId control_domain_id;
  std::uint64_t observation_generation{};
  std::int64_t observed_at_ns{};
  ComponentLifecycleStateDTO observed_state{ComponentLifecycleStateDTO::kUndeclared};
  std::vector<LifecycleFactDTO> facts;

  friend bool operator==(const ComponentIncidentObservationDTO&,
                         const ComponentIncidentObservationDTO&) = default;
};

/// Provider occurrence named by the component when the incident opened.
/// Process exit information is not known to CCLC.
struct ComponentIncidentProviderDTO final {
  ControlDomainId control_domain_id;
  ProviderInstanceId provider_instance_id;
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;

  friend bool operator==(const ComponentIncidentProviderDTO&,
                         const ComponentIncidentProviderDTO&) = default;
};

/// One incident: open from detection until the operator's acknowledgement.
struct ComponentLifecycleIncidentDTO final {
  std::string incident_id;
  ControllableComponentId controllable_component_id;
  LifecycleBlockingCauseClassDTO cause_class{LifecycleBlockingCauseClassDTO::kUnknown};
  std::vector<std::string> vendor_codes;
  std::int64_t detected_at_ns{};
  std::string summary;
  std::optional<ComponentIncidentObservationDTO> last_good_observation;
  std::vector<ComponentIncidentProviderDTO> providers;
  ComponentIncidentContainmentStateDTO containment_state{
      ComponentIncidentContainmentStateDTO::kPending};
  std::optional<ComponentIncidentContainmentReceiptDTO> containment;
  std::optional<std::string> containment_error;
  bool acknowledged{};
  std::optional<std::string> acknowledged_by;
  std::optional<std::int64_t> acknowledged_at_ns;
  std::optional<RequestId> acknowledgement_request_id;
  std::optional<ErrorDTO> diagnostic{};
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ComponentLifecycleIncidentDTO&,
                         const ComponentLifecycleIncidentDTO&) = default;
};

/// The operator's Recover of one open incident (contract item 3). It is an
/// authorization-only effect: CCLC records it, closes the incident and clears
/// the retained cause, but dispatches no vendor primitive; the policy-permitted
/// steps run later through the target-following driver once ordinary
/// admission is open again (for example after the cell latch is recovered).
enum class ComponentIncidentClosureReasonDTO : std::uint8_t {
  kCellClosedByOperator = 0,
};

struct ComponentIncidentRecoveryDTO final {
  std::string incident_id;
  ControllableComponentId controllable_component_id;
  LifecycleBlockingCauseClassDTO cause_class{LifecycleBlockingCauseClassDTO::kUnknown};
  std::string acknowledged_by;
  std::int64_t acknowledged_at_ns{};
  std::optional<ComponentIncidentClosureReasonDTO> closure_reason{};

  [[nodiscard]] bool locally_valid() const noexcept {
    return !incident_id.empty() && incident_id.size() <= kMaxComponentIncidentIdBytes &&
           !controllable_component_id.empty() &&
           lifecycle_cause_requires_intervention(cause_class) && !acknowledged_by.empty() &&
           acknowledged_at_ns >= 0 &&
           (!closure_reason || *closure_reason == ComponentIncidentClosureReasonDTO::kCellClosedByOperator);
  }

  friend bool operator==(const ComponentIncidentRecoveryDTO&,
                         const ComponentIncidentRecoveryDTO&) = default;
};

/// One durable history event of the incident record, in journal order.
struct ComponentIncidentEventDTO final {
  std::uint64_t sequence{};
  std::string event_kind;
  std::string incident_id;
  std::int64_t occurred_at_ns{};
  /// Canonical JSON of the contract item 5 fields.
  std::string payload_json;

  friend bool operator==(const ComponentIncidentEventDTO&,
                         const ComponentIncidentEventDTO&) = default;
};

/// Per-cause recovery policy (spec 240 profile data, addendum 2 item 6).
/// CCLC applies it; adapters only classify. The defaults all require the
/// operator's acknowledgement; a profile override is a later hook.
struct LifecycleIncidentRecoveryPolicyDTO final {
  LifecycleBlockingCauseClassDTO cause_class{LifecycleBlockingCauseClassDTO::kUnknown};
  bool acknowledgement_required{true};
  /// A physical action (release, power cycle) must precede the recovery.
  bool physical_action_required{};
  /// A Manager-managed provider process may be restarted only after the
  /// acknowledgement. CCLC never restarts a process itself.
  bool provider_restart_after_acknowledgement{};
  /// Recovery steps after the acknowledgement, for the operator and record.
  std::string_view recovery_steps;

  friend bool operator==(const LifecycleIncidentRecoveryPolicyDTO&,
                         const LifecycleIncidentRecoveryPolicyDTO&) = default;
};

/// Addendum 2 defaults, one row per intervention-requiring cause class.
[[nodiscard]] std::vector<LifecycleIncidentRecoveryPolicyDTO>
DefaultLifecycleIncidentRecoveryPolicy();

/// Row for `cause` in `policy`, or the fail-closed default (acknowledgement
/// required) when the table has none.
[[nodiscard]] LifecycleIncidentRecoveryPolicyDTO LifecycleIncidentRecoveryPolicyFor(
    const std::vector<LifecycleIncidentRecoveryPolicyDTO>& policy,
    LifecycleBlockingCauseClassDTO cause);

/// Vendor codes rendered from the adapter's `manyforge.vendor_code.*` facts:
/// a token is the code itself (`C2`), an integer or boolean is
/// `<last id segment>=<value>` (`mode=0`). Bounded, in fact order.
[[nodiscard]] std::vector<std::string> ComponentIncidentVendorCodes(
    const std::vector<LifecycleFactDTO>& facts);

/// Canonical JSON of one history event of the incident record (contract item
/// 5): identity, cause, vendor codes, detection time, timeline, last good
/// observation, captured operations, Program run, containment Stop reference,
/// providers, recovery policy and, for `component_incident_acknowledged`, who
/// and when.
[[nodiscard]] std::string ComponentIncidentEventPayload(
    const ComponentLifecycleIncidentDTO& incident, const LifecycleIncidentRecoveryPolicyDTO& policy,
    std::string_view event_kind, const std::optional<ErrorDTO>& diagnostic = std::nullopt,
    std::shared_ptr<const content_identity::CanonicalValue> snapshot = {});

/// The record with its containment outcome: state, Stop reference, captured
/// operations and Program run, or the refusal reason.
[[nodiscard]] std::string ComponentIncidentRecordWithContainment(
    std::string_view record_json, ComponentIncidentContainmentStateDTO state,
    const std::optional<ComponentIncidentContainmentReceiptDTO>& receipt,
    const std::optional<std::string>& error);

/// The record after a further unrequested change while it is open: `cause`
/// becomes the incident's cause, `vendor_codes` replace the record's codes and
/// one bounded timeline entry is appended.
[[nodiscard]] std::string ComponentIncidentRecordWithChange(
    std::string_view record_json, std::int64_t at_ns, LifecycleBlockingCauseClassDTO cause,
    const std::vector<std::string>& vendor_codes,
    const std::optional<ErrorDTO>& diagnostic = std::nullopt,
    std::shared_ptr<const content_identity::CanonicalValue> snapshot = {});

[[nodiscard]] bool ComponentIncidentDiagnosticIsValid(const ErrorDTO& diagnostic) noexcept;
/// Ingress validates each closed field before the journal omits optional
/// settings to fit the complete record budget. Persisted/status values use
/// the strict aggregate validator below.
[[nodiscard]] bool ComponentIncidentDiagnosticSnapshotShapeIsValid(
    const content_identity::CanonicalValue& snapshot) noexcept;
[[nodiscard]] bool ComponentIncidentDiagnosticSnapshotIsValid(
    const content_identity::CanonicalValue& snapshot) noexcept;
[[nodiscard]] content_identity::CanonicalValue ComponentIncidentDiagnosticValue(
    const ErrorDTO& diagnostic, const content_identity::CanonicalValue& snapshot);

/// First short error can be stored with a null snapshot; its one later rich
/// completion preserves every original error fact and adds only capture metadata.
[[nodiscard]] std::string ComponentIncidentRecordWithDiagnostic(
    std::string_view record_json, const ErrorDTO& diagnostic,
    std::shared_ptr<const content_identity::CanonicalValue> snapshot = {});
[[nodiscard]] std::optional<ErrorDTO> ComponentIncidentDiagnosticFromRecord(
    std::string_view record_json,
    std::shared_ptr<const content_identity::CanonicalValue>* snapshot = nullptr) noexcept;

/// Decode the real accepted Stop reference kept in the existing record.
/// Explicit status reads hydrate this; routine facts do not parse evidence.
[[nodiscard]] std::optional<ComponentIncidentContainmentReceiptDTO>
ComponentIncidentContainmentReceiptFromRecord(
    std::string_view record_json, std::optional<std::string>* containment_error = nullptr) noexcept;

/// The `component_incident_acknowledged` payload: the record plus who
/// recovered it, when and with which request.
[[nodiscard]] std::string ComponentIncidentAcknowledgedPayload(
    std::string_view record_json, const ComponentLifecycleIncidentDTO& acknowledged,
    std::optional<ComponentIncidentClosureReasonDTO> closure_reason = std::nullopt);

}  // namespace manyforge::core
