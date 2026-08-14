#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include <manyforge_ros2_control_execution/ros2_control_capability.hpp>

// spec/260 §5.6: the ros2_control adapter's motion-interrupt stop
// capability is computed by a pure package helper that inspects the
// deployment's Ros2ControlExecutionBackendConfig. The helper is callable
// without a running controller so deployments can audit which stop modes a
// configuration will advertise before launching execution.
// These tests pin the evidence-gating rules so the capability gate
// stays honest as the config evolves.

using namespace manyforge::ros2_control_execution;
using namespace manyforge::core;

namespace {

Ros2ControlExecutionBackendConfig base_config() {
  Ros2ControlExecutionBackendConfig cfg;
  cfg.action_name = "/joint_trajectory_controller/follow_joint_trajectory";
  cfg.required_joints = {"j1", "j2", "j3"};
  return cfg;
}

ControllerBindingContract binding_contract() {
  ControllerBindingContract contract;
  contract.generation_id = "binding-sha256";
  contract.controller_manager = "/cell/controller_manager";
  contract.activity_topic = "/cell/controller_manager/activity";
  contract.hardware_component_name = "lite6_system";
  contract.trajectory_controller_manager_name = "lite6_traj_controller";
  contract.trajectory_controller_ros_name =
      "/cell/lite6_traj_controller";
  contract.trajectory_controller_type =
      "joint_trajectory_controller/JointTrajectoryController";
  contract.follow_joint_trajectory_action =
      "/cell/lite6_traj_controller/follow_joint_trajectory";
  contract.controller_state_topic =
      "/cell/lite6_traj_controller/controller_state";
  contract.joint_state_topic = "/cell/joint_states";
  contract.required_joints = {"joint1", "joint2"};
  contract.claimed_command_interfaces = {
      "joint1/position", "joint1/velocity",
      "joint2/position", "joint2/velocity"};
  contract.required_state_interfaces = {
      "joint1/position", "joint1/velocity",
      "joint2/position", "joint2/velocity"};
  contract.snapshot_max_age_ns = 1'000'000'000;
  return contract;
}

ControllerBindingSnapshot valid_binding_snapshot() {
  ControllerBindingSnapshot snapshot;
  snapshot.generation = 11;
  snapshot.activity_generation = 7;
  snapshot.contract_generation_id = "binding-sha256";
  snapshot.observed_timestamp_ns = 1'000;
  snapshot.received_timestamp_ns = 1'000;
  snapshot.fresh_until_ns = 2'000;
  snapshot.controller_active = true;
  snapshot.hardware_active = true;
  snapshot.controller_type_matches = true;
  snapshot.claimed_interfaces_match = true;
  snapshot.state_interfaces_available = true;
  snapshot.conflicting_claims_absent = true;
  snapshot.valid = true;
  return snapshot;
}

ExecutionAdmissionSnapshot valid_admission_snapshot() {
  ExecutionAdmissionSnapshot snapshot;
  snapshot.generation = 19;
  snapshot.lifecycle_generation = 5;
  snapshot.robot_id = "lite6-a";
  snapshot.binding_contract_generation_id = "binding-sha256";
  snapshot.runtime_generation_id = "runtime-1";
  snapshot.provider_component_epoch = "provider-epoch-1";
  snapshot.transport_owner_component_epoch = "owner-epoch-1";
  snapshot.authority_component_epoch = "authority-epoch-1";
  snapshot.cell_supervisor_component_epoch = "supervisor-epoch-1";
  snapshot.observed_timestamp_ns = 1'000;
  snapshot.received_timestamp_ns = 1'000;
  snapshot.fresh_until_ns = 2'000;
  snapshot.controller_ready = true;
  snapshot.motion_admitted = true;
  return snapshot;
}

}  // namespace

TEST(Ros2ControllerBindingTest,
     ExactFreshSnapshotsProduceOneGenerationPin) {
  const auto contract = binding_contract();
  const auto binding = valid_binding_snapshot();
  const auto admission = valid_admission_snapshot();
  const auto decision = evaluate_dispatch_admission(
      &contract, &binding, &admission, true, 1'500,
      contract.follow_joint_trajectory_action,
      contract.required_joints);

  ASSERT_TRUE(decision.admitted);
  ASSERT_TRUE(decision.pin.has_value());
  EXPECT_EQ(decision.pin->binding_contract_generation_id,
            contract.generation_id);
  EXPECT_EQ(decision.pin->binding_snapshot_generation,
            binding.generation);
  EXPECT_EQ(decision.pin->binding_activity_generation,
            binding.activity_generation);
  EXPECT_EQ(decision.pin->admission_snapshot_generation,
            admission.generation);
  EXPECT_EQ(decision.pin->lifecycle_generation,
            admission.lifecycle_generation);
  EXPECT_EQ(decision.pin->transport_owner_component_epoch,
            admission.transport_owner_component_epoch);
  EXPECT_EQ(decision.pin->authority_component_epoch,
            admission.authority_component_epoch);
}

