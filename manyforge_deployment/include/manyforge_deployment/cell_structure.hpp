#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/component_type.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {

class CellStructureError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class StructuralPlacementSource : std::uint8_t {
  /// A passive/unowned model uses ModelInstanceDTO::root_pose_in_cell.
  kDirectModelPoseInCell = 0,
  /// A root model uses its unattached component's root_pose_in_cell.
  kDirectComponentPoseInCell,
  /// A root model is derived through an incoming component attachment.
  kDerivedFromComponentAttachment,
  /// A non-root model is derived through its component's model-mount tree.
  kDerivedFromComponentModelMount,
};

/// Every graph model occurrence, including passive/unowned geometry.
///
/// The graph remains the placement-value authority. This row only classifies
/// which exact graph relation supplies or derives that placement.
struct StructuralModelInstanceResolution final {
  core::ModelInstanceId model_instance_id;
  core::ModelAssetId model_asset_id;
  core::ModelRevision model_revision;
  std::string source_root_link_name;
  std::optional<core::ControllableComponentId> owner_component_id;
  std::optional<core::ModelRoleId> owner_model_role_id;
  StructuralPlacementSource placement_source;

  friend bool operator==(const StructuralModelInstanceResolution&,
                         const StructuralModelInstanceResolution&) = default;
};

/// One component-local model role resolved to one exact graph occurrence.
struct StructuralModelOccurrence final {
  core::ModelRoleId model_role_id;
  core::ModelInstanceId model_instance_id;
  core::ModelAssetId model_asset_id;
  core::ModelRevision model_revision;

  friend bool operator==(const StructuralModelOccurrence&,
                         const StructuralModelOccurrence&) = default;
};

/// One type-scoped frame role resolved through an exact model occurrence.
///
/// `source_link_name` is still the source symbol. No generated name or scene
/// frame is claimed by this structural phase.
struct StructuralFrameResolution final {
  StructuralModelOccurrence model;
  core::FrameRoleId frame_role_id;
  std::string source_link_name;

  friend bool operator==(const StructuralFrameResolution&,
                         const StructuralFrameResolution&) = default;
};

struct StructuralModelMountResolution final {
  StructuralFrameResolution parent;
  StructuralFrameResolution child;
  core::PlacementPoseDTO child_frame_pose_in_parent_frame;

  friend bool operator==(const StructuralModelMountResolution&,
                         const StructuralModelMountResolution&) = default;
};

struct StructuralModelGroupResolution final {
  core::ModelGroupRoleId model_group_role_id;
  StructuralModelOccurrence model;
  core::ModelGroupRoleId asset_model_group_role_id;
  std::string source_group_name;

  friend bool operator==(const StructuralModelGroupResolution&,
                         const StructuralModelGroupResolution&) = default;
};

struct StructuralModelResourceResolution final {
  core::ResourceRoleId resource_role_id;
  StructuralModelOccurrence model;
  core::ResourceRoleId asset_resource_role_id;
  std::vector<std::string> source_joint_names;

  friend bool operator==(const StructuralModelResourceResolution&,
                         const StructuralModelResourceResolution&) = default;
};

struct StructuralComponentLocalResourceResolution final {
  core::ResourceRoleId resource_role_id;

  friend bool operator==(
      const StructuralComponentLocalResourceResolution&,
      const StructuralComponentLocalResourceResolution&) = default;
};

using StructuralResourceResolution =
    std::variant<StructuralModelResourceResolution,
                 StructuralComponentLocalResourceResolution>;

struct StructuralTargetFrameResolution final {
  core::FrameRoleId frame_role_id;
  StructuralFrameResolution source;

  friend bool operator==(const StructuralTargetFrameResolution&,
                         const StructuralTargetFrameResolution&) = default;
};

struct StructuralCollisionBodyResolution final {
  core::CollisionBodyRoleId collision_body_role_id;
  StructuralModelOccurrence model;
  core::CollisionBodyRoleId asset_collision_body_role_id;
  std::vector<std::string> source_link_names;

  friend bool operator==(const StructuralCollisionBodyResolution&,
                         const StructuralCollisionBodyResolution&) = default;
};

/// Source-role mapping for one declared command target.
///
/// Capability/control compatibility remains deliberately outside this phase.
struct StructuralCommandTargetResolution final {
  core::CommandTargetId command_target_id;
  std::vector<StructuralModelGroupResolution> model_groups;
  std::vector<StructuralResourceResolution> resources;
  std::vector<StructuralTargetFrameResolution> frames;
  std::vector<StructuralCollisionBodyResolution> collision_bodies;

  friend bool operator==(const StructuralCommandTargetResolution&,
                         const StructuralCommandTargetResolution&) = default;
};

struct StructuralAttachmentInterfaceResolution final {
  core::AttachmentInterfaceId attachment_interface_id;
  StructuralFrameResolution frame;
  std::vector<std::string> accepts_tags;
  std::uint64_t max_attachments;

