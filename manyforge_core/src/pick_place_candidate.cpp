#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <manyforge_core/pick_place_candidate.hpp>
#include <numbers>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

constexpr double kVectorEpsilon = 1.0e-9;

template <typename T>
[[nodiscard]] Result<T> candidate_error(std::string reason, std::string message) {
  return Result<T>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                 std::move(message),
                                 {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool finite_pose(const Pose3DTO& pose) noexcept {
  const double values[]{pose.position_m.x_m, pose.position_m.y_m, pose.position_m.z_m,
                        pose.orientation.x,  pose.orientation.y,  pose.orientation.z,
                        pose.orientation.w};
  return std::all_of(std::begin(values), std::end(values),
                     [](const double value) { return std::isfinite(value); });
}

[[nodiscard]] double quaternion_norm(const QuaternionDTO& value) noexcept {
  return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
}

[[nodiscard]] Result<QuaternionDTO> normalized(QuaternionDTO value) {
  const double norm = quaternion_norm(value);
  if (!std::isfinite(norm) || norm < kVectorEpsilon) {
    return candidate_error<QuaternionDTO>("pick_candidate_orientation_invalid",
                                          "pick candidate orientation is non-finite or near zero");
  }
  value.x /= norm;
  value.y /= norm;
  value.z /= norm;
  value.w /= norm;
  return Result<QuaternionDTO>::Ok(value);
}

[[nodiscard]] QuaternionDTO multiply_quaternions(const QuaternionDTO& left,
                                                 const QuaternionDTO& right) {
  return {left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
          left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
          left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
          left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
}

[[nodiscard]] QuaternionDTO rpy_degrees_to_quaternion(const std::array<double, 3U>& rpy_deg) {
  const double scale = std::numbers::pi / 360.0;
  const double cr = std::cos(rpy_deg[0] * scale);
  const double sr = std::sin(rpy_deg[0] * scale);
  const double cp = std::cos(rpy_deg[1] * scale);
  const double sp = std::sin(rpy_deg[1] * scale);
  const double cy = std::cos(rpy_deg[2] * scale);
  const double sy = std::sin(rpy_deg[2] * scale);
  return {sr * cp * cy - cr * sp * sy, cr * sp * cy + sr * cp * sy, cr * cp * sy - sr * sp * cy,
          cr * cp * cy + sr * sp * sy};
}

[[nodiscard]] std::array<double, 3U> rotate_vector(const QuaternionDTO& orientation,
                                                   const std::array<double, 3U>& vector) {
  const QuaternionDTO input{vector[0], vector[1], vector[2], 0.0};
  const QuaternionDTO inverse{-orientation.x, -orientation.y, -orientation.z, orientation.w};
  const auto rotated = multiply_quaternions(multiply_quaternions(orientation, input), inverse);
  return {rotated.x, rotated.y, rotated.z};
}

[[nodiscard]] double dot(const std::array<double, 3U>& left,
                         const std::array<double, 3U>& right) noexcept {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] std::array<double, 3U> cross(const std::array<double, 3U>& left,
                                           const std::array<double, 3U>& right) noexcept {
  return {left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2],
          left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] double norm(const std::array<double, 3U>& value) noexcept {
  return std::sqrt(dot(value, value));
}

[[nodiscard]] std::array<double, 3U> normalize_vector(const std::array<double, 3U>& value) {
  const double length = norm(value);
  if (!std::isfinite(length) || length < kVectorEpsilon) {
    throw std::invalid_argument("cannot normalize near-zero vector");
  }
  return {value[0] / length, value[1] / length, value[2] / length};
}

[[nodiscard]] Result<QuaternionDTO> vertical_down_orientation(
    const QuaternionDTO& object_orientation) {
  auto normalized_object = normalized(object_orientation);
  if (!normalized_object.ok()) {
    return normalized_object;
  }
  const std::array<double, 3U> down{0.0, 0.0, -1.0};
  auto object_x = rotate_vector(normalized_object.value(), {1.0, 0.0, 0.0});
  const auto project = [&](const std::array<double, 3U>& value) {
    const double scale = dot(value, down);
    return std::array<double, 3U>{value[0] - scale * down[0], value[1] - scale * down[1],
                                  value[2] - scale * down[2]};
  };
  auto x_axis = project(object_x);
  if (norm(x_axis) < kVectorEpsilon) {
    x_axis = project(rotate_vector(normalized_object.value(), {0.0, 1.0, 0.0}));
  }
  x_axis = norm(x_axis) < kVectorEpsilon ? std::array<double, 3U>{1.0, 0.0, 0.0}
                                         : normalize_vector(x_axis);
  auto y_axis = cross(down, x_axis);
  y_axis = norm(y_axis) < kVectorEpsilon ? std::array<double, 3U>{0.0, -1.0, 0.0}
                                         : normalize_vector(y_axis);

  const double m00 = x_axis[0];
  const double m01 = y_axis[0];
  const double m02 = down[0];
  const double m10 = x_axis[1];
  const double m11 = y_axis[1];
  const double m12 = down[1];
  const double m20 = x_axis[2];
  const double m21 = y_axis[2];
  const double m22 = down[2];
  QuaternionDTO result;
  const double trace = m00 + m11 + m22;
  if (trace > 0.0) {
    const double scale = std::sqrt(trace + 1.0) * 2.0;
    result = {(m21 - m12) / scale, (m02 - m20) / scale, (m10 - m01) / scale, 0.25 * scale};
  } else if (m00 > m11 && m00 > m22) {
    const double scale = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
    result = {0.25 * scale, (m01 + m10) / scale, (m02 + m20) / scale, (m21 - m12) / scale};
  } else if (m11 > m22) {
    const double scale = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
    result = {(m01 + m10) / scale, 0.25 * scale, (m12 + m21) / scale, (m02 - m20) / scale};
  } else {
    const double scale = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
    result = {(m02 + m20) / scale, (m12 + m21) / scale, 0.25 * scale, (m10 - m01) / scale};
  }
  return normalized(result);
}

[[nodiscard]] Result<std::vector<double>> axis_values(const double minimum, const double maximum,
                                                      const double step) {
  if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(step) || step <= 0.0) {
    return candidate_error<std::vector<double>>(
        "pick_candidate_grid_axis_invalid",
        "pick candidate grid bounds and step must be finite with a positive "
        "step");
  }
  if (minimum > maximum + kVectorEpsilon) {
    return candidate_error<std::vector<double>>(
        "pick_candidate_grid_axis_reversed", "pick candidate envelope minimum exceeds its maximum");
  }
  if (std::abs(minimum - maximum) <= kVectorEpsilon) {
    return Result<std::vector<double>>::Ok({minimum});
  }
  std::vector<double> values;
  double current = minimum;
  while (current <= maximum + kVectorEpsilon) {
    const double rounded = std::round(current * 1.0e9) / 1.0e9;
    values.push_back(rounded);
    if (values.size() > 1'000'000U || current > std::numeric_limits<double>::max() - step) {
      return candidate_error<std::vector<double>>(
          "pick_candidate_grid_axis_unbounded",
          "pick candidate grid axis exceeds its deterministic bound");
    }
    current += step;
  }
  if (std::abs(values.back() - maximum) > kVectorEpsilon) {
    values.push_back(maximum);
  }
  return Result<std::vector<double>>::Ok(std::move(values));
}

[[nodiscard]] Result<std::vector<PickCandidateGrid::OffsetSample>> offset_samples(
    const PickOffsetEnvelopeDTO& envelope, const double translation_step_m,
    const double rotation_step_deg, const bool include_rotation,
    const std::size_t maximum_sample_count) {
  std::array<std::vector<double>, 6U> axes;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    auto values = axis_values(envelope.position_min_m[axis], envelope.position_max_m[axis],
                              translation_step_m);
    if (!values.ok()) {
      return Result<std::vector<PickCandidateGrid::OffsetSample>>::Err(values.error());
    }
    axes[axis] = std::move(values.value());
  }
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    if (!include_rotation) {
      axes[axis + 3U] = {0.0};
      continue;
    }
    auto values = axis_values(envelope.orientation_rpy_min_deg[axis],
                              envelope.orientation_rpy_max_deg[axis], rotation_step_deg);
    if (!values.ok()) {
      return Result<std::vector<PickCandidateGrid::OffsetSample>>::Err(values.error());
    }
    axes[axis + 3U] = std::move(values.value());
  }

  std::size_t count = 1U;
  for (const auto& axis : axes) {
    if (axis.empty() || count > std::numeric_limits<std::size_t>::max() / axis.size()) {
      return candidate_error<std::vector<PickCandidateGrid::OffsetSample>>(
          "pick_candidate_grid_size_overflow", "pick candidate grid size cannot be represented");
    }
    count *= axis.size();
    if (count > maximum_sample_count) {
      return candidate_error<std::vector<PickCandidateGrid::OffsetSample>>(
          "pick_candidate_count_exceeded",
          "pick candidate offset grid exceeds its declared total bound");
    }
  }
  std::vector<PickCandidateGrid::OffsetSample> result;
  result.reserve(count);
  for (const double x : axes[0]) {
    for (const double y : axes[1]) {
      for (const double z : axes[2]) {
        for (const double roll : axes[3]) {
          for (const double pitch : axes[4]) {
            for (const double yaw : axes[5]) {
              result.push_back({{x, y, z}, {roll, pitch, yaw}});
            }
          }
        }
      }
    }
  }
  return Result<std::vector<PickCandidateGrid::OffsetSample>>::Ok(std::move(result));
}

