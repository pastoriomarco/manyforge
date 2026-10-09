#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/execution/motion_step.hpp>
#include <manyforge_core/execution/prepared_motion.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <new>
#include <optional>
#include <span>
#include <vector>

// Counting allocation hook (same pattern as test_bounded_stop_control_executor):
// only allocations made while `track_allocations` is set are counted.
namespace {
std::atomic<bool> track_allocations{false};
std::atomic<std::size_t> tracked_allocations{0U};

void* Allocate(const std::size_t size) {
  if (track_allocations) ++tracked_allocations;
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) return memory;
  throw std::bad_alloc();
}
}  // namespace

void* operator new(const std::size_t size) { return Allocate(size); }
void* operator new[](const std::size_t size) { return Allocate(size); }
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (track_allocations) ++tracked_allocations;
  return std::malloc(size == 0U ? 1U : size);
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (track_allocations) ++tracked_allocations;
  return std::malloc(size == 0U ? 1U : size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }

namespace manyforge::core {
namespace {

using detail::RuntimeScaleBounds;
using detail::RuntimeScaleGovernor;

constexpr std::int64_t kPeriodNs = 1'000'000;  // 1 ms nominal tick.
constexpr double kDurationS = 4.0;
constexpr std::array<double, 2> kStart{0.3, -1.2};
constexpr std::array<double, 2> kTravel{1.5, 0.8};
constexpr RuntimeScaleBounds kGovernorBounds{2.0, 8.0};
constexpr RuntimeScaleBounds kStopCeiling{4.0, 16.0};
constexpr double kMaxStopPathRate = 1.5;

// Minimum-jerk quintic: the quintic Hermite segments reproduce it exactly, so
// derivatives (including jerk) are continuous across knots.
std::array<double, 4> MinJerk(double travel, double t) {
  const double u = t / kDurationS;
  const double d = kDurationS;
  return {travel * (10 * u * u * u - 15 * u * u * u * u + 6 * u * u * u * u * u),
          travel * (30 * u * u - 60 * u * u * u + 30 * u * u * u * u) / d,
          travel * (60 * u - 180 * u * u + 120 * u * u * u) / (d * d),
          travel * (60 - 360 * u + 360 * u * u) / (d * d * d)};
}

TrajectoryDTO MakeTrajectory() {
  TrajectoryDTO trajectory;
  trajectory.joint_names = {"j1", "j2"};
  for (const double t : {0.0, 1.0, 2.0, 3.0, 4.0}) {
    TrajectoryWaypointDTO point{t, {}, {}, {}};
    for (std::size_t j = 0; j < 2; ++j) {
      const auto value = MinJerk(kTravel[j], t);
      point.positions_rad.push_back(kStart[j] + value[0]);
      point.velocities_rad_s.push_back(value[1]);
      point.accelerations_rad_s2.push_back(value[2]);
    }
    trajectory.waypoints.push_back(point);
  }
  return trajectory;
}

PreparedMotion MakePrepared() {
  PreparedMotion prepared;
  prepared.key.operation_id = OperationId{"op"};
  prepared.key.controller_goal_id = ControllerGoalId{"goal"};
  prepared.key.controller_goal_generation = ControllerGoalGeneration{1U};
  prepared.key.prepared_execution_id = PreparedExecutionId{7U};
  prepared.trajectory = MakeTrajectory();
  prepared.interpolation = MotionInterpolation::kQuintic;
  prepared.joint_count = 2U;
  prepared.runtime_scale_bounds =
      TrajectoryRuntimeScaleBoundsDTO{kGovernorBounds.max_rate_per_s,
                                      kGovernorBounds.max_rate_change_per_s2, kMaxStopPathRate,
                                      1.0};
  prepared.governor_bounds = kGovernorBounds;
  prepared.stop_braking_bounds = kStopCeiling;
  prepared.tick_envelope = MotionTickEnvelope{kPeriodNs, kPeriodNs};
  return prepared;
}

struct Loop {
  explicit Loop(double initial_scale = 1.0, PreparedMotion motion = MakePrepared())
      : prepared(std::move(motion)) {
    EXPECT_TRUE(InstallMotionStep(prepared, now_ns, initial_scale, RuntimeScaleGeneration{0U},
                                  state));
  }

  MotionStepOutput Tick(const StopTransition* stop = nullptr, const ScaleTarget* scale = nullptr,
                        std::int64_t period_ns = kPeriodNs, DeviceScalingFacts device = {}) {
    now_ns += period_ns;
    MotionStepOutput output;
    const MotionStepInputs inputs{now_ns, stop, scale, device};
    const auto status = StepMotion(prepared, state, inputs, output);
    EXPECT_EQ(status, output.status);
    return output;
  }

  ScaleTarget Scale(std::uint64_t generation, double target) const {
    return ScaleTarget{prepared.key.fence(), RuntimeScaleGeneration{generation}, target};
  }

