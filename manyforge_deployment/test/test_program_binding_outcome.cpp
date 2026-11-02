#include <gtest/gtest.h>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>

#include <string>
#include <vector>

namespace manyforge::deployment {
namespace {

namespace core = manyforge::core;

constexpr std::string_view kDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

[[nodiscard]] ProgramBindingUnresolvedRoleDTO ComponentRow() {
  return {core::ComponentRoleId("gripper"), std::nullopt,
          core::ControllableComponentId("gripper.1"), std::nullopt,
          ProgramBindingUnresolvedReason::kComponentMissing,
          "component role selects a missing or ambiguous retained component"};
}

[[nodiscard]] ProgramBindingUnresolvedRoleDTO TargetRow() {
  return {core::ComponentRoleId("gripper"), core::TargetRoleId("jaw"),
          core::ControllableComponentId("gripper.1"),
          core::CommandTargetId("jaw"),
          ProgramBindingUnresolvedReason::kComponentRoleUnresolved,
          "target role depends on an unresolved component role"};
}

[[nodiscard]] ProgramNonExecutableOutcomeDTO Outcome() {
  return {core::ProgramId("pick_and_place"),
          core::ProgramRevision(std::string(kDigest)),
          core::RetainedBundleId(std::string(kDigest)),
          core::GraphRevision(std::string(kDigest)),
          core::ContentDigest(std::string(kDigest)),
          {ComponentRow(), TargetRow()},
          {core::ProgramNodeInstanceId("close_gripper")},
          {"grasp"}};
}

TEST(ProgramBindingOutcome, RoundTripsCanonicallyWithAStableDigest) {
  const auto outcome = Outcome();
  ASSERT_TRUE(outcome.valid());
  const auto encoded = EncodeProgramNonExecutableOutcome(outcome);
  EXPECT_EQ(DecodeProgramNonExecutableOutcome(encoded), outcome);
  EXPECT_EQ(DigestProgramNonExecutableOutcome(outcome),
            DigestProgramNonExecutableOutcome(
                DecodeProgramNonExecutableOutcome(encoded)));
  EXPECT_THROW(static_cast<void>(DecodeProgramNonExecutableOutcome(encoded + " ")),
               ProgramBindingOutcomeError);
  auto unknown = encoded;
  const auto position = unknown.find("component_missing");
  ASSERT_NE(position, std::string::npos);
  unknown.replace(position, 17U, "component_absent_");
  EXPECT_THROW(static_cast<void>(DecodeProgramNonExecutableOutcome(unknown)),
               ProgramBindingOutcomeError);
}

TEST(ProgramBindingOutcome, StatesOnlyExactNamedReasons) {
  auto empty = Outcome();
  empty.unresolved_roles.clear();
  EXPECT_FALSE(empty.valid());

  auto unsorted = Outcome();
  std::swap(unsorted.unresolved_roles[0], unsorted.unresolved_roles[1]);
  EXPECT_FALSE(unsorted.valid());

  auto all_group = Outcome();
  all_group.affected_execution_groups = {"all"};
  EXPECT_FALSE(all_group.valid());

  // A target reason needs a target row and vice versa.
  auto target_reason_on_component = ComponentRow();
  target_reason_on_component.reason = ProgramBindingUnresolvedReason::kTargetMissing;
  EXPECT_FALSE(target_reason_on_component.valid());
  auto component_reason_on_target = TargetRow();
  component_reason_on_target.reason =
      ProgramBindingUnresolvedReason::kComponentMissing;
  EXPECT_FALSE(component_reason_on_target.valid());

  auto long_detail = TargetRow();
  long_detail.detail.assign(kMaximumProgramBindingUnresolvedDetailBytes + 1U, 'x');
  EXPECT_FALSE(long_detail.valid());
}

TEST(ProgramBindingOutcome, AffectedScopeNamesNodesAndAuthoredGroupsOnly) {
  core::ProgramExecutionTableDTO table;
  table.nodes = {
      {core::ProgramNodeInstanceId("close_gripper"), "gripper.command",
       {{core::TargetRoleId("jaw"), core::CapabilityId("tool.gripper.command"), true}}},
      {core::ProgramNodeInstanceId("move_arm"), "motion.move",
       {{core::TargetRoleId("arm"), core::CapabilityId("execution.trajectory.execute"),
         true}}},
      {core::ProgramNodeInstanceId("wait"), "logic.wait", {}},
  };
  table.named_selections = {
      {"arm_only", {1U}, core::ProgramExecutionPolicy::kIndependent},
      {"grasp", {0U, 1U}, core::ProgramExecutionPolicy::kIndependent},
  };

  const auto scope =
      program_binding_affected_scope(table, {core::TargetRoleId("jaw")});
  EXPECT_EQ(scope.nodes,
            (std::vector{core::ProgramNodeInstanceId("close_gripper")}));
  EXPECT_EQ(scope.execution_groups, (std::vector<std::string>{"grasp"}));
  EXPECT_TRUE(program_binding_affected_scope(table, {}).nodes.empty());
}

TEST(ProgramBindingOutcome, RoleAssignmentParsesExactlyAndRejectsDuplicates) {
  const auto parse = [](std::string_view json) {
    return parse_program_role_assignment(parse_strict_json_model_yaml(json).root());
  };
  const auto assignment = parse(
      R"({"component_roles":[{"component_role_id":"robot","controllable_component_id":"lite6"}],)"
      R"("target_roles":[{"command_target_id":"arm","target_role_id":"arm"}]})");
  ASSERT_EQ(assignment.component_roles.size(), 1U);
  EXPECT_EQ(assignment.component_roles.front().controllable_component_id,
            core::ControllableComponentId("lite6"));
  ASSERT_EQ(assignment.target_roles.size(), 1U);
  EXPECT_EQ(assignment.target_roles.front().command_target_id,
            core::CommandTargetId("arm"));

  EXPECT_THROW(
      static_cast<void>(parse(
          R"({"component_roles":[{"component_role_id":"robot","controllable_component_id":"a"},)"
          R"({"component_role_id":"robot","controllable_component_id":"b"}],"target_roles":[]})")),
      ProgramBindingOutcomeError);
  EXPECT_THROW(static_cast<void>(parse(
                   R"({"component_roles":[],"extra":[],"target_roles":[]})")),
               ProgramBindingOutcomeError);
}

} // namespace
} // namespace manyforge::deployment
