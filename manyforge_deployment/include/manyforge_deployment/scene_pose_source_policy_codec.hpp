#pragma once

#include <algorithm>
#include <manyforge_core/types/scene_pose_source_policy.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <stdexcept>
#include <tuple>

namespace manyforge::deployment {

inline std::vector<core::ScenePoseSourcePolicyDTO>
DecodeScenePoseSources(const CanonicalValue &value) {
  const auto &rows = value.as_array();
  if (rows.size() > 128U)
    throw std::invalid_argument("scene_pose_sources exceeds 128 bindings");
  std::vector<core::ScenePoseSourcePolicyDTO> result;
  for (const auto &row : rows) {
    if (row.as_object().size() != 3U)
      throw std::invalid_argument(
          "scene pose-source binding has unknown fields");
    const auto text = [&](const char *name) -> std::string {
      const auto *field = row.find_object_member(name);
      if (!field || !field->is_string() || field->as_string().empty() ||
          field->as_string().size() > 256U)
        throw std::invalid_argument("invalid scene pose-source binding field");
      return field->as_string();
    };
    core::ScenePoseSourcePolicyDTO binding{
        text("source_id"), text("scene_resource_id"), text("apply_policy")};
    if (binding.apply_policy != "observe_only" &&
        binding.apply_policy != "before_planning" &&
        binding.apply_policy != "between_nodes" &&
        binding.apply_policy != "continuous" &&
        binding.apply_policy != "continuous_unattached" &&
        binding.apply_policy != "continuous_relative_to_attached")
      throw std::invalid_argument("unsupported scene pose-source apply policy");
    result.push_back(std::move(binding));
  }
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source_id, a.scene_resource_id) <
           std::tie(b.source_id, b.scene_resource_id);
  });
  for (std::size_t i = 1; i < result.size(); ++i)
    if (result[i - 1].source_id == result[i].source_id &&
        result[i - 1].scene_resource_id == result[i].scene_resource_id)
      throw std::invalid_argument("duplicate scene pose-source binding");
  return result;
}

inline CanonicalValue EncodeScenePoseSources(
    const std::vector<core::ScenePoseSourcePolicyDTO> &bindings) {
  CanonicalValue::Array rows;
  for (const auto &binding : bindings)
    rows.emplace_back(CanonicalValue::Object{
        {"source_id", CanonicalValue(binding.source_id)},
        {"scene_resource_id", CanonicalValue(binding.scene_resource_id)},
        {"apply_policy", CanonicalValue(binding.apply_policy)}});
  return CanonicalValue(std::move(rows));
}

} // namespace manyforge::deployment