  StopTransition Stop(std::uint64_t generation, RuntimeScaleBounds bounds) const {
    return StopTransition{prepared.key.fence(), StopGeneration{generation},
                          static_cast<std::uint32_t>(generation), bounds,
                          now_ns + 1'000'000'000};
  }

  PreparedMotion prepared;
  MotionStepState state{};
  std::int64_t now_ns{1'000'000'000};
};

void ExpectSameOutput(const MotionStepOutput& a, const MotionStepOutput& b) {
  EXPECT_EQ(a.status, b.status);
  EXPECT_EQ(a.path_time_s, b.path_time_s);
  EXPECT_EQ(a.applied_scale, b.applied_scale);
  EXPECT_EQ(a.scale_rate_per_s, b.scale_rate_per_s);
  EXPECT_EQ(a.goal, b.goal);
  EXPECT_EQ(a.consumed_scale_generation, b.consumed_scale_generation);
  EXPECT_EQ(a.consumed_stop_generation, b.consumed_stop_generation);
  EXPECT_EQ(a.stop_validation, b.stop_validation);
  EXPECT_EQ(a.scale_validation, b.scale_validation);
  EXPECT_EQ(a.device_validation, b.device_validation);
  EXPECT_EQ(a.observed_tick_delay_ns, b.observed_tick_delay_ns);
  EXPECT_EQ(a.tick_delay_within_envelope, b.tick_delay_within_envelope);
  EXPECT_EQ(a.command.joint_count, b.command.joint_count);
  EXPECT_EQ(a.command.jerk_qualified, b.command.jerk_qualified);
  EXPECT_EQ(a.command.position_rad, b.command.position_rad);
  EXPECT_EQ(a.command.velocity_rad_s, b.command.velocity_rad_s);
  EXPECT_EQ(a.command.acceleration_rad_s2, b.command.acceleration_rad_s2);
  EXPECT_EQ(a.command.jerk_rad_s3, b.command.jerk_rad_s3);
}

void ExpectSameState(const MotionStepState& a, const MotionStepState& b) {
  ASSERT_EQ(a.governor.has_value(), b.governor.has_value());
  if (a.governor) {
    EXPECT_EQ(a.governor->scale(), b.governor->scale());
    EXPECT_EQ(a.governor->rate_per_s(), b.governor->rate_per_s());
    EXPECT_EQ(a.governor->target_scale(), b.governor->target_scale());
    EXPECT_EQ(a.governor->scale_acceleration_per_s2(), b.governor->scale_acceleration_per_s2());
    EXPECT_EQ(a.governor->remaining_transition_s(), b.governor->remaining_transition_s());
  }
  if (std::isnan(a.path_time_s)) {
    EXPECT_TRUE(std::isnan(b.path_time_s));
  } else {
    EXPECT_EQ(a.path_time_s, b.path_time_s);
  }
  EXPECT_EQ(a.segment_hint, b.segment_hint);
  EXPECT_EQ(a.last_tick_ns, b.last_tick_ns);
  EXPECT_EQ(a.goal, b.goal);
  EXPECT_EQ(a.scale_generation, b.scale_generation);
  EXPECT_EQ(a.mode, b.mode);
  ASSERT_EQ(a.active_stop.has_value(), b.active_stop.has_value());
  if (a.active_stop) {
    EXPECT_EQ(a.active_stop->stop_generation, b.active_stop->stop_generation);
  }
}

TEST(MotionStep, SameInputsProduceIdenticalOutputs) {
  Loop first;
  Loop second;
  const auto slow = first.Scale(1U, 0.4);
  for (int tick = 0; tick < 600; ++tick) {
    const ScaleTarget* scale = tick >= 50 ? &slow : nullptr;
    std::optional<StopTransition> stop;
    if (tick >= 400) stop = first.Stop(1U, kStopCeiling);
    const std::int64_t period = tick == 200 ? 7 * kPeriodNs : kPeriodNs;
    const auto a = first.Tick(stop ? &*stop : nullptr, scale, period);
    const auto b = second.Tick(stop ? &*stop : nullptr, scale, period);
    ExpectSameOutput(a, b);
  }
  ExpectSameState(first.state, second.state);
}

TEST(MotionStep, RetargetMidRampKeepsScaleAndRateContinuousAndIsNeverRetroactive) {
  Loop loop;
  Loop reference;
  const auto down = loop.Scale(1U, 0.2);
  const auto up = loop.Scale(2U, 0.9);
  double previous_scale = 1.0;
  double previous_rate = 0.0;
  const double dt = 1e-3;
  for (int tick = 0; tick < 1000; ++tick) {
    const ScaleTarget* scale = tick >= 10 ? (tick >= 150 ? &up : &down) : nullptr;
    // The reference sees the second target one tick later.
    const ScaleTarget* reference_scale = tick >= 10 ? (tick >= 151 ? &up : &down) : nullptr;
    const auto out = loop.Tick(nullptr, scale);
    const auto ref = reference.Tick(nullptr, reference_scale);
    if (tick == 150) {
      // Mid ramp: the governor is still moving toward 0.2.
      ASSERT_GT(std::abs(out.scale_rate_per_s), 0.1);
      // The elapsed interval ran under the previous target: same cursor,
      // scale and rate as a loop that has not seen the new target.
      EXPECT_EQ(out.path_time_s, ref.path_time_s);
      EXPECT_EQ(out.applied_scale, ref.applied_scale);
      EXPECT_EQ(out.scale_rate_per_s, ref.scale_rate_per_s);
      EXPECT_EQ(out.command.position_rad, ref.command.position_rad);
      EXPECT_EQ(out.command.velocity_rad_s, ref.command.velocity_rad_s);
      EXPECT_EQ(out.scale_validation, MotionInputValidation::kAccepted);
      EXPECT_EQ(out.consumed_scale_generation, RuntimeScaleGeneration{2U});
    }
    EXPECT_LE(std::abs(out.applied_scale - previous_scale),
              kGovernorBounds.max_rate_per_s * dt * (1.0 + 1e-9));
    EXPECT_LE(std::abs(out.scale_rate_per_s - previous_rate),
              kGovernorBounds.max_rate_change_per_s2 * dt * (1.0 + 1e-9));
    previous_scale = out.applied_scale;
    previous_rate = out.scale_rate_per_s;
  }
  EXPECT_NEAR(previous_scale, 0.9, 1e-12);
}

TEST(MotionStep, ZeroScaleHoldsPathTimeAndResumes) {
  Loop loop;
  const auto hold = loop.Scale(1U, 0.0);
  MotionStepOutput out;
  for (int tick = 0; tick < 1000 && out.status != MotionStepStatus::kHolding; ++tick) {
    out = loop.Tick(nullptr, &hold);
  }
  ASSERT_EQ(out.status, MotionStepStatus::kHolding);
  const auto held = out;
  for (int tick = 0; tick < 200; ++tick) {
    out = loop.Tick(nullptr, &hold);
    EXPECT_EQ(out.status, MotionStepStatus::kHolding);
    EXPECT_EQ(out.path_time_s, held.path_time_s);
    EXPECT_EQ(out.command.position_rad, held.command.position_rad);
    EXPECT_EQ(out.command.velocity_rad_s[0], 0.0);
    EXPECT_EQ(out.command.acceleration_rad_s2[0], 0.0);
  }
  const auto resume = loop.Scale(2U, 1.0);
  out = loop.Tick(nullptr, &resume);
  EXPECT_EQ(out.path_time_s, held.path_time_s);  // Not retroactive.
  out = loop.Tick(nullptr, &resume);
  EXPECT_EQ(out.status, MotionStepStatus::kRunning);
  EXPECT_GT(out.path_time_s, held.path_time_s);
}

TEST(MotionStep, ValidStopTakesEffectWithStaleOrInvalidScaleAndDevice) {
  Loop loop;
  for (int tick = 0; tick < 100; ++tick) loop.Tick();
  const auto stop = loop.Stop(1U, kStopCeiling);
  auto foreign = loop.Scale(5U, 0.5);
  foreign.goal.controller_goal_generation = ControllerGoalGeneration{9U};
  DeviceScalingFacts device{DeviceScalingSource::kDeviceReported, 0.5, 1U, loop.now_ns, false};
  auto out = loop.Tick(&stop, &foreign, kPeriodNs, device);
  EXPECT_EQ(out.stop_validation, MotionInputValidation::kAccepted);
  EXPECT_EQ(out.scale_validation, MotionInputValidation::kStale);
  EXPECT_EQ(out.device_validation, MotionInputValidation::kInvalid);
  EXPECT_EQ(out.status, MotionStepStatus::kBraking);
  EXPECT_EQ(out.consumed_stop_generation, StopGeneration{1U});
  EXPECT_EQ(loop.state.governor->target_scale(), 0.0);

  // While braking, a non-finite target is invalid and a valid one is ignored;
  // neither is consumed and neither changes the braking target.
  const auto nan_scale = loop.Scale(6U, std::nan(""));
  out = loop.Tick(&stop, &nan_scale);
  EXPECT_EQ(out.scale_validation, MotionInputValidation::kInvalid);
  EXPECT_EQ(out.stop_validation, MotionInputValidation::kStale);  // Already consumed.
  const auto valid = loop.Scale(7U, 1.0);
  device.valid = true;
  out = loop.Tick(nullptr, &valid, kPeriodNs, device);
  EXPECT_EQ(out.scale_validation, MotionInputValidation::kIgnoredForStop);
  EXPECT_EQ(out.device_validation, MotionInputValidation::kIgnoredForStop);
  EXPECT_EQ(out.consumed_scale_generation, RuntimeScaleGeneration{0U});
  EXPECT_EQ(loop.state.governor->target_scale(), 0.0);
  EXPECT_EQ(out.status, MotionStepStatus::kBraking);

  // A scale target that is invalid on the same tick as the Stop also never
  // blocks it.
  Loop other;
  for (int tick = 0; tick < 100; ++tick) other.Tick();
  const auto other_stop = other.Stop(1U, kStopCeiling);
  const auto out_of_range = other.Scale(1U, 1.5);
  out = other.Tick(&other_stop, &out_of_range);
  EXPECT_EQ(out.scale_validation, MotionInputValidation::kInvalid);
  EXPECT_EQ(out.status, MotionStepStatus::kBraking);
}

TEST(MotionStep, CannotProceedLeavesStateUnchanged) {
  Loop loop;
  const auto slow = loop.Scale(1U, 0.5);
  for (int tick = 0; tick < 100; ++tick) loop.Tick(nullptr, &slow);
  const MotionStepState before = loop.state;
  const auto expect_cannot = [&](MotionStepOutput out) {
    EXPECT_EQ(out.status, MotionStepStatus::kCannotProceed);
    EXPECT_EQ(out.command.joint_count, 0U);
    ExpectSameState(loop.state, before);
  };

  // Reversed clock.
  expect_cannot(loop.Tick(nullptr, nullptr, -1));
  loop.now_ns += 1;

  // Invalid prepared data.
  PreparedMotion broken = loop.prepared;
  broken.joint_count = 0U;
  std::swap(broken, loop.prepared);
  expect_cannot(loop.Tick());
  std::swap(broken, loop.prepared);
  broken = loop.prepared;
  broken.trajectory.waypoints[1].accelerations_rad_s2.clear();  // Quintic data missing.
  std::swap(broken, loop.prepared);
  expect_cannot(loop.Tick());
  std::swap(broken, loop.prepared);

  // Goal-fenced newer Stop with bounds looser than the prepared Stop bounds.
  const auto loose = loop.Stop(1U, {kStopCeiling.max_rate_per_s * 2.0, 1.0});
  auto out = loop.Tick(&loose);
  EXPECT_EQ(out.stop_validation, MotionInputValidation::kInvalid);
  expect_cannot(out);

  // Non-finite state is never repaired.
  loop.state.path_time_s = std::nan("");
  const MotionStepState corrupt = loop.state;
  out = loop.Tick();
  EXPECT_EQ(out.status, MotionStepStatus::kCannotProceed);
  ExpectSameState(loop.state, corrupt);

  // Not installed.
  MotionStepState empty{};
  MotionStepOutput uninstalled;
  EXPECT_EQ(StepMotion(loop.prepared, empty, MotionStepInputs{loop.now_ns, nullptr, nullptr, {}},
                       uninstalled),
            MotionStepStatus::kCannotProceed);
}

TEST(MotionStep, BrakingStartsFromTheActualStateOfTheSameTick) {
  Loop loop;
  Loop reference;
  const auto slow = loop.Scale(1U, 0.6);
  std::optional<StopTransition> stop;
  MotionStepOutput previous;
  for (int tick = 0; tick < 3000; ++tick) {
    const ScaleTarget* scale = tick >= 20 ? &slow : nullptr;
    // Stop mid-ramp toward 0.6, while the scale rate is nonzero.
    if (tick == 100) stop = loop.Stop(1U, {4.0, 4.0});
    const auto out = loop.Tick(stop ? &*stop : nullptr, scale);
    const auto ref = reference.Tick(nullptr, scale);
    if (tick == 100) {
      ASSERT_LT(ref.scale_rate_per_s, -0.1);
      EXPECT_EQ(out.status, MotionStepStatus::kBraking);
      EXPECT_EQ(out.path_time_s, ref.path_time_s);
      EXPECT_EQ(out.applied_scale, ref.applied_scale);
      EXPECT_EQ(out.scale_rate_per_s, ref.scale_rate_per_s);
      EXPECT_EQ(out.command.position_rad, ref.command.position_rad);
      EXPECT_EQ(out.command.velocity_rad_s, ref.command.velocity_rad_s);
      // a = q''*s^2 + q'*s_dot has no s_ddot term: also continuous.
      EXPECT_EQ(out.command.acceleration_rad_s2, ref.command.acceleration_rad_s2);
    }
    if (tick > 100) {
      EXPECT_TRUE(out.status == MotionStepStatus::kBraking ||
                  out.status == MotionStepStatus::kStopped);
      EXPECT_LE(out.applied_scale, previous.applied_scale);
      for (std::size_t j = 0; j < 2; ++j) {
        EXPECT_LE(std::abs(out.command.position_rad[j] - previous.command.position_rad[j]),
                  2e-3);
        EXPECT_LE(std::abs(out.command.velocity_rad_s[j] - previous.command.velocity_rad_s[j]),
                  0.02);
      }
    }
    previous = out;
    if (out.status == MotionStepStatus::kStopped) break;
  }
  ASSERT_EQ(previous.status, MotionStepStatus::kStopped);
  EXPECT_EQ(previous.applied_scale, 0.0);
  EXPECT_EQ(previous.scale_rate_per_s, 0.0);
  const auto after = loop.Tick();
  EXPECT_EQ(after.status, MotionStepStatus::kStopped);
  EXPECT_EQ(after.path_time_s, previous.path_time_s);
  EXPECT_EQ(after.command.velocity_rad_s[0], 0.0);
}

TEST(MotionStep, StrongerStageContinuesFromBrakingStateAndNeverReturnsToNominal) {
  Loop loop;
  Loop reference;
  for (int tick = 0; tick < 100; ++tick) {
    loop.Tick();
    reference.Tick();
  }
  const auto soft = loop.Stop(1U, {4.0, 1.0});
  const auto hard = loop.Stop(2U, kStopCeiling);
  const auto resume = loop.Scale(1U, 1.0);
  int stopped_ticks_reference = -1;
  int stopped_ticks = -1;
  double previous_scale = 1.0;
  double previous_rate = 0.0;
  for (int tick = 0; tick < 5000 && (stopped_ticks < 0 || stopped_ticks_reference < 0); ++tick) {
    const StopTransition* stage = tick >= 200 ? &hard : &soft;
    const auto out = loop.Tick(stage, &resume);
    const auto ref = reference.Tick(&soft, &resume);
    if (tick == 200) {
      EXPECT_EQ(out.stop_validation, MotionInputValidation::kAccepted);
      EXPECT_EQ(out.consumed_stop_generation, StopGeneration{2U});
      ASSERT_LT(ref.scale_rate_per_s, 0.0);
      EXPECT_EQ(out.applied_scale, ref.applied_scale);
      EXPECT_EQ(out.scale_rate_per_s, ref.scale_rate_per_s);
      EXPECT_EQ(out.path_time_s, ref.path_time_s);
    }
    if (tick > 0) {
      EXPECT_NE(out.status, MotionStepStatus::kRunning);
      EXPECT_NE(out.status, MotionStepStatus::kHolding);
      EXPECT_NE(out.status, MotionStepStatus::kCannotProceed);
      EXPECT_EQ(out.scale_validation, MotionInputValidation::kIgnoredForStop);
      EXPECT_LE(std::abs(out.applied_scale - previous_scale), 4.0 * 1e-3 * (1.0 + 1e-9));
      EXPECT_LE(std::abs(out.scale_rate_per_s - previous_rate), 16.0 * 1e-3 * (1.0 + 1e-9));
    }
    previous_scale = out.applied_scale;
    previous_rate = out.scale_rate_per_s;
    if (stopped_ticks < 0 && out.status == MotionStepStatus::kStopped) stopped_ticks = tick;
    if (stopped_ticks_reference < 0 && ref.status == MotionStepStatus::kStopped) {
      stopped_ticks_reference = tick;
    }
  }
  ASSERT_GE(stopped_ticks, 0);
  EXPECT_LT(stopped_ticks, stopped_ticks_reference);
  // The superseded stage is stale; braking stays complete.
  const auto out = loop.Tick(&soft, &resume);
  EXPECT_EQ(out.stop_validation, MotionInputValidation::kStale);
  EXPECT_EQ(out.status, MotionStepStatus::kStopped);
}

TEST(MotionStep, StageUnrealizableFromCurrentStateIsContainedNotIgnored) {
  // Rate above the stage's rate bound.
  Loop loop;
  const auto slow = loop.Scale(1U, 0.2);
  for (int tick = 0; tick < 200; ++tick) loop.Tick(nullptr, &slow);
  ASSERT_GT(std::abs(loop.state.governor->rate_per_s()), 0.5);
  const MotionStepState before = loop.state;
  const auto tight = loop.Stop(1U, {0.1, 16.0});
  auto out = loop.Tick(&tight);
  EXPECT_EQ(out.status, MotionStepStatus::kCannotProceed);
  EXPECT_EQ(out.stop_validation, MotionInputValidation::kInvalid);
  ExpectSameState(loop.state, before);

  // Overshoot above the `max_stop_path_rate` ceiling while accelerating.
  Loop rising(0.2);
  const auto fast = rising.Scale(1U, 1.0);
  for (int tick = 0; tick < 300; ++tick) rising.Tick(nullptr, &fast);
  ASSERT_GT(rising.state.governor->rate_per_s(), 1.0);
  const auto gentle = rising.Stop(1U, {4.0, 0.25});
  out = rising.Tick(&gentle);
  EXPECT_EQ(out.status, MotionStepStatus::kCannotProceed);
  EXPECT_EQ(out.stop_validation, MotionInputValidation::kInvalid);
}

TEST(MotionStep, BrakingMayUseTheCertifiedStopPathRateAboveOne) {
  // Accelerating toward 1, a Stop with a gentler rate change overshoots above
  // one before braking; the sealed certificate admits it up to 1.5.
  constexpr RuntimeScaleBounds kGentle{4.0, 2.0};
  for (int start_tick = 0; start_tick < 400; ++start_tick) {
    Loop probe(0.2);
    const auto fast = probe.Scale(1U, 1.0);
    for (int tick = 0; tick < start_tick; ++tick) probe.Tick(nullptr, &fast);
    const double s = probe.state.governor->scale();
    const double r = probe.state.governor->rate_per_s();
    const double peak = s + (r > 0.0 ? r * r / (2.0 * kGentle.max_rate_change_per_s2) : 0.0);
    if (!(peak > 1.05 && peak < 1.45)) continue;
    // Advance one more tick under the old target exactly as the step will.
    const auto stop = probe.Stop(1U, kGentle);
    double max_scale = 0.0;
    MotionStepOutput out = probe.Tick(&stop, &fast);
    ASSERT_EQ(out.status, MotionStepStatus::kBraking);
    while (out.status == MotionStepStatus::kBraking) {
      max_scale = std::max(max_scale, out.applied_scale);
      out = probe.Tick(nullptr, &fast);
    }
    EXPECT_EQ(out.status, MotionStepStatus::kStopped);
    EXPECT_GT(max_scale, 1.0);
    EXPECT_LE(max_scale, kMaxStopPathRate);
    return;
  }
  FAIL() << "no tick with a certified overshoot found";
}

TEST(MotionStep, LateTickIsReportedAndAdvancesAtMostTheEnvelopeWithoutCatchUp) {
  Loop loop;
  for (int tick = 0; tick < 10; ++tick) loop.Tick();
  ASSERT_EQ(loop.state.governor->scale(), 1.0);

  auto before = loop.state.path_time_s;
  auto out = loop.Tick(nullptr, nullptr, 1'800'000);  // Inside the envelope.
  EXPECT_TRUE(out.tick_delay_within_envelope);
  EXPECT_EQ(out.observed_tick_delay_ns, 800'000);
  EXPECT_NEAR(out.path_time_s - before, 1.8e-3, 1e-15);

  before = loop.state.path_time_s;
  out = loop.Tick(nullptr, nullptr, 50'000'000);  // Far outside.
  EXPECT_FALSE(out.tick_delay_within_envelope);
  EXPECT_EQ(out.observed_tick_delay_ns, 49'000'000);
  EXPECT_NEAR(out.path_time_s - before, 2e-3, 1e-15);  // Envelope bound only.
  EXPECT_EQ(loop.state.last_tick_ns, loop.now_ns);

  before = loop.state.path_time_s;
  out = loop.Tick();  // The dropped remainder is never caught up.
  EXPECT_TRUE(out.tick_delay_within_envelope);
  EXPECT_EQ(out.observed_tick_delay_ns, 0);
  EXPECT_NEAR(out.path_time_s - before, 1e-3, 1e-15);
}

TEST(MotionStep, ComposedDerivativesMatchFiniteDifferencesOfSampledPositions) {
  constexpr std::int64_t kFinePeriodNs = 100'000;  // 0.1 ms
  constexpr double h = 1e-4;
  PreparedMotion prepared = MakePrepared();
  prepared.tick_envelope = MotionTickEnvelope{kFinePeriodNs, kFinePeriodNs};
  Loop loop(1.0, prepared);
  const auto slow = loop.Scale(1U, 0.3);
  const auto fast = loop.Scale(2U, 0.8);
  std::vector<MotionStepOutput> outputs;
  for (int tick = 0; tick < 12000; ++tick) {
    const ScaleTarget* scale = tick >= 2000 ? (tick >= 4500 ? &fast : &slow) : nullptr;
    outputs.push_back(loop.Tick(nullptr, scale, kFinePeriodNs));
  }
  int checked_jerk = 0;
  for (std::size_t k = 1; k + 1 < outputs.size(); ++k) {
    const auto& prev = outputs[k - 1].command;
    const auto& cur = outputs[k].command;
    const auto& next = outputs[k + 1].command;
    ASSERT_TRUE(cur.jerk_qualified);
    for (std::size_t j = 0; j < 2; ++j) {
      const double fd_v = (next.position_rad[j] - prev.position_rad[j]) / (2 * h);
      const double fd_a = (next.velocity_rad_s[j] - prev.velocity_rad_s[j]) / (2 * h);
      EXPECT_NEAR(cur.velocity_rad_s[j], fd_v, 1e-6) << "k=" << k;
      // s_ddot is piecewise constant: acceleration has corners, not steps.
      EXPECT_NEAR(cur.acceleration_rad_s2[j], fd_a, 2e-3) << "k=" << k;
      // Jerk has steps where s_ddot switches phase; compare only where the
      // neighbouring jerk samples show no step.
      if (std::abs(next.jerk_rad_s3[j] - prev.jerk_rad_s3[j]) < 1e-2) {
        const double fd_j = (next.acceleration_rad_s2[j] - prev.acceleration_rad_s2[j]) / (2 * h);
        EXPECT_NEAR(cur.jerk_rad_s3[j], fd_j, 1e-4) << "k=" << k;
        ++checked_jerk;
      }
    }
  }
  EXPECT_GT(checked_jerk, 20000);
}

TEST(MotionStep, ComputedEndHoldsTheFinalPointAndIsReportedRepeatedly) {
  Loop loop;
  MotionStepOutput out;
  int ticks = 0;
  while (out.status != MotionStepStatus::kComputedEnd && ticks < 5000) {
    out = loop.Tick();
    ++ticks;
  }
  ASSERT_EQ(out.status, MotionStepStatus::kComputedEnd);
  EXPECT_NEAR(ticks, 4000, 1);
  EXPECT_EQ(out.path_time_s, kDurationS);
  const auto& last = loop.prepared.trajectory.waypoints.back();
  for (std::size_t j = 0; j < 2; ++j) {
    EXPECT_EQ(out.command.position_rad[j], last.positions_rad[j]);
    EXPECT_EQ(out.command.velocity_rad_s[j], 0.0);
    EXPECT_EQ(out.command.acceleration_rad_s2[j], 0.0);
    EXPECT_EQ(out.command.jerk_rad_s3[j], 0.0);
  }
  EXPECT_EQ(loop.state.segment_hint, 3U);
  out = loop.Tick();
  EXPECT_EQ(out.status, MotionStepStatus::kComputedEnd);
  EXPECT_EQ(out.path_time_s, kDurationS);
}

TEST(MotionStep, TickPathDoesNotAllocate) {
  Loop loop;
  const auto slow = loop.Scale(1U, 0.4);
  const auto stop = loop.Stop(1U, kStopCeiling);
  const auto hard = loop.Stop(2U, kStopCeiling);
  const DeviceScalingFacts device{DeviceScalingSource::kDeviceReported, 0.8, 3U, loop.now_ns,
                                  true};
  MotionStepOutput out;
  std::size_t statuses = 0U;
  tracked_allocations = 0U;
  track_allocations = true;
  for (int tick = 0; tick < 1500; ++tick) {
    const StopTransition* stage = tick >= 1200 ? &hard : (tick >= 900 ? &stop : nullptr);
    const std::int64_t period = tick == 500 ? 40 * kPeriodNs : kPeriodNs;
    loop.now_ns += period;
    const MotionStepInputs inputs{loop.now_ns, stage, &slow, device};
    statuses += static_cast<std::size_t>(StepMotion(loop.prepared, loop.state, inputs, out));
  }
  loop.now_ns -= 5;  // Reversed clock: the failure path does not allocate either.
  statuses +=
      static_cast<std::size_t>(StepMotion(loop.prepared, loop.state,
                                          MotionStepInputs{loop.now_ns, nullptr, nullptr, {}}, out));
  track_allocations = false;
  EXPECT_EQ(tracked_allocations.load(), 0U);
  EXPECT_GT(statuses, 0U);
  // Positive control: the hook observes allocations (a direct operator call
  // cannot be elided, unlike a new-expression).
  track_allocations = true;
  ::operator delete(::operator new(16U));
  track_allocations = false;
  EXPECT_EQ(tracked_allocations.load(), 1U);
}

// ---- Shared numerical extensions used by the step. ----

TEST(MotionStep, TickCrossingManyKnotsUsesBoundedLookupAndSamplesTheRightSegment) {
  // Dense knots: one long tick crosses far more knots than the linear steps.
  PreparedMotion dense = MakePrepared();
  dense.trajectory.waypoints.clear();
  for (int k = 0; k <= 400; ++k) {
    const double t = kDurationS * static_cast<double>(k) / 400.0;
    TrajectoryWaypointDTO point{t, {}, {}, {}};
    for (std::size_t j = 0; j < 2; ++j) {
      const auto value = MinJerk(kTravel[j], t);
      point.positions_rad.push_back(kStart[j] + value[0]);
      point.velocities_rad_s.push_back(value[1]);
      point.accelerations_rad_s2.push_back(value[2]);
    }
    dense.trajectory.waypoints.push_back(point);
  }
  dense.tick_envelope = MotionTickEnvelope{kPeriodNs, 2'000'000'000};
  ASSERT_TRUE(validate_trajectory_for_sampling(dense.trajectory).ok());
  Loop loop(1.0, dense);
  const auto output = loop.Tick(nullptr, nullptr, 1'500'000'000);
  ASSERT_EQ(output.status, MotionStepStatus::kRunning);
  EXPECT_GT(loop.state.segment_hint, 100U);
  const auto& waypoints = loop.prepared.trajectory.waypoints;
  EXPECT_LE(waypoints[loop.state.segment_hint].time_from_start_s, loop.state.path_time_s);
  EXPECT_GT(waypoints[loop.state.segment_hint + 1U].time_from_start_s, loop.state.path_time_s);
  TrajectorySampleResult reference;
  ASSERT_TRUE(
      sample_trajectory_prevalidated(loop.prepared.trajectory, loop.state.path_time_s, reference)
          .ok());
  for (std::size_t j = 0; j < 2; ++j) {
    EXPECT_NEAR(output.command.position_rad[j], reference.positions_rad[j], 1e-12);
  }
}

TEST(RuntimeScaleGovernorExtensions, FactoriesMatchConstructorsWithoutThrowing) {
  EXPECT_FALSE(RuntimeScaleGovernor::create(1.1, kGovernorBounds));
  EXPECT_FALSE(RuntimeScaleGovernor::create(0.5, {0.0, 1.0}));
  EXPECT_FALSE(RuntimeScaleGovernor::create(std::nan(""), kGovernorBounds));
  const auto created = RuntimeScaleGovernor::create(0.5, kGovernorBounds);
  ASSERT_TRUE(created);
  EXPECT_EQ(created->scale(), 0.5);
  EXPECT_EQ(created->max_scale(), 1.0);
  EXPECT_FALSE(RuntimeScaleGovernor::create_continuation(0.5, kGovernorBounds, 3.0, 0.0));
  EXPECT_FALSE(RuntimeScaleGovernor::create_continuation(1.2, kGovernorBounds, 0.0, 0.0));
  EXPECT_FALSE(RuntimeScaleGovernor::create_continuation(0.5, kGovernorBounds, 0.0, 0.0, 0.9));
  EXPECT_TRUE(RuntimeScaleGovernor::create_continuation(1.2, kGovernorBounds, 0.0, 0.0, 1.5));

  RuntimeScaleGovernor original(0.6, kGovernorBounds);
  ASSERT_TRUE(original.retarget(0.1));
  ASSERT_TRUE(original.advance(0.05));
  const RuntimeScaleGovernor thrown(original.scale(), kGovernorBounds, original.rate_per_s(),
                                    original.target_scale());
  const auto factory = RuntimeScaleGovernor::create_continuation(
      original.scale(), kGovernorBounds, original.rate_per_s(), original.target_scale());
  ASSERT_TRUE(factory);
  EXPECT_EQ(factory->scale(), thrown.scale());
  EXPECT_EQ(factory->rate_per_s(), thrown.rate_per_s());
  EXPECT_EQ(factory->remaining_transition_s(), thrown.remaining_transition_s());
}

TEST(RuntimeScaleGovernorExtensions, ScaleAccelerationIsTheDerivativeOfTheRate) {
  RuntimeScaleGovernor governor(1.0, kGovernorBounds);
  EXPECT_EQ(governor.scale_acceleration_per_s2(), 0.0);
  ASSERT_TRUE(governor.retarget(0.2));
  const double h = 1e-5;
  int checked = 0;
  while (governor.transitioning()) {
    const double rate = governor.rate_per_s();
    const double s_ddot = governor.scale_acceleration_per_s2();
    EXPECT_LE(std::abs(s_ddot), kGovernorBounds.max_rate_change_per_s2);
    auto next = governor;
    ASSERT_TRUE(next.advance(h));
    if (next.transitioning() && next.scale_acceleration_per_s2() == s_ddot) {
      EXPECT_NEAR((next.rate_per_s() - rate) / h, s_ddot, 1e-6);
      ++checked;
    }
    ASSERT_TRUE(governor.advance(1e-3));
  }
  EXPECT_GT(checked, 100);
  EXPECT_EQ(governor.scale_acceleration_per_s2(), 0.0);
}

TEST(TrajectorySegmentEvaluation, MatchesSamplerAndExposesThirdDerivative) {
  const auto trajectory = MakeTrajectory();
  ASSERT_TRUE(validate_trajectory_for_sampling(trajectory).ok());
  TrajectorySampleResult sample;
  std::array<double, 2> p{}, v{}, a{}, j{};
  for (const double t : {0.0, 0.2, 1.0, 1.77, 2.31, 3.999, 4.0}) {
    ASSERT_TRUE(sample_trajectory_prevalidated(trajectory, t, sample).ok());
    ASSERT_TRUE(evaluate_trajectory_segment(trajectory, *sample.segment_index, t, 5, p, v, a, j));
    for (std::size_t k = 0; k < 2; ++k) {
      EXPECT_EQ(p[k], sample.positions_rad[k]);
      EXPECT_EQ(v[k], sample.velocities_rad_s[k]);
      EXPECT_EQ(a[k], sample.accelerations_rad_s2[k]);
      EXPECT_NEAR(j[k], MinJerk(kTravel[k], t)[3], 1e-9);
    }
  }
  // Exact degree: missing acceleration data is refused, never downgraded.
  auto cubic_only = trajectory;
  cubic_only.waypoints[1].accelerations_rad_s2.clear();
  EXPECT_FALSE(evaluate_trajectory_segment(cubic_only, 0, 0.2, 5, p, v, a, j));
  EXPECT_TRUE(evaluate_trajectory_segment(cubic_only, 0, 0.2, 3, p, v, a, j));
  // Wrong segment for the time.
  EXPECT_FALSE(evaluate_trajectory_segment(trajectory, 0, 1.0, 5, p, v, a, j));
  EXPECT_FALSE(evaluate_trajectory_segment(trajectory, 0, 0.2, 4, p, v, a, j));
}

}  // namespace
}  // namespace manyforge::core
