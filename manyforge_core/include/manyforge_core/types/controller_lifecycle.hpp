#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/gripper.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_core/types/controller_topology.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/retained_artifact.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace manyforge::core {

struct ControllerImplementationIdTag final {};
using ControllerImplementationId = StrongString<ControllerImplementationIdTag>;

struct ControllerImplementationRevisionTag final {};
using ControllerImplementationRevision = StrongString<ControllerImplementationRevisionTag>;

inline constexpr std::size_t kMaxControllerLifecycleInterfaces = 4096U;
inline constexpr std::size_t kMaxControllerLifecycleEndpoints = 4096U;
inline constexpr std::size_t kMaxControllerLifecycleBindingRoutes = 4096U;
inline constexpr std::size_t kMaxControllerLifecycleTransitionSteps = 512U;
inline constexpr std::size_t kMaxControllerLifecycleReasonCodes = 16U;
inline constexpr std::size_t kMaxControllerLifecycleInterfaceTokenBytes = 128U;
inline constexpr std::size_t kMaxControllerLifecycleImplementationTypeBytes = 256U;
inline constexpr std::size_t kMaxControllerLifecycleEndpointSymbolBytes = 256U;
inline constexpr std::size_t kMaxControllerLifecycleReasonCodeBytes = 128U;

/// Exact concrete resource/interface pair normalized by the selected adapter.
struct ControllerInterfaceInstanceDTO final {
  ResourceId resource_id;
  std::string interface_name;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerInterfaceInstanceDTO&,
                         const ControllerInterfaceInstanceDTO&) = default;
};

[[nodiscard]] inline bool CanonicalControllerInterfaceInstanceLess(
    const ControllerInterfaceInstanceDTO& lhs, const ControllerInterfaceInstanceDTO& rhs) noexcept {
  return std::tie(lhs.resource_id, lhs.interface_name) <
         std::tie(rhs.resource_id, rhs.interface_name);
}

/// Product-qualified expected installed controller projection.
struct ControllerLifecycleExpectedControllerDTO final {
  ControllerRoleId controller_role_id;
  ControllerImplementationId implementation_id;
  ControllerImplementationRevision implementation_revision;
  std::string implementation_type;
  ContentDigest configuration_digest;
  std::vector<ControllerInterfaceInstanceDTO> claimed_command_interfaces;
  std::vector<ControllerInterfaceInstanceDTO> claimed_state_interfaces;
  std::vector<ControllerInterfaceInstanceDTO> exported_reference_interfaces;
  std::vector<ControllerInterfaceInstanceDTO> exported_state_interfaces;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleExpectedControllerDTO&,
                         const ControllerLifecycleExpectedControllerDTO&) = default;
};

enum class ControllerLifecycleActivityDTO : std::uint8_t {
  kUnconfigured = 0,
  kInactive,
  kActive,
  kFinalized,
  kUnknown,
};

[[nodiscard]] constexpr std::string_view to_string(
    ControllerLifecycleActivityDTO activity) noexcept {
  switch (activity) {
    case ControllerLifecycleActivityDTO::kUnconfigured:
      return "unconfigured";
    case ControllerLifecycleActivityDTO::kInactive:
      return "inactive";
    case ControllerLifecycleActivityDTO::kActive:
      return "active";
    case ControllerLifecycleActivityDTO::kFinalized:
      return "finalized";
    case ControllerLifecycleActivityDTO::kUnknown:
      return "unknown";
  }
  return {};
}

[[nodiscard]] constexpr bool IsKnownControllerLifecycleActivity(
    ControllerLifecycleActivityDTO activity) noexcept {
  return !to_string(activity).empty();
}

struct ControllerLifecycleObservedControllerDTO final {
  ControllerLifecycleExpectedControllerDTO expected_controller;
  ControllerLifecycleActivityDTO activity;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleObservedControllerDTO&,
                         const ControllerLifecycleObservedControllerDTO&) = default;
};

