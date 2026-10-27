#pragma once

#include <algorithm>
#include <cmath>
#include <manyforge_core/types/scene_pose_source_policy.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

namespace manyforge::deployment {

/// Object links (`policies.scene_pose_sources`): closed rows of
/// `scene_resource_id`, `source_id`, `source_key` (exactly one of
/// `simulation_entity` or `detection3d`), `apply_policy` and the optional
/// deadbands. One row per `scene_resource_id`; sorted by
/// (source_id, scene_resource_id). Defaults (zero deadband, zero offset,
/// identity orientation) are absent in the canonical encoding.
/// Throws std::invalid_argument (`scene_pose_source_duplicate_object` for a
/// second row of the same object).
inline std::vector<core::ScenePoseSourcePolicyDTO>
DecodeScenePoseSources(const CanonicalValue &value) {
  const auto &rows = value.as_array();
  if (rows.size() > 128U)
    throw std::invalid_argument("scene_pose_sources exceeds 128 bindings");
  const auto closed = [](const CanonicalValue &object,
                         std::initializer_list<std::string_view> required,
                         std::initializer_list<std::string_view> optional) {
    std::size_t present = 0U;
    for (const auto name : optional)
      present += object.find_object_member(name) ? 1U : 0U;
    for (const auto name : required)
      if (!object.find_object_member(name))
        throw std::invalid_argument("scene pose-source binding misses " +
                                    std::string(name));
    if (object.as_object().size() != required.size() + present)
      throw std::invalid_argument(
          "scene pose-source binding has unknown fields");
  };
  const auto text = [](const CanonicalValue &object, const char *name,
                       std::size_t min, std::size_t max) -> std::string {
    const auto *field = object.find_object_member(name);
    if (!field || !field->is_string() || field->as_string().size() < min ||
        field->as_string().size() > max)
      throw std::invalid_argument(
          std::string("invalid scene pose-source binding field ") + name);
    return field->as_string();
  };
  const auto number = [](const CanonicalValue &object, const char *name,
                         double fallback) -> double {
    const auto *field = object.find_object_member(name);
    if (!field)
      return fallback;
    const auto *value = std::get_if<double>(&field->storage());
    if (!value || !std::isfinite(*value))
      throw std::invalid_argument(
          std::string("invalid scene pose-source number ") + name);
    return *value;
  };
  const auto nonnegative = [&](const CanonicalValue &object,
                               const char *name) -> double {
    const auto result = number(object, name, 0.0);
    if (result < 0.0)
      throw std::invalid_argument(
          std::string("scene pose-source field must be nonnegative: ") + name);
    return result;
  };
  const auto vector = [&](const CanonicalValue &object, const char *name,
                          auto fallback) {
    const auto *field = object.find_object_member(name);
    if (!field)
      return fallback;
    if (!field->is_array() || field->as_array().size() != fallback.size())
      throw std::invalid_argument(
          std::string("invalid scene pose-source vector ") + name);
    for (std::size_t i = 0; i != fallback.size(); ++i) {
      const auto *item = std::get_if<double>(&field->as_array()[i].storage());
      if (!item || !std::isfinite(*item))
        throw std::invalid_argument(
            std::string("invalid scene pose-source vector ") + name);
      fallback[i] = *item;
    }
    return fallback;
  };
  const auto key = [&](const CanonicalValue &value) -> core::ScenePoseSourceKeyDTO {
    if (!value.is_object() || value.as_object().size() != 1U)
      throw std::invalid_argument(
          "scene pose-source source_key must have exactly one member");
    if (const auto *entity = value.find_object_member("simulation_entity")) {
      closed(*entity, {"entity", "source_header_frame_id"},
             {"local_position_xyz", "local_orientation_xyzw"});
      return core::ScenePoseSimulationEntityKeyDTO{
          text(*entity, "entity", 1U, 1024U),
          text(*entity, "source_header_frame_id", 0U, 1024U),
          vector(*entity, "local_position_xyz",
                 std::array<double, 3>{0.0, 0.0, 0.0}),
          vector(*entity, "local_orientation_xyzw",
                 std::array<double, 4>{0.0, 0.0, 0.0, 1.0})};
    }
    if (const auto *selector = value.find_object_member("detection3d")) {
      closed(*selector,
             {"class_id", "detection_id", "confidence_min",
              "max_position_variance_m2", "selection"},
             {});
      core::ScenePoseDetection3dKeyDTO result{
          text(*selector, "class_id", 0U, 1024U),
          text(*selector, "detection_id", 0U, 1024U),
          number(*selector, "confidence_min", 0.0),
          nonnegative(*selector, "max_position_variance_m2"),
          text(*selector, "selection", 0U, 16U)};
      if (result.selection != "" && result.selection != "first" &&
          result.selection != "best_score")
        throw std::invalid_argument(
            "unsupported scene pose-source detection3d selection");
      return result;
    }
    throw std::invalid_argument("unsupported scene pose-source source_key");
  };
  std::vector<core::ScenePoseSourcePolicyDTO> result;
  for (const auto &row : rows) {
    closed(row, {"scene_resource_id", "source_id", "source_key", "apply_policy"},
           {"deadband_translation_m", "deadband_rotation_rad"});
    core::ScenePoseSourcePolicyDTO binding{
        text(row, "source_id", 1U, 256U),
        text(row, "scene_resource_id", 1U, 256U),
        key(*row.find_object_member("source_key")),
        text(row, "apply_policy", 1U, 256U),
        nonnegative(row, "deadband_translation_m"),
        nonnegative(row, "deadband_rotation_rad")};
    if (binding.apply_policy != "observe_only" &&
        binding.apply_policy != "before_planning" &&
        binding.apply_policy != "between_nodes" &&
        binding.apply_policy != "continuous" &&
        binding.apply_policy != "continuous_unattached" &&
        binding.apply_policy != "continuous_relative_to_attached")
      throw std::invalid_argument("unsupported scene pose-source apply policy");
    result.push_back(std::move(binding));
  }
  std::set<std::string_view> objects;
  for (const auto &binding : result)
    if (!objects.insert(binding.scene_resource_id).second)
      throw std::invalid_argument("scene_pose_source_duplicate_object");
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source_id, a.scene_resource_id) <
           std::tie(b.source_id, b.scene_resource_id);
  });
  return result;
}

