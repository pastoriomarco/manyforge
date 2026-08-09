#include "managed_kernel_apply_fence_owner.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

namespace core = manyforge::core;
using manyforge::roboplan_backend::ManagedKernelAdmissionGate;

[[nodiscard]] core::ConfigurationFenceScopeDTO AffectedScope() {
  return core::ConfigurationFenceScopeDTO{
      {core::ControllableComponentId("robot.1")},
      {core::ConfigurationFenceDomainScopeDTO{
          core::ControlDomainId("domain.arm"),
          core::ControlDomainRevision("domain.arm.v1"),
          core::ControlDomainRevision("domain.arm.v2")}},
      {core::ConfigurationFenceBindingScopeDTO{
          core::ControlBindingId("binding.arm"),
          core::ControlBindingRevision("binding.arm.v1"),
          core::ControlBindingRevision("binding.arm.v2")}},
      {core::ResourceId("joint.1"), core::ResourceId("joint.2")}};
}

[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
ActiveConfigurations() {
  return {core::DomainActiveControlConfigurationDTO{
      core::ControlDomainId("domain.arm"),
      core::ActiveControlConfigurationId("active.arm.v1"),
      core::ActiveControlConfigurationGeneration(1U)}};
}

[[nodiscard]] core::ResourceSlotOwnerDTO OperationOwner() {
  return core::ResourceSlotOwnerDTO{
      core::OperationId("operation.1"),
      core::OperationRevision(1U),
      core::OperationParticipantId("participant.1"),
      core::ControlDomainId("domain.arm"),
      core::ControlDomainRevision("domain.arm.v1"),
      core::ControlBindingId("binding.arm"),
      core::ControlBindingRevision("binding.arm.v1"),
      core::ControllerTopologyId("topology.native"),
      core::TopologyRevision("topology.native.v1"),
      core::ControlRouteId("route.native"),
      core::ActiveControlConfigurationId("active.arm.v1"),
      core::ActiveControlConfigurationGeneration(1U),
      core::ContainmentGranularity::kResource};
}

TEST(ManagedKernelAdmissionGate,
     SerializesOperationFirstAndFenceFirstOnTheActualSlotTable) {
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      std::vector<core::ResourceId>{core::ResourceId("joint.1"),
                                    core::ResourceId("joint.2")},
      ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  const auto scope = AffectedScope();
  const auto owner = OperationOwner();

  ASSERT_TRUE(gate.reserve(*table, owner, scope.resources).ok());
  const auto operation_first = gate.Observe(scope);
  EXPECT_EQ(operation_first.operation_resource_generation, 1U);
  EXPECT_EQ(operation_first.affected_queued_operation_count, 1U);
  EXPECT_EQ(operation_first.affected_active_operation_count, 0U);
  bool rejected_commit_called = false;
  EXPECT_THROW(
      gate.AcquireFenceDurably(
          core::ConfigurationFenceId("fence.rejected"), scope,
          operation_first.operation_resource_generation,
          ActiveConfigurations(), [&] { rejected_commit_called = true; }),
      std::runtime_error);
  EXPECT_FALSE(rejected_commit_called);

  ASSERT_TRUE(table->release(owner, scope.resources).ok());
  const auto idle = gate.Observe(scope);
  EXPECT_EQ(idle.operation_resource_generation, 2U);
  EXPECT_EQ(idle.affected_queued_operation_count, 0U);
  EXPECT_EQ(idle.affected_active_operation_count, 0U);
  bool durable_commit_called = false;
  gate.AcquireFenceDurably(
      core::ConfigurationFenceId("fence.accepted"), scope,
      idle.operation_resource_generation, ActiveConfigurations(),
      [&] { durable_commit_called = true; });
  EXPECT_TRUE(durable_commit_called);

  const auto fenced_reservation = gate.reserve(*table, owner, scope.resources);
  ASSERT_FALSE(fenced_reservation.ok());
  EXPECT_EQ(fenced_reservation.error().details.at("reason"),
            "configuration_fence_held");
  const auto still_idle = gate.Observe(scope);
  EXPECT_EQ(still_idle.operation_resource_generation, 2U);
  EXPECT_EQ(still_idle.affected_queued_operation_count, 0U);

  gate.ReleaseFence(core::ConfigurationFenceId("fence.accepted"), scope);
  ASSERT_TRUE(gate.reserve(*table, owner, scope.resources).ok());
  EXPECT_EQ(gate.Observe(scope).affected_queued_operation_count, 1U);
}

TEST(ManagedKernelAdmissionGate,
     RestoredFenceBlocksAResourceTableAttachedAfterRestart) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  gate.RestoreFence(core::ConfigurationFenceId("fence.restored"), scope);
  auto table = std::make_shared<core::ResourceSlotTable>(
      std::vector<core::ResourceId>{core::ResourceId("joint.1"),
                                    core::ResourceId("joint.2")},
      ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);

  const auto blocked = gate.reserve(*table, OperationOwner(), scope.resources);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"),
            "configuration_fence_held");
  EXPECT_EQ(gate.Observe(scope).operation_resource_generation, 0U);
}

TEST(ManagedKernelAdmissionGate,
     UnreconciledOrphanWorkRejectsFenceAcquisitionFailClosed) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ASSERT_TRUE(table->mark_unreconciled_external_or_orphan_goal(
                       scope.resources)
                  .ok());
  const auto observation = gate.Observe(scope);
  EXPECT_EQ(observation.operation_resource_generation, 1U);
  EXPECT_EQ(observation.affected_queued_operation_count, 0U);
  EXPECT_EQ(observation.affected_active_operation_count, 1U);
  bool commit_called = false;
  EXPECT_THROW(
      gate.AcquireFenceDurably(
          core::ConfigurationFenceId("fence.orphan-rejected"), scope,
          observation.operation_resource_generation, ActiveConfigurations(),
          [&] { commit_called = true; }),
      std::runtime_error);
  EXPECT_FALSE(commit_called);
}

}  // namespace
