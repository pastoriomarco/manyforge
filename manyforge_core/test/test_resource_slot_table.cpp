#include <gtest/gtest.h>

#include <manyforge_core/resource_slot_table.hpp>
#include <stdexcept>
#include <vector>

namespace {

using namespace manyforge::core;

[[nodiscard]] ResourceSlotOwnerDTO owner(std::string operation = "op.1",
                                         std::string participant = "part.1") {
  return ResourceSlotOwnerDTO{OperationId(std::move(operation)),
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

[[nodiscard]] std::vector<DomainActiveControlConfigurationDTO> active_configurations(
    std::string id = "active.native",
    ActiveControlConfigurationGeneration generation = ActiveControlConfigurationGeneration(1U)) {
  return {DomainActiveControlConfigurationDTO{
      ControlDomainId("domain.arm"), ActiveControlConfigurationId(std::move(id)), generation}};
}

[[nodiscard]] ResourceSlotStopFenceOwnerDTO stop_fence_owner(
    ResourceSlotOwnerDTO expected_owner, std::vector<ResourceId> resources,
    OperationRevision next_revision = OperationRevision(2U)) {
  return ResourceSlotStopFenceOwnerDTO{std::move(expected_owner), std::move(resources),
                                       next_revision};
}

[[nodiscard]] ResourceSlotReservationOwnerDTO reservation_owner(ResourceSlotOwnerDTO value,
                                                                std::vector<ResourceId> resources) {
  return ResourceSlotReservationOwnerDTO{std::move(value), std::move(resources)};
}

TEST(ResourceSlotTableTest, RunsOneExactScopeThroughTerminalAndRelease) {
  ResourceSlotTable table({ResourceId("joint.2"), ResourceId("gripper.1"), ResourceId("joint.1")});
  const auto operation = owner();
  const auto resources = arm_resources();

  ASSERT_TRUE(table.reserve(operation, resources).ok());
  ASSERT_EQ(table.active_operation_ids(), std::vector<OperationId>{operation.operation_id});
  ASSERT_TRUE(table.transition(operation, resources, ResourceSlotState::kDispatched).ok());
  ASSERT_TRUE(table.transition(operation, resources, ResourceSlotState::kStopping).ok());
  ASSERT_TRUE(table.transition(operation, resources, ResourceSlotState::kTerminal).ok());
  ASSERT_TRUE(table.release(operation, resources).ok());
  EXPECT_TRUE(table.active_operation_ids().empty());

  const auto snapshot = table.snapshot();
  EXPECT_EQ(snapshot.size(), 3U);
  for (const auto& slot : snapshot) {
    EXPECT_EQ(slot.state, ResourceSlotState::kFree);
    EXPECT_FALSE(slot.owner.has_value());
  }
}

TEST(ResourceSlotTableTest, RejectsOverlapWithoutPartialMutation) {
  ResourceSlotTable table({ResourceId("joint.1"), ResourceId("joint.2"), ResourceId("gripper.1")});
  const auto first = owner();
  ASSERT_TRUE(table.reserve(first, arm_resources()).ok());

  const auto second = owner("op.2", "part.2");
  const auto rejected = table.reserve(second, {ResourceId("joint.2"), ResourceId("gripper.1")});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "resource_scope_busy");

  const auto snapshot = table.snapshot();
  const auto gripper = std::find_if(snapshot.begin(), snapshot.end(), [](const auto& slot) {
    return slot.resource_id == ResourceId("gripper.1");
  });
  ASSERT_NE(gripper, snapshot.end());
  EXPECT_EQ(gripper->state, ResourceSlotState::kFree);
  EXPECT_FALSE(gripper->owner.has_value());
}

TEST(ResourceSlotTableTest, AdmitsDisjointOperationsAndProgressesThemIndependently) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("op.a", "part.a");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());

  const auto second = owner("op.b", "part.b");
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());
  EXPECT_EQ(table.active_operation_ids(),
            (std::vector<OperationId>{OperationId("op.a"), OperationId("op.b")}));

  ASSERT_TRUE(
      table.transition(first, {ResourceId("robot.a.joint")}, ResourceSlotState::kDispatched).ok());
  const auto snapshot = table.snapshot();
  ASSERT_EQ(snapshot.size(), 2U);
  EXPECT_EQ(snapshot.front().state, ResourceSlotState::kDispatched);
  EXPECT_EQ(snapshot.back().state, ResourceSlotState::kReserved);

  ASSERT_TRUE(
      table.transition(first, {ResourceId("robot.a.joint")}, ResourceSlotState::kTerminal).ok());
  ASSERT_TRUE(table.release(first, {ResourceId("robot.a.joint")}).ok());
  EXPECT_EQ(table.active_operation_ids(), std::vector<OperationId>{OperationId("op.b")});
  ASSERT_TRUE(table.release(second, {ResourceId("robot.b.joint")}).ok());
  EXPECT_TRUE(table.active_operation_ids().empty());
}