TEST(Ros2ControllerBindingTest,
     DispatchMatrixFailsClosedWithStableReasons) {
  const auto contract = binding_contract();
  const auto action = contract.follow_joint_trajectory_action;
  const auto joints = contract.required_joints;

  auto binding = valid_binding_snapshot();
  auto admission = valid_admission_snapshot();
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, nullptr, &admission, true, 1'500,
                action, joints)
                .reason_code,
            "controller_binding_unavailable");

  binding.fresh_until_ns = 1'500;
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action, joints)
                .reason_code,
            "controller_binding_stale");
  binding = valid_binding_snapshot();
  binding.valid = false;
  binding.reason_codes = {"trajectory_controller_inactive"};
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action, joints)
                .reason_code,
            "controller_binding_invalid");
  binding = valid_binding_snapshot();
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action + "_wrong", joints)
                .reason_code,
            "controller_action_binding_mismatch");
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action, {"joint2", "joint1"})
                .reason_code,
            "controller_joint_order_mismatch");
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, nullptr, true, 1'500,
                action, joints)
                .reason_code,
            "execution_admission_unavailable");

  admission = valid_admission_snapshot();
  admission.fresh_until_ns = 1'500;
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action, joints)
                .reason_code,
            "execution_admission_stale");
  admission = valid_admission_snapshot();
  admission.binding_contract_generation_id = "other-binding";
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action, joints)
                .reason_code,
            "execution_admission_binding_mismatch");
  admission = valid_admission_snapshot();
  admission.controller_ready = false;
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action, joints)
                .reason_code,
            "execution_controller_not_ready");
  admission = valid_admission_snapshot();
  admission.motion_admitted = false;
  EXPECT_EQ(evaluate_dispatch_admission(
                &contract, &binding, &admission, true, 1'500,
                action, joints)
                .reason_code,
            "execution_motion_not_admitted");
}

TEST(Ros2ControllerBindingTest,
     RuntimeAllowsLeaseRefreshButRejectsAuthorityChurn) {
  const auto contract = binding_contract();
  auto binding = valid_binding_snapshot();
  auto admission = valid_admission_snapshot();
  const auto dispatch = evaluate_dispatch_admission(
      &contract, &binding, &admission, true, 1'500,
      contract.follow_joint_trajectory_action,
      contract.required_joints);
  ASSERT_TRUE(dispatch.pin.has_value());

  // Refresh publications may renew freshness without pretending a lifecycle
  // transition occurred.
  binding.generation += 1;
  binding.fresh_until_ns += 1'000;
  admission.generation += 1;
  admission.fresh_until_ns += 1'000;
  EXPECT_TRUE(evaluate_active_dispatch_authority(
                  &contract, &binding, &admission, true, 2'000,
                  *dispatch.pin)
                  .admitted);

  binding.activity_generation += 1;
  EXPECT_EQ(evaluate_active_dispatch_authority(
                &contract, &binding, &admission, true, 2'000,
                *dispatch.pin)
                .reason_code,
            "controller_binding_generation_changed");
  binding.activity_generation -= 1;
  admission.lifecycle_generation += 1;
  EXPECT_EQ(evaluate_active_dispatch_authority(
                &contract, &binding, &admission, true, 2'000,
                *dispatch.pin)
                .reason_code,
            "execution_admission_generation_changed");

  admission = valid_admission_snapshot();
  admission.lifecycle_generation += 1;
  admission.transport_owner_component_epoch = "owner-epoch-2";
  admission.fresh_until_ns += 1'000;
  EXPECT_EQ(evaluate_active_dispatch_authority(
                &contract, &binding, &admission, true, 2'000,
                *dispatch.pin)
                .reason_code,
            "execution_environment_epoch_changed");
}

