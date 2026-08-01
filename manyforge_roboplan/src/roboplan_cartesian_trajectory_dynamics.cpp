#include <manyforge_roboplan/detail/cartesian_trajectory_contract.hpp>

// Extracted unchanged from c4b075fa6dfb69e463c61f3efa8cfce8de681e8a; contains no planner or scene owner.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Core>
#include <pinocchio/algorithm/frames.hpp>
#include <pinocchio/algorithm/kinematics.hpp>

namespace manyforge::roboplan_backend::detail {
namespace {

constexpr std::size_t kMaxDynamicsIntervalChecks = 500000;
constexpr std::size_t kMaxDynamicsSubdivisionDepth = 40;
constexpr std::size_t kPlanningControlCheckStride = 64;
constexpr long double kFloatingPointBoundHeadroom = 1.0L + 1e-12L;
// The accepted envelope remains fully conservative. This value only controls
// how tightly branch-and-bound resolves the global conservative peak. The
// interval residual remains part of the accepted envelope, so this affects
// retiming tightness rather than safety. Intervals whose upper bound cannot
// exceed the best observed value by two percent of the limit are pruned. This
// caps avoidable speed retiming at two percent (about one percent for an
// acceleration-bound trajectory) without turning certification into the
// dominant Cartesian-planning cost.
constexpr long double kResidualBudgetFraction = 0.02L;

bool finite_positive(double value) {
  return std::isfinite(value) &&
         value >= std::numeric_limits<double>::min();
}

std::optional<core::ErrorDTO> planning_control_error(
    const CartesianPlanningControl& control) {
  if (control.cancellation_requested && control.cancellation_requested()) {
    return core::ErrorDTO{
        core::kErrorPlanningFailed,
        "Cartesian dynamics certification cancelled by interrupt",
        {{"reason", "planning_cancelled_by_interrupt"},
         {"failure_stage", "cartesian_dynamics_validation"}}};
  }
  if (control.deadline_expired && control.deadline_expired()) {
    return core::ErrorDTO{
        core::kErrorPlanningTimeout,
        "Cartesian dynamics certification timed out",
        {{"reason", "planning_timeout"},
         {"failure_stage", "cartesian_dynamics_validation"}}};
  }
  return std::nullopt;
}

core::ErrorDTO dynamics_resource_error(std::size_t segment_index,
                                       std::size_t interval_checks) {
  return core::ErrorDTO{
      core::kErrorCartesianIncomplete,
      "Cartesian dynamics could not be certified within the bounded work "
      "budget",
      {{"reason", "cartesian_resource_budget_exceeded"},
       {"failure_stage", "cartesian_dynamics_validation"},
       {"resource", "cartesian_dynamics_interval_checks"},
       {"max_interval_checks",
        std::to_string(kMaxDynamicsIntervalChecks)},
       {"interval_checks", std::to_string(interval_checks)},
       {"segment_index", std::to_string(segment_index)}}};
}

template <std::size_t N>
long double evaluate_polynomial(const std::array<double, N>& coefficients,
                                long double value) {
  long double result = 0.0L;
  for (auto coefficient = coefficients.rbegin();
       coefficient != coefficients.rend(); ++coefficient) {
    result = result * value + static_cast<long double>(*coefficient);
  }
  return result;
}

constexpr long double binomial(std::size_t n, std::size_t k) {
  if (k > n) {
    return 0.0L;
  }
  if (k > n - k) {
    k = n - k;
  }
  long double result = 1.0L;
  for (std::size_t index = 1; index <= k; ++index) {
    result *= static_cast<long double>(n - (k - index));
    result /= static_cast<long double>(index);
  }
  return result;
}

// Re-express p(u) as p(lower + (upper-lower)*s), then apply the triangle
// inequality for s in [0, 1]. Unlike point sampling, this is a true interval
// upper bound and converges as adaptive subdivision narrows the interval.
template <std::size_t N>
long double polynomial_absolute_interval_bound(
    const std::array<double, N>& coefficients, long double lower,
    long double upper) {
  const long double width = upper - lower;
  long double result = 0.0L;
  long double width_power = 1.0L;
  for (std::size_t local_power = 0; local_power < N; ++local_power) {
    long double local_coefficient = 0.0L;
    for (std::size_t source_power = local_power; source_power < N;
         ++source_power) {
      local_coefficient +=
          static_cast<long double>(coefficients[source_power]) *
          binomial(source_power, local_power) *
          std::pow(lower, static_cast<int>(source_power - local_power));
    }
    result += std::abs(local_coefficient * width_power);
    width_power *= width;
  }
  return result * kFloatingPointBoundHeadroom;
}

std::optional<std::array<double, 6>> quintic_position_coefficients(
    double position_from, double position_to, double velocity_from,
    double velocity_to, double acceleration_from, double acceleration_to,
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
  const double scaled_acceleration_to = duration_squared * acceleration_to;
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

double conservative_model_motion_radius(const roboplan::Scene& scene) {
  long double joint_offset_sum = 0.0L;
  for (const auto& placement : scene.getModel().jointPlacements) {
    joint_offset_sum +=
        static_cast<long double>(placement.translation().norm());
  }
  for (const auto& joint_name : scene.getJointNamesWithMimics()) {
    const auto info = scene.getJointInfo(joint_name);
    if (!info.has_value() || info->type != roboplan::JointType::PRISMATIC) {
      continue;
    }
    if (info->limits.min_position.size() != 1 ||
        info->limits.max_position.size() != 1 ||
        !std::isfinite(info->limits.min_position[0]) ||
        !std::isfinite(info->limits.max_position[0])) {
      return std::numeric_limits<double>::infinity();
    }
    joint_offset_sum += static_cast<long double>(std::max(
        std::abs(info->limits.min_position[0]),
        std::abs(info->limits.max_position[0])));
  }
  long double largest_frame_offset = 0.0L;
  long double second_largest_frame_offset = 0.0L;
  for (const auto& frame : scene.getModel().frames) {
    const auto offset =
        static_cast<long double>(frame.placement.translation().norm());
    if (offset >= largest_frame_offset) {
      second_largest_frame_offset = largest_frame_offset;
      largest_frame_offset = offset;
    } else {
      second_largest_frame_offset =
          std::max(second_largest_frame_offset, offset);
    }
  }
  const long double radius =
      std::max(1.0L, joint_offset_sum + largest_frame_offset +
                         second_largest_frame_offset);
  if (!std::isfinite(radius) ||
      radius > static_cast<long double>(std::numeric_limits<double>::max())) {
    return std::numeric_limits<double>::infinity();
  }
  return static_cast<double>(radius);
}

std::optional<CartesianKinematicMotionBounds> resolve_motion_amplification(
    const roboplan::Scene& scene,
    const std::vector<std::string>& trajectory_joint_names) {
  CartesianKinematicMotionBounds result;
  result.angular_amplification.assign(trajectory_joint_names.size(), 0.0L);
  result.prismatic_amplification.assign(trajectory_joint_names.size(), 0.0L);
  for (std::size_t joint = 0; joint < trajectory_joint_names.size(); ++joint) {
    const auto info = scene.getJointInfo(trajectory_joint_names[joint]);
    if (!info.has_value() || info->mimic_info.has_value() ||
        info->num_position_dofs != 1 || info->num_velocity_dofs != 1) {
      return std::nullopt;
    }
    if (info->type == roboplan::JointType::PRISMATIC) {
      result.prismatic_amplification[joint] = 1.0L;
    } else if (info->type == roboplan::JointType::REVOLUTE ||
               info->type == roboplan::JointType::CONTINUOUS) {
      result.angular_amplification[joint] = 1.0L;
    } else {
      return std::nullopt;
    }
  }

  const auto& model_joint_names = scene.getJointNamesWithMimics();
  for (const auto& joint_name : model_joint_names) {
    const auto info = scene.getJointInfo(joint_name);
    if (!info.has_value() || !info->mimic_info.has_value()) {
      continue;
    }
    const bool mimic_is_prismatic =
        info->type == roboplan::JointType::PRISMATIC;
    if (!mimic_is_prismatic &&
        info->type != roboplan::JointType::REVOLUTE &&
        info->type != roboplan::JointType::CONTINUOUS) {
      return std::nullopt;
    }
    long double scale =
        static_cast<long double>(info->mimic_info->scaling);
    std::string source = info->mimic_info->mimicked_joint_name;
    if (!std::isfinite(scale) ||
        !std::isfinite(info->mimic_info->offset)) {
      return std::nullopt;
    }
    std::vector<std::string> visited{joint_name};
    bool resolved = false;
    for (std::size_t depth = 0; depth <= model_joint_names.size(); ++depth) {
      if (std::find(visited.begin(), visited.end(), source) != visited.end()) {
        return std::nullopt;
      }
      visited.push_back(source);
      const auto match = std::find(trajectory_joint_names.begin(),
                                   trajectory_joint_names.end(), source);
      if (match != trajectory_joint_names.end()) {
        const auto index = static_cast<std::size_t>(
            std::distance(trajectory_joint_names.begin(), match));
        if (mimic_is_prismatic) {
          result.prismatic_amplification[index] += std::abs(scale);
        } else {
          result.angular_amplification[index] += std::abs(scale);
        }
        resolved = true;
        break;
      }
      const auto source_info = scene.getJointInfo(source);
      if (!source_info.has_value()) {
        return std::nullopt;
      }
      if (!source_info->mimic_info.has_value()) {
        // The independent source is outside this planned group and is fixed
        // throughout the candidate trajectory.
        resolved = true;
        break;
      }
      if (!std::isfinite(source_info->mimic_info->scaling) ||
          !std::isfinite(source_info->mimic_info->offset)) {
        return std::nullopt;
      }
      scale *=
          static_cast<long double>(source_info->mimic_info->scaling);
      source = source_info->mimic_info->mimicked_joint_name;
      if (!std::isfinite(scale)) {
        return std::nullopt;
      }
    }
    if (!resolved) {
      return std::nullopt;
    }
  }
  result.model_radius_m = conservative_model_motion_radius(scene);
  if (!finite_positive(result.model_radius_m)) {
    return std::nullopt;
  }
  return result;
}

struct FrameDynamicsSample {
  double linear_speed_mps = 0.0;
  double angular_speed_rad_s = 0.0;
  double linear_acceleration_mps2 = 0.0;
  double angular_acceleration_rad_s2 = 0.0;
};

struct KinematicDerivativeBounds {
  long double linear_acceleration_mps2 = 0.0L;
  long double angular_acceleration_rad_s2 = 0.0L;
  long double linear_jerk_mps3 = 0.0L;
  long double angular_jerk_rad_s3 = 0.0L;
};

struct SegmentPolynomial {
  std::vector<std::array<double, 6>> positions;
  double duration_s = 0.0;
};

bool finite_sample(const FrameDynamicsSample& sample) {
  return std::isfinite(sample.linear_speed_mps) &&
         std::isfinite(sample.angular_speed_rad_s) &&
         std::isfinite(sample.linear_acceleration_mps2) &&
         std::isfinite(sample.angular_acceleration_rad_s2);
}

}  // namespace

core::Result<CartesianKinematicMotionBounds>
resolve_cartesian_kinematic_motion_bounds(
    const std::shared_ptr<roboplan::Scene>& scene,
    const std::string& group_id,
    const std::vector<std::string>& trajectory_joint_names) {
  const auto fail = [](std::string message,
                       std::map<std::string, std::string> details = {}) {
    details["reason"] =
        "cartesian_trajectory_interpolant_joint_mapping_invalid";
    details["failure_stage"] = "cartesian_output_validation";
    return core::Result<CartesianKinematicMotionBounds>::Err(
        core::ErrorDTO{core::kErrorCartesianIncomplete, std::move(message),
                       std::move(details)});
  };
  if (!scene || trajectory_joint_names.empty()) {
    return fail("Cartesian kinematic motion model is unavailable");
  }
  const auto group_info = scene->getJointGroupInfo(group_id);
  if (!group_info.has_value() ||
      group_info->joint_names != trajectory_joint_names ||
      group_info->nq_collapsed != trajectory_joint_names.size() ||
      group_info->q_indices.size() !=
          static_cast<Eigen::Index>(trajectory_joint_names.size()) ||
      group_info->v_indices.size() !=
          static_cast<Eigen::Index>(trajectory_joint_names.size())) {
    return fail("Cartesian trajectory cannot be mapped to one scalar per "
                "qualified group joint",
                {{"group_id", group_id}});
  }
  auto resolved =
      resolve_motion_amplification(*scene, trajectory_joint_names);
  if (!resolved.has_value()) {
    return fail("Cartesian trajectory mimic chain or kinematic radius cannot "
                "be conservatively bounded",
                {{"group_id", group_id}});
  }
  return core::Result<CartesianKinematicMotionBounds>::Ok(
      std::move(*resolved));
}

core::Result<CartesianFrameDynamicsEnvelope>
measure_cartesian_frame_dynamics_envelope(
    const core::PlanRequestDTO& request, const core::CartesianPathDTO& path,
    const std::shared_ptr<roboplan::Scene>& scene,
    const roboplan::JointTrajectory& trajectory,
    const CartesianPlanningControl& control) {
  CartesianFrameDynamicsEnvelope envelope;
  const std::size_t joint_count = trajectory.joint_names.size();
  const std::size_t sample_count = trajectory.positions.size();
  if (!scene || joint_count == 0 || sample_count < 2 ||
      trajectory.times.size() != sample_count ||
      trajectory.velocities.size() != sample_count ||
      trajectory.accelerations.size() != sample_count) {
    envelope.complete = false;
    return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
        std::move(envelope));
  }
  const auto group_info = scene->getJointGroupInfo(request.group_id);
  const auto tip_frame_id = scene->getFrameId(request.tcp_frame);
  const auto base_frame_id = scene->getFrameId(request.base_frame);
  if (!group_info.has_value() || !tip_frame_id.has_value() ||
      !base_frame_id.has_value() ||
      group_info->joint_names != trajectory.joint_names ||
      group_info->q_indices.size() !=
          static_cast<Eigen::Index>(joint_count) ||
      group_info->v_indices.size() !=
          static_cast<Eigen::Index>(joint_count)) {
    envelope.complete = false;
    return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
        std::move(envelope));
  }
  auto amplification = resolve_cartesian_kinematic_motion_bounds(
      scene, request.group_id, trajectory.joint_names);
  if (!amplification.ok()) {
    return core::Result<CartesianFrameDynamicsEnvelope>::Err(
        amplification.error());
  }
  const auto& motion_bounds = amplification.value();

