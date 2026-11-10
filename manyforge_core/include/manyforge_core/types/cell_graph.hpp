#pragma once

#include <compare>
#include <cstdint>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/json_value.hpp>
#include <manyforge_core/types/placement_pose.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kCellGraphApiVersion = "manyforge.cell_graph.v1";

enum class ContainmentGranularity : std::uint8_t {
  kResource = 0,
  kControllerSet,
  kControlDomain,
};

enum class StateAuthorityClass : std::uint8_t {
  kMeasured = 0,
  kCommanded,
  kPredicted,
};

/// Binding-selected acknowledgement contract for a live runtime-scale
/// command. The current closed alternative requires an independently readable
/// applied-state value; command delivery alone is never application evidence.
enum class RuntimeScaleAcknowledgementKind : std::uint8_t {
  kAppliedState = 0,
};

[[nodiscard]] constexpr std::string_view to_string(ContainmentGranularity value) noexcept {
  switch (value) {
    case ContainmentGranularity::kResource:
      return "resource";
    case ContainmentGranularity::kControllerSet:
      return "controller_set";
    case ContainmentGranularity::kControlDomain:
      return "control_domain";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(StateAuthorityClass value) noexcept {
  switch (value) {
    case StateAuthorityClass::kMeasured:
      return "measured";
    case StateAuthorityClass::kCommanded:
      return "commanded";
    case StateAuthorityClass::kPredicted:
      return "predicted";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(
    RuntimeScaleAcknowledgementKind value) noexcept {
  switch (value) {
    case RuntimeScaleAcknowledgementKind::kAppliedState:
      return "applied_state";
  }
  return {};
}

struct ModelRoleBindingDTO {
  ModelRoleId model_role_id;
  ModelInstanceId model_instance_id;

  friend bool operator==(const ModelRoleBindingDTO&, const ModelRoleBindingDTO&) = default;
};

struct ResourceBindingDTO {
  ResourceRoleId resource_role_id;
  ResourceId resource_id;

  friend bool operator==(const ResourceBindingDTO&, const ResourceBindingDTO&) = default;
};

struct RouteEndpointProjectionDTO {
  ControlRouteId control_route_id;
  std::vector<EndpointRoleId> command_endpoint_role_ids;
  std::vector<EndpointRoleId> state_endpoint_role_ids;

  friend bool operator==(const RouteEndpointProjectionDTO&,
                         const RouteEndpointProjectionDTO&) = default;
};

/// Route-specific retained qualification for runtime scaling.
/// Absence means unsupported. Endpoint roles must belong to the named route's
/// exact command/state projections; no runtime adapter may infer support from
/// provider brand or implementation type.
struct RuntimeScaleRouteQualificationDTO {
  ControlRouteId control_route_id;
  ScalingDomainId scaling_domain_id;
  EndpointRoleId command_endpoint_role_id;
  EndpointRoleId applied_state_endpoint_role_id;
  std::optional<CoordinatedScalingBarrierId> coordinated_barrier_id;
  RuntimeScaleAcknowledgementKind acknowledgement_kind{
      RuntimeScaleAcknowledgementKind::kAppliedState};
  double maximum_slew_per_second{};
  std::int64_t freshness_budget_ns{};

  friend bool operator==(const RuntimeScaleRouteQualificationDTO&,
                         const RuntimeScaleRouteQualificationDTO&) = default;
};

struct ComponentAttachmentEndpointDTO {
  ControllableComponentId controllable_component_id;
  AttachmentInterfaceId attachment_interface_id;

  friend bool operator==(const ComponentAttachmentEndpointDTO&,
                         const ComponentAttachmentEndpointDTO&) = default;
};

// Catalog references in graph records carry only the referent's typed ID and
// semantic revision. Schema identity, exact-byte content digest, and store key
// belong exclusively to CatalogRecordRefDTO.
/// One of exactly six mutable graph-record kinds.
struct RuntimeUnitDTO {
  RuntimeUnitId runtime_unit_id;
  RuntimeProfileId runtime_profile_id;
  RuntimeProfileRevision runtime_profile_revision;
  std::vector<RuntimeUnitId> dependency_runtime_unit_ids;
  JsonObjectDTO configuration;

  friend bool operator==(const RuntimeUnitDTO&, const RuntimeUnitDTO&) = default;
};

/// One of exactly six mutable graph-record kinds.
/// One user-authored custom TCP of a component's model occurrence (the
/// Deployment row's `custom_tcps`, parent resolved to a link). It composes
/// like a descriptor offset frame: a generated massless link named
/// `frame_role_id` fixed to `parent_link_name` at `pose_in_parent_link`.
/// The parent is a source link or a descriptor offset frame, never another
/// custom TCP.
struct ModelCustomTcpDTO {
  FrameRoleId frame_role_id;
  std::string parent_link_name;
  PlacementPoseDTO pose_in_parent_link;

  friend bool operator==(const ModelCustomTcpDTO&, const ModelCustomTcpDTO&) = default;
};

struct ModelInstanceDTO {
  ModelInstanceId model_instance_id;
  ModelAssetId model_asset_id;
  ModelRevision model_revision;
  std::optional<PlacementPoseDTO> root_pose_in_cell;
  /// Sorted by `frame_role_id`; only on a component-owned occurrence.
  std::vector<ModelCustomTcpDTO> custom_tcps{};

  friend bool operator==(const ModelInstanceDTO&, const ModelInstanceDTO&) = default;
};

/// One of exactly six mutable graph-record kinds.
struct ControllableComponentDTO {
  ControllableComponentId controllable_component_id;
  ComponentTypeId component_type_id;
  ComponentTypeRevision component_type_revision;
  std::vector<ModelRoleBindingDTO> model_role_bindings;
  std::vector<RuntimeUnitId> runtime_unit_ids;
  std::optional<PlacementPoseDTO> root_pose_in_cell;
  /// B5: the component's default TCP (authored `default_tcp`, else the one
  /// target that declares a default); absent when it has no TCP.
  std::optional<ComponentDefaultTcpDTO> default_tcp{};

  friend bool operator==(const ControllableComponentDTO&,
                         const ControllableComponentDTO&) = default;
};

struct ControllerControlDomainDTO final {
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  friend bool operator==(const ControllerControlDomainDTO&,
                         const ControllerControlDomainDTO&) = default;
};

/// Compiler-derived ownership of one genuine I/O driver. Signal membership
/// and its normalized configuration digest cover only this provider's rows.
/// No motion controller/topology/route is implied by this alternative.
struct ProviderControlDomainDTO final {
  ControllableComponentId controllable_component_id;
  RuntimeUnitId provider_runtime_unit_id;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  ContentDigest lifecycle_plan_catalog_digest;
  ContentDigest signal_configuration_digest;
  std::vector<std::string> signal_names;
  Duration lifecycle_command_timeout_ns;
  [[nodiscard]] bool locally_valid() const noexcept;
  friend bool operator==(const ProviderControlDomainDTO&,
                         const ProviderControlDomainDTO&) = default;
};

/// One of exactly six mutable graph-record kinds. The controller alternative
/// retains its existing canonical wire representation.
struct ControlDomainDTO {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  std::vector<RuntimeUnitId> runtime_unit_ids;
  EndpointRoleId control_coordinator_endpoint_role;
  ContainmentGranularity containment_granularity;
  std::variant<ControllerControlDomainDTO, ProviderControlDomainDTO> payload;

  ControlDomainDTO(ControlDomainId id, ControlDomainRevision revision,
                   std::vector<RuntimeUnitId> units, EndpointRoleId endpoint,
                   ControllerTopologyId topology, TopologyRevision topology_revision,
                   ContainmentGranularity containment)
      : control_domain_id(std::move(id)),
        control_domain_revision(std::move(revision)),
        runtime_unit_ids(std::move(units)),
        control_coordinator_endpoint_role(std::move(endpoint)),
        containment_granularity(containment),
        payload(ControllerControlDomainDTO{std::move(topology), std::move(topology_revision)}) {}

  ControlDomainDTO(ControlDomainId id, ControlDomainRevision revision,
                   std::vector<RuntimeUnitId> units, EndpointRoleId endpoint,
                   ContainmentGranularity containment, ProviderControlDomainDTO provider)
      : control_domain_id(std::move(id)),
        control_domain_revision(std::move(revision)),
        runtime_unit_ids(std::move(units)),
        control_coordinator_endpoint_role(std::move(endpoint)),
        containment_granularity(containment),
        payload(std::move(provider)) {}

  [[nodiscard]] ControllerControlDomainDTO& controller() {
    return std::get<ControllerControlDomainDTO>(payload);
  }
  [[nodiscard]] const ControllerControlDomainDTO& controller() const {
    return std::get<ControllerControlDomainDTO>(payload);
  }
  [[nodiscard]] const ProviderControlDomainDTO* provider() const noexcept {
    return std::get_if<ProviderControlDomainDTO>(&payload);
  }

  friend bool operator==(const ControlDomainDTO&, const ControlDomainDTO&) = default;
};

/// One of exactly six mutable graph-record kinds.
struct ControlBindingDTO {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  ControlDomainId control_domain_id;
  std::vector<ResourceBindingDTO> resource_bindings;
  ControlInterfaceContractId control_interface_contract_id;
  ControlInterfaceRevision control_interface_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  std::vector<ControlRouteId> legal_control_route_ids;
  std::vector<RouteEndpointProjectionDTO> route_endpoint_projections;
  ProviderInstanceId provider_instance_id;
  /// Exact managed runtime unit that hosts this binding's provider instance.
  /// External endpoints remain resources reached through that managed owner.
  RuntimeUnitId provider_runtime_unit_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  StateAuthorityId state_authority_id;
  StateAuthorityClass state_authority_class;
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  ContainmentGranularity containment_granularity;
  /// Canonical route-keyed qualifications. Empty means this binding has no
  /// retained operation-scoped runtime-scaling support.
  std::vector<RuntimeScaleRouteQualificationDTO>
      runtime_scale_route_qualifications;

  friend bool operator==(const ControlBindingDTO&, const ControlBindingDTO&) = default;
};

/// A link pair the mount disables for collision checking: a source link of
/// the child component's model and one of the parent component's model.
struct ComponentAttachmentCollisionExclusionDTO {
  std::string child_link;
  std::string parent_link;

  friend auto operator<=>(const ComponentAttachmentCollisionExclusionDTO&,
                          const ComponentAttachmentCollisionExclusionDTO&) = default;
};

/// One of exactly six mutable graph-record kinds.
struct ComponentAttachmentEdgeDTO {
  ComponentAttachmentId component_attachment_id;
  ComponentAttachmentRevision component_attachment_revision;
  ComponentAttachmentEndpointDTO parent;
  ComponentAttachmentEndpointDTO child;
  PlacementPoseDTO child_interface_pose_in_parent_interface;
  /// B5: the authored primary tool among several on one parent interface.
  bool primary{};
  /// Authored arm-tool pairs that never touch, sorted; empty when unset.
  std::vector<ComponentAttachmentCollisionExclusionDTO> collision_exclusions{};

  friend bool operator==(const ComponentAttachmentEdgeDTO&,
                         const ComponentAttachmentEdgeDTO&) = default;
};

/// Closed v1 graph envelope. Individual records deliberately do not repeat
/// graph_revision.
struct CellGraphDTO {
  std::string api_version;
  GraphRevision graph_revision;
  CellId cell_id;
  std::vector<RuntimeUnitDTO> runtime_units;
  std::vector<ModelInstanceDTO> model_instances;
  std::vector<ControllableComponentDTO> controllable_components;
  std::vector<ControlDomainDTO> control_domains;
  std::vector<ControlBindingDTO> control_bindings;
  std::vector<ComponentAttachmentEdgeDTO> component_attachments;

  friend bool operator==(const CellGraphDTO&, const CellGraphDTO&) = default;
};

}  // namespace manyforge::core