TEST(ResourceSlotTableTest, CountsDisjointOperationOwnersWithinTheSelectedScope) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("op.a", "part.a");
  const auto second = owner("op.b", "part.b");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());

  const auto queued =
      table.admission_snapshot({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  EXPECT_EQ(queued.affected_queued_operation_count, 2U);
  EXPECT_EQ(queued.affected_active_operation_count, 0U);

  ASSERT_TRUE(
      table.transition(first, {ResourceId("robot.a.joint")}, ResourceSlotState::kDispatched).ok());
  const auto mixed =
      table.admission_snapshot({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  EXPECT_EQ(mixed.affected_queued_operation_count, 1U);
  EXPECT_EQ(mixed.affected_active_operation_count, 1U);
}

TEST(ResourceSlotTableTest, DeduplicatesOneOperationAcrossDisjointParticipantOwners) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("coordinated.op", "participant.a");
  const auto second = owner("coordinated.op", "participant.b");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());

  EXPECT_EQ(table.active_operation_ids(), std::vector<OperationId>{OperationId("coordinated.op")});
  const auto admission =
      table.admission_snapshot({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  EXPECT_EQ(admission.affected_queued_operation_count, 1U);
  EXPECT_EQ(admission.affected_active_operation_count, 0U);
}

TEST(ResourceSlotTableTest, BatchReservationPublishesEveryParticipantWithOneGenerationAdvance) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("coordinated.op", "participant.a");
  const auto second = owner("coordinated.op", "participant.b");

  const auto reserved =
      table.reserve_batch({reservation_owner(first, {ResourceId("robot.a.joint")}),
                           reservation_owner(second, {ResourceId("robot.b.joint")})});
  ASSERT_TRUE(reserved.ok()) << reserved.error().message;

  const auto snapshot = table.coherent_snapshot();
  EXPECT_EQ(snapshot.operation_resource_generation, 1U);
  ASSERT_EQ(snapshot.slots.size(), 2U);
  EXPECT_EQ(snapshot.slots[0].owner, std::optional(first));
  EXPECT_EQ(snapshot.slots[1].owner, std::optional(second));
  EXPECT_EQ(table.active_operation_ids(), std::vector<OperationId>{OperationId("coordinated.op")});
}

TEST(ResourceSlotTableTest, BatchReservationRejectsAnyBusyOrInvalidRowWithoutMutation) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto occupied = owner("existing.op", "existing.participant");
  ASSERT_TRUE(table.reserve(occupied, {ResourceId("robot.b.joint")}).ok());
  const auto before = table.coherent_snapshot();
  const auto first = owner("coordinated.op", "participant.a");
  const auto second = owner("coordinated.op", "participant.b");

  const auto busy = table.reserve_batch({reservation_owner(first, {ResourceId("robot.a.joint")}),
                                         reservation_owner(second, {ResourceId("robot.b.joint")})});
  ASSERT_FALSE(busy.ok());
  EXPECT_EQ(busy.error().details.at("reason"), "resource_scope_busy");
  EXPECT_EQ(table.coherent_snapshot(), before);

  const auto noncanonical =
      table.reserve_batch({reservation_owner(second, {ResourceId("robot.a.joint")}),
                           reservation_owner(first, {ResourceId("robot.b.joint")})});
  ASSERT_FALSE(noncanonical.ok());
  EXPECT_EQ(noncanonical.error().details.at("reason"), "resource_reservation_not_canonical");
  EXPECT_EQ(table.coherent_snapshot(), before);
}

TEST(ResourceSlotTableTest, BatchReservationRejectsOverlapAndRevisionDisagreement) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("coordinated.op", "participant.a");
  auto second = owner("coordinated.op", "participant.b");

  const auto overlap =
      table.reserve_batch({reservation_owner(first, {ResourceId("robot.a.joint")}),
                           reservation_owner(second, {ResourceId("robot.a.joint")})});
  ASSERT_FALSE(overlap.ok());
  EXPECT_EQ(overlap.error().details.at("reason"), "resource_reservation_scope_overlap");
  EXPECT_EQ(table.coherent_snapshot().operation_resource_generation, 0U);

  second.operation_revision = OperationRevision(2U);
  const auto inconsistent =
      table.reserve_batch({reservation_owner(first, {ResourceId("robot.a.joint")}),
                           reservation_owner(second, {ResourceId("robot.b.joint")})});
  ASSERT_FALSE(inconsistent.ok());
  EXPECT_EQ(inconsistent.error().details.at("reason"),
            "resource_reservation_operation_revision_inconsistent");
  EXPECT_EQ(table.coherent_snapshot().operation_resource_generation, 0U);
}