TEST(Ros2CapabilityTest,
     SpringBackHoldIsAlwaysSupported) {
  // kSpringBackHold has no controller-config preconditions: it is a
  // backend-computed on-path replacement goal dispatched through stock JTC.
  // Capability profile id is the v1 baseline.
  auto cfg = base_config();
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSpringBackHold));
  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id, "ros2_control_v1");
  ASSERT_TRUE(cap.hard_stop_level.has_value());
  EXPECT_EQ(*cap.hard_stop_level, "jtc_cancel_unprofiled_hold");
  ASSERT_TRUE(cap.derivative_proof_level.has_value());
  EXPECT_EQ(*cap.derivative_proof_level, "none");
  ASSERT_TRUE(cap.scaling_source_visibility.has_value());
  EXPECT_EQ(*cap.scaling_source_visibility, "jtc_speed_scaling_input");
  ASSERT_TRUE(cap.scaling_source_probe_status.has_value());
  EXPECT_EQ(*cap.scaling_source_probe_status, "configured_not_live_probed");
  EXPECT_EQ(cap.unsupported_reasons.at("hard_deceleration"),
            "decelerate_on_cancel_evidence_incomplete");
}

TEST(Ros2ObservedStateSourcesTest,
     CommandedSimulationPrecedesMeasuredDiagnosticsExactly) {
  auto config = base_config();
  config.execution_state_source_mode = "commanded_sim";
  config.observed_state_source_precedence = {
      "controller_state", "joint_state", "action_feedback"};
  config.joint_state_max_age_s = 0.25;

  const auto sources = ros2_control_observed_state_sources(config);

  ASSERT_EQ(sources.size(), 4U);
  EXPECT_EQ(sources[0].kind, ObservedRobotStateSourceKind::kCommandedState);
  EXPECT_EQ(sources[0].source_id, "commanded_state");
  EXPECT_EQ(sources[0].authority_profile,
            ObservedRobotStateAuthorityProfile::kCommandedSimulation);
  EXPECT_EQ(sources[0].freshness_budget_ns, 0);
  EXPECT_EQ(sources[0].authority_profile_id,
            "ros2_control_commanded_sim_v1");
  for (std::size_t index = 1; index < sources.size(); ++index) {
    EXPECT_EQ(sources[index].authority_profile,
              ObservedRobotStateAuthorityProfile::kMeasured);
    EXPECT_EQ(sources[index].freshness_budget_ns, 250'000'000);
    EXPECT_EQ(sources[index].authority_profile_id,
              "ros2_control_measured_diagnostic_v1");
  }
  EXPECT_EQ(ros2_control_addressed_state_sources(config),
            std::vector<ObservedRobotStateSourceConfig>{sources.front()});
  const std::vector<ObservedRobotStateSourceConfig> expected_diagnostics(
      sources.begin() + 1, sources.end());
  EXPECT_EQ(ros2_control_diagnostic_state_sources(config),
            expected_diagnostics);
}

TEST(Ros2CapabilityTest,
     FaultErrorCarriesStableReasonTokenAndMeasurements) {
  auto error = make_ros2_control_fault_error(
      ErrorCode::kExecutionAborted,
      "Controller tracking error exceeded tolerance",
      ros2_control_fault_reason::kTrackingErrorExceeded,
      {{"joint_name", "shoulder_pan_joint"},
       {"tracking_error_rad", "0.125"},
       {"tracking_error_tolerance_rad", "0.05"},
       {"elapsed_s", "1.25"}});

  EXPECT_EQ(error.code, kErrorExecutionAborted);
  EXPECT_EQ(error.message, "Controller tracking error exceeded tolerance");
  EXPECT_EQ(error.details.at("backend"), "ros2_control");
  EXPECT_EQ(error.details.at("surface"), "ros2_control_execution_backend");
  EXPECT_EQ(error.details.at("reason_code"), "tracking_error_exceeded");
  EXPECT_EQ(error.details.at("joint_name"), "shoulder_pan_joint");
  EXPECT_EQ(error.details.at("tracking_error_rad"), "0.125");
  EXPECT_EQ(error.details.at("tracking_error_tolerance_rad"), "0.05");
  EXPECT_EQ(error.details.at("elapsed_s"), "1.25");
}

