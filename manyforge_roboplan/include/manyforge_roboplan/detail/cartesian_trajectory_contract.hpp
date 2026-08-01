#pragma once

// Bounded internal trajectory helpers extracted from c4b075fa6dfb69e463c61f3efa8cfce8de681e8a;
// the deleted Cartesian planner request body is intentionally absent.

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <roboplan/core/scene.hpp>
#include <roboplan/core/types.hpp>

#include <manyforge_core/types/plan_request.hpp>

#include <manyforge_roboplan/cartesian_planning_contract.hpp>

namespace manyforge::roboplan_backend::detail {

struct CartesianJointDynamicsEnvelope {
  bool complete = true;
  double velocity_ratio = 1.0;
  double acceleration_ratio = 1.0;
};

struct CartesianFrameDynamicsEnvelope {
  bool complete = true;
  double linear_speed_ratio = 0.0;
  double angular_speed_ratio = 0.0;
  double linear_acceleration_ratio = 0.0;
  double angular_acceleration_ratio = 0.0;
  std::size_t interval_checks = 0;
};

/// Model-wide conservative motion attribution for one scalar planning group.
/// Both continuous-path fidelity and TCP-dynamics certification consume this
/// single resolver so mimic chains and kinematic-radius assumptions cannot
/// drift between two safety checks.
struct CartesianKinematicMotionBounds {
  std::vector<long double> angular_amplification;
  std::vector<long double> prismatic_amplification;
  double model_radius_m = 0.0;
};

struct CartesianInterpolantPath {
  std::vector<Eigen::Matrix4d> poses;
  /// Conservative motion still possible between adjacent sampled poses. The
  /// fidelity checker reserves these margins instead of treating samples as
  /// the continuous controller path.
  double unsampled_translation_bound_m = 0.0;
  double unsampled_rotation_bound_rad = 0.0;
};

/// Validates untrusted planner output before applying ManyForge's neutral
/// rest-boundary convention. Keeping both operations behind one checked seam
/// prevents normalization from masking malformed producer derivatives.
std::optional<core::ErrorDTO>
validate_and_normalize_cartesian_trajectory_output(
    roboplan::JointTrajectory& trajectory,
    const std::vector<std::string>& expected_joint_names,
    const CartesianPlanningControl& control);

/// Measures the complete quintic trajectory implied by position, velocity,
/// and acceleration waypoints. This does not trust producer-stamped endpoint
/// derivatives: extrema between waypoints are included in the envelope.
core::Result<CartesianJointDynamicsEnvelope>
measure_cartesian_joint_dynamics_envelope(
    const core::PlanRequestDTO& request,
    const roboplan::JointTrajectory& trajectory,
    std::span<const double> model_velocity_limits,
    std::span<const double> model_acceleration_limits,
    const CartesianPlanningControl& control);

/// Certifies TCP dynamics for the exact quintic trajectory used at execution.
/// Pinocchio provides the endpoint values for each adaptive interval; analytic
/// joint-derivative and model-radius bounds conservatively cover all unsampled
/// time. Exhausting the declared work bound is a fail-closed planning error.
core::Result<CartesianFrameDynamicsEnvelope>
measure_cartesian_frame_dynamics_envelope(
    const core::PlanRequestDTO& request,
    const core::CartesianPathDTO& path,
    const std::shared_ptr<roboplan::Scene>& scene,
    const roboplan::JointTrajectory& trajectory,
    const CartesianPlanningControl& control);

core::Result<CartesianKinematicMotionBounds>
resolve_cartesian_kinematic_motion_bounds(
    const std::shared_ptr<roboplan::Scene>& scene,
    const std::string& group_id,
    const std::vector<std::string>& trajectory_joint_names);

/// Samples the exact quintic interpolation used by the execution sampler at a
/// tolerance-derived bounded resolution. The returned residual bounds let the
/// path verifier certify the continuous trajectory rather than only trusting
/// planner waypoints.
core::Result<CartesianInterpolantPath> sample_cartesian_interpolant_path(
    const core::PlanRequestDTO& request,
    const std::shared_ptr<roboplan::Scene>& scene,
    const roboplan::JointTrajectory& trajectory,
    double translation_tolerance_m,
    double rotation_tolerance_rad,
    const CartesianPlanningControl& control);

}  // namespace manyforge::roboplan_backend::detail