TEST(ResourceSlotTableTest, BatchTransitionAndReleaseEachAdvanceTheGenerationOnce) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("coordinated.op", "participant.a");
  const auto second = owner("coordinated.op", "participant.b");
  const std::vector<ResourceSlotReservationOwnerDTO> owners{
      reservation_owner(first, {ResourceId("robot.a.joint")}),
      reservation_owner(second, {ResourceId("robot.b.joint")})};
  ASSERT_TRUE(table.reserve_batch(owners).ok());

  ASSERT_TRUE(table.transition_batch(owners, ResourceSlotState::kDispatched).ok());
  auto snapshot = table.coherent_snapshot();
  EXPECT_EQ(snapshot.operation_resource_generation, 2U);
  EXPECT_TRUE(std::all_of(snapshot.slots.begin(), snapshot.slots.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kDispatched;
  }));

  ASSERT_TRUE(table.transition_batch(owners, ResourceSlotState::kTerminal).ok());
  ASSERT_TRUE(table.release_batch(owners).ok());
  snapshot = table.coherent_snapshot();
  EXPECT_EQ(snapshot.operation_resource_generation, 4U);
  EXPECT_TRUE(std::all_of(snapshot.slots.begin(), snapshot.slots.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kFree && !slot.owner.has_value();
  }));
}

TEST(ResourceSlotTableTest, BatchTransitionAndReleasePrevalidateWithoutPartialMutation) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("coordinated.op", "participant.a");
  const auto second = owner("coordinated.op", "participant.b");
  const std::vector<ResourceSlotReservationOwnerDTO> owners{
      reservation_owner(first, {ResourceId("robot.a.joint")}),
      reservation_owner(second, {ResourceId("robot.b.joint")})};
  ASSERT_TRUE(table.reserve_batch(owners).ok());
  const auto before = table.coherent_snapshot();

  auto stale = owners;
  stale.back().owner.operation_revision = OperationRevision(2U);
  const auto transition = table.transition_batch(stale, ResourceSlotState::kDispatched);
  ASSERT_FALSE(transition.ok());
  EXPECT_EQ(transition.error().details.at("reason"), "resource_owner_mismatch");
  EXPECT_EQ(table.coherent_snapshot(), before);

  const auto release = table.release_batch(stale);
  ASSERT_FALSE(release.ok());
  EXPECT_EQ(release.error().details.at("reason"), "resource_owner_mismatch");
  EXPECT_EQ(table.coherent_snapshot(), before);
}

TEST(ResourceSlotTableTest, ProjectsExactGenerationConfigurationsAndAffectedOperationCounts) {
  ResourceSlotTable table(
      {ResourceId("joint.1"), ResourceId("joint.2"), ResourceId("gripper.1")},
      {DomainActiveControlConfigurationDTO{ControlDomainId("domain.arm"),
                                           ActiveControlConfigurationId("active.native"),
                                           ActiveControlConfigurationGeneration(1U)}});
  const auto operation = owner();

  const auto initial = table.admission_snapshot({ResourceId("joint.2")});
  EXPECT_EQ(initial.operation_resource_generation, 0U);
  ASSERT_EQ(initial.active_control_configurations.size(), 1U);
  EXPECT_EQ(initial.route_published_active_control_configurations,
            initial.active_control_configurations);
  EXPECT_EQ(initial.active_control_configurations.front().control_domain_id,
            ControlDomainId("domain.arm"));
  EXPECT_EQ(initial.affected_queued_operation_count, 0U);
  EXPECT_EQ(initial.affected_active_operation_count, 0U);

  ASSERT_TRUE(table.reserve(operation, arm_resources()).ok());
  const auto reserved = table.admission_snapshot({ResourceId("joint.2")});
  EXPECT_EQ(reserved.operation_resource_generation, 1U);
  EXPECT_EQ(reserved.affected_queued_operation_count, 1U);
  EXPECT_EQ(reserved.affected_active_operation_count, 0U);

  ASSERT_TRUE(table.transition(operation, arm_resources(), ResourceSlotState::kDispatched).ok());
  const auto dispatched = table.admission_snapshot({ResourceId("joint.1")});
  EXPECT_EQ(dispatched.operation_resource_generation, 2U);
  EXPECT_EQ(dispatched.affected_queued_operation_count, 0U);
  EXPECT_EQ(dispatched.affected_active_operation_count, 1U);

  ASSERT_TRUE(table.transition(operation, arm_resources(), ResourceSlotState::kTerminal).ok());
  const auto terminal = table.admission_snapshot(arm_resources());
  EXPECT_EQ(terminal.operation_resource_generation, 3U);
  EXPECT_EQ(terminal.affected_active_operation_count, 1U);

  ASSERT_TRUE(table.release(operation, arm_resources()).ok());
  const auto released = table.admission_snapshot(arm_resources());
  EXPECT_EQ(released.operation_resource_generation, 4U);
  EXPECT_EQ(released.affected_queued_operation_count, 0U);
  EXPECT_EQ(released.affected_active_operation_count, 0U);

  ASSERT_TRUE(
      table
          .mark_unreconciled_external_or_orphan_goal({ResourceId("joint.1"), ResourceId("joint.2")})
          .ok());
  const auto orphan = table.admission_snapshot(arm_resources());
  EXPECT_EQ(orphan.operation_resource_generation, 5U);
  EXPECT_EQ(orphan.affected_active_operation_count, 1U);
}

