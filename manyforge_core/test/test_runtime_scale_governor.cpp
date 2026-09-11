#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <manyforge_core/detail/coordinated_execution_clock.hpp>
#include <manyforge_core/detail/coordinated_motion_preparation.hpp>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <stdexcept>
#include <thread>

namespace manyforge::core::detail {
namespace {
constexpr RuntimeScaleBounds kBounds{4.0, 16.0};

TEST(CoordinatedExecutionClock, WorkersShareOriginAndRetargetWithoutIndependentIntegration) {
  const OperationParticipantId a("a"), b("b");
  FakeClock clock;
  clock.set(1000000000);
  CoordinatedExecutionClock common({a, b}, kBounds);
  EXPECT_FALSE(common.target_scale());
  std::optional<RuntimeScaleTimeAnchor> first;
  std::jthread worker(
      [&](std::stop_token stop) { first = common.start_and_wait(a, clock, 1.0, stop); });
  EXPECT_FALSE(common.sample(clock.now()));
  clock.advance(1000000000);
  const auto second = common.start_and_wait(b, clock, 1.0, {});
  worker.join();
  ASSERT_TRUE(first && second);
  EXPECT_EQ(first->timestamp_ns, second->timestamp_ns);
  EXPECT_EQ(first->timestamp_ns, clock.now());
  EXPECT_DOUBLE_EQ(first->trajectory_time_s, 0.0);
  EXPECT_FALSE(common.start_and_wait(a, clock, 1.0, {}));
  clock.advance(100000000);
  const auto seam = common.sample(clock.now());
  ASSERT_TRUE(seam);
  ASSERT_TRUE(common.retarget(0.2));
  EXPECT_DOUBLE_EQ(*common.target_scale(), 0.2);
  const auto changed = common.sample(clock.now());
  ASSERT_TRUE(changed);
  EXPECT_DOUBLE_EQ(changed->trajectory_time_s, seam->trajectory_time_s);
  EXPECT_DOUBLE_EQ(changed->governor.scale(), seam->governor.scale());
  EXPECT_DOUBLE_EQ(changed->governor.rate_per_s(), seam->governor.rate_per_s());
  clock.advance(-1);
  EXPECT_FALSE(common.retarget(0.4));
  clock.advance(1);
  EXPECT_FALSE(common.retarget(2.0));
  common.cancel_pending_start();
  clock.advance(300000000);
  const auto later = common.sample(clock.now());
  const auto expected = sample_runtime_scale_time(*changed, clock.now());
  ASSERT_TRUE(later && expected);
  EXPECT_DOUBLE_EQ(later->trajectory_time_s, expected->trajectory_time_s);
  EXPECT_DOUBLE_EQ(later->governor.scale(), expected->governor.scale());
  common.freeze_retargeting();
  EXPECT_FALSE(common.retarget(1.0));
  EXPECT_DOUBLE_EQ(*common.target_scale(), 0.2);
  common.freeze_retargeting();
  clock.advance(100000000);
  const auto frozen = common.sample_now();
  const auto continuing = sample_runtime_scale_time(*changed, clock.now());
  ASSERT_TRUE(frozen && continuing);
  EXPECT_DOUBLE_EQ(frozen->trajectory_time_s, continuing->trajectory_time_s);
  EXPECT_DOUBLE_EQ(frozen->governor.scale(), continuing->governor.scale());
}

TEST(CoordinatedExecutionClock, SharedCommandIdentityChangesAtomicallyAndCannotRegress) {
  FakeClock clock;
  CoordinatedExecutionClock common({OperationParticipantId("a"), OperationParticipantId("b")},
                                   kBounds);
  std::jthread worker([&](std::stop_token stop) {
    (void)common.start_and_wait(OperationParticipantId("a"), clock, 1.0, stop);
  });
  ASSERT_TRUE(common.start_and_wait(OperationParticipantId("b"), clock, 1.0, {}));
  worker.join();
  CoordinatedRuntimeScaleCommandDTO command{OperationId("op.1"), OperationRevision(1U),
                                            RuntimeScaleGeneration(1U)};
  ASSERT_TRUE(common.retarget(0.25, command));
  ASSERT_TRUE(common.target());
  EXPECT_EQ(common.target()->command, command);
  EXPECT_DOUBLE_EQ(common.target()->scale, 0.25);
  EXPECT_TRUE(common.retarget(0.25, command));
  EXPECT_FALSE(common.retarget(0.5, command));
  EXPECT_FALSE(common.retarget(0.5));
  command.operation_id = OperationId("op.other");
  command.generation = RuntimeScaleGeneration(2U);
  EXPECT_FALSE(common.retarget(0.5, command));
  command.operation_id = OperationId("op.1");
  EXPECT_FALSE(common.retarget(2.0, command));
  EXPECT_EQ(common.target()->command->generation, RuntimeScaleGeneration(1U));
  EXPECT_TRUE(common.retarget(0.5, command));
  common.freeze_retargeting();
  command.generation = RuntimeScaleGeneration(3U);
  EXPECT_FALSE(common.retarget(0.75, command));
  EXPECT_EQ(common.target()->command->generation, RuntimeScaleGeneration(2U));
  EXPECT_DOUBLE_EQ(common.target()->scale, 0.5);
}

TEST(CoordinatedExecutionClock, InconsistentOrCancelledStartReleasesEveryWaiter) {
  const OperationParticipantId a("a"), b("b");
  for (int mismatch = 0; mismatch != 5; ++mismatch) {
    SCOPED_TRACE(mismatch);
    FakeClock first_clock, second_clock;
    CoordinatedExecutionClock common({a, b}, kBounds);
    std::optional<RuntimeScaleTimeAnchor> first;
    std::jthread worker(
        [&](std::stop_token stop) { first = common.start_and_wait(a, first_clock, 1.0, stop); });
    if (mismatch == 2) common.cancel_pending_start();
    if (mismatch == 3) worker.request_stop();
    if (mismatch == 4) common.freeze_retargeting();
    const auto second = common.start_and_wait(b, mismatch == 0 ? second_clock : first_clock,
                                              mismatch == 1 ? 0.5 : 1.0, {});
    worker.join();
    EXPECT_FALSE(first);
    EXPECT_FALSE(second);
    EXPECT_FALSE(common.sample(first_clock.now()));
    EXPECT_FALSE(common.retarget(1.0));
    EXPECT_EQ(common.stop_prevented_start(), mismatch == 4);
  }
}

TEST(RuntimeScaleTimeAnchor, ReadersSampleTheSameLawWithoutChangingItsOrigin) {
  RuntimeScaleTimeAnchor anchor{2.0, RuntimeScaleGovernor(1.0, kBounds), 1000000000};
  ASSERT_TRUE(anchor.governor.retarget(0.0));
  const auto early = sample_runtime_scale_time(anchor, 1125000000);
  const auto late = sample_runtime_scale_time(anchor, 1375000000);
  ASSERT_TRUE(early && late);
  const auto from_early = sample_runtime_scale_time(*early, late->timestamp_ns);
  ASSERT_TRUE(from_early);
  EXPECT_NEAR(from_early->trajectory_time_s, late->trajectory_time_s, 1e-14);
  EXPECT_NEAR(from_early->governor.scale(), late->governor.scale(), 1e-14);
  EXPECT_NEAR(from_early->governor.rate_per_s(), late->governor.rate_per_s(), 1e-14);
  EXPECT_DOUBLE_EQ(anchor.trajectory_time_s, 2.0);
  EXPECT_DOUBLE_EQ(anchor.governor.scale(), 1.0);
  EXPECT_EQ(anchor.timestamp_ns, 1000000000);

  auto changed = *early;
  ASSERT_TRUE(changed.governor.retarget(1.0));
  const auto seam = sample_runtime_scale_time(changed, changed.timestamp_ns);
  ASSERT_TRUE(seam);
  EXPECT_DOUBLE_EQ(seam->trajectory_time_s, early->trajectory_time_s);
  EXPECT_DOUBLE_EQ(seam->governor.scale(), early->governor.scale());
  EXPECT_DOUBLE_EQ(seam->governor.rate_per_s(), early->governor.rate_per_s());
  EXPECT_DOUBLE_EQ(anchor.governor.target_scale(), 0.0);
}

TEST(RuntimeScaleTimeAnchor, ClockArithmeticPreservesSmallIntervalsAndRejectsReversal) {
  const auto high = std::numeric_limits<std::int64_t>::max();
  RuntimeScaleTimeAnchor anchor{0.0, RuntimeScaleGovernor(1.0, kBounds), high - 10};
  const auto sample = sample_runtime_scale_time(anchor, high);
  ASSERT_TRUE(sample);
  EXPECT_DOUBLE_EQ(sample->trajectory_time_s, 1e-8);
  EXPECT_FALSE(sample_runtime_scale_time(anchor, high - 11));
  anchor.timestamp_ns = std::numeric_limits<std::int64_t>::min();
  const auto wide = sample_runtime_scale_time(anchor, high);
  ASSERT_TRUE(wide);
  EXPECT_DOUBLE_EQ(wide->trajectory_time_s,
                   static_cast<double>(std::numeric_limits<std::uint64_t>::max()) / 1e9);
  anchor.trajectory_time_s = std::numeric_limits<double>::infinity();
  EXPECT_FALSE(sample_runtime_scale_time(anchor, high));
}

TEST(RuntimeScalePreparation, ParticipantsJoinOneBudgetAndCancellationReleasesWaiters) {
  const OperationParticipantId a("participant.a"), b("participant.b");
  CoordinatedMotionPreparation group({a, b});
  std::optional<RuntimeScalePreparation> first;
  std::jthread worker(
      [&](std::stop_token stop) { first = group.arrive_and_wait(a, {2.0, 8.0, 1.0, 2.0}, stop); });
  const auto second = group.arrive_and_wait(b, {1.0, 4.0, 1.0, 3.0}, {});
  worker.join();
  ASSERT_TRUE(first && second);
  EXPECT_DOUBLE_EQ(first->trajectory_time_multiplier, 3.0);
  EXPECT_DOUBLE_EQ(first->scale_bounds.max_rate_per_s, 1.0);
  EXPECT_DOUBLE_EQ(first->scale_bounds.max_rate_change_per_s2, 4.0);
  EXPECT_DOUBLE_EQ(second->trajectory_time_multiplier, first->trajectory_time_multiplier);

  for (bool cancel_by_token : {false, true}) {
    CoordinatedMotionPreparation cancelled({a, b});
    first = RuntimeScalePreparation{1.0, kBounds};
    std::jthread waiting([&](std::stop_token stop) {
      first = cancelled.arrive_and_wait(a, {2.0, 8.0, 1.0, 2.0}, stop);
    });
    if (cancel_by_token)
      waiting.request_stop();
    else
      cancelled.cancel();
    waiting.join();
    EXPECT_FALSE(first);
    EXPECT_FALSE(cancelled.arrive_and_wait(b, {1.0, 4.0, 1.0, 3.0}, {}));
  }
  CoordinatedMotionPreparation missing({a, b});
  EXPECT_FALSE(missing.arrive_and_wait(a, {2.0, 8.0}, {}));
  EXPECT_FALSE(missing.arrive_and_wait(b, {1.0, 4.0, 1.0, 3.0}, {}));
  EXPECT_THROW((CoordinatedMotionPreparation({a, a})), std::invalid_argument);
}

TEST(RuntimeScalePreparation, CommonRetimingPreservesCurveAndCartesianMilestones) {
  TrajectoryDTO original{
      {ResourceId("joint.1")},
      {"joint_1"},
      {{0.0, {0.0}, {0.0}, {0.0}}, {1.0, {0.4}, {0.2}, {-0.1}}, {2.0, {1.0}, {0.0}, {0.0}}},
      TrajectoryRuntimeScaleBoundsDTO{2.0, 8.0, 1.5, 2.0},
      {0.0, 1.0, 1.0, 2.0}};
  auto candidate = original;
  ASSERT_TRUE(apply_common_runtime_scaling(candidate, {6.0, {1.0, 4.0}}));
  EXPECT_EQ(candidate.resources, original.resources);
  EXPECT_EQ(candidate.cartesian_waypoint_times_s, (std::vector<double>{0.0, 3.0, 3.0, 6.0}));
  EXPECT_DOUBLE_EQ(candidate.runtime_scale_bounds->max_stop_path_rate, 1.5);
  EXPECT_NE(EncodePreparedTrajectory(candidate).value(),
            EncodePreparedTrajectory(original).value());
  for (double time : {0.0, 0.25, 0.75, 1.0, 1.5, 2.0}) {
    const auto before = sample_trajectory(original, time);
    const auto after = sample_trajectory(candidate, time * 3.0);
    ASSERT_TRUE(before.ok() && after.ok());
    EXPECT_NEAR(before.value().positions_rad[0], after.value().positions_rad[0], 1e-12);
    EXPECT_NEAR(before.value().velocities_rad_s[0] / 3.0, after.value().velocities_rad_s[0], 1e-12);
    EXPECT_NEAR(before.value().accelerations_rad_s2[0] / 9.0, after.value().accelerations_rad_s2[0],
                1e-12);
  }
  const auto encoded = EncodePreparedTrajectory(candidate).value();
  ASSERT_TRUE(apply_common_runtime_scaling(candidate, {6.0, {1.0, 4.0}}));
  EXPECT_EQ(EncodePreparedTrajectory(candidate).value(), encoded);
  for (const auto invalid :
       {RuntimeScalePreparation{5.0, {1.0, 4.0}}, RuntimeScalePreparation{6.0, {2.0, 4.0}},
        RuntimeScalePreparation{1e308, {1.0, 4.0}}}) {
    EXPECT_FALSE(apply_common_runtime_scaling(candidate, invalid));
    EXPECT_EQ(EncodePreparedTrajectory(candidate).value(), encoded);
  }
  candidate.runtime_scale_bounds->nominal_time_multiplier.reset();
  EXPECT_FALSE(apply_common_runtime_scaling(candidate, {6.0, {1.0, 4.0}}));
}

TEST(RuntimeScaleGovernor, CapturedBrakeOnlyTailToleratesRelativeCancellation) {
  RuntimeScaleGovernor captured(2.4999999999999998e-14, {4.0, 0.05}, -5e-8, 0.0);
  const auto advance = captured.advance(1e-6);
  ASSERT_TRUE(advance);
  EXPECT_GE(*advance, 0.0);
  EXPECT_EQ(captured.scale(), 0.0);
  EXPECT_EQ(captured.rate_per_s(), 0.0);
  RuntimeScaleGovernor positive_tail(0.4811820000000188, {3.999999999996, 15.999999999984},
                                     0.7759999999992241, 0.5);
  ASSERT_TRUE(positive_tail.advance(0.0485));
  EXPECT_NEAR(positive_tail.scale(), 0.5, 1e-14);
  EXPECT_NEAR(positive_tail.rate_per_s(), 0.0, 1e-14);
  EXPECT_THROW((RuntimeScaleGovernor(0.0, {4.0, 0.05}, -0.5, 0.0)), std::invalid_argument);
}

TEST(RuntimeScaleGovernor, CapturedContinuationPreservesIntegralAndRate) {
  for (const double target : {0.0, 0.4, 1.0}) {
    for (const double capture : {0.01, 0.10, 0.25, 0.40}) {
      RuntimeScaleGovernor original(0.6, kBounds);
      ASSERT_TRUE(original.retarget(target));
      ASSERT_TRUE(original.advance(capture));
      RuntimeScaleGovernor captured(original.scale(), kBounds, original.rate_per_s(),
                                    original.target_scale());
      for (const double dt : {0.01, 0.04, 0.25, 1.0}) {
        const auto expected = original.advance(dt);
        const auto actual = captured.advance(dt);
        ASSERT_TRUE(expected && actual);
        EXPECT_NEAR(*actual, *expected, 1e-12);
        EXPECT_NEAR(captured.scale(), original.scale(), 1e-12);
        EXPECT_NEAR(captured.rate_per_s(), original.rate_per_s(), 1e-12);
      }
    }
  }
}

TEST(RuntimeScalePreparation, CommonDilationAndScalarBoundsRespectEveryDimension) {
  const std::array derivatives{TrajectoryDerivativeBounds{2.0, 4.0, 60.0},
                               TrajectoryDerivativeBounds{0.5, 2.0, 10.0}};
  const std::array limits{TrajectoryDerivativeLimits{1.0, 3.0, 8.0},
                          TrajectoryDerivativeLimits{0.5, 1.0, 3.0}};
  const auto result = prepare_runtime_scaling(derivatives, limits, 0.1, kBounds);
  ASSERT_TRUE(result.has_value());
  EXPECT_GE(result->trajectory_time_multiplier, 2.0);
  EXPECT_GT(result->scale_bounds.max_rate_per_s, 0.0);
  EXPECT_GT(result->scale_bounds.max_rate_change_per_s2, 0.0);
  for (std::size_t index = 0U; index < derivatives.size(); ++index) {
    // Further slowdown can retain the existing conservative governor bounds;
    // coordination need not transport the full envelopes to recompute them.
    for (double extra_slowdown : {1.0, 1.5, 4.0}) {
      const double k = result->trajectory_time_multiplier * extra_slowdown;
      const double v = derivatives[index].velocity / k;
      const double a = derivatives[index].acceleration / k / k;
      const double j = derivatives[index].jerk / k / k / k;
      const double r = result->scale_bounds.max_rate_per_s;
      const double b = result->scale_bounds.max_rate_change_per_s2;
      EXPECT_LE(v, limits[index].velocity);
      EXPECT_LE(a + v * r, limits[index].acceleration);
      EXPECT_LE(j + 3.0 * a * r + v * b, *limits[index].jerk);
    }
  }
}

TEST(RuntimeScalePreparation, JerkOffDoesNotInventACapAndStationaryDimensionsDoNotDivideByZero) {
  const std::array derivatives{TrajectoryDerivativeBounds{0.1, 0.1, 1e12},
                               TrajectoryDerivativeBounds{0.0, 0.0, 0.0}};
  const std::array limits{TrajectoryDerivativeLimits{1.0, 1.0, std::nullopt},
                          TrajectoryDerivativeLimits{1.0, 1.0, 0.01}};
  const auto result = prepare_runtime_scaling(derivatives, limits, 0.1, kBounds);
  ASSERT_TRUE(result.has_value());
  EXPECT_DOUBLE_EQ(result->trajectory_time_multiplier, 1.0);
  EXPECT_NEAR(result->scale_bounds.max_rate_per_s, 4.0, 1e-10);
  EXPECT_NEAR(result->scale_bounds.max_rate_change_per_s2, 16.0, 1e-10);
}

TEST(RuntimeScalePreparation, ParticipantsShareDilationAndBoundsWithoutIndependentRetiming) {
  const std::array derivatives{TrajectoryDerivativeBounds{0.5, 1.0, 2.0},
                               TrajectoryDerivativeBounds{4.0, 8.0, 100.0},
                               TrajectoryDerivativeBounds{0.0, 0.0, 0.0}};
  const std::array limits{TrajectoryDerivativeLimits{1.0, 2.0, std::nullopt},
                          TrajectoryDerivativeLimits{1.0, 2.0, 4.0},
                          TrajectoryDerivativeLimits{1.0, 1.0, 1.0}};
  const auto dimensions = std::span(derivatives);
  const auto ceilings = std::span(limits);
  std::array participants{TrajectoryDynamicsEnvelope{dimensions.first(1), ceilings.first(1)},
                          TrajectoryDynamicsEnvelope{dimensions.subspan(1), ceilings.subspan(1)}};
  const auto first_alone =
      prepare_runtime_scaling(dimensions.first(1), ceilings.first(1), 0.2, kBounds);
  const auto together = prepare_runtime_scaling(participants, 0.2, kBounds);
  const auto flat = prepare_runtime_scaling(derivatives, limits, 0.2, kBounds);
  ASSERT_TRUE(first_alone && together && flat);
  EXPECT_GT(together->trajectory_time_multiplier, first_alone->trajectory_time_multiplier);
  EXPECT_DOUBLE_EQ(together->trajectory_time_multiplier, flat->trajectory_time_multiplier);
  EXPECT_DOUBLE_EQ(together->scale_bounds.max_rate_per_s, flat->scale_bounds.max_rate_per_s);
  EXPECT_DOUBLE_EQ(together->scale_bounds.max_rate_change_per_s2,
                   flat->scale_bounds.max_rate_change_per_s2);
  std::swap(participants[0], participants[1]);
  const auto reordered = prepare_runtime_scaling(participants, 0.2, kBounds);
  ASSERT_TRUE(reordered);
  EXPECT_DOUBLE_EQ(reordered->trajectory_time_multiplier, together->trajectory_time_multiplier);
  EXPECT_DOUBLE_EQ(reordered->scale_bounds.max_rate_per_s, together->scale_bounds.max_rate_per_s);
  EXPECT_DOUBLE_EQ(reordered->scale_bounds.max_rate_change_per_s2,
                   together->scale_bounds.max_rate_change_per_s2);
  for (std::size_t i = 0; i < derivatives.size(); ++i) {
    const double k = together->trajectory_time_multiplier;
    const double v = derivatives[i].velocity / k;
    const double a = derivatives[i].acceleration / k / k;
    const double j = derivatives[i].jerk / k / k / k;
    EXPECT_LE(v, limits[i].velocity);
    EXPECT_LE(a + v * together->scale_bounds.max_rate_per_s, limits[i].acceleration);
    if (limits[i].jerk) {
      EXPECT_LE(j + 3 * a * together->scale_bounds.max_rate_per_s +
                    v * together->scale_bounds.max_rate_change_per_s2,
                *limits[i].jerk);
    }
  }
}

TEST(RuntimeScalePreparation, IncompleteParticipantCannotProduceACommonBudget) {
  const std::array derivatives{TrajectoryDerivativeBounds{1.0, 1.0, 1.0}};
  const std::array limits{TrajectoryDerivativeLimits{1.0, 1.0, 1.0}};
  std::array participants{TrajectoryDynamicsEnvelope{derivatives, limits},
                          TrajectoryDynamicsEnvelope{}};
  EXPECT_FALSE(prepare_runtime_scaling(participants, 0.2, kBounds));
  participants[1].derivatives = derivatives;
  EXPECT_FALSE(prepare_runtime_scaling(participants, 0.2, kBounds));
  EXPECT_FALSE(prepare_runtime_scaling({}, 0.2, kBounds));
  participants[1].limits = limits;
  for (const double invalid :
       {0.5, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    participants[1].minimum_time_multiplier = invalid;
    EXPECT_FALSE(prepare_runtime_scaling(participants, 0.2, kBounds));
  }
}

TEST(RuntimeScalePreparation, AdditionalTimingFloorContributesBeforeGovernorMargins) {
  const std::array derivatives{TrajectoryDerivativeBounds{1.0, 1.0, 1.0}};
  const std::array limits{TrajectoryDerivativeLimits{1.0, 1.0, 1.0}};
  const auto joint_only = prepare_runtime_scaling(derivatives, limits, 0.2, kBounds);
  const std::array participants{TrajectoryDynamicsEnvelope{derivatives, limits, 4.0}};
  const auto with_tcp = prepare_runtime_scaling(participants, 0.2, kBounds);
  ASSERT_TRUE(joint_only && with_tcp);
  EXPECT_DOUBLE_EQ(with_tcp->trajectory_time_multiplier, 4.0);
  EXPECT_GT(with_tcp->scale_bounds.max_rate_per_s, joint_only->scale_bounds.max_rate_per_s);
  EXPECT_GT(with_tcp->scale_bounds.max_rate_change_per_s2,
            joint_only->scale_bounds.max_rate_change_per_s2);
  const double v = 1.0 / 4.0, a = 1.0 / 16.0, j = 1.0 / 64.0;
  EXPECT_LE(a + v * with_tcp->scale_bounds.max_rate_per_s, 1.0);
  EXPECT_LE(j + 3 * a * with_tcp->scale_bounds.max_rate_per_s +
                v * with_tcp->scale_bounds.max_rate_change_per_s2,
            1.0);
}

TEST(RuntimeScalePreparation, RejectsMalformedAndUnrepresentableBudgets) {
  const std::array derivatives{TrajectoryDerivativeBounds{1.0, 1.0, 1.0}};
  std::array limits{TrajectoryDerivativeLimits{1.0, 1.0, 1.0}};
  EXPECT_FALSE(prepare_runtime_scaling({}, {}, 0.1, kBounds).has_value());
  EXPECT_FALSE(prepare_runtime_scaling(derivatives, {}, 0.1, kBounds).has_value());
  for (double invalid : {0.0, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_FALSE(prepare_runtime_scaling(derivatives, limits, invalid, kBounds).has_value());
  }
  limits[0].jerk = 0.0;
  EXPECT_FALSE(prepare_runtime_scaling(derivatives, limits, 0.1, kBounds).has_value());
  limits[0].jerk = std::numeric_limits<double>::denorm_min();
  EXPECT_FALSE(prepare_runtime_scaling(derivatives, limits, 0.99, kBounds).has_value());
}

TEST(RuntimeScaleGovernor, ConstantScaleIntegratesExactlyIncludingZeroHold) {
  for (double scale : {0.0, 0.25, 1.0}) {
    RuntimeScaleGovernor governor(scale, kBounds);
    EXPECT_FALSE(governor.transitioning());
    EXPECT_DOUBLE_EQ(governor.advance(2.0).value(), 2.0 * scale);
    EXPECT_DOUBLE_EQ(governor.scale(), scale);
    EXPECT_DOUBLE_EQ(governor.rate_per_s(), 0.0);
  }
}

TEST(RuntimeScaleGovernor, TriangularRateHasExactClockIntegralAndNoRateStep) {
  RuntimeScaleGovernor governor(0.0, kBounds);
  ASSERT_TRUE(governor.retarget(1.0));
  EXPECT_DOUBLE_EQ(governor.scale(), 0.0);
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), 0.0);
  EXPECT_DOUBLE_EQ(governor.remaining_transition_s(), 0.5);
  EXPECT_NEAR(governor.advance(0.25).value(), 1.0 / 24.0, 1e-14);
  EXPECT_DOUBLE_EQ(governor.scale(), 0.5);
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), 4.0);
  EXPECT_NEAR(governor.advance(0.25).value(), 5.0 / 24.0, 1e-14);
  EXPECT_DOUBLE_EQ(governor.scale(), 1.0);
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), 0.0);
  EXPECT_FALSE(governor.transitioning());
}

