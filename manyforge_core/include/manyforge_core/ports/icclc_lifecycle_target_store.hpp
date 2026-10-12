#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/lifecycle_target.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxComponentIncidentEventPage = 64U;

/// One stored incident: its presentation columns (record parts such as the
/// last good observation are not decoded) and the canonical JSON payload of
/// its `component_incident` history event.
struct ComponentIncidentRecordDTO final {
  ComponentLifecycleIncidentDTO incident;
  std::string record_json;
  // Set-once auxiliary diagnostic from existing row JSON; original event unchanged.
  std::optional<ErrorDTO> diagnostic{};
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot{};
  // Routine facts may omit decoding; this is different from known absence.
  bool diagnostic_loaded{true};

  friend bool operator==(const ComponentIncidentRecordDTO&,
                         const ComponentIncidentRecordDTO&) = default;
};

/// Error code of a lifecycle-target write refused because the component's
/// recovery generation advanced after the facts it was based on were read:
/// the operator's Recover (or a cancelled target) intervened. Nothing was
/// written; the driver re-evaluates from fresh facts on its next pass.
inline constexpr std::string_view kLifecycleFactsStaleErrorCode = "component_lifecycle_facts_stale";

/// One consistent read of everything a driver pass decides from for one
/// component. `recovery_generation` advances in every commit that clears or
/// resets these facts outside the driver: the operator's Recover (incident or
/// ordinary) and the cancellation of the component's target; it is 0 before
/// the first. The driver's own writes do not advance it. Every driver write
/// that depends on these facts names the generation it read, and the journal
/// refuses it with kLifecycleFactsStaleErrorCode once it has advanced.
struct ComponentLifecycleTargetFactsDTO final {
  /// The present target; absent when the component has none.
  std::optional<ComponentLifecycleTargetDTO> target;
  std::uint64_t recovery_generation{};
  std::vector<ComponentLifecycleInterventionCauseDTO> intervention_causes;
  std::optional<ComponentIncidentRecordDTO> open_incident;

  friend bool operator==(const ComponentLifecycleTargetFactsDTO&,
                         const ComponentLifecycleTargetFactsDTO&) = default;
};

/// Durable session-scoped requested-target rows owned by the CCLC journal.
///
/// The store mints target generations and keeps intervention-required causes;
/// it owns no lifecycle plan, observation or transaction semantics. Startup
/// targets are written atomically with native/ROS Apply fact adoption by the
/// journal itself, never through this surface.
class ICCLCLifecycleTargetStore {
 public:
  virtual ~ICCLCLifecycleTargetStore() = default;

  /// Replace the target of one currently adopted component with the next
  /// generation. Fails closed when the component has no adopted fact.
  /// Operator Prepare/Enable/Disable targets are written by the journal in the
  /// same commit as the request's acceptance, not through this surface; the
  /// open-request port only wakes the driver after an accepted new request.
  [[nodiscard]] virtual Result<ComponentLifecycleTargetDTO> SetLifecycleTarget(
      const ControllableComponentId& component, LifecycleTargetStateDTO requested_state,
      LifecycleTargetOriginDTO origin) = 0;

  /// Present (non-cancelled) targets in canonical component order.
  [[nodiscard]] virtual Result<std::vector<ComponentLifecycleTargetDTO>>
  LoadLifecycleTargets() = 0;

  [[nodiscard]] virtual Result<std::optional<ComponentLifecycleTargetDTO>> LoadLifecycleTarget(
      const ControllableComponentId& component) = 0;

  /// The component's target, recovery generation, retained intervention
  /// causes and open incident, read in one journal snapshot.
  [[nodiscard]] virtual Result<ComponentLifecycleTargetFactsDTO> LoadLifecycleTargetFacts(
      const ControllableComponentId& component) = 0;

