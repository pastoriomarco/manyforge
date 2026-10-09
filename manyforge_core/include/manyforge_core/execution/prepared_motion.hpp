#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/execution/execution_goal_key.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
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

/// Exact interpolation of every segment. The builder always selects quintic:
/// a sealed trajectory with runtime scale bounds carries complete P/V/A knots
/// (prepared trajectory codec). The step never downgrades per segment
/// (unlike the legacy sampler's success-side fallback).
enum class MotionInterpolation : std::uint8_t {
  kLinear = 1,
  kCubicHermite = 3,
  kQuintic = 5,
};

/// Tick-delay envelope read by `StepMotion` (within it the step advances
/// normally; beyond it the step reports the delay and caps the advance). Not
/// derived by `BuildPreparedMotionSlot`: it exists only for the step's
/// in-loop braking path, removed with the native loop step.
struct MotionTickEnvelope final {
  std::int64_t nominal_period_ns{};
  std::int64_t max_delay_ns{};

  friend constexpr bool operator==(const MotionTickEnvelope&,
                                   const MotionTickEnvelope&) = default;
};

/// Continuity tolerance for installing a replacement motion
/// (`InstallReplacementMotionStep`): the replacement's initial command must
/// match the command the loop emitted at the installation tick, and under a
/// Stop its last waypoint must be at rest. Both sides come from the same
/// in-process computation, so this absorbs floating-point rounding only.
struct MotionContinuityTolerance final {
  double position_rad{};
  double velocity_rad_s{};
  double acceleration_rad_s2{};
};
inline constexpr MotionContinuityTolerance kMotionContinuityTolerance{1e-9, 1e-9, 1e-9};

/// Cold, immutable, NRT-owned product of Prepare, built once from the sealed
/// `PreparedOperationParticipantDTO` (identified by its digest) by the one
/// shared builder `BuildPreparedMotionSlot` (prepared_motion_builder.hpp), in
/// the host and in every receiver alike. Never modified after construction;
/// no slider update rescans or replans it.
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

  /// The trajectory's sealed time-law bounds, unchanged.
  TrajectoryRuntimeScaleBoundsDTO runtime_scale_bounds{};
  /// Ordinary governor bounds derived from `runtime_scale_bounds`.
  detail::RuntimeScaleBounds governor_bounds{};
  /// Ceiling `StepMotion` applies to a `StopTransition`'s braking bounds. Not
  /// derived by `BuildPreparedMotionSlot`: it exists only for the step's
  /// in-loop braking path, removed with the native loop step.
  detail::RuntimeScaleBounds stop_braking_bounds{};

  /// The participant's resolved per-resource dynamics caps (index <
  /// joint_count), as planned; an absent value means no cap from any source.
  std::array<JointDynamicsLimitDTO, kMaxMotionJoints> effective_limits{};

  StateAuthorityClass state_authority{StateAuthorityClass::kMeasured};
  MotionTickEnvelope tick_envelope{};

  /// Final-position check (completion): the motion request's joint position
  /// tolerance.
  std::array<double, kMaxMotionJoints> final_position_tolerance_rad{};
  /// Rest proof for measured authority: the executing loop reports rest at
  /// the first cycle where every joint's measured velocity is at or below
  /// this threshold (no dwell, no position check). Predicted/commanded
  /// authority rests when the Stop finishes at commanded rest.
  std::array<double, kMaxMotionJoints> rest_velocity_threshold_rad_s{};
};

}  // namespace manyforge::core