TEST(RuntimeScaleGovernor, TrapezoidalRateAndMultiPhaseTickHaveSameIntegral) {
  RuntimeScaleGovernor whole(0.0, {1.0, 4.0});
  auto split = whole;
  ASSERT_TRUE(whole.retarget(1.0));
  ASSERT_TRUE(split.retarget(1.0));
  EXPECT_DOUBLE_EQ(whole.remaining_transition_s(), 1.25);
  const double integral = whole.advance(2.0).value();
  double accumulated = 0.0;
  for (double dt : {0.1, 0.3, 0.1, 0.5, 1.0}) {
    accumulated += split.advance(dt).value();
  }
  EXPECT_NEAR(integral, 1.375, 1e-14);
  EXPECT_NEAR(accumulated, integral, 1e-14);
  EXPECT_DOUBLE_EQ(split.scale(), 1.0);
  EXPECT_DOUBLE_EQ(split.rate_per_s(), 0.0);
}

TEST(RuntimeScaleGovernor, CrossingTargetBrakesCurrentRateRatherThanResettingIt) {
  RuntimeScaleGovernor governor(0.0, kBounds);
  ASSERT_TRUE(governor.retarget(1.0));
  ASSERT_TRUE(governor.advance(0.125).has_value());
  const double scale = governor.scale();
  const double rate = governor.rate_per_s();
  ASSERT_TRUE(governor.retarget(0.01));
  EXPECT_DOUBLE_EQ(governor.scale(), scale);
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), rate);
  ASSERT_TRUE(governor.advance(0.125).has_value());
  EXPECT_NEAR(governor.scale(), 0.25, 1e-14);
  EXPECT_NEAR(governor.rate_per_s(), 0.0, 1e-14);
  ASSERT_TRUE(governor.advance(governor.remaining_transition_s() + 1.0).has_value());
  EXPECT_DOUBLE_EQ(governor.scale(), 0.01);
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), 0.0);
}