TEST(Ros2CapabilityTest,
     FaultReasonTokensCoverPhase6ATaxonomy) {
  struct Case {
    const char* reason_code;
    ErrorCode top_level_code;
    std::map<std::string, std::string> details;
    std::vector<std::string> required_keys;
  };

  const std::vector<Case> cases = {
      {ros2_control_fault_reason::kTrackingErrorExceeded,
       ErrorCode::kExecutionAborted,
       {{"joint_name", "shoulder_pan_joint"},
        {"peak_tracking_error_rad", "0.125"},
        {"tracking_error_tolerance_rad", "0.05"},
        {"trajectory_elapsed_s", "1.25"}},
       {"joint_name",
        "peak_tracking_error_rad",
        "tracking_error_tolerance_rad",
        "trajectory_elapsed_s"}},
      {ros2_control_fault_reason::kStartStateMismatch,
       ErrorCode::kStartStateMismatch,
       {{"joint_name", "elbow_joint"},
        {"max_joint_error_rad", "0.031"},
        {"start_tolerance_rad", "0.02"},
        {"expected_position_rad", "0.1"},
        {"observed_position_rad", "0.131"}},
       {"joint_name", "max_joint_error_rad", "start_tolerance_rad"}},
      {ros2_control_fault_reason::kActionServerUnavailable,
       ErrorCode::kExecutionRejected,
       {{"action_name", "/arm/follow_joint_trajectory"},
        {"wait_for_action_server_timeout_s", "2"}},
       {"action_name", "wait_for_action_server_timeout_s"}},
      {ros2_control_fault_reason::kEmptyTrajectory,
       ErrorCode::kInvalidRequest,
       {{"waypoint_count", "0"}, {"joint_count", "6"}},
       {"waypoint_count", "joint_count"}},
      {ros2_control_fault_reason::kStaleControllerState,
       ErrorCode::kExecutionRejected,
       {{"joint_state_topic", "/joint_states"},
        {"controller_state_topic", "/arm/state"},
        {"required_joint_count", "6"},
        {"wait_for_state_timeout_s", "1"}},
       {"joint_state_topic", "required_joint_count", "wait_for_state_timeout_s"}},
      {ros2_control_fault_reason::kControllerCancelTimeout,
       ErrorCode::kStopFailed,
       {{"stop_completion_timeout_s", "2"}},
       {"stop_completion_timeout_s"}},
      {ros2_control_fault_reason::kControllerFeedbackTimeout,
       ErrorCode::kExecutionRejected,
       {{"wait_for_state_timeout_s", "1"}},
       {"wait_for_state_timeout_s"}},
      {ros2_control_fault_reason::kControllerGoalDispatchFailed,
       ErrorCode::kExecutionRejected,
       {{"exception", "dispatch failed"}},
       {"exception"}},
      {ros2_control_fault_reason::kControllerGoalRejected,
       ErrorCode::kExecutionRejected,
       {},
       {}},
      {ros2_control_fault_reason::kControllerGoalResponseTimeout,
       ErrorCode::kExecutionRejected,
       {{"wait_for_action_server_timeout_s", "2"}},
       {"wait_for_action_server_timeout_s"}},
      {ros2_control_fault_reason::kControllerResultFailed,
       ErrorCode::kExecutionAborted,
       {{"action_result_code", "4"},
        {"controller_result_code", "6"},
        {"controller_error", "path tolerance violated"}},
       {"action_result_code", "controller_result_code", "controller_error"}},
      {ros2_control_fault_reason::kMonitorThreadStillShuttingDown,
       ErrorCode::kExecutionRejected,
       {},
       {}},
  };

  for (const auto& entry : cases) {
    const auto error = make_ros2_control_fault_error(
        entry.top_level_code, "backend fault", entry.reason_code,
        entry.details);
    EXPECT_EQ(error.details.at("backend"), "ros2_control");
    EXPECT_EQ(error.details.at("surface"),
              "ros2_control_execution_backend");
    EXPECT_EQ(error.details.at("reason_code"), entry.reason_code);
    EXPECT_EQ(error.code, std::string(to_string(entry.top_level_code)));
    for (const auto& key : entry.required_keys) {
      EXPECT_TRUE(error.details.contains(key))
          << "missing detail key '" << key << "' for "
          << entry.reason_code;
    }
  }
}

