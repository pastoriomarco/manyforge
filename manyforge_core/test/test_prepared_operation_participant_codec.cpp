#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/prepared_planning_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/types/prepared_execution.hpp>

#include "prepared_participant_test_fixture.hpp"

namespace manyforge::core {
namespace {

using namespace prepared_participant_fixture;

TEST(PreparedOperationParticipantCodecTest, SealsAndRoundTripsBothPayloadTags) {
  const auto base = BasePlan();
  auto command_inputs = Inputs(base, true);
  auto command = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command.ok()) << command.error().details.at("reason");
  const auto command_bytes = EncodePreparedOperationParticipant(
      command.value(), command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command_bytes.ok()) << command_bytes.error().message;
  // Current candidates prove deterministic construction and round-trip
  // integrity; the obsolete pre-typed-Stop aggregate hash is not a contract.
  const auto rebuilt = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(rebuilt.ok());
  EXPECT_EQ(EncodePreparedOperationParticipant(rebuilt.value(), command_inputs.binding,
                                               command_inputs.readiness, DefaultLimits(),
                                               MotionInterruptNodeCategory::kGeneric)
                .value(),
            command_bytes.value());
  const auto command_decoded = DecodePreparedOperationParticipant(
      command_bytes.value(), command_inputs.binding, command_inputs.readiness,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command_decoded.ok()) << command_decoded.error().message;
  EXPECT_EQ(EncodePreparedOperationParticipant(command_decoded.value(), command_inputs.binding,
                                               command_inputs.readiness, DefaultLimits(),
                                               MotionInterruptNodeCategory::kGeneric)
                .value(),
            command_bytes.value());

