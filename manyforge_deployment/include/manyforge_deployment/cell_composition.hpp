#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/placement_pose.hpp>
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

// C0.2m owns one aggregate budget for each retained composition family. These
// limits prevent legal per-source inputs from multiplying into an unbounded
// cell-wide product when the same reusable asset is instantiated many times.
inline constexpr std::size_t kMaxCompositionSymbols = 1000000U;
inline constexpr std::size_t kMaxCompositionReferences = 4000000U;
inline constexpr std::size_t kMaxCompositionPathSegments = 4000000U;
inline constexpr std::size_t kMaxCompositionCoordinateProjections = 1000000U;
inline constexpr std::size_t kMaxCompositionFrameResolutions = 65536U;
inline constexpr std::size_t kMaxCompositionTargetProjectionRows = 1000000U;
inline constexpr std::uint64_t kMaxCompositionFlattenedXmlBytes =
    512ULL * 1024ULL * 1024ULL;

class CellCompositionError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class OccurrenceSymbolKind : std::uint8_t {
  kModel = 0,
  kRobot,
  kMaterial,
  kLink,
  kJoint,
  kVisual,
  kCollision,
  kSrdfGroup,
  kSrdfGroupState,
  kSrdfEndEffector,
};

/// Exact typed origin of one compiler-owned occurrence symbol.
///
/// `scope_name` is empty for globally scoped source symbols. It is the source
/// link plus member kind/index for visual/collision names and inline material
/// definitions, and the source SRDF group name for group-state names.
struct OccurrenceSymbolKey final {
  core::ModelInstanceId model_instance_id;
  OccurrenceSymbolKind kind;
  std::string scope_name;
  std::string source_name;

  friend bool operator==(const OccurrenceSymbolKey&,
                         const OccurrenceSymbolKey&) = default;
};

struct OccurrenceSymbolRewrite final {
  OccurrenceSymbolKey source;
  std::string compiled_name;

  friend bool operator==(const OccurrenceSymbolRewrite&,
                         const OccurrenceSymbolRewrite&) = default;
};

struct OccurrenceReverseSymbolRewrite final {
  std::string compiled_name;
  OccurrenceSymbolKey source;

  friend bool operator==(const OccurrenceReverseSymbolRewrite&,
                         const OccurrenceReverseSymbolRewrite&) = default;
};

enum class OccurrenceReferenceKind : std::uint8_t {
  kJointParentLink = 0,
  kJointChildLink,
  kJointMimic,
  kVisualMaterial,
  kSrdfGroupLink,
  kSrdfGroupJoint,
  kSrdfChainBaseLink,
  kSrdfChainTipLink,
  kSrdfSubgroup,
  kSrdfGroupStateGroup,
  kSrdfGroupStateJoint,
  kSrdfEndEffectorParentLink,
  kSrdfEndEffectorGroup,
  kSrdfEndEffectorParentGroup,
  kSrdfPassiveJoint,
  kSrdfDisabledCollisionFirstLink,
  kSrdfDisabledCollisionSecondLink,
};

/// One retained proof that a source reference was rewritten to the exact
/// compiler-owned target symbol.
struct OccurrenceReferenceRewrite final {
  core::ModelInstanceId model_instance_id;
  OccurrenceReferenceKind kind;
  std::string owner_scope_name;
  std::string owner_source_name;
  std::size_t reference_ordinal;
  OccurrenceSymbolKey source_target;
  std::string compiled_target_name;

  friend bool operator==(const OccurrenceReferenceRewrite&,
                         const OccurrenceReferenceRewrite&) = default;
};

struct CompositionModelOccurrence final {
  core::ModelInstanceId model_instance_id;
  core::ModelAssetId model_asset_id;
  core::ModelRevision model_revision;
  std::optional<core::ControllableComponentId> owner_component_id;
  std::optional<core::ModelRoleId> owner_model_role_id;
  std::string compiled_model_name;
  std::string compiled_robot_name;
  std::string compiled_root_link_name;

  friend bool operator==(const CompositionModelOccurrence&,
                         const CompositionModelOccurrence&) = default;
};

struct CompositionCellFrame final {
  core::CellId cell_id;
  std::string compiled_frame_name;

  friend bool operator==(const CompositionCellFrame&,
                         const CompositionCellFrame&) = default;
};

struct CompositionComponentModelFrame final {
  core::ControllableComponentId controllable_component_id;
  core::ModelInstanceId model_instance_id;
  core::ModelRevision model_revision;
  core::FrameRoleId frame_role_id;
  std::string compiled_frame_name;

