#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/observed_state.hpp>
#include <manyforge_core/types/placement_pose.hpp>
#include <manyforge_core/types/resolved_frame.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

struct KinematicControlBindingSourceDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;

  friend bool operator==(const KinematicControlBindingSourceDTO&,
                         const KinematicControlBindingSourceDTO&) = default;
};

/// Compiler-owned candidate mapping from one authority resource to one model
/// coordinate. Active-control facts select candidates; callers never do.
struct KinematicResourceCoordinateDependencyDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ResourceId resource_id;
  CoordinateKind coordinate_kind{CoordinateKind::kAngular};
  double multiplier{1.0};
  double offset{};
  bool derived_mimic{};

  friend bool operator==(const KinematicResourceCoordinateDependencyDTO&,
                         const KinematicResourceCoordinateDependencyDTO&) = default;
};

enum class KinematicJointCoordinateKind : std::uint8_t {
  kFixed = 0,
  kAngular,
  kLinear,
  kPlanar,
  kFloating,
};

struct KinematicDirectModelAnchorPathSegmentDTO final {
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::string compiled_joint_name;
  std::string compiled_parent_link_name;
  std::string compiled_child_link_name;
  PlacementPoseDTO child_pose_in_parent;

  friend bool operator==(const KinematicDirectModelAnchorPathSegmentDTO&,
                         const KinematicDirectModelAnchorPathSegmentDTO&) = default;
};

struct KinematicDirectComponentAnchorPathSegmentDTO final {
  ControllableComponentId controllable_component_id;
  ModelInstanceId root_model_instance_id;
  ModelRevision root_model_revision;
  std::string compiled_joint_name;
  std::string compiled_parent_link_name;
  std::string compiled_child_link_name;
  PlacementPoseDTO child_pose_in_parent;

  friend bool operator==(const KinematicDirectComponentAnchorPathSegmentDTO&,
                         const KinematicDirectComponentAnchorPathSegmentDTO&) = default;
};

struct KinematicModelMountPathSegmentDTO final {
  ControllableComponentId controllable_component_id;
  ModelInstanceId parent_model_instance_id;
  ModelRevision parent_model_revision;
  ModelInstanceId child_model_instance_id;
  ModelRevision child_model_revision;
  std::string compiled_joint_name;
  std::string compiled_parent_link_name;
  std::string compiled_child_link_name;
  PlacementPoseDTO child_pose_in_parent;

  friend bool operator==(const KinematicModelMountPathSegmentDTO&,
                         const KinematicModelMountPathSegmentDTO&) = default;
};

struct KinematicComponentAttachmentPathSegmentDTO final {
  ComponentAttachmentId component_attachment_id;
  ControllableComponentId parent_component_id;
  ControllableComponentId child_component_id;
  std::string compiled_joint_name;
  std::string compiled_parent_link_name;
  std::string compiled_child_link_name;
  PlacementPoseDTO child_pose_in_parent;

  friend bool operator==(const KinematicComponentAttachmentPathSegmentDTO&,
                         const KinematicComponentAttachmentPathSegmentDTO&) = default;
};

struct KinematicModelJointPathSegmentDTO final {
  std::optional<ControllableComponentId> controllable_component_id;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::size_t composition_coordinate_index{};
  std::string source_joint_name;
  std::string compiled_joint_name;
  std::string compiled_parent_link_name;
  std::string compiled_child_link_name;
  KinematicJointCoordinateKind coordinate_kind{KinematicJointCoordinateKind::kFixed};
  std::vector<KinematicResourceCoordinateDependencyDTO> eligible_resource_dependencies;

  friend bool operator==(const KinematicModelJointPathSegmentDTO&,
                         const KinematicModelJointPathSegmentDTO&) = default;
};

