#include <manyforge_roboplan/cartesian_planning_contract.hpp>
#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Eigen/Geometry>

// Pure and bounded extraction from c4b075fa6dfb69e463c61f3efa8cfce8de681e8a. The former
// plan_cartesian_path request body and RoboPlan Cartesian planner dependency
// are intentionally excluded; this file restores contract behavior only.
namespace manyforge::roboplan_backend {
namespace {

constexpr std::size_t kMaxInterpolantPathSamples = 500000;
constexpr std::size_t kPlanningControlCheckStride = 64;
constexpr double kUnsampledFidelityBudgetFraction = 0.25;

enum class PathParameterMetric {
  kEuclideanTolerance,
  kMaximumDuration,
};

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

double rotation_distance(const Eigen::Matrix3d& lhs,
                         const Eigen::Matrix3d& rhs) {
  Eigen::Quaterniond relative(lhs.transpose() * rhs);
  relative.normalize();
  return 2.0 * std::acos(std::clamp(std::abs(relative.w()), 0.0, 1.0));
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
    const CartesianTrajectoryContractInput& request,
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
  envelope.joint_derivatives.resize(joint_count);
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
      const double duration_cubed = duration_squared * duration;
      if (!finite_positive(duration_cubed)) {
        envelope.complete = false;
        return core::Result<CartesianJointDynamicsEnvelope>::Ok(
            std::move(envelope));
      }
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
        envelope.joint_derivatives[joint].velocity = std::max(
            envelope.joint_derivatives[joint].velocity, std::abs(velocity));
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
        envelope.joint_derivatives[joint].acceleration =
            std::max(envelope.joint_derivatives[joint].acceleration,
                     std::abs(acceleration));
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
      // Quintic P/V/A interpolation has quadratic jerk. Its only possible
      // interior extremum is the zero of its linear derivative (snap).
      const std::array<double, 3U> jerk_coefficients{
          6.0 * (*position_coefficients)[3], 24.0 * (*position_coefficients)[4],
          60.0 * (*position_coefficients)[5]};
      const auto record_jerk = [&](double progress) {
        const double jerk =
            evaluate_polynomial(jerk_coefficients, progress) / duration_cubed;
        if (!std::isfinite(jerk)) {
          envelope.complete = false;
          return false;
        }
        envelope.joint_derivatives[joint].jerk =
            std::max(envelope.joint_derivatives[joint].jerk, std::abs(jerk));
        return true;
      };
      if (!record_jerk(0.0) || !record_jerk(1.0)) {
        return core::Result<CartesianJointDynamicsEnvelope>::Ok(
            std::move(envelope));
      }
      if (jerk_coefficients[2] != 0.0) {
        const double extremum =
            -0.5 * (jerk_coefficients[1] / jerk_coefficients[2]);
        if (extremum > 0.0 && extremum < 1.0 && !record_jerk(extremum)) {
          return core::Result<CartesianJointDynamicsEnvelope>::Ok(
              std::move(envelope));
        }
      }
    }
    auto &bounds = envelope.joint_derivatives[joint];
    // Numerical headroom for the existing bounded root solver and coefficient
    // evaluation. Zero derivatives remain zero (stationary joints add no cap).
    bounds.velocity *= 1.0 + 1e-12;
    bounds.acceleration *= 1.0 + 1e-12;
    bounds.jerk *= 1.0 + 1e-12;
    if (!std::isfinite(bounds.velocity) ||
        !std::isfinite(bounds.acceleration) || !std::isfinite(bounds.jerk)) {
      envelope.complete = false;
      return core::Result<CartesianJointDynamicsEnvelope>::Ok(
          std::move(envelope));
    }
  }
  return core::Result<CartesianJointDynamicsEnvelope>::Ok(
      std::move(envelope));
}

