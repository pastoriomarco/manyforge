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
inline constexpr std::size_t kMaxControllerLifecycleInterfaceTokenBytes = 128U;
inline constexpr std::size_t kMaxControllerLifecycleImplementationTypeBytes = 256U;
inline constexpr std::size_t kMaxControllerLifecycleEndpointSymbolBytes = 256U;

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

/// Selected strict pneumatic none has no physical ROS controller.
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

}  // namespace manyforge::core
