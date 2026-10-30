#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/placement_pose.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kModelAssetApiVersion =
    "manyforge.model_asset.v1";
inline constexpr std::size_t kMaxModelAssetCollectionItems = 4096U;
inline constexpr std::size_t kMaxModelAssetLogicalUriBytes = 2048U;
inline constexpr std::size_t kMaxModelAssetMediaTypeBytes = 255U;
inline constexpr std::size_t kMaxModelAssetSourceSymbolCodePoints = 1024U;
inline constexpr std::uint64_t kMaxModelAssetByteSize = 9007199254740991ULL;

enum class ModelAssetInstanceCardinality : std::uint8_t {
  kOnePerCell = 0,
  kMultiplePerCell,
};

[[nodiscard]] constexpr std::string_view to_string(
    ModelAssetInstanceCardinality value) noexcept {
  switch (value) {
    case ModelAssetInstanceCardinality::kOnePerCell:
      return "one_per_cell";
    case ModelAssetInstanceCardinality::kMultiplePerCell:
      return "multiple_per_cell";
  }
  return {};
}

/// One exact expanded-model source blob.
///
/// `logical_uri` is a closure-local symbol, not a mutable location.
/// Retrieval is content-digest based and belongs to the later source-store
/// boundary.
struct ModelSourceArtifactDTO {
  std::string logical_uri;
  std::string media_type;
  std::uint64_t byte_size = 0U;
  ContentDigest content_digest;

  friend bool operator==(const ModelSourceArtifactDTO&,
                         const ModelSourceArtifactDTO&) = default;
};

/// Exact immutable expanded description selected by one model asset.
struct ExpandedModelSourceDTO {
  std::string urdf_logical_uri;
  std::optional<std::string> srdf_logical_uri;
  std::vector<ModelSourceArtifactDTO> artifacts;

  friend bool operator==(const ExpandedModelSourceDTO&,
                         const ExpandedModelSourceDTO&) = default;
};

/// A frame role names an existing source link, or (`pose_in_source_link`
/// present) an offset frame: a generated massless link named after the frame
/// role, fixed to `source_link_name` at that pose (T_source_link_frame).
/// Offset frames never chain: their parent is always a source link.
struct ModelFrameRoleBindingDTO {
  FrameRoleId frame_role_id;
  std::string source_link_name;
  std::optional<PlacementPoseDTO> pose_in_source_link{};

  friend bool operator==(const ModelFrameRoleBindingDTO&,
                         const ModelFrameRoleBindingDTO&) = default;
};

/// The source-local link a frame role resolves to: its own link, or the
/// generated link of an offset frame (named after the frame role).
[[nodiscard]] inline const std::string& frame_role_link_name(
    const ModelFrameRoleBindingDTO& binding) noexcept {
  return binding.pose_in_source_link.has_value()
             ? binding.frame_role_id.value()
             : binding.source_link_name;
}

struct ModelGroupRoleBindingDTO {
  ModelGroupRoleId model_group_role_id;
  std::string source_group_name;

  friend bool operator==(const ModelGroupRoleBindingDTO&,
                         const ModelGroupRoleBindingDTO&) = default;
};

struct ModelResourceRoleBindingDTO {
  ResourceRoleId resource_role_id;
  std::vector<std::string> source_joint_names;

  friend bool operator==(const ModelResourceRoleBindingDTO&,
                         const ModelResourceRoleBindingDTO&) = default;
};

struct ModelCollisionBodyRoleBindingDTO {
  CollisionBodyRoleId collision_body_role_id;
  std::vector<std::string> source_link_names;

  friend bool operator==(const ModelCollisionBodyRoleBindingDTO&,
                         const ModelCollisionBodyRoleBindingDTO&) = default;
};

/// A model's own payload for a robot controller: its mass and centre of
/// mass in the model's root frame-role link. Only a mounted tool's payload
/// is used (added to the robot's payload at the mount); absent means 0.
struct ModelAssetPayloadDTO {
  double mass_kg{};
  std::array<double, 3> center_of_mass_m{};

  friend bool operator==(const ModelAssetPayloadDTO&,
                         const ModelAssetPayloadDTO&) = default;
};

/// Exact normalized payload for `manyforge.model_asset.v1`.
///
/// This declaration does not prove source availability, byte integrity, XML
/// validity, dependency closure, source-symbol existence, model-root
/// correctness, namespace rewriting, or lane compatibility. Those properties
/// require the later retained deployment compiler.
struct ModelAssetDTO {
  ModelAssetId model_asset_id;
  ModelRevision model_revision;
  ModelAssetInstanceCardinality instance_cardinality;
  ExpandedModelSourceDTO expanded_source;
  FrameRoleId root_frame_role_id;
  std::vector<ModelFrameRoleBindingDTO> frame_role_bindings;
  std::vector<ModelGroupRoleBindingDTO> model_group_role_bindings;
  std::vector<ModelResourceRoleBindingDTO> resource_role_bindings;
  std::vector<ModelCollisionBodyRoleBindingDTO>
      collision_body_role_bindings;
  std::optional<ModelAssetPayloadDTO> payload{};

  friend bool operator==(const ModelAssetDTO&, const ModelAssetDTO&) = default;
};

}  // namespace manyforge::core
