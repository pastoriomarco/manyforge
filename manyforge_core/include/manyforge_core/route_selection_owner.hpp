#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/route_selection.hpp>
#include <manyforge_core/types/controller_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <optional>
#include <utility>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxRouteReadinessPublicationRows = 4096U;

/// One coherent, read-only route-admission closure produced by the current
/// configuration owner. Public request fields are compared with this value;
/// they never populate it.
struct RouteSelectionCurrentSnapshotDTO final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::vector<RouteSelectionComponentExpectationDTO> component_expectations;
  std::optional<ActiveControlConfigurationDTO> active_control_configuration;
  ControllerLifecycleExpectedDomainDTO expected_controller_domain;
  ConfigurationFenceScopeDTO configuration_fence_scope;

  friend bool operator==(const RouteSelectionCurrentSnapshotDTO&,
                         const RouteSelectionCurrentSnapshotDTO&) = default;
};

/// Sealed current route closure. The source/repository composition verifies
/// the raw DTO once; CCLC retains the seal across admission and rereads an
/// exact second seal after acquiring the shared domain reservation.
class RouteSelectionCurrentSnapshot final {
 public:
  RouteSelectionCurrentSnapshot(const RouteSelectionCurrentSnapshot&) = default;
  RouteSelectionCurrentSnapshot& operator=(const RouteSelectionCurrentSnapshot&) = default;
  RouteSelectionCurrentSnapshot(RouteSelectionCurrentSnapshot&&) noexcept = default;
  RouteSelectionCurrentSnapshot& operator=(RouteSelectionCurrentSnapshot&&) noexcept = default;

  [[nodiscard]] const RouteSelectionCurrentSnapshotDTO& value() const noexcept { return value_; }
  [[nodiscard]] const ControllerLifecycleExpectedDomain& expected_controller_domain()
      const noexcept {
    return expected_controller_domain_;
  }

 private:
  RouteSelectionCurrentSnapshot(RouteSelectionCurrentSnapshotDTO value,
                                ControllerLifecycleExpectedDomain expected_controller_domain)
      : value_(std::move(value)),
        expected_controller_domain_(std::move(expected_controller_domain)) {}

  RouteSelectionCurrentSnapshotDTO value_;
  ControllerLifecycleExpectedDomain expected_controller_domain_;

  friend Result<RouteSelectionCurrentSnapshot> verify_route_selection_current_snapshot(
      RouteSelectionCurrentSnapshotDTO);
};

[[nodiscard]] Result<RouteSelectionCurrentSnapshot> verify_route_selection_current_snapshot(
    RouteSelectionCurrentSnapshotDTO value);

/// Observation-only current closure of one retained domain: the exact
/// retained identity, the agreed current configuration (CCLC durable and
/// Kernel current) and the expected controller domain. It carries no
/// component lifecycle expectation or fence scope, so it observes a Declared
/// component or one without lifecycle facts. It is not a route-selection
/// snapshot and can never admit a route change, claim or fence.
struct RouteConfigurationObservationSnapshotDTO final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::optional<ActiveControlConfigurationDTO> active_control_configuration;
  ControllerLifecycleExpectedDomainDTO expected_controller_domain;

  friend bool operator==(const RouteConfigurationObservationSnapshotDTO&,
                         const RouteConfigurationObservationSnapshotDTO&) = default;
};

/// Sealed observation-only closure; constructed only by its verifier.
class RouteConfigurationObservationSnapshot final {
 public:
  RouteConfigurationObservationSnapshot(const RouteConfigurationObservationSnapshot&) = default;
  RouteConfigurationObservationSnapshot& operator=(const RouteConfigurationObservationSnapshot&) =
      default;
  RouteConfigurationObservationSnapshot(RouteConfigurationObservationSnapshot&&) noexcept =
      default;
  RouteConfigurationObservationSnapshot& operator=(
      RouteConfigurationObservationSnapshot&&) noexcept = default;

  [[nodiscard]] const RouteConfigurationObservationSnapshotDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const ControllerLifecycleExpectedDomain& expected_controller_domain()
      const noexcept {
    return expected_controller_domain_;
  }