TEST(Ros2CapabilityTest,
     HardDecelerationOmittedWhenDecelerateOnCancelFlagFalse) {
  // First precondition: decelerate_on_cancel MUST be true. Even with
  // velocity interfaces and positive per-joint limits, omitting the
  // flag means the controller will not honor cancel with a
  // deceleration profile — the capability MUST stay empty.
  auto cfg = base_config();
  cfg.decelerate_on_cancel = false;
  cfg.velocity_state_interface_joints = {"j1", "j2", "j3"};
  cfg.max_deceleration_on_cancel_rad_s2 = {{"j1", 5.0}, {"j2", 5.0}, {"j3", 5.0}};
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
}

TEST(Ros2CapabilityTest,
     HardDecelerationOmittedWhenVelocityInterfaceMissingForSomeJoint) {
  // Second precondition: every required_joints entry MUST have a
  // velocity state interface. A missing interface on even one joint
  // disqualifies the deployment from advertising the mode.
  auto cfg = base_config();
  cfg.decelerate_on_cancel = true;
  cfg.velocity_state_interface_joints = {"j1", "j2"};  // j3 missing
  cfg.max_deceleration_on_cancel_rad_s2 = {{"j1", 5.0}, {"j2", 5.0}, {"j3", 5.0}};
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
}

TEST(Ros2CapabilityTest,
     HardDecelerationOmittedWhenPerJointLimitMissingOrNonPositive) {
  // Third precondition: max_deceleration_on_cancel_rad_s2 MUST have a
  // strictly positive entry for every required joint. Missing entries
  // and zero / negative values both disqualify the mode.
  auto cfg = base_config();
  cfg.decelerate_on_cancel = true;
  cfg.velocity_state_interface_joints = {"j1", "j2", "j3"};

  // Missing entry for j3.
  cfg.max_deceleration_on_cancel_rad_s2 = {{"j1", 5.0}, {"j2", 5.0}};
  auto cap_missing =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_FALSE(cap_missing.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));

  // Zero value for j3 (degenerates to immediate stop).
  cfg.max_deceleration_on_cancel_rad_s2 = {{"j1", 5.0}, {"j2", 5.0}, {"j3", 0.0}};
  auto cap_zero =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_FALSE(cap_zero.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));

  // Negative value for j2 (meaningless physically).
  cfg.max_deceleration_on_cancel_rad_s2 = {{"j1", 5.0}, {"j2", -3.0}, {"j3", 5.0}};
  auto cap_neg =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_FALSE(cap_neg.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
}

TEST(Ros2CapabilityTest,
     EmptyRequiredJointsReportsInvalidConfigWithEmptyCapability) {
  // Empty required_joints is a runtime-rejected misconfiguration.
  // The audit-facing helper MUST
  // report an empty capability set with a dedicated invalid-config
  // profile id so deployment tools can distinguish "deployment cannot
  // start" from "deployment can start but does not declare a mode."
  // Advertising kSpringBackHold here would mislead audit tools.
  Ros2ControlExecutionBackendConfig cfg;
  cfg.required_joints.clear();
  cfg.decelerate_on_cancel = true;
  cfg.velocity_state_interface_joints = {"j1"};
  cfg.max_deceleration_on_cancel_rad_s2 = {{"j1", 5.0}};
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.empty())
      << "invalid config MUST NOT advertise any stop mode";
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id, "ros2_control_v1_invalid_config");
}

TEST(Ros2CapabilityTest,
     HardDecelerationAdvertisedOnlyWithCompleteEvidence) {
  // Happy path: all three preconditions satisfied for every required
  // joint. Capability advertises both modes with the qualified
  // profile id so audit can distinguish hard-decel-qualified
  // deployments from baseline ones.
  auto cfg = base_config();
  cfg.decelerate_on_cancel = true;
  cfg.velocity_state_interface_joints = {"j1", "j2", "j3"};
  cfg.max_deceleration_on_cancel_rad_s2 = {
      {"j1", 5.0}, {"j2", 5.0}, {"j3", 5.0}};
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSpringBackHold));
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id, "ros2_control_v1_hard_decel");
  ASSERT_TRUE(cap.hard_stop_level.has_value());
  EXPECT_EQ(*cap.hard_stop_level, "hard_deceleration");
}

