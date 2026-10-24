#include <gtest/gtest.h>

#include <manyforge_core/io_rule_stop_response.hpp>

namespace manyforge::core {
namespace {
std::vector<IoRuleStopEntryRowDTO> qualified_rows() {
  return {{StopEscalationStageId("controlled"), StopEscalationStageKind::kNominal,
           StopContainmentCommandKind::kMotionControlledStop, StopActionClass::kControlled},
          {StopEscalationStageId("cancel"), StopEscalationStageKind::kControlledFallback,
           StopContainmentCommandKind::kControllerGoalCancel, StopActionClass::kControlled},
          {StopEscalationStageId("floor"), StopEscalationStageKind::kSoftwareHard,
           StopContainmentCommandKind::kRegistered, StopActionClass::kSoftwareHard}};
}

TEST(IoRuleStopResponse, DeclarationSelectionKeepsOriginalIndexAndMandatorySuffix) {
  const auto rows = qualified_rows();
  EXPECT_EQ(ResolveIoRuleStopEntryIndex(rows, {}).value(), 0U);
  EXPECT_EQ(ResolveIoRuleStopEntryIndex(rows, StopEscalationStageId("cancel")).value(), 1U);
  EXPECT_EQ(ResolveIoRuleStopEntryIndex(rows, StopEscalationStageId("floor")).value(), 2U);
  EXPECT_FALSE(ResolveIoRuleStopEntryIndex(rows, StopEscalationStageId("foreign")).ok());
  auto missing = rows;
  missing.pop_back();
  EXPECT_FALSE(ResolveIoRuleStopEntryIndex(missing, {}).ok());
  auto beyond_floor = rows;
  beyond_floor.push_back(
      {StopEscalationStageId("after"), StopEscalationStageKind::kControlledFallback,
       StopContainmentCommandKind::kControllerGoalCancel, StopActionClass::kControlled});
  EXPECT_FALSE(ResolveIoRuleStopEntryIndex(beyond_floor, StopEscalationStageId("after")).ok());
}

TEST(IoRuleStopResponse, DeclarationSelectionRejectsDuplicateAndUnqualifiedMetadata) {
  auto rows = qualified_rows();
  rows[1].stop_escalation_stage_id = rows[0].stop_escalation_stage_id;
  EXPECT_FALSE(ResolveIoRuleStopEntryIndex(rows, {}).ok());
  rows = qualified_rows();
  rows[2].stop_action_class = StopActionClass::kControlled;
  EXPECT_FALSE(ResolveIoRuleStopEntryIndex(rows, StopEscalationStageId("floor")).ok());
  rows = qualified_rows();
  rows[0].command_kind = StopContainmentCommandKind::kRegistered;
  EXPECT_FALSE(ResolveIoRuleStopEntryIndex(rows, {}).ok());
}

TEST(IoRuleStopResponse, SuccessThresholdIsQualifiedBetweenEntryAndSoftwareFloor) {
  const auto rows = qualified_rows();
  EXPECT_EQ(ResolveIoRuleSuccessThroughIndex(rows, {}, {}).value(), 0U);
  EXPECT_EQ(ResolveIoRuleSuccessThroughIndex(rows, StopEscalationStageId("cancel"), {}).value(),
            1U);
  EXPECT_EQ(ResolveIoRuleSuccessThroughIndex(rows, {}, StopEscalationStageId("cancel")).value(),
            1U);
  EXPECT_EQ(ResolveIoRuleSuccessThroughIndex(rows, {}, StopEscalationStageId("floor")).value(), 2U);
  EXPECT_FALSE(ResolveIoRuleSuccessThroughIndex(rows, StopEscalationStageId("cancel"),
                                                StopEscalationStageId("controlled"))
                   .ok());
  EXPECT_FALSE(ResolveIoRuleSuccessThroughIndex(rows, {}, StopEscalationStageId("foreign")).ok());
  auto beyond_floor = rows;
  beyond_floor.push_back(
      {StopEscalationStageId("later"), StopEscalationStageKind::kControlledFallback,
       StopContainmentCommandKind::kControllerGoalCancel, StopActionClass::kControlled});
  // A later floor cannot make an earlier mandatory floor optional.
  beyond_floor.push_back({StopEscalationStageId("last.floor"),
                          StopEscalationStageKind::kSoftwareHard,
                          StopContainmentCommandKind::kRegistered, StopActionClass::kSoftwareHard});
  EXPECT_FALSE(
      ResolveIoRuleSuccessThroughIndex(beyond_floor, {}, StopEscalationStageId("later")).ok());
}

TEST(IoRuleStopResponse, SelectedModeKeepsSettingsAndOnlyConfiguredLowerFallbacks) {
  MotionInterruptStopProfileDTO defaults;
  defaults.profile_id = "deployment.arm.stop";
  defaults.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  defaults.deceleration_time_s = 0.75;
  defaults.max_stop_time_s = 1.25;
  defaults.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}},
                                         {"joint_2", AngularAccelerationDTO{3.0}}};
  defaults.allow_natural_completion_on_short_path = true;
  defaults.fallback_stop_modes = {MotionInterruptStopMode::kSpringBackHold,
                                  MotionInterruptStopMode::kSpringBackFast};
  defaults.require_backend_confirmation = true;
  defaults.per_joint_settle_velocity_threshold = std::map<std::string, CoordinateVelocityDTO>{
      {"joint_1", AngularVelocityDTO{0.02}}, {"joint_2", AngularVelocityDTO{0.03}}};
  defaults.diagnostics.emplace("qualified", interrupt_structured_bool(true));
  defaults.jerk_limits_enabled = true;
  defaults.per_joint_max_jerk = {{"joint_1", AngularJerkDTO{8.0}},
                                 {"joint_2", AngularJerkDTO{9.0}}};

  const auto hold = SelectIoRuleStopProfile(defaults, MotionInterruptStopMode::kSpringBackHold);
  ASSERT_TRUE(hold.ok()) << hold.error().message;
  EXPECT_EQ(hold.value().stop_mode, MotionInterruptStopMode::kSpringBackHold);
  EXPECT_EQ(hold.value().fallback_stop_modes,
            (std::vector{MotionInterruptStopMode::kSpringBackFast}));
  EXPECT_EQ(hold.value().profile_id, defaults.profile_id);
  EXPECT_EQ(hold.value().deceleration_time_s, defaults.deceleration_time_s);
  EXPECT_EQ(hold.value().max_stop_time_s, defaults.max_stop_time_s);
  EXPECT_EQ(hold.value().per_joint_max_deceleration, defaults.per_joint_max_deceleration);
  EXPECT_EQ(hold.value().per_joint_settle_velocity_threshold,
            defaults.per_joint_settle_velocity_threshold);
  EXPECT_EQ(hold.value().jerk_limits_enabled, defaults.jerk_limits_enabled);
  EXPECT_EQ(hold.value().per_joint_max_jerk, defaults.per_joint_max_jerk);
  EXPECT_EQ(hold.value().diagnostics, defaults.diagnostics);

  const auto fast = SelectIoRuleStopProfile(defaults, MotionInterruptStopMode::kSpringBackFast);
  ASSERT_TRUE(fast.ok()) << fast.error().message;
  EXPECT_EQ(fast.value().stop_mode, MotionInterruptStopMode::kSpringBackFast);
  EXPECT_TRUE(fast.value().fallback_stop_modes.empty());
}

