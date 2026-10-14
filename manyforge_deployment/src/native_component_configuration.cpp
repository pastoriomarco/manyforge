#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/execution_runtime_contract.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <utility>

#include <manyforge_deployment/verified_retained_bundle.hpp>
#include "native_execution_runtime_unit_internal.hpp"

namespace manyforge::deployment {
namespace {

using TargetKey = std::pair<core::ControllableComponentId, core::CommandTargetId>;

[[noreturn]] void Fail(const char *reason) {
  throw RetainedBundleError(reason);
}

} // namespace

execution_runtime_contract::Configuration
detail::select_native_component_configuration(
    execution_runtime_contract::Configuration result,
    const core::CellGraphDTO &graph, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id) {
  const auto component = std::find_if(
      graph.controllable_components.begin(), graph.controllable_components.end(),
      [&](const auto &row) {
        return row.controllable_component_id == component_id &&
               std::find(row.runtime_unit_ids.begin(), row.runtime_unit_ids.end(),
                         runtime_unit_id) != row.runtime_unit_ids.end();
      });
  if (component == graph.controllable_components.end()) {
    Fail("native component configuration has no exact hosted component");
  }

  // The aggregate state follows component/target order, then the binding's
  // coordinate resources. This is also the native owner's construction order.
  std::map<TargetKey, std::size_t> motion_counts;
  std::set<core::ControllableComponentId> motion_components;
  for (const auto &binding : graph.control_bindings) {
    if (binding.provider_runtime_unit_id != runtime_unit_id ||
        binding.capability_id.value() != "execution.trajectory.execute") {
      continue;
    }
    if (binding.resource_bindings.empty() ||
        !motion_counts.emplace(TargetKey{binding.controllable_component_id,
                                        binding.command_target_id},
                               binding.resource_bindings.size()).second) {
      Fail("native initial-state ownership is empty or ambiguous");
    }
    if (!motion_components.insert(binding.controllable_component_id).second) {
      throw RetainedBundleError(
          "native runtime supports at most one motion target per component: " +
          binding.controllable_component_id.value());
    }
  }
  if (result.motion_target_limits.size() != motion_counts.size() ||
      std::any_of(result.motion_target_limits.begin(),
                  result.motion_target_limits.end(), [&](const auto &row) {
                    return !motion_counts.contains(
                        {row.controllable_component_id, row.command_target_id});
                  })) {
    Fail("native motion limits do not exactly cover hosted targets");
  }
  std::size_t total = 0U;
  std::size_t selected_offset = 0U;
  std::size_t selected_count = 0U;
  for (const auto &[key, count] : motion_counts) {
    if (count > result.initial_joint_positions_rad.size() - total) {
      Fail("native initial state does not match exact binding resources");
    }
    if (key.first == component_id) {
      selected_offset = total;
      selected_count = count;
    }
    total += count;
  }
  if (total != result.initial_joint_positions_rad.size()) {
    Fail("native initial state has unowned coordinates");
  }
  result.initial_joint_positions_rad = std::vector<double>(
      result.initial_joint_positions_rad.begin() + selected_offset,
      result.initial_joint_positions_rad.begin() + selected_offset +
          selected_count);
  const auto unrelated = [&](const auto &row) {
    return row.controllable_component_id != component_id;
  };
  std::erase_if(result.motion_target_limits, unrelated);
  std::erase_if(result.direct_control_joint_limits, unrelated);
  std::erase_if(result.gripper_target_configurations, unrelated);
  return result;
}

namespace native_execution_runtime_contract {

Configuration require_component_configuration(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id) {
  return detail::select_native_component_configuration(
      require_configuration(bundle, runtime_unit_id), bundle.value().cell_graph,
      runtime_unit_id, component_id);
}

CanonicalValue component_configuration_projection(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id) {
  const auto selected =
      require_component_configuration(bundle, runtime_unit_id, component_id);
  const auto *graph = bundle.normalized_root().find_object_member("cell_graph");
  const auto *units =
      graph == nullptr ? nullptr : graph->find_object_member("runtime_units");
  if (units == nullptr || !units->is_array()) {
    Fail("native component configuration lacks its verified graph projection");
  }
  for (const auto &unit : units->as_array()) {
    const auto *id = unit.find_object_member("runtime_unit_id");
    if (id == nullptr || !id->is_string() ||
        id->as_string() != runtime_unit_id.value()) {
      continue;
    }
    const auto *configuration = unit.find_object_member("configuration");
    if (configuration == nullptr || !configuration->is_object()) {
      Fail("native component configuration lacks its verified settings");
    }
    auto result = configuration->as_object();
    for (auto &[name, value] : result) {
      if (name == "initial_joint_positions_rad") {
        CanonicalValue::Array positions;
        for (const auto position : selected.initial_joint_positions_rad) {
          positions.emplace_back(position);
        }
        value = CanonicalValue(std::move(positions));
      } else if (name == "motion_target_limits" ||
                 name == "direct_control_joint_limits" ||
                 name == "gripper_target_configurations") {
        CanonicalValue::Array rows;
        for (const auto &row : value.as_array()) {
          const auto *owner = row.find_object_member("controllable_component_id");
          if (owner == nullptr || !owner->is_string()) {
            Fail("native target configuration has no component owner");
          }
          if (owner->as_string() == component_id.value()) {
            rows.push_back(row);
          }
        }
        value = CanonicalValue(std::move(rows));
      }
    }
    return CanonicalValue(std::move(result));
  }
  Fail("native component configuration has no runtime projection");
}

} // namespace native_execution_runtime_contract
} // namespace manyforge::deployment
