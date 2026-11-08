#pragma once

#include <manyforge_core/types/cartesian_fidelity.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {

/// TCP constraint for the unchanged closed prefix of a moving replacement.
struct RetainedTrajectoryPrefixValidationOptionsDTO final {
  double end_time_s{};
  double max_tcp_linear_speed_mps{};
  std::string tcp_frame;
  std::string base_frame;
  std::string tcp_frame_role_id{};  // portable role of tcp_frame; empty for a cell frame
};

/// Validation options for trajectory validation
struct ValidationOptionsDTO {
  double max_tcp_linear_speed_mps = 0.0;  // > 0: effective TCP speed limit to enforce
  std::string tcp_frame;                  // frame for TCP speed measurement
  std::string base_frame;                 // reference frame for TCP speed measurement
  // When false, validate_trajectory() must run non-collision validation only
  // (for example TCP-speed checks). Collision authority is then expected to
  // live in the normalized CollisionValidationCoordinator path.
  bool run_collision_checks = true;
  std::optional<RetainedTrajectoryPrefixValidationOptionsDTO> retained_prefix{};
  // Portable role of tcp_frame, reported as `frame_role` when the limit is
  // exceeded; empty for a cell frame.
  std::string tcp_frame_role_id{};
};

/// Compact measurements made while validating the exact accepted trajectory.
///
/// Joint vectors use the trajectory's ordered joint names.  An empty
/// acceleration vector means that neither authored derivatives nor enough
/// trajectory segments were available to measure acceleration.  TCP speed is
/// absent when no exact TCP/base-frame speed check was requested.
struct TrajectoryValidationMetricsDTO final {
  TrajectoryValidationMetricsDTO() = default;
  TrajectoryValidationMetricsDTO(
      double trajectory_duration_s_,
      std::vector<double> peak_abs_joint_velocity_rad_s_,
      std::vector<double> peak_abs_joint_acceleration_rad_s2_,
      std::optional<double> peak_tcp_linear_speed_mps_,
      std::optional<CartesianFidelityMetricsDTO> cartesian_fidelity_metrics_ = std::nullopt)
      : trajectory_duration_s(trajectory_duration_s_),
        peak_abs_joint_velocity_rad_s(std::move(peak_abs_joint_velocity_rad_s_)),
        peak_abs_joint_acceleration_rad_s2(
            std::move(peak_abs_joint_acceleration_rad_s2_)),
        peak_tcp_linear_speed_mps(peak_tcp_linear_speed_mps_),
        cartesian_fidelity_metrics(std::move(cartesian_fidelity_metrics_)) {}

  double trajectory_duration_s{};
  std::vector<double> peak_abs_joint_velocity_rad_s;
  std::vector<double> peak_abs_joint_acceleration_rad_s2;
  std::optional<double> peak_tcp_linear_speed_mps;
  std::optional<CartesianFidelityMetricsDTO> cartesian_fidelity_metrics;
  /// Compiled TCP frame of the peak above; absent when no TCP speed was
  /// measured.
  std::optional<std::string> peak_tcp_linear_speed_frame{};

  friend bool operator==(const TrajectoryValidationMetricsDTO&,
                         const TrajectoryValidationMetricsDTO&) = default;
};

/// Piece diagnostics from the same whole-curve validator traversal.
struct MovingReplacementValidationEvidenceDTO final {
  TrajectoryValidationMetricsDTO unchanged_prefix;
  TrajectoryValidationMetricsDTO changed_future;

  friend bool operator==(const MovingReplacementValidationEvidenceDTO&,
                         const MovingReplacementValidationEvidenceDTO&) = default;
};

/// Validation result
struct ValidationResultDTO {
  ValidationResultDTO() = default;
  ValidationResultDTO(
      bool valid_, std::optional<ErrorDTO> error_,
      std::optional<TrajectoryValidationMetricsDTO> trajectory_metrics_ = std::nullopt)
      : valid(valid_),
        error(std::move(error_)),
        trajectory_metrics(std::move(trajectory_metrics_)) {}

  bool valid = false;
  std::optional<ErrorDTO> error;  // present when valid is false; absent on success
  /// Present on successful trajectory validation.  This is evidence about the
  /// exact accepted trajectory, not a second policy or execution authority.
  std::optional<TrajectoryValidationMetricsDTO> trajectory_metrics;
  std::optional<MovingReplacementValidationEvidenceDTO> moving_replacement{};
};

/// Collision check request
struct CollisionCheckRequestDTO {
  RobotStateDTO robot_state;
};

/// Collision check result
struct CollisionCheckResultDTO {
  CollisionCheckResultDTO() = default;
  CollisionCheckResultDTO(bool in_collision_,
                          std::optional<std::string> contact_body_a_ = std::nullopt,
                          std::optional<std::string> contact_body_b_ = std::nullopt,
                          std::optional<double> penetration_depth_m_ = std::nullopt,
                          std::optional<double> estimated_force_n_ = std::nullopt)
      : in_collision(in_collision_),
        contact_body_a(std::move(contact_body_a_)),
        contact_body_b(std::move(contact_body_b_)),
        penetration_depth_m(penetration_depth_m_),
        estimated_force_n(estimated_force_n_) {}

  bool in_collision = false;
  std::optional<std::string> contact_body_a;  // optional: first colliding body name
  std::optional<std::string> contact_body_b;  // optional: second colliding body name
  std::optional<double> penetration_depth_m;  // optional: max penetration depth
  std::optional<double> estimated_force_n;    // optional: estimated contact force
};

/// Forward kinematics request
struct FkRequestDTO {
  RobotStateDTO robot_state;
  std::string target_frame;  // link/frame to compute pose for
  std::string base_frame;    // frame in which the result is expressed
};

/// Joint limits
struct JointLimitsDTO {
  std::vector<std::string> joint_names;
  std::vector<double> position_lower_rad;
  std::vector<double> position_upper_rad;
  std::vector<double> velocity_max_rad_s;
  std::vector<double> acceleration_max_rad_s2;  // optional
};

}  // namespace manyforge::core