/// Exact selected-route endpoint projection from the retained control product.
struct ControllerLifecycleExpectedEndpointDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControlRouteId control_route_id;
  EndpointRoleId endpoint_role_id;
  std::string logical_endpoint_symbol;
  bool is_command_endpoint{};
  bool is_state_endpoint{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleExpectedEndpointDTO&,
                         const ControllerLifecycleExpectedEndpointDTO&) = default;
};

[[nodiscard]] inline bool CanonicalControllerLifecycleEndpointLess(
    const ControllerLifecycleExpectedEndpointDTO& lhs,
    const ControllerLifecycleExpectedEndpointDTO& rhs) noexcept {
  return std::tie(lhs.control_binding_id, lhs.control_route_id, lhs.endpoint_role_id) <
         std::tie(rhs.control_binding_id, rhs.control_route_id, rhs.endpoint_role_id);
}

struct ControllerLifecycleObservedEndpointDTO final {
  ControllerLifecycleExpectedEndpointDTO expected_endpoint;
  bool ready{};

  [[nodiscard]] bool locally_valid() const noexcept { return expected_endpoint.locally_valid(); }

  friend bool operator==(const ControllerLifecycleObservedEndpointDTO&,
                         const ControllerLifecycleObservedEndpointDTO&) = default;
};

/// Exact selected route/activation configuration identified by one adapter
/// observation.
///
/// Absence from an observation means the adapter cannot yet prove one
/// complete selected configuration. Controller activities, active edges, and
/// endpoint readiness remain separate observed facts and may show that this
/// selected configuration is not physically reconciled. Callers must not
/// substitute the desired configuration when this value is absent.
struct ControllerLifecycleObservedConfigurationDTO final {
  ActivationSetId activation_set_id;
  std::vector<ActiveControlConfigurationBindingRouteDTO> binding_routes;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleObservedConfigurationDTO&,
                         const ControllerLifecycleObservedConfigurationDTO&) = default;
};

/// Exact resource-bound interface contract for one retained control binding.
///
/// This row is decoded from the retained control product together with the
/// controller and endpoint tables.  It prevents a prepared participant from
/// treating aggregate controller claims as authority for a particular
/// binding when one domain contains more than one interface contract.
struct ControllerLifecycleExpectedBindingDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  std::vector<ControllerInterfaceInstanceDTO> claimed_command_interfaces;
  std::vector<ControllerInterfaceInstanceDTO> claimed_state_interfaces;

  /// Exact source-qualified logical resources for strict pneumatic none only.
  /// Physical-required domains keep this empty and retain interface claims.
  std::vector<ResourceId> logical_owner_resources{};

  /// Strict source-qualified actuation; absence retains the ordinary hard floor.
  std::optional<GripperActuationKind> gripper_actuation_kind{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleExpectedBindingDTO&,
                         const ControllerLifecycleExpectedBindingDTO&) = default;
};

[[nodiscard]] inline bool CanonicalControllerLifecycleExpectedBindingLess(
    const ControllerLifecycleExpectedBindingDTO& lhs,
    const ControllerLifecycleExpectedBindingDTO& rhs) noexcept {
  return std::tie(lhs.control_binding_id, lhs.control_binding_revision) <
         std::tie(rhs.control_binding_id, rhs.control_binding_revision);
}

/// DX791: selected strict pneumatic none has no physical ROS controller.
/// Deployment seals that source case; this is never inferred from empty rows.
enum class ControllerLifecyclePhysicalExpectation : std::uint8_t {
  kRequired,
  kNotApplicablePneumaticNone,
};