TEST(RuntimeScaleGovernor, RepeatedRetargetingPreservesBoundsAndLatestTarget) {
  const std::array targets{0.99, 0.01, 1.0, 0.0, 0.6, 0.2, 0.8, 0.4};
  for (const auto bounds :
       {RuntimeScaleBounds{0.25, 0.5}, kBounds, RuntimeScaleBounds{4.0, 0.05}}) {
    for (double initial : {0.0, 0.25, 1.0}) {
      RuntimeScaleGovernor governor(initial, bounds);
      for (std::size_t update = 0U; update < 80U; ++update) {
        const double scale = governor.scale();
        const double rate = governor.rate_per_s();
        ASSERT_TRUE(governor.retarget(targets[update % targets.size()]));
        EXPECT_DOUBLE_EQ(governor.scale(), scale);
        EXPECT_DOUBLE_EQ(governor.rate_per_s(), rate);
        constexpr double dt = 0.013;
        const auto advance = governor.advance(dt);
        ASSERT_TRUE(advance.has_value());
        EXPECT_GE(*advance, 0.0);
        EXPECT_LE(*advance, dt + 1e-14);
        EXPECT_GE(governor.scale(), 0.0);
        EXPECT_LE(governor.scale(), 1.0);
        EXPECT_LE(std::abs(governor.rate_per_s()), bounds.max_rate_per_s + 1e-13);
        EXPECT_LE(std::abs(governor.rate_per_s() - rate),
                  bounds.max_rate_change_per_s2 * dt + 1e-13);
      }
      ASSERT_TRUE(governor.advance(governor.remaining_transition_s() + 1.0).has_value());
      EXPECT_DOUBLE_EQ(governor.scale(), targets.back());
      EXPECT_DOUBLE_EQ(governor.rate_per_s(), 0.0);
      EXPECT_FALSE(governor.transitioning());
    }
  }
}