[[nodiscard]] double shape_top_z(const SceneObjectSnapshotDTO& object,
                                 const Pose3DTO& object_pose) {
  double top = object_pose.position_m.z_m;
  for (const auto& shape : object.collision_shapes) {
    top = std::max(top, object_pose.position_m.z_m +
                            std::visit(
                                [](const auto& value) {
                                  using Shape = std::remove_cvref_t<decltype(value)>;
                                  if constexpr (std::is_same_v<Shape, BoxShapeDTO>) {
                                    return value.dimensions_m.z_m / 2.0;
                                  } else if constexpr (std::is_same_v<Shape, SphereShapeDTO>) {
                                    return value.radius_m;
                                  } else if constexpr (std::is_same_v<Shape, CylinderShapeDTO>) {
                                    return value.height_m / 2.0;
                                  } else {
                                    return 0.0;
                                  }
                                },
                                shape));
  }
  return top;
}

[[nodiscard]] bool clamp_minimum_z(Pose3DTO& pose, const std::optional<double> minimum) {
  if (!minimum.has_value() || pose.position_m.z_m >= *minimum) {
    return false;
  }
  pose.position_m.z_m = *minimum;
  return true;
}

[[nodiscard]] double trajectory_duration(const MotionPreviewResultDTO& value) {
  return value.trajectory.waypoints.empty() ? 0.0
                                            : value.trajectory.waypoints.back().time_from_start_s;
}

