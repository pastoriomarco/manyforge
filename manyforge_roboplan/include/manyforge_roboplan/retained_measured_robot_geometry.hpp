#pragma once

#include <manyforge_core/configured_robot_measured_state.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// Pure geometry join for caller-authenticated complete robot membership.
/// Transport availability cannot narrow retained arm/tool geometry. The
/// product owner authenticates route/provider/source associations separately;
/// this leaf neither reads providers nor invents missing descriptors.
[[nodiscard]] inline core::Result<
    std::vector<core::ConfiguredRobotMeasuredStateRequirement>>
QualifyRetainedRobotMeasuredGeometry(
    const deployment::VerifiedRetainedModelMap &model_map,
    std::span<const core::ControllableComponentId> configured_robot_ids,
    std::span<const core::RuntimeParticipantBinding> compiled_physical_bindings,
    std::span<const core::RequiredRobotMeasuredStateSource> descriptor_rows) {
  using Result =
      core::Result<std::vector<core::ConfiguredRobotMeasuredStateRequirement>>;
  using CoordinateKey = std::pair<core::ModelInstanceId, std::string>;
  using BindingKey = std::tuple<core::ControllableComponentId,
                                core::CommandTargetId, core::ControlBindingId>;
  const auto fail =
      [](std::string reason,
         const std::optional<core::ControllableComponentId> &component =
             std::nullopt,
         std::string target_id = {}, std::string binding_id = {},
         std::string source_joint_name = {},
         std::string compiled_joint_name = {}) {
        core::ErrorDTO error{"retained_measured_robot_geometry_invalid",
                             "retained robot measured geometry does not match",
                             {{"reason", std::move(reason)}}};
        if (component && !component->empty())
          error.details.emplace("component_id", component->value());
        if (!target_id.empty())
          error.details.emplace("command_target_id", std::move(target_id));
        if (!binding_id.empty())
          error.details.emplace("control_binding_id", std::move(binding_id));
        if (!source_joint_name.empty())
          error.details.emplace("source_joint_name",
                                std::move(source_joint_name));
        if (!compiled_joint_name.empty())
          error.details.emplace("compiled_joint_name",
                                std::move(compiled_joint_name));
        return Result::Err(std::move(error));
      };
  const auto kind_of =
      [](const deployment::CompositionCoordinateProjection &coordinate)
      -> std::optional<core::CoordinateKind> {
    if (coordinate.coordinate_kind ==
            deployment::UrdfCoordinateKind::kAngular &&
        coordinate.unit == deployment::CompositionCoordinateUnit::kRadians)
      return core::CoordinateKind::kAngular;
    if (coordinate.coordinate_kind == deployment::UrdfCoordinateKind::kLinear &&
        coordinate.unit == deployment::CompositionCoordinateUnit::kMeters)
      return core::CoordinateKind::kLinear;
    return std::nullopt;
  };
  std::set<core::ControllableComponentId> membership;
  for (const auto &component : configured_robot_ids) {
    if (component.empty() || !membership.insert(component).second)
      return fail("configured_robot_membership_invalid", component);
  }

  std::vector<core::ConfiguredRobotMeasuredStateRequirement> robots;
  robots.reserve(configured_robot_ids.size());
  std::set<BindingKey> matched_bindings;
  for (const auto &component : configured_robot_ids) {
    std::set<core::ResourceId> required_resources;
    std::vector<core::RuntimeResourceBindingDTO> geometry_coordinates;
    std::set<CoordinateKey> covered_coordinates;
    for (const auto &binding : compiled_physical_bindings) {
      const auto &value = binding.value();
      if (value.controllable_component_id != component)
        continue;
      const auto fail_physical = [&](std::string reason,
                                     std::string source_joint_name = {},
                                     std::string compiled_joint_name = {}) {
        return fail(
            std::move(reason), component, value.command_target_id.value(),
            value.control_path ? value.control_path->control_binding_id.value()
                               : "",
            std::move(source_joint_name), std::move(compiled_joint_name));
      };
      if (value.retained_bundle_id != model_map.query().retained_bundle_id ||
          !value.control_path)
        return fail_physical("physical_binding_scope_mismatch");
      const auto *map =
          model_map.find_binding(component, value.command_target_id,
                                 value.control_path->control_binding_id);
      if (map == nullptr)
        return fail_physical("physical_binding_map_missing");
      const bool local_only =
          std::all_of(map->resources.begin(), map->resources.end(),
                      [](const auto &row) { return row.coordinates.empty(); });
      if (local_only)
        continue;
      if (!matched_bindings
               .emplace(component, value.command_target_id,
                        value.control_path->control_binding_id)
               .second)
        return fail_physical("physical_binding_duplicate");
      if (map->resources.size() != value.resources.size())
        return fail_physical("physical_resource_coverage_mismatch");
      for (std::size_t index = 0; index < map->resources.size(); ++index) {
        const auto &row = map->resources[index];
        const auto &physical = value.resources[index];
        if (row.coordinates.empty())
          return fail_physical("physical_model_coordinate_missing");
        const auto &direct = row.coordinates.front();
        const auto kind = kind_of(direct);
        if (!kind || row.resource_id != physical.resource_id ||
            row.resource_role_id != physical.resource_role_id ||
            physical.source_joint_name != direct.source_joint_name ||
            physical.compiled_resource_name != direct.compiled_joint_name ||
            physical.coordinate_kind != *kind)
          return fail_physical("physical_direct_coordinate_mismatch",
                               physical.source_joint_name.value_or(""),
                               physical.compiled_resource_name);
        for (std::size_t coordinate_index = 0;
             coordinate_index < row.coordinates.size(); ++coordinate_index) {
          const auto &coordinate = row.coordinates[coordinate_index];
          if (kind_of(coordinate) != kind ||
              !std::isfinite(coordinate.multiplier) ||
              !std::isfinite(coordinate.offset) ||
              coordinate.derived_mimic != (coordinate_index != 0) ||
              (coordinate_index == 0 &&
               (coordinate.multiplier != 1.0 || coordinate.offset != 0.0)))
            return fail_physical("physical_coordinate_transform_mismatch", coordinate.source_joint_name,
                                 coordinate.compiled_joint_name);
          const auto occurrence = std::find_if(
              model_map.model_occurrences().begin(),
              model_map.model_occurrences().end(), [&](const auto &model) {
                return model.model_instance_id == coordinate.model_instance_id;
              });
          if (occurrence == model_map.model_occurrences().end() ||
              occurrence->owner_component_id != component ||
              !occurrence->owner_model_role_id ||
              std::none_of(value.model_bindings.begin(),
                           value.model_bindings.end(), [&](const auto &model) {
                             return model.model_instance_id ==
                                        occurrence->model_instance_id &&
                                    model.model_revision ==
                                        occurrence->model_revision &&
                                    model.model_role_id ==
                                        *occurrence->owner_model_role_id;
                           }))
            return fail_physical("physical_model_occurrence_mismatch",
                                 coordinate.source_joint_name,
                                 coordinate.compiled_joint_name);
          const auto retained = std::find_if(
              model_map.coordinates().begin(), model_map.coordinates().end(),
              [&](const auto &candidate) {
                return candidate.model_instance_id ==
                           coordinate.model_instance_id &&
                       candidate.compiled_joint_name ==
                           coordinate.compiled_joint_name;
              });
          if (retained == model_map.coordinates().end() ||
              retained->owner_component_id != component ||
              retained->source_joint_name != coordinate.source_joint_name ||
              retained->coordinate_kind != coordinate.coordinate_kind ||
              retained->unit != coordinate.unit)
            return fail_physical("physical_retained_coordinate_mismatch",
                                 coordinate.source_joint_name,
                                 coordinate.compiled_joint_name);
          covered_coordinates.emplace(coordinate.model_instance_id,
                                      coordinate.compiled_joint_name);
        }
        if (required_resources.insert(row.resource_id).second) {
          geometry_coordinates.push_back(physical);
        } else {
          const auto previous = std::find_if(geometry_coordinates.begin(), geometry_coordinates.end(),
              [&](const auto& coordinate) { return coordinate.resource_id == row.resource_id; });
          if (previous == geometry_coordinates.end() ||
              previous->source_joint_name != physical.source_joint_name ||
              previous->compiled_resource_name != physical.compiled_resource_name ||
              previous->coordinate_kind != physical.coordinate_kind)
            return fail_physical("physical_geometry_resource_mismatch");
        }
      }
    }
    for (const auto &map : model_map.control_bindings()) {
      if (map.controllable_component_id == component &&
          std::any_of(
              map.resources.begin(), map.resources.end(),
              [](const auto &row) { return !row.coordinates.empty(); }) &&
          !matched_bindings.contains(
              {component, map.command_target_id, map.control_binding_id})) {
        // Several logical capabilities can address the same physical group.
        // Authenticate each alias against an actual supplied representative;
        // every retained resource and complete direct/mimic projection agrees.
        // Resource roles belong to each capability's own binding validation.
        const bool equivalent = std::any_of(
            compiled_physical_bindings.begin(),
            compiled_physical_bindings.end(), [&](const auto &binding) {
              const auto &value = binding.value();
              if (value.controllable_component_id != component ||
                  value.command_target_id != map.command_target_id ||
                  !value.control_path ||
                  !matched_bindings.contains(
                      {component, value.command_target_id,
                       value.control_path->control_binding_id}))
                return false;
              const auto *selected = model_map.find_binding(
                  component, value.command_target_id,
                  value.control_path->control_binding_id);
              return selected != nullptr &&
                     selected->resources.size() == map.resources.size() &&
                     std::equal(
                         selected->resources.begin(), selected->resources.end(),
                         map.resources.begin(),
                         [](const auto &left, const auto &right) {
                           return left.resource_id == right.resource_id &&
                                  left.coordinates == right.coordinates;
                         });
            });
        if (!equivalent) {
          const auto row =
              std::find_if(map.resources.begin(), map.resources.end(),
                           [](const auto &resource) {
                             return !resource.coordinates.empty();
                           });
          return fail("physical_geometry_binding_missing", component,
                      map.command_target_id.value(),
                      map.control_binding_id.value(),
                      row == map.resources.end()
                          ? ""
                          : row->coordinates.front().source_joint_name,
                      row == map.resources.end()
                          ? ""
                          : row->coordinates.front().compiled_joint_name);
        }
      }
    }
    for (const auto &coordinate : model_map.coordinates()) {
      if (coordinate.owner_component_id == component &&
          coordinate.coordinate_kind != deployment::UrdfCoordinateKind::kNone &&
          !covered_coordinates.contains(
              {coordinate.model_instance_id, coordinate.compiled_joint_name}))
        return fail("physical_geometry_coordinate_missing", component, {}, {},
                    coordinate.source_joint_name,
                    coordinate.compiled_joint_name);
    }
    if (required_resources.empty())
      return fail("configured_robot_geometry_missing", component);
    std::sort(geometry_coordinates.begin(), geometry_coordinates.end(),
              [](const auto &left, const auto &right) {
                return left.resource_id < right.resource_id;
              });
    robots.push_back({component,
                      {required_resources.begin(), required_resources.end()},
                      {},
                      std::move(geometry_coordinates)});
  }

  std::set<BindingKey> supplied_sources;
  std::set<std::pair<core::ControllableComponentId, core::ResourceId>>
      source_resources;
  for (const auto &source : descriptor_rows) {
    const auto &query = source.descriptor.publication.query_context;
    const auto &component = query.controllable_component_id;
    if (!membership.contains(component))
      return fail("descriptor_robot_not_configured", component);
    const auto valid = source.descriptor.validate();
    if (!valid.ok())
      return Result::Err(valid.error());
    const auto *map = model_map.find_binding(component, query.command_target_id,
                                             query.control_binding_id);
    if (map == nullptr ||
        !matched_bindings.contains(
            {component, query.command_target_id, query.control_binding_id}) ||
        map->resources.size() != query.resources.size())
      return fail("descriptor_geometry_binding_mismatch", component);
    if (!supplied_sources
             .emplace(component, query.command_target_id,
                      query.control_binding_id)
             .second)
      return fail("descriptor_duplicate", component);
    for (std::size_t index = 0; index < map->resources.size(); ++index) {
      const auto &row = map->resources[index];
      if (row.coordinates.empty() ||
          row.resource_id != query.resources[index] ||
          row.coordinates.front().compiled_joint_name !=
              query.joint_names[index] ||
          kind_of(row.coordinates.front()) != query.coordinate_kinds[index] ||
          !source_resources.emplace(component, row.resource_id).second)
        return fail("descriptor_geometry_coordinate_mismatch", component);
    }
    const auto robot =
        std::find_if(robots.begin(), robots.end(), [&](const auto &row) {
          return row.component_id == component;
        });
    robot->sources.push_back(source);
  }
  return Result::Ok(std::move(robots));
}

} // namespace manyforge::roboplan_backend