TEST(RuntimeScaleGovernor, SameTargetDoesNotRestartAnExistingTransition) {
  RuntimeScaleGovernor governor(0.0, kBounds);
  ASSERT_TRUE(governor.retarget(1.0));
  ASSERT_TRUE(governor.advance(0.125).has_value());
  auto uninterrupted = governor;
  ASSERT_TRUE(governor.retarget(1.0));
  EXPECT_DOUBLE_EQ(governor.advance(0.25).value(), uninterrupted.advance(0.25).value());
  EXPECT_DOUBLE_EQ(governor.scale(), uninterrupted.scale());
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), uninterrupted.rate_per_s());
}

TEST(RuntimeScaleGovernor, ZeroHoldAndResumeDoNotAdvanceWhileHeld) {
  RuntimeScaleGovernor governor(1.0, kBounds);
  ASSERT_TRUE(governor.retarget(0.0));
  EXPECT_NEAR(governor.advance(0.5).value(), 0.25, 1e-14);
  EXPECT_DOUBLE_EQ(governor.scale(), 0.0);
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), 0.0);
  EXPECT_DOUBLE_EQ(governor.advance(100.0).value(), 0.0);
  ASSERT_TRUE(governor.retarget(1.0));
  EXPECT_NEAR(governor.advance(0.5).value(), 0.25, 1e-14);
  EXPECT_DOUBLE_EQ(governor.scale(), 1.0);
}