/// Complete independently verified expected closure for one control domain.
struct ControllerLifecycleExpectedDomainDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ProductId product_id;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  ContentDigest control_artifact_digest;
  ContentDigest source_conformance_digest;
  RuntimeUnitId source_runtime_unit_id;
  RuntimeEpoch source_runtime_epoch;
  ControllerTopologyDTO controller_topology;
  DesiredControlConfigurationDTO desired_configuration;
  std::vector<ControllerLifecycleExpectedBindingDTO> expected_bindings;
  std::vector<ControllerLifecycleExpectedControllerDTO> expected_controllers;
  std::vector<ControllerLifecycleExpectedEndpointDTO> expected_endpoints;
  ControllerLifecyclePhysicalExpectation physical_expectation{
      ControllerLifecyclePhysicalExpectation::kRequired};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleExpectedDomainDTO&,
                         const ControllerLifecycleExpectedDomainDTO&) = default;
};

/// Sealed independently decoded controller-domain closure.
///
/// Public operation and prepared-payload callers cannot construct this type.
/// Its raw value remains inspectable so pure evaluators and codecs can retain
/// exact bytes, while downstream seals require this referent in addition to a
/// candidate DTO.
class ControllerLifecycleExpectedDomain final {
 public:
  ControllerLifecycleExpectedDomain(const ControllerLifecycleExpectedDomain&) = default;
  ControllerLifecycleExpectedDomain& operator=(const ControllerLifecycleExpectedDomain&) = default;
  ControllerLifecycleExpectedDomain(ControllerLifecycleExpectedDomain&&) noexcept = default;
  ControllerLifecycleExpectedDomain& operator=(ControllerLifecycleExpectedDomain&&) noexcept =
      default;

  [[nodiscard]] const ControllerLifecycleExpectedDomainDTO& value() const noexcept {
    return value_;
  }

 private:
  explicit ControllerLifecycleExpectedDomain(ControllerLifecycleExpectedDomainDTO value)
      : value_(std::move(value)) {}

  ControllerLifecycleExpectedDomainDTO value_;

  friend Result<ControllerLifecycleExpectedDomain> verify_controller_lifecycle_expected_domain(
      ControllerLifecycleExpectedDomainDTO);
};

/// Seal one owner-produced expected-domain value after full closed-shape and
/// cross-table verification. Deployment's retained-product decoder is the
/// production caller; public request adapters never expose this function.
[[nodiscard]] Result<ControllerLifecycleExpectedDomain> verify_controller_lifecycle_expected_domain(
    ControllerLifecycleExpectedDomainDTO value);

/// Read-only exact selector for one controller-lifecycle source epoch.
struct ControllerLifecycleObservationQueryDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ProductId product_id;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  ContentDigest control_artifact_digest;
  ContentDigest source_conformance_digest;
  RuntimeUnitId source_runtime_unit_id;
  RuntimeEpoch source_runtime_epoch;
  ControllerLifecyclePhysicalExpectation physical_expectation{
      ControllerLifecyclePhysicalExpectation::kRequired};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleObservationQueryDTO&,
                         const ControllerLifecycleObservationQueryDTO&) = default;
};

[[nodiscard]] ControllerLifecycleObservationQueryDTO MakeControllerLifecycleObservationQuery(
    const ControllerLifecycleExpectedDomainDTO& expected);

/// One complete, fresh adapter observation for an exact expected source.
struct ControllerLifecycleObservationDTO final {
  ControllerLifecycleObservationQueryDTO query;
  std::uint64_t source_sequence{};
  std::int64_t observed_at_ns{};
  std::int64_t received_at_ns{};
  std::int64_t valid_until_ns{};
  std::optional<ControllerLifecycleObservedConfigurationDTO> active_configuration;
  std::vector<ControllerLifecycleObservedControllerDTO> controllers;
  std::vector<ControllerChainEdgeRefDTO> active_chain_edges;
  std::vector<ControllerLifecycleObservedEndpointDTO> endpoints;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleObservationDTO&,
                         const ControllerLifecycleObservationDTO&) = default;
};