  auto motion_inputs = Inputs(base, false);
  auto capped_payload = MotionPayload(motion_inputs);
  capped_payload.requested_tcp_linear_speed_mps = 0.75;
  auto motion = MakePreparedOperationParticipant(
      Candidate(motion_inputs.binding, motion_inputs.readiness, capped_payload),
      motion_inputs.binding, motion_inputs.readiness,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion.ok()) << motion.error().details.at("reason");
  const auto motion_bytes = EncodePreparedOperationParticipant(
      motion.value(), motion_inputs.binding, motion_inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion_bytes.ok()) << motion_bytes.error().message;
  const auto motion_decoded = DecodePreparedOperationParticipant(
      motion_bytes.value(), motion_inputs.binding, motion_inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion_decoded.ok()) << motion_decoded.error().message;
  const auto& decoded_payload =
      std::get<PreparedMotionExecutionDTO>(motion_decoded.value().payload);
  EXPECT_EQ(decoded_payload.move_id.value(), "move.1");
  ASSERT_TRUE(
      decoded_payload.validation_evidence.noncollision_result.trajectory_metrics.has_value());
  const auto& metrics = *decoded_payload.validation_evidence.noncollision_result.trajectory_metrics;
  EXPECT_DOUBLE_EQ(metrics.trajectory_duration_s, 1.0);
  EXPECT_EQ(metrics.peak_abs_joint_velocity_rad_s, (std::vector<double>{0.5, 0.5}));
  EXPECT_EQ(metrics.peak_abs_joint_acceleration_rad_s2, (std::vector<double>{0.0, 0.0}));
  ASSERT_TRUE(metrics.peak_tcp_linear_speed_mps.has_value());
  EXPECT_DOUBLE_EQ(*metrics.peak_tcp_linear_speed_mps, 0.1);
  ASSERT_TRUE(decoded_payload.requested_tcp_linear_speed_mps.has_value());
  EXPECT_DOUBLE_EQ(*decoded_payload.requested_tcp_linear_speed_mps, 0.75);
  EXPECT_DOUBLE_EQ(decoded_payload.deployment_maximum_tcp_linear_speed_mps, 0.5);
  EXPECT_EQ(decoded_payload.plan_request.speed_profile.max_tcp_linear_speed_mps, 0.5);
}

TEST(PreparedOperationParticipantCodecTest, SealedMotionInputsAreTheVerifiedCanonicalBytes) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  const auto motion = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(motion.ok()) << motion.error().details.at("reason");
  const auto sealed =
      MakeSealedPreparedMotionInputs(motion.value(), inputs.binding, inputs.readiness,
                                     DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(sealed.ok()) << sealed.error().details.at("reason");
  EXPECT_EQ(sealed.value().participant_digest, motion.value().prepared_contract_digest);
  EXPECT_EQ(sealed.value().canonical_participant_bytes,
            EncodePreparedOperationParticipant(motion.value(), inputs.binding, inputs.readiness,
                                               DefaultLimits(),
                                               MotionInterruptNodeCategory::kGeneric)
                .value());
  // A receiver decodes exactly the sealed participant and its trajectory.
  const auto decoded = DecodePreparedOperationParticipant(
      sealed.value().canonical_participant_bytes, inputs.binding, inputs.readiness,
      DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  const auto trajectory = DecodePreparedTrajectory(sealed.value().canonical_trajectory_bytes);
  ASSERT_TRUE(trajectory.ok()) << trajectory.error().message;
  EXPECT_EQ(EncodePreparedTrajectory(trajectory.value()).value(),
            sealed.value().canonical_trajectory_bytes);
  EXPECT_EQ(EncodePreparedTrajectory(
                std::get<PreparedMotionExecutionDTO>(decoded.value().payload).trajectory)
                .value(),
            sealed.value().canonical_trajectory_bytes);

  // A forged digest and a non-motion payload refuse.
  auto forged = motion.value();
  forged.prepared_contract_digest = ContentDigest{Digest('f')};
  EXPECT_FALSE(MakeSealedPreparedMotionInputs(forged, inputs.binding, inputs.readiness,
                                              DefaultLimits(), MotionInterruptNodeCategory::kGeneric)
                   .ok());
  auto command_inputs = Inputs(base, true);
  const auto command = MakePreparedOperationParticipant(
      Candidate(command_inputs.binding, command_inputs.readiness, CommandPayload()),
      command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(command.ok());
  const auto refused = MakeSealedPreparedMotionInputs(
      command.value(), command_inputs.binding, command_inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "sealed_motion_inputs_require_motion_payload");
}

TEST(PreparedOperationParticipantCodecTest, RejectsCartesianMilestonesForAnotherIntent) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto payload = MotionPayload(inputs);
  payload.trajectory.cartesian_waypoint_times_s = {0.0, 1.0};
  const auto seal = [&] {
    return MakePreparedOperationParticipant(
        Candidate(inputs.binding, inputs.readiness, payload), inputs.binding, inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  };
  auto wrong_kind = seal();
  ASSERT_FALSE(wrong_kind.ok());
  EXPECT_EQ(wrong_kind.error().details.at("reason"),
            "prepared_cartesian_waypoint_milestones_mismatch");
  CartesianPathDTO path;
  path.waypoints.resize(3U);
  payload.plan_request.motion = path;
  auto wrong_count = seal();
  ASSERT_FALSE(wrong_count.ok());
  EXPECT_EQ(wrong_count.error().details.at("reason"),
            "prepared_cartesian_waypoint_milestones_mismatch");
}

TEST(PreparedOperationParticipantCodecTest, RejectsSelfAttestedClaimsAndDigestForgery) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, true);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, CommandPayload()), inputs.binding,
      inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  auto forged = made.value();
  forged.claimed_command_interfaces.front().interface_name = "velocity";
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
  forged = made.value();
  forged.prepared_contract_digest = ContentDigest(Digest('0'));
  EXPECT_FALSE(VerifyPreparedOperationParticipant(forged, inputs.binding, inputs.readiness,
                                                  DefaultLimits(),
                                                  MotionInterruptNodeCategory::kGeneric)
                   .ok());
}

TEST(PreparedOperationParticipantCodecTest, BackendBindingRejectsCrossedParticipant) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());
  EXPECT_FALSE(
      prepared_participant_backend_binding_field_error(
          made.value(), inputs.binding, PreparedParticipantPayloadKindDTO::kMotionExecution)
          .has_value());
  EXPECT_EQ(
      prepared_participant_backend_binding_field_error(
          made.value(), inputs.binding, PreparedParticipantPayloadKindDTO::kCapabilityCommand),
      "prepared_participant_backend_shape_invalid");

  auto crossed = made.value();
  crossed.control_route_id = ControlRouteId("route.other");
  EXPECT_EQ(prepared_participant_backend_binding_field_error(
                crossed, inputs.binding, PreparedParticipantPayloadKindDTO::kMotionExecution),
            "prepared_participant_backend_control_route_id_mismatch");
}

