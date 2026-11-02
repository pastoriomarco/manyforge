#include <manyforge_core/configured_robot_measured_state.hpp>
#include <map>

#include <algorithm>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace manyforge::core {
namespace {

ErrorDTO unavailable(std::string reason,
                     const std::optional<ControllableComponentId>& component = std::nullopt) {
  ErrorDTO error{"configured_robot_measured_state_unavailable",
                 "configured robot measured position is unavailable",
                 {{"reason", std::move(reason)}}};
  if (component && !component->empty()) error.details.emplace("component_id", component->value());
  return error;
}

ErrorDTO source_error(ErrorDTO error,
                      const ConfiguredRobotMeasuredStateRequirement& robot,
                      const RequiredRobotMeasuredStateSource& source,
                      const ObservedStateSnapshotDTO* actual = nullptr,
                      const std::int64_t now_ns = 0) {
  const auto& query = source.descriptor.publication.query_context;
  // These name required configuration, never inferred observed authority.
  error.details.emplace("required_component_id", robot.component_id.value());
  error.details.emplace("required_source_id", source.descriptor.publication.source_precedence.front().source_id);
  error.details.emplace("required_control_binding_id", query.control_binding_id.value());
  error.details.emplace("required_control_binding_revision", query.control_binding_revision.value());
  error.details.emplace("required_runtime_epoch", query.runtime_epoch.value());
  if (actual != nullptr) {
    error.details.emplace("observed_received_timestamp_ns", std::to_string(actual->received_timestamp_ns));
    if (actual->received_timestamp_ns >= 0 && now_ns >= actual->received_timestamp_ns) {
      error.details.emplace("observed_age_ns", std::to_string(now_ns - actual->received_timestamp_ns));
    }
  }
  return error;
}

bool matches(const ObservedStateSnapshotDTO& state,
             const MeasuredStateSourceDescriptor& descriptor) {
  const auto& config = descriptor.publication;
  const auto& query = config.query_context;
  if (state.controllable_component_id != query.controllable_component_id ||
      state.command_target_id != query.command_target_id ||
      state.control_domain_id != query.control_domain_id ||
      state.control_domain_revision != query.control_domain_revision ||
      state.control_binding_id != query.control_binding_id ||
      state.control_binding_revision != query.control_binding_revision ||
      state.controller_topology_id != query.controller_topology_id ||
      state.topology_revision != query.topology_revision ||
      state.control_route_id != query.control_route_id ||
      state.active_control_configuration_id != query.active_control_configuration_id ||
      state.active_control_configuration_generation != query.active_control_configuration_generation ||
      state.provider_instance_id != query.provider_instance_id ||
      state.provider_manifest_id != query.provider_manifest_id ||
      state.provider_manifest_revision != query.provider_manifest_revision ||
      state.source_epoch != query.runtime_epoch ||
      state.state_authority_id != query.state_authority_id ||
      state.state_authority_class != StateAuthorityClass::kMeasured ||
      state.authority_generation != config.authority_generation ||
      state.freshness_budget_ns != query.freshness_budget_ns ||
      state.source_kind != ObservedStateSourceKind::kJointState ||
      state.source_id != config.source_precedence.front().source_id ||
      state.resources != query.resources || state.joint_names != query.joint_names ||
      state.positions.size() != query.coordinate_kinds.size()) return false;
  for (std::size_t i = 0; i < state.positions.size(); ++i) {
    if (coordinate_kind(state.positions[i]) != query.coordinate_kinds[i]) return false;
  }
  return true;
}

}  // namespace

Result<void> MeasuredStateSourceDescriptor::validate() const {
  const auto& query = publication.query_context;
  if (query.state_authority_class != StateAuthorityClass::kMeasured ||
      query.freshness_budget_ns <= 0 || publication.source_precedence.size() != 1U ||
      publication.source_precedence.front().kind != ObservedRobotStateSourceKind::kJointState) {
    return Result<void>::Err(unavailable("continuous_measured_joint_source_required"));
  }
  const std::set<std::string> source_names(source_joint_names.begin(), source_joint_names.end());
  if (source_joint_names.size() != query.resources.size() ||
      source_names.size() != source_joint_names.size() || source_names.contains("")) {
    return Result<void>::Err(unavailable("measured_source_coordinate_mapping_invalid"));
  }
  try {
    // Reuse the existing full construction grammar, without publishing or
    // minting a sample. The descriptor remains distinct from command authority.
    const AddressedObservedStatePublication checked(publication);
    static_cast<void>(checked);
    return Result<void>::Ok();
  } catch (const std::exception&) {
    return Result<void>::Err(unavailable("measured_source_descriptor_invalid"));
  }
}

