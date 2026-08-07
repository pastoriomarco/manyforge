#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace manyforge::core;

class ScopedTemporaryDirectory final {
 public:
  ScopedTemporaryDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-operation-journal-test-" +
             std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
             std::to_string(next.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create operation journal test root");
    }
  }

  ~ScopedTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] OperationIntentDTO CommandIntent(std::string request_id, std::string owner_epoch) {
  return OperationIntentDTO{
      PublicRequestContextDTO{RequestId(std::move(request_id)),
                              RequestOwnerEpoch(std::move(owner_epoch)), "journal-test",
                              std::nullopt},
      ProgramId("program.pick"),
      ProgramRevision("program-revision"),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      GraphRevision("graph-revision"),
      RetainedBundleId("retained-bundle"),
      std::nullopt,
      {OperationParticipantIntentDTO{
          ComponentRoleId("robot"), TargetRoleId("gripper"), CapabilityId("tool.gripper.command"),
          std::nullopt,
          CapabilityCommandIntentDTO{
              CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
              CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
              GripperPositionCommandValueDTO{LinearPositionDTO{0.02}, LinearEffortDTO{8.0}},
              2.0,
              {CommandDurationOptionDTO{CommandOptionId("command_duration_s"), 0.5}}}}},
      OperationCoordinationPolicy::kIndependent};
}

[[nodiscard]] OperationStateDTO OperationState(std::string operation_id, OperationStateKind kind,
                                               std::int64_t updated_at_ns) {
  const OperationId id(std::move(operation_id));
  return OperationStateDTO{
      id,
      OperationRevision(1U),
      ProgramBindingId("program-binding"),
      ProgramRunId("run.1"),
      OperationCoordinationPolicy::kIndependent,
      kind,
      {OperationParticipantStateDTO{OperationParticipantId(id.value() + ":participant:1"),
                                    ControllerGoalId(id.value() + ":controller-goal:1"),
                                    ControllerGoalGeneration(1U),
                                    ControllableComponentId("robot.1"),
                                    CommandTargetId("gripper"),
                                    CapabilityId("tool.gripper.command"),
                                    ControlDomainId("domain.gripper"),
                                    ControlDomainRevision("domain-revision"),
                                    ControlBindingId("binding.gripper"),
                                    ControlBindingRevision("binding-revision"),
                                    std::nullopt,
                                    std::nullopt,
                                    OperationParticipantPayloadKind::kCapabilityCommand,
                                    kind,
                                    kind == OperationStateKind::kCompleted ? 1.0 : 0.25,
                                    {ResourceId("robot.gripper")},
                                    std::nullopt}},
      updated_at_ns,
      std::nullopt,
      std::nullopt,
      std::nullopt};
}

[[nodiscard]] OperationAcceptedDTO Acceptance(const OperationIntentDTO& intent,
                                              const OperationStateDTO& state) {
  return OperationAcceptedDTO{
      intent.request_context.request_id,
      RequestReplayStatusDTO::kAcceptedNew,
      state.operation_id,
      state.operation_revision,
      {OperationParticipantAssignmentDTO{0U, state.participants.front().participant_id}},
      state.updated_at_ns};
}