TEST(ResourceSlotTableTest, CoherentSnapshotReturnsSlotsAndTheirExactGenerationTogether) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto initial = table.coherent_snapshot();
  EXPECT_EQ(initial.operation_resource_generation, 0U);
  ASSERT_EQ(initial.slots.size(), 2U);

  const auto first = owner("op.a", "part.a");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());
  const auto reserved = table.coherent_snapshot();
  EXPECT_EQ(reserved.operation_resource_generation, 1U);
  ASSERT_EQ(reserved.slots.size(), 2U);
  EXPECT_EQ(reserved.slots.front().state, ResourceSlotState::kReserved);
  EXPECT_EQ(reserved.slots.front().owner, std::optional(first));
  EXPECT_EQ(reserved.slots.front().stop_generation, StopGeneration(0U));
  EXPECT_EQ(reserved.slots.back().state, ResourceSlotState::kFree);
}

TEST(ResourceSlotTableTest,
     AtomicallyFencesMultipleOwnersAndOrphanWithOneResourceGenerationAdvance) {
  ResourceSlotTable table(
      {ResourceId("orphan.joint"), ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("op.a", "part.a");
  const auto second = owner("op.b", "part.b");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());
  ASSERT_TRUE(
      table.transition(second, {ResourceId("robot.b.joint")}, ResourceSlotState::kDispatched).ok());
  ASSERT_TRUE(table.mark_unreconciled_external_or_orphan_goal({ResourceId("orphan.joint")}).ok());
  const auto captured = table.coherent_snapshot();
  ASSERT_EQ(captured.operation_resource_generation, 4U);

  const ResourceSlotStopFenceRequestDTO request{
      captured.operation_resource_generation,
      StopGeneration(9U),
      {stop_fence_owner(first, {ResourceId("robot.a.joint")}),
       stop_fence_owner(second, {ResourceId("robot.b.joint")})},
      {ResourceId("orphan.joint")}};
  const auto fenced = table.stop_fence_batch(request);
  ASSERT_TRUE(fenced.ok()) << fenced.error().message;
  EXPECT_EQ(fenced.value().operation_resource_generation, 5U);
  EXPECT_EQ(fenced.value().stop_generation, StopGeneration(9U));
  ASSERT_EQ(fenced.value().revised_owners.size(), 2U);
  EXPECT_EQ(fenced.value().revised_owners[0].operation_revision, OperationRevision(2U));
  EXPECT_EQ(fenced.value().revised_owners[1].operation_revision, OperationRevision(2U));
  EXPECT_EQ(fenced.value().orphan_resources, std::vector<ResourceId>{ResourceId("orphan.joint")});

  const auto after = table.coherent_snapshot();
  EXPECT_EQ(after.operation_resource_generation, 5U);
  for (const auto& slot : after.slots) {
    EXPECT_EQ(slot.stop_generation, StopGeneration(9U));
    if (slot.resource_id == ResourceId("orphan.joint")) {
      EXPECT_EQ(slot.state, ResourceSlotState::kUnreconciledExternalOrOrphanGoal);
      EXPECT_FALSE(slot.owner.has_value());
    } else {
      EXPECT_EQ(slot.state, ResourceSlotState::kStopping);
      ASSERT_TRUE(slot.owner.has_value());
      EXPECT_EQ(slot.owner->operation_revision, OperationRevision(2U));
    }
  }
}

TEST(ResourceSlotTableTest, BatchFencePrevalidatesEveryOwnerBeforeMutatingAnySlot) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("op.a", "part.a");
  const auto second = owner("op.b", "part.b");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());
  const auto before = table.coherent_snapshot();

  auto stale_second = second;
  stale_second.operation_revision = OperationRevision(7U);
  const auto rejected = table.stop_fence_batch(ResourceSlotStopFenceRequestDTO{
      before.operation_resource_generation,
      StopGeneration(1U),
      {stop_fence_owner(first, {ResourceId("robot.a.joint")}),
       stop_fence_owner(stale_second, {ResourceId("robot.b.joint")}, OperationRevision(8U))},
      {}});
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "resource_owner_mismatch");
  EXPECT_EQ(table.coherent_snapshot(), before);
}