enum class ControllerLifecycleReconciliationDispositionDTO : std::uint8_t {
  kReconciled = 0,
  kExternalOrUnknownUnreconciled,
};

[[nodiscard]] constexpr std::string_view to_string(
    ControllerLifecycleReconciliationDispositionDTO disposition) noexcept {
  switch (disposition) {
    case ControllerLifecycleReconciliationDispositionDTO::kReconciled:
      return "reconciled";
    case ControllerLifecycleReconciliationDispositionDTO::kExternalOrUnknownUnreconciled:
      return "external_or_unknown_unreconciled";
  }
  return {};
}

struct ControllerLifecycleReconciliationDTO final {
  ControllerLifecycleObservationQueryDTO query;
  std::uint64_t source_sequence{};
  ControllerLifecycleReconciliationDispositionDTO disposition;
  ActivationSetId desired_activation_set_id;
  std::vector<DesiredControlBindingRouteDTO> desired_binding_routes;
  std::vector<std::string> reason_codes;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleReconciliationDTO&,
                         const ControllerLifecycleReconciliationDTO&) = default;
};

enum class ControllerLifecycleTransitionKindDTO : std::uint8_t {
  kDeactivate = 0,
  kActivate,
};

[[nodiscard]] constexpr std::string_view to_string(
    ControllerLifecycleTransitionKindDTO kind) noexcept {
  switch (kind) {
    case ControllerLifecycleTransitionKindDTO::kDeactivate:
      return "deactivate";
    case ControllerLifecycleTransitionKindDTO::kActivate:
      return "activate";
  }
  return {};
}

struct ControllerLifecycleTransitionStepDTO final {
  ControllerLifecycleTransitionKindDTO kind;
  std::vector<ControllerRoleId> controller_role_ids;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleTransitionStepDTO&,
                         const ControllerLifecycleTransitionStepDTO&) = default;
};

/// Pure bounded plan from one fresh exact observation to the selected set.
struct ControllerLifecycleTransitionPlanDTO final {
  ControllerLifecycleObservationQueryDTO query;
  std::uint64_t expected_source_sequence{};
  std::vector<ControllerRoleId> observed_active_controller_role_ids;
  std::vector<ControllerChainEdgeRefDTO> observed_active_chain_edges;
  ActivationSetId desired_activation_set_id;
  std::vector<ControllerRoleId> desired_active_controller_role_ids;
  std::vector<ControllerChainEdgeRefDTO> desired_active_chain_edges;
  std::vector<ControllerLifecycleTransitionStepDTO> steps;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleTransitionPlanDTO&,
                         const ControllerLifecycleTransitionPlanDTO&) = default;
};

struct ControllerLifecycleTransitionIntentDTO final {
  ControllerLifecycleObservationQueryDTO query;
  std::uint64_t expected_source_sequence{};
  std::uint32_t step_index{};
  ControllerLifecycleTransitionStepDTO step;
  std::int64_t remaining_duration_ns{};

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleTransitionIntentDTO&,
                         const ControllerLifecycleTransitionIntentDTO&) = default;
};

enum class ControllerLifecycleTransitionDispatchStateDTO : std::uint8_t {
  kNotDispatched = 0,
  kDispatchedKnown,
  kDispatchedUncertain,
};

enum class ControllerLifecycleTransitionKnownResultDTO : std::uint8_t {
  kSucceeded = 0,
  kFailed,
};

struct ControllerLifecycleTransitionResultDTO final {
  ControllerLifecycleTransitionIntentDTO intent;
  ControllerLifecycleTransitionDispatchStateDTO dispatch_state;
  std::optional<ControllerLifecycleTransitionKnownResultDTO> known_result;
  std::optional<ErrorDTO> error;

  [[nodiscard]] bool locally_valid() const noexcept;

  friend bool operator==(const ControllerLifecycleTransitionResultDTO&,
                         const ControllerLifecycleTransitionResultDTO&) = default;
};

}  // namespace manyforge::core