TEST(Ros2CapabilityTest,
     SoftOnPathOmittedByDefault) {
  // kSoftOnPath is production-disabled by default. A config that has
  // not opted in MUST NOT advertise the mode, so the
  // kernel arm-time gate rejects any scope requesting it.
  auto cfg = base_config();
  // Even with full hard_decel evidence, soft_on_path stays off.
  cfg.decelerate_on_cancel = true;
  cfg.velocity_state_interface_joints = {"j1", "j2", "j3"};
  cfg.max_deceleration_on_cancel_rad_s2 = {
      {"j1", 5.0}, {"j2", 5.0}, {"j3", 5.0}};
  ASSERT_FALSE(cfg.enable_soft_on_path_experimental);
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
}

TEST(Ros2CapabilityTest,
     SoftOnPathAdvertisedWhenExperimentalOptInSet) {
  // When the deployment opts in explicitly (after running the
  // qualification fixture on its controller), the mode appears
  // in the capability set with a profile id that includes the
  // experimental tag so audit tools can tell.
  auto cfg = base_config();
  cfg.enable_soft_on_path_experimental = true;
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
  // Soft_on_path + no hard_decel evidence: experimental tag only.
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id,
            "ros2_control_v1_soft_on_path_experimental");
}

TEST(Ros2CapabilityTest,
     SoftOnPathPathRampAdvertisedEvenWithJtcSpeedScalingInputEnabled) {
  // soft_on_path is NOT gated on JTC speed-scaling: when speed-scaling is
  // active, the backend publishes unit execution scale immediately before the
  // preempting stop dispatch. With path-ramp enabled, speed-scaling enabled,
  // AND a valid soft_on_path config, kSoftOnPath is advertised; live seam
  // qualification covers the non-atomic DDS/controller ordering.
  auto cfg = base_config();
  cfg.enable_soft_on_path_path_ramp = true;
  ASSERT_TRUE(cfg.enable_jtc_speed_scaling_input);
  cfg.soft_on_path_stop_traj_delta_s = 0.04;
  cfg.soft_on_path_sample_period_s = 0.005;
  cfg.soft_on_path_max_jerk_rad_s3 = {{"j1", 20.0}, {"j2", 21.0}};
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id,
            "ros2_control_v1_soft_on_path_path_ramp");
  ASSERT_TRUE(cap.derivative_proof_level.has_value());
  EXPECT_EQ(*cap.derivative_proof_level,
            "manyforge_core_sampled_path_ramp");
}

TEST(Ros2CapabilityTest,
     SoftOnPathPathRampAdvertisedOnlyWithScaleIsolation) {
  auto cfg = base_config();
  cfg.enable_soft_on_path_path_ramp = true;
  cfg.enable_jtc_speed_scaling_input = false;
  cfg.soft_on_path_stop_traj_delta_s = 0.04;
  cfg.soft_on_path_sample_period_s = 0.005;
  cfg.soft_on_path_max_initial_path_deviation_rad = 0.02;
  cfg.soft_on_path_max_initial_velocity_deviation_rad_s = 0.4;
  cfg.soft_on_path_max_jerk_rad_s3 = {{"j1", 20.0}, {"j2", 21.0}};
  cfg.allow_soft_on_path_model_velocity_fallback = true;
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id,
            "ros2_control_v1_soft_on_path_path_ramp");
  ASSERT_TRUE(cap.soft_on_path_planner_config.has_value());
  EXPECT_DOUBLE_EQ(cap.soft_on_path_planner_config->stop_traj_delta_s, 0.04);
  EXPECT_DOUBLE_EQ(cap.soft_on_path_planner_config->sample_period_s, 0.005);
  ASSERT_TRUE(cap.soft_on_path_planner_config
                  ->max_initial_path_deviation_rad.has_value());
  EXPECT_DOUBLE_EQ(
      *cap.soft_on_path_planner_config->max_initial_path_deviation_rad,
      0.02);
  ASSERT_TRUE(cap.soft_on_path_planner_config
                  ->max_initial_velocity_deviation_rad_s.has_value());
  EXPECT_DOUBLE_EQ(*cap.soft_on_path_planner_config
                        ->max_initial_velocity_deviation_rad_s,
                   0.4);
  EXPECT_EQ(cap.soft_on_path_planner_config->per_joint_max_jerk_rad_s3.at("j1"),
            20.0);
  EXPECT_TRUE(cap.soft_on_path_planner_config->allow_model_velocity_fallback);
}

TEST(Ros2CapabilityTest,
     SoftOnPathPathRampOmittedWhenPlannerConfigInvalid) {
  auto cfg = base_config();
  cfg.enable_soft_on_path_path_ramp = true;
  cfg.enable_jtc_speed_scaling_input = false;
  cfg.soft_on_path_max_jerk_rad_s3 = {{"j1", 0.0}};

  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);

  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
  EXPECT_FALSE(cap.soft_on_path_planner_config.has_value());
}