  double linear_speed_limit = path.max_linear_speed_mps;
  if (request.speed_profile.max_tcp_linear_speed_mps.has_value()) {
    linear_speed_limit = std::min(
        linear_speed_limit,
        *request.speed_profile.max_tcp_linear_speed_mps);
  }
  linear_speed_limit *= request.speed_profile.velocity_scale;
  const double angular_speed_limit =
      path.max_angular_speed_rad_s * request.speed_profile.velocity_scale;
  const double linear_acceleration_limit =
      path.max_linear_acceleration_mps2 *
      request.speed_profile.acceleration_scale;
  const double angular_acceleration_limit =
      path.max_angular_acceleration_rad_s2 *
      request.speed_profile.acceleration_scale;
  if (!finite_positive(linear_speed_limit) ||
      !finite_positive(angular_speed_limit) ||
      !finite_positive(linear_acceleration_limit) ||
      !finite_positive(angular_acceleration_limit)) {
    envelope.complete = false;
    return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
        std::move(envelope));
  }

  const auto& model = scene->getModel();
  pinocchio::Data data(model);
  Eigen::VectorXd group_positions(static_cast<Eigen::Index>(joint_count));
  Eigen::VectorXd group_velocities(static_cast<Eigen::Index>(joint_count));
  Eigen::VectorXd group_accelerations(static_cast<Eigen::Index>(joint_count));
  Eigen::VectorXd full_velocities = Eigen::VectorXd::Zero(model.nv);
  Eigen::VectorXd full_accelerations = Eigen::VectorXd::Zero(model.nv);
  std::size_t sample_evaluations = 0;
  FrameDynamicsSample observed_peak;
  const auto record_observed = [&](const FrameDynamicsSample& sample) {
    observed_peak.linear_speed_mps =
        std::max(observed_peak.linear_speed_mps, sample.linear_speed_mps);
    observed_peak.angular_speed_rad_s =
        std::max(observed_peak.angular_speed_rad_s,
                 sample.angular_speed_rad_s);
    observed_peak.linear_acceleration_mps2 =
        std::max(observed_peak.linear_acceleration_mps2,
                 sample.linear_acceleration_mps2);
    observed_peak.angular_acceleration_rad_s2 =
        std::max(observed_peak.angular_acceleration_rad_s2,
                 sample.angular_acceleration_rad_s2);
  };

  for (std::size_t segment_index = 0; segment_index + 1 < sample_count;
       ++segment_index) {
    if (auto error = planning_control_error(control); error.has_value()) {
      return core::Result<CartesianFrameDynamicsEnvelope>::Err(
          std::move(*error));
    }
    SegmentPolynomial segment;
    segment.duration_s = trajectory.times[segment_index + 1] -
                         trajectory.times[segment_index];
    if (!finite_positive(segment.duration_s)) {
      envelope.complete = false;
      return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
          std::move(envelope));
    }
    segment.positions.reserve(joint_count);
    for (std::size_t joint = 0; joint < joint_count; ++joint) {
      const Eigen::Index index = static_cast<Eigen::Index>(joint);
      if (trajectory.positions[segment_index].size() !=
              static_cast<Eigen::Index>(joint_count) ||
          trajectory.positions[segment_index + 1].size() !=
              static_cast<Eigen::Index>(joint_count) ||
          trajectory.velocities[segment_index].size() !=
              static_cast<Eigen::Index>(joint_count) ||
          trajectory.velocities[segment_index + 1].size() !=
              static_cast<Eigen::Index>(joint_count) ||
          trajectory.accelerations[segment_index].size() !=
              static_cast<Eigen::Index>(joint_count) ||
          trajectory.accelerations[segment_index + 1].size() !=
              static_cast<Eigen::Index>(joint_count)) {
        envelope.complete = false;
        return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
            std::move(envelope));
      }
      auto coefficients = quintic_position_coefficients(
          trajectory.positions[segment_index][index],
          trajectory.positions[segment_index + 1][index],
          trajectory.velocities[segment_index][index],
          trajectory.velocities[segment_index + 1][index],
          trajectory.accelerations[segment_index][index],
          trajectory.accelerations[segment_index + 1][index],
          segment.duration_s);
      if (!coefficients.has_value()) {
        envelope.complete = false;
        return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
            std::move(envelope));
      }
      segment.positions.push_back(*coefficients);
    }

    const auto evaluate_sample = [&](double progress)
        -> std::optional<FrameDynamicsSample> {
      if (sample_evaluations++ >= kMaxDynamicsIntervalChecks) {
        return std::nullopt;
      }
      const long double normalized_time =
          static_cast<long double>(progress);
      const long double duration =
          static_cast<long double>(segment.duration_s);
      const long double duration_squared = duration * duration;
      for (std::size_t joint = 0; joint < joint_count; ++joint) {
        const auto& coefficients = segment.positions[joint];
        const std::array<double, 5> velocity_coefficients{
            coefficients[1], 2.0 * coefficients[2],
            3.0 * coefficients[3], 4.0 * coefficients[4],
            5.0 * coefficients[5]};
        const std::array<double, 4> acceleration_coefficients{
            2.0 * coefficients[2], 6.0 * coefficients[3],
            12.0 * coefficients[4], 20.0 * coefficients[5]};
        group_positions[static_cast<Eigen::Index>(joint)] =
            static_cast<double>(
                evaluate_polynomial(coefficients, normalized_time));
        group_velocities[static_cast<Eigen::Index>(joint)] =
            static_cast<double>(evaluate_polynomial(
                                    velocity_coefficients, normalized_time) /
                                duration);
        group_accelerations[static_cast<Eigen::Index>(joint)] =
            static_cast<double>(evaluate_polynomial(
                                    acceleration_coefficients,
                                    normalized_time) /
                                duration_squared);
      }
      if (!group_positions.allFinite() || !group_velocities.allFinite() ||
          !group_accelerations.allFinite()) {
        return std::nullopt;
      }
      const auto full_positions =
          scene->toFullJointPositions(request.group_id, group_positions);
      full_velocities.setZero();
      full_accelerations.setZero();
      for (Eigen::Index joint = 0;
           joint < static_cast<Eigen::Index>(joint_count); ++joint) {
        const Eigen::Index velocity_index = group_info->v_indices[joint];
        if (velocity_index < 0 || velocity_index >= model.nv) {
          return std::nullopt;
        }
        full_velocities[velocity_index] = group_velocities[joint];
        full_accelerations[velocity_index] = group_accelerations[joint];
      }
      pinocchio::forwardKinematics(model, data, full_positions,
                                   full_velocities, full_accelerations);
      pinocchio::updateFramePlacements(model, data);
      const auto tip_velocity = pinocchio::getFrameVelocity(
          model, data, tip_frame_id.value(),
          pinocchio::LOCAL_WORLD_ALIGNED);
      const auto base_velocity = pinocchio::getFrameVelocity(
          model, data, base_frame_id.value(),
          pinocchio::LOCAL_WORLD_ALIGNED);
      const auto tip_acceleration = pinocchio::getFrameClassicalAcceleration(
          model, data, tip_frame_id.value(),
          pinocchio::LOCAL_WORLD_ALIGNED);
      const auto base_acceleration =
          pinocchio::getFrameClassicalAcceleration(
              model, data, base_frame_id.value(),
              pinocchio::LOCAL_WORLD_ALIGNED);

      const Eigen::Vector3d displacement =
          data.oMf[tip_frame_id.value()].translation() -
          data.oMf[base_frame_id.value()].translation();
      const Eigen::Vector3d velocity_difference =
          tip_velocity.linear() - base_velocity.linear();
      const Eigen::Vector3d relative_linear_velocity =
          velocity_difference -
          base_velocity.angular().cross(displacement);
      const Eigen::Vector3d relative_angular_velocity =
          tip_velocity.angular() - base_velocity.angular();
      // Differentiate T_base^-1*T_tip in a world-aligned basis. Norms are
      // rotation invariant, while these transport terms are required when the
      // requested base frame itself moves.
      const Eigen::Vector3d relative_linear_acceleration =
          tip_acceleration.linear() - base_acceleration.linear() -
          base_acceleration.angular().cross(displacement) -
          2.0 * base_velocity.angular().cross(velocity_difference) +
          base_velocity.angular().cross(
              base_velocity.angular().cross(displacement));
      const Eigen::Vector3d relative_angular_acceleration =
          tip_acceleration.angular() - base_acceleration.angular() -
          base_velocity.angular().cross(relative_angular_velocity);
      FrameDynamicsSample result{
          relative_linear_velocity.norm(),
          relative_angular_velocity.norm(),
          relative_linear_acceleration.norm(),
          relative_angular_acceleration.norm()};
      if (!finite_sample(result)) {
        return std::nullopt;
      }
      return result;
    };

    const auto derivative_bounds =
        [&](double lower, double upper)
        -> std::optional<KinematicDerivativeBounds> {
      long double angular_speed = 0.0L;
      long double angular_acceleration = 0.0L;
      long double angular_jerk = 0.0L;
      long double prismatic_speed = 0.0L;
      long double prismatic_acceleration = 0.0L;
      long double prismatic_jerk = 0.0L;
      const long double duration =
          static_cast<long double>(segment.duration_s);
      const long double duration_squared = duration * duration;
      const long double duration_cubed = duration_squared * duration;
      for (std::size_t joint = 0; joint < joint_count; ++joint) {
        const auto& coefficients = segment.positions[joint];
        const std::array<double, 5> velocity_coefficients{
            coefficients[1], 2.0 * coefficients[2],
            3.0 * coefficients[3], 4.0 * coefficients[4],
            5.0 * coefficients[5]};
        const std::array<double, 4> acceleration_coefficients{
            2.0 * coefficients[2], 6.0 * coefficients[3],
            12.0 * coefficients[4], 20.0 * coefficients[5]};
        const std::array<double, 3> jerk_coefficients{
            6.0 * coefficients[3], 24.0 * coefficients[4],
            60.0 * coefficients[5]};
        const long double speed = polynomial_absolute_interval_bound(
                                      velocity_coefficients, lower, upper) /
                                  duration;
        const long double acceleration =
            polynomial_absolute_interval_bound(acceleration_coefficients,
                                               lower, upper) /
            duration_squared;
        const long double jerk = polynomial_absolute_interval_bound(
                                     jerk_coefficients, lower, upper) /
                                 duration_cubed;
        angular_speed +=
            speed * motion_bounds.angular_amplification[joint];
        angular_acceleration +=
            acceleration * motion_bounds.angular_amplification[joint];
        angular_jerk +=
            jerk * motion_bounds.angular_amplification[joint];
        prismatic_speed +=
            speed * motion_bounds.prismatic_amplification[joint];
        prismatic_acceleration +=
            acceleration * motion_bounds.prismatic_amplification[joint];
        prismatic_jerk +=
            jerk * motion_bounds.prismatic_amplification[joint];
      }
      const long double angular_acceleration_bound =
          angular_acceleration + angular_speed * angular_speed;
      const long double angular_jerk_bound =
          angular_jerk +
          3.0L * angular_speed * angular_acceleration +
          angular_speed * angular_speed * angular_speed;
      const long double radius =
          static_cast<long double>(motion_bounds.model_radius_m);
      KinematicDerivativeBounds result;
      result.angular_acceleration_rad_s2 = angular_acceleration_bound;
      result.angular_jerk_rad_s3 = angular_jerk_bound;
      result.linear_acceleration_mps2 =
          prismatic_acceleration +
          2.0L * angular_speed * prismatic_speed +
          radius * angular_acceleration_bound;
      result.linear_jerk_mps3 =
          prismatic_jerk +
          6.0L * angular_speed * prismatic_acceleration +
          6.0L * angular_acceleration_bound * prismatic_speed +
          radius * angular_jerk_bound;
      if (!std::isfinite(result.linear_acceleration_mps2) ||
          !std::isfinite(result.angular_acceleration_rad_s2) ||
          !std::isfinite(result.linear_jerk_mps3) ||
          !std::isfinite(result.angular_jerk_rad_s3)) {
        return std::nullopt;
      }
      return result;
    };

    std::array<std::optional<FrameDynamicsSample>, 5> seed_samples{
        evaluate_sample(0.0), evaluate_sample(0.25), evaluate_sample(0.5),
        evaluate_sample(0.75), evaluate_sample(1.0)};
    if (!std::all_of(seed_samples.begin(), seed_samples.end(),
                     [](const auto& sample) { return sample.has_value(); })) {
      if (sample_evaluations > kMaxDynamicsIntervalChecks) {
        return core::Result<CartesianFrameDynamicsEnvelope>::Err(
            dynamics_resource_error(segment_index,
                                    envelope.interval_checks));
      }
      envelope.complete = false;
      return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
          std::move(envelope));
    }
    for (const auto& sample : seed_samples) {
      record_observed(*sample);
    }

    struct Interval {
      double lower = 0.0;
      double upper = 1.0;
      FrameDynamicsSample lower_sample;
      FrameDynamicsSample upper_sample;
      std::size_t depth = 0;
    };
    constexpr std::array<double, 5> kSeedProgress{0.0, 0.25, 0.5, 0.75,
                                                  1.0};
    std::vector<Interval> pending;
    pending.reserve(4);
    for (std::size_t seed = 1; seed < kSeedProgress.size(); ++seed) {
      pending.push_back(Interval{kSeedProgress[seed - 1],
                                 kSeedProgress[seed],
                                 *seed_samples[seed - 1],
                                 *seed_samples[seed], 0});
    }
    while (!pending.empty()) {
      Interval interval = std::move(pending.back());
      pending.pop_back();
      if (++envelope.interval_checks > kMaxDynamicsIntervalChecks) {
        return core::Result<CartesianFrameDynamicsEnvelope>::Err(
            dynamics_resource_error(segment_index,
                                    envelope.interval_checks));
      }
      if (envelope.interval_checks % kPlanningControlCheckStride == 0) {
        if (auto error = planning_control_error(control); error.has_value()) {
          return core::Result<CartesianFrameDynamicsEnvelope>::Err(
              std::move(*error));
        }
      }
      const auto bounds = derivative_bounds(interval.lower, interval.upper);
      if (!bounds.has_value()) {
        envelope.complete = false;
        return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
            std::move(envelope));
      }
      const long double half_interval_s =
          0.5L * static_cast<long double>(segment.duration_s) *
          static_cast<long double>(interval.upper - interval.lower);
      const long double linear_speed_residual =
          bounds->linear_acceleration_mps2 * half_interval_s;
      const long double angular_speed_residual =
          bounds->angular_acceleration_rad_s2 * half_interval_s;
      const long double linear_acceleration_residual =
          bounds->linear_jerk_mps3 * half_interval_s;
      const long double angular_acceleration_residual =
          bounds->angular_jerk_rad_s3 * half_interval_s;
      const long double linear_speed_upper =
          std::max(interval.lower_sample.linear_speed_mps,
                   interval.upper_sample.linear_speed_mps) +
          linear_speed_residual;
      const long double angular_speed_upper =
          std::max(interval.lower_sample.angular_speed_rad_s,
                   interval.upper_sample.angular_speed_rad_s) +
          angular_speed_residual;
      const long double linear_acceleration_upper =
          std::max(interval.lower_sample.linear_acceleration_mps2,
                   interval.upper_sample.linear_acceleration_mps2) +
          linear_acceleration_residual;
      const long double angular_acceleration_upper =
          std::max(interval.lower_sample.angular_acceleration_rad_s2,
                   interval.upper_sample.angular_acceleration_rad_s2) +
          angular_acceleration_residual;
      const bool sufficiently_resolved =
          linear_speed_upper <=
              observed_peak.linear_speed_mps +
                  kResidualBudgetFraction * linear_speed_limit &&
          angular_speed_upper <=
              observed_peak.angular_speed_rad_s +
                  kResidualBudgetFraction * angular_speed_limit &&
          linear_acceleration_upper <=
              observed_peak.linear_acceleration_mps2 +
                  kResidualBudgetFraction * linear_acceleration_limit &&
          angular_acceleration_upper <=
              observed_peak.angular_acceleration_rad_s2 +
                  kResidualBudgetFraction * angular_acceleration_limit;
      if (!sufficiently_resolved) {
        const double midpoint =
            interval.lower + (interval.upper - interval.lower) * 0.5;
        if (interval.depth >= kMaxDynamicsSubdivisionDepth ||
            midpoint <= interval.lower || midpoint >= interval.upper) {
          return core::Result<CartesianFrameDynamicsEnvelope>::Err(
              dynamics_resource_error(segment_index,
                                      envelope.interval_checks));
        }
        const auto midpoint_sample = evaluate_sample(midpoint);
        if (!midpoint_sample.has_value()) {
          if (sample_evaluations > kMaxDynamicsIntervalChecks) {
            return core::Result<CartesianFrameDynamicsEnvelope>::Err(
                dynamics_resource_error(segment_index,
                                        envelope.interval_checks));
          }
          envelope.complete = false;
          return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
              std::move(envelope));
        }
        record_observed(*midpoint_sample);
        pending.push_back(Interval{midpoint, interval.upper,
                                   *midpoint_sample, interval.upper_sample,
                                   interval.depth + 1});
        pending.push_back(Interval{interval.lower, midpoint,
                                   interval.lower_sample, *midpoint_sample,
                                   interval.depth + 1});
        continue;
      }

      envelope.linear_speed_ratio = std::max(
          envelope.linear_speed_ratio,
          static_cast<double>(linear_speed_upper / linear_speed_limit));
      envelope.angular_speed_ratio = std::max(
          envelope.angular_speed_ratio,
          static_cast<double>(angular_speed_upper / angular_speed_limit));
      envelope.linear_acceleration_ratio = std::max(
          envelope.linear_acceleration_ratio,
          static_cast<double>(linear_acceleration_upper /
                              linear_acceleration_limit));
      envelope.angular_acceleration_ratio = std::max(
          envelope.angular_acceleration_ratio,
          static_cast<double>(angular_acceleration_upper /
                              angular_acceleration_limit));
    }
  }
  return core::Result<CartesianFrameDynamicsEnvelope>::Ok(
      std::move(envelope));
}

}  // namespace manyforge::roboplan_backend::detail