[[nodiscard]] bool start_collision(const ErrorDTO& error) {
  const auto found = error.details.find("start_in_collision");
  if (found != error.details.end() &&
      (found->second == "true" || found->second == "1" || found->second == "yes")) {
    return true;
  }
  const auto failure_stage = error.details.find("failure_stage");
  return failure_stage != error.details.end() && failure_stage->second == "start_state";
}

void set_absolute_pose(ExecuteRequestDTO& request, const Pose3DTO& pose) {
  request.target_mode = MotionTargetModeDTO::kAbsolute;
  request.target_reference.reset();
  request.offset_reference.reset();
  request.motion = PoseGoalDTO{pose};
  request.cache_mode = CacheMode::kDisable;
}

void set_absolute_path(ExecuteRequestDTO& request, const CartesianPathDTO& path) {
  request.target_mode = MotionTargetModeDTO::kAbsolute;
  request.target_reference.reset();
  request.offset_reference.reset();
  request.motion = path;
  request.cache_mode = CacheMode::kDisable;
}

[[nodiscard]] PickCandidateStageReportDTO successful_stage(const PickCandidateStageDTO stage,
                                                           const MotionPreviewResultDTO& preview) {
  return {stage, true, std::nullopt, trajectory_duration(preview),
          preview.trajectory.waypoints.size()};
}

[[nodiscard]] PickCandidateStageReportDTO failed_stage(const PickCandidateStageDTO stage,
                                                       ErrorDTO error) {
  return {stage, false, std::move(error), 0.0, 0U};
}

[[nodiscard]] Result<MotionSpecDTO> portable_motion_for_sequence(
    const ResolvedMotionSpecDTO& motion) {
  if (const auto* value = std::get_if<PoseGoalDTO>(&motion)) {
    return Result<MotionSpecDTO>::Ok(*value);
  }
  if (const auto* value = std::get_if<NamedTargetDTO>(&motion)) {
    return Result<MotionSpecDTO>::Ok(*value);
  }
  if (const auto* value = std::get_if<CartesianPathDTO>(&motion)) {
    return Result<MotionSpecDTO>::Ok(*value);
  }
  return candidate_error<MotionSpecDTO>(
      "place_sequence_motion_not_portable",
      "place sequencing cannot reconstruct a portable joint goal from "
      "adapter-private resolved resource identities");
}

}  // namespace

PickCandidateGrid::PickCandidateGrid(SceneObjectSnapshotDTO selected_object,
                                     Pose3DTO object_pose_in_motion_base,
                                     PickCandidateSynthesisConfigDTO config,
                                     CartesianPathDTO descend_path_template,
                                     CartesianPathDTO retreat_path_template,
                                     std::vector<OffsetSample> pick_offsets,
                                     std::vector<OffsetSample> approach_offsets)
    : selected_object_(std::move(selected_object)),
      object_pose_in_motion_base_(std::move(object_pose_in_motion_base)),
      config_(std::move(config)),
      descend_path_template_(std::move(descend_path_template)),
      retreat_path_template_(std::move(retreat_path_template)),
      pick_offsets_(std::move(pick_offsets)),
      approach_offsets_(std::move(approach_offsets)) {}