using KinematicFramePathSegmentDTO =
    std::variant<KinematicDirectModelAnchorPathSegmentDTO,
                 KinematicDirectComponentAnchorPathSegmentDTO, KinematicModelMountPathSegmentDTO,
                 KinematicComponentAttachmentPathSegmentDTO, KinematicModelJointPathSegmentDTO>;

struct KinematicFramePathDTO final {
  ResolvedFrameDTO terminal_frame;
  std::vector<KinematicFramePathSegmentDTO> segments;

  friend bool operator==(const KinematicFramePathDTO&, const KinematicFramePathDTO&) = default;
};

struct KinematicActiveControlConfigurationExpectationDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;

  friend bool operator==(const KinematicActiveControlConfigurationExpectationDTO&,
                         const KinematicActiveControlConfigurationExpectationDTO&) = default;
};

struct KinematicResourceSourceProjectionDTO final {
  ResourceId resource_id;
  std::vector<KinematicControlBindingSourceDTO> contributing_control_bindings;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  StateAuthorityId state_authority_id;
  StateAuthorityClass state_authority_class{StateAuthorityClass::kMeasured};
  ObservedStateSourceKind publication_source{ObservedStateSourceKind::kJointState};

  friend bool operator==(const KinematicResourceSourceProjectionDTO&,
                         const KinematicResourceSourceProjectionDTO&) = default;
};

struct KinematicStatePathQueryDTO final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  KinematicFramePathDTO frame_path;
  std::vector<KinematicActiveControlConfigurationExpectationDTO>
      expected_active_control_configurations;
  std::int64_t maximum_receiver_time_skew_ns{};
  std::vector<KinematicResourceSourceProjectionDTO> source_projections;

  friend bool operator==(const KinematicStatePathQueryDTO&,
                         const KinematicStatePathQueryDTO&) = default;
};

struct KinematicResourceStateRowDTO final {
  ResourceId resource_id;
  CoordinatePositionDTO position;
  KinematicResourceSourceProjectionDTO source_projection;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  RuntimeEpoch runtime_epoch;
  std::uint64_t authority_generation{};
  std::uint64_t publication_sequence{};
  std::int64_t source_timestamp_ns{};
  std::int64_t snapshot_received_timestamp_ns{};
  std::int64_t publication_received_timestamp_ns{};
  std::int64_t freshness_budget_ns{};

  friend bool operator==(const KinematicResourceStateRowDTO&,
                         const KinematicResourceStateRowDTO&) = default;
};

struct KinematicModelCoordinateProjectionDTO final {
  std::size_t composition_coordinate_index{};
  ResourceId resource_id;
  CoordinateKind coordinate_kind{CoordinateKind::kAngular};
  double multiplier{1.0};
  double offset{};
  bool derived_mimic{};

  friend bool operator==(const KinematicModelCoordinateProjectionDTO&,
                         const KinematicModelCoordinateProjectionDTO&) = default;
};

struct ComponentModelStateViewDTO final {
  ControllableComponentId controllable_component_id;
  ModelInstanceId model_instance_id;
  ModelRevision model_revision;
  std::vector<KinematicModelCoordinateProjectionDTO> coordinate_projection;
  std::vector<CoordinatePositionDTO> positions;

  friend bool operator==(const ComponentModelStateViewDTO&,
                         const ComponentModelStateViewDTO&) = default;
};

struct KinematicStatePathViewDTO final {
  KinematicStatePathQueryDTO query;
  std::vector<KinematicResourceStateRowDTO> resource_states;
  std::vector<ComponentModelStateViewDTO> model_states;
  std::int64_t evaluated_at_ns{};
  std::optional<std::int64_t> earliest_received_timestamp_ns;
  std::optional<std::int64_t> latest_received_timestamp_ns;
  std::int64_t actual_receiver_time_skew_ns{};
  std::int64_t accepted_receiver_time_skew_ns{};
  ContentDigest view_digest;

  friend bool operator==(const KinematicStatePathViewDTO&,
                         const KinematicStatePathViewDTO&) = default;
};

}  // namespace manyforge::core