TEST(PreparedOperationParticipantCodecTest,
     BackendPhysicalBindingAcceptsContainedProgramAndRejectsCrossedRoute) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  auto made = MakePreparedOperationParticipant(
      Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs)), inputs.binding,
      inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_TRUE(made.ok());

  auto physical_value = inputs.binding.value();
  physical_value.program_id = ProgramId("direct-control.product-owner");
  physical_value.program_revision = ProgramRevision(Digest('e'));
  physical_value.program_binding_id = ProgramBindingId(Digest('f'));
  physical_value.component_role_id = ComponentRoleId("direct_control_component");
  physical_value.target_role_id = TargetRoleId("direct_control_target");
  auto physical = verify_runtime_participant_binding(std::move(physical_value));
  ASSERT_TRUE(physical.ok()) << physical.error().message;

  EXPECT_EQ(
      prepared_participant_backend_binding_field_error(
          made.value(), physical.value(), PreparedParticipantPayloadKindDTO::kMotionExecution),
      "prepared_participant_backend_program_id_mismatch");
  EXPECT_FALSE(
      prepared_participant_backend_physical_binding_field_error(
          made.value(), physical.value(), PreparedParticipantPayloadKindDTO::kMotionExecution)
          .has_value());

  auto crossed = made.value();
  crossed.control_route_id = ControlRouteId("route.other");
  EXPECT_EQ(prepared_participant_backend_physical_binding_field_error(
                crossed, physical.value(), PreparedParticipantPayloadKindDTO::kMotionExecution),
            "prepared_participant_backend_control_route_id_mismatch");
}

TEST(PreparedOperationParticipantCodecTest,
     MotionRefusesInterruptScopesAndRequiresDeploymentMaximum) {
  const auto base = BasePlan();
  auto inputs = Inputs(base, false);
  const auto candidate = Candidate(inputs.binding, inputs.readiness, MotionPayload(inputs));

  auto interrupted = candidate;
  std::get<PreparedMotionExecutionDTO>(interrupted.payload)
      .interrupt_scopes.push_back(PreparedMotionInterruptScopeDTO{
          MotionInterruptScopeId("scope.1"), InterruptPolicyId("interrupt.contact"),
          MotionInterruptActivationMode::kRequireInactiveAtArm, std::nullopt, std::nullopt, true,
          MotionInterruptStopProfileDTO{}, MotionInterruptNodeCategory::kGeneric, {},
          candidate.ordinary_stop_plan});
  auto interrupted_result =
      MakePreparedOperationParticipant(std::move(interrupted), inputs.binding, inputs.readiness,
                                       DefaultLimits(), MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(interrupted_result.ok());
  EXPECT_EQ(interrupted_result.error().details.at("reason"),
            "motion_interrupt_policy_not_admitted");

  auto weaker = candidate;
  std::get<PreparedMotionExecutionDTO>(weaker.payload)
      .plan_request.speed_profile.max_tcp_linear_speed_mps = 0.75;
  auto weaker_result = MakePreparedOperationParticipant(
      std::move(weaker), inputs.binding, inputs.readiness, DefaultLimits(),
      MotionInterruptNodeCategory::kGeneric);
  ASSERT_FALSE(weaker_result.ok());
  EXPECT_EQ(weaker_result.error().details.at("reason"), "motion_payload_join_invalid");
  auto unexpectedly_lower = candidate;
  std::get<PreparedMotionExecutionDTO>(unexpectedly_lower.payload)
      .plan_request.speed_profile.max_tcp_linear_speed_mps = 0.25;
  EXPECT_FALSE(MakePreparedOperationParticipant(
      std::move(unexpectedly_lower), inputs.binding, inputs.readiness, DefaultLimits(), MotionInterruptNodeCategory::kGeneric).ok());
}

}  // namespace
}  // namespace manyforge::core
