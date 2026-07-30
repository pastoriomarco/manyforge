#pragma once

#include <cstddef>
#include <manyforge_core/types/control_interface_contract.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kControllerTopologyApiVersion =
    "manyforge.controller_topology.v1";

inline constexpr std::size_t kMaxControllerTopologyControllers = 256U;
inline constexpr std::size_t
    kMaxControllerTopologyControlInterfaceClaimsPerController = 256U;
inline constexpr std::size_t
    kMaxControllerTopologyClaimedDirectionsPerClaim = 2U;
inline constexpr std::size_t
    kMaxControllerTopologyExportedReferenceInterfacesPerController =
    kMaxControlInterfaceContractItems;
inline constexpr std::size_t
    kMaxControllerTopologyExportedStateInterfacesPerController =
        kMaxControlInterfaceContractItems;
inline constexpr std::size_t kMaxControllerTopologyChainEdges = 4096U;
inline constexpr std::size_t
    kMaxControllerTopologyReferenceInterfacesPerChainEdge =
        kMaxControlInterfaceContractItems;
inline constexpr std::size_t
    kMaxControllerTopologyStateInterfacesPerChainEdge =
        kMaxControlInterfaceContractItems;
inline constexpr std::size_t kMaxControllerTopologyActivationSets = 256U;
inline constexpr std::size_t
    kMaxControllerTopologyActiveControllerRolesPerActivationSet = 256U;
inline constexpr std::size_t
    kMaxControllerTopologyActiveChainEdgesPerActivationSet = 4096U;
inline constexpr std::size_t kMaxControllerTopologyControlRoutes = 256U;
inline constexpr std::size_t
    kMaxControllerTopologyOrderedControllerRolesPerRoute = 256U;
inline constexpr std::size_t
    kMaxControllerTopologyCompatibleActivationSetsPerRoute = 256U;
inline constexpr std::size_t
    kMaxControllerTopologyDistinctControlInterfaceContracts = 256U;

/// The direction in which one controller uses an exact control-interface
/// contract. A claim may select either direction or both.
enum class ControllerClaimDirection {
  kCommand,
  kState,
};

/// One exact control-interface contract selected by a controller role.
///
/// `claimed_directions` is a canonical non-empty set. The qualified topology
/// validator proves that each selected direction is non-empty in the exact
/// referenced contract.
struct ControllerControlInterfaceClaimDTO {
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  std::vector<ControllerClaimDirection> claimed_directions;

  friend bool operator==(const ControllerControlInterfaceClaimDTO&,
                         const ControllerControlInterfaceClaimDTO&) = default;
};

/// One logical controller role declared by a reusable controller topology.
///
/// Interface claims select authored control-interface contracts. Exported
/// reference/state interfaces are expressed through the same abstract
/// resource-role vocabulary and never contain generated controller, endpoint,
/// or deployment-resource names.
struct ControllerTopologyControllerDTO {
  ControllerRoleId controller_role_id;
  std::vector<ControllerControlInterfaceClaimDTO> control_interface_claims;
  std::vector<ResourceInterfaceClaimDTO> exported_reference_interfaces;
  std::vector<ResourceInterfaceClaimDTO> exported_state_interfaces;

  friend bool operator==(const ControllerTopologyControllerDTO&,
                         const ControllerTopologyControllerDTO&) = default;
};

/// One directed controller chain from the external-command side toward the
/// hardware side.
struct ControllerChainEdgeDTO {
  ControllerRoleId upstream_controller_role_id;
  ControllerRoleId downstream_controller_role_id;
  std::vector<ResourceInterfaceClaimDTO> reference_interfaces;
  std::vector<ResourceInterfaceClaimDTO> state_interfaces;

  friend bool operator==(const ControllerChainEdgeDTO&, const ControllerChainEdgeDTO&) = default;
};

/// Exact topology-local identity of a controller chain edge.
struct ControllerChainEdgeRefDTO {
  ControllerRoleId upstream_controller_role_id;
  ControllerRoleId downstream_controller_role_id;

  friend bool operator==(const ControllerChainEdgeRefDTO&,
                         const ControllerChainEdgeRefDTO&) = default;
};

/// One complete, compatible set of simultaneously active controller roles and
/// chain edges.
struct ControllerActivationSetDTO {
  ActivationSetId activation_set_id;
  std::vector<ControllerRoleId> active_controller_role_ids;
  std::vector<ControllerChainEdgeRefDTO> active_chain_edges;

  friend bool operator==(const ControllerActivationSetDTO&,
                         const ControllerActivationSetDTO&) = default;
};

/// One named legal path through a controller topology.
///
/// The ordered roles run from the external-command side toward the hardware
/// side. Compatible activation sets are a canonical non-empty set.
struct ControlRouteDTO {
  ControlRouteId control_route_id;
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  std::vector<ControllerRoleId> ordered_controller_role_ids;
  std::vector<ActivationSetId> compatible_activation_set_ids;

  friend bool operator==(const ControlRouteDTO&, const ControlRouteDTO&) = default;
};

/// Exact normalized payload for `manyforge.controller_topology.v1`.
///
/// The API version is fixed by this type and
/// `kControllerTopologyApiVersion`. This reusable catalog declaration contains
/// only logical controller roles, abstract interface claims, legal chain
/// structure, compatible activation sets, and named control routes. It does
/// not contain plugins, controller configuration, generated names, endpoints,
/// runtime state, or a selected active route.
struct ControllerTopologyDTO {
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  std::vector<ControllerTopologyControllerDTO> controllers;
  std::vector<ControllerChainEdgeDTO> chain_edges;
  std::vector<ControllerActivationSetDTO> activation_sets;
  std::vector<ControlRouteDTO> legal_routes;

  friend bool operator==(const ControllerTopologyDTO&, const ControllerTopologyDTO&) = default;
};

}  // namespace manyforge::core