TEST(IoRuleStopResponse, SelectedHardUsesRetainedSettleLimitsAndClearsControlledSettings) {
  MotionInterruptStopProfileDTO defaults;
  defaults.profile_id = "deployment.arm.stop";
  defaults.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  defaults.deceleration_time_s = 0.75;
  defaults.max_stop_time_s = 1.25;
  defaults.per_joint_max_deceleration = {{"joint_1", AngularAccelerationDTO{2.0}}};
  defaults.allow_natural_completion_on_short_path = true;
  defaults.fallback_stop_modes = {MotionInterruptStopMode::kSpringBackHold};
  defaults.per_joint_settle_velocity_threshold =
      std::map<std::string, CoordinateVelocityDTO>{{"joint_1", AngularVelocityDTO{0.02}}};
  defaults.jerk_limits_enabled = true;
  defaults.per_joint_max_jerk = {{"joint_1", AngularJerkDTO{8.0}}};

  const auto selected =
      SelectIoRuleStopProfile(defaults, MotionInterruptStopMode::kHardDeceleration);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_EQ(selected.value().stop_mode, MotionInterruptStopMode::kHardDeceleration);
  EXPECT_FALSE(selected.value().deceleration_time_s);
  EXPECT_FALSE(selected.value().max_stop_time_s);
  EXPECT_TRUE(selected.value().per_joint_max_deceleration.empty());
  EXPECT_FALSE(selected.value().allow_natural_completion_on_short_path);
  EXPECT_TRUE(selected.value().fallback_stop_modes.empty());
  EXPECT_EQ(selected.value().jerk_limits_enabled, false);
  EXPECT_TRUE(selected.value().per_joint_max_jerk.empty());
  EXPECT_EQ(selected.value().per_joint_settle_velocity_threshold,
            defaults.per_joint_settle_velocity_threshold);
}

TEST(IoRuleStopResponse, SelectedControlledModeNeedsControlledDeploymentParameters) {
  MotionInterruptStopProfileDTO hard;
  hard.profile_id = "deployment.arm.hard";
  hard.stop_mode = MotionInterruptStopMode::kHardDeceleration;
  hard.jerk_limits_enabled = false;
  hard.per_joint_settle_velocity_threshold =
      std::map<std::string, CoordinateVelocityDTO>{{"joint_1", AngularVelocityDTO{0.02}}};
  const auto unavailable = SelectIoRuleStopProfile(hard, MotionInterruptStopMode::kSoftOnPath);
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"), "io_rule_stop_mode_parameters_unavailable");

  const auto invalid = SelectIoRuleStopProfile(hard, static_cast<MotionInterruptStopMode>(255));
  ASSERT_FALSE(invalid.ok());
  EXPECT_EQ(invalid.error().details.at("reason"), "io_rule_stop_mode_invalid");
}
}  // namespace
}  // namespace manyforge::core
