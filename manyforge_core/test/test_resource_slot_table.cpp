#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include <manyforge_core/resource_slot_table.hpp>

namespace {

using namespace manyforge::core;

[[nodiscard]] ResourceSlotOwnerDTO owner(std::string operation = "op.1",
                                         std::string participant = "part.1") {
  return ResourceSlotOwnerDTO{
      OperationId(std::move(operation)),
      OperationRevision(1U),
      OperationParticipantId(std::move(participant)),
      ControlDomainId("domain.arm"),
      ControlDomainRevision("domain-revision.arm"),
      ControlBindingId("binding.arm"),
      ControlBindingRevision("binding-revision.arm"),
      ControllerTopologyId("topology.native"),
      TopologyRevision("topology-revision.native"),
      ControlRouteId("route.native"),
      ActiveControlConfigurationId("active.native"),
      ActiveControlConfigurationGeneration(1U),
      ContainmentGranularity::kResource};
}

[[nodiscard]] std::vector<ResourceId> arm_resources() {
  return {ResourceId("joint.1"), ResourceId("joint.2")};
}

TEST(ResourceSlotTableTest, RunsOneExactScopeThroughTerminalAndRelease) {
  ResourceSlotTable table(
      {ResourceId("joint.2"), ResourceId("gripper.1"),
       ResourceId("joint.1")});
  const auto operation = owner();
  const auto resources = arm_resources();

  ASSERT_TRUE(table.reserve(operation, resources).ok());
  ASSERT_EQ(table.active_operation_id(), operation.operation_id);
  ASSERT_TRUE(table
                  .transition(operation, resources,
                              ResourceSlotState::kDispatched)
                  .ok());
  ASSERT_TRUE(table
                  .transition(operation, resources,
                              ResourceSlotState::kStopping)
                  .ok());
  ASSERT_TRUE(table
                  .transition(operation, resources,
                              ResourceSlotState::kTerminal)
                  .ok());
  ASSERT_TRUE(table.release(operation, resources).ok());
  EXPECT_FALSE(table.active_operation_id().has_value());

  const auto snapshot = table.snapshot();
  EXPECT_EQ(snapshot.size(), 3U);
  for (const auto& slot : snapshot) {
    EXPECT_EQ(slot.state, ResourceSlotState::kFree);
    EXPECT_FALSE(slot.owner.has_value());
  }
}

TEST(ResourceSlotTableTest, RejectsOverlapWithoutPartialMutation) {
  ResourceSlotTable table(
      {ResourceId("joint.1"), ResourceId("joint.2"),
       ResourceId("gripper.1")});
  const auto first = owner();
  ASSERT_TRUE(table.reserve(first, arm_resources()).ok());

  const auto second = owner("op.2", "part.2");
  const auto rejected = table.reserve(
      second, {ResourceId("joint.2"), ResourceId("gripper.1")});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "i6_single_operation_busy");

  const auto snapshot = table.snapshot();
  const auto gripper = std::find_if(
      snapshot.begin(), snapshot.end(), [](const auto& slot) {
        return slot.resource_id == ResourceId("gripper.1");
      });
  ASSERT_NE(gripper, snapshot.end());
  EXPECT_EQ(gripper->state, ResourceSlotState::kFree);
  EXPECT_FALSE(gripper->owner.has_value());
}

TEST(ResourceSlotTableTest, KeepsI6SingleOperationRuleForDisjointResources) {
  ResourceSlotTable table(
      {ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("op.a", "part.a");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());

  const auto second = owner("op.b", "part.b");
  const auto rejected =
      table.reserve(second, {ResourceId("robot.b.joint")});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "i6_single_operation_busy");

  ASSERT_TRUE(table.release(first, {ResourceId("robot.a.joint")}).ok());
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());
}

TEST(ResourceSlotTableTest,
     ProjectsExactGenerationConfigurationsAndAffectedOperationCounts) {
  ResourceSlotTable table(
      {ResourceId("joint.1"), ResourceId("joint.2"),
       ResourceId("gripper.1")},
      {DomainActiveControlConfigurationDTO{
          ControlDomainId("domain.arm"),
          ActiveControlConfigurationId("active.native"),
          ActiveControlConfigurationGeneration(1U)}});
  const auto operation = owner();

  const auto initial =
      table.admission_snapshot({ResourceId("joint.2")});
  EXPECT_EQ(initial.operation_resource_generation, 0U);
  ASSERT_EQ(initial.active_control_configurations.size(), 1U);
  EXPECT_EQ(initial.active_control_configurations.front().control_domain_id,
            ControlDomainId("domain.arm"));
  EXPECT_EQ(initial.affected_queued_operation_count, 0U);
  EXPECT_EQ(initial.affected_active_operation_count, 0U);

  ASSERT_TRUE(table.reserve(operation, arm_resources()).ok());
  const auto reserved =
      table.admission_snapshot({ResourceId("joint.2")});
  EXPECT_EQ(reserved.operation_resource_generation, 1U);
  EXPECT_EQ(reserved.affected_queued_operation_count, 1U);
  EXPECT_EQ(reserved.affected_active_operation_count, 0U);

  ASSERT_TRUE(table
                  .transition(operation, arm_resources(),
                              ResourceSlotState::kDispatched)
                  .ok());
  const auto dispatched =
      table.admission_snapshot({ResourceId("joint.1")});
  EXPECT_EQ(dispatched.operation_resource_generation, 2U);
  EXPECT_EQ(dispatched.affected_queued_operation_count, 0U);
  EXPECT_EQ(dispatched.affected_active_operation_count, 1U);

  ASSERT_TRUE(table
                  .transition(operation, arm_resources(),
                              ResourceSlotState::kTerminal)
                  .ok());
  const auto terminal = table.admission_snapshot(arm_resources());
  EXPECT_EQ(terminal.operation_resource_generation, 3U);
  EXPECT_EQ(terminal.affected_active_operation_count, 1U);

  ASSERT_TRUE(table.release(operation, arm_resources()).ok());
  const auto released = table.admission_snapshot(arm_resources());
  EXPECT_EQ(released.operation_resource_generation, 4U);
  EXPECT_EQ(released.affected_queued_operation_count, 0U);
  EXPECT_EQ(released.affected_active_operation_count, 0U);

  ASSERT_TRUE(table.mark_unreconciled_external_or_orphan_goal(
                       {ResourceId("joint.1"), ResourceId("joint.2")})
                  .ok());
  const auto orphan = table.admission_snapshot(arm_resources());
  EXPECT_EQ(orphan.operation_resource_generation, 5U);
  EXPECT_EQ(orphan.affected_active_operation_count, 1U);
}