Result<PickCandidateGrid> PickCandidateGrid::Create(SceneObjectSnapshotDTO selected_object,
                                                    Pose3DTO object_pose_in_motion_base,
                                                    PickCandidateSynthesisConfigDTO config,
                                                    CartesianPathDTO descend_path_template,
                                                    CartesianPathDTO retreat_path_template) {
  if (selected_object.scene_resource_id.empty() || selected_object.collision_shapes.empty() ||
      !finite_pose(object_pose_in_motion_base) || config.maximum_candidate_count == 0U ||
      config.maximum_candidate_count > 1'000'000U ||
      (config.min_tcp_z_m.has_value() &&
       (!std::isfinite(*config.min_tcp_z_m) || *config.min_tcp_z_m < 0.0))) {
    return candidate_error<PickCandidateGrid>(
        "pick_candidate_input_invalid",
        "pick candidate object, pose, minimum height, or bound is invalid");
  }
  auto object_orientation = normalized(object_pose_in_motion_base.orientation);
  auto pick_basis = normalized(config.pick_offset_basis.resolved_orientation_in_base);
  auto approach_basis = normalized(config.approach_offset_basis.resolved_orientation_in_base);
  if (!object_orientation.ok() || !pick_basis.ok() || !approach_basis.ok()) {
    return candidate_error<PickCandidateGrid>(
        "pick_candidate_orientation_invalid",
        "pick candidate object or offset-basis orientation is invalid");
  }
  object_pose_in_motion_base.orientation = object_orientation.value();
  config.pick_offset_basis.resolved_orientation_in_base = pick_basis.value();
  config.approach_offset_basis.resolved_orientation_in_base = approach_basis.value();

  auto pick_offsets = offset_samples(config.pick_offset_envelope, config.translation_step_m,
                                     config.rotation_step_deg, !config.force_vertical_pick,
                                     config.maximum_candidate_count);
  if (!pick_offsets.ok()) {
    return Result<PickCandidateGrid>::Err(pick_offsets.error());
  }
  auto approach_offsets = offset_samples(config.approach_offset_envelope, config.translation_step_m,
                                         config.rotation_step_deg, !config.force_vertical_pick,
                                         config.maximum_candidate_count);
  if (!approach_offsets.ok()) {
    return Result<PickCandidateGrid>::Err(approach_offsets.error());
  }
  if (pick_offsets.value().empty() || approach_offsets.value().empty() ||
      pick_offsets.value().size() >
          config.maximum_candidate_count / approach_offsets.value().size()) {
    return candidate_error<PickCandidateGrid>(
        "pick_candidate_count_exceeded", "pick candidate grid exceeds its declared total bound");
  }
  return Result<PickCandidateGrid>::Ok(PickCandidateGrid(
      std::move(selected_object), std::move(object_pose_in_motion_base), std::move(config),
      std::move(descend_path_template), std::move(retreat_path_template),
      std::move(pick_offsets.value()), std::move(approach_offsets.value())));
}

std::size_t PickCandidateGrid::size() const noexcept {
  return pick_offsets_.size() * approach_offsets_.size();
}