TEST(ResourceSlotTableTest, BatchFenceRejectsStaleCaptureAndInvalidOrphanWithoutPartialMutation) {
  ResourceSlotTable table({ResourceId("free.joint"), ResourceId("robot.a.joint")});
  const auto operation = owner("op.a", "part.a");
  ASSERT_TRUE(table.reserve(operation, {ResourceId("robot.a.joint")}).ok());
  const auto before = table.coherent_snapshot();

  const auto stale = table.stop_fence_batch(
      ResourceSlotStopFenceRequestDTO{before.operation_resource_generation - 1U,
                                      StopGeneration(1U),
                                      {stop_fence_owner(operation, {ResourceId("robot.a.joint")})},
                                      {}});
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "operation_resource_generation_stale");
  EXPECT_EQ(table.coherent_snapshot(), before);

  const auto invalid_orphan = table.stop_fence_batch(
      ResourceSlotStopFenceRequestDTO{before.operation_resource_generation,
                                      StopGeneration(1U),
                                      {stop_fence_owner(operation, {ResourceId("robot.a.joint")})},
                                      {ResourceId("free.joint")}});
  ASSERT_FALSE(invalid_orphan.ok());
  EXPECT_EQ(invalid_orphan.error().details.at("reason"),
            "resource_stop_fence_orphan_state_invalid");
  EXPECT_EQ(table.coherent_snapshot(), before);
}

TEST(ResourceSlotTableTest, BatchFenceRequiresCanonicalDisjointOwnerScopesAndRevisions) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("op.a", "part.a");
  const auto second = owner("op.b", "part.b");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());
  const auto before = table.coherent_snapshot();

  const auto noncanonical = table.stop_fence_batch(
      ResourceSlotStopFenceRequestDTO{before.operation_resource_generation,
                                      StopGeneration(1U),
                                      {stop_fence_owner(second, {ResourceId("robot.b.joint")}),
                                       stop_fence_owner(first, {ResourceId("robot.a.joint")})},
                                      {}});
  ASSERT_FALSE(noncanonical.ok());
  EXPECT_EQ(noncanonical.error().details.at("reason"), "resource_stop_fence_not_canonical");

  const auto overlap = table.stop_fence_batch(
      ResourceSlotStopFenceRequestDTO{before.operation_resource_generation,
                                      StopGeneration(1U),
                                      {stop_fence_owner(first, {ResourceId("robot.a.joint")}),
                                       stop_fence_owner(second, {ResourceId("robot.a.joint")})},
                                      {}});
  ASSERT_FALSE(overlap.ok());
  EXPECT_EQ(overlap.error().details.at("reason"), "resource_stop_fence_scope_overlap");
  EXPECT_EQ(table.coherent_snapshot(), before);
}

TEST(ResourceSlotTableTest, BatchFenceAdvancesEveryParticipantOwnerOfOneOperationConsistently) {
  ResourceSlotTable table({ResourceId("robot.a.joint"), ResourceId("robot.b.joint")});
  const auto first = owner("coordinated.op", "participant.a");
  const auto second = owner("coordinated.op", "participant.b");
  ASSERT_TRUE(table.reserve(first, {ResourceId("robot.a.joint")}).ok());
  ASSERT_TRUE(table.reserve(second, {ResourceId("robot.b.joint")}).ok());
  const auto before = table.coherent_snapshot();

  const auto inconsistent = table.stop_fence_batch(ResourceSlotStopFenceRequestDTO{
      before.operation_resource_generation,
      StopGeneration(2U),
      {stop_fence_owner(first, {ResourceId("robot.a.joint")}, OperationRevision(2U)),
       stop_fence_owner(second, {ResourceId("robot.b.joint")}, OperationRevision(3U))},
      {}});
  ASSERT_FALSE(inconsistent.ok());
  EXPECT_EQ(inconsistent.error().details.at("reason"),
            "resource_stop_fence_operation_revision_inconsistent");
  EXPECT_EQ(table.coherent_snapshot(), before);

  const auto fenced = table.stop_fence_batch(ResourceSlotStopFenceRequestDTO{
      before.operation_resource_generation,
      StopGeneration(2U),
      {stop_fence_owner(first, {ResourceId("robot.a.joint")}, OperationRevision(3U)),
       stop_fence_owner(second, {ResourceId("robot.b.joint")}, OperationRevision(3U))},
      {}});
  ASSERT_TRUE(fenced.ok()) << fenced.error().message;
  EXPECT_EQ(fenced.value().operation_resource_generation,
            before.operation_resource_generation + 1U);
  ASSERT_EQ(fenced.value().revised_owners.size(), 2U);
  EXPECT_EQ(fenced.value().revised_owners[0].operation_id, OperationId("coordinated.op"));
  EXPECT_EQ(fenced.value().revised_owners[0].operation_revision, OperationRevision(3U));
  EXPECT_EQ(fenced.value().revised_owners[1].operation_id, OperationId("coordinated.op"));
  EXPECT_EQ(fenced.value().revised_owners[1].operation_revision, OperationRevision(3U));
}

