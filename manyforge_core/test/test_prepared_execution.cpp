#include <gtest/gtest.h>

#include <limits>
#include <manyforge_core/prepared_execution_membership.hpp>
#include <manyforge_core/types/prepared_execution.hpp>

namespace manyforge::core {
namespace {

[[nodiscard]] std::string Digest(char value) {
  return "sha256:" + std::string(64U, value);
}

[[nodiscard]] PreparedOperationParticipantDTO PreparedParticipant() {
  const OperationId operation_id("operation.1");
  const OperationRevision operation_revision(3U);
  const OperationParticipantId participant_id("participant.1");
  const ProgramId program_id("program.1");
  const ProgramRevision program_revision(Digest('1'));
  const ProgramBindingId program_binding_id(Digest('2'));
  const ProgramRunId program_run_id("run.1");
  PreparedParticipantStopPlanDTO stop_plan{
      ContentDigest(Digest('3')),
      ResolvedStopBasePlanId(Digest('4')),
      operation_id,
      operation_revision,
      participant_id,
      program_id,
      program_revision,
      program_binding_id,
      program_run_id,
      std::nullopt,
      ParticipantStopProfilePurposeDTO::kOrdinary,
      std::nullopt,
      {}};
  PreparedCapabilityCommandDTO payload{
      CommandSchemaId("command.schema"),
      CommandSchemaRevision(Digest('5')),
      GripperPositionCommandValueDTO{AngularPositionDTO{0.2}, std::nullopt},
      {},
      {},
      {},
      2.0,
      CommandCompletionRule::kGoalFencedTargetReached,
      CommandConfirmationRule::kBindingFeedbackRequired,
      {CommandStopEvidenceRequirement::kCommandTerminal}};
  return PreparedOperationParticipantDTO{
      ContentDigest(Digest('a')),
      operation_id,
      operation_revision,
      participant_id,
      program_id,
      program_revision,
      program_binding_id,
      program_run_id,
      RetainedBundleId(Digest('6')),
      GraphRevision(Digest('7')),
      ControllableComponentId("robot.1"),
      ComponentLifecycleGeneration(9U),
      ComponentTypeId("robot.type"),
      ComponentTypeRevision("robot.type.revision"),
      CommandTargetId("gripper"),
      CapabilityId("gripper.position"),
      CapabilityDefinitionRevision(Digest('8')),
      {},
      {},
      ControlDomainId("domain.1"),
      ControlDomainRevision(Digest('9')),
      ControlBindingId("binding.1"),
      ControlBindingRevision(Digest('b')),
      ControlInterfaceContractId("control.interface"),
      ControlInterfaceRevision(Digest('c')),
      ControllerTopologyId("topology.1"),
      TopologyRevision(Digest('d')),
      ControlRouteId("route.1"),
      ActivationSetId("activation.1"),
      PreparedProviderRuntimeOccurrenceDTO{ProviderInstanceId("provider.1"),
                                           ProviderManifestId(Digest('e')),
                                           ProviderManifestRevision(Digest('f')),
                                           RuntimeUnitId("runtime.1"), RuntimeEpoch("epoch.1")},
      std::nullopt,
      {ResourceId("resource.1"), ResourceId("resource.2")},
      PreparedParticipantFocusedPortDTO{RuntimeUnitId("runtime.1")},
      {{ResourceId("resource.1"), "position"}, {ResourceId("resource.2"), "position"}},
      {{ResourceId("resource.1"), "position"}, {ResourceId("resource.2"), "position"}},
      ActiveControlConfigurationId(Digest('0')),
      ActiveControlConfigurationGeneration(11U),
      {{RuntimeUnitId("runtime.1"), RuntimeEpoch("epoch.1")}},
      StopEvidencePolicyId("stop.evidence"),
      StopEvidencePolicyRevision(Digest('1')),
      std::move(stop_plan),
      std::move(payload),
      std::nullopt};
}

TEST(PreparedExecutionTest, PrepareResultPreservesExactParticipantIdentity) {
  const auto participant = PreparedParticipant();
  const auto result =
      MakePrepareParticipantResult(participant, PrepareParticipantResultStateDTO::kPrepared);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(prepare_participant_result_field_error(result.value(), participant).has_value());
  EXPECT_EQ(result.value().prepared_contract_digest,
            std::optional(participant.prepared_contract_digest));
  EXPECT_EQ(result.value().payload_kind, PreparedParticipantPayloadKindDTO::kCapabilityCommand);

  auto forged = result.value();
  forged.control_route_id = ControlRouteId("route.other");
  EXPECT_EQ(prepare_participant_result_field_error(forged, participant),
            "prepare_participant_result_identity_invalid");
  forged = result.value();
  forged.error = ErrorDTO{std::string(kErrorExecutionRejected), "unexpected", {}};
  EXPECT_EQ(prepare_participant_result_field_error(forged, participant),
            "prepare_participant_result_presence_invalid");
}

TEST(PreparedExecutionTest, RejectedPrepareRequiresStableErrorAndNoDigest) {
  const auto participant = PreparedParticipant();
  const auto error = ErrorDTO{std::string(kErrorExecutionRejected),
                              "prepared participant rejected",
                              {{"reason", "prepared_contract_conflict"}}};
  const auto result =
      MakePrepareParticipantResult(participant, PrepareParticipantResultStateDTO::kRejected, error);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(result.value().prepared_contract_digest.has_value());
  EXPECT_EQ(result.value().error, std::optional(error));

  EXPECT_FALSE(
      MakePrepareParticipantResult(participant, PrepareParticipantResultStateDTO::kRejected).ok());
  EXPECT_FALSE(
      MakePrepareParticipantResult(participant, PrepareParticipantResultStateDTO::kPrepared, error)
          .ok());
}

TEST(PreparedExecutionTest, DispatchHandleRequiresExactPreparedGoalMembership) {
  const auto participant = PreparedParticipant();
  const ControllerGoalId goal_id("goal.1");
  const ControllerGoalGeneration goal_generation(7U);
  const auto wait = [](std::int64_t) { return true; };
  const auto snapshot = [] { return ExecutionSnapshot{}; };
  ExecutionHandle handle{wait,    snapshot,        0U,
                         goal_id, goal_generation, participant.prepared_contract_digest};
  EXPECT_FALSE(prepared_execution_handle_field_error(handle, participant, goal_id, goal_generation)
                   .has_value());

  auto wrong_goal = handle;
  wrong_goal.controller_goal_id = ControllerGoalId("goal.other");
  EXPECT_EQ(
      prepared_execution_handle_field_error(wrong_goal, participant, goal_id, goal_generation),
      "prepared_execution_handle_membership_mismatch");

  auto wrong_generation = handle;
  wrong_generation.controller_goal_generation = ControllerGoalGeneration(8U);
  EXPECT_EQ(prepared_execution_handle_field_error(wrong_generation, participant, goal_id,
                                                  goal_generation),
            "prepared_execution_handle_membership_mismatch");

  auto wrong_digest = handle;
  wrong_digest.prepared_contract_digest = ContentDigest(Digest('f'));
  EXPECT_EQ(
      prepared_execution_handle_field_error(wrong_digest, participant, goal_id, goal_generation),
      "prepared_execution_handle_membership_mismatch");

  auto missing_observer = handle;
  missing_observer.get_snapshot = {};
  EXPECT_EQ(prepared_execution_handle_field_error(missing_observer, participant, goal_id,
                                                  goal_generation),
            "prepared_execution_handle_observer_missing");
}

TEST(PreparedExecutionTest, BackendStatusIsGoalAndBindingFenced) {
  const auto participant = PreparedParticipant();
  const ControllerGoalId goal_id("goal.1");
  const ControllerGoalGeneration goal_generation(7U);
  const auto result = MakeExecutionBackendStatus(participant, goal_id, goal_generation,
                                                 ExecutionBackendStateDTO::kExecuting, 0.25, 0.4,
                                                 12U, 50'000'000LL);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_FALSE(
      execution_backend_status_field_error(result.value(), participant, goal_id, goal_generation)
          .has_value());

  auto forged = result.value();
  forged.controller_goal_generation = ControllerGoalGeneration(8U);
  EXPECT_EQ(execution_backend_status_field_error(forged, participant, goal_id, goal_generation),
            "execution_backend_status_identity_invalid");
  forged = result.value();
  forged.resources = {ResourceId("resource.1")};
  EXPECT_EQ(execution_backend_status_field_error(forged, participant, goal_id, goal_generation),
            "execution_backend_status_identity_invalid");
}

TEST(PreparedExecutionTest, BackendStatusEnforcesStateAndEvidencePresence) {
  const auto participant = PreparedParticipant();
  const ControllerGoalId goal_id("goal.1");
  const ControllerGoalGeneration goal_generation(7U);
  const auto error =
      ErrorDTO{std::string(kErrorExecutionAborted), "controller execution failed", {}};
  EXPECT_TRUE(MakeExecutionBackendStatus(participant, goal_id, goal_generation,
                                         ExecutionBackendStateDTO::kFailed, 0.5, 0.5, 13U,
                                         60'000'000LL, error)
                  .ok());
  EXPECT_FALSE(MakeExecutionBackendStatus(participant, goal_id, goal_generation,
                                          ExecutionBackendStateDTO::kFailed, 0.5, 0.5, 13U,
                                          60'000'000LL)
                   .ok());
  EXPECT_FALSE(MakeExecutionBackendStatus(participant, goal_id, goal_generation,
                                          ExecutionBackendStateDTO::kCompleted, 0.5, 1.0, 14U,
                                          70'000'000LL, error)
                   .ok());
  EXPECT_FALSE(MakeExecutionBackendStatus(participant, goal_id, goal_generation,
                                          ExecutionBackendStateDTO::kExecuting, 0.5, 1.1, 15U,
                                          80'000'000LL)
                   .ok());
}

TEST(PreparedExecutionMembershipTest, RetiredRequestsNeverReopenOrClearANewerExecution) {
  PreparedExecutionMembership membership;
  auto participant = PreparedParticipant();
  EXPECT_TRUE(membership.prepare(0U, participant));
  ASSERT_FALSE(membership.prepare(1U, participant));
  EXPECT_TRUE(membership.matches(participant));
  EXPECT_FALSE(membership.prepare(1U, participant));
  auto conflict = participant;
  conflict.prepared_contract_digest = ContentDigest(Digest('b'));
  EXPECT_TRUE(membership.prepare(1U, conflict));
  EXPECT_TRUE(membership.prepare(2U, participant));
  EXPECT_FALSE(membership.retire(2U));
  EXPECT_TRUE(membership.matches(participant));
  ASSERT_TRUE(membership.retire(1U));
  EXPECT_FALSE(membership.matches(participant));
  EXPECT_TRUE(membership.prepare(1U, participant));
  ASSERT_FALSE(membership.prepare(2U, participant));
  EXPECT_TRUE(membership.retire(1U));
  EXPECT_TRUE(membership.matches(participant));
  EXPECT_EQ(membership.active_execution_id(), 2U);
}

TEST(PreparedExecutionMembershipTest, RepeatedExecutionsAndResumesUseOneCurrentEntry) {
  PreparedExecutionMembership membership;
  auto participant = PreparedParticipant();
  for (std::uint64_t execution = 1U; execution <= 8192U; ++execution) {
    ASSERT_FALSE(membership.prepare(execution, participant));
    ASSERT_TRUE(membership.matches(participant));
    ASSERT_TRUE(membership.retire(execution));
    ASSERT_FALSE(membership.active_execution_id());
  }
  ASSERT_FALSE(membership.prepare(8193U, participant));
  for (std::uint64_t revision = 4U; revision <= 8192U; ++revision) {
    const auto prior = participant;
    participant.operation_revision = OperationRevision(revision);
    ASSERT_FALSE(membership.prepare(8193U, participant));
    ASSERT_TRUE(membership.prepare(8193U, prior));
    ASSERT_TRUE(membership.matches(participant));
  }
  ASSERT_TRUE(membership.retire(8193U));
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  ASSERT_FALSE(membership.prepare(maximum, participant));
  ASSERT_TRUE(membership.retire(maximum));
  EXPECT_TRUE(membership.prepare(maximum, participant));
  EXPECT_TRUE(membership.prepare(0U, participant));
}

TEST(PreparedExecutionMembershipTest, ClosingCancelledPreparationFencesItsDelayedArrival) {
  PreparedExecutionMembership membership;
  auto participant = PreparedParticipant();
  ASSERT_TRUE(membership.retire(7U));
  EXPECT_TRUE(membership.prepare(7U, participant));
  ASSERT_FALSE(membership.prepare(8U, participant));
  auto other = participant;
  other.operation_id = OperationId("another.operation");
  EXPECT_TRUE(membership.prepare(8U, other));
  EXPECT_FALSE(membership.retire(9U));
  EXPECT_TRUE(membership.matches(participant));
  ASSERT_TRUE(membership.retire(8U));
  EXPECT_FALSE(membership.prepare(9U, other));
}

}  // namespace
}  // namespace manyforge::core