ConfiguredRobotMeasuredStateInventory::ConfiguredRobotMeasuredStateInventory(
    std::vector<ConfiguredRobotMeasuredStateRequirement> robots)
    : robots_(std::move(robots)) {
  std::set<ControllableComponentId> components;
  for (const auto& robot : robots_) {
    if (robot.component_id.empty() || !components.insert(robot.component_id).second ||
        robot.required_resources.empty()) {
      throw std::invalid_argument("measured robot inventory has invalid membership");
    }
    const std::set<ResourceId> required(robot.required_resources.begin(), robot.required_resources.end());
    if (std::any_of(required.begin(), required.end(), [](const auto& resource) { return resource.empty(); }) ||
        required.size() != robot.required_resources.size()) {
      throw std::invalid_argument("measured robot inventory has invalid geometry resources");
    }
    std::map<ResourceId, const RuntimeResourceBindingDTO*> geometry;
    if (!robot.geometry_coordinates.empty()) {
      for (const auto& coordinate : robot.geometry_coordinates) {
        if (coordinate.resource_role_id.empty() ||
            !required.contains(coordinate.resource_id) ||
            !geometry.emplace(coordinate.resource_id, &coordinate).second ||
            coordinate.compiled_resource_name.empty() ||
            !coordinate.source_joint_name || coordinate.source_joint_name->empty() ||
            (coordinate.coordinate_kind != CoordinateKind::kAngular &&
             coordinate.coordinate_kind != CoordinateKind::kLinear)) {
          throw std::invalid_argument("measured robot inventory has invalid geometry coordinates");
        }
      }
      if (geometry.size() != required.size()) {
        throw std::invalid_argument("measured robot inventory has incomplete geometry coordinates");
      }
    }
    std::set<ResourceId> covered;
    for (const auto& source : robot.sources) {
      if (!source.descriptor.validate().ok() ||
          source.descriptor.publication.query_context.controllable_component_id != robot.component_id) {
        throw std::invalid_argument("measured source does not name its configured robot");
      }
      const auto& query = source.descriptor.publication.query_context;
      for (std::size_t index = 0; index < query.resources.size(); ++index) {
        const auto& resource = query.resources[index];
        if (!required.contains(resource) || !covered.insert(resource).second) {
          throw std::invalid_argument("measured robot sources overlap or exceed geometry resources");
        }
        if (!geometry.empty()) {
          const auto& coordinate = *geometry.at(resource);
          if (coordinate.compiled_resource_name != query.joint_names[index] ||
              coordinate.coordinate_kind != query.coordinate_kinds[index] ||
              *coordinate.source_joint_name != source.descriptor.source_joint_names[index]) {
            throw std::invalid_argument("measured source differs from verified geometry coordinates");
          }
        }
      }
    }
    // A missing source is retained as unavailable membership. Partial coverage
    // must never make that robot disappear from a later all-cell read.
  }
}

Result<RobotMeasuredStateCapture> ConfiguredRobotMeasuredStateInventory::ReadRequirement(
    const ConfiguredRobotMeasuredStateRequirement& robot,
    const std::vector<RequiredRobotMeasuredStateSource>& sources,
    const std::int64_t now_ns) const {
  using RobotResult = Result<RobotMeasuredStateCapture>;
  RobotMeasuredStateCapture observed{robot.component_id, {}};
  observed.sources.reserve(sources.size());
  std::set<ResourceId> covered;
  for (const auto& source : sources) {
    if (!source.reader) return RobotResult::Err(source_error(
        unavailable("measured_source_missing", robot.component_id), robot, source));
    try {
      auto sample = source.reader->current_state(source.descriptor.publication.query_context, now_ns);
      if (!sample.ok()) return RobotResult::Err(source_error(sample.error(), robot, source));
      if (!matches(sample.value(), source.descriptor)) {
        return RobotResult::Err(source_error(
            unavailable("measured_source_identity_mismatch", robot.component_id),
            robot, source, &sample.value(), now_ns));
      }
      auto valid = validate_observed_state_snapshot(sample.value(), now_ns);
      if (!valid.ok()) {
        auto error = valid.error();
        error.details.emplace("component_id", robot.component_id.value());
        return RobotResult::Err(source_error(std::move(error), robot, source, &sample.value(), now_ns));
      }
      covered.insert(sample.value().resources.begin(), sample.value().resources.end());
      observed.sources.push_back(std::move(sample.value()));
    } catch (const std::exception&) {
      return RobotResult::Err(source_error(
          unavailable("measured_source_read_failed", robot.component_id), robot, source));
    }
  }
  if (covered != std::set<ResourceId>(robot.required_resources.begin(), robot.required_resources.end())) {
    return RobotResult::Err(unavailable("measured_geometry_coordinates_missing", robot.component_id));
  }
  return RobotResult::Ok(std::move(observed));
}

