#pragma once

#include <string>
#include <vector>

namespace manyforge::core {

/// Deployment-owned application timing; source acquisition remains adapter-owned.
/// `apply_policy` is one of observe_only, before_planning, between_nodes,
/// continuous, continuous_unattached or continuous_relative_to_attached.
/// A pose change within the deadband counts as unchanged; 0 means exact.
struct ScenePoseSourcePolicyDTO final {
  std::string source_id;
  std::string scene_resource_id;
  std::string apply_policy;
  double deadband_translation_m{};
  double deadband_rotation_rad{};
  friend bool operator==(const ScenePoseSourcePolicyDTO&,
                         const ScenePoseSourcePolicyDTO&) = default;
};

}  // namespace manyforge::core