Result<PickPoseCandidateDTO> PickCandidateGrid::candidate(
    const std::size_t zero_based_index) const {
  if (zero_based_index >= size()) {
    return candidate_error<PickPoseCandidateDTO>(
        "pick_candidate_index_out_of_range",
        "pick candidate index is outside the deterministic grid");
  }
  const auto& pick_offset = pick_offsets_[zero_based_index / approach_offsets_.size()];
  const auto& approach_offset = approach_offsets_[zero_based_index % approach_offsets_.size()];

  QuaternionDTO pick_orientation;
  std::array<double, 3U> pick_rpy{};
  if (config_.force_vertical_pick) {
    auto vertical = vertical_down_orientation(object_pose_in_motion_base_.orientation);
    if (!vertical.ok()) {
      return Result<PickPoseCandidateDTO>::Err(vertical.error());
    }
    pick_orientation = vertical.value();
  } else {
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      pick_rpy[axis] =
          config_.base_tool_orientation_rpy_deg[axis] + pick_offset.orientation_rpy_deg[axis];
    }
    pick_orientation = rpy_degrees_to_quaternion(pick_rpy);
  }
  const auto pick_translation =
      rotate_vector(config_.pick_offset_basis.use_candidate_tcp_orientation
                        ? pick_orientation
                        : config_.pick_offset_basis.resolved_orientation_in_base,
                    pick_offset.position);
  PickPoseCandidateDTO result;
  result.candidate_index = zero_based_index + 1U;
  result.pick_pose = {
      {object_pose_in_motion_base_.position_m.x_m + pick_translation[0],
       object_pose_in_motion_base_.position_m.y_m + pick_translation[1],
       shape_top_z(selected_object_, object_pose_in_motion_base_) + pick_translation[2]},
      pick_orientation};
  result.min_tcp_z_applied = clamp_minimum_z(result.pick_pose, config_.min_tcp_z_m);

  const auto approach_translation =
      rotate_vector(config_.approach_offset_basis.use_candidate_tcp_orientation
                        ? result.pick_pose.orientation
                        : config_.approach_offset_basis.resolved_orientation_in_base,
                    approach_offset.position);
  result.approach_pose.position_m = {result.pick_pose.position_m.x_m + approach_translation[0],
                                     result.pick_pose.position_m.y_m + approach_translation[1],
                                     result.pick_pose.position_m.z_m + approach_translation[2]};
  if (config_.force_vertical_pick) {
    result.approach_pose.orientation = result.pick_pose.orientation;
  } else {
    std::array<double, 3U> approach_rpy{};
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      approach_rpy[axis] = pick_rpy[axis] + approach_offset.orientation_rpy_deg[axis];
    }
    result.approach_pose.orientation = rpy_degrees_to_quaternion(approach_rpy);
  }
  result.min_tcp_z_applied =
      clamp_minimum_z(result.approach_pose, config_.min_tcp_z_m) || result.min_tcp_z_applied;
  result.retreat_pose = result.approach_pose;
  result.descend_path = descend_path_template_;
  result.descend_path.waypoints = {result.approach_pose, result.pick_pose};
  result.retreat_path = retreat_path_template_;
  result.retreat_path.waypoints = {result.pick_pose, result.retreat_pose};
  return Result<PickPoseCandidateDTO>::Ok(std::move(result));
}

