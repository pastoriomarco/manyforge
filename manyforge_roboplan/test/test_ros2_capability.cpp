#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include <manyforge_roboplan/ros2_control_capability.hpp>

// spec/260 §5.6: the ros2_control adapter's
// motion-interrupt stop capability is computed by a pure helper that
// inspects the deployment's Ros2ControlExecutionBackendConfig. The
// helper is callable without ROS so deployments can audit which stop
// modes a configuration will advertise BEFORE running the controller.
// These tests pin the evidence-gating rules so the capability gate
// stays honest as the config evolves.

using namespace manyforge::roboplan_backend;
using namespace manyforge::core;

namespace {

Ros2ControlExecutionBackendConfig base_config() {
  Ros2ControlExecutionBackendConfig cfg;
  cfg.action_name = "/joint_trajectory_controller/follow_joint_trajectory";
  cfg.required_joints = {"j1", "j2", "j3"};
  return cfg;
}

}  // namespace

TEST(Ros2CapabilityTest,
     SpringBackHoldIsAlwaysSupported) {
  // kSpringBackHold has no controller-config preconditions — the
  // existing JTC preempt + anchored-hold pattern is universally
  // available. Capability profile id is the v1 baseline.
  auto cfg = base_config();
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_TRUE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSpringBackHold));
  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kHardDeceleration));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id, "ros2_control_v1");
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
     SoftOnPathPathRampOmittedWhenJtcSpeedScalingInputEnabled) {
  // Real path-ramp dispatch cannot be advertised while the adapter
  // also publishes JTC speed_scaling_input, because that
  // controller-side scale can stretch the replacement stop trajectory.
  auto cfg = base_config();
  cfg.enable_soft_on_path_path_ramp = true;
  ASSERT_TRUE(cfg.enable_jtc_speed_scaling_input);
  auto cap =
      compute_ros2_control_motion_interrupt_stop_capability(cfg);
  EXPECT_FALSE(cap.supported_stop_modes.contains(
      MotionInterruptStopMode::kSoftOnPath));
  ASSERT_TRUE(cap.capability_profile_id.has_value());
  EXPECT_EQ(*cap.capability_profile_id, "ros2_control_v1");
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