TEST(ResourceSlotTableTest, ResourceStopGenerationSurvivesTerminalReleaseAndReuse) {
  ResourceSlotTable table({ResourceId("arm.joint")});
  const auto first = owner("op.a", "part.a");
  ASSERT_TRUE(table.reserve(first, {ResourceId("arm.joint")}).ok());
  auto fenced = table.stop_fence_batch(ResourceSlotStopFenceRequestDTO{
      1U, StopGeneration(4U), {stop_fence_owner(first, {ResourceId("arm.joint")})}, {}});
  ASSERT_TRUE(fenced.ok()) << fenced.error().message;
  const auto revised = fenced.value().revised_owners.front();
  ASSERT_TRUE(
      table.transition(revised, {ResourceId("arm.joint")}, ResourceSlotState::kTerminal).ok());
  ASSERT_TRUE(table.release(revised, {ResourceId("arm.joint")}).ok());
  ASSERT_TRUE(table.reserve(owner("op.b", "part.b"), {ResourceId("arm.joint")}).ok());

  const auto snapshot = table.coherent_snapshot();
  ASSERT_EQ(snapshot.slots.size(), 1U);
  EXPECT_EQ(snapshot.slots.front().state, ResourceSlotState::kReserved);
  EXPECT_EQ(snapshot.slots.front().stop_generation, StopGeneration(4U));
  ASSERT_TRUE(snapshot.slots.front().owner.has_value());

  const auto stale = table.stop_fence_batch(ResourceSlotStopFenceRequestDTO{
      snapshot.operation_resource_generation,
      StopGeneration(4U),
      {stop_fence_owner(*snapshot.slots.front().owner, {ResourceId("arm.joint")})},
      {}});
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "resource_stop_generation_stale");
  EXPECT_EQ(table.coherent_snapshot(), snapshot);
}

TEST(ResourceSlotTableTest, RejectsStaleOwnerAndIllegalTransition) {
  ResourceSlotTable table(arm_resources());
  const auto current = owner();
  ASSERT_TRUE(table.reserve(current, arm_resources()).ok());

  const auto stale = owner("op.stale", "part.stale");
  EXPECT_FALSE(table.transition(stale, arm_resources(), ResourceSlotState::kDispatched).ok());
  EXPECT_FALSE(table.transition(current, arm_resources(), ResourceSlotState::kFree).ok());

  const auto snapshot = table.snapshot();
  EXPECT_TRUE(std::all_of(snapshot.begin(), snapshot.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kReserved;
  }));
}

TEST(ResourceSlotTableTest, AdmissionProjectionAcceptsCandidateOnlyResourcesAndRejectsDuplicates) {
  ResourceSlotTable table(arm_resources());
  const auto candidate_only = table.admission_snapshot({ResourceId("candidate.only.resource")});
  EXPECT_EQ(candidate_only.operation_resource_generation, 0U);
  EXPECT_EQ(candidate_only.affected_queued_operation_count, 0U);
  EXPECT_EQ(candidate_only.affected_active_operation_count, 0U);

  EXPECT_THROW(table.admission_snapshot({ResourceId("joint.1"), ResourceId("joint.1")}),
               std::invalid_argument);
}

TEST(ResourceSlotTableTest, RejectsInvalidInventoryAtConstruction) {
  EXPECT_THROW(ResourceSlotTable({ResourceId("joint.1"), ResourceId("joint.1")}),
               std::invalid_argument);
  EXPECT_THROW(ResourceSlotTable(
                   {ResourceId("joint.1")},
                   {DomainActiveControlConfigurationDTO{ControlDomainId("domain.z"),
                                                        ActiveControlConfigurationId("active.z"),
                                                        ActiveControlConfigurationGeneration(1U)},
                    DomainActiveControlConfigurationDTO{ControlDomainId("domain.a"),
                                                        ActiveControlConfigurationId("active.a"),
                                                        ActiveControlConfigurationGeneration(1U)}}),
               std::invalid_argument);
}

TEST(ResourceSlotTableTest, EmptyRuntimeInventoryCanBePopulatedAndEmptied) {
  ResourceSlotTable table(std::vector<ResourceId>{});
  EXPECT_TRUE(table.snapshot().empty());
  ASSERT_TRUE(table.reconfigure_runtime_inventory(
                       {ResourceId("joint.1")}, {}, {})
                  .ok());
  ASSERT_EQ(table.snapshot().size(), 1U);
  ASSERT_TRUE(table.reconfigure_runtime_inventory({}, {}, {}).ok());
  EXPECT_TRUE(table.snapshot().empty());
}

TEST(ResourceSlotTableTest, RevisesExactOwnerWithoutChangingSlotState) {
  ResourceSlotTable table({ResourceId("arm.j1")});
  const auto initial = owner("operation.1", "participant.1");
  ASSERT_TRUE(table.reserve(initial, {ResourceId("arm.j1")}).ok());

  auto revised = table.revise_owner(initial, {ResourceId("arm.j1")}, OperationRevision(2U));
  ASSERT_TRUE(revised.ok()) << revised.error().message;
  EXPECT_EQ(revised.value().operation_revision, OperationRevision(2U));
  const auto snapshot = table.snapshot();
  ASSERT_EQ(snapshot.size(), 1U);
  EXPECT_EQ(snapshot.front().state, ResourceSlotState::kReserved);
  ASSERT_TRUE(snapshot.front().owner.has_value());
  EXPECT_EQ(snapshot.front().owner->operation_revision, OperationRevision(2U));

  const auto stale =
      table.transition(initial, {ResourceId("arm.j1")}, ResourceSlotState::kDispatched);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "resource_owner_mismatch");
  EXPECT_TRUE(
      table.transition(revised.value(), {ResourceId("arm.j1")}, ResourceSlotState::kDispatched)
          .ok());
}