TEST(SqliteOperationJournalTest,
     ReplacementRetainsHistoryAndFencesOnlyPreviouslyNonterminalResources) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto intent = CommandIntent("request.command.1", first_epoch.value());
  const auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    const auto begun = journal.BeginOwnerEpoch(first_epoch, 1, 4U, inventory);
    ASSERT_TRUE(begun.ok()) << begun.error().message;
    EXPECT_TRUE(begun.value().historical_operations.empty());
    const auto recorded =
        journal.RecordOperationAccepted(first_epoch, intent, Acceptance(intent, state), state);
    ASSERT_TRUE(recorded.ok()) << recorded.error().message;
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.2"), 20, 4U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  const auto& historical = recovered.value().historical_operations.front();
  EXPECT_EQ(historical.operation_id, state.operation_id);
  EXPECT_EQ(historical.state, OperationStateKind::kInterventionRequired);
  EXPECT_EQ(historical.updated_at_ns, 20);
  ASSERT_TRUE(historical.participants.front().terminal_error.has_value());
  EXPECT_EQ(historical.participants.front().terminal_error->details.at("reason"),
            "unreconciled_external_or_orphan_goal");
  EXPECT_EQ(recovered.value().unreconciled_resources, inventory);
  replacement.Close();

  SqliteOperationJournal second_replacement(database, cell_id, 8U);
  const auto recovered_again = second_replacement.BeginOwnerEpoch(
      RequestOwnerEpoch("execution-host.epoch.3"), 30, 4U, inventory);
  ASSERT_TRUE(recovered_again.ok()) << recovered_again.error().message;
  ASSERT_EQ(recovered_again.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered_again.value().historical_operations.front(), historical);
  EXPECT_EQ(recovered_again.value().unreconciled_resources, inventory);
}

TEST(SqliteOperationJournalTest, TerminalHistoryDoesNotFenceResourcesAndOwnerEpochsCannotBeReused) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto intent = CommandIntent("request.command.1", first_epoch.value());
  auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(first_epoch, 1, 4U, inventory).ok());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(first_epoch, intent, Acceptance(intent, state), state)
            .ok());
    state.state = OperationStateKind::kCompleted;
    state.participants.front().state = OperationStateKind::kCompleted;
    state.participants.front().progress = 1.0;
    state.updated_at_ns = 15;
    ASSERT_TRUE(
        journal.RecordOperationState(first_epoch, state, nullptr, "subordinate_completed").ok());
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.2"), 20, 4U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front().state, OperationStateKind::kCompleted);
  EXPECT_TRUE(recovered.value().unreconciled_resources.empty());
  const auto reused = replacement.BeginOwnerEpoch(first_epoch, 30, 4U, inventory);
  ASSERT_FALSE(reused.ok());
  EXPECT_EQ(reused.error().code, "operation_journal_owner_epoch_reused");
}

