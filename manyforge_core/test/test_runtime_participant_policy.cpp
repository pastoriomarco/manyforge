#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/types/tcp_speed_check.hpp>
#include <optional>
#include <string>

namespace {

using namespace manyforge::core;

[[nodiscard]] RuntimeParticipantBinding binding() {
  auto result = verify_runtime_participant_binding(RuntimeParticipantBindingDTO{
      ProgramId("program"),
      ProgramRevision("program.revision"),
      ProgramBindingId("program.binding"),
      RetainedBundleId("bundle"),
      GraphRevision("graph"),
      ComponentRoleId("robot"),
      TargetRoleId("arm"),
      ControllableComponentId("robot.1"),
      ComponentTypeId("robot.type"),
      ComponentTypeRevision("robot.type.revision"),
      CommandTargetId("arm"),
      CapabilityId("execution.trajectory.execute"),
      CapabilityDefinitionRevision("capability.revision"),
      std::nullopt,
      {ProgramModelRoleBindingDTO{ModelRoleId("body"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision")}},
      {RuntimeModelGroupBindingDTO{
          ModelGroupRoleId("arm"), ModelInstanceId("robot.model"), "robot_arm", {"joint_1"}}},
      {ProgramFrameRoleBindingDTO{FrameRoleId("tcp"), ModelInstanceId("robot.model"),
                                  ModelRevision("model.revision"), "tcp"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("joint_1"), ResourceId("robot.joint.1"), "joint_1",
                                 "joint_1", CoordinateKind::kAngular}},
      {},
      RuntimeControlPathDTO{ControlDomainId("domain"),
                            ControlDomainRevision("domain.revision"),
                            ControlBindingId("binding"),
                            ControlBindingRevision("binding.revision"),
                            ControlInterfaceContractId("interface"),
                            ControlInterfaceRevision("interface.revision"),
                            ControllerTopologyId("topology"),
                            TopologyRevision("topology.revision"),
                            ControlRouteId("route"),
                            ActivationSetId("activation"),
                            ActiveControlConfigurationId("active"),
                            ActiveControlConfigurationGeneration(1U),
                            ProviderInstanceId("provider"),
                            ProviderManifestId("manifest"),
                            ProviderManifestRevision("manifest.revision"),
                            RuntimeUnitId("runtime"),
                            RuntimeEpoch("runtime.epoch"),
                            StateAuthorityId("state"),
                            StateAuthorityClass::kMeasured,
                            StopEvidencePolicyId("stop"),
                            StopEvidencePolicyRevision("stop.revision"),
                            ContainmentGranularity::kResource,
                            std::nullopt},
      std::nullopt,
      {},
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime"), RuntimeEpoch("runtime.epoch")}}});
  EXPECT_TRUE(result.ok()) << result.error().message;
  return std::move(result.value());
}

TEST(RuntimeParticipantPolicy, SealsExactTargetWithoutJerkProfile) {
  const auto participant = binding();
  const auto selected = select_runtime_participant_policy(participant);
  EXPECT_EQ(selected.value().controllable_component_id, ControllableComponentId("robot.1"));
  EXPECT_EQ(selected.value().command_target_id, CommandTargetId("arm"));
  EXPECT_FALSE(selected.value().ordinary_motion_jerk);
}

TEST(RuntimeParticipantPolicy, DeploymentMaximumCapsOperationRequest) {
  auto unspecified = apply_deployment_motion_speed_limit(0.25, {});
  ASSERT_TRUE(unspecified.ok());
  EXPECT_EQ(unspecified.value().max_tcp_linear_speed_mps, 0.25);

  SpeedProfileDTO tighter;
  tighter.max_tcp_linear_speed_mps = 0.1;
  auto applied = apply_deployment_motion_speed_limit(0.25, tighter);
  ASSERT_TRUE(applied.ok());
  EXPECT_EQ(applied.value().max_tcp_linear_speed_mps, 0.1);
  tighter.max_tcp_linear_speed_mps = 0.3;
  const auto excessive = apply_deployment_motion_speed_limit(0.25, tighter);
  ASSERT_TRUE(excessive.ok());
  EXPECT_EQ(excessive.value().max_tcp_linear_speed_mps, 0.25);
  tighter.max_tcp_linear_speed_mps = 0.25;
  EXPECT_EQ(apply_deployment_motion_speed_limit(0.25, tighter).value()
                .max_tcp_linear_speed_mps, 0.25);
  // Resolve the same authored request independently for each target.
  EXPECT_EQ(apply_deployment_motion_speed_limit(0.15, tighter).value()
                .max_tcp_linear_speed_mps, 0.15);
  tighter.max_tcp_linear_speed_mps = 0.0;
  EXPECT_FALSE(apply_deployment_motion_speed_limit(0.25, tighter).ok());
  tighter.max_tcp_linear_speed_mps = -0.1;
  EXPECT_FALSE(apply_deployment_motion_speed_limit(0.25, tighter).ok());
  for (const double invalid : {std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    tighter.max_tcp_linear_speed_mps = invalid;
    EXPECT_FALSE(apply_deployment_motion_speed_limit(0.25, tighter).ok());
    EXPECT_FALSE(apply_deployment_motion_speed_limit(invalid, {}).ok());
  }
  EXPECT_FALSE(apply_deployment_motion_speed_limit(0.0, {}).ok());
  EXPECT_FALSE(apply_deployment_motion_speed_limit(1000.1, {}).ok());
}

TEST(RuntimeParticipantPolicy, TcpSpeedCheckNamesExactlyTwoModes) {
  EXPECT_EQ(parse_tcp_speed_check("active_tcp"), TcpSpeedCheck::kActiveTcp);
  EXPECT_EQ(parse_tcp_speed_check("active_tcp_and_flange"),
            TcpSpeedCheck::kActiveTcpAndFlange);
  for (const auto invalid : {"", "flange", "Active_TCP", "active_tcp_and_tool"})
    EXPECT_FALSE(parse_tcp_speed_check(invalid)) << invalid;
  for (const auto mode : {TcpSpeedCheck::kActiveTcp, TcpSpeedCheck::kActiveTcpAndFlange})
    EXPECT_EQ(parse_tcp_speed_check(to_string(mode)), mode);
}

TEST(RuntimeParticipantPolicy, SpeedCheckFramesAddTheRobotFlangeOnce) {
  const ResolvedFrameDTO robot_tcp = ResolvedComponentModelFrameDTO{
      ControllableComponentId("lite6"), ModelInstanceId("lite6.model"), FrameRoleId("tcp"),
      "mf2_link_tcp", ModelRevision("lite6.revision")};
  const ResolvedFrameDTO flange = ResolvedComponentModelFrameDTO{
      ControllableComponentId("lite6"), ModelInstanceId("lite6.model"), FrameRoleId("flange"),
      "mf2_link_eef", ModelRevision("lite6.revision")};
  // `active_tcp`: the active TCP alone.
  EXPECT_EQ(speed_check_frames(robot_tcp, std::nullopt), std::vector{robot_tcp});
  // `active_tcp_and_flange`: the TCP first, then the robot flange.
  EXPECT_EQ(speed_check_frames(robot_tcp, flange), (std::vector{robot_tcp, flange}));
  // A TCP on a mounted tool keeps the robot's flange as the second point.
  const ResolvedFrameDTO tool_tcp = ResolvedComponentModelFrameDTO{
      ControllableComponentId("vacuum_gripper"), ModelInstanceId("vacuum_gripper.model"),
      FrameRoleId("cup_center"), "mf2_cup_center", ModelRevision("vacuum.revision")};
  const auto mounted = speed_check_frames(tool_tcp, flange);
  ASSERT_EQ(mounted.size(), 2U);
  EXPECT_EQ(frame_role_id(mounted[0]), "cup_center");
  EXPECT_EQ(std::get<ResolvedComponentModelFrameDTO>(mounted[1]).controllable_component_id,
            ControllableComponentId("lite6"));
  EXPECT_EQ(frame_role_id(mounted[1]), "flange");
  // The flange itself as TCP, or UR's `tcp` and `flange` on one link: one point.
  EXPECT_EQ(speed_check_frames(flange, flange), std::vector{flange});
  const ResolvedFrameDTO ur_tcp = ResolvedComponentModelFrameDTO{
      ControllableComponentId("ur10e"), ModelInstanceId("ur10e.model"), FrameRoleId("tcp"),
      "mf2_tool0", ModelRevision("ur10e.revision")};
  const ResolvedFrameDTO ur_flange = ResolvedComponentModelFrameDTO{
      ControllableComponentId("ur10e"), ModelInstanceId("ur10e.model"), FrameRoleId("flange"),
      "mf2_tool0", ModelRevision("ur10e.revision")};
  EXPECT_EQ(speed_check_frames(ur_tcp, ur_flange), std::vector{ur_tcp});
  EXPECT_EQ(frame_role_id(ResolvedCellFrameDTO{CellId("cell"), "world"}), "");
}

TEST(RuntimeParticipantPolicy, RefusesEveryMotionInterruptScope) {
  EXPECT_TRUE(validate_motion_interrupt_scope_admission({}).ok());
  MotionInterruptScopeDTO scope;
  scope.scope_id = "scope.1";
  scope.policy_id = "interrupt.contact";
  const auto rejected = validate_motion_interrupt_scope_admission({&scope, 1U});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code, std::string(to_string(ErrorCode::kInvalidRequest)));
  EXPECT_EQ(rejected.error().details.at("reason"), "motion_interrupt_policy_not_admitted");
}

TEST(RuntimeParticipantPolicy, OrdinaryJerkPolicyFillsOmissionsAndRejectsWeakerRequests) {
  const auto participant = binding();
  const auto base = select_runtime_participant_policy(participant);
  OrdinaryMotionJerkPolicy profile{true, false, {{"joint_1", AngularJerkDTO{10.0}}}};
  const auto selected = apply_deployment_motion_jerk_policy(base, participant, profile);
  ASSERT_TRUE(selected.ok());
  profile.limits.at("joint_1") = AngularJerkDTO{100.0};
  const auto filled = apply_runtime_participant_motion_jerk_policy(selected.value(), {});
  ASSERT_TRUE(filled.ok());
  EXPECT_EQ(filled.value().at("joint_1").max_jerk_rad_s3, 10.0);
  PerJointDynamicsLimitsDTO requested{{"joint_1", {1.0, 2.0, 5.0}}};
  const auto tightened = apply_runtime_participant_motion_jerk_policy(selected.value(), requested);
  ASSERT_TRUE(tightened.ok());
  EXPECT_EQ(tightened.value(), requested);
  requested.at("joint_1").max_jerk_rad_s3 = 11.0;
  const auto rejected = apply_runtime_participant_motion_jerk_policy(selected.value(), requested);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "ordinary_motion_jerk_limit_exceeded");
  EXPECT_FALSE(base.value().ordinary_motion_jerk);
}

TEST(RuntimeParticipantPolicy, OrdinaryJerkOffDoesNotActivateRetainedValuesOrWaiveRequiredLimits) {
  const auto participant = binding();
  const auto base = select_runtime_participant_policy(participant);
  OrdinaryMotionJerkPolicy profile{false, false, {{"joint_1", AngularJerkDTO{10.0}}}};
  const auto selected = apply_deployment_motion_jerk_policy(base, participant, profile);
  ASSERT_TRUE(selected.ok());
  EXPECT_TRUE(apply_runtime_participant_motion_jerk_policy(selected.value(), {}).value().empty());
  const auto rejected =
      apply_runtime_participant_motion_jerk_policy(selected.value(), {{"joint_1", {{}, {}, 5.0}}});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "ordinary_motion_jerk_disabled");
  profile.machine_required = true;
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(base, participant, profile).ok());
  profile.enabled = true;
  const auto required = apply_deployment_motion_jerk_policy(base, participant, profile);
  ASSERT_TRUE(required.ok());
  profile.machine_required = false;
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(required.value(), participant, profile).ok());
  profile.machine_required = true;
  profile.limits.at("joint_1") = AngularJerkDTO{11.0};
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(required.value(), participant, profile).ok());
  profile.limits.at("joint_1") = AngularJerkDTO{5.0};
  EXPECT_TRUE(apply_deployment_motion_jerk_policy(required.value(), participant, profile).ok());
}

TEST(RuntimeParticipantPolicy, OrdinaryJerkPolicyRejectsIncompleteCrossedAndWrongUnitLimits) {
  const auto participant = binding();
  const auto base = select_runtime_participant_policy(participant);
  EXPECT_FALSE(
      apply_deployment_motion_jerk_policy(base, participant, {true, false, {}}).ok());
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(base, participant,
                                                   {true, false, {{"other", AngularJerkDTO{10.0}}}})
                   .ok());
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(
                   base, participant, {true, false, {{"joint_1", LinearJerkDTO{10.0}}}})
                   .ok());
  EXPECT_FALSE(apply_deployment_motion_jerk_policy(
                   base, participant, {true, false, {{"joint_1", AngularJerkDTO{0.0}}}})
                   .ok());
}

}  // namespace