  /// Record the latest intervention-required cause for one domain (`nullopt`
  /// removes the row; only the journal's own Recover and cancellation paths
  /// clear causes in production). Returns whether the durable row changed; an
  /// unchanged value writes nothing. Refused with kLifecycleFactsStaleErrorCode
  /// when `recovery_generation` is no longer the component's.
  [[nodiscard]] virtual Result<bool> RecordInterventionCause(
      const ControllableComponentId& component, const ControlDomainId& domain,
      std::optional<LifecycleBlockingCauseClassDTO> cause_class, std::int64_t observed_at_ns,
      std::uint64_t recovery_generation) = 0;

  /// Record whether the component reached its present target on fresh status.
  /// Returns whether the durable row changed. Refused with
  /// kLifecycleFactsStaleErrorCode when `recovery_generation` is no longer
  /// the component's.
  [[nodiscard]] virtual Result<bool> RecordLifecycleTargetReached(
      const ControllableComponentId& component, bool reached,
      std::uint64_t recovery_generation) = 0;

  /// True while a recorded, not yet adopted Apply plan replaces (changes) or
  /// removes the component: only that component's loss is then explained by
  /// the Apply. A component the plan retains unchanged is false: its provider
  /// survives, so its loss is unexpected. Adoption of the plan ends it.
  [[nodiscard]] virtual Result<bool> ComponentInUnadoptedApplyPlan(
      const ControllableComponentId& component) = 0;

  /// Open one incident with containment `pending`. A component that already
  /// has an open incident keeps it and that one is returned unchanged.
  /// Refused with kLifecycleFactsStaleErrorCode when `recovery_generation` is
  /// no longer the component's (a Recover closed what the detection saw).
  [[nodiscard]] virtual Result<ComponentIncidentRecordDTO> OpenComponentIncident(
      const ComponentIncidentRecordDTO& record, std::uint64_t recovery_generation) = 0;
  [[nodiscard]] virtual Result<std::optional<ComponentIncidentRecordDTO>>
  LoadOpenComponentIncident(const ControllableComponentId& component) = 0;
  /// Record the containment outcome of a pending open incident and append its
  /// `component_incident` history event in the same transaction. Returns
  /// false (writing nothing) when the incident is no longer pending.
  [[nodiscard]] virtual Result<bool> RecordComponentIncidentContainment(
      const std::string& incident_id, ComponentIncidentContainmentStateDTO state,
      const std::string& record_json, std::int64_t occurred_at_ns) = 0;
  /// The operator's Recover of the component's open incident through the ordinary
  /// request identity/replay checks; nullopt when ordinary CCLC admission decides.
  [[nodiscard]] virtual Result<std::optional<CCLCLifecycleRequestResultDTO>> RecoverComponentIncident(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const std::string& acknowledged_by, std::int64_t acknowledged_at_ns) = 0;
  /// A further unrequested change while the incident is open: the most
  /// recent intervention-requiring cause becomes the incident's cause, its
  /// vendor codes and timeline (inside `record_json`) are extended. Returns
  /// false (writing nothing) when that incident is not open.
  [[nodiscard]] virtual Result<bool> UpdateOpenComponentIncident(
      const std::string& incident_id, LifecycleBlockingCauseClassDTO cause_class,
      const std::vector<std::string>& vendor_codes, const std::string& record_json) = 0;
  /// Set once in the existing row JSON and append one exit-evidence event.
  /// Does not change the original event, summary or cause. False for replay/closed.
  [[nodiscard]] virtual Result<bool> AppendComponentIncidentDiagnostic(
      const std::string&, std::int64_t, const ErrorDTO&,
      std::shared_ptr<const content_identity::CanonicalValue> = {}) {
    return Result<bool>::Err({"component_incident_diagnostic_unavailable",
                              "incident diagnostic append is not composed",
                              {}});
  }
  /// Durable incident history events after `after_sequence`, oldest first,
  /// at most `limit` (bounded by kMaxComponentIncidentEventPage).
  [[nodiscard]] virtual Result<std::vector<ComponentIncidentEventDTO>>
  LoadComponentIncidentEvents(std::uint64_t after_sequence, std::size_t limit) = 0;
};

}  // namespace manyforge::core