TEST(ResourceSlotTableTest, BatchRevisionAdvancesEveryParticipantOwnerExactlyOnce) {
  ResourceSlotTable table({ResourceId("arm.j1"), ResourceId("gripper.j1")});
  const std::vector<ResourceSlotReservationOwnerDTO> owners{
      reservation_owner(owner("operation.1", "participant.1"), {ResourceId("arm.j1")}),
      reservation_owner(owner("operation.1", "participant.2"), {ResourceId("gripper.j1")})};
  ASSERT_TRUE(table.reserve_batch(owners).ok());
  const auto generation_before = table.coherent_snapshot().operation_resource_generation;

  const auto revised = table.revise_owner_batch(owners, OperationRevision(2U));
  ASSERT_TRUE(revised.ok()) << revised.error().message;
  ASSERT_EQ(revised.value().size(), 2U);
  EXPECT_TRUE(std::all_of(revised.value().begin(), revised.value().end(), [](const auto& row) {
    return row.operation_revision == OperationRevision(2U);
  }));
  const auto snapshot = table.coherent_snapshot();
  EXPECT_EQ(snapshot.operation_resource_generation, generation_before + 1U);
  EXPECT_TRUE(std::all_of(snapshot.slots.begin(), snapshot.slots.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kReserved && slot.owner.has_value() &&
           slot.owner->operation_revision == OperationRevision(2U);
  }));
}

TEST(ResourceSlotTableTest, BatchTransitionRevisionPrevalidatesWithoutPartialMutation) {
  ResourceSlotTable table({ResourceId("arm.j1"), ResourceId("gripper.j1")});
  const std::vector<ResourceSlotReservationOwnerDTO> owners{
      reservation_owner(owner("operation.1", "participant.1"), {ResourceId("arm.j1")}),
      reservation_owner(owner("operation.1", "participant.2"), {ResourceId("gripper.j1")})};
  ASSERT_TRUE(table.reserve_batch(owners).ok());
  auto invalid = owners;
  invalid.back().owner.operation_id = OperationId("operation.2");
  const auto rejected = table.transition_and_revise_owner_batch(
      invalid, ResourceSlotState::kTerminal, OperationRevision(2U));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "resource_revision_batch_row_invalid");
  const auto unchanged = table.snapshot();
  EXPECT_TRUE(std::all_of(unchanged.begin(), unchanged.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kReserved && slot.owner.has_value() &&
           slot.owner->operation_revision == OperationRevision(1U);
  }));

  const auto revised = table.transition_and_revise_owner_batch(owners, ResourceSlotState::kTerminal,
                                                               OperationRevision(2U));
  ASSERT_TRUE(revised.ok()) << revised.error().message;
  const auto terminal = table.snapshot();
  EXPECT_TRUE(std::all_of(terminal.begin(), terminal.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kTerminal && slot.owner.has_value() &&
           slot.owner->operation_revision == OperationRevision(2U);
  }));
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

TEST(ResourceSlotTableTest, ReconcilesOnlyAnExactFreshOrphanSubsetAtomically) {
  ResourceSlotTable table(arm_resources());
  ASSERT_TRUE(
      table
          .mark_unreconciled_external_or_orphan_goal({ResourceId("joint.1"), ResourceId("joint.2")})
          .ok());
  const auto marked = table.coherent_snapshot();
  ASSERT_EQ(marked.operation_resource_generation, 1U);

  const auto first = table.reconcile_unreconciled_external_or_orphan_goal(
      marked.operation_resource_generation, {ResourceId("joint.1")});
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first.value(), 2U);
  const auto partial = table.snapshot();
  ASSERT_EQ(partial.size(), 2U);
  EXPECT_EQ(partial[0].state, ResourceSlotState::kFree);
  EXPECT_EQ(partial[1].state, ResourceSlotState::kUnreconciledExternalOrOrphanGoal);

  const auto stale = table.reconcile_unreconciled_external_or_orphan_goal(
      marked.operation_resource_generation, {ResourceId("joint.2")});
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().details.at("reason"), "resource_orphan_reconciliation_generation_stale");
  EXPECT_EQ(table.snapshot(), partial);

  const auto noncanonical = table.reconcile_unreconciled_external_or_orphan_goal(
      first.value(), {ResourceId("joint.2"), ResourceId("joint.1")});
  ASSERT_FALSE(noncanonical.ok());
  EXPECT_EQ(noncanonical.error().details.at("reason"),
            "resource_orphan_reconciliation_scope_noncanonical");
  EXPECT_EQ(table.snapshot(), partial);

  const auto second =
      table.reconcile_unreconciled_external_or_orphan_goal(first.value(), {ResourceId("joint.2")});
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(second.value(), 3U);
  const auto complete = table.snapshot();
  EXPECT_TRUE(std::all_of(complete.begin(), complete.end(), [](const auto& slot) {
    return slot.state == ResourceSlotState::kFree && !slot.owner.has_value();
  }));
}