TEST(ResourceSlotTableTest, RejectsStaleOwnerAndIllegalTransition) {
  ResourceSlotTable table(arm_resources());
  const auto current = owner();
  ASSERT_TRUE(table.reserve(current, arm_resources()).ok());

  const auto stale = owner("op.stale", "part.stale");
  EXPECT_FALSE(table
                   .transition(stale, arm_resources(),
                               ResourceSlotState::kDispatched)
                   .ok());
  EXPECT_FALSE(table
                   .transition(current, arm_resources(),
                               ResourceSlotState::kFree)
                   .ok());

  const auto snapshot = table.snapshot();
  EXPECT_TRUE(std::all_of(snapshot.begin(), snapshot.end(),
                          [](const auto& slot) {
                            return slot.state == ResourceSlotState::kReserved;
                          }));
}

TEST(ResourceSlotTableTest,
     AdmissionProjectionAcceptsCandidateOnlyResourcesAndRejectsDuplicates) {
  ResourceSlotTable table(arm_resources());
  const auto candidate_only = table.admission_snapshot(
      {ResourceId("candidate.only.resource")});
  EXPECT_EQ(candidate_only.operation_resource_generation, 0U);
  EXPECT_EQ(candidate_only.affected_queued_operation_count, 0U);
  EXPECT_EQ(candidate_only.affected_active_operation_count, 0U);

  EXPECT_THROW(table.admission_snapshot(
                   {ResourceId("joint.1"), ResourceId("joint.1")}),
               std::invalid_argument);
}

TEST(ResourceSlotTableTest, RejectsInvalidInventoryAtConstruction) {
  EXPECT_THROW(ResourceSlotTable({ResourceId("joint.1"),
                                  ResourceId("joint.1")}),
               std::invalid_argument);
  EXPECT_THROW(ResourceSlotTable(std::vector<ResourceId>{}),
               std::invalid_argument);
  EXPECT_THROW(
      ResourceSlotTable(
          {ResourceId("joint.1")},
          {DomainActiveControlConfigurationDTO{
               ControlDomainId("domain.z"),
               ActiveControlConfigurationId("active.z"),
               ActiveControlConfigurationGeneration(1U)},
           DomainActiveControlConfigurationDTO{
               ControlDomainId("domain.a"),
               ActiveControlConfigurationId("active.a"),
               ActiveControlConfigurationGeneration(1U)}}),
      std::invalid_argument);
}

TEST(ResourceSlotTableTest, RevisesExactOwnerWithoutChangingSlotState) {
  ResourceSlotTable table({ResourceId("arm.j1")});
  const auto initial = owner("operation.1", "participant.1");
  ASSERT_TRUE(table.reserve(initial, {ResourceId("arm.j1")}).ok());

  auto revised = table.revise_owner(initial, {ResourceId("arm.j1")},
                                    OperationRevision(2U));
  ASSERT_TRUE(revised.ok()) << revised.error().message;
  EXPECT_EQ(revised.value().operation_revision, OperationRevision(2U));
  const auto snapshot = table.snapshot();
  ASSERT_EQ(snapshot.size(), 1U);
  EXPECT_EQ(snapshot.front().state, ResourceSlotState::kReserved);
  ASSERT_TRUE(snapshot.front().owner.has_value());
  EXPECT_EQ(snapshot.front().owner->operation_revision,
            OperationRevision(2U));

  const auto stale = table.transition(
      initial, {ResourceId("arm.j1")}, ResourceSlotState::kDispatched);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "resource_owner_mismatch");
  EXPECT_TRUE(table
                  .transition(revised.value(), {ResourceId("arm.j1")},
                              ResourceSlotState::kDispatched)
                  .ok());
}

TEST(ResourceSlotTableTest, OrphanRecoveryMarksOnlyFreeUnownedResourcesAndBlocksAdmission) {
  ResourceSlotTable table(arm_resources());
  ASSERT_TRUE(table.mark_unreconciled_external_or_orphan_goal({ResourceId("joint.1")}).ok());
  const auto snapshot = table.snapshot();
  ASSERT_EQ(snapshot.size(), 2U);
  EXPECT_EQ(snapshot.front().resource_id, ResourceId("joint.1"));
  EXPECT_EQ(snapshot.front().state, ResourceSlotState::kUnreconciledExternalOrOrphanGoal);
  EXPECT_FALSE(snapshot.front().owner.has_value());
  const auto blocked = table.reserve(owner(), arm_resources());
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "resource_scope_busy");
  EXPECT_FALSE(table.mark_unreconciled_external_or_orphan_goal({ResourceId("joint.1")}).ok());
}

}  // namespace