Result<ConfiguredRobotMeasuredStateCapture> ConfiguredRobotMeasuredStateInventory::Read(
    const std::int64_t now_ns) const {
  using CaptureResult = Result<ConfiguredRobotMeasuredStateCapture>;
  if (now_ns < 0) return CaptureResult::Err(unavailable("receiver_time_invalid"));
  ConfiguredRobotMeasuredStateCapture capture{now_ns, {}};
  capture.robots.reserve(robots_.size());
  for (const auto& robot : robots_) {
    auto observed = ReadRequirement(robot, robot.sources, now_ns);
    if (!observed.ok()) return CaptureResult::Err(observed.error());
    capture.robots.push_back(std::move(observed.value()));
  }
  return CaptureResult::Ok(std::move(capture));
}

Result<RobotMeasuredStateCapture> ConfiguredRobotMeasuredStateInventory::ReadComponent(
    const RobotMeasuredStateSourceView& source_view, const std::int64_t now_ns) const {
  using RobotResult = Result<RobotMeasuredStateCapture>;
  if (now_ns < 0) return RobotResult::Err(unavailable("receiver_time_invalid"));
  const auto robot = std::find_if(robots_.begin(), robots_.end(), [&](const auto& requirement) {
    return requirement.component_id == source_view.component_id;
  });
  if (robot == robots_.end()) {
    return RobotResult::Err(unavailable("configured_robot_missing", source_view.component_id));
  }
  // Static rows were qualified by construction. An explicit route-source
  // view must satisfy the same construction grammar before any provider read.
  const std::set<ResourceId> required(robot->required_resources.begin(),
                                      robot->required_resources.end());
  std::map<ResourceId, std::pair<std::string, CoordinateKind>> coordinates;
  if (!robot->geometry_coordinates.empty()) {
    for (const auto& coordinate : robot->geometry_coordinates) {
      coordinates.emplace(coordinate.resource_id,
                          std::pair{coordinate.compiled_resource_name, coordinate.coordinate_kind});
    }
  } else {
    for (const auto& source : robot->sources) {
      const auto& query = source.descriptor.publication.query_context;
      for (std::size_t index = 0; index < query.resources.size(); ++index) {
        coordinates.emplace(query.resources[index],
                            std::pair{query.joint_names[index], query.coordinate_kinds[index]});
      }
    }
  }
  std::set<ResourceId> covered;
  if (source_view.sources.size() > required.size()) {
    return RobotResult::Err(unavailable("measured_source_view_invalid", robot->component_id));
  }
  for (const auto& source : source_view.sources) {
    if (!source.descriptor.validate().ok() ||
        source.descriptor.publication.query_context.controllable_component_id !=
            robot->component_id) {
      return RobotResult::Err(unavailable("measured_source_view_invalid", robot->component_id));
    }
    const auto& query = source.descriptor.publication.query_context;
    for (std::size_t index = 0; index < query.resources.size(); ++index) {
      const auto& resource = query.resources[index];
      if (!required.contains(resource) || !covered.insert(resource).second) {
        return RobotResult::Err(unavailable("measured_source_view_invalid", robot->component_id));
      }
      const auto coordinate = coordinates.find(resource);
      const auto physical = std::find_if(robot->geometry_coordinates.begin(),
          robot->geometry_coordinates.end(), [&](const auto& row) { return row.resource_id == resource; });
      if (coordinate == coordinates.end() ||
          coordinate->second.first != query.joint_names[index] ||
          coordinate->second.second != query.coordinate_kinds[index] ||
          (physical != robot->geometry_coordinates.end() &&
           *physical->source_joint_name != source.descriptor.source_joint_names[index])) {
        return RobotResult::Err(unavailable("measured_source_view_geometry_mismatch",
                                           robot->component_id));
      }
    }
  }
  // A partial/missing view remains an unavailable configured requirement;
  // the shared read performs the same completeness check as all-cell Read.
  return ReadRequirement(*robot, source_view.sources, now_ns);
}

}  // namespace manyforge::core