  friend bool operator==(const CompositionComponentModelFrame&,
                         const CompositionComponentModelFrame&) = default;
};

struct CompositionPassiveModelFrame final {
  core::ModelInstanceId model_instance_id;
  core::ModelRevision model_revision;
  core::FrameRoleId frame_role_id;
  std::string compiled_frame_name;

  friend bool operator==(const CompositionPassiveModelFrame&,
                         const CompositionPassiveModelFrame&) = default;
};

using CompositionResolvedFrame =
    std::variant<CompositionCellFrame, CompositionComponentModelFrame,
                 CompositionPassiveModelFrame>;

struct CompositionFrameResolution final {
  CompositionResolvedFrame frame;
  std::string source_link_name;

  friend bool operator==(const CompositionFrameResolution&,
                         const CompositionFrameResolution&) = default;
};

enum class CompositionCoordinateUnit : std::uint8_t {
  kNone = 0,
  kRadians,
  kMeters,
  kPlanarMetersRadians,
  kFloatingMetersRadians,
};

/// Complete source-order coordinate inventory for one model occurrence.
struct CompositionModelCoordinate final {
  core::ModelInstanceId model_instance_id;
  std::optional<core::ControllableComponentId> owner_component_id;
  std::size_t source_joint_index;
  std::string source_joint_name;
  std::string compiled_joint_name;
  UrdfJointType joint_type;
  UrdfCoordinateKind coordinate_kind;
  CompositionCoordinateUnit unit;
  std::optional<std::string> mimic_source_compiled_joint_name;
  double mimic_multiplier = 1.0;
  double mimic_offset = 0.0;

  friend bool operator==(const CompositionModelCoordinate&,
                         const CompositionModelCoordinate&) = default;
};

/// One model coordinate projected from an element of a bound ResourceId.
///
/// `coordinates[0]` in each model-backed resource row is the direct,
/// independent coordinate owned by that ResourceId. Any following rows are
/// deterministic source-order mimics carrying their complete transitive
/// affine transform.
struct CompositionCoordinateProjection final {
  core::ModelInstanceId model_instance_id;
  std::string source_joint_name;
  std::string compiled_joint_name;
  UrdfCoordinateKind coordinate_kind;
  CompositionCoordinateUnit unit;
  double multiplier = 1.0;
  double offset = 0.0;
  bool derived_mimic = false;

  friend bool operator==(const CompositionCoordinateProjection&,
                         const CompositionCoordinateProjection&) = default;
};

struct CompositionResourceCoordinateRow final {
  core::ResourceRoleId resource_role_id;
  core::ResourceId resource_id;
  std::vector<CompositionCoordinateProjection> coordinates;

  friend bool operator==(const CompositionResourceCoordinateRow&,
                         const CompositionResourceCoordinateRow&) = default;
};

struct CompositionControlBindingCoordinateMap final {
  core::ControlBindingId control_binding_id;
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  std::vector<CompositionResourceCoordinateRow> resources;

  friend bool operator==(const CompositionControlBindingCoordinateMap&,
                         const CompositionControlBindingCoordinateMap&) =
      default;
};

struct CompositionTargetGroupResolution final {
  core::ModelGroupRoleId model_group_role_id;
  core::ModelInstanceId model_instance_id;
  std::string compiled_group_name;
  std::vector<std::string> expanded_compiled_link_names;
  std::vector<std::string> expanded_compiled_joint_names;

  friend bool operator==(const CompositionTargetGroupResolution&,
                         const CompositionTargetGroupResolution&) = default;
};

struct CompositionTargetFrameResolution final {
  core::FrameRoleId frame_role_id;
  CompositionResolvedFrame frame;

  friend bool operator==(const CompositionTargetFrameResolution&,
                         const CompositionTargetFrameResolution&) = default;
};

struct CompositionTargetCollisionResolution final {
  core::CollisionBodyRoleId collision_body_role_id;
  core::ModelInstanceId model_instance_id;
  std::vector<std::string> compiled_link_names;

  friend bool operator==(const CompositionTargetCollisionResolution&,
                         const CompositionTargetCollisionResolution&) =
      default;
};

struct CompositionTargetResolution final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  std::vector<CompositionTargetGroupResolution> model_groups;
  std::vector<CompositionTargetFrameResolution> frames;
  std::vector<CompositionTargetCollisionResolution> collision_bodies;

  friend bool operator==(const CompositionTargetResolution&,
                         const CompositionTargetResolution&) = default;
};

