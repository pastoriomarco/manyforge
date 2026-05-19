#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>

namespace manyforge::core {

/// Soft-stop strategy applied when a streaming session ends or when
/// ExecutionKernel::stop() is called from kStreaming (ADR-0016).
///
/// `spring_back` is self-contained: the backend builds a two-point
/// FollowJointTrajectory anchored at the observed state with negative
/// time-from-start and a future point at the observed position with zero
/// velocity. JTC interpolation produces a smooth deceleration along the
/// path-tangent direction and settles at the on-path stop position.
///
/// `velocity_scaling` requires controller-side support (newer JTC releases):
/// the backend ramps the controller's velocity scaling factor to zero over
/// `soft_stop_deceleration_time_s` and then cancels the active goal. Qualification
/// matrix lives in `docs/open-points.md` "Soft-stop strategy under JTC for
/// live-streamed targets".
enum class SoftStopStrategy {
  kSpringBack,
  kVelocityScaling,
};

inline const char* to_string(SoftStopStrategy strategy) {
  switch (strategy) {
    using enum SoftStopStrategy;
    case kSpringBack: return "spring_back";
    case kVelocityScaling: return "velocity_scaling";
  }
  return "Unknown";
}

/// Input semantics for a streaming target (ADR-0016).
///
/// Cartesian targets are pose-in-base (translation + xyzw quaternion).
/// Joint targets are a partial joint-name -> position map. Velocity feedforward
/// is reserved for future revisions and is not honored today.
enum class StreamingInputMode {
  kCartesian,
  kJoint,
};

inline const char* to_string(StreamingInputMode mode) {
  switch (mode) {
    using enum StreamingInputMode;
    case kCartesian: return "cartesian";
    case kJoint: return "joint";
  }
  return "Unknown";
}

/// Configuration for a streaming session, supplied to
/// IExecutionBackend::begin_streaming_session (ADR-0016).
///
/// Reliability/efficiency parameters (anchor_negative_time_ms,
/// target_time_from_start_min_ms, max_dispatch_rate_hz) tune the backend's
/// rolling-horizon FJT goal builder. They are deployment-configurable; sensible
/// defaults are baked in for Isaac-class controllers and may need raising for
/// slower real-hardware controllers.
struct StreamingSessionConfigDTO {
  /// The robot group this session drives (matches existing motion-coordinator
  /// semantics — see ExecuteRequestDTO::group_id).
  std::string group_id;

  /// Joint names this session may command. Empty means "all drivable joints in
  /// the bound group". For Cartesian-only sessions, this drives the IK seed
  /// joint ordering and the anchored-FJT joint axis order.
  std::vector<std::string> joint_names;

  /// Default input semantics. A push_streaming_target call MAY carry a
  /// different kind (e.g. cartesian targets in a primarily-joint session) if
  /// the backend supports it; otherwise the target is rejected.
  StreamingInputMode default_input_mode = StreamingInputMode::kCartesian;

  /// Reference frame for Cartesian targets ("base", "world", "tcp"). Joint
  /// targets ignore this field.
  std::string cartesian_reference_frame = "base";

  /// TCP frame name. Used by the kernel's IK pre-resolution (ADR-0016) to
  /// pick which body in the kinematic chain to solve for. Required for
  /// Cartesian-mode sessions; ignored for joint-mode sessions.
  std::string tcp_frame;

  /// Soft-stop strategy used by stop() and end_streaming_session() when no
  /// override is supplied at end-time.
  SoftStopStrategy default_soft_stop_strategy = SoftStopStrategy::kSpringBack;

  /// Soft-stop deceleration duration. Used as the future-point
  /// time_from_start for spring-back stops, and as the ramp duration for
  /// velocity-scaling stops.
  double soft_stop_deceleration_time_s = 0.4;

  /// FJT anchor point time-from-start (negative). A small magnitude is best —
  /// it only needs to give JTC's spline a continuous starting condition.
  /// Defaults to -10 ms.
  double anchor_negative_time_s = -0.010;

  /// Floor on the target point's time_from_start. The actual dispatched value
  /// is max(this, computed_from_joint_velocity_limits + margin). Defaults to
  /// 30 ms, which matches the ~30 Hz target dispatch envelope.
  double target_time_from_start_min_s = 0.030;

  /// Backend cap on outbound FJT goal submissions. Inbound targets faster than
  /// this rate are coalesced; the most-recent wins. Defaults to 30 Hz.
  double max_dispatch_rate_hz = 30.0;

  /// If no new target arrives within this window, the backend MUST hold
  /// position (re-dispatch the last target with a zero-velocity profile).
  /// Defaults to 200 ms.
  double target_max_age_s = 0.200;

  /// Per-session velocity cap (fraction of joint velocity limits). Defaults to
  /// 1.0 (no extra cap); deployments expecting LLM-driven sessions should
  /// typically set this lower.
  double velocity_scale = 1.0;

