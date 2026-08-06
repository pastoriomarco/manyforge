#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <manyforge_core/types/coordinate_state.hpp>

namespace manyforge::core {

/// Exact owner-issued coordinates for one observed-state query. The ordered
/// resources, names, and kinds are copied from the retained control-interface
/// binding; callers cannot select or reorder them.
struct ObservedStateQueryContextDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  std::vector<ResourceId> resources;
  std::vector<std::string> joint_names;
  std::vector<CoordinateKind> coordinate_kinds;
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeEpoch runtime_epoch;
  StateAuthorityId state_authority_id;
  StateAuthorityClass state_authority_class{StateAuthorityClass::kMeasured};
  std::int64_t freshness_budget_ns{};

  friend bool operator==(const ObservedStateQueryContextDTO&,
                         const ObservedStateQueryContextDTO&) = default;
};

/// Latest immutable value publication for one exact control binding.
struct ObservedStatePublicationDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  StateAuthorityId state_authority_id;
  StateAuthorityClass state_authority_class{StateAuthorityClass::kMeasured};
  RuntimeEpoch runtime_epoch;
  std::uint64_t authority_generation{};
  std::uint64_t publication_sequence{};
  ObservedStateSnapshotDTO snapshot;
  std::int64_t received_timestamp_ns{};

  friend bool operator==(const ObservedStatePublicationDTO&,
                         const ObservedStatePublicationDTO&) = default;
};

}  // namespace manyforge::core