TEST(ResourceSlotTableTest, ReconfiguresFreeInventoryWhilePreservingOneUnchangedOperation) {
  ResourceSlotTable table(
      {ResourceId("joint.1"), ResourceId("joint.2"), ResourceId("removed.free")},
      active_configurations());
  const auto operation = owner();
  ASSERT_TRUE(table.reserve(operation, arm_resources()).ok());
  ASSERT_TRUE(table.transition(operation, arm_resources(), ResourceSlotState::kDispatched).ok());

  const auto reconfigured = table.reconfigure_runtime_inventory(
      {ResourceId("joint.2"), ResourceId("added.free"), ResourceId("joint.1")},
      active_configurations(), active_configurations());
  ASSERT_TRUE(reconfigured.ok()) << reconfigured.error().message;

  const auto snapshot = table.snapshot();
  ASSERT_EQ(snapshot.size(), 3U);
  EXPECT_EQ(snapshot.front().resource_id, ResourceId("added.free"));
  EXPECT_EQ(snapshot.front().state, ResourceSlotState::kFree);
  for (const auto& slot : snapshot) {
    if (slot.resource_id == ResourceId("added.free")) {
      continue;
    }
    EXPECT_EQ(slot.state, ResourceSlotState::kDispatched);
    ASSERT_TRUE(slot.owner.has_value());
    EXPECT_EQ(*slot.owner, operation);
  }
  const auto admission = table.admission_snapshot(arm_resources());
  EXPECT_EQ(admission.operation_resource_generation, 3U);
  EXPECT_EQ(admission.affected_active_operation_count, 1U);
}

TEST(ResourceSlotTableTest, RejectsReconfigurationThatRemovesAnOccupiedResource) {
  ResourceSlotTable table(arm_resources(), active_configurations());
  const auto operation = owner();
  ASSERT_TRUE(table.reserve(operation, arm_resources()).ok());

  const auto rejected =
      table.reconfigure_runtime_inventory({ResourceId("joint.1")}, active_configurations(),
                                          active_configurations());
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"),
            "runtime_resource_inventory_removes_occupied_resource");
  EXPECT_EQ(table.snapshot().size(), 2U);
}

TEST(ResourceSlotTableTest, RejectsReconfigurationThatChangesAnOccupiedControlConfiguration) {
  ResourceSlotTable table(arm_resources(), active_configurations());
  const auto operation = owner();
  ASSERT_TRUE(table.reserve(operation, arm_resources()).ok());

  const auto rejected = table.reconfigure_runtime_inventory(
      arm_resources(), active_configurations("active.replaced"),
      active_configurations("active.replaced"));
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().details.at("reason"), "runtime_active_control_configuration_changed");
  const auto admission = table.admission_snapshot(arm_resources());
  EXPECT_EQ(admission.operation_resource_generation, 1U);
  EXPECT_EQ(admission.active_control_configurations, active_configurations());
}

TEST(ResourceSlotTableTest, ReconfiguresAQuiescentRuntimeToNoCommandableResources) {
  ResourceSlotTable table(arm_resources(), active_configurations());

  const auto reconfigured = table.reconfigure_runtime_inventory({}, {}, {});
  ASSERT_TRUE(reconfigured.ok()) << reconfigured.error().message;
  EXPECT_TRUE(table.snapshot().empty());
  const auto admission = table.admission_snapshot(arm_resources());
  EXPECT_EQ(admission.operation_resource_generation, 1U);
  EXPECT_TRUE(admission.active_control_configurations.empty());
  EXPECT_TRUE(admission.route_published_active_control_configurations.empty());
  EXPECT_EQ(admission.affected_queued_operation_count, 0U);
  EXPECT_EQ(admission.affected_active_operation_count, 0U);
}

TEST(ResourceSlotTableTest, SeparatesApplyStagedAndRoutePublishedConfigurations) {
  ResourceSlotTable table(arm_resources(), active_configurations());

  const auto staged = table.reconfigure_runtime_inventory(
      arm_resources(), active_configurations("active.staged"), {});
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  const auto admission = table.admission_snapshot(arm_resources());
  EXPECT_EQ(admission.active_control_configurations,
            active_configurations("active.staged"));
  EXPECT_TRUE(admission.route_published_active_control_configurations.empty());

  const auto published = table.replace_route_publication_inventory(
      active_configurations("active.cclc"));
  ASSERT_TRUE(published.ok()) << published.error().message;
  const auto current = table.admission_snapshot(arm_resources());
  EXPECT_EQ(current.active_control_configurations,
            active_configurations("active.staged"));
  EXPECT_EQ(current.route_published_active_control_configurations,
            active_configurations("active.cclc"));
}

}  // namespace
