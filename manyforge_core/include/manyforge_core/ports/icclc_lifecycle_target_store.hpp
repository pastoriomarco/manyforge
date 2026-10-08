#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/lifecycle_target.hpp>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxComponentIncidentEventPage = 64U;

/// One stored incident: its presentation columns (record parts such as the
/// last good observation are not decoded) and the canonical JSON payload of
/// its `component_incident` history event.
struct ComponentIncidentRecordDTO final {
  ComponentLifecycleIncidentDTO incident;
  std::string record_json;

  friend bool operator==(const ComponentIncidentRecordDTO&,
                         const ComponentIncidentRecordDTO&) = default;
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
  [[nodiscard]] virtual Result<ComponentLifecycleTargetDTO> SetLifecycleTarget(
      const ControllableComponentId& component, LifecycleTargetStateDTO requested_state,
      LifecycleTargetOriginDTO origin) = 0;

  /// Present (non-cancelled) targets in canonical component order.
  [[nodiscard]] virtual Result<std::vector<ComponentLifecycleTargetDTO>>
  LoadLifecycleTargets() = 0;

  [[nodiscard]] virtual Result<std::optional<ComponentLifecycleTargetDTO>> LoadLifecycleTarget(
      const ControllableComponentId& component) = 0;

  /// Record the latest intervention-required cause for one domain, or clear
  /// it with `nullopt` after fresh status no longer reports one. Returns
  /// whether the durable row changed; an unchanged value writes nothing.
  [[nodiscard]] virtual Result<bool> RecordInterventionCause(
      const ControllableComponentId& component, const ControlDomainId& domain,
      std::optional<LifecycleBlockingCauseClassDTO> cause_class, std::int64_t observed_at_ns) = 0;

  [[nodiscard]] virtual Result<std::vector<ComponentLifecycleInterventionCauseDTO>>
  LoadInterventionCauses(const ControllableComponentId& component) = 0;

  /// Record whether the component reached its present target on fresh status.
  /// Returns whether the durable row changed.
  [[nodiscard]] virtual Result<bool> RecordLifecycleTargetReached(
      const ControllableComponentId& component, bool reached) = 0;

  /// True while a recorded Apply plan that names the component (changed,
  /// retained or removed) is not yet adopted: its change is requested, so no
  /// incident may open for it in that window.
  [[nodiscard]] virtual Result<bool> ComponentInUnadoptedApplyPlan(
      const ControllableComponentId& component) = 0;

  /// Open one incident with containment `pending`. A component that already
  /// has an open incident keeps it and that one is returned unchanged.
  [[nodiscard]] virtual Result<ComponentIncidentRecordDTO> OpenComponentIncident(
      const ComponentIncidentRecordDTO& record) = 0;
  [[nodiscard]] virtual Result<std::optional<ComponentIncidentRecordDTO>>
  LoadOpenComponentIncident(const ControllableComponentId& component) = 0;
  /// Record the containment outcome of a pending open incident and append its
  /// `component_incident` history event in the same transaction. Returns
  /// false (writing nothing) when the incident is no longer pending.
  [[nodiscard]] virtual Result<bool> RecordComponentIncidentContainment(
      const std::string& incident_id, ComponentIncidentContainmentStateDTO state,
      const std::string& record_json, std::int64_t occurred_at_ns) = 0;
  /// Close the open incident with the operator's acknowledgement, clear the
  /// component's retained intervention causes and reached marker, and append
  /// the `component_incident_acknowledged` event, atomically. Returns false
  /// (writing nothing) when that incident is not open.
  [[nodiscard]] virtual Result<bool> AcknowledgeComponentIncident(
      const ComponentLifecycleIncidentDTO& acknowledged, const std::string& payload_json) = 0;
  /// The Recover that closed one of the component's retained incidents with
  /// exactly this request id, for exact replay of the operator's request.
  [[nodiscard]] virtual Result<std::optional<ComponentIncidentRecoveryDTO>>
  LoadComponentIncidentRecovery(const ControllableComponentId& component,
                                const RequestId& request_id) = 0;
  /// A further unrequested change while the incident is open: the most
  /// recent intervention-requiring cause becomes the incident's cause, its
  /// vendor codes and timeline (inside `record_json`) are extended. Returns
  /// false (writing nothing) when that incident is not open.
  [[nodiscard]] virtual Result<bool> UpdateOpenComponentIncident(
      const std::string& incident_id, LifecycleBlockingCauseClassDTO cause_class,
      const std::vector<std::string>& vendor_codes, const std::string& record_json) = 0;
  /// Durable incident history events after `after_sequence`, oldest first,
  /// at most `limit` (bounded by kMaxComponentIncidentEventPage).
  [[nodiscard]] virtual Result<std::vector<ComponentIncidentEventDTO>>
  LoadComponentIncidentEvents(std::uint64_t after_sequence, std::size_t limit) = 0;
};

}  // namespace manyforge::core
