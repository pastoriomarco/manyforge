#pragma once

#include <string>
#include <vector>

namespace manyforge::core {

/// Deployment-owned application timing; source acquisition remains adapter-owned.
/// `apply_policy` is one of observe_only, before_planning, between_nodes,
/// continuous, continuous_unattached or continuous_relative_to_attached.
struct ScenePoseSourcePolicyDTO final {
  std::string source_id;
  std::string scene_resource_id;
  std::string apply_policy;
  friend bool operator==(const ScenePoseSourcePolicyDTO&,
                         const ScenePoseSourcePolicyDTO&) = default;
};

}  // namespace manyforge::core