TEST(SqliteOperationJournalTest, TypedControlScaleAndStopRecordsRoundTripAsTerminalHistory) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto intent = CommandIntent("request.command.1", first_epoch.value());
  auto state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  state.runtime_scale_generation = RuntimeScaleGeneration(0U);

  {
    SqliteOperationJournal journal(database, cell_id, 8U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(first_epoch, 1, 8U, inventory).ok());
    ASSERT_TRUE(
        journal.RecordOperationAccepted(first_epoch, intent, Acceptance(intent, state), state)
            .ok());

    const OperationControlRequestDTO pause_request{
        PublicRequestContextDTO{RequestId("request.pause.1"), first_epoch, "journal-test",
                                std::nullopt},
        state.operation_id, state.operation_revision, std::nullopt};
    const auto prior_revision = state.operation_revision;
    state.operation_revision = OperationRevision(2U);
    state.updated_at_ns = 11;
    const OperationControlAcceptedDTO pause_accepted{pause_request.request_context.request_id,
                                                     RequestReplayStatusDTO::kAcceptedNew,
                                                     state.operation_id,
                                                     prior_revision,
                                                     OperationControlKind::kPause,
                                                     state.operation_revision,
                                                     state.updated_at_ns};
    ASSERT_TRUE(
        journal.RecordControlAccepted(first_epoch, pause_request, pause_accepted, state).ok());

    const RuntimeScaleRequestDTO scale_request{
        PublicRequestContextDTO{RequestId("request.scale.1"), first_epoch, "journal-test",
                                std::nullopt},
        state.operation_id, state.operation_revision, RuntimeScaleGeneration(0U), 0.5};
    state.runtime_scale_generation = RuntimeScaleGeneration(1U);
    state.updated_at_ns = 12;
    const RuntimeScaleAcceptedDTO scale_accepted{scale_request.request_context.request_id,
                                                 RequestReplayStatusDTO::kAcceptedNew,
                                                 state.operation_id,
                                                 state.operation_revision,
                                                 scale_request.scale,
                                                 *state.runtime_scale_generation,
                                                 state.updated_at_ns};
    const auto& participant = state.participants.front();
    const RuntimeScaleStatusDTO scale_status{
        state.operation_id,
        state.operation_revision,
        *state.runtime_scale_generation,
        scale_request.scale,
        scale_request.scale,
        {RuntimeScaleParticipantStatusDTO{participant.participant_id,
                                          participant.controllable_component_id,
                                          participant.command_target_id,
                                          participant.capability_id,
                                          participant.control_domain_id,
                                          participant.control_domain_revision,
                                          participant.control_binding_id,
                                          participant.control_binding_revision,
                                          participant.controller_goal_id,
                                          participant.controller_goal_generation,
                                          ControllerTopologyId("topology.1"),
                                          TopologyRevision("topology-revision"),
                                          ControlRouteId("route.1"),
                                          ScalingDomainId("scale.1"),
                                          std::nullopt,
                                          participant.resources,
                                          ActiveControlConfigurationId("configuration.1"),
                                          ActiveControlConfigurationGeneration(1U),
                                          *state.runtime_scale_generation,
                                          scale_request.scale,
                                          scale_request.scale,
                                          0.0,
                                          RuntimeEpoch("runtime.epoch.1"),
                                          1U,
                                          state.updated_at_ns,
                                          true,
                                          RuntimeScaleApplicationState::kApplied,
                                          std::nullopt}},
        RuntimeScaleApplicationState::kApplied,
        state.updated_at_ns};
    ASSERT_TRUE(journal
                    .RecordRuntimeScaleAccepted(first_epoch, scale_request, scale_accepted, state,
                                                scale_status)
                    .ok());

    const StopIntentDTO stop_request{
        PublicRequestContextDTO{RequestId("request.stop.1"), first_epoch, "journal-test",
                                std::nullopt},
        {StopOperationTargetDTO{state.operation_id, state.operation_revision, StopGeneration(0U)}},
        std::nullopt};
    state.operation_revision = OperationRevision(3U);
    state.state = OperationStateKind::kStopping;
    state.participants.front().state = OperationStateKind::kStopping;
    state.active_stop_id = StopId("stop:execution-host.epoch.1:1");
    state.active_stop_generation = StopGeneration(1U);
    state.updated_at_ns = 13;
    const StopAcceptedDTO stop_accepted{stop_request.request_context.request_id,
                                        RequestReplayStatusDTO::kAcceptedNew,
                                        *state.active_stop_id,
                                        *state.active_stop_generation,
                                        {state.operation_id},
                                        state.operation_revision,
                                        state.updated_at_ns};
    ASSERT_TRUE(journal.RecordStopAccepted(first_epoch, stop_request, stop_accepted, state).ok());

    state.state = OperationStateKind::kStopped;
    state.participants.front().state = OperationStateKind::kStopped;
    state.updated_at_ns = 14;
    ASSERT_TRUE(
        journal.RecordOperationState(first_epoch, state, nullptr, "subordinate_stopped").ok());
    journal.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 8U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.2"), 20, 8U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
  EXPECT_EQ(recovered.value().historical_operations.front(), state);
  EXPECT_TRUE(recovered.value().unreconciled_resources.empty());
}

