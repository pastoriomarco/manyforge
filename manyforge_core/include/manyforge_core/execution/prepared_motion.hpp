#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/execution/execution_goal_key.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Fixed joint capacity of every real-time motion value (prepared limits,
/// step output). No existing RT joint bound exists in the repository; the
/// 256-row limits in codecs and collision capability are cold-path schema
/// bounds. 16 covers every supported arm (UFactory xArm 5/6/7, Lite 6, UF850,
/// UR) plus external axes with margin while keeping one step output under
/// 600 bytes. Preparation refuses a participant with more resources.
inline constexpr std::size_t kMaxMotionJoints = 16U;

/// Exact interpolation selected at Prepare for every segment. Preparation
/// refuses a trajectory whose derivative data cannot support it; the step
/// never downgrades on missing derivatives (unlike the legacy sampler's
/// success-side fallback).
enum class MotionInterpolation : std::uint8_t {
  kLinear = 1,
  kCubicHermite = 3,
  kQuintic = 5,
};

/// Boundary at which an adapter's computed commands count as realized.
/// Mechanism-neutral: native = the actuator loop wrote them; Isaac = the
/// bridge published them unsuppressed; xArm = the supervised session
/// submitted them. A successful return that suppressed or coalesced a command
/// is reported as such and never counts as realized. Realization is not
/// downstream delivery, position reached or rest.
enum class CommandRealizationBoundary : std::uint8_t {
  kLoopInterfaceWrite = 0,
  kTransportPublication,
  kDeviceSessionSubmission,
};

/// Adapter-declared command-realization contract, sealed into the prepared
/// motion so completion and "scale applied" use exactly this rule.
struct CommandRealizationContract final {
  CommandRealizationBoundary boundary{CommandRealizationBoundary::kLoopInterfaceWrite};
  /// The mechanism may suppress a write while returning success.
  bool may_suppress{};
  /// The mechanism may replace an unsent command by a newer one; skipped
  /// sequences are reported separately from the submitted one.
  bool may_coalesce{};

  friend constexpr bool operator==(const CommandRealizationContract&,
                                   const CommandRealizationContract&) = default;
};

/// Permissible tick-delay envelope. Within it the step advances normally;
/// beyond it the step reports the delay. The late-tick response (bounded
/// advance, hold or containment through Stop) is not fixed yet and is
/// qualified in the experiment. Stop deadlines always use actual monotonic
/// elapsed time, never a capped advance.
struct MotionTickEnvelope final {
  std::int64_t nominal_period_ns{};
  std::int64_t max_delay_ns{};

  friend constexpr bool operator==(const MotionTickEnvelope&,
                                   const MotionTickEnvelope&) = default;
};

/// Tolerance for installing an off-tick replacement plan (spring-back or a
/// braking profile the governor cannot express): its initial state must
/// match the actual loop state within these bounds, otherwise installation is
/// refused and the Kernel escalates.
struct MotionContinuityTolerance final {
  double position_rad{};
  double velocity_rad_s{};
  double acceleration_rad_s2{};

  friend constexpr bool operator==(const MotionContinuityTolerance&,
                                   const MotionContinuityTolerance&) = default;
};

/// Cold, immutable, NRT-owned product of Prepare, built once from the sealed
/// `PreparedOperationParticipantDTO` (identified by its digest). Never
/// modified after construction; no slider update rescans or replans it.
///
/// Lifetime: NRT allocates and reclaims. The loop borrows it through a stable
/// slot (`IExecutionMechanism::Submit`) and NRT reclaims only after the loop
/// acknowledges it no longer references it. RT code only reads it: the
/// trajectory's waypoint storage is read in place; everything else the step
/// reads per joint is fixed-capacity.
struct PreparedMotion final {
  ExecutionGoalKey key;
  ContentDigest prepared_contract_digest{std::string{}};

  /// Validated and sealed trajectory (prevalidated for sampling).
  TrajectoryDTO trajectory;
  MotionInterpolation interpolation{MotionInterpolation::kQuintic};
  std::uint8_t joint_count{};

  /// The trajectory's sealed time-law bounds, including the Stop path-rate
  /// certificate (`max_stop_path_rate`).
  TrajectoryRuntimeScaleBoundsDTO runtime_scale_bounds{};
  /// Ordinary governor bounds derived from `runtime_scale_bounds`.
  detail::RuntimeScaleBounds governor_bounds{};
  /// Governor bounds for on-path braking under the selected Stop policy.
  detail::RuntimeScaleBounds stop_braking_bounds{};
  /// Worst-case on-path braking duration under `stop_braking_bounds` from any
  /// reachable scale/rate. Preparation refuses when it does not fit the
  /// selected policy's Stop budget.
  std::int64_t worst_case_on_path_braking_ns{};

  /// Effective per-joint limits (index < joint_count); missing jerk means
  /// jerk is not qualified, never an invented cap.
  std::array<detail::TrajectoryDerivativeLimits, kMaxMotionJoints> effective_limits{};

  StateAuthorityClass state_authority{StateAuthorityClass::kMeasured};
  CommandRealizationContract command_realization{};
  MotionTickEnvelope tick_envelope{};
  MotionContinuityTolerance continuity_tolerance{};

  /// Authority-specific final-position check (completion).
  std::array<double, kMaxMotionJoints> final_position_tolerance_rad{};
  /// Rest evidence needs position convergence as well as velocity.
  std::array<double, kMaxMotionJoints> rest_position_tolerance_rad{};
  std::array<double, kMaxMotionJoints> rest_velocity_threshold_rad_s{};
  /// Minimum local monotonic duration over which fresh samples of the
  /// binding's authority must stay within both rest tolerances before Stop
  /// rest evidence may report stationary. A single sample never does.
  std::int64_t rest_dwell_ns{};
};

}  // namespace manyforge::core
