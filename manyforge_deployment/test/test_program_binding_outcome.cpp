#include <gtest/gtest.h>

#include <manyforge_deployment/program_binding_outcome.hpp>

#include <string>
#include <vector>

namespace manyforge::deployment {
namespace {
namespace core = manyforge::core;
constexpr std::string_view kDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

ProgramBindingUnresolvedTargetDTO
Target(std::string component, std::string target,
       ProgramBindingUnresolvedReason reason) {
  return {{core::ControllableComponentId(std::move(component)),
           core::CommandTargetId(std::move(target))},
          reason,
          "candidate target unavailable"};
}
ProgramNonExecutableOutcomeDTO Outcome() {
  return {core::ProgramId("pick_and_place"),
          core::ProgramRevision(std::string(kDigest)),
          core::RetainedBundleId(std::string(kDigest)),
          core::GraphRevision(std::string(kDigest)),
          {Target("gripper", "jaw",
                  ProgramBindingUnresolvedReason::kComponentMissing),
           Target("robot", "arm",
                  ProgramBindingUnresolvedReason::kCapabilityUnavailable)},
          {core::ProgramNodeInstanceId("close_gripper")},
          {"grasp"}};
}

TEST(ProgramBindingOutcome, RoundTripsExactDirectTargetsAndDigest) {
  const auto outcome = Outcome();
  ASSERT_TRUE(outcome.valid());
  const auto encoded = EncodeProgramNonExecutableOutcome(outcome);
  EXPECT_EQ(DecodeProgramNonExecutableOutcome(encoded), outcome);
  EXPECT_EQ(DigestProgramNonExecutableOutcome(outcome),
            DigestProgramNonExecutableOutcome(
                DecodeProgramNonExecutableOutcome(encoded)));
  EXPECT_EQ(encoded.find("role_assignment"), std::string::npos);
  EXPECT_THROW(
      static_cast<void>(DecodeProgramNonExecutableOutcome(encoded + " ")),
      ProgramBindingOutcomeError);
  auto unknown = encoded;
  const auto position = unknown.find("component_missing");
  ASSERT_NE(position, std::string::npos);
  unknown.replace(position, 17U, "component_absent_");
  EXPECT_THROW(static_cast<void>(DecodeProgramNonExecutableOutcome(unknown)),
               ProgramBindingOutcomeError);
}

TEST(ProgramBindingOutcome, RequiresUniqueSortedTargetsAndBoundedDetails) {
  auto empty = Outcome();
  empty.unresolved_targets.clear();
  EXPECT_FALSE(empty.valid());
  auto unsorted = Outcome();
  std::swap(unsorted.unresolved_targets[0], unsorted.unresolved_targets[1]);
  EXPECT_FALSE(unsorted.valid());
  auto duplicate = Outcome();
  duplicate.unresolved_targets[1].target =
      duplicate.unresolved_targets[0].target;
  EXPECT_FALSE(duplicate.valid());
  auto all = Outcome();
  all.affected_execution_groups = {"all"};
  EXPECT_FALSE(all.valid());
  auto long_detail =
      Target("robot", "arm", ProgramBindingUnresolvedReason::kTargetMissing);
  long_detail.detail.assign(kMaximumProgramBindingUnresolvedDetailBytes + 1U,
                            'x');
  EXPECT_FALSE(long_detail.valid());
}

TEST(ProgramBindingOutcome, AffectedScopeUsesExactComponentAndTarget) {
  core::ProgramExecutionTableDTO table;
  table.nodes = {
      {core::ProgramNodeInstanceId("close_gripper"),
       "gripper.command",
       {{{core::ControllableComponentId("gripper"),
          core::CommandTargetId("jaw")},
         core::CapabilityId("tool.gripper.command"),
         true}}},
      {core::ProgramNodeInstanceId("move_arm"),
       "motion.move",
       {{{core::ControllableComponentId("robot"), core::CommandTargetId("arm")},
         core::CapabilityId("execution.trajectory.execute"),
         true}}},
      {core::ProgramNodeInstanceId("wait"), "logic.wait", {}}};
  table.named_selections = {
      {"arm_only", {1U}, core::ProgramExecutionPolicy::kIndependent},
      {"grasp", {0U, 1U}, core::ProgramExecutionPolicy::kIndependent}};
  const auto scope = program_binding_affected_scope(
      table, {{core::ControllableComponentId("gripper"),
               core::CommandTargetId("jaw")}});
  EXPECT_EQ(scope.nodes,
            (std::vector{core::ProgramNodeInstanceId("close_gripper")}));
  EXPECT_EQ(scope.execution_groups, (std::vector<std::string>{"grasp"}));
  EXPECT_TRUE(program_binding_affected_scope(
                  table, {{core::ControllableComponentId("other"),
                           core::CommandTargetId("jaw")}})
                  .nodes.empty());
}
} // namespace
} // namespace manyforge::deployment