TEST(SqliteOperationJournalTest, UnresolvedOrphanFenceSurvivesBoundedHistoryCompaction) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper"), ResourceId("robot.arm")};
  const RequestOwnerEpoch first_epoch("execution-host.epoch.1");
  const auto orphan_intent = CommandIntent("request.command.1", first_epoch.value());
  const auto orphan_state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  {
    SqliteOperationJournal journal(database, cell_id, 1U);
    ASSERT_TRUE(journal.BeginOwnerEpoch(first_epoch, 1, 4U, inventory).ok());
    ASSERT_TRUE(journal
                    .RecordOperationAccepted(first_epoch, orphan_intent,
                                             Acceptance(orphan_intent, orphan_state), orphan_state)
                    .ok());
    journal.Close();
  }

  const RequestOwnerEpoch second_epoch("execution-host.epoch.2");
  {
    SqliteOperationJournal replacement(database, cell_id, 1U);
    const auto recovered = replacement.BeginOwnerEpoch(second_epoch, 20, 4U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    EXPECT_EQ(recovered.value().unreconciled_resources,
              std::vector<ResourceId>{ResourceId("robot.gripper")});

    const auto terminal_intent = CommandIntent("request.command.2", second_epoch.value());
    auto terminal_state =
        OperationState("operation:execution-host.epoch.2:1", OperationStateKind::kExecuting, 21);
    terminal_state.participants.front().resources = {ResourceId("robot.arm")};
    ASSERT_TRUE(replacement
                    .RecordOperationAccepted(second_epoch, terminal_intent,
                                             Acceptance(terminal_intent, terminal_state),
                                             terminal_state)
                    .ok());
    terminal_state.state = OperationStateKind::kCompleted;
    terminal_state.participants.front().state = OperationStateKind::kCompleted;
    terminal_state.participants.front().progress = 1.0;
    terminal_state.updated_at_ns = 22;
    ASSERT_TRUE(
        replacement
            .RecordOperationState(second_epoch, terminal_state, nullptr, "subordinate_completed")
            .ok());
    replacement.Close();
  }

  const RequestOwnerEpoch third_epoch("execution-host.epoch.3");
  {
    SqliteOperationJournal replacement(database, cell_id, 1U);
    const auto recovered = replacement.BeginOwnerEpoch(third_epoch, 30, 4U, inventory);
    ASSERT_TRUE(recovered.ok()) << recovered.error().message;
    ASSERT_EQ(recovered.value().historical_operations.size(), 1U);
    EXPECT_EQ(recovered.value().historical_operations.front().operation_id,
              orphan_state.operation_id);
    EXPECT_EQ(recovered.value().unreconciled_resources,
              std::vector<ResourceId>{ResourceId("robot.gripper")});
    replacement.Close();
  }

  SqliteOperationJournal replacement(database, cell_id, 1U);
  const auto recovered =
      replacement.BeginOwnerEpoch(RequestOwnerEpoch("execution-host.epoch.4"), 40, 4U, inventory);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().unreconciled_resources,
            std::vector<ResourceId>{ResourceId("robot.gripper")});
}

TEST(SqliteOperationJournalTest, RequestCapacityAndFilesystemIdentityFailClosedBeforeMutation) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const CellId cell_id("cell.1");
  const std::vector<ResourceId> inventory{ResourceId("robot.gripper")};
  const RequestOwnerEpoch epoch("execution-host.epoch.1");
  SqliteOperationJournal journal(database, cell_id, 4U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(epoch, 1, 1U, inventory).ok());
  const auto first_intent = CommandIntent("request.command.1", epoch.value());
  const auto first_state =
      OperationState("operation:execution-host.epoch.1:1", OperationStateKind::kExecuting, 10);
  ASSERT_TRUE(journal
                  .RecordOperationAccepted(epoch, first_intent,
                                           Acceptance(first_intent, first_state), first_state)
                  .ok());
  const auto second_intent = CommandIntent("request.command.2", epoch.value());
  const auto second_state =
      OperationState("operation:execution-host.epoch.1:2", OperationStateKind::kExecuting, 11);
  const auto full = journal.RecordOperationAccepted(
      epoch, second_intent, Acceptance(second_intent, second_state), second_state);
  ASSERT_FALSE(full.ok());
  EXPECT_EQ(full.error().code, "request_ledger_full");
  journal.Close();

  const auto symlink = temporary.path() / "linked-journal.sqlite3";
  std::filesystem::create_symlink(database, symlink);
  ASSERT_TRUE(std::filesystem::is_symlink(symlink));
  EXPECT_THROW(SqliteOperationJournal(symlink, cell_id, 4U), std::runtime_error);
  EXPECT_THROW(SqliteOperationJournal(database, CellId("another-cell"), 4U), std::runtime_error);
}

}  // namespace