Result<PickCandidateSearchOutcomeDTO> AddressedPickCandidatePlanner::Search(
    PickCandidatePlanningRequestDTO request, const std::stop_token stop_token) {
  const auto reject = [](std::string reason, std::string message) {
    return candidate_error<PickCandidateSearchOutcomeDTO>(std::move(reason), std::move(message));
  };
  const auto& predecessor = request.predecessor_projection;
  const auto* attach =
      std::get_if<AttachCarriedObjectRequestDTO>(&request.attach_preview_request.request.operation);
  if (request.grid.size() == 0U || request.start_state.joint_names.empty() ||
      !predecessor.projection_binding.valid() || !predecessor.logical_scene ||
      !predecessor.logical_scene->locally_valid() || !predecessor.bundle.world ||
      !predecessor.bundle.policy.has_value() || attach == nullptr ||
      !request.carrier_frame_selector.valid() || request.candidate_lease_scope.empty() ||
      request.attach_preview_request.frame_resolution.has_value() ||
      request.attach_preview_request.request.expected_projection_revision !=
          predecessor.projection_binding.projection_revision ||
      attach->scene_resource_id != request.grid.selected_object_id() ||
      attach->controllable_component_id !=
          request.carrier_frame_selector.controllable_component_id ||
      attach->command_target_id != request.carrier_frame_selector.command_target_id ||
      attach->frame_role_id != request.carrier_frame_selector.frame_role_id ||
      request.approach_request.binding != request.descend_request.binding ||
      request.approach_request.binding != request.retreat_request.binding ||
      !request.descend_request.resolved_contact_grasp.has_value() ||
      request.descend_request.resolved_contact_grasp->selected_object_id !=
          request.grid.selected_object_id()) {
    return reject("pick_candidate_planning_request_invalid",
                  "pick candidate planning request is incomplete or crosses "
                  "binding, object, carrier, or projection identity");
  }

  request.approach_request.resolved_contact_grasp.reset();
  request.approach_request.contact_policy_id.clear();
  request.retreat_request.resolved_contact_grasp.reset();
  request.retreat_request.contact_policy_id.clear();
  PickCandidateSearchOutcomeDTO outcome;
  outcome.reports.reserve(request.grid.size());
  std::optional<PlannedPickCandidateDTO> best;

  for (std::size_t index = 0U; index < request.grid.size(); ++index) {
    if (stop_token.stop_requested()) {
      return Result<PickCandidateSearchOutcomeDTO>::Err(
          ErrorDTO{std::string(to_string(ErrorCode::kExecutionAborted)),
                   "Pick candidate search was cancelled",
                   {{"reason", "pick_candidate_search_cancelled"}}});
    }
    auto synthesized = request.grid.candidate(index);
    if (!synthesized.ok()) {
      return Result<PickCandidateSearchOutcomeDTO>::Err(synthesized.error());
    }
    ++outcome.attempted_candidate_count;
    PickCandidateReportDTO report{synthesized.value().candidate_index, {}};
    report.stages.reserve(4U);
    const auto attempt = static_cast<std::uint32_t>(index);

    auto approach_request = request.approach_request;
    set_absolute_pose(approach_request, synthesized.value().approach_pose);
    auto approach = motion_.preview_motion(std::move(approach_request), predecessor,
                                           request.start_state, attempt, stop_token);
    if (!approach.ok()) {
      const bool invariant = start_collision(approach.error());
      report.stages.push_back(failed_stage(PickCandidateStageDTO::kApproach, approach.error()));
      outcome.reports.push_back(std::move(report));
      if (invariant) {
        outcome.stopped_on_invariant_failure = true;
        break;
      }
      continue;
    }
    report.stages.push_back(successful_stage(PickCandidateStageDTO::kApproach, approach.value()));

    auto descend_request = request.descend_request;
    set_absolute_path(descend_request, synthesized.value().descend_path);
    auto descend = motion_.preview_motion(std::move(descend_request), predecessor,
                                          approach.value().final_state, attempt, stop_token);
    if (!descend.ok()) {
      report.stages.push_back(failed_stage(PickCandidateStageDTO::kDescend, descend.error()));
      outcome.reports.push_back(std::move(report));
      continue;
    }
    report.stages.push_back(successful_stage(PickCandidateStageDTO::kDescend, descend.value()));

    auto frame = projection_.ResolveCandidateSceneFrame(CandidateSceneFrameResolutionQueryDTO{
        request.carrier_frame_selector, predecessor.projection_binding,
        descend.value().final_controlled_state});
    if (!frame.ok()) {
      report.stages.push_back(failed_stage(PickCandidateStageDTO::kCandidateAttach, frame.error()));
      outcome.reports.push_back(std::move(report));
      outcome.stopped_on_invariant_failure = true;
      return Result<PickCandidateSearchOutcomeDTO>::Ok(std::move(outcome));
    }
    auto candidate_request = request.attach_preview_request;
    candidate_request.frame_resolution = std::move(frame.value());
    auto attached = scene_.PreviewResolvedSceneCandidate(candidate_request);
    if (!attached.ok()) {
      report.stages.push_back(
          failed_stage(PickCandidateStageDTO::kCandidateAttach, attached.error()));
      outcome.reports.push_back(std::move(report));
      outcome.stopped_on_invariant_failure = true;
      return Result<PickCandidateSearchOutcomeDTO>::Ok(std::move(outcome));
    }
    auto candidate_projection = projection_.ProjectSceneCandidate(
        attached.value(), predecessor.projection_binding,
        ProjectionLeaseId(request.candidate_lease_scope + "." +
                          std::to_string(synthesized.value().candidate_index)));
    if (!candidate_projection.ok()) {
      report.stages.push_back(
          failed_stage(PickCandidateStageDTO::kCandidateAttach, candidate_projection.error()));
      outcome.reports.push_back(std::move(report));
      outcome.stopped_on_invariant_failure = true;
      return Result<PickCandidateSearchOutcomeDTO>::Ok(std::move(outcome));
    }
    report.stages.push_back({PickCandidateStageDTO::kCandidateAttach, true, std::nullopt, 0.0, 0U});

    auto retreat_request = request.retreat_request;
    set_absolute_path(retreat_request, synthesized.value().retreat_path);
    auto retreat =
        motion_.preview_motion(std::move(retreat_request), std::move(candidate_projection.value()),
                               descend.value().final_state, attempt, stop_token);
    if (!retreat.ok()) {
      report.stages.push_back(failed_stage(PickCandidateStageDTO::kRetreat, retreat.error()));
      outcome.reports.push_back(std::move(report));
      continue;
    }
    report.stages.push_back(successful_stage(PickCandidateStageDTO::kRetreat, retreat.value()));

    const double score = trajectory_duration(approach.value()) +
                         trajectory_duration(descend.value()) +
                         trajectory_duration(retreat.value()) +
                         0.01 * static_cast<double>(approach.value().trajectory.waypoints.size() +
                                                    descend.value().trajectory.waypoints.size() +
                                                    retreat.value().trajectory.waypoints.size());
    PlannedPickCandidateDTO planned{std::move(synthesized.value()),
                                    std::move(approach.value()),
                                    std::move(descend.value()),
                                    std::move(candidate_request.request),
                                    std::move(attached.value()),
                                    std::move(retreat.value()),
                                    score};
    outcome.reports.push_back(std::move(report));
    if (request.grid.search_strategy() == PickCandidateSearchStrategyDTO::kFirstFeasible) {
      outcome.selected = std::move(planned);
      return Result<PickCandidateSearchOutcomeDTO>::Ok(std::move(outcome));
    }
    if (!best.has_value() || planned.planning_score < best->planning_score) {
      best = std::move(planned);
    }
  }
  outcome.selected = std::move(best);
  return Result<PickCandidateSearchOutcomeDTO>::Ok(std::move(outcome));
}

