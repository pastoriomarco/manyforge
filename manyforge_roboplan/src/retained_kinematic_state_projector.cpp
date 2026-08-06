#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>

#include <Eigen/Core>

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] core::ErrorDTO projection_error(std::string reason,
                                              std::string message) {
  return core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
      std::move(message), {{"reason", std::move(reason)}}};
}

[[noreturn]] void construction_fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}

[[nodiscard]] double position_value(
    const core::CoordinatePositionDTO& value) noexcept {
  if (const auto* angular = std::get_if<core::AngularPositionDTO>(&value)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(value).value_m;
}

[[nodiscard]] core::CoordinatePositionDTO tagged_position(
    core::CoordinateKind kind, double value) {
  return kind == core::CoordinateKind::kAngular
             ? core::CoordinatePositionDTO{core::AngularPositionDTO{value}}
             : core::CoordinatePositionDTO{core::LinearPositionDTO{value}};
}

[[nodiscard]] bool snapshot_matches_context(
    const core::ObservedStateSnapshotDTO& state,
    const core::ObservedStateQueryContextDTO& context) noexcept {
  if (state.controllable_component_id != context.controllable_component_id ||
      state.command_target_id != context.command_target_id ||
      state.control_domain_id != context.control_domain_id ||
      state.control_domain_revision != context.control_domain_revision ||
      state.control_binding_id != context.control_binding_id ||
      state.control_binding_revision != context.control_binding_revision ||
      state.provider_instance_id != context.provider_instance_id ||
      state.provider_manifest_id != context.provider_manifest_id ||
      state.provider_manifest_revision != context.provider_manifest_revision ||
      state.controller_topology_id != context.controller_topology_id ||
      state.topology_revision != context.topology_revision ||
      state.control_route_id != context.control_route_id ||
      state.active_control_configuration_id !=
          context.active_control_configuration_id ||
      state.active_control_configuration_generation !=
          context.active_control_configuration_generation ||
      state.resources != context.resources ||
      state.joint_names != context.joint_names ||
      state.source_epoch != context.runtime_epoch ||
      state.state_authority_id != context.state_authority_id ||
      state.state_authority_class != context.state_authority_class ||
      state.freshness_budget_ns != context.freshness_budget_ns ||
      state.positions.size() != context.coordinate_kinds.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < state.positions.size(); ++index) {
    if (core::coordinate_kind(state.positions[index]) !=
        context.coordinate_kinds[index]) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::size_t scalar_q_index(const roboplan::Scene& scene,
                                         const std::string& joint_name) {
  const auto& model = scene.getModel();
  if (joint_name.empty() || !model.existJointName(joint_name)) {
    construction_fail("retained state coordinate is absent from RoboPlan");
  }
  const auto joint_id = model.getJointId(joint_name);
  if (joint_id >= model.idx_qs.size() || joint_id >= model.nqs.size() ||
      model.nqs[joint_id] != 1) {
    construction_fail(
        "retained state coordinate is not a scalar RoboPlan joint");
  }
  return static_cast<std::size_t>(model.idx_qs[joint_id]);
}

[[nodiscard]] std::size_t mimic_expanded_index(
    const roboplan::Scene& scene, const std::string& joint_name) {
  const auto names = scene.getJointNamesWithMimics();
  const auto found = std::find(names.begin(), names.end(), joint_name);
  if (found == names.end()) {
    construction_fail(
        "retained state coordinate is absent from RoboPlan expanded state");
  }
  return static_cast<std::size_t>(std::distance(names.begin(), found));
}

[[nodiscard]] bool projection_matches(double actual, double expected) {
  constexpr double kAbsoluteTolerance = 1e-12;
  constexpr double kRelativeTolerance = 1e-10;
  return std::isfinite(actual) && std::isfinite(expected) &&
         std::abs(actual - expected) <=
             kAbsoluteTolerance +
                 kRelativeTolerance *
                     std::max(std::abs(actual), std::abs(expected));
}

}  // namespace

struct RetainedKinematicStateProjector::TargetProjection final {
  struct Coordinate final {
    core::ModelInstanceId model_instance_id;
    std::string compiled_joint_name;
    core::CoordinateKind coordinate_kind{core::CoordinateKind::kAngular};
    double multiplier{1.0};
    double offset{};
    bool derived_mimic{};
    std::optional<std::size_t> q_index;
    std::size_t expanded_index{};
  };

  core::ObservedStateQueryContextDTO context;
  std::vector<std::vector<Coordinate>> resource_coordinates;
};

RetainedKinematicStateProjector::RetainedKinematicStateProjector(
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex,
    std::vector<TargetProjection> targets)
    : scene_(std::move(scene)),
      scene_mutex_(std::move(scene_mutex)),
      targets_(std::move(targets)) {}

RetainedKinematicStateProjector::~RetainedKinematicStateProjector() = default;

RetainedKinematicStateProjector::RetainedKinematicStateProjector(
    RetainedKinematicStateProjector&&) noexcept = default;

RetainedKinematicStateProjector&
RetainedKinematicStateProjector::operator=(
    RetainedKinematicStateProjector&&) noexcept = default;

RetainedKinematicStateProjector
RetainedKinematicStateProjector::create(
    std::shared_ptr<roboplan::Scene> scene,
    std::shared_ptr<std::mutex> scene_mutex,
    RetainedKinematicModelMapInput model_map,
    std::vector<core::RuntimeParticipantBinding> bindings,
    std::int64_t freshness_budget_ns) {
  if (!scene || !scene_mutex || bindings.empty() ||
      model_map.retained_bundle_id.empty() || model_map.graph_revision.empty() ||
      model_map.control_bindings.empty()) {
    construction_fail(
        "retained state projector requires verified model inputs");
  }

  std::vector<TargetProjection> targets;
  targets.reserve(bindings.size());
  std::set<std::tuple<core::ControllableComponentId, core::CommandTargetId,
                      core::ControlBindingId>>
      retained_target_rows;
  for (const auto& row : model_map.control_bindings) {
    if (row.control_binding_id.empty() ||
        row.controllable_component_id.empty() || row.command_target_id.empty() ||
        row.resources.empty() ||
        !retained_target_rows
             .emplace(row.controllable_component_id, row.command_target_id,
                      row.control_binding_id)
             .second) {
      construction_fail(
          "retained state model map has an incomplete or duplicate target");
    }
    std::set<core::ResourceId> row_resources;
    std::set<std::pair<core::ModelInstanceId, std::string>> row_coordinates;
    for (const auto& resource : row.resources) {
      if (resource.resource_id.empty() || resource.coordinates.empty() ||
          !row_resources.insert(resource.resource_id).second) {
        construction_fail(
            "retained state model map has an incomplete or duplicate resource");
      }
      for (const auto& coordinate : resource.coordinates) {
        if (!row_coordinates
                 .emplace(coordinate.model_instance_id,
                          coordinate.compiled_joint_name)
                 .second) {
          construction_fail(
              "retained state model map repeats one model coordinate");
        }
      }
    }
  }
  std::set<std::pair<core::ControlBindingId, core::ControlBindingRevision>>
      control_bindings;
  for (const auto& binding : bindings) {
    const auto& value = binding.value();
    if (value.retained_bundle_id != model_map.retained_bundle_id ||
        value.graph_revision != model_map.graph_revision ||
        !value.control_path.has_value()) {
      construction_fail(
          "runtime state binding crosses its retained bundle or graph");
    }
    auto context_result = core::make_observed_state_query_context(
        binding, freshness_budget_ns);
    if (!context_result.ok()) {
      construction_fail(context_result.error().message);
    }
    auto context = std::move(context_result.value());
    if (!control_bindings
             .emplace(context.control_binding_id,
                      context.control_binding_revision)
             .second) {
      construction_fail(
          "retained state projector repeats one control binding");
    }
    const auto coordinate_map = std::find_if(
        model_map.control_bindings.begin(), model_map.control_bindings.end(),
        [&](const RetainedKinematicModelMapInput::ControlBinding& candidate) {
          return candidate.controllable_component_id ==
                     context.controllable_component_id &&
                 candidate.command_target_id == context.command_target_id &&
                 candidate.control_binding_id == context.control_binding_id;
        });
    if (coordinate_map == model_map.control_bindings.end() ||
        coordinate_map->resources.size() != context.resources.size()) {
      construction_fail(
          "runtime state binding has no exact retained target coordinate map");
    }

    TargetProjection projection{std::move(context), {}};
    projection.resource_coordinates.reserve(coordinate_map->resources.size());
    for (std::size_t index = 0U;
         index < coordinate_map->resources.size(); ++index) {
      const auto& row = coordinate_map->resources[index];
      if (row.resource_id != projection.context.resources[index] ||
          row.coordinates.empty() || row.coordinates.front().derived_mimic ||
          row.coordinates.front().compiled_joint_name !=
              projection.context.joint_names[index] ||
          row.coordinates.front().coordinate_kind !=
              projection.context.coordinate_kinds[index]) {
        construction_fail(
            "runtime state resource order or unit differs from retained map");
      }
      std::vector<TargetProjection::Coordinate> coordinates;
      coordinates.reserve(row.coordinates.size());
      for (std::size_t coordinate_index = 0U;
           coordinate_index < row.coordinates.size(); ++coordinate_index) {
        const auto& coordinate = row.coordinates[coordinate_index];
        if (coordinate.model_instance_id.empty() ||
            coordinate.compiled_joint_name.empty() ||
            !std::isfinite(coordinate.multiplier) ||
            !std::isfinite(coordinate.offset) ||
            coordinate.coordinate_kind !=
                projection.context.coordinate_kinds[index] ||
            coordinate.derived_mimic != (coordinate_index != 0U) ||
            (coordinate_index == 0U &&
             (coordinate.multiplier != 1.0 || coordinate.offset != 0.0))) {
          construction_fail(
              "retained state coordinate transform or unit is invalid");
        }
        coordinates.push_back(TargetProjection::Coordinate{
            coordinate.model_instance_id,
            coordinate.compiled_joint_name,
            projection.context.coordinate_kinds[index],
            coordinate.multiplier,
            coordinate.offset,
            coordinate.derived_mimic,
            coordinate.derived_mimic
                ? std::nullopt
                : std::optional<std::size_t>(scalar_q_index(
                      *scene, coordinate.compiled_joint_name)),
            mimic_expanded_index(*scene, coordinate.compiled_joint_name)});
      }
      projection.resource_coordinates.push_back(std::move(coordinates));
    }
    targets.push_back(std::move(projection));
  }
  return RetainedKinematicStateProjector(
      std::move(scene), std::move(scene_mutex), std::move(targets));
}

RetainedStateProjectionResult RetainedKinematicStateProjector::apply(
    const core::ObservedStateSnapshotDTO& state,
    std::int64_t now_ns,
    RetainedStateProjectionMode mode) {
  const auto found = std::find_if(
      targets_.begin(), targets_.end(), [&](const TargetProjection& target) {
        return target.context.control_binding_id == state.control_binding_id &&
               target.context.control_binding_revision ==
                   state.control_binding_revision;
      });
  if (found == targets_.end() || !snapshot_matches_context(state, found->context)) {
    return {RetainedStateProjectionStatus::kRejected,
            projection_error("retained_state_binding_mismatch",
                             "observed state does not match a retained target")};
  }
  if (auto valid = core::validate_observed_state_snapshot(state, now_ns);
      !valid.ok()) {
    return {RetainedStateProjectionStatus::kRejected, valid.error()};
  }

  std::unique_lock<std::mutex> lock(*scene_mutex_, std::defer_lock);
  if (mode == RetainedStateProjectionMode::kBestEffort) {
    if (!lock.try_lock()) {
      return {RetainedStateProjectionStatus::kBusy, std::nullopt};
    }
  } else {
    lock.lock();
  }
  std::optional<Eigen::VectorXd> previous;
  bool scene_mutated = false;
  const auto restore_previous = [&]() noexcept {
    if (!scene_mutated || !previous.has_value()) {
      return true;
    }
    try {
      scene_->setJointPositions(*previous);
      scene_mutated = false;
      return true;
    } catch (...) {
      return false;
    }
  };
  try {
    previous = scene_->getCurrentJointPositions();
    Eigen::VectorXd q = *previous;
    for (std::size_t resource_index = 0U;
         resource_index < found->resource_coordinates.size();
         ++resource_index) {
      const double direct = position_value(state.positions[resource_index]);
      for (const auto& coordinate :
           found->resource_coordinates[resource_index]) {
        if (!coordinate.q_index.has_value()) {
          continue;
        }
        if (*coordinate.q_index >= static_cast<std::size_t>(q.size())) {
          return {RetainedStateProjectionStatus::kFailed,
                  projection_error("retained_state_q_index_invalid",
                                   "retained model coordinate index changed")};
        }
        q[static_cast<Eigen::Index>(*coordinate.q_index)] = direct;
      }
    }
    scene_->setJointPositions(q);
    scene_mutated = true;
    const Eigen::VectorXd expanded =
        scene_->getCurrentJointPositionsWithMimics();
    for (std::size_t resource_index = 0U;
         resource_index < found->resource_coordinates.size();
         ++resource_index) {
      const double direct = position_value(state.positions[resource_index]);
      for (const auto& coordinate :
           found->resource_coordinates[resource_index]) {
        if (coordinate.expanded_index >=
                static_cast<std::size_t>(expanded.size()) ||
            !projection_matches(
                expanded[static_cast<Eigen::Index>(
                    coordinate.expanded_index)],
                direct * coordinate.multiplier + coordinate.offset)) {
          if (!restore_previous()) {
            return {RetainedStateProjectionStatus::kFailed,
                    projection_error(
                        "retained_state_rollback_failed",
                        "RoboPlan rejected the retained mimic projection and "
                        "the previous state could not be restored")};
          }
          return {RetainedStateProjectionStatus::kFailed,
                  projection_error(
                      "retained_state_mimic_semantics_mismatch",
                      "RoboPlan state differs from the retained coordinate "
                      "projection")};
        }
      }
    }
    scene_mutated = false;
    return {RetainedStateProjectionStatus::kApplied, std::nullopt};
  } catch (const std::exception& error) {
    const bool restored = restore_previous();
    return {RetainedStateProjectionStatus::kFailed,
            projection_error(
                restored ? "retained_state_projection_failed"
                         : "retained_state_rollback_failed",
                restored
                    ? error.what()
                    : "RoboPlan state projection failed and rollback did not "
                      "restore the previous state")};
  } catch (...) {
    const bool restored = restore_previous();
    return {RetainedStateProjectionStatus::kFailed,
            projection_error(
                restored ? "retained_state_projection_failed"
                         : "retained_state_rollback_failed",
                restored
                    ? "RoboPlan state projection failed"
                    : "RoboPlan state projection failed and rollback did not "
                      "restore the previous state")};
  }
}

core::Result<core::ControlledStateDTO>
RetainedKinematicStateProjector::current_state(
    const core::ControlBindingId& control_binding_id,
    const core::ControlBindingRevision& control_binding_revision) const {
  const auto found = std::find_if(
      targets_.begin(), targets_.end(), [&](const TargetProjection& target) {
        return target.context.control_binding_id == control_binding_id &&
               target.context.control_binding_revision ==
                   control_binding_revision;
      });
  if (found == targets_.end()) {
    return core::Result<core::ControlledStateDTO>::Err(
        projection_error("retained_state_binding_unknown",
                         "retained state binding is not registered"));
  }
  std::lock_guard<std::mutex> lock(*scene_mutex_);
  try {
    const Eigen::VectorXd q = scene_->getCurrentJointPositions();
    std::vector<core::CoordinatePositionDTO> positions;
    positions.reserve(found->resource_coordinates.size());
    for (const auto& coordinates : found->resource_coordinates) {
      const auto& direct = coordinates.front();
      if (!direct.q_index.has_value() ||
          *direct.q_index >= static_cast<std::size_t>(q.size())) {
        return core::Result<core::ControlledStateDTO>::Err(
            projection_error("retained_state_q_index_invalid",
                             "retained model coordinate index changed"));
      }
      positions.push_back(tagged_position(
          direct.coordinate_kind,
          q[static_cast<Eigen::Index>(*direct.q_index)]));
    }
    const auto& context = found->context;
    core::ControlledStateDTO state{
        context.controllable_component_id,
        context.command_target_id,
        context.control_domain_id,
        context.control_domain_revision,
        context.control_binding_id,
        context.control_binding_revision,
        context.controller_topology_id,
        context.topology_revision,
        context.control_route_id,
        context.active_control_configuration_id,
        context.active_control_configuration_generation,
        context.resources,
        context.joint_names,
        std::move(positions),
        std::nullopt};
    if (auto valid = core::validate_controlled_state(state); !valid.ok()) {
      return core::Result<core::ControlledStateDTO>::Err(valid.error());
    }
    return core::Result<core::ControlledStateDTO>::Ok(std::move(state));
  } catch (const std::exception& error) {
    return core::Result<core::ControlledStateDTO>::Err(
        projection_error("retained_state_read_failed", error.what()));
  }
}

core::Result<std::vector<RetainedModelCoordinateStateDTO>>
RetainedKinematicStateProjector::model_coordinate_state() const {
  std::lock_guard<std::mutex> lock(*scene_mutex_);
  try {
    const Eigen::VectorXd expanded =
        scene_->getCurrentJointPositionsWithMimics();
    std::vector<RetainedModelCoordinateStateDTO> result;
    std::set<std::string> names;
    for (const auto& target : targets_) {
      for (const auto& coordinates : target.resource_coordinates) {
        for (const auto& coordinate : coordinates) {
          if (!names.insert(coordinate.compiled_joint_name).second) {
            continue;
          }
          if (coordinate.expanded_index >=
              static_cast<std::size_t>(expanded.size())) {
            return core::Result<
                std::vector<RetainedModelCoordinateStateDTO>>::Err(
                projection_error("retained_state_q_index_invalid",
                                 "retained model coordinate index changed"));
          }
          result.push_back(RetainedModelCoordinateStateDTO{
              coordinate.model_instance_id,
              coordinate.compiled_joint_name,
              tagged_position(
                  coordinate.coordinate_kind,
                  expanded[static_cast<Eigen::Index>(
                      coordinate.expanded_index)])});
        }
      }
    }
    std::sort(result.begin(), result.end(), [](const auto& left,
                                               const auto& right) {
      return std::tie(left.model_instance_id, left.compiled_joint_name) <
             std::tie(right.model_instance_id, right.compiled_joint_name);
    });
    return core::Result<std::vector<RetainedModelCoordinateStateDTO>>::Ok(
        std::move(result));
  } catch (const std::exception& error) {
    return core::Result<std::vector<RetainedModelCoordinateStateDTO>>::Err(
        projection_error("retained_state_read_failed", error.what()));
  }
}

std::vector<core::ObservedStateQueryContextDTO>
RetainedKinematicStateProjector::query_contexts() const {
  std::vector<core::ObservedStateQueryContextDTO> result;
  result.reserve(targets_.size());
  for (const auto& target : targets_) {
    result.push_back(target.context);
  }
  return result;
}

}  // namespace manyforge::roboplan_backend
