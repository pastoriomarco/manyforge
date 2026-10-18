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

TEST(IoRuleStopResponse, ActualNominalDefaultsCheckModeWithoutRewritingSubstitution) {
  JsonObjectDTO defaults{{"stop_mode", JsonValueDTO{std::string("soft_on_path")}}};
  MotionInterruptStopProfileDTO profile;
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.recoverable_substitution_mode = MotionInterruptStopMode::kSpringBackHold;
  EXPECT_TRUE(ValidateIoRuleNominalStopMode(defaults, profile).ok());
  EXPECT_EQ(profile.recoverable_substitution_mode, MotionInterruptStopMode::kSpringBackHold);
  profile.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  EXPECT_FALSE(ValidateIoRuleNominalStopMode(defaults, profile).ok());
  EXPECT_FALSE(ValidateIoRuleNominalStopMode(JsonObjectDTO{}, profile).ok());
}
}  // namespace
}  // namespace manyforge::core