TEST(RuntimeScaleGovernor, InvalidInputsDoNotMutateTheActiveTransition) {
  RuntimeScaleGovernor governor(0.0, kBounds);
  ASSERT_TRUE(governor.retarget(1.0));
  ASSERT_TRUE(governor.advance(0.125).has_value());
  auto unchanged = governor;
  for (double invalid :
       {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_FALSE(governor.retarget(invalid));
    EXPECT_FALSE(governor.advance(invalid).has_value());
  }
  EXPECT_FALSE(governor.retarget(1.01));
  EXPECT_DOUBLE_EQ(governor.advance(0.25).value(), unchanged.advance(0.25).value());
  EXPECT_DOUBLE_EQ(governor.scale(), unchanged.scale());
  EXPECT_DOUBLE_EQ(governor.rate_per_s(), unchanged.rate_per_s());
  EXPECT_THROW((RuntimeScaleGovernor(-0.1, kBounds)), std::invalid_argument);
  EXPECT_THROW((RuntimeScaleGovernor(0.0, {0.0, 1.0})), std::invalid_argument);
  EXPECT_THROW((RuntimeScaleGovernor(0.0, {1.0, 0.0})), std::invalid_argument);
}

TEST(RuntimeScaleGovernor, TinyTailToZeroDoesNotGetStuckOnCancellationRoundoff) {
  for (double extra : {0.0, 1.0}) {
    RuntimeScaleGovernor governor(1.0, kBounds);
    ASSERT_TRUE(governor.retarget(0.0));
    ASSERT_TRUE(governor.advance(0.25).has_value());
    ASSERT_TRUE(governor.advance(0.25 - 1e-10).has_value());
    EXPECT_GT(governor.scale(), 0.0);
    const auto tail = governor.advance(governor.remaining_transition_s() + extra);
    ASSERT_TRUE(tail.has_value());
    EXPECT_GE(*tail, 0.0);
    EXPECT_DOUBLE_EQ(governor.scale(), 0.0);
    EXPECT_DOUBLE_EQ(governor.rate_per_s(), 0.0);
    EXPECT_FALSE(governor.transitioning());
    EXPECT_DOUBLE_EQ(governor.advance(1.0).value(), 0.0);
  }
}

}  // namespace
}  // namespace manyforge::core::detail