core::Result<CartesianInterpolantPath> sample_cartesian_interpolant_path(
    const CartesianTrajectoryContractInput& request,
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
      scene, request.compiled_group_name, trajectory.joint_names);
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
  result.times_s.reserve(result.poses.capacity());
  const auto append_pose = [&](const Eigen::VectorXd& group_positions, double time_s)
      -> std::optional<core::ErrorDTO> {
    if (!group_positions.allFinite() || !std::isfinite(time_s) || time_s < 0.0 ||
        (!result.times_s.empty() && time_s <= result.times_s.back())) {
      return core::ErrorDTO{
          core::kErrorCartesianIncomplete,
          "Cartesian trajectory interpolant has invalid positions or sample times",
          {{"reason", "cartesian_trajectory_interpolant_invalid"},
           {"failure_stage", "cartesian_output_validation"}}};
    }
    const auto full_positions =
        scene->toFullJointPositions(request.compiled_group_name,
                                    group_positions);
    const auto pose = scene->forwardKinematics(
        full_positions, request.compiled_tcp_frame,
        request.compiled_base_frame);
    if (!pose.allFinite()) {
      return core::ErrorDTO{
          core::kErrorCartesianIncomplete,
          "Cartesian trajectory interpolant FK is not finite",
          {{"reason", "cartesian_trajectory_interpolant_invalid"},
           {"failure_stage", "cartesian_output_validation"}}};
    }
    result.poses.push_back(pose);
    result.times_s.push_back(time_s);
    return std::nullopt;
  };
  if (auto error = append_pose(trajectory.positions.front(), trajectory.times.front());
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
      const double time_s = step == subdivisions
                                ? trajectory.times[sample]
                                : trajectory.times[sample - 1] + duration *
                                      (static_cast<double>(step) / static_cast<double>(subdivisions));
      if (auto error = append_pose(group_positions, time_s); error.has_value()) {
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

ControlledCartesianPathFidelityResult verify_cartesian_path_fidelity_impl(
    std::span<const Eigen::Matrix4d> requested_path,
    std::span<const Eigen::Matrix4d> actual_path,
    double translation_tolerance_m, double rotation_tolerance_rad,
    const CartesianPlanningControl *control,
    std::span<const std::size_t> waypoint_indices = {}) {
  ControlledCartesianPathFidelityResult result;
  auto &report = result.report;
  if (waypoint_indices.size() > 4096U ||
      !std::is_sorted(waypoint_indices.begin(), waypoint_indices.end()) ||
      (!waypoint_indices.empty() &&
       waypoint_indices.back() >= requested_path.size())) {
    result.error = cartesian_incomplete_error(
        "Cartesian waypoint correspondence indices are invalid",
        "cartesian_waypoint_correspondence_invalid", 0.0, 0U);
    return result;
  }
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
    report.waypoint_actual_sample_indices.reserve(waypoint_indices.size());
    for (const auto index : waypoint_indices) {
      // The existing fidelity proof establishes this ordered correspondence.
      // Retain the upper actual sample, not an interpolated time estimate.
      const auto match =
          std::lower_bound(actual_parameter->begin(), actual_parameter->end(),
                           (*requested_parameter)[index]);
      report.waypoint_actual_sample_indices.push_back(
          index == 0U ? 0U
          : index + 1U == requested_path.size() ||
                  match == actual_parameter->end()
              ? actual_path.size() - 1U
              : static_cast<std::size_t>(match - actual_parameter->begin()));
    }
  }
  return result;
}

} // namespace

CartesianPathFidelityReport
verify_cartesian_path_fidelity(std::span<const Eigen::Matrix4d> requested_path,
                               std::span<const Eigen::Matrix4d> actual_path,
                               double translation_tolerance_m,
                               double rotation_tolerance_rad) {
  return verify_cartesian_path_fidelity_impl(
             requested_path, actual_path, translation_tolerance_m,
             rotation_tolerance_rad, nullptr)
      .report;
}

core::Result<CartesianPathFidelityReport>
verify_cartesian_path_fidelity_controlled(
    std::span<const Eigen::Matrix4d> requested_path,
    std::span<const Eigen::Matrix4d> actual_path,
    double translation_tolerance_m, double rotation_tolerance_rad,
    const CartesianPlanningControl& control,
    std::span<const std::size_t> waypoint_indices) {
  auto result = verify_cartesian_path_fidelity_impl(
      requested_path, actual_path, translation_tolerance_m,
      rotation_tolerance_rad, &control, waypoint_indices);
  if (result.error.has_value()) {
    return core::Result<CartesianPathFidelityReport>::Err(
        std::move(*result.error));
  }
  return core::Result<CartesianPathFidelityReport>::Ok(
      std::move(result.report));
}


}  // namespace manyforge::roboplan_backend