inline CanonicalValue EncodeScenePoseSourceKey(
    const core::ScenePoseSourceKeyDTO &source_key) {
  const auto numbers = [](const auto &values) {
    CanonicalValue::Array result;
    for (const auto value : values)
      result.emplace_back(value);
    return CanonicalValue(std::move(result));
  };
  if (const auto *entity =
          std::get_if<core::ScenePoseSimulationEntityKeyDTO>(&source_key)) {
    CanonicalValue::Object row{
        {"entity", CanonicalValue(entity->entity)},
        {"source_header_frame_id",
         CanonicalValue(entity->source_header_frame_id)}};
    // Zero offset and identity orientation are the defaults and stay absent.
    if (entity->local_position_xyz != std::array<double, 3>{0.0, 0.0, 0.0})
      row.emplace_back("local_position_xyz",
                       numbers(entity->local_position_xyz));
    if (entity->local_orientation_xyzw !=
        std::array<double, 4>{0.0, 0.0, 0.0, 1.0})
      row.emplace_back("local_orientation_xyzw",
                       numbers(entity->local_orientation_xyzw));
    return CanonicalValue(CanonicalValue::Object{
        {"simulation_entity", CanonicalValue(std::move(row))}});
  }
  const auto &selector = std::get<core::ScenePoseDetection3dKeyDTO>(source_key);
  return CanonicalValue(CanonicalValue::Object{
      {"detection3d",
       CanonicalValue(CanonicalValue::Object{
           {"class_id", CanonicalValue(selector.class_id)},
           {"detection_id", CanonicalValue(selector.detection_id)},
           {"confidence_min", CanonicalValue(selector.confidence_min)},
           {"max_position_variance_m2",
            CanonicalValue(selector.max_position_variance_m2)},
           {"selection", CanonicalValue(selector.selection)}})}});
}

inline CanonicalValue EncodeScenePoseSources(
    const std::vector<core::ScenePoseSourcePolicyDTO> &bindings) {
  CanonicalValue::Array rows;
  for (const auto &binding : bindings) {
    CanonicalValue::Object row{
        {"source_id", CanonicalValue(binding.source_id)},
        {"scene_resource_id", CanonicalValue(binding.scene_resource_id)},
        {"source_key", EncodeScenePoseSourceKey(binding.source_key)},
        {"apply_policy", CanonicalValue(binding.apply_policy)}};
    // A zero deadband is the default and stays absent.
    if (binding.deadband_translation_m != 0.0)
      row.emplace_back("deadband_translation_m",
                       CanonicalValue(binding.deadband_translation_m));
    if (binding.deadband_rotation_rad != 0.0)
      row.emplace_back("deadband_rotation_rad",
                       CanonicalValue(binding.deadband_rotation_rad));
    rows.emplace_back(std::move(row));
  }
  return CanonicalValue(std::move(rows));
}

} // namespace manyforge::deployment
