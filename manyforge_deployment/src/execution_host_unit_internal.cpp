#include "execution_host_unit_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>

#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {

std::vector<execution_runtime_contract::Configuration::MotionTargetLimit>
decode_execution_motion_target_limits(
    const core::JsonValueDTO::Array &motion_target_limits) {
  using execution_runtime_contract::Configuration;
  std::vector<Configuration::MotionTargetLimit> result;
  result.reserve(motion_target_limits.size());
  for (const auto &entry : motion_target_limits) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    // `ordinary_motion_jerk` is schema-materialized; absent keeps its
    // default (jerk switch off).
    const bool has_jerk = object != nullptr &&
                          object->find("ordinary_motion_jerk") != object->end();
    if (object == nullptr ||
        object->size() != 3U + (has_jerk ? 1U : 0U)) {
      throw RetainedBundleError(
          "execution runtime configuration has an invalid motion target limit");
    }
    const auto component = object->find("controllable_component_id");
    const auto target = object->find("command_target_id");
    const auto limit = object->find("maximum_tcp_linear_speed_mps");
    if (component == object->end() || target == object->end() ||
        limit == object->end()) {
      throw RetainedBundleError("execution runtime configuration has an "
                                "incomplete motion target limit");
    }
    const auto *component_id =
        std::get_if<std::string>(&component->second.value);
    const auto *target_id = std::get_if<std::string>(&target->second.value);
    const auto *maximum = std::get_if<double>(&limit->second.value);
    if (component_id == nullptr || component_id->empty() ||
        target_id == nullptr || target_id->empty() || maximum == nullptr ||
        !std::isfinite(*maximum) || *maximum <= 0.0 || *maximum > 1000.0) {
      throw RetainedBundleError(
          "execution runtime configuration has an invalid "
          "motion target limit value");
    }
    result.push_back(Configuration::MotionTargetLimit{
        core::ControllableComponentId(*component_id),
        core::CommandTargetId(*target_id), *maximum,
        has_jerk
            ? detail::decode_ordinary_motion_jerk_settings(
                  std::get<core::JsonObjectDTO>(
                      object->at("ordinary_motion_jerk").value))
            : execution_runtime_contract::OrdinaryMotionJerkSettings{}});
  }
  std::sort(result.begin(), result.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  if (std::adjacent_find(result.begin(), result.end(),
                         [](const auto &left, const auto &right) {
                           return left.controllable_component_id ==
                                      right.controllable_component_id &&
                                  left.command_target_id ==
                                      right.command_target_id;
                         }) != result.end()) {
    throw RetainedBundleError(
        "execution runtime configuration repeats a motion target limit");
  }
  return result;
}

bool execution_gripper_command_duration_is_valid(
    const std::string_view actuation_kind, const double duration_s) noexcept {
  return std::isfinite(duration_s) && duration_s <= 60.0 &&
         ((actuation_kind == "pneumatic" && duration_s >= 0.0) ||
          (actuation_kind == "position_commanded" && duration_s > 0.0));
}

std::vector<
    execution_runtime_contract::Configuration::GripperTargetConfiguration>
decode_execution_gripper_target_configurations(
    const core::JsonValueDTO::Array &gripper_target_configurations,
    const std::string_view label) {
  using execution_runtime_contract::Configuration;
  std::vector<Configuration::GripperTargetConfiguration> result;
  result.reserve(
      gripper_target_configurations.size());
  for (const auto &entry : gripper_target_configurations) {
    const auto *object = std::get_if<core::JsonObjectDTO>(&entry.value);
    if (object == nullptr || object->size() != 12U) {
      throw RetainedBundleError(
          std::string(label) + " has an invalid gripper target configuration");
    }
    const auto string_member = [&](const std::string_view name) -> std::string {
      const auto found = object->find(name);
      if (found == object->end()) {
        throw RetainedBundleError(
            std::string(label) + " has an incomplete gripper target configuration");
      }
      const auto *value = std::get_if<std::string>(&found->second.value);
      if (value == nullptr || value->empty()) {
        throw RetainedBundleError(
            std::string(label) + " has an invalid gripper target string");
      }
      return *value;
    };
    const auto string_array_member =
        [&](const std::string_view name, const bool require_nonempty) {
          const auto found = object->find(name);
          const auto *values = found == object->end()
                                   ? nullptr
                                   : std::get_if<core::JsonValueDTO::Array>(
                                         &found->second.value);
          if (values == nullptr || (require_nonempty && values->empty())) {
            throw RetainedBundleError(
                std::string(label) + " has an invalid gripper target list");
          }
          std::vector<std::string> result_values;
          result_values.reserve(values->size());
          for (const auto &value : *values) {
            const auto *text = std::get_if<std::string>(&value.value);
            if (text == nullptr || text->empty()) {
              throw RetainedBundleError(
                  std::string(label) + " has an invalid gripper target list item");
            }
            result_values.push_back(*text);
          }
          if (!std::is_sorted(result_values.begin(), result_values.end()) ||
              std::adjacent_find(result_values.begin(), result_values.end()) !=
                  result_values.end()) {
            throw RetainedBundleError(
                std::string(label) + " has a noncanonical gripper target list");
          }
          return result_values;
        };
    const auto duration = object->find("command_duration_s");
    const auto *duration_s =
        duration == object->end()
            ? nullptr
            : std::get_if<double>(&duration->second.value);
    const auto component_id = string_member("controllable_component_id");
    const auto target_id = string_member("command_target_id");
    const auto actuation_kind = string_member("actuation_kind");
    if (duration_s == nullptr ||
        !execution_gripper_command_duration_is_valid(actuation_kind,
                                                     *duration_s)) {
      throw RetainedBundleError(
          std::string(label) + " has invalid gripper target semantics");
    }
    result.push_back(Configuration::GripperTargetConfiguration{
        core::ControllableComponentId(component_id), core::CommandTargetId(target_id),
        string_member("description"), actuation_kind, string_member("open_state_ref"),
        string_member("closed_state_ref"), *duration_s, string_array_member("touch_links", true),
        string_member("mount_frame"), string_member("attach_frame"), string_member("tcp_frame"),
        string_array_member("auxiliary_frames", false)});
  }
  std::sort(result.begin(),
            result.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  if (std::adjacent_find(
          result.begin(),
          result.end(),
          [](const auto &left, const auto &right) {
            return left.controllable_component_id ==
                       right.controllable_component_id &&
                   left.command_target_id == right.command_target_id;
          }) != result.end()) {
    throw RetainedBundleError(
        std::string(label) + " repeats a gripper target configuration");
  }
  return result;
}

} // namespace manyforge::deployment::detail