 private:
  RouteConfigurationObservationSnapshot(RouteConfigurationObservationSnapshotDTO value,
                                        ControllerLifecycleExpectedDomain expected_controller_domain)
      : value_(std::move(value)),
        expected_controller_domain_(std::move(expected_controller_domain)) {}

  RouteConfigurationObservationSnapshotDTO value_;
  ControllerLifecycleExpectedDomain expected_controller_domain_;

  friend Result<RouteConfigurationObservationSnapshot>
  verify_route_configuration_observation_snapshot(RouteConfigurationObservationSnapshotDTO);
};

/// Verifies the retained identity, the expected controller domain and that
/// any current configuration is exact truth for that domain. Component
/// lifecycle state is deliberately not part of this closure.
[[nodiscard]] Result<RouteConfigurationObservationSnapshot>
verify_route_configuration_observation_snapshot(RouteConfigurationObservationSnapshotDTO value);

/// Construct one candidate-generation active configuration exclusively from
/// sealed retained facts and the adapter-observed selected configuration.
/// The desired configuration is used only as the reconciliation comparator;
/// it is never substituted for absent or external observed route truth.
[[nodiscard]] Result<ActiveControlConfigurationDTO> ResolveRouteSelectionActiveControlConfiguration(
    const RouteSelectionAcceptedDTO& acceptance,
    const ControllerLifecycleExpectedDomain& expected_controller_domain,
    const ControllerLifecycleObservationDTO& observation,
    const ControllerLifecycleReconciliationDTO& reconciliation,
    RouteSelectionEffectDTO transaction_effect,
    std::vector<KinematicResourceSourceProjectionDTO> source_projections);

/// Exact independently read-back readiness row for one selected control
/// binding. A route publication is complete only when every selected binding
/// has one row and every row points at an immutable verified base plan.
struct RouteReadinessPublicationRowDTO final {
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ResolvedStopBasePlanId resolved_stop_base_plan_id;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteReadinessPublicationRowDTO&,
                         const RouteReadinessPublicationRowDTO&) = default;
};

[[nodiscard]] bool CanonicalRouteReadinessPublicationRowLess(
    const RouteReadinessPublicationRowDTO& lhs,
    const RouteReadinessPublicationRowDTO& rhs) noexcept;

/// Receipt returned only after configuration, base-plan repository, sealed
/// readiness, and Kernel registration readback all agree.
struct RouteReadinessPublicationReceiptDTO final {
  RouteSelectionTransactionId route_selection_transaction_id;
  RouteSelectionTransactionGeneration route_selection_transaction_generation;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation{0U};
  std::vector<RouteReadinessPublicationRowDTO> readiness_rows;
  /// Domain route readiness: the route and its Stop readiness are coherent.
  /// It is not component command eligibility, which the Kernel keeps
  /// separately per component and lifecycle generation.
  bool operation_admission_open{};
  std::int64_t published_at_ns{};
  ContentDigest receipt_digest;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const RouteReadinessPublicationReceiptDTO&,
                         const RouteReadinessPublicationReceiptDTO&) = default;
};

/// Exact Kernel-owned route/configuration projection after one atomic runtime
/// registration publication. This is an internal composition readback, not a
/// public request or a replacement for the durable CCLC route status.
struct RuntimeRouteConfigurationReadbackDTO final {
  ActiveControlConfigurationDTO active_configuration;
  std::vector<RouteReadinessPublicationRowDTO> readiness_rows;
  /// Domain route readiness: the route and its Stop readiness are coherent.
  /// It is not component command eligibility, which the Kernel keeps
  /// separately per component and lifecycle generation.
  bool operation_admission_open{};

  friend bool operator==(const RuntimeRouteConfigurationReadbackDTO&,
                         const RuntimeRouteConfigurationReadbackDTO&) = default;
};

/// Verify a publisher receipt against the exact already-durable route status.
/// Reconciled configuration requires one readiness row per selected binding
/// and open admission. Unreconciled truth requires no readiness rows and a
/// closed persistent configuration gate.
[[nodiscard]] Result<void> VerifyRouteReadinessPublicationReceipt(
    const RouteSelectionStatusDTO& durable_status,
    const RouteReadinessPublicationReceiptDTO& receipt);

}  // namespace manyforge::core
