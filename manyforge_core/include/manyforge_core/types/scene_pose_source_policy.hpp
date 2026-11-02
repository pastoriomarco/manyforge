#pragma once

#include <array>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Isaac entity key of an object link: the entity path, the frame its pose
/// header names, and a local offset (default identity).
struct ScenePoseSimulationEntityKeyDTO final {
  std::string entity;
  std::string source_header_frame_id;
  std::array<double, 3> local_position_xyz{0.0, 0.0, 0.0};
  std::array<double, 4> local_orientation_xyzw{0.0, 0.0, 0.0, 1.0};
  friend bool operator==(const ScenePoseSimulationEntityKeyDTO&,
                         const ScenePoseSimulationEntityKeyDTO&) = default;
};

/// Detection3D selector of an object link (vision_msgs Detection3DArray).
/// `selection` is one of "", "first" or "best_score".
struct ScenePoseDetection3dKeyDTO final {
  std::string class_id;
  std::string detection_id;
  double confidence_min{};
  double max_position_variance_m2{};
  std::string selection;
  friend bool operator==(const ScenePoseDetection3dKeyDTO&,
                         const ScenePoseDetection3dKeyDTO&) = default;
};

using ScenePoseSourceKeyDTO =
    std::variant<ScenePoseSimulationEntityKeyDTO, ScenePoseDetection3dKeyDTO>;

/// One object link (`policies.scene_pose_sources[]`): the scene object, the
/// providing `pose_source` component, how that component finds the object,
/// and the Deployment-owned application timing. One row per object.
/// `apply_policy` is one of observe_only, before_planning, between_nodes,
/// continuous, continuous_unattached or continuous_relative_to_attached.
/// A pose change within the deadband counts as unchanged; 0 means exact.
struct ScenePoseSourcePolicyDTO final {
  std::string source_id;
  std::string scene_resource_id;
  ScenePoseSourceKeyDTO source_key;
  std::string apply_policy;
  double deadband_translation_m{};
  double deadband_rotation_rad{};
  friend bool operator==(const ScenePoseSourcePolicyDTO&,
                         const ScenePoseSourcePolicyDTO&) = default;
};

}  // namespace manyforge::core