  /// Per-joint absolute velocity caps (rad/s, parallel to joint_names).
  /// 0.0 entries (or empty vector / size mismatch) signal "use engine
  /// default (0.5 rad/s)". The session's velocity_scale multiplies these.
  /// Sourced from `deployment.robot_config.streaming_joint_limits[joint].max_velocity_rad_s`
  /// at session begin (spec/430 §3.2 streaming-joint-limits, ADR-0016).
  std::vector<double> per_joint_velocity_caps_rad_s;

  /// Per-joint absolute acceleration caps (rad/s², parallel to joint_names).
  /// 0.0 entries (or empty vector / size mismatch) signal "use engine
  /// default (2.0 rad/s²)". NOT scaled by velocity_scale — acceleration
  /// is a per-joint physical limit, not a UX-rate knob. Sourced from
  /// `deployment.robot_config.streaming_joint_limits[joint].max_acceleration_rad_s2`
  /// at session begin.
  std::vector<double> per_joint_acceleration_caps_rad_s2;

  /// Absolute Cartesian (TCP) linear speed cap in m/s. The backend's
  /// dispatch worker MUST clamp target_time_from_start so that the
  /// implied Cartesian linear speed never exceeds this value. This is
  /// the streaming-session counterpart to
  /// `deployment.robot_config.global_max_tcp_linear_speed_mps` enforced
  /// by motion.py for planned moves (ADR-0015). 0.0 means "unlimited"
  /// (no clamp); deployments should always set a non-zero global cap.
  double max_tcp_linear_speed_mps = 0.0;

  /// Audit fields. Required by upstream cycle_manager integration to write
  /// session ownership into the intervention record.
  std::string principal_id;
  std::string authority_source = "human";  // human | llm | program_node | policy
  std::string trigger_reason = "operator_direct_control";
};

/// A single streaming target push (ADR-0016). Latest-wins coalescing applies
/// at the backend.
struct StreamingTargetDTO {
  /// Input semantics. Either matches the session's default_input_mode or, if
  /// the backend supports it, overrides on a per-target basis.
  StreamingInputMode kind = StreamingInputMode::kCartesian;

  /// Cartesian payload (used when kind == kCartesian).
  std::array<double, 3> position_m{0.0, 0.0, 0.0};
  std::array<double, 4> orientation_xyzw{0.0, 0.0, 0.0, 1.0};

  /// Joint payload (used when kind == kJoint). Partial maps are valid; joints
  /// not listed retain the latest dispatched commanded value.
  std::vector<std::string> joint_names;
  std::vector<double> joint_positions_rad;

  /// Wall-clock timestamp of the source event (e.g. WS frame receive time).
  /// Used for staleness detection and trace correlation; not for motion
  /// scheduling.
  int64_t source_timestamp_ns = 0;

  /// Monotonic sequence number from the caller. Helpful for correlating
  /// dispatched/rejected events with upstream frames.
  uint64_t source_sequence = 0;

  /// Minimum time_from_start the backend MUST honor when scheduling
  /// this target's anchored FJT goal. Populated by the kernel when
  /// enforcing Cartesian speed caps (ADR-0015 on streaming sessions):
  /// the kernel sets this to `distance_m / max_tcp_linear_speed_mps`
  /// so the robot's commanded motion respects the cap regardless of
  /// the operator's input rate. The backend then uses
  /// `max(target_time_from_start_min_s, joint_velocity_dt,
  ///      min_time_from_start_s)` for the FJT time_from_start.
  /// 0.0 means "no kernel-side override"; the backend uses its own
  /// floors only.
  double min_time_from_start_s = 0.0;
};

/// Acknowledgement returned by push_streaming_target. MUST NOT block on
/// motion; the backend acknowledges acceptance into its target register, not
/// motion completion.
struct StreamingPushResultDTO {
  bool accepted = false;
  /// Sequence number assigned by the backend for this push (monotonic per
  /// session; useful for correlating downstream streaming_target_dispatched
  /// events).
  uint64_t backend_sequence = 0;
  std::optional<ErrorDTO> error;  // present when accepted is false
};

/// Result of begin_streaming_session.
struct StreamingSessionBeginResultDTO {
  bool accepted = false;
  uint64_t session_id = 0;
  std::optional<ErrorDTO> error;  // present when accepted is false
};

/// Result of end_streaming_session.
struct StreamingSessionEndResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;  // present when accepted is false
};

/// Observer-side handle returned by ExecutionKernel::begin_streaming_session
/// (mirrors ExecutionHandle for the discrete-trajectory path).
struct StreamingSessionHandle {
  uint64_t session_id = 0;
  std::function<bool(int64_t timeout_ns)> wait_for_completion;
  std::function<ExecutionSnapshot()> get_snapshot;
};

}  // namespace manyforge::core