struct CompositionDirectModelAnchor final {
  core::ModelInstanceId model_instance_id;
  core::PlacementPoseDTO root_pose_in_cell;

  friend bool operator==(const CompositionDirectModelAnchor&,
                         const CompositionDirectModelAnchor&) = default;
};

struct CompositionDirectComponentAnchor final {
  core::ControllableComponentId controllable_component_id;
  core::ModelInstanceId root_model_instance_id;
  core::PlacementPoseDTO root_pose_in_cell;

  friend bool operator==(const CompositionDirectComponentAnchor&,
                         const CompositionDirectComponentAnchor&) = default;
};

struct CompositionModelMount final {
  core::ControllableComponentId controllable_component_id;
  core::ModelInstanceId parent_model_instance_id;
  core::ModelInstanceId child_model_instance_id;
  core::PlacementPoseDTO child_frame_pose_in_parent_frame;

  friend bool operator==(const CompositionModelMount&,
                         const CompositionModelMount&) = default;
};

struct CompositionComponentAttachment final {
  core::ComponentAttachmentId component_attachment_id;
  core::ControllableComponentId parent_component_id;
  core::ControllableComponentId child_component_id;
  core::PlacementPoseDTO child_interface_pose_in_parent_interface;

  friend bool operator==(const CompositionComponentAttachment&,
                         const CompositionComponentAttachment&) = default;
};

using CompositionConnectionSource =
    std::variant<CompositionDirectModelAnchor,
                 CompositionDirectComponentAnchor, CompositionModelMount,
                 CompositionComponentAttachment>;

struct CompositionFixedConnection final {
  CompositionConnectionSource source;
  std::string compiled_joint_name;
  std::string compiled_parent_link_name;
  std::string compiled_child_link_name;
  core::PlacementPoseDTO child_pose_in_parent;

  friend bool operator==(const CompositionFixedConnection&,
                         const CompositionFixedConnection&) = default;
};

struct CompositionFixedPathSegment final {
  std::size_t fixed_connection_index;

  friend bool operator==(const CompositionFixedPathSegment&,
                         const CompositionFixedPathSegment&) = default;
};

struct CompositionModelJointPathSegment final {
  core::ModelInstanceId model_instance_id;
  std::size_t composition_coordinate_index;
  std::string source_joint_name;
  std::string compiled_joint_name;
  std::string compiled_parent_link_name;
  std::string compiled_child_link_name;

  friend bool operator==(const CompositionModelJointPathSegment&,
                         const CompositionModelJointPathSegment&) = default;
};

using CompositionFramePathSegment =
    std::variant<CompositionFixedPathSegment,
                 CompositionModelJointPathSegment>;

struct CompositionFramePath final {
  CompositionResolvedFrame terminal_frame;
  std::vector<CompositionFramePathSegment> segments;

  friend bool operator==(const CompositionFramePath&,
                         const CompositionFramePath&) = default;
};

/// Immutable, non-caller-constructible C0.2m composition result.
///
/// This object proves deterministic occurrence composition only. It deliberately
/// does not claim control/provider/Stop applicability, retained products,
/// runtime readiness, or any execution lane.
class CompositionQualifiedCellIr final {
 public:
  CompositionQualifiedCellIr(const CompositionQualifiedCellIr&) = default;
  CompositionQualifiedCellIr& operator=(const CompositionQualifiedCellIr&) =
      default;

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
  [[nodiscard]] const CompositionCellFrame& cell_frame() const noexcept {
    return cell_frame_;
  }
  [[nodiscard]] const UrdfDocumentIr& flattened_urdf() const noexcept {
    return flattened_urdf_;
  }
  [[nodiscard]] const SrdfDocumentIr& flattened_srdf() const noexcept {
    return flattened_srdf_;
  }
  [[nodiscard]] const std::vector<CompositionModelOccurrence>&
  model_occurrences() const noexcept {
    return model_occurrences_;
  }
  [[nodiscard]] const std::vector<OccurrenceSymbolRewrite>& symbol_rewrites()
      const noexcept {
    return symbol_rewrites_;
  }
  [[nodiscard]] const std::vector<OccurrenceReverseSymbolRewrite>&
  reverse_symbol_rewrites() const noexcept {
    return reverse_symbol_rewrites_;
  }
  [[nodiscard]] const std::vector<OccurrenceReferenceRewrite>&
  reference_rewrites() const noexcept {
    return reference_rewrites_;
  }
  [[nodiscard]] const std::vector<CompositionFrameResolution>& frames()
      const noexcept {
    return frames_;
  }
  [[nodiscard]] const std::vector<CompositionFixedConnection>&
  fixed_connections() const noexcept {
    return fixed_connections_;
  }
  [[nodiscard]] const std::vector<CompositionFramePath>& frame_paths()
      const noexcept {
    return frame_paths_;
  }
  [[nodiscard]] const std::vector<CompositionModelCoordinate>& coordinates()
      const noexcept {
    return coordinates_;
  }
  [[nodiscard]] const std::vector<CompositionControlBindingCoordinateMap>&
  control_binding_coordinate_maps() const noexcept {
    return control_binding_coordinate_maps_;
  }
  [[nodiscard]] const std::vector<CompositionTargetResolution>& targets()
      const noexcept {
    return targets_;
  }