Result<PlacePlanningOutcomeDTO> AddressedPlacePlanner::Plan(PlacePlanningRequestDTO request,
                                                            const std::stop_token stop_token) {
  const auto reject = [](std::string reason, std::string message) {
    return candidate_error<PlacePlanningOutcomeDTO>(std::move(reason), std::move(message));
  };
  const auto& predecessor = request.predecessor_projection;
  const auto* requested_detach =
      std::get_if<DetachCarriedObjectRequestDTO>(&request.detach_preview_request.request.operation);
  if (request.selected_object_id.empty() || request.start_state.joint_names.empty() ||
      !predecessor.projection_binding.valid() || !predecessor.logical_scene ||
      !predecessor.logical_scene->locally_valid() || !predecessor.bundle.world ||
      !predecessor.bundle.policy.has_value() || requested_detach == nullptr ||
      !request.carrier_frame_selector.valid() || request.candidate_lease_scope.empty() ||
      request.detach_preview_request.frame_resolution.has_value() ||
      request.detach_preview_request.request.expected_projection_revision !=
          predecessor.projection_binding.projection_revision ||
      request.approach_request.binding != request.descend_request.binding ||
      request.approach_request.binding != request.retreat_request.binding ||
      request.approach_request.resolved_contact_grasp.has_value() ||
      request.descend_request.resolved_contact_grasp.has_value() ||
      request.retreat_request.resolved_contact_grasp.has_value() ||
      !request.approach_request.contact_policy_id.empty() ||
      !request.descend_request.contact_policy_id.empty() ||
      !request.retreat_request.contact_policy_id.empty()) {
    return reject("place_planning_request_invalid",
                  "place planning request is incomplete or crosses binding, "
                  "carrier, contact-policy, or projection identity");
  }

  const auto object =
      std::lower_bound(predecessor.logical_scene->objects.begin(),
                       predecessor.logical_scene->objects.end(), request.selected_object_id,
                       [](const SceneObjectSnapshotDTO& candidate, const SceneResourceId& id) {
                         return candidate.scene_resource_id < id;
                       });
  const auto attachment = std::lower_bound(
      predecessor.logical_scene->carried_object_attachments.begin(),
      predecessor.logical_scene->carried_object_attachments.end(),
      requested_detach->carried_object_attachment_id,
      [](const CarriedObjectAttachmentDTO& candidate, const CarriedObjectAttachmentId& id) {
        return candidate.carried_object_attachment_id < id;
      });
  if (object == predecessor.logical_scene->objects.end() ||
      object->scene_resource_id != request.selected_object_id ||
      !std::holds_alternative<CarriedSceneObjectPlacementDTO>(object->placement) ||
      attachment == predecessor.logical_scene->carried_object_attachments.end() ||
      attachment->carried_object_attachment_id != requested_detach->carried_object_attachment_id ||
      attachment->carried_object_attachment_revision !=
          requested_detach->expected_carried_object_attachment_revision ||
      attachment->scene_resource_id != request.selected_object_id ||
      attachment->controllable_component_id !=
          request.carrier_frame_selector.controllable_component_id ||
      attachment->command_target_id != request.carrier_frame_selector.command_target_id ||
      !std::holds_alternative<ComponentSceneFrameRefDTO>(attachment->carrier_frame_ref)) {
    return reject("place_selected_attachment_mismatch",
                  "place requires the selected object to be carried by the "
                  "exact bound carrier and attachment revision");
  }
  const auto& carrier_frame = std::get<ComponentSceneFrameRefDTO>(attachment->carrier_frame_ref);
  if (carrier_frame.controllable_component_id !=
          request.carrier_frame_selector.controllable_component_id ||
      carrier_frame.frame_role_id != request.carrier_frame_selector.frame_role_id) {
    return reject("place_selected_carrier_frame_mismatch",
                  "place attachment carrier differs from the exact selected "
                  "carrier frame");
  }

  PickCandidateReportDTO report{1U, {}};
  report.stages.reserve(4U);
  auto approach_motion = portable_motion_for_sequence(request.approach_request.motion);
  auto descend_motion = portable_motion_for_sequence(request.descend_request.motion);
  auto retreat_motion = portable_motion_for_sequence(request.retreat_request.motion);
  if (!approach_motion.ok()) {
    return Result<PlacePlanningOutcomeDTO>::Err(approach_motion.error());
  }
  if (!descend_motion.ok()) {
    return Result<PlacePlanningOutcomeDTO>::Err(descend_motion.error());
  }
  if (!retreat_motion.ok()) {
    return Result<PlacePlanningOutcomeDTO>::Err(retreat_motion.error());
  }
  auto approach = motion_.preview_motion(std::move(request.approach_request), predecessor,
                                         request.start_state, 0U, stop_token);
  if (!approach.ok()) {
    report.stages.push_back(failed_stage(PickCandidateStageDTO::kApproach, approach.error()));
    return Result<PlacePlanningOutcomeDTO>::Ok(PlacePlanningOutcomeDTO{
        std::nullopt, std::move(report), start_collision(approach.error())});
  }
  report.stages.push_back(successful_stage(PickCandidateStageDTO::kApproach, approach.value()));

  auto descend = motion_.preview_motion(std::move(request.descend_request), predecessor,
                                        approach.value().final_state, 0U, stop_token);
  if (!descend.ok()) {
    report.stages.push_back(failed_stage(PickCandidateStageDTO::kDescend, descend.error()));
    return Result<PlacePlanningOutcomeDTO>::Ok(
        PlacePlanningOutcomeDTO{std::nullopt, std::move(report), false});
  }
  report.stages.push_back(successful_stage(PickCandidateStageDTO::kDescend, descend.value()));

  auto frame = projection_.ResolveCandidateSceneFrame(CandidateSceneFrameResolutionQueryDTO{
      request.carrier_frame_selector, predecessor.projection_binding,
      descend.value().final_controlled_state});
  if (!frame.ok()) {
    report.stages.push_back(failed_stage(PickCandidateStageDTO::kCandidateDetach, frame.error()));
    return Result<PlacePlanningOutcomeDTO>::Ok(
        PlacePlanningOutcomeDTO{std::nullopt, std::move(report), true});
  }
  auto candidate_request = request.detach_preview_request;
  candidate_request.frame_resolution = frame.value();
  auto* candidate_detach =
      std::get_if<DetachCarriedObjectRequestDTO>(&candidate_request.request.operation);
  candidate_detach->pose = attachment->pose_in_carrier_frame;
  candidate_detach->frame_ref = attachment->carrier_frame_ref;
  auto detached = scene_.PreviewResolvedSceneCandidate(candidate_request);
  if (!detached.ok()) {
    report.stages.push_back(
        failed_stage(PickCandidateStageDTO::kCandidateDetach, detached.error()));
    return Result<PlacePlanningOutcomeDTO>::Ok(
        PlacePlanningOutcomeDTO{std::nullopt, std::move(report), true});
  }
  auto candidate_projection =
      projection_.ProjectSceneCandidate(detached.value(), predecessor.projection_binding,
                                        ProjectionLeaseId(request.candidate_lease_scope + ".1"));
  if (!candidate_projection.ok()) {
    report.stages.push_back(
        failed_stage(PickCandidateStageDTO::kCandidateDetach, candidate_projection.error()));
    return Result<PlacePlanningOutcomeDTO>::Ok(
        PlacePlanningOutcomeDTO{std::nullopt, std::move(report), true});
  }
  report.stages.push_back({PickCandidateStageDTO::kCandidateDetach, true, std::nullopt, 0.0, 0U});

  auto retreat = motion_.preview_motion(std::move(request.retreat_request),
                                        std::move(candidate_projection.value()),
                                        descend.value().final_state, 0U, stop_token);
  if (!retreat.ok()) {
    report.stages.push_back(failed_stage(PickCandidateStageDTO::kRetreat, retreat.error()));
    return Result<PlacePlanningOutcomeDTO>::Ok(
        PlacePlanningOutcomeDTO{std::nullopt, std::move(report), false});
  }
  report.stages.push_back(successful_stage(PickCandidateStageDTO::kRetreat, retreat.value()));
  auto selected =
      PlannedPlaceDTO{std::move(approach_motion.value()), std::move(descend_motion.value()),
                      std::move(retreat_motion.value()),  std::move(approach.value()),
                      std::move(descend.value()),         std::move(candidate_request.request),
                      std::move(detached.value()),        std::move(retreat.value())};
  return Result<PlacePlanningOutcomeDTO>::Ok(PlacePlanningOutcomeDTO{
      std::optional<PlannedPlaceDTO>{std::move(selected)}, std::move(report), false});
}

}  // namespace manyforge::core