TEST(Ros2CapabilityTest,
     SoftOnPathAndHardDecelerationCombinedProfileId) {
  // Fully qualified deployment (hard_deceleration evidence +
  // soft_on_path opt-in): both modes advertised, profile id
  // reflects the combination so qualification audit can identify it.
  auto cfg = base_config();
  cfg.decelerate_on_cancel = true;
  cfg.velocity_state_interface_joints = {"j1", "j2", "j3"};
  cfg.max_deceleration_on_cancel_rad_s2 = {
      {"j1", 5.0}, {"j2", 5.0}, {"j3", 5.0}};
  cfg.enable_soft_on_path_path_ramp = true;
  cfg.enable_jtc_speed_scaling_input = false;
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSpringBackHold));
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id,
            "ros2_control_v1_hard_decel_soft_on_path_path_ramp");
}

TEST(Ros2CapabilityTest,
     SoftOnPathStillOmittedOnInvalidConfig) {
  // Defense in depth: even with the opt-in set, invalid_config wins
  // (empty required_joints means the backend cannot start at all).
  Ros2ControlExecutionBackendConfig cfg;
  cfg.required_joints.clear();
  cfg.enable_soft_on_path_experimental = true;
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.empty());
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id, "ros2_control_v1_invalid_config");
}

// resolve_soft_on_path_anchor_velocity_source — the per-lane anchor-state
// authority. The commanded-sim lane's reported feedback velocity is decoupled
// from the commanded motion, so the seam anchor must take the model-derived
// velocity even when measured feedback is present (anchoring on the decoupled
// feedback sign-flips the seam). A lane with faithful feedback keeps measured;
// an explicit config forces either choice. These pin the decision the seam
// dispatch depends on.
TEST(SoftOnPathAnchorVelocityAuthority, CommandedSimPrefersModelEvenWithFeedback) {
  auto cfg = base_config();
  cfg.execution_state_source_mode = "commanded_sim";
  EXPECT_EQ(
      resolve_soft_on_path_anchor_velocity_source(cfg, /*has_measured=*/true),
      SoftOnPathStateSource::kModelDerived);
}

TEST(SoftOnPathAnchorVelocityAuthority, CommandedSimModelWhenNoFeedback) {
  auto cfg = base_config();
  cfg.execution_state_source_mode = "commanded_sim";
  EXPECT_EQ(
      resolve_soft_on_path_anchor_velocity_source(cfg, /*has_measured=*/false),
      SoftOnPathStateSource::kModelDerived);
}

TEST(SoftOnPathAnchorVelocityAuthority, NonSimUsesMeasuredWhenPresent) {
  auto cfg = base_config();
  cfg.execution_state_source_mode = "observed_ros2";
  EXPECT_EQ(
      resolve_soft_on_path_anchor_velocity_source(cfg, /*has_measured=*/true),
      SoftOnPathStateSource::kMeasuredFeedback);
}

TEST(SoftOnPathAnchorVelocityAuthority, NonSimModelFallbackWhenNoFeedback) {
  auto cfg = base_config();
  cfg.execution_state_source_mode = "observed_ros2";
  EXPECT_EQ(
      resolve_soft_on_path_anchor_velocity_source(cfg, /*has_measured=*/false),
      SoftOnPathStateSource::kModelDerived);
}

TEST(SoftOnPathAnchorVelocityAuthority, ExplicitPreferModelOverridesNonSim) {
  auto cfg = base_config();
  cfg.execution_state_source_mode = "observed_ros2";
  cfg.prefer_model_anchor_velocity = true;
  EXPECT_EQ(
      resolve_soft_on_path_anchor_velocity_source(cfg, /*has_measured=*/true),
      SoftOnPathStateSource::kModelDerived);
}

TEST(SoftOnPathAnchorVelocityAuthority, ExplicitMeasuredOverridesCommandedSim) {
  auto cfg = base_config();
  cfg.execution_state_source_mode = "commanded_sim";
  cfg.prefer_model_anchor_velocity = false;
  EXPECT_EQ(
      resolve_soft_on_path_anchor_velocity_source(cfg, /*has_measured=*/true),
      SoftOnPathStateSource::kMeasuredFeedback);
}