 private:
  CompositionQualifiedCellIr(
      const GraphRevisionVerifiedCellGraph& verified_graph,
      std::vector<SourceVerifiedModelAsset> model_assets,
      std::vector<QualifiedComponentType> component_types,
      CompositionCellFrame cell_frame, UrdfDocumentIr flattened_urdf,
      SrdfDocumentIr flattened_srdf,
      std::vector<CompositionModelOccurrence> model_occurrences,
      std::vector<OccurrenceSymbolRewrite> symbol_rewrites,
      std::vector<OccurrenceReverseSymbolRewrite> reverse_symbol_rewrites,
      std::vector<OccurrenceReferenceRewrite> reference_rewrites,
      std::vector<CompositionFrameResolution> frames,
      std::vector<CompositionFixedConnection> fixed_connections,
      std::vector<CompositionFramePath> frame_paths,
      std::vector<CompositionModelCoordinate> coordinates,
      std::vector<CompositionControlBindingCoordinateMap>
          control_binding_coordinate_maps,
      std::vector<CompositionTargetResolution> targets)
      : verified_graph_(verified_graph),
        model_assets_(std::move(model_assets)),
        component_types_(std::move(component_types)),
        cell_frame_(std::move(cell_frame)),
        flattened_urdf_(std::move(flattened_urdf)),
        flattened_srdf_(std::move(flattened_srdf)),
        model_occurrences_(std::move(model_occurrences)),
        symbol_rewrites_(std::move(symbol_rewrites)),
        reverse_symbol_rewrites_(std::move(reverse_symbol_rewrites)),
        reference_rewrites_(std::move(reference_rewrites)),
        frames_(std::move(frames)),
        fixed_connections_(std::move(fixed_connections)),
        frame_paths_(std::move(frame_paths)),
        coordinates_(std::move(coordinates)),
        control_binding_coordinate_maps_(
            std::move(control_binding_coordinate_maps)),
        targets_(std::move(targets)) {}

  GraphRevisionVerifiedCellGraph verified_graph_;
  std::vector<SourceVerifiedModelAsset> model_assets_;
  std::vector<QualifiedComponentType> component_types_;
  CompositionCellFrame cell_frame_;
  UrdfDocumentIr flattened_urdf_;
  SrdfDocumentIr flattened_srdf_;
  std::vector<CompositionModelOccurrence> model_occurrences_;
  std::vector<OccurrenceSymbolRewrite> symbol_rewrites_;
  std::vector<OccurrenceReverseSymbolRewrite> reverse_symbol_rewrites_;
  std::vector<OccurrenceReferenceRewrite> reference_rewrites_;
  std::vector<CompositionFrameResolution> frames_;
  std::vector<CompositionFixedConnection> fixed_connections_;
  std::vector<CompositionFramePath> frame_paths_;
  std::vector<CompositionModelCoordinate> coordinates_;
  std::vector<CompositionControlBindingCoordinateMap>
      control_binding_coordinate_maps_;
  std::vector<CompositionTargetResolution> targets_;

  friend CompositionQualifiedCellIr compose_cell_occurrences(
      const GraphRevisionVerifiedCellGraph&,
      std::span<const SourceVerifiedModelAsset>,
      std::span<const QualifiedComponentType>);
};

/// Runs the private structural stage and deterministic occurrence composition
/// for one exact identity-verified graph and its exact qualified closures.
///
/// Duplicate, missing, stale, or unused source/type closure entries reject.
/// No structurally qualified intermediate is exposed to callers.
[[nodiscard]] CompositionQualifiedCellIr compose_cell_occurrences(
    const GraphRevisionVerifiedCellGraph& graph,
    std::span<const SourceVerifiedModelAsset> model_assets,
    std::span<const QualifiedComponentType> component_types);

}  // namespace manyforge::deployment
