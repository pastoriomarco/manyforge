#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <manyforge_core/execution/prepared_motion_builder.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "prepared_participant_test_fixture.hpp"

namespace manyforge::core {
namespace {

using namespace prepared_participant_fixture;

constexpr TrajectoryRuntimeScaleBoundsDTO kSealedBounds{2.0, 4.0, 1.3, 1.0};

// ------------------------------------------------------------- trajectories

// Minimum-jerk rest-to-rest motion, reproduced exactly by quintic knots.
TrajectoryDTO SmoothTrajectory() {
  constexpr double kDuration = 2.0;
  constexpr std::array<double, 2> kTravel{0.6, -0.4};
  TrajectoryDTO trajectory{
      {ResourceId("joint.1"), ResourceId("joint.2")}, {"joint_1", "joint_2"}, {}, kSealedBounds};
  for (const double t : {0.0, 0.5, 1.0, 1.5, 2.0}) {
    const double u = t / kDuration;
    TrajectoryWaypointDTO point{t, {}, {}, {}};
    // `+ 0.0` normalizes a negative zero, which canonical JSON refuses.
    for (const double travel : kTravel) {
      point.positions_rad.push_back(
          travel * (10 * u * u * u - 15 * u * u * u * u + 6 * u * u * u * u * u) + 0.0);
      point.velocities_rad_s.push_back(
          travel * (30 * u * u - 60 * u * u * u + 30 * u * u * u * u) / kDuration + 0.0);
      point.accelerations_rad_s2.push_back(
          travel * (60 * u - 180 * u * u + 120 * u * u * u) / (kDuration * kDuration) + 0.0);
    }
    trajectory.waypoints.push_back(point);
  }
  return trajectory;
}

// A sharp joint-space corner within one short segment.
TrajectoryDTO CornerTrajectory() {
  return TrajectoryDTO{{ResourceId("joint.1"), ResourceId("joint.2")},
                       {"joint_1", "joint_2"},
                       {{0.0, {0.0, 0.0}, {0.0, 0.0}, {0.0, 0.0}},
                        {0.6, {0.2, 0.0}, {0.6, 0.0}, {0.0, 0.0}},
                        {0.75, {0.27, 0.07}, {0.0, 0.6}, {0.0, 0.0}},
                        {1.35, {0.27, 0.27}, {0.0, 0.0}, {0.0, 0.0}}},
                       kSealedBounds};
}

// ------------------------------------------------------------------ inputs

struct Case {
  BasePlanOptions base{};
  TrajectoryDTO trajectory = SmoothTrajectory();
  StateAuthorityClass authority = StateAuthorityClass::kMeasured;
  PerResourceDynamicsLimitsDTO dynamics{};
};

ExecutionGoalKey Key(const PreparedOperationParticipantDTO& participant) {
  return {participant.operation_id, ControllerGoalId("goal.1"), ControllerGoalGeneration(1U),
          PreparedExecutionId(7U),
          ExecutionBindingIdentity{participant.active_control_configuration_generation,
                                   participant.control_provider.runtime_epoch}};
}

// The host side: Prepare, seal and build.
struct Host final {
  SealedInputs inputs;
  PreparedOperationParticipantDTO participant;
  SealedPreparedMotionInputs sealed;
  Result<PreparedMotionSlot> slot;
};

Host Prepare(const Case& value) {
  const auto base = BasePlan(value.base);
  auto inputs = Inputs(base, false, value.authority);
  auto payload = MotionPayload(inputs, value.trajectory);
  payload.plan_request.per_resource_dynamics_limits = value.dynamics;
  auto participant = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, std::move(payload)), inputs.binding,
      inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  if (!participant.ok()) throw std::logic_error(participant.error().details.at("reason"));
  auto sealed =
      MakeSealedPreparedMotionInputs(participant.value(), inputs.binding, inputs.readiness,
                                     DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  if (!sealed.ok()) throw std::logic_error(sealed.error().details.at("reason"));
  auto slot =
      BuildPreparedMotionSlot(sealed.value(), Key(participant.value()), inputs.binding,
                              inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  return {std::move(inputs), std::move(participant.value()), std::move(sealed.value()),
          std::move(slot)};
}

// A receiver: its own configured binding/readiness, the transmitted sealed
// bytes and the transmitted key; nothing else crosses.
Result<PreparedMotionSlot> Receive(const Case& value, const SealedPreparedMotionInputs& sealed,
                                   const ExecutionGoalKey& key) {
  const auto base = BasePlan(value.base);
  const auto inputs = Inputs(base, false, value.authority);
  return BuildPreparedMotionSlot(sealed, key, inputs.binding, inputs.readiness, DefaultLimits(),
                                 MotionInterruptNodeCategory::kGeneric);
}

std::string Reason(const Result<PreparedMotionSlot>& result) {
  return result.ok() ? std::string("ok") : result.error().details.at("reason");
}

std::string Describe(const Result<PreparedMotionSlot>& result) {
  if (result.ok()) return "ok";
  std::string text;
  for (const auto& [name, value] : result.error().details) text += name + "=" + value + " ";
  return text;
}

const PreparedMotion& Motion(const Host& host) {
  if (!host.slot.ok()) throw std::logic_error(Describe(host.slot));
  return host.slot.value().motion;
}

// -------------------------------------------------------------- comparisons

void ExpectSameSlot(const PreparedMotionSlot& a, const PreparedMotionSlot& b) {
  EXPECT_EQ(a.sealed, b.sealed);
  const auto& x = a.motion;
  const auto& y = b.motion;
  EXPECT_EQ(x.key, y.key);
  EXPECT_EQ(x.prepared_contract_digest, y.prepared_contract_digest);
  EXPECT_EQ(EncodePreparedTrajectory(x.trajectory).value(),
            EncodePreparedTrajectory(y.trajectory).value());
  EXPECT_EQ(x.interpolation, y.interpolation);
  EXPECT_EQ(x.joint_count, y.joint_count);
  EXPECT_EQ(x.runtime_scale_bounds.max_rate_per_s, y.runtime_scale_bounds.max_rate_per_s);
  EXPECT_EQ(x.runtime_scale_bounds.max_rate_change_per_s2,
            y.runtime_scale_bounds.max_rate_change_per_s2);
  EXPECT_EQ(x.runtime_scale_bounds.max_stop_path_rate, y.runtime_scale_bounds.max_stop_path_rate);
  EXPECT_EQ(x.runtime_scale_bounds.nominal_time_multiplier,
            y.runtime_scale_bounds.nominal_time_multiplier);
  EXPECT_EQ(x.governor_bounds.max_rate_per_s, y.governor_bounds.max_rate_per_s);
  EXPECT_EQ(x.governor_bounds.max_rate_change_per_s2, y.governor_bounds.max_rate_change_per_s2);
  EXPECT_EQ(x.effective_limits, y.effective_limits);
  EXPECT_EQ(x.state_authority, y.state_authority);
  EXPECT_EQ(x.final_position_tolerance_rad, y.final_position_tolerance_rad);
  EXPECT_EQ(x.rest_velocity_threshold_rad_s, y.rest_velocity_threshold_rad_s);
}

ResolvedResourceDynamicsLimitDTO Dynamics(const char* resource, const char* joint,
                                          JointDynamicsLimitDTO limit) {
  return {ResourceId(resource), joint, limit};
}

// ------------------------------------------------------------------- tests

TEST(PreparedMotionBuilderTest, DerivesEveryFieldFromSealedInputs) {
  Case value;
  value.dynamics = {Dynamics("joint.1", "joint_1", {3.0, 20.0, std::nullopt}),
                    Dynamics("joint.2", "joint_2", {std::nullopt, 10.0, 50.0})};
  const auto host = Prepare(value);
  ASSERT_TRUE(host.slot.ok()) << Describe(host.slot);
  const auto& slot = host.slot.value();
  const auto& motion = slot.motion;
  EXPECT_EQ(slot.sealed, host.sealed);
  EXPECT_EQ(motion.key, Key(host.participant));
  EXPECT_EQ(motion.prepared_contract_digest, host.participant.prepared_contract_digest);
  EXPECT_EQ(EncodePreparedTrajectory(motion.trajectory).value(),
            host.sealed.canonical_trajectory_bytes);
  EXPECT_EQ(motion.interpolation, MotionInterpolation::kQuintic);
  EXPECT_EQ(motion.joint_count, 2U);
  // Sealed time-law bounds, unchanged (Stop path rate included).
  EXPECT_EQ(motion.runtime_scale_bounds.max_rate_per_s, kSealedBounds.max_rate_per_s);
  EXPECT_EQ(motion.runtime_scale_bounds.max_rate_change_per_s2,
            kSealedBounds.max_rate_change_per_s2);
  EXPECT_EQ(motion.runtime_scale_bounds.max_stop_path_rate, kSealedBounds.max_stop_path_rate);
  EXPECT_EQ(motion.runtime_scale_bounds.nominal_time_multiplier,
            kSealedBounds.nominal_time_multiplier);
  EXPECT_EQ(motion.governor_bounds.max_rate_per_s, kSealedBounds.max_rate_per_s);
  EXPECT_EQ(motion.governor_bounds.max_rate_change_per_s2, kSealedBounds.max_rate_change_per_s2);
  // The motion request's resolved dynamics caps, per trajectory resource.
  EXPECT_EQ(motion.effective_limits[0], value.dynamics[0].limit);
  EXPECT_EQ(motion.effective_limits[1], value.dynamics[1].limit);
  EXPECT_EQ(motion.effective_limits[2], JointDynamicsLimitDTO{});
  EXPECT_EQ(motion.state_authority, StateAuthorityClass::kMeasured);
  // The motion request's joint position tolerance (fixture: 0.01).
  EXPECT_EQ(motion.final_position_tolerance_rad[0], 0.01);
  EXPECT_EQ(motion.final_position_tolerance_rad[1], 0.01);
  EXPECT_EQ(motion.final_position_tolerance_rad[2], 0.0);
  // Retained default and controlled-stage settle thresholds (both 0.02).
  EXPECT_EQ(motion.rest_velocity_threshold_rad_s[0], 0.02);
  EXPECT_EQ(motion.rest_velocity_threshold_rad_s[1], 0.02);
  EXPECT_EQ(motion.rest_velocity_threshold_rad_s[2], 0.0);
}

TEST(PreparedMotionBuilderTest, AbsentDynamicsCapsStayAbsent) {
  const auto host = Prepare(Case{});
  const auto& motion = Motion(host);
  EXPECT_EQ(motion.effective_limits[0], JointDynamicsLimitDTO{});
  EXPECT_EQ(motion.effective_limits[1], JointDynamicsLimitDTO{});
}

TEST(PreparedMotionBuilderTest, StateAuthorityFollowsTheBindingControlPath) {
  for (const auto authority : {StateAuthorityClass::kPredicted, StateAuthorityClass::kCommanded}) {
    Case value;
    value.authority = authority;
    EXPECT_EQ(Motion(Prepare(value)).state_authority, authority);
  }
}

TEST(PreparedMotionBuilderTest, RestVelocityThresholdIsTheStrictestRetainedThreshold) {
  // A controlled Stop stage may only tighten the retained default (0.02,
  // the fixture's equal case is covered above); the stricter stage wins.
  Case stricter;
  stricter.base.settle_velocity_threshold_rad_s = 0.01;
  const auto& tight = Motion(Prepare(stricter));
  EXPECT_EQ(tight.rest_velocity_threshold_rad_s[0], 0.01);
  EXPECT_EQ(tight.rest_velocity_threshold_rad_s[1], 0.01);
}

TEST(PreparedMotionBuilderTest, HostAndReceiverRebuildIdenticalSlots) {
  Case corner;
  corner.trajectory = CornerTrajectory();
  corner.dynamics = {Dynamics("joint.1", "joint_1", {3.0, 20.0, 100.0})};
  for (const auto& value : {Case{}, corner}) {
    const auto host = Prepare(value);
    ASSERT_TRUE(host.slot.ok()) << Describe(host.slot);
    // Transport copies only the sealed bytes and the key.
    const SealedPreparedMotionInputs transmitted{
        ContentDigest(std::string(host.sealed.participant_digest.value())),
        std::string(host.sealed.canonical_participant_bytes),
        std::string(host.sealed.canonical_trajectory_bytes)};
    const auto received = Receive(value, transmitted, Key(host.participant));
    ASSERT_TRUE(received.ok()) << Reason(received);
    ExpectSameSlot(host.slot.value(), received.value());
    // Repeated builds are bit-identical as well.
    const auto again = Receive(value, transmitted, Key(host.participant));
    ASSERT_TRUE(again.ok());
    ExpectSameSlot(received.value(), again.value());
  }
}

TEST(PreparedMotionBuilderTest, RefusesForgedOrMismatchedSealedInputs) {
  const Case value;
  const auto host = Prepare(value);
  ASSERT_TRUE(host.slot.ok()) << Describe(host.slot);
  const auto key = Key(host.participant);

  // Altered participant bytes: re-verification fails (digest included).
  auto tampered = host.sealed;
  const auto at = tampered.canonical_participant_bytes.find("\"operation_revision\":1");
  ASSERT_NE(at, std::string::npos);
  tampered.canonical_participant_bytes.replace(at, 22U, "\"operation_revision\":2");
  EXPECT_EQ(Reason(Receive(value, tampered, key)), "prepared_motion_participant_invalid");

  // A transported digest other than the recomputed one.
  auto forged = host.sealed;
  forged.participant_digest = ContentDigest(Digest('f'));
  EXPECT_EQ(Reason(Receive(value, forged, key)), "prepared_motion_participant_digest_mismatch");

  // Another valid trajectory than the participant's.
  auto swapped = host.sealed;
  swapped.canonical_trajectory_bytes = EncodePreparedTrajectory(CornerTrajectory()).value();
  EXPECT_EQ(Reason(Receive(value, swapped, key)), "prepared_motion_trajectory_mismatch");
  swapped.canonical_trajectory_bytes = "{}";
  EXPECT_EQ(Reason(Receive(value, swapped, key)), "prepared_motion_trajectory_invalid");

  // A receiver whose retained Stop configuration differs cannot verify.
  auto other_stop = value;
  other_stop.base.max_deceleration_rad_s2 = 15.0;
  EXPECT_EQ(Reason(Receive(other_stop, host.sealed, key)), "prepared_motion_participant_invalid");

  // Keys bound to another operation, configuration or provider epoch.
  auto wrong = key;
  wrong.operation_id = OperationId("operation.2");
  EXPECT_EQ(Reason(Receive(value, host.sealed, wrong)), "prepared_motion_key_mismatch");
  wrong = key;
  wrong.binding.provider_runtime_epoch = RuntimeEpoch("epoch.other");
  EXPECT_EQ(Reason(Receive(value, host.sealed, wrong)), "prepared_motion_key_mismatch");
  wrong = key;
  wrong.binding.active_control_configuration_generation = ActiveControlConfigurationGeneration(
      key.binding.active_control_configuration_generation.value() + 1U);
  EXPECT_EQ(Reason(Receive(value, host.sealed, wrong)), "prepared_motion_key_mismatch");
  wrong = key;
  wrong.controller_goal_generation = ControllerGoalGeneration(0U);
  EXPECT_EQ(Reason(Receive(value, host.sealed, wrong)), "prepared_motion_key_mismatch");
}

TEST(PreparedMotionBuilderTest, RefusesATrajectoryWithoutRuntimeScaleBounds) {
  Case value;
  value.trajectory.runtime_scale_bounds.reset();
  EXPECT_EQ(Reason(Prepare(value).slot), "prepared_motion_runtime_scale_bounds_missing");
}

}  // namespace
}  // namespace manyforge::core
