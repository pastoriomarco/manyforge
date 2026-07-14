#include <manyforge_roboplan/roboplan_cartesian_path_adapter.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Eigen/Geometry>
#include <roboplan_cartesian_planning/cartesian_path_planner.hpp>

namespace manyforge::roboplan_backend {
namespace {

constexpr std::size_t kMaxInputWaypoints = 4096;
constexpr std::size_t kMaxDensifiedWaypoints = 100000;
constexpr std::size_t kMaxInterpolantPathSamples = 500000;
constexpr std::size_t kPlanningControlCheckStride = 64;
constexpr double kNumericalLimitTolerance = 1.0 + 1e-6;
constexpr double kRetimeHeadroom = 1.001;
constexpr double kUnsampledFidelityBudgetFraction = 0.25;

enum class PathParameterMetric {
  kEuclideanTolerance,
  kMaximumDuration,
};

struct DensifiedCartesianPath {
  std::vector<Eigen::Matrix4d> poses;
  std::vector<std::size_t> source_waypoint_dense_indices;
};

core::ErrorDTO invalid_cartesian_request(
    std::string message, std::string reason,
    std::map<std::string, std::string> details = {}) {
  details["reason"] = std::move(reason);
  details["failure_stage"] = "cartesian_request_validation";
  return core::ErrorDTO{core::kErrorInvalidRequest, std::move(message),
                        std::move(details)};
}

bool finite_positive(double value) {
  // Subnormal command values are not portable across optimized numerical
  // backends because some runtimes flush them to zero. Reject them at the
  // neutral boundary instead of allowing backend-dependent semantics.
  return std::isfinite(value) &&
         value >= std::numeric_limits<double>::min();
}

std::optional<core::ErrorDTO> planning_control_error(
    const CartesianPlanningControl& control, std::string failure_stage) {
  const bool cancelled =
      control.cancellation_requested && control.cancellation_requested();
  if (cancelled) {
    return core::ErrorDTO{
        core::kErrorPlanningFailed,
        "Cartesian planning cancelled by interrupt",
        {{"reason", "planning_cancelled_by_interrupt"},
         {"failure_stage", std::move(failure_stage)}}};
  }
  const bool timed_out = control.deadline_expired && control.deadline_expired();
  if (timed_out) {
    return core::ErrorDTO{
        core::kErrorPlanningTimeout,
        "Cartesian planning timed out",
        {{"reason", "planning_timeout"},
         {"failure_stage", std::move(failure_stage)}}};
  }
  return std::nullopt;
}

bool contains_text(std::string_view value, std::string_view fragment) {
  return value.find(fragment) != std::string_view::npos;
}

core::ErrorDTO cartesian_incomplete_error(
    std::string message, std::string reason, double fraction_completed,
    std::optional<std::size_t> first_failing_waypoint_index,
    std::map<std::string, std::string> details = {}) {
  details["reason"] = std::move(reason);
  details["fraction_completed"] =
      std::to_string(std::clamp(fraction_completed, 0.0, 1.0));
  details["first_failing_waypoint_index"] =
      first_failing_waypoint_index.has_value()
          ? std::to_string(*first_failing_waypoint_index)
          : "-1";
  return core::ErrorDTO{core::kErrorCartesianIncomplete, std::move(message),
                        std::move(details)};
}

Eigen::Quaterniond normalized_quaternion(const core::QuaternionDTO& value) {
  Eigen::Quaterniond quaternion(value.w, value.x, value.y, value.z);
  if (!quaternion.coeffs().allFinite()) {
    throw std::invalid_argument(
        "Cartesian waypoint orientation must be a finite, non-zero quaternion");
  }
  // Normalize after scaling by the largest component. Calling norm() first can
  // overflow for a perfectly finite, scale-equivalent quaternion near DBL_MAX
  // and silently turn the orientation into zeros when divided by infinity.
  const double scale = quaternion.coeffs().cwiseAbs().maxCoeff();
  if (!std::isfinite(scale) || scale <= 0.0) {
    throw std::invalid_argument(
        "Cartesian waypoint orientation must be a finite, non-zero quaternion");
  }
  quaternion.coeffs() /= scale;
  const double norm = quaternion.norm();
  if (!std::isfinite(norm) || norm <= 0.0) {
    throw std::invalid_argument(
        "Cartesian waypoint orientation could not be normalized");
  }
  quaternion.coeffs() /= norm;
  return quaternion;
}

Eigen::Matrix4d pose_matrix(const core::Pose3DTO& pose) {
  if (!std::isfinite(pose.position_m.x_m) ||
      !std::isfinite(pose.position_m.y_m) ||
      !std::isfinite(pose.position_m.z_m)) {
    throw std::invalid_argument(
        "Cartesian waypoint position must contain only finite values");
  }
  const auto quaternion = normalized_quaternion(pose.orientation);
  Eigen::Matrix4d result = Eigen::Matrix4d::Identity();
  result.block<3, 3>(0, 0) = quaternion.toRotationMatrix();
  result(0, 3) = pose.position_m.x_m;
  result(1, 3) = pose.position_m.y_m;
  result(2, 3) = pose.position_m.z_m;
  return result;
}

double rotation_distance(const Eigen::Matrix3d& lhs,
                         const Eigen::Matrix3d& rhs) {
  Eigen::Quaterniond relative(lhs.transpose() * rhs);
  relative.normalize();
  return 2.0 * std::acos(std::clamp(std::abs(relative.w()), 0.0, 1.0));
}

core::Result<DensifiedCartesianPath> densify_path(
    const core::CartesianPathDTO& path,
    const CartesianPlanningControl& control) {
  std::vector<Eigen::Matrix4d> source;
  source.reserve(path.waypoints.size());
  try {
    for (std::size_t index = 0; index < path.waypoints.size(); ++index) {
      if (index % kPlanningControlCheckStride == 0) {
        if (auto error = planning_control_error(
                control, "cartesian_path_densification");
            error.has_value()) {
          return core::Result<DensifiedCartesianPath>::Err(
              std::move(*error));
        }
      }
      source.push_back(pose_matrix(path.waypoints[index]));
    }
  } catch (const std::invalid_argument& error) {
    return core::Result<DensifiedCartesianPath>::Err(
        invalid_cartesian_request(error.what(), "cartesian_waypoint_not_finite"));
  }

  DensifiedCartesianPath result;
  result.poses.reserve(source.size());
  result.source_waypoint_dense_indices.reserve(source.size());
  result.poses.push_back(source.front());
  result.source_waypoint_dense_indices.push_back(0);
  for (std::size_t segment = 1; segment < source.size(); ++segment) {
    if (auto error = planning_control_error(
            control, "cartesian_path_densification");
        error.has_value()) {
      return core::Result<DensifiedCartesianPath>::Err(
          std::move(*error));
    }

    const auto& from = source[segment - 1];
    const auto& to = source[segment];
    const double translation =
        (to.block<3, 1>(0, 3) - from.block<3, 1>(0, 3)).norm();
    const double rotation =
        rotation_distance(from.block<3, 3>(0, 0), to.block<3, 3>(0, 0));
    if (!std::isfinite(translation) || !std::isfinite(rotation)) {
      return core::Result<DensifiedCartesianPath>::Err(
          invalid_cartesian_request(
              "Cartesian segment length is not finite",
              "cartesian_segment_not_finite",
              {{"segment_index", std::to_string(segment - 1)}}));
    }
    const double required_subdivisions = std::max(
        1.0, std::ceil(std::max(translation / path.max_translation_step_m,
                                rotation / path.max_rotation_step_rad)));
    const std::size_t remaining_budget =
        kMaxDensifiedWaypoints - result.poses.size();
    // Converting an out-of-range floating value to size_t is undefined. Reject
    // adversarially small step limits while the required count is still a
    // floating value, before any allocation or integer conversion.
    if (!std::isfinite(required_subdivisions) ||
        required_subdivisions > static_cast<double>(remaining_budget)) {
      return core::Result<DensifiedCartesianPath>::Err(
          invalid_cartesian_request(
              "Cartesian path exceeds the bounded densified waypoint budget",
              "cartesian_path_too_dense",
              {{"max_densified_waypoints",
                std::to_string(kMaxDensifiedWaypoints)},
               {"segment_index", std::to_string(segment - 1)}}));
    }
    const auto subdivisions =
        static_cast<std::size_t>(required_subdivisions);

    Eigen::Quaterniond from_rotation(from.block<3, 3>(0, 0));
    Eigen::Quaterniond to_rotation(to.block<3, 3>(0, 0));
    if (from_rotation.dot(to_rotation) < 0.0) {
      to_rotation.coeffs() *= -1.0;
    }
    for (std::size_t step = 1; step <= subdivisions; ++step) {
      if (step % kPlanningControlCheckStride == 0) {
        if (auto error = planning_control_error(
                control, "cartesian_path_densification");
            error.has_value()) {
          return core::Result<DensifiedCartesianPath>::Err(
              std::move(*error));
        }
      }
      const double alpha =
          static_cast<double>(step) / static_cast<double>(subdivisions);
      Eigen::Matrix4d waypoint = Eigen::Matrix4d::Identity();
      waypoint.block<3, 3>(0, 0) =
          from_rotation.slerp(alpha, to_rotation).normalized().toRotationMatrix();
      waypoint.block<3, 1>(0, 3) =
          (1.0 - alpha) * from.block<3, 1>(0, 3) +
          alpha * to.block<3, 1>(0, 3);
      result.poses.push_back(std::move(waypoint));
    }
    result.source_waypoint_dense_indices.push_back(result.poses.size() - 1);
  }
  return core::Result<DensifiedCartesianPath>::Ok(std::move(result));
}

void set_path_parameterization_error(
    std::optional<core::ErrorDTO>* error,
    std::string reason = "cartesian_resource_budget_exceeded") {
  if (error == nullptr || error->has_value()) {
    return;
  }
  *error = cartesian_incomplete_error(
      "Cartesian path cannot be represented within the bounded fidelity "
      "parameter range",
      std::move(reason), 0.0, 0U,
      {{"failure_stage", "cartesian_output_validation"},
       {"resource", "path_parameter_dynamic_range"}});
}

std::optional<std::vector<double>> normalized_path_parameter(
    std::span<const Eigen::Matrix4d> poses, double translation_tolerance_m,
    double rotation_tolerance_rad,
    PathParameterMetric metric,
    const CartesianPlanningControl* control = nullptr,
    std::optional<core::ErrorDTO>* error = nullptr) {
  if (poses.empty() || !finite_positive(translation_tolerance_m) ||
      !finite_positive(rotation_tolerance_rad)) {
    set_path_parameterization_error(error,
                                    "cartesian_path_parameter_invalid");
    return std::nullopt;
  }

  std::vector<double> segment_log_weights(poses.size() - 1,
                                          -std::numeric_limits<double>::infinity());
  double maximum_log_weight = -std::numeric_limits<double>::infinity();
  const double translation_log_scale = std::log(translation_tolerance_m);
  const double rotation_log_scale = std::log(rotation_tolerance_rad);
  for (std::size_t index = 1; index < poses.size(); ++index) {
    if (control != nullptr && index % kPlanningControlCheckStride == 0) {
      if (auto control_failure = planning_control_error(
              *control, "cartesian_path_fidelity_validation");
          control_failure.has_value()) {
        if (error != nullptr) {
          *error = std::move(*control_failure);
        }
        return std::nullopt;
      }
    }
    const double translation =
        (poses[index].block<3, 1>(0, 3) -
         poses[index - 1].block<3, 1>(0, 3))
            .norm();
    const double rotation = rotation_distance(
        poses[index - 1].block<3, 3>(0, 0),
        poses[index].block<3, 3>(0, 0));
    if (!std::isfinite(translation) || translation < 0.0 ||
        !std::isfinite(rotation) || rotation < 0.0) {
      set_path_parameterization_error(error,
                                      "cartesian_path_parameter_invalid");
      return std::nullopt;
    }
    if (translation == 0.0 && rotation == 0.0) {
      continue;
    }

    const double translation_log =
        translation > 0.0
            ? std::log(translation) - translation_log_scale
            : -std::numeric_limits<double>::infinity();
    const double rotation_log =
        rotation > 0.0 ? std::log(rotation) - rotation_log_scale
                       : -std::numeric_limits<double>::infinity();
    const double dominant_log = std::max(translation_log, rotation_log);
    if (!std::isfinite(dominant_log)) {
      set_path_parameterization_error(error);
      return std::nullopt;
    }

    double segment_log_weight = dominant_log;
    if (metric == PathParameterMetric::kEuclideanTolerance) {
      const double translation_component =
          std::isfinite(translation_log)
              ? std::exp(translation_log - dominant_log)
              : 0.0;
      const double rotation_component =
          std::isfinite(rotation_log)
              ? std::exp(rotation_log - dominant_log)
              : 0.0;
      segment_log_weight +=
          std::log(std::hypot(translation_component, rotation_component));
    }
    if (!std::isfinite(segment_log_weight)) {
      set_path_parameterization_error(error);
      return std::nullopt;
    }
    segment_log_weights[index - 1] = segment_log_weight;
    maximum_log_weight = std::max(maximum_log_weight, segment_log_weight);
  }

  std::vector<double> parameter(poses.size(), 0.0);
  if (!std::isfinite(maximum_log_weight)) {
    return parameter;
  }

  std::vector<long double> cumulative(poses.size(), 0.0L);
  for (std::size_t index = 1; index < poses.size(); ++index) {
    const double segment_log_weight = segment_log_weights[index - 1];
    if (!std::isfinite(segment_log_weight)) {
      cumulative[index] = cumulative[index - 1];
      continue;
    }
    const double scaled_weight =
        std::exp(segment_log_weight - maximum_log_weight);
    if (!std::isfinite(scaled_weight) || scaled_weight <= 0.0) {
      set_path_parameterization_error(error);
      return std::nullopt;
    }
    cumulative[index] =
        cumulative[index - 1] + static_cast<long double>(scaled_weight);
  }
  const long double total = cumulative.back();
  if (!std::isfinite(total) || total <= 0.0L) {
    set_path_parameterization_error(error);
    return std::nullopt;
  }
  for (std::size_t index = 1; index < parameter.size(); ++index) {
    parameter[index] = static_cast<double>(cumulative[index] / total);
    const bool positive_segment =
        std::isfinite(segment_log_weights[index - 1]);
    if (!std::isfinite(parameter[index]) || parameter[index] < 0.0 ||
        parameter[index] > 1.0 ||
        (positive_segment && parameter[index] <= parameter[index - 1]) ||
        (!positive_segment && parameter[index] != parameter[index - 1])) {
      set_path_parameterization_error(error);
      return std::nullopt;
    }
  }
  parameter.back() = 1.0;
  return parameter;
}

Eigen::Matrix4d interpolate_path_pose(
    std::span<const Eigen::Matrix4d> poses,
    std::span<const double> parameter, double fraction) {
  if (poses.size() == 1) {
    return poses.front();
  }
  const auto upper_match = std::upper_bound(
      parameter.begin(), parameter.end(), fraction);
  const std::size_t cursor =
      upper_match == parameter.begin()
          ? 0
          : static_cast<std::size_t>(
                std::distance(parameter.begin(), upper_match) - 1);
  if (cursor + 1 >= poses.size()) {
    return poses.back();
  }
  const double lower = parameter[cursor];
  const double upper = parameter[cursor + 1];
  if (upper <= lower) {
    return poses[cursor + 1];
  }
  const double alpha = std::clamp((fraction - lower) / (upper - lower),
                                  0.0, 1.0);
  Eigen::Matrix4d pose = Eigen::Matrix4d::Identity();
  pose.block<3, 1>(0, 3) =
      (1.0 - alpha) * poses[cursor].block<3, 1>(0, 3) +
      alpha * poses[cursor + 1].block<3, 1>(0, 3);
  Eigen::Quaterniond from(poses[cursor].block<3, 3>(0, 0));
  Eigen::Quaterniond to(poses[cursor + 1].block<3, 3>(0, 0));
  if (from.dot(to) < 0.0) {
    to.coeffs() *= -1.0;
  }
  pose.block<3, 3>(0, 0) =
      from.normalized().slerp(alpha, to.normalized()).toRotationMatrix();
  return pose;
}

std::vector<double> merged_path_breakpoints(
    std::span<const double> lhs, std::span<const double> rhs,
    const CartesianPlanningControl* control = nullptr,
    std::optional<core::ErrorDTO>* error = nullptr) {
  const auto valid_parameter = [](std::span<const double> values) {
    return !values.empty() &&
           std::is_sorted(values.begin(), values.end()) &&
           std::all_of(values.begin(), values.end(), [](double value) {
             return std::isfinite(value) && value >= 0.0 && value <= 1.0;
           });
  };
  if (!valid_parameter(lhs) || !valid_parameter(rhs)) {
    set_path_parameterization_error(error,
                                    "cartesian_path_parameter_invalid");
    return {};
  }

  std::vector<double> breakpoints;
  breakpoints.reserve(lhs.size() + rhs.size() + 1);
  breakpoints.push_back(0.0);
  std::size_t lhs_index = 0;
  std::size_t rhs_index = 0;
  std::size_t iteration = 0;
  while (lhs_index < lhs.size() || rhs_index < rhs.size()) {
    if (control != nullptr &&
        iteration++ % kPlanningControlCheckStride == 0) {
      if (auto control_failure = planning_control_error(
              *control, "cartesian_path_fidelity_validation");
          control_failure.has_value()) {
        if (error != nullptr) {
          *error = std::move(*control_failure);
        }
        return {};
      }
    }
    const double lhs_value = lhs_index < lhs.size()
                                 ? lhs[lhs_index]
                                 : std::numeric_limits<double>::infinity();
    const double rhs_value = rhs_index < rhs.size()
                                 ? rhs[rhs_index]
                                 : std::numeric_limits<double>::infinity();
    const double next = std::min(lhs_value, rhs_value);
    while (lhs_index < lhs.size() && lhs[lhs_index] <= next) {
      ++lhs_index;
    }
    while (rhs_index < rhs.size() && rhs[rhs_index] <= next) {
      ++rhs_index;
    }
    if (next > breakpoints.back()) {
      breakpoints.push_back(next);
    }
  }
  if (breakpoints.back() < 1.0) {
    breakpoints.push_back(1.0);
  }
  return breakpoints;
}

std::size_t source_waypoint_for_dense_sample(
    const DensifiedCartesianPath& path, std::size_t dense_sample) {
  const auto match = std::lower_bound(
      path.source_waypoint_dense_indices.begin(),
      path.source_waypoint_dense_indices.end(), dense_sample);
  if (match == path.source_waypoint_dense_indices.end()) {
    return path.source_waypoint_dense_indices.size() - 1;
  }
  return static_cast<std::size_t>(
      std::distance(path.source_waypoint_dense_indices.begin(), match));
}

std::size_t source_waypoint_for_reference_progress(
    const DensifiedCartesianPath& path, double fraction,
    double linear_speed, double angular_speed) {
  std::vector<Eigen::Matrix4d> source_poses;
  source_poses.reserve(path.source_waypoint_dense_indices.size());
  for (const auto dense_index : path.source_waypoint_dense_indices) {
    source_poses.push_back(path.poses[dense_index]);
  }
  const auto parameter = normalized_path_parameter(
      source_poses, linear_speed, angular_speed,
      PathParameterMetric::kMaximumDuration);
  if (!parameter.has_value()) {
    const double bounded_fraction = std::clamp(fraction, 0.0, 1.0);
    return std::min(
        static_cast<std::size_t>(std::ceil(
            bounded_fraction *
            static_cast<double>(source_poses.size() - 1))),
        source_poses.size() - 1);
  }
  const auto match = std::lower_bound(parameter->begin(), parameter->end(),
                                      std::clamp(fraction, 0.0, 1.0));
  return std::min(
      static_cast<std::size_t>(std::distance(parameter->begin(), match)),
      parameter->size() - 1);
}

std::optional<core::ErrorDTO> validate_cartesian_trajectory_output(
    const roboplan::JointTrajectory& trajectory,
    const std::vector<std::string>& expected_joint_names,
    const CartesianPlanningControl& control) {
  const std::size_t sample_count = trajectory.positions.size();
  if (trajectory.joint_names != expected_joint_names) {
    return core::ErrorDTO{
        core::kErrorPlanningFailed,
        "Cartesian planner returned a trajectory with a non-canonical joint "
        "order",
        {{"reason", "cartesian_trajectory_joint_order_invalid"},
         {"failure_stage", "cartesian_output_validation"}}};
  }
  if (sample_count < 2 || trajectory.times.size() != sample_count ||
      trajectory.velocities.size() != sample_count ||
      trajectory.accelerations.size() != sample_count) {
    return core::ErrorDTO{
        core::kErrorPlanningFailed,
        "Cartesian planner returned an incomplete trajectory shape",
        {{"reason", "cartesian_trajectory_shape_invalid"},
         {"failure_stage", "cartesian_output_validation"},
         {"sample_count", std::to_string(sample_count)}}};
  }

  double previous_time = -1.0;
  for (std::size_t sample = 0; sample < sample_count; ++sample) {
    if (sample % kPlanningControlCheckStride == 0) {
      if (auto error = planning_control_error(
              control, "cartesian_output_validation");
          error.has_value()) {
        return error;
      }
    }
    const bool dimensions_match =
        trajectory.positions[sample].size() ==
            static_cast<Eigen::Index>(expected_joint_names.size()) &&
        trajectory.velocities[sample].size() ==
            static_cast<Eigen::Index>(expected_joint_names.size()) &&
        trajectory.accelerations[sample].size() ==
            static_cast<Eigen::Index>(expected_joint_names.size());
    const bool values_are_finite =
        std::isfinite(trajectory.times[sample]) &&
        trajectory.positions[sample].allFinite() &&
        trajectory.velocities[sample].allFinite() &&
        trajectory.accelerations[sample].allFinite();
    const bool time_is_ordered =
        (sample == 0 ? trajectory.times[sample] == 0.0
                     : trajectory.times[sample] > previous_time);
    if (!dimensions_match || !values_are_finite || !time_is_ordered) {
      return core::ErrorDTO{
          core::kErrorPlanningFailed,
          "Cartesian planner returned malformed trajectory samples",
          {{"reason", "cartesian_trajectory_sample_invalid"},
           {"failure_stage", "cartesian_output_validation"},
           {"sample_index", std::to_string(sample)}}};
    }
    previous_time = trajectory.times[sample];
  }
  return std::nullopt;
}

void normalize_trajectory_boundaries(roboplan::JointTrajectory& trajectory) {
  if (trajectory.velocities.empty() || trajectory.accelerations.empty()) {
    return;
  }
  trajectory.velocities.front().setZero();
  trajectory.accelerations.front().setZero();
  trajectory.velocities.back().setZero();
  trajectory.accelerations.back().setZero();
}

double evaluate_polynomial(std::span<const double> coefficients,
                           double value) {
  double result = 0.0;
  for (auto coefficient = coefficients.rbegin();
       coefficient != coefficients.rend(); ++coefficient) {
    result = std::fma(result, value, *coefficient);
  }
  return result;
}

template <std::size_t Capacity>
struct UnitRoots {
  std::array<double, Capacity> values{};
  std::size_t count = 0;
};

template <std::size_t Capacity>
void append_unit_root(UnitRoots<Capacity>& roots, double value) {
  constexpr double kRootMergeTolerance = 64.0 *
      std::numeric_limits<double>::epsilon();
  if (!std::isfinite(value) || value < -kRootMergeTolerance ||
      value > 1.0 + kRootMergeTolerance) {
    return;
  }
  value = std::clamp(value, 0.0, 1.0);
  for (std::size_t index = 0; index < roots.count; ++index) {
    if (std::abs(roots.values[index] - value) <= kRootMergeTolerance) {
      return;
    }
  }
  if (roots.count < Capacity) {
    roots.values[roots.count++] = value;
  }
}

UnitRoots<2> quadratic_unit_roots(double c0, double c1, double c2) {
  UnitRoots<2> roots;
  const double coefficient_scale =
      std::max({std::abs(c0), std::abs(c1), std::abs(c2)});
  if (!std::isfinite(coefficient_scale) || coefficient_scale == 0.0) {
    return roots;
  }
  c0 /= coefficient_scale;
  c1 /= coefficient_scale;
  c2 /= coefficient_scale;
  const double zero_tolerance =
      64.0 * std::numeric_limits<double>::epsilon();
  if (std::abs(c2) <= zero_tolerance) {
    if (std::abs(c1) > zero_tolerance) {
      append_unit_root(roots, -c0 / c1);
    }
    return roots;
  }

  double discriminant = std::fma(c1, c1, -4.0 * c2 * c0);
  const double discriminant_tolerance =
      128.0 * std::numeric_limits<double>::epsilon() *
      std::max(c1 * c1, std::abs(4.0 * c2 * c0));
  if (discriminant < -discriminant_tolerance ||
      !std::isfinite(discriminant)) {
    return roots;
  }
  discriminant = std::max(0.0, discriminant);
  const double square_root = std::sqrt(discriminant);
  const double q = -0.5 * (c1 + std::copysign(square_root, c1));
  if (q == 0.0) {
    append_unit_root(roots, -c1 / (2.0 * c2));
  } else {
    append_unit_root(roots, q / c2);
    append_unit_root(roots, c0 / q);
  }
  std::sort(roots.values.begin(),
            roots.values.begin() + static_cast<std::ptrdiff_t>(roots.count));
  return roots;
}

UnitRoots<3> cubic_unit_roots(double c0, double c1, double c2, double c3) {
  const double coefficient_scale = std::max(
      {std::abs(c0), std::abs(c1), std::abs(c2), std::abs(c3)});
  if (!std::isfinite(coefficient_scale) || coefficient_scale == 0.0) {
    return {};
  }
  c0 /= coefficient_scale;
  c1 /= coefficient_scale;
  c2 /= coefficient_scale;
  c3 /= coefficient_scale;
  const double zero_tolerance =
      128.0 * std::numeric_limits<double>::epsilon();
  if (std::abs(c3) <= zero_tolerance) {
    const auto quadratic_roots = quadratic_unit_roots(c0, c1, c2);
    UnitRoots<3> roots;
    for (std::size_t index = 0; index < quadratic_roots.count; ++index) {
      append_unit_root(roots, quadratic_roots.values[index]);
    }
    return roots;
  }

  std::array<double, 4> partitions{};
  std::size_t partition_count = 0;
  partitions[partition_count++] = 0.0;
  const auto critical_points = quadratic_unit_roots(c1, 2.0 * c2,
                                                     3.0 * c3);
  for (std::size_t index = 0; index < critical_points.count; ++index) {
    partitions[partition_count++] = critical_points.values[index];
  }
  partitions[partition_count++] = 1.0;
  std::sort(partitions.begin(),
            partitions.begin() +
                static_cast<std::ptrdiff_t>(partition_count));
  partition_count = static_cast<std::size_t>(std::distance(
      partitions.begin(),
      std::unique(partitions.begin(),
                  partitions.begin() +
                      static_cast<std::ptrdiff_t>(partition_count))));

  const std::array<double, 4> polynomial{c0, c1, c2, c3};
  UnitRoots<3> roots;
  for (std::size_t index = 0; index < partition_count; ++index) {
    const double point = partitions[index];
    const double value = evaluate_polynomial(polynomial, point);
    if (std::isfinite(value) && std::abs(value) <= zero_tolerance) {
      append_unit_root(roots, point);
    }
  }
  for (std::size_t interval = 1; interval < partition_count; ++interval) {
    double lower = partitions[interval - 1];
    double upper = partitions[interval];
    double lower_value = evaluate_polynomial(polynomial, lower);
    double upper_value = evaluate_polynomial(polynomial, upper);
    if (!std::isfinite(lower_value) || !std::isfinite(upper_value) ||
        lower_value == 0.0 || upper_value == 0.0 ||
        std::signbit(lower_value) == std::signbit(upper_value)) {
      continue;
    }
    for (std::size_t iteration = 0; iteration < 80; ++iteration) {
      const double midpoint = lower + (upper - lower) * 0.5;
      if (midpoint <= lower || upper <= midpoint) {
        break;
      }
      const double midpoint_value = evaluate_polynomial(polynomial, midpoint);
      if (!std::isfinite(midpoint_value)) {
        break;
      }
      if (midpoint_value == 0.0) {
        lower = midpoint;
        upper = midpoint;
        break;
      }
      if (std::signbit(midpoint_value) == std::signbit(lower_value)) {
        lower = midpoint;
        lower_value = midpoint_value;
      } else {
        upper = midpoint;
        upper_value = midpoint_value;
      }
    }
    append_unit_root(roots, lower + (upper - lower) * 0.5);
  }
  std::sort(roots.values.begin(),
            roots.values.begin() + static_cast<std::ptrdiff_t>(roots.count));
  return roots;
}

std::optional<std::array<double, 6>> quintic_position_coefficients(
    double position_from, double position_to,
    double velocity_from, double velocity_to,
    double acceleration_from, double acceleration_to,
    double duration) {
  const double duration_squared = duration * duration;
  if (!finite_positive(duration) || !std::isfinite(duration_squared) ||
      duration_squared <= 0.0 || !std::isfinite(position_from) ||
      !std::isfinite(position_to) || !std::isfinite(velocity_from) ||
      !std::isfinite(velocity_to) || !std::isfinite(acceleration_from) ||
      !std::isfinite(acceleration_to)) {
    return std::nullopt;
  }
  const double delta = position_to - position_from;
  const double scaled_velocity_from = duration * velocity_from;
  const double scaled_velocity_to = duration * velocity_to;
  const double scaled_acceleration_from =
      duration_squared * acceleration_from;
  const double scaled_acceleration_to =
      duration_squared * acceleration_to;
  const std::array<double, 6> coefficients{
      position_from,
      scaled_velocity_from,
      0.5 * scaled_acceleration_from,
      10.0 * delta - 6.0 * scaled_velocity_from -
          4.0 * scaled_velocity_to - 1.5 * scaled_acceleration_from +
          0.5 * scaled_acceleration_to,
      -15.0 * delta + 8.0 * scaled_velocity_from +
          7.0 * scaled_velocity_to + 1.5 * scaled_acceleration_from -
          scaled_acceleration_to,
      6.0 * delta - 3.0 * (scaled_velocity_from + scaled_velocity_to) -
          0.5 * scaled_acceleration_from +
          0.5 * scaled_acceleration_to};
  if (!std::all_of(coefficients.begin(), coefficients.end(),
                   [](double value) { return std::isfinite(value); })) {
    return std::nullopt;
  }
  return coefficients;
}

double maximum_normalized_joint_speed(
    const std::array<double, 6>& position_coefficients) {
  const std::array<double, 5> velocity_coefficients{
      position_coefficients[1], 2.0 * position_coefficients[2],
      3.0 * position_coefficients[3],
      4.0 * position_coefficients[4],
      5.0 * position_coefficients[5]};
  const std::array<double, 4> acceleration_coefficients{
      2.0 * position_coefficients[2],
      6.0 * position_coefficients[3],
      12.0 * position_coefficients[4],
      20.0 * position_coefficients[5]};
  const auto acceleration_roots = cubic_unit_roots(
      acceleration_coefficients[0], acceleration_coefficients[1],
      acceleration_coefficients[2], acceleration_coefficients[3]);
  double maximum = std::max(
      std::abs(evaluate_polynomial(velocity_coefficients, 0.0)),
      std::abs(evaluate_polynomial(velocity_coefficients, 1.0)));
  for (std::size_t root = 0; root < acceleration_roots.count; ++root) {
    maximum = std::max(
        maximum,
        std::abs(evaluate_polynomial(
            velocity_coefficients, acceleration_roots.values[root])));
  }
  return maximum;
}

struct TrajectoryEnvelope {
  bool complete = true;
  double joint_velocity_ratio = 1.0;
  double joint_acceleration_ratio = 1.0;
  double linear_speed_ratio = 1.0;
  double angular_speed_ratio = 1.0;
  double linear_acceleration_ratio = 1.0;
  double angular_acceleration_ratio = 1.0;
};

core::Result<TrajectoryEnvelope> measure_envelope(
    const core::PlanRequestDTO& request,
    const core::CartesianPathDTO& path,
    const std::shared_ptr<roboplan::Scene>& scene,
    const roboplan::JointTrajectory& trajectory,
    const Eigen::VectorXd& model_velocity_limits,
    const Eigen::VectorXd& model_acceleration_limits,
    const CartesianPlanningControl& control) {
  TrajectoryEnvelope envelope;
  if (trajectory.positions.size() < 2 ||
      trajectory.times.size() != trajectory.positions.size() ||
      trajectory.velocities.size() != trajectory.positions.size() ||
      trajectory.accelerations.size() != trajectory.positions.size()) {
    envelope.complete = false;
    return core::Result<TrajectoryEnvelope>::Ok(std::move(envelope));
  }

  if (model_velocity_limits.size() !=
          static_cast<Eigen::Index>(trajectory.joint_names.size()) ||
      model_acceleration_limits.size() !=
          static_cast<Eigen::Index>(trajectory.joint_names.size())) {
    envelope.complete = false;
    return core::Result<TrajectoryEnvelope>::Ok(std::move(envelope));
  }
  auto joint_envelope = detail::measure_cartesian_joint_dynamics_envelope(
      request, trajectory,
      std::span<const double>(model_velocity_limits.data(),
                              static_cast<std::size_t>(
                                  model_velocity_limits.size())),
      std::span<const double>(model_acceleration_limits.data(),
                              static_cast<std::size_t>(
                                  model_acceleration_limits.size())),
      control);
  if (!joint_envelope.ok()) {
    return core::Result<TrajectoryEnvelope>::Err(joint_envelope.error());
  }
  if (!joint_envelope.value().complete) {
    envelope.complete = false;
    return core::Result<TrajectoryEnvelope>::Ok(std::move(envelope));
  }
  envelope.joint_velocity_ratio = joint_envelope.value().velocity_ratio;
  envelope.joint_acceleration_ratio =
      joint_envelope.value().acceleration_ratio;

  auto frame_envelope = detail::measure_cartesian_frame_dynamics_envelope(
      request, path, scene, trajectory, control);
  if (!frame_envelope.ok()) {
    return core::Result<TrajectoryEnvelope>::Err(frame_envelope.error());
  }
  if (!frame_envelope.value().complete) {
    envelope.complete = false;
    return core::Result<TrajectoryEnvelope>::Ok(std::move(envelope));
  }
  envelope.linear_speed_ratio = frame_envelope.value().linear_speed_ratio;
  envelope.angular_speed_ratio = frame_envelope.value().angular_speed_ratio;
  envelope.linear_acceleration_ratio =
      frame_envelope.value().linear_acceleration_ratio;
  envelope.angular_acceleration_ratio =
      frame_envelope.value().angular_acceleration_ratio;
  return core::Result<TrajectoryEnvelope>::Ok(std::move(envelope));
}

double required_time_scale(const TrajectoryEnvelope& envelope) {
  return std::max(
      {1.0, envelope.joint_velocity_ratio, envelope.linear_speed_ratio,
       envelope.angular_speed_ratio, std::sqrt(envelope.joint_acceleration_ratio),
       std::sqrt(envelope.linear_acceleration_ratio),
       std::sqrt(envelope.angular_acceleration_ratio)});
}

void scale_trajectory_time(roboplan::JointTrajectory& trajectory,
                           double scale) {
  const double acceleration_scale = scale * scale;
  for (auto& time : trajectory.times) {
    time *= scale;
  }
  for (auto& velocity : trajectory.velocities) {
    velocity /= scale;
  }
  for (auto& acceleration : trajectory.accelerations) {
    acceleration /= acceleration_scale;
  }
}

void scale_envelope_time(TrajectoryEnvelope& envelope, double scale) {
  const double acceleration_scale = scale * scale;
  envelope.joint_velocity_ratio /= scale;
  envelope.linear_speed_ratio /= scale;
  envelope.angular_speed_ratio /= scale;
  envelope.joint_acceleration_ratio /= acceleration_scale;
  envelope.linear_acceleration_ratio /= acceleration_scale;
  envelope.angular_acceleration_ratio /= acceleration_scale;
}

}  // namespace

namespace detail {

std::optional<core::ErrorDTO>
validate_and_normalize_cartesian_trajectory_output(
    roboplan::JointTrajectory& trajectory,
    const std::vector<std::string>& expected_joint_names,
    const CartesianPlanningControl& control) {
  if (auto error = validate_cartesian_trajectory_output(
          trajectory, expected_joint_names, control);
      error.has_value()) {
    return error;
  }
  normalize_trajectory_boundaries(trajectory);
  return std::nullopt;
}

core::Result<CartesianJointDynamicsEnvelope>
measure_cartesian_joint_dynamics_envelope(
    const core::PlanRequestDTO& request,
    const roboplan::JointTrajectory& trajectory,
    std::span<const double> model_velocity_limits,
    std::span<const double> model_acceleration_limits,
    const CartesianPlanningControl& control) {
  CartesianJointDynamicsEnvelope envelope;
  const std::size_t joint_count = trajectory.joint_names.size();
  const std::size_t sample_count = trajectory.positions.size();
  if (joint_count == 0 || sample_count < 2 ||
      model_velocity_limits.size() != joint_count ||
      model_acceleration_limits.size() != joint_count ||
      trajectory.times.size() != sample_count ||
      trajectory.velocities.size() != sample_count ||
      trajectory.accelerations.size() != sample_count) {
    envelope.complete = false;
    return core::Result<CartesianJointDynamicsEnvelope>::Ok(
        std::move(envelope));
  }

  std::size_t work_index = 0;
  for (std::size_t joint = 0; joint < joint_count; ++joint) {
    double velocity_limit = std::abs(model_velocity_limits[joint]);
    double acceleration_limit = std::abs(model_acceleration_limits[joint]);
    const auto requested_limit = request.per_joint_dynamics_limits.find(
        trajectory.joint_names[joint]);
    if (requested_limit != request.per_joint_dynamics_limits.end()) {
      if (requested_limit->second.max_velocity_rad_s.has_value()) {
        velocity_limit = std::min(
            velocity_limit,
            *requested_limit->second.max_velocity_rad_s);
      }
      if (requested_limit->second.max_acceleration_rad_s2.has_value()) {
        acceleration_limit = std::min(
            acceleration_limit,
            *requested_limit->second.max_acceleration_rad_s2);
      }
    }
    velocity_limit *= request.speed_profile.velocity_scale;
    acceleration_limit *= request.speed_profile.acceleration_scale;
    if (!finite_positive(velocity_limit) ||
        !finite_positive(acceleration_limit)) {
      envelope.complete = false;
      return core::Result<CartesianJointDynamicsEnvelope>::Ok(
          std::move(envelope));
    }

    for (std::size_t sample = 1; sample < sample_count; ++sample) {
      if (work_index++ % kPlanningControlCheckStride == 0) {
        if (auto error = planning_control_error(
                control, "cartesian_dynamics_validation");
            error.has_value()) {
          return core::Result<CartesianJointDynamicsEnvelope>::Err(
              std::move(*error));
        }
      }
      const Eigen::Index joint_index = static_cast<Eigen::Index>(joint);
      const auto& from_positions = trajectory.positions[sample - 1];
      const auto& to_positions = trajectory.positions[sample];
      const auto& from_velocities = trajectory.velocities[sample - 1];
      const auto& to_velocities = trajectory.velocities[sample];
      const auto& from_accelerations = trajectory.accelerations[sample - 1];
      const auto& to_accelerations = trajectory.accelerations[sample];
      if (joint_index >= from_positions.size() ||
          joint_index >= to_positions.size() ||
          joint_index >= from_velocities.size() ||
          joint_index >= to_velocities.size() ||
          joint_index >= from_accelerations.size() ||
          joint_index >= to_accelerations.size()) {
        envelope.complete = false;
        return core::Result<CartesianJointDynamicsEnvelope>::Ok(
            std::move(envelope));
      }

      const double duration =
          trajectory.times[sample] - trajectory.times[sample - 1];
      const double duration_squared = duration * duration;
      const double position_from = from_positions[joint_index];
      const double position_to = to_positions[joint_index];
      const double velocity_from = from_velocities[joint_index];
      const double velocity_to = to_velocities[joint_index];
      const double acceleration_from = from_accelerations[joint_index];
      const double acceleration_to = to_accelerations[joint_index];
      // Coefficients of the exact normalized-time quintic used by the
      // ManyForge trajectory sampler. Checking only the producer-provided
      // endpoint derivatives would allow a dishonest or buggy planner to hide
      // between-waypoint speed and acceleration peaks.
      const auto position_coefficients = quintic_position_coefficients(
          position_from, position_to, velocity_from, velocity_to,
          acceleration_from, acceleration_to, duration);
      if (!position_coefficients.has_value()) {
        envelope.complete = false;
        return core::Result<CartesianJointDynamicsEnvelope>::Ok(
            std::move(envelope));
      }

      const std::array<double, 5> velocity_coefficients{
          (*position_coefficients)[1], 2.0 * (*position_coefficients)[2],
          3.0 * (*position_coefficients)[3],
          4.0 * (*position_coefficients)[4],
          5.0 * (*position_coefficients)[5]};
      const std::array<double, 4> acceleration_coefficients{
          2.0 * (*position_coefficients)[2],
          6.0 * (*position_coefficients)[3],
          12.0 * (*position_coefficients)[4],
          20.0 * (*position_coefficients)[5]};
      if (!std::all_of(velocity_coefficients.begin(),
                       velocity_coefficients.end(),
                       [](double value) { return std::isfinite(value); }) ||
          !std::all_of(acceleration_coefficients.begin(),
                       acceleration_coefficients.end(),
                       [](double value) { return std::isfinite(value); })) {
        envelope.complete = false;
        return core::Result<CartesianJointDynamicsEnvelope>::Ok(
            std::move(envelope));
      }

      const auto acceleration_roots = cubic_unit_roots(
          acceleration_coefficients[0], acceleration_coefficients[1],
          acceleration_coefficients[2], acceleration_coefficients[3]);
      const auto record_velocity = [&](double progress) {
        const double velocity =
            evaluate_polynomial(velocity_coefficients, progress) / duration;
        if (!std::isfinite(velocity)) {
          envelope.complete = false;
          return false;
        }
        envelope.velocity_ratio = std::max(
            envelope.velocity_ratio, std::abs(velocity) / velocity_limit);
        return true;
      };
      if (!record_velocity(0.0) || !record_velocity(1.0)) {
        return core::Result<CartesianJointDynamicsEnvelope>::Ok(
            std::move(envelope));
      }
      for (std::size_t root = 0; root < acceleration_roots.count; ++root) {
        if (!record_velocity(acceleration_roots.values[root])) {
          return core::Result<CartesianJointDynamicsEnvelope>::Ok(
              std::move(envelope));
        }
      }

      const auto jerk_roots = quadratic_unit_roots(
          6.0 * (*position_coefficients)[3],
          24.0 * (*position_coefficients)[4],
          60.0 * (*position_coefficients)[5]);
      const auto record_acceleration = [&](double progress) {
        const double acceleration =
            evaluate_polynomial(acceleration_coefficients, progress) /
            duration_squared;
        if (!std::isfinite(acceleration)) {
          envelope.complete = false;
          return false;
        }
        envelope.acceleration_ratio =
            std::max(envelope.acceleration_ratio,
                     std::abs(acceleration) / acceleration_limit);
        return true;
      };
      if (!record_acceleration(0.0) || !record_acceleration(1.0)) {
        return core::Result<CartesianJointDynamicsEnvelope>::Ok(
            std::move(envelope));
      }
      for (std::size_t root = 0; root < jerk_roots.count; ++root) {
        if (!record_acceleration(jerk_roots.values[root])) {
          return core::Result<CartesianJointDynamicsEnvelope>::Ok(
              std::move(envelope));
        }
      }
    }
  }
  return core::Result<CartesianJointDynamicsEnvelope>::Ok(
      std::move(envelope));
}

core::Result<CartesianInterpolantPath> sample_cartesian_interpolant_path(
    const core::PlanRequestDTO& request,
    const std::shared_ptr<roboplan::Scene>& scene,
    const roboplan::JointTrajectory& trajectory,
    double translation_tolerance_m,
    double rotation_tolerance_rad,
    const CartesianPlanningControl& control) {
  const auto fail = [](std::string message, std::string reason,
                       std::map<std::string, std::string> details = {}) {
    details["reason"] = std::move(reason);
    details["failure_stage"] = "cartesian_output_validation";
    return core::Result<CartesianInterpolantPath>::Err(core::ErrorDTO{
        core::kErrorCartesianIncomplete, std::move(message),
        std::move(details)});
  };
  const std::size_t sample_count = trajectory.positions.size();
  const std::size_t joint_count = trajectory.joint_names.size();
  if (!scene || !finite_positive(translation_tolerance_m) ||
      !finite_positive(rotation_tolerance_rad) || joint_count == 0 ||
      sample_count < 2 || sample_count > kMaxInterpolantPathSamples ||
      trajectory.times.size() != trajectory.positions.size() ||
      trajectory.velocities.size() != trajectory.positions.size() ||
      trajectory.accelerations.size() != trajectory.positions.size()) {
    return fail("Cartesian trajectory interpolant is malformed",
                "cartesian_trajectory_interpolant_invalid");
  }
  auto motion_bounds = resolve_cartesian_kinematic_motion_bounds(
      scene, request.group_id, trajectory.joint_names);
  if (!motion_bounds.ok()) {
    return core::Result<CartesianInterpolantPath>::Err(
        motion_bounds.error());
  }
  for (std::size_t sample = 0; sample < sample_count; ++sample) {
    if (!std::isfinite(trajectory.times[sample]) ||
        trajectory.positions[sample].size() !=
            static_cast<Eigen::Index>(joint_count) ||
        trajectory.velocities[sample].size() !=
            static_cast<Eigen::Index>(joint_count) ||
        trajectory.accelerations[sample].size() !=
            static_cast<Eigen::Index>(joint_count) ||
        !trajectory.positions[sample].allFinite() ||
        !trajectory.velocities[sample].allFinite() ||
        !trajectory.accelerations[sample].allFinite() ||
        (sample > 0 &&
         !finite_positive(trajectory.times[sample] -
                          trajectory.times[sample - 1]))) {
      return fail("Cartesian trajectory interpolant samples are malformed",
                  "cartesian_trajectory_interpolant_invalid",
                  {{"sample_index", std::to_string(sample)}});
    }
  }

  const auto& kinematic_bounds = motion_bounds.value();
  const double model_radius = kinematic_bounds.model_radius_m;
  const double translation_budget =
      kUnsampledFidelityBudgetFraction * translation_tolerance_m;
  const double rotation_budget =
      kUnsampledFidelityBudgetFraction * rotation_tolerance_rad;
  if (!finite_positive(model_radius) || !finite_positive(translation_budget) ||
      !finite_positive(rotation_budget)) {
    return fail("Cartesian fidelity tolerance cannot be represented for this "
                "kinematic model",
                "cartesian_resource_budget_exceeded",
                {{"resource", "interpolant_fidelity_resolution"}});
  }

  CartesianInterpolantPath result;
  result.poses.reserve(std::min(kMaxInterpolantPathSamples,
                               sample_count * static_cast<std::size_t>(2)));
  const auto append_pose = [&](const Eigen::VectorXd& group_positions)
      -> std::optional<core::ErrorDTO> {
    if (!group_positions.allFinite()) {
      return core::ErrorDTO{
          core::kErrorCartesianIncomplete,
          "Cartesian trajectory interpolant contains a non-finite position",
          {{"reason", "cartesian_trajectory_interpolant_invalid"},
           {"failure_stage", "cartesian_output_validation"}}};
    }
    const auto full_positions =
        scene->toFullJointPositions(request.group_id, group_positions);
    const auto pose = scene->forwardKinematics(
        full_positions, request.tcp_frame, request.base_frame);
    if (!pose.allFinite()) {
      return core::ErrorDTO{
          core::kErrorCartesianIncomplete,
          "Cartesian trajectory interpolant FK is not finite",
          {{"reason", "cartesian_trajectory_interpolant_invalid"},
           {"failure_stage", "cartesian_output_validation"}}};
    }
    result.poses.push_back(pose);
    return std::nullopt;
  };
  if (auto error = append_pose(trajectory.positions.front());
      error.has_value()) {
    return core::Result<CartesianInterpolantPath>::Err(std::move(*error));
  }

  std::size_t work_index = 0;
  for (std::size_t sample = 1; sample < trajectory.positions.size(); ++sample) {
    if (auto error = planning_control_error(
            control, "cartesian_path_interpolant_validation");
        error.has_value()) {
      return core::Result<CartesianInterpolantPath>::Err(std::move(*error));
    }
    const double duration =
        trajectory.times[sample] - trajectory.times[sample - 1];
    std::vector<std::array<double, 6>> coefficients;
    coefficients.reserve(trajectory.joint_names.size());
    long double prismatic_motion_bound_m = 0.0L;
    long double angular_motion_bound_rad = 0.0L;
    for (std::size_t joint = 0; joint < trajectory.joint_names.size(); ++joint) {
      const Eigen::Index index = static_cast<Eigen::Index>(joint);
      if (index >= trajectory.positions[sample - 1].size() ||
          index >= trajectory.positions[sample].size() ||
          index >= trajectory.velocities[sample - 1].size() ||
          index >= trajectory.velocities[sample].size() ||
          index >= trajectory.accelerations[sample - 1].size() ||
          index >= trajectory.accelerations[sample].size()) {
        return fail("Cartesian trajectory interpolant dimensions are invalid",
                    "cartesian_trajectory_interpolant_invalid");
      }
      auto joint_coefficients = quintic_position_coefficients(
          trajectory.positions[sample - 1][index],
          trajectory.positions[sample][index],
          trajectory.velocities[sample - 1][index],
          trajectory.velocities[sample][index],
          trajectory.accelerations[sample - 1][index],
          trajectory.accelerations[sample][index], duration);
      if (!joint_coefficients.has_value()) {
        return fail("Cartesian trajectory interpolant coefficients are invalid",
                    "cartesian_trajectory_interpolant_invalid",
                    {{"segment_index", std::to_string(sample - 1)},
                     {"joint", trajectory.joint_names[joint]}});
      }
      const double maximum_speed =
          maximum_normalized_joint_speed(*joint_coefficients);
      if (!std::isfinite(maximum_speed)) {
        return fail("Cartesian trajectory interpolant motion bound is invalid",
                    "cartesian_trajectory_interpolant_invalid");
      }
      const long double normalized_motion =
          static_cast<long double>(maximum_speed);
      prismatic_motion_bound_m +=
          normalized_motion *
          kinematic_bounds.prismatic_amplification[joint];
      angular_motion_bound_rad +=
          normalized_motion * kinematic_bounds.angular_amplification[joint];
      coefficients.push_back(*joint_coefficients);
    }
    const long double translation_motion_bound_m =
        prismatic_motion_bound_m +
        static_cast<long double>(model_radius) * angular_motion_bound_rad;
    const long double translation_subdivisions =
        translation_motion_bound_m /
        static_cast<long double>(translation_budget);
    const long double rotation_subdivisions =
        angular_motion_bound_rad / static_cast<long double>(rotation_budget);
    const long double required_subdivisions = std::max(
        1.0L, std::ceil(std::max(translation_subdivisions,
                                rotation_subdivisions)));
    const std::size_t remaining_budget =
        kMaxInterpolantPathSamples - result.poses.size();
    if (!std::isfinite(required_subdivisions) ||
        required_subdivisions > static_cast<long double>(remaining_budget)) {
      return fail("Cartesian trajectory exceeds the bounded continuous-path "
                  "fidelity budget",
                  "cartesian_resource_budget_exceeded",
                  {{"resource", "interpolant_fidelity_samples"},
                   {"max_interpolant_samples",
                    std::to_string(kMaxInterpolantPathSamples)},
                   {"segment_index", std::to_string(sample - 1)}});
    }
    const auto subdivisions =
        static_cast<std::size_t>(required_subdivisions);
    result.unsampled_rotation_bound_rad = std::max(
        result.unsampled_rotation_bound_rad,
        static_cast<double>(angular_motion_bound_rad /
                            static_cast<long double>(subdivisions)));
    result.unsampled_translation_bound_m = std::max(
        result.unsampled_translation_bound_m,
        static_cast<double>(translation_motion_bound_m /
                            static_cast<long double>(subdivisions)));

    Eigen::VectorXd group_positions(
        static_cast<Eigen::Index>(trajectory.joint_names.size()));
    for (std::size_t step = 1; step <= subdivisions; ++step) {
      if (work_index++ % kPlanningControlCheckStride == 0) {
        if (auto error = planning_control_error(
                control, "cartesian_path_interpolant_validation");
            error.has_value()) {
          return core::Result<CartesianInterpolantPath>::Err(
              std::move(*error));
        }
      }
      if (step == subdivisions) {
        group_positions = trajectory.positions[sample];
      } else {
        const double progress = static_cast<double>(step) /
                                static_cast<double>(subdivisions);
        for (std::size_t joint = 0; joint < coefficients.size(); ++joint) {
          group_positions[static_cast<Eigen::Index>(joint)] =
              evaluate_polynomial(coefficients[joint], progress);
        }
      }
      if (auto error = append_pose(group_positions); error.has_value()) {
        return core::Result<CartesianInterpolantPath>::Err(
            std::move(*error));
      }
    }
  }
  return core::Result<CartesianInterpolantPath>::Ok(std::move(result));
}

}  // namespace detail

core::ErrorDTO map_cartesian_backend_failure(
    std::string_view planner_error, double fraction_completed,
    std::optional<std::size_t> first_failing_waypoint_index) {
  std::string reason = "cartesian_path_incomplete";
  std::string failure_stage = "cartesian_tracking";
  auto code = core::kErrorCartesianIncomplete;
  if (contains_text(planner_error, "Could not resolve tip frame") ||
      contains_text(planner_error, "Could not resolve base frame")) {
    reason = "cartesian_frame_unknown";
    failure_stage = "cartesian_request_validation";
    code = core::kErrorInvalidRequest;
  } else if (contains_text(planner_error,
                           "bounded control-step budget")) {
    reason = "cartesian_resource_budget_exceeded";
    failure_stage = "cartesian_tracking";
  } else if (contains_text(planner_error, "stalled") ||
             contains_text(planner_error, "singularity")) {
    reason = "cartesian_tracking_stalled";
  } else if (contains_text(planner_error, "within tolerance") ||
             contains_text(planner_error, "maximum number of control steps")) {
    reason = "cartesian_path_tolerance_exceeded";
  } else if (contains_text(planner_error, "Oink solve failed")) {
    reason = "cartesian_ik_failure";
  } else if (contains_text(planner_error, "Joint position limit") ||
             contains_text(planner_error, "Joint velocity limit")) {
    reason = "cartesian_joint_limit_violation";
    failure_stage = "cartesian_output_validation";
    code = core::kErrorValidationLimitsViolation;
  } else if (contains_text(planner_error, "TOPP-RA")) {
    reason = "cartesian_time_parameterization_failed";
    failure_stage = "cartesian_time_parameterization";
    code = core::kErrorPlanningFailed;
  }
  const std::string planner_error_string(planner_error);
  if (code == core::kErrorCartesianIncomplete) {
    return cartesian_incomplete_error(
        "Cartesian path could not be traced: " + planner_error_string,
        std::move(reason), fraction_completed,
        first_failing_waypoint_index,
        {{"failure_stage", std::move(failure_stage)},
         {"planner_error", planner_error_string}});
  }
  return core::ErrorDTO{
      code,
      "Cartesian path could not be traced: " + planner_error_string,
      {{"reason", std::move(reason)},
       {"failure_stage", std::move(failure_stage)},
       {"planner_error", planner_error_string}}};
}

namespace {

struct ControlledCartesianPathFidelityResult {
  CartesianPathFidelityReport report;
  std::optional<core::ErrorDTO> error;
};

ControlledCartesianPathFidelityResult
verify_cartesian_path_fidelity_controlled(
    std::span<const Eigen::Matrix4d> requested_path,
    std::span<const Eigen::Matrix4d> actual_path,
    double translation_tolerance_m, double rotation_tolerance_rad,
    const CartesianPlanningControl* control) {
  ControlledCartesianPathFidelityResult result;
  auto& report = result.report;
  if (requested_path.empty() || actual_path.empty() ||
      !finite_positive(translation_tolerance_m) ||
      !finite_positive(rotation_tolerance_rad)) {
    report.first_failing_path_sample_index = 0;
    return result;
  }
  const auto all_finite = [](std::span<const Eigen::Matrix4d> poses) {
    return std::all_of(poses.begin(), poses.end(),
                       [](const auto& pose) { return pose.allFinite(); });
  };
  if (!all_finite(requested_path) || !all_finite(actual_path)) {
    report.first_failing_path_sample_index = 0;
    return result;
  }

  const auto requested_parameter = normalized_path_parameter(
      requested_path, translation_tolerance_m, rotation_tolerance_rad,
      PathParameterMetric::kEuclideanTolerance, control, &result.error);
  if (!requested_parameter.has_value()) {
    report.first_failing_path_sample_index = 0;
    return result;
  }
  const auto actual_parameter = normalized_path_parameter(
      actual_path, translation_tolerance_m, rotation_tolerance_rad,
      PathParameterMetric::kEuclideanTolerance, control, &result.error);
  if (!actual_parameter.has_value()) {
    report.first_failing_path_sample_index = 0;
    return result;
  }
  const auto breakpoints = merged_path_breakpoints(
      *requested_parameter, *actual_parameter, control, &result.error);
  if (result.error.has_value()) {
    return result;
  }
  constexpr std::size_t kMaxFidelitySubdivisionDepth = 24;
  constexpr std::size_t kMaxFidelityIntervalChecks = 500000;
  if (breakpoints.size() > kMaxFidelityIntervalChecks + 1) {
    result.error = cartesian_incomplete_error(
        "Cartesian path exceeds the bounded fidelity comparison budget",
        "cartesian_resource_budget_exceeded", 0.0, 0U,
        {{"failure_stage", "cartesian_path_fidelity_validation"},
         {"resource", "fidelity_interval_checks"},
         {"max_interval_checks",
          std::to_string(kMaxFidelityIntervalChecks)}});
    report.first_failing_path_sample_index = 0;
    return result;
  }
  struct PathComparison {
    Eigen::Matrix4d requested;
    Eigen::Matrix4d actual;
    double translation_error_m = 0.0;
    double rotation_error_rad = 0.0;
    bool within_tolerance = false;
  };
  std::size_t comparison_count = 0;
  double certified_through = 0.0;

  const auto mark_failure = [&](double fraction) {
    if (report.first_failing_path_sample_index.has_value()) {
      return;
    }
    const auto match = std::lower_bound(requested_parameter->begin(),
                                        requested_parameter->end(), fraction);
    report.first_failing_path_sample_index =
        match == requested_parameter->end()
            ? requested_parameter->size() - 1
            : static_cast<std::size_t>(
                  std::distance(requested_parameter->begin(), match));
    report.fraction_completed = certified_through;
  };
  const auto compare_at = [&](double fraction) -> PathComparison {
    PathComparison comparison;
    comparison.requested = interpolate_path_pose(
        requested_path, *requested_parameter, fraction);
    comparison.actual = interpolate_path_pose(
        actual_path, *actual_parameter, fraction);
    comparison.translation_error_m =
        (comparison.actual.block<3, 1>(0, 3) -
         comparison.requested.block<3, 1>(0, 3))
            .norm();
    comparison.rotation_error_rad = rotation_distance(
        comparison.actual.block<3, 3>(0, 0),
        comparison.requested.block<3, 3>(0, 0));
    report.max_translation_error_m = std::max(
        report.max_translation_error_m, comparison.translation_error_m);
    report.max_rotation_error_rad = std::max(
        report.max_rotation_error_rad, comparison.rotation_error_rad);
    comparison.within_tolerance =
        std::isfinite(comparison.translation_error_m) &&
        std::isfinite(comparison.rotation_error_rad) &&
        comparison.translation_error_m <= translation_tolerance_m &&
        comparison.rotation_error_rad <= rotation_tolerance_rad;
    return comparison;
  };

  const auto certify_interval =
      [&](auto&& self, double lower_fraction, double upper_fraction,
          const PathComparison& lower, const PathComparison& upper,
          std::size_t depth) -> bool {
    if (comparison_count >= kMaxFidelityIntervalChecks) {
      result.error = cartesian_incomplete_error(
          "Cartesian path exceeds the bounded fidelity comparison budget",
          "cartesian_resource_budget_exceeded", certified_through,
          report.first_failing_path_sample_index,
          {{"failure_stage", "cartesian_path_fidelity_validation"},
           {"resource", "fidelity_interval_checks"},
           {"max_interval_checks",
            std::to_string(kMaxFidelityIntervalChecks)}});
      return false;
    }
    const std::size_t interval_check = comparison_count++;
    report.interval_checks = comparison_count;
    if (control != nullptr &&
        interval_check % kPlanningControlCheckStride == 0) {
      if (auto error = planning_control_error(
              *control, "cartesian_path_fidelity_validation");
          error.has_value()) {
        result.error = std::move(*error);
        return false;
      }
    }
    if (!lower.within_tolerance) {
      mark_failure(lower_fraction);
      return false;
    }
    if (!upper.within_tolerance) {
      mark_failure(upper_fraction);
      return false;
    }

    // Translation interpolation is affine on both sides of this merged
    // interval, so its normed difference is convex and is bounded by the two
    // endpoint errors. For SO(3), certify the entire interval with the triangle
    // inequality. If the conservative bound is inconclusive, subdivide. This
    // cannot accept a hidden between-sample rotation excursion.
    const double requested_rotation_span = rotation_distance(
        lower.requested.block<3, 3>(0, 0),
        upper.requested.block<3, 3>(0, 0));
    const double actual_rotation_span = rotation_distance(
        lower.actual.block<3, 3>(0, 0),
        upper.actual.block<3, 3>(0, 0));
    const double rotation_upper_bound = std::min(
        lower.rotation_error_rad + requested_rotation_span +
            actual_rotation_span,
        upper.rotation_error_rad + requested_rotation_span +
            actual_rotation_span);
    if (std::isfinite(rotation_upper_bound) &&
        rotation_upper_bound <= rotation_tolerance_rad) {
      certified_through = upper_fraction;
      return true;
    }

    const double midpoint =
        lower_fraction + (upper_fraction - lower_fraction) * 0.5;
    const auto middle = compare_at(midpoint);
    if (!middle.within_tolerance) {
      mark_failure(midpoint);
      return false;
    }
    if (comparison_count >= kMaxFidelityIntervalChecks) {
      result.error = cartesian_incomplete_error(
          "Cartesian path exceeds the bounded fidelity comparison budget",
          "cartesian_resource_budget_exceeded", certified_through,
          report.first_failing_path_sample_index,
          {{"failure_stage", "cartesian_path_fidelity_validation"},
           {"resource", "fidelity_interval_checks"},
           {"max_interval_checks",
            std::to_string(kMaxFidelityIntervalChecks)}});
      return false;
    }
    if (depth >= kMaxFidelitySubdivisionDepth ||
        midpoint <= lower_fraction || upper_fraction <= midpoint) {
      report.unresolved_rotation_error_upper_bound_rad =
          rotation_upper_bound;
      mark_failure(midpoint);
      return false;
    }
    if (!self(self, lower_fraction, midpoint, lower, middle, depth + 1)) {
      return false;
    }
    return self(self, midpoint, upper_fraction, middle, upper, depth + 1);
  };

  if (breakpoints.empty()) {
    report.first_failing_path_sample_index = 0;
    return result;
  }
  auto lower = compare_at(breakpoints.front());
  if (!lower.within_tolerance) {
    mark_failure(breakpoints.front());
    return result;
  }
  for (std::size_t index = 1; index < breakpoints.size(); ++index) {
    const auto upper = compare_at(breakpoints[index]);
    if (!certify_interval(certify_interval, breakpoints[index - 1],
                          breakpoints[index], lower, upper, 0)) {
      return result;
    }
    lower = upper;
  }
  report.complete = !report.first_failing_path_sample_index.has_value();
  if (report.complete) {
    report.fraction_completed = 1.0;
  }
  return result;
}

}  // namespace

CartesianPathFidelityReport verify_cartesian_path_fidelity(
    std::span<const Eigen::Matrix4d> requested_path,
    std::span<const Eigen::Matrix4d> actual_path,
    double translation_tolerance_m, double rotation_tolerance_rad) {
  return verify_cartesian_path_fidelity_controlled(
             requested_path, actual_path, translation_tolerance_m,
             rotation_tolerance_rad, nullptr)
      .report;
}

core::Result<roboplan::JointTrajectory> plan_cartesian_path(
    const core::PlanRequestDTO& request, const core::CartesianPathDTO& path,
    const std::shared_ptr<roboplan::Scene>& planning_scene,
    const CartesianPlanningControl& control) {
  if (!planning_scene) {
    return core::Result<roboplan::JointTrajectory>::Err(core::ErrorDTO{
        core::kErrorPlanningFailed, "Cartesian planning scene is unavailable",
        {{"reason", "cartesian_planning_scene_unavailable"}}});
  }
  if (request.base_frame.empty() || request.tcp_frame.empty()) {
    return core::Result<roboplan::JointTrajectory>::Err(
        invalid_cartesian_request(
            "Cartesian path requires non-empty base and TCP frames",
            "cartesian_frame_missing"));
  }
  if (path.speed_mode != core::CartesianSpeedModeDTO::kBounded) {
    return core::Result<roboplan::JointTrajectory>::Err(
        invalid_cartesian_request("Cartesian speed mode is unsupported",
                                  "cartesian_speed_mode_unsupported"));
  }
  if (path.fallback_policy != core::CartesianFallbackPolicyDTO::kFailClosed &&
      path.fallback_policy !=
          core::CartesianFallbackPolicyDTO::kAllowJointSpaceFallback) {
    return core::Result<roboplan::JointTrajectory>::Err(
        invalid_cartesian_request("Cartesian fallback policy is unsupported",
                                  "cartesian_fallback_policy_unsupported"));
  }
  if (path.waypoints.size() < 2 ||
      path.waypoints.size() > kMaxInputWaypoints) {
    return core::Result<roboplan::JointTrajectory>::Err(
        invalid_cartesian_request(
            "Cartesian path must contain between 2 and 4096 waypoints",
            "cartesian_waypoint_count_invalid",
            {{"waypoint_count", std::to_string(path.waypoints.size())}}));
  }
  if (!finite_positive(path.max_translation_step_m) ||
      !finite_positive(path.max_rotation_step_rad) ||
      !finite_positive(request.tolerances.translation_tol_m) ||
      !finite_positive(request.tolerances.rotation_tol_rad) ||
      !finite_positive(request.speed_profile.velocity_scale) ||
      request.speed_profile.velocity_scale > 1.0 ||
      !finite_positive(request.speed_profile.acceleration_scale) ||
      request.speed_profile.acceleration_scale > 1.0) {
    return core::Result<roboplan::JointTrajectory>::Err(
        invalid_cartesian_request(
            "Cartesian path steps, tolerances, and speed scales must be finite "
            "and within their supported positive ranges",
            "cartesian_numeric_option_invalid"));
  }
  if (request.speed_profile.max_tcp_linear_speed_mps.has_value() &&
      !finite_positive(*request.speed_profile.max_tcp_linear_speed_mps)) {
    return core::Result<roboplan::JointTrajectory>::Err(
        invalid_cartesian_request(
            "The Cartesian TCP speed cap must be finite and strictly positive",
            "cartesian_tcp_speed_limit_invalid"));
  }
  if (path.speed_mode == core::CartesianSpeedModeDTO::kBounded &&
      (!finite_positive(path.max_linear_speed_mps) ||
       !finite_positive(path.max_angular_speed_rad_s) ||
       !finite_positive(path.max_linear_acceleration_mps2) ||
       !finite_positive(path.max_angular_acceleration_rad_s2))) {
    return core::Result<roboplan::JointTrajectory>::Err(
        invalid_cartesian_request(
            "Bounded Cartesian speed and acceleration limits must be finite "
            "and strictly positive",
            "cartesian_bounded_limit_invalid"));
  }
  for (const auto& [joint_name, limits] : request.per_joint_dynamics_limits) {
    if (limits.max_jerk_rad_s3.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(core::ErrorDTO{
          core::kErrorUnsupportedCapability,
          "Cartesian planning cannot prove a requested per-joint jerk cap",
          {{"reason", "unsupported_joint_dynamics_limit"},
           {"dimension", "jerk"},
           {"joint", joint_name},
           {"failure_stage", "cartesian_request_validation"}}});
    }
    if ((limits.max_velocity_rad_s.has_value() &&
         !finite_positive(*limits.max_velocity_rad_s)) ||
        (limits.max_acceleration_rad_s2.has_value() &&
         !finite_positive(*limits.max_acceleration_rad_s2))) {
      return core::Result<roboplan::JointTrajectory>::Err(
          invalid_cartesian_request(
              "Per-joint Cartesian dynamics caps must be finite and strictly "
              "positive",
              "cartesian_joint_limit_invalid", {{"joint", joint_name}}));
    }
  }

  auto dense_path = densify_path(path, control);
  if (!dense_path.ok()) {
    return core::Result<roboplan::JointTrajectory>::Err(dense_path.error());
  }

  try {
    const auto group_info = planning_scene->getJointGroupInfo(request.group_id);
    if (!group_info.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(
          invalid_cartesian_request(
              "Cartesian planning group could not be resolved",
              "cartesian_group_unknown",
              {{"group_id", request.group_id},
               {"planner_error", group_info.error()}}));
    }
    if (!planning_scene->getFrameId(request.base_frame).has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(
          invalid_cartesian_request(
              "Cartesian base frame could not be resolved",
              "cartesian_frame_unknown",
              {{"frame_role", "base"}, {"frame", request.base_frame}}));
    }
    if (!planning_scene->getFrameId(request.tcp_frame).has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(
          invalid_cartesian_request(
              "Cartesian TCP frame could not be resolved",
              "cartesian_frame_unknown",
              {{"frame_role", "tcp"}, {"frame", request.tcp_frame}}));
    }
    if (group_info->joint_names != request.start_state.joint_names) {
      return core::Result<roboplan::JointTrajectory>::Err(
          invalid_cartesian_request(
              "Cartesian start-state joints do not match the canonical planning "
              "group order",
              "cartesian_start_joint_order_mismatch",
              {{"group_id", request.group_id}}));
    }
    const auto model_velocity_limits =
        planning_scene->getVelocityLimitVectors(request.group_id);
    const auto model_acceleration_limits =
        planning_scene->getAccelerationLimitVectors(request.group_id);
    if (!model_velocity_limits.has_value() ||
        !model_acceleration_limits.has_value() ||
        model_velocity_limits->second.size() !=
            static_cast<Eigen::Index>(group_info->joint_names.size()) ||
        model_acceleration_limits->second.size() !=
            static_cast<Eigen::Index>(group_info->joint_names.size())) {
      return core::Result<roboplan::JointTrajectory>::Err(core::ErrorDTO{
          core::kErrorUnsupportedCapability,
          "Cartesian planning requires complete model dynamics limits",
          {{"reason", "cartesian_model_dynamics_limits_unavailable"},
           {"failure_stage", "cartesian_request_validation"},
           {"group_id", request.group_id}}});
    }
    if (request.start_state.positions_rad.size() !=
        request.start_state.joint_names.size()) {
      return core::Result<roboplan::JointTrajectory>::Err(
          invalid_cartesian_request(
              "Cartesian start-state joint names and positions differ in size",
              "cartesian_start_state_size_mismatch"));
    }
    for (const auto& [joint_name, limits] :
         request.per_joint_dynamics_limits) {
      (void)limits;
      if (std::find(group_info->joint_names.begin(),
                    group_info->joint_names.end(),
                    joint_name) == group_info->joint_names.end()) {
        return core::Result<roboplan::JointTrajectory>::Err(
            core::ErrorDTO{
                core::kErrorUnsupportedCapability,
                "Cartesian dynamics cap names a joint outside the planning group",
                {{"reason", "unsupported_joint_dynamics_limit"},
                 {"joint", joint_name},
                 {"failure_stage", "cartesian_request_validation"}}});
      }
    }
    Eigen::VectorXd group_start(request.start_state.positions_rad.size());
    for (std::size_t index = 0; index < request.start_state.positions_rad.size();
         ++index) {
      if (!std::isfinite(request.start_state.positions_rad[index])) {
        return core::Result<roboplan::JointTrajectory>::Err(
            invalid_cartesian_request(
                "Cartesian start state contains a non-finite joint position",
                "cartesian_start_state_not_finite"));
      }
      group_start[static_cast<Eigen::Index>(index)] =
          request.start_state.positions_rad[index];
    }
    const Eigen::VectorXd full_start =
        planning_scene->toFullJointPositions(request.group_id, group_start);
    const Eigen::Matrix4d actual_start = planning_scene->forwardKinematics(
        full_start, request.tcp_frame, request.base_frame);
    const double start_translation_error =
        (actual_start.block<3, 1>(0, 3) -
         dense_path.value().poses.front().block<3, 1>(0, 3))
            .norm();
    const double start_rotation_error = rotation_distance(
        actual_start.block<3, 3>(0, 0),
        dense_path.value().poses.front().block<3, 3>(0, 0));
    if (start_translation_error > request.tolerances.translation_tol_m ||
        start_rotation_error > request.tolerances.rotation_tol_rad) {
      return core::Result<roboplan::JointTrajectory>::Err(
          invalid_cartesian_request(
              "Cartesian path first waypoint does not match the requested "
              "start state",
              "cartesian_start_waypoint_mismatch",
              {{"translation_error_m",
                std::to_string(start_translation_error)},
               {"rotation_error_rad", std::to_string(start_rotation_error)}}));
    }

    roboplan::CartesianPlannerOptions options;
    options.group_name = request.group_id;
    options.dt = 0.01;
    options.speed_mode = roboplan::CartesianSpeedMode::Bounded;
    options.max_linear_speed = path.max_linear_speed_mps;
    if (request.speed_profile.max_tcp_linear_speed_mps.has_value()) {
      options.max_linear_speed = std::min(
          options.max_linear_speed,
          *request.speed_profile.max_tcp_linear_speed_mps);
    }
    options.max_angular_speed = path.max_angular_speed_rad_s;
    options.max_linear_acceleration = path.max_linear_acceleration_mps2;
    options.max_angular_acceleration =
        path.max_angular_acceleration_rad_s2;
    options.max_position_error = request.tolerances.translation_tol_m;
    options.max_orientation_error = request.tolerances.rotation_tol_rad;
    options.velocity_scale = request.speed_profile.velocity_scale;
    options.acceleration_scale = request.speed_profile.acceleration_scale;
    options.limit_ratio_tolerance = 1.0;
    // A non-zero joint-space blend cannot prove the neutral Cartesian path
    // tolerance. The densified path remains the fidelity control in v1.
    options.toppra_blend_deviation = 0.0;
    options.cancellation_requested = [&control] {
      return (control.cancellation_requested &&
              control.cancellation_requested()) ||
             (control.deadline_expired && control.deadline_expired());
    };
    double planner_progress = 0.0;
    options.progress_observer = [&planner_progress](double progress) {
      planner_progress = std::clamp(progress, 0.0, 1.0);
    };

    roboplan::CartesianPathPlanner planner(planning_scene, options);
    roboplan::CartesianPath native_path(
        {request.base_frame}, {request.tcp_frame}, {dense_path.value().poses});
    roboplan::JointConfiguration native_start;
    native_start.positions = full_start;
    auto planned = planner.plan(native_path, native_start);
    if (!planned.has_value()) {
      if (auto error = planning_control_error(control, "cartesian_tracking");
          error.has_value()) {
        if (error->code == core::kErrorPlanningTimeout) {
          error->details["timeout_s"] =
              std::to_string(request.planning_timeout_s);
        }
        return core::Result<roboplan::JointTrajectory>::Err(
            std::move(*error));
      }
      return core::Result<roboplan::JointTrajectory>::Err(
          map_cartesian_backend_failure(
              planned.error(), planner_progress,
              source_waypoint_for_reference_progress(
                  dense_path.value(), planner_progress,
                  options.max_linear_speed, options.max_angular_speed)));
    }

    auto trajectory = std::move(planned.value());
    if (auto error = planning_control_error(
            control, "cartesian_output_validation");
        error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
    }
    // Raw producer output is validated before valid endpoint derivatives are
    // normalized to the neutral rest-boundary contract. Reversing this order
    // can hide NaN/Inf endpoint values supplied by an adapter.
    if (const auto output_error =
            detail::validate_and_normalize_cartesian_trajectory_output(
                trajectory, group_info->joint_names, control);
        output_error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(*output_error);
    }
    auto interpolant_path_result = detail::sample_cartesian_interpolant_path(
        request, planning_scene, trajectory,
        request.tolerances.translation_tol_m,
        request.tolerances.rotation_tol_rad, control);
    if (!interpolant_path_result.ok()) {
      return core::Result<roboplan::JointTrajectory>::Err(
          interpolant_path_result.error());
    }
    const auto& interpolant_path = interpolant_path_result.value();
    // The sampled poses are not treated as the continuous controller path.
    // The exact quintic joint-speed bound above limits how far any unsampled
    // pose can move from its preceding sample. Reserve that residual here so
    // the triangle inequality certifies the executed interpolant against the
    // caller's original tolerance, rather than only certifying waypoints.
    const double certified_translation_tolerance =
        request.tolerances.translation_tol_m -
        interpolant_path.unsampled_translation_bound_m;
    const double certified_rotation_tolerance =
        request.tolerances.rotation_tol_rad -
        interpolant_path.unsampled_rotation_bound_rad;
    if (!finite_positive(certified_translation_tolerance) ||
        !finite_positive(certified_rotation_tolerance)) {
      return core::Result<roboplan::JointTrajectory>::Err(
          cartesian_incomplete_error(
              "Cartesian continuous-path fidelity margin cannot be "
              "represented",
              "cartesian_resource_budget_exceeded", 0.0, 0U,
              {{"failure_stage", "cartesian_output_validation"},
               {"resource", "interpolant_fidelity_margin"},
               {"unsampled_translation_bound_m",
                std::to_string(
                    interpolant_path.unsampled_translation_bound_m)},
               {"unsampled_rotation_bound_rad",
                std::to_string(
                    interpolant_path.unsampled_rotation_bound_rad)}}));
    }
    auto fidelity_result = verify_cartesian_path_fidelity_controlled(
        dense_path.value().poses, interpolant_path.poses,
        certified_translation_tolerance, certified_rotation_tolerance,
        &control);
    if (fidelity_result.error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(
          std::move(*fidelity_result.error));
    }
    const auto& fidelity = fidelity_result.report;
    if (!fidelity.complete) {
      std::optional<std::size_t> first_failing_waypoint;
      if (fidelity.first_failing_path_sample_index.has_value()) {
        first_failing_waypoint = source_waypoint_for_dense_sample(
            dense_path.value(), *fidelity.first_failing_path_sample_index);
      }
      std::map<std::string, std::string> details{
          {"failure_stage", "cartesian_output_validation"},
          {"max_translation_error_m",
           std::to_string(fidelity.max_translation_error_m)},
          {"max_rotation_error_rad",
           std::to_string(fidelity.max_rotation_error_rad)},
          {"unsampled_translation_bound_m",
           std::to_string(interpolant_path.unsampled_translation_bound_m)},
          {"unsampled_rotation_bound_rad",
           std::to_string(interpolant_path.unsampled_rotation_bound_rad)}};
      if (fidelity.unresolved_rotation_error_upper_bound_rad.has_value()) {
        details["unresolved_rotation_error_upper_bound_rad"] =
            std::to_string(
                *fidelity.unresolved_rotation_error_upper_bound_rad);
      }
      return core::Result<roboplan::JointTrajectory>::Err(
          cartesian_incomplete_error(
              "Cartesian trajectory deviated from the requested path",
              "cartesian_path_tolerance_exceeded",
              fidelity.fraction_completed, first_failing_waypoint,
              std::move(details)));
    }

    auto envelope_result =
        measure_envelope(request, path, planning_scene, trajectory,
                         model_velocity_limits->second,
                         model_acceleration_limits->second, control);
    if (!envelope_result.ok()) {
      return core::Result<roboplan::JointTrajectory>::Err(
          envelope_result.error());
    }
    auto envelope = std::move(envelope_result.value());
    if (!envelope.complete) {
      return core::Result<roboplan::JointTrajectory>::Err(core::ErrorDTO{
          core::kErrorValidationLimitsViolation,
          "Cartesian trajectory does not contain a complete finite dynamics "
          "envelope",
          {{"reason", "cartesian_trajectory_envelope_incomplete"},
           {"failure_stage", "cartesian_output_validation"}}});
    }
    const double scale = required_time_scale(envelope);
    if (scale > kNumericalLimitTolerance) {
      const double applied_scale = scale * kRetimeHeadroom;
      scale_trajectory_time(trajectory, applied_scale);
      // Uniform retiming leaves the geometric path unchanged and scales every
      // certified velocity by 1/s and every certified acceleration by 1/s^2.
      // Transform the already-conservative envelope algebraically instead of
      // repeating the expensive FK interval proof under the same polynomial.
      scale_envelope_time(envelope, applied_scale);
    }
    if (auto error = planning_control_error(
            control, "cartesian_dynamics_validation");
        error.has_value()) {
      return core::Result<roboplan::JointTrajectory>::Err(std::move(*error));
    }
    if (!envelope.complete ||
        required_time_scale(envelope) > kNumericalLimitTolerance) {
      return core::Result<roboplan::JointTrajectory>::Err(core::ErrorDTO{
          core::kErrorValidationLimitsViolation,
          "Cartesian trajectory exceeds a requested speed or acceleration "
          "limit after conservative retiming",
          {{"reason", "cartesian_trajectory_limit_violation"},
           {"failure_stage", "cartesian_output_validation"},
           {"required_time_scale",
            std::to_string(required_time_scale(envelope))}}});
    }

    return core::Result<roboplan::JointTrajectory>::Ok(std::move(trajectory));
  } catch (const std::exception& error) {
    return core::Result<roboplan::JointTrajectory>::Err(core::ErrorDTO{
        core::kErrorPlanningFailed,
        "Cartesian planner adapter failed: " + std::string(error.what()),
        {{"reason", "cartesian_adapter_failure"},
         {"failure_stage", "cartesian_adapter"}}});
  }
}

}  // namespace manyforge::roboplan_backend