  friend bool operator==(const StructuralAttachmentInterfaceResolution&,
                         const StructuralAttachmentInterfaceResolution&) =
      default;
};

/// Complete structural resolution for one controllable-component occurrence.
struct StructuralComponentResolution final {
  core::ControllableComponentId controllable_component_id;
  core::ComponentTypeId component_type_id;
  core::ComponentTypeRevision component_type_revision;
  std::vector<std::string> component_tags;
  std::vector<StructuralModelOccurrence> model_roles;
  std::optional<StructuralFrameResolution> component_root;
  std::vector<StructuralModelMountResolution> model_mounts;
  std::vector<StructuralCommandTargetResolution> command_targets;
  std::vector<StructuralAttachmentInterfaceResolution>
      attachment_interfaces;

  friend bool operator==(const StructuralComponentResolution&,
                         const StructuralComponentResolution&) = default;
};

struct StructuralAttachmentEndpointResolution final {
  core::ControllableComponentId controllable_component_id;
  core::AttachmentInterfaceId attachment_interface_id;
  StructuralFrameResolution frame;

  friend bool operator==(const StructuralAttachmentEndpointResolution&,
                         const StructuralAttachmentEndpointResolution&) =
      default;
};

struct StructuralAttachmentResolution final {
  core::ComponentAttachmentId component_attachment_id;
  StructuralAttachmentEndpointResolution parent;
  StructuralAttachmentEndpointResolution child;
  core::PlacementPoseDTO child_interface_pose_in_parent_interface;

  friend bool operator==(const StructuralAttachmentResolution&,
                         const StructuralAttachmentResolution&) = default;
};

/// A graph whose exact component-type/model-source closures and structural
/// composition have been proven.
///
/// This immutable trust stage owns the unchanged revision-verified graph,
/// source metadata/parsed IR (never raw source bytes), type metadata, and all
/// occurrence/source-role/attachment resolutions. It makes no claim about
/// SRDF group expansion, generated names, scene-frame naming, control
/// compatibility, retained products, runtime readiness, or any lane.
class StructurallyQualifiedCellGraph final {
 public:
  [[nodiscard]] const GraphRevisionVerifiedCellGraph& verified_graph()
      const noexcept {
    return verified_graph_;
  }
  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept {
    return verified_graph_.graph();
  }
  [[nodiscard]] const std::vector<SourceVerifiedModelAsset>& model_assets()
      const noexcept {
    return model_assets_;
  }
  [[nodiscard]] const std::vector<QualifiedComponentType>& component_types()
      const noexcept {
    return component_types_;
  }
  [[nodiscard]] const std::vector<StructuralModelInstanceResolution>&
  model_instances() const noexcept {
    return model_instances_;
  }
  [[nodiscard]] const std::vector<StructuralComponentResolution>& components()
      const noexcept {
    return components_;
  }
  [[nodiscard]] const std::vector<StructuralAttachmentResolution>&
  attachments() const noexcept {
    return attachments_;
  }

 private:
  StructurallyQualifiedCellGraph(
      GraphRevisionVerifiedCellGraph verified_graph,
      std::vector<SourceVerifiedModelAsset> model_assets,
      std::vector<QualifiedComponentType> component_types,
      std::vector<StructuralModelInstanceResolution> model_instances,
      std::vector<StructuralComponentResolution> components,
      std::vector<StructuralAttachmentResolution> attachments)
      : verified_graph_(std::move(verified_graph)),
        model_assets_(std::move(model_assets)),
        component_types_(std::move(component_types)),
        model_instances_(std::move(model_instances)),
        components_(std::move(components)),
        attachments_(std::move(attachments)) {}

  GraphRevisionVerifiedCellGraph verified_graph_;
  std::vector<SourceVerifiedModelAsset> model_assets_;
  std::vector<QualifiedComponentType> component_types_;
  std::vector<StructuralModelInstanceResolution> model_instances_;
  std::vector<StructuralComponentResolution> components_;
  std::vector<StructuralAttachmentResolution> attachments_;

  friend StructurallyQualifiedCellGraph qualify_cell_structure(
      const GraphRevisionVerifiedCellGraph&,
      std::span<const SourceVerifiedModelAsset>,
      std::span<const QualifiedComponentType>);
};

/// Resolves and proves the pure structural boundary for one exact graph.
///
/// Both dependency spans are exact caller-prefiltered closures. Duplicate
/// exact entries, missing/stale selections, and unused extras reject. The
/// compiler proves model cardinality, component model-role bijection, root,
/// mount, command-target and attachment-interface source roles, plus
/// attachment endpoint compatibility and capacity.
[[nodiscard]] StructurallyQualifiedCellGraph qualify_cell_structure(
    const GraphRevisionVerifiedCellGraph& graph,
    std::span<const SourceVerifiedModelAsset> model_assets,
    std::span<const QualifiedComponentType> component_types);

}  // namespace manyforge::deployment
