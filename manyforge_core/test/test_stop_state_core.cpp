// Stop core of the stop state model (design 2026-11-13 §3.1, §5.4, §8,
// §11.3): duplicate recognition, invalid-Stop widening (PD-1), the ring's
// read (K16, K19) and coverage of a moving operation. FakeClock, fake engines.

#include <manyforge_core/testing/request_key_fixture.hpp>
#include "resource_addressed_kernel_test_fixture.hpp"

// Allocation counting (§9.2 test) uses the fixture's counting operator new:
// `AllocationsDuring` counts this thread's allocations of exactly the call.

namespace manyforge::core {
namespace {

[[nodiscard]] FullStopIntentDTO full(const char* key) {
  return FullStopIntentDTO{{TestRequestKey(key), "test"}, {}};
}

struct CoreHarness final {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  std::unique_ptr<ResourceAddressedExecutionKernel> owner{make_claimed_kernel(
      {registration(make_binding("arm", "execution.trajectory.execute", CoordinateKind::kAngular),
                    engine)},
      clock, scheduler, journal, {kTestApplicationStartId, CellId("cell.test"), 8U})};
  ResourceAddressedExecutionKernel& kernel{*owner};
};

TEST(StopStateCoreTest, StopAllWithNothingMovingHasNoEffectAndReplaysExactly) {
  CoreHarness harness;
  const auto first = harness.kernel.stop_all(full("stop.all.1"));
  EXPECT_EQ(first.replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(first.requested, StopKind::kStopAll);
  EXPECT_EQ(first.applied, StopKind::kStopAll);
  EXPECT_EQ(first.widen_reason, StopWidenReason::kNone);
  EXPECT_TRUE(first.covered.empty());
  const auto status = harness.kernel.read_stop(first.event_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_TRUE(status.value().complete);
  EXPECT_EQ(status.value().result, StopEventResult::kNoEffect);

  const auto repeat = harness.kernel.stop_all(full("stop.all.1"));
  EXPECT_EQ(repeat.replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(repeat.event_id, first.event_id);
}

TEST(StopStateCoreTest, KeyReuseWithOtherContentWidensAndIsRecordedUnderTheKey) {
  CoreHarness harness;
  const auto first = harness.kernel.stop_all(full("stop.key"));
  const auto conflict = harness.kernel.cell_stop(full("stop.key"));
  EXPECT_EQ(conflict.replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_NE(conflict.event_id, first.event_id);
  EXPECT_EQ(conflict.requested, StopKind::kCellStop);
  EXPECT_EQ(conflict.applied, StopKind::kStopAll);
  EXPECT_EQ(conflict.widen_reason, StopWidenReason::kKeyConflict);
  // An exact resend of the conflicting request gets its widened answer.
  const auto resend = harness.kernel.cell_stop(full("stop.key"));
  EXPECT_EQ(resend.replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(resend.event_id, conflict.event_id);
}

TEST(StopStateCoreTest, RecognizedStopsWidenByRecognisedRoute) {
  CoreHarness harness;
  const auto stops = harness.kernel.recognized_stop(RecognizedStopIngressDTO{
      TestRequestKey("malformed.1"), {}, "/api/execution/stops", "recognized_stop_payload_invalid"});
  EXPECT_EQ(stops.applied, StopKind::kStopAll);
  EXPECT_EQ(stops.widen_reason, StopWidenReason::kMalformed);
  const auto halt = harness.kernel.recognized_stop(RecognizedStopIngressDTO{
      TestRequestKey("malformed.2"), {}, "/api/execution/halt", "recognized_stop_payload_invalid"});
  EXPECT_EQ(halt.applied, StopKind::kHalt);
  const auto unknown = harness.kernel.recognized_stop(RecognizedStopIngressDTO{
      TestRequestKey("malformed.3"), {}, "/api/other", "recognized_stop_payload_invalid"});
  EXPECT_EQ(unknown.applied, StopKind::kHalt);
  EXPECT_EQ(unknown.widen_reason, StopWidenReason::kUnreadableKind);
  const auto oversized = harness.kernel.recognized_stop(RecognizedStopIngressDTO{
      TestRequestKey("malformed.4"), {}, "/api/execution/stop-all",
      "recognized_stop_body_oversized"});
  EXPECT_EQ(oversized.requested, StopKind::kStopAll);
  EXPECT_EQ(oversized.applied, StopKind::kStopAll);
  EXPECT_EQ(oversized.widen_reason, StopWidenReason::kMalformed);
}

TEST(StopStateCoreTest, StopOfAnotherStartIsRecordedAsStaleStartAndReplaysExactly) {
  CoreHarness harness;
  const RecognizedStopIngressDTO ingress{TestRequestKey("stale.1"), {}, "/api/execution/stops",
                                         "recognized_stop_start_not_current",
                                         std::string("operator_ui")};
  const auto first = harness.kernel.recognized_stop(ingress);
  EXPECT_EQ(first.replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(first.requested, StopKind::kOperation);
  EXPECT_EQ(first.applied, StopKind::kStopAll);
  EXPECT_EQ(first.widen_reason, StopWidenReason::kStaleStart);
  const auto status = harness.kernel.read_stop(first.event_id);
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_EQ(status.value().widen_reason, StopWidenReason::kStaleStart);

  // The same key and request is its exact replay, with the kept answer.
  const auto repeat = harness.kernel.recognized_stop(ingress);
  EXPECT_EQ(repeat.replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(repeat.event_id, first.event_id);
  EXPECT_EQ(repeat.widen_reason, StopWidenReason::kStaleStart);
}

TEST(StopStateCoreTest, UnknownOperationTargetWidensToStopAll) {
  CoreHarness harness;
  const auto stopped = harness.kernel.stop(StopIntentDTO{
      {TestRequestKey("stop.unknown"), "test"},
      std::vector<StopOperationTargetDTO>{{OperationNumber(4242U), OperationRevision(1U)}},
      std::nullopt});
  EXPECT_EQ(stopped.requested, StopKind::kOperation);
  EXPECT_EQ(stopped.applied, StopKind::kStopAll);
  EXPECT_EQ(stopped.widen_reason, StopWidenReason::kUnknownTarget);
}

TEST(StopStateCoreTest, ReadStopAnswersNotResidentOnceOverwritten) {
  CoreHarness harness;  // R = 8
  const auto first = harness.kernel.stop_all(full("ring.0"));
  for (int index = 1; index <= 8; ++index)
    static_cast<void>(harness.kernel.stop_all(full(("ring." + std::to_string(index)).c_str())));
  const auto status = harness.kernel.read_stop(first.event_id);
  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.error().details.at("reason"), "stop_status_not_resident");
  // A resend after its record was overwritten is simply a new Stop.
  const auto resend = harness.kernel.stop_all(full("ring.0"));
  EXPECT_EQ(resend.replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_NE(resend.event_id, first.event_id);
}

TEST(StopStateCoreTest, CellStopActionNamesTheNewestEvent) {
  CoreHarness harness;
  EXPECT_FALSE(harness.kernel.cell_stop_action_projection().last_event_id.has_value());
  const auto stopped = harness.kernel.halt(full("halt.1"));
  const auto projection = harness.kernel.cell_stop_action_projection();
  ASSERT_TRUE(projection.last_event_id.has_value());
  EXPECT_EQ(*projection.last_event_id, stopped.event_id);
  ASSERT_TRUE(projection.last_completed.has_value());
  EXPECT_EQ(projection.last_completed->event_id, stopped.event_id);
  EXPECT_EQ(projection.last_completed->result, StopEventResult::kNoEffect);
}

TEST(StopStateCoreTest, OperationStopStartsTheMovingParticipantAndOwnsTheOperation) {
  CoreHarness harness;
  const auto operation = harness.kernel.execute(motion_operation("request.core.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto before = harness.kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(before.ok());
  const auto stopped = harness.kernel.stop(
      stop_request("request.core.stop", operation.value(), before.value().operation_revision.value()));
  EXPECT_EQ(stopped.applied, StopKind::kOperation);
  ASSERT_EQ(stopped.covered.size(), 1U);
  EXPECT_TRUE(stopped.covered.front().started);
  EXPECT_EQ(harness.engine.addressed_stop_fence_calls.load(), 1);
  const auto after = harness.kernel.get_operation_state(operation.value().operation_id);
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value().stop_event_id, stopped.event_id);
  EXPECT_GT(after.value().operation_revision, before.value().operation_revision);
  // A second Stop covers the stopping participant without restarting it.
  const auto again = harness.kernel.stop(
      stop_request("request.core.stop.2", operation.value(), after.value().operation_revision.value()));
  ASSERT_EQ(again.covered.size(), 1U);
  EXPECT_FALSE(again.covered.front().started);
  EXPECT_EQ(harness.engine.addressed_stop_fence_calls.load(), 1);
}


// ---------------------------------------------------------------------------
// Follow-up tests (coordinator 2026-11-13).
// ---------------------------------------------------------------------------

[[nodiscard]] StopRequest cell_request_of(const StopKind kind, const RequestKey key) {
  StopRequest request;
  request.request_key = key;
  request.kind = kind;
  request.selector = StopSelectorKind::kCell;
  request.source_label = "test";
  request.attribution_rank.attribution_class = StopAttributionClass::kOperator;
  return request;
}

struct TwoRobotHarness final {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  RuntimeParticipantBinding first =
      make_binding("arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  std::unique_ptr<ResourceAddressedExecutionKernel> owner{make_claimed_kernel(
      {registration(first, first_engine), registration(coordinated_occurrence(first, "2"), second_engine)},
      clock, scheduler, journal, {kTestApplicationStartId, CellId("cell.test"), 8U})};
  ResourceAddressedExecutionKernel& kernel{*owner};

  /// One single-robot motion on robot `suffix` ("1": the first robot).
  [[nodiscard]] OperationIntentDTO single(const char* request, const char* suffix) {
    auto operation = motion_operation(request);
    if (std::string(suffix) != "1") {
      const auto coordinated = coordinated_motion_operation(std::string(request) + ".shape", 2U);
      operation.participants.front() = coordinated.participants.back();
      scope_default_program_participant(operation.participants.front());
    }
    return operation;
  }
};

[[nodiscard]] bool wait_phase(ResourceAddressedExecutionKernel& kernel, const OperationNumber& id,
                              const std::size_t ordinal, const StopPhase phase) {
  return wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(kernel);
    return StopControlExecutorTestAccess::ParticipantStop(kernel, id, ordinal).phase == phase;
  });
}

TEST(StopStateCoreTest, StopAcceptanceAllocatesNothing) {
  CoreHarness harness;
  const auto operation = harness.kernel.execute(motion_operation("request.alloc.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  const std::vector<StopOperationTargetDTO> targets{
      {operation.value().operation_id, state.operation_revision}};
  auto& handoff = StopControlExecutorTestAccess::ThreadHandoff(harness.kernel);  // sized first
  std::uint64_t sequence = 1U;
  for (const auto kind : {StopKind::kOperation, StopKind::kStopAll, StopKind::kHalt,
                          StopKind::kCellStop, StopKind::kGroups, StopKind::kComponentFault}) {
    auto request = cell_request_of(kind, TestRequestKey(("alloc." + std::to_string(sequence++)).c_str()));
    if (kind == StopKind::kOperation) {
      request.selector = StopSelectorKind::kOperations;
      request.operations = targets;
    } else if (kind == StopKind::kGroups) {
      request.selector = StopSelectorKind::kProgramGroups;
      request.program_run_id = ProgramRunNumber(1U);
    }
    std::size_t locked_allocations = 0U;
    static_cast<void>(StopControlExecutorTestAccess::RequestStopLocked(
        harness.kernel, request, handoff, locked_allocations));
    EXPECT_EQ(locked_allocations, 0U) << "kind " << to_string(kind);
    // After the unlock (§9.2): call (2), publish, signal, listener, worker
    // wake allocate nothing with the fake engine (its call (2) builds no
    // reason string).
    EXPECT_EQ(AllocationsDuring([&] {
                StopControlExecutorTestAccess::CompleteHandoff(harness.kernel, handoff);
              }),
              0U)
        << "after unlock, kind " << to_string(kind);
  }
}

TEST(StopStateCoreTest, FenceCallOneUnderTheLockAndCallTwoAfterTheUnlockBeforePublish) {
  CoreHarness harness;
  harness.engine.defer_prepared_dispatch_effect = true;
  const auto operation = harness.kernel.execute(motion_operation("request.fence.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  const std::vector<StopOperationTargetDTO> targets{
      {operation.value().operation_id, state.operation_revision}};
  auto request = cell_request_of(StopKind::kOperation, TestRequestKey("fence.1"));
  request.selector = StopSelectorKind::kOperations;
  request.operations = targets;
  auto& handoff = StopControlExecutorTestAccess::ThreadHandoff(harness.kernel);
  std::size_t locked_allocations = 0U;
  static_cast<void>(StopControlExecutorTestAccess::RequestStopLocked(harness.kernel, request,
                                                                     handoff, locked_allocations));
  // The lock is released: call (1) ran, call (2) has not, nothing published.
  EXPECT_EQ(harness.engine.addressed_stop_fence_calls.load(), 1);
  EXPECT_EQ(harness.engine.addressed_stop_fence_completions.load(), 0);
  EXPECT_EQ(harness.engine.translate_stop_stage_calls.load(), 0);
  StopControlExecutorTestAccess::CompleteHandoff(harness.kernel, handoff);
  EXPECT_EQ(harness.engine.addressed_stop_fence_completions.load(), 1);
  EXPECT_EQ(harness.engine.last_fence_completion_gates,
            static_cast<std::uint8_t>(AddressedParticipantStopFenceGate::kActiveSession));
}

TEST(StopStateCoreTest, StopBetweenInsertionAndDispatchDecisionDispatchesNothing) {
  CoreHarness harness;
  // The fake's prepared dispatch stays requested (not yet sent): the Stop's
  // call (1) closes its gate, so the participant is cancelled pre-dispatch.
  harness.engine.defer_prepared_dispatch_effect = true;
  const auto operation = harness.kernel.execute(motion_operation("request.predispatch.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  const auto stopped = harness.kernel.stop(stop_request(
      "request.predispatch.stop", operation.value(), state.operation_revision.value()));
  ASSERT_EQ(stopped.covered.size(), 1U);
  // The Stop lane settles the cancelled-before-dispatch participant on its own
  // thread: wait for the Stop to complete, then check the settled outcome
  // (the acceptance-time kCancelledPreDispatch is transient).
  ASSERT_TRUE(wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(harness.kernel);
    const auto status = harness.kernel.read_stop(stopped.event_id);
    return status.ok() && status.value().complete;
  }));
  EXPECT_EQ(harness.kernel.read_stop(stopped.event_id).value().result, StopEventResult::kStopped);
  const auto after = harness.kernel.get_operation_state(operation.value().operation_id).value();
  EXPECT_EQ(after.participants.front().state, OperationStateKind::kStopped);
  // Nothing was dispatched and no Stop stage was sent.
  EXPECT_EQ(harness.engine.stop_calls.load(), 0);
  EXPECT_EQ(harness.engine.translate_stop_stage_calls.load(), 0);
}

TEST(StopStateCoreTest, FenceDegradedRecordedNotPrinted) {
  CoreHarness harness;
  // The journal writer (which logs the degraded fence) is held in the batch
  // of the operation's rows, so the Stop's row waits until it is released.
  ASSERT_TRUE(StopControlExecutorTestAccess::DrainJournal(harness.kernel));
  std::size_t batches = 0U;
  {
    std::lock_guard lock(harness.journal.mutex);
    batches = harness.journal.batches.size();
  }
  harness.journal.Block();
  const auto operation = harness.kernel.execute(motion_operation("request.degraded.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  ASSERT_TRUE(harness.journal.WaitBatchesEntered(batches + 1U));
  // The slot can no longer be fenced by its recorded owner.
  auto slots = std::const_pointer_cast<ResourceSlotTable>(harness.kernel.resource_admission_table());
  const auto snapshot = slots->coherent_snapshot();
  const auto owned = std::find_if(snapshot.slots.begin(), snapshot.slots.end(),
                                  [](const auto& slot) { return slot.owner.has_value(); });
  ASSERT_NE(owned, snapshot.slots.end());
  ASSERT_TRUE(slots->transition(*owned->owner, {owned->resource_id}, ResourceSlotState::kStopping).ok());
  HeldSettlementCall stage;
  harness.engine.translate_stop_stage_hook = stage.Blocker();
  ::testing::internal::CaptureStderr();
  const auto stopped = harness.kernel.stop_all(full("request.degraded.stop"));
  const auto during = ::testing::internal::GetCapturedStderr();
  EXPECT_TRUE(during.empty()) << during;
  const auto record = StopControlExecutorTestAccess::Record(
      harness.kernel, static_cast<std::uint32_t>((stopped.event_id.value() - 1U) % 8U));
  EXPECT_TRUE(record.fence_degraded);
  ::testing::internal::CaptureStderr();
  harness.journal.Unblock();
  ASSERT_TRUE(StopControlExecutorTestAccess::DrainJournal(harness.kernel));
  const auto logged = ::testing::internal::GetCapturedStderr();
  EXPECT_NE(logged.find("code=stop_fence_degraded"), std::string::npos) << logged;
  stage.Release();
}

TEST(StopStateCoreTest, FreeListUnderflowFailsParticipantTyped) {
  CoreHarness harness;
  const auto operation = harness.kernel.execute(motion_operation("request.underflow.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  const auto free = StopControlExecutorTestAccess::ExhaustStopLanes(harness.kernel);
  const auto stopped = harness.kernel.stop(stop_request(
      "request.underflow.stop", operation.value(), state.operation_revision.value()));
  StopControlExecutorTestAccess::RestoreStopLanes(harness.kernel, free);
  ASSERT_EQ(stopped.covered.size(), 1U);
  EXPECT_TRUE(stopped.covered.front().started);
  const auto stop = StopControlExecutorTestAccess::ParticipantStop(
      harness.kernel, operation.value().operation_id, 0U);
  ASSERT_TRUE(stop.error.has_value());
  EXPECT_EQ(stop.error->code, "stop_lane_index_exhausted");
  const auto status = harness.kernel.read_stop(stopped.event_id);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.value().result, StopEventResult::kSomeFailed);
  // Its slots and its engine were fenced all the same (E1, E2).
  EXPECT_EQ(harness.engine.addressed_stop_fence_calls.load(), 1);
  EXPECT_EQ(harness.kernel.get_operation_state(operation.value().operation_id)
                .value()
                .stop_event_id,
            stopped.event_id);
}

TEST(StopStateCoreTest, RingOverwriteOfPendingRecordKeepsStopping) {
  CoreHarness harness;  // R = 8
  const auto operation = harness.kernel.execute(motion_operation("request.ring.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  HeldSettlementCall stage;
  harness.engine.translate_stop_stage_hook = stage.Blocker();
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  const auto stopped = harness.kernel.stop(
      stop_request("request.ring.stop", operation.value(), state.operation_revision.value()));
  ASSERT_TRUE(stage.Entered());
  for (int index = 0; index < 8; ++index)
    static_cast<void>(harness.kernel.cell_stop(full(("ring.more." + std::to_string(index)).c_str())));
  EXPECT_FALSE(harness.kernel.read_stop(stopped.event_id).ok());
  EXPECT_EQ(StopControlExecutorTestAccess::ParticipantStop(harness.kernel,
                                                           operation.value().operation_id, 0U)
                .phase,
            StopPhase::kStopping);
  stage.Release();
  EXPECT_TRUE(wait_phase(harness.kernel, operation.value().operation_id, 0U, StopPhase::kStopped));
}

TEST(StopStateCoreTest, LaneReuseDoesNotCompleteOlderEvent) {
  // Event E covers robots A and B; A ends while B is held; A's lane is
  // released (generation raised) and reused by a new Stop's start; E
  // completes only when B ends.
  TwoRobotHarness harness;
  const auto a = harness.kernel.execute(harness.single("request.lane.a", "1"));
  const auto b = harness.kernel.execute(harness.single("request.lane.b", "2"));
  ASSERT_TRUE(a.ok() && b.ok());
  HeldSettlementCall held_b;
  harness.second_engine.translate_stop_stage_hook = held_b.Blocker();
  const auto event = harness.kernel.stop_all(full("request.lane.e"));
  ASSERT_EQ(event.covered.size(), 2U);
  const auto lane_a =
      StopControlExecutorTestAccess::ParticipantStop(harness.kernel, a.value().operation_id, 0U);
  ASSERT_TRUE(wait_phase(harness.kernel, a.value().operation_id, 0U, StopPhase::kStopped));
  // A's index is free again under a new generation: a Stop of the same
  // operation (re-attempt is not taken: it is contained) and of B's cover.
  const auto pending = harness.kernel.read_stop(event.event_id);
  ASSERT_TRUE(pending.ok());
  EXPECT_FALSE(pending.value().complete);
  const auto later = harness.kernel.stop_all(full("request.lane.f"));
  EXPECT_FALSE(harness.kernel.read_stop(event.event_id).value().complete);
  held_b.Release();
  ASSERT_TRUE(wait_phase(harness.kernel, b.value().operation_id, 0U, StopPhase::kStopped));
  EXPECT_TRUE(harness.kernel.read_stop(event.event_id).value().complete);
  EXPECT_TRUE(harness.kernel.read_stop(later.event_id).value().complete);
  static_cast<void>(lane_a);
}

TEST(StopStateCoreTest, HardStageTimesOutAtKernelDeadline) {
  CoreHarness harness;
  const auto operation = harness.kernel.execute(motion_operation("request.hard.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  HeldSettlementCall stage;  // the port never reports
  harness.engine.translate_stop_stage_hook = stage.Blocker();
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  const auto stopped = harness.kernel.stop(
      stop_request("request.hard.stop", operation.value(), state.operation_revision.value()));
  ASSERT_TRUE(stage.Entered());
  // The plan's first stage is controlled: it fails at its own watchdog
  // (from the Stop's acceptance), and the ladder escalates to the last,
  // software-hard stage, whose Kernel deadline counts from its own start (P7).
  const auto* plan = StopControlExecutorTestAccess::ParticipantStop(
                         harness.kernel, operation.value().operation_id, 0U)
                         .plan;
  ASSERT_NE(plan, nullptr);
  ASSERT_EQ(plan->stage_commands.size(), 2U);
  const auto controlled_deadline = controlled_stage_deadline_ns(plan->stage_commands.front());
  ASSERT_TRUE(controlled_deadline.has_value());
  harness.clock.advance(controlled_stage_watchdog_ns(*controlled_deadline) + 1);
  ASSERT_TRUE(wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(harness.kernel);
    return harness.engine.translate_stop_stage_calls.load() >= 2;
  }));
  EXPECT_FALSE(harness.kernel.read_stop(stopped.event_id).value().complete);
  // The hard stage's port never reports either.
  harness.clock.advance(controlled_stage_watchdog_ns(kTestHardStopCompletionTimeoutNs) + 1);
  // The watchdog fails the attempt; the failed-Stop Halt then re-attempts the
  // participant in the same lock hold (attempt 2), so phase kFailed is not
  // observable here: the first event's outcome is.
  ASSERT_TRUE(wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(harness.kernel);
    const auto status = harness.kernel.read_stop(stopped.event_id);
    return status.ok() && status.value().complete;
  }));
  const auto status = harness.kernel.read_stop(stopped.event_id).value();
  EXPECT_EQ(status.result, StopEventResult::kSomeFailed);
  ASSERT_EQ(status.covered.size(), 1U);
  EXPECT_EQ(status.covered.front().phase, StopPhase::kFailed);
  EXPECT_EQ(StopControlExecutorTestAccess::ParticipantStop(harness.kernel,
                                                           operation.value().operation_id, 0U)
                .attempt,
            2U);
  stage.Release();
}

TEST(StopStateCoreTest, ApplyDuringStopLeavesStopWorkIntact) {
  CoreHarness harness;
  const auto operation = harness.kernel.execute(motion_operation("request.apply.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  HeldSettlementCall stage;
  harness.engine.translate_stop_stage_hook = stage.Blocker();
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  static_cast<void>(harness.kernel.stop(
      stop_request("request.apply.stop", operation.value(), state.operation_revision.value())));
  ASSERT_TRUE(stage.Entered());
  const auto before = StopControlExecutorTestAccess::ParticipantStop(
      harness.kernel, operation.value().operation_id, 0U);
  static_cast<void>(harness.kernel.reconfigure_runtime_participants(
      {registration(make_binding("arm", "execution.trajectory.execute", CoordinateKind::kAngular),
                    harness.engine)}));
  const auto after = StopControlExecutorTestAccess::ParticipantStop(
      harness.kernel, operation.value().operation_id, 0U);
  EXPECT_EQ(after.phase, StopPhase::kStopping);
  EXPECT_EQ(after.lane_index, before.lane_index);
  EXPECT_EQ(after.lane_generation, before.lane_generation);
  EXPECT_EQ(StopControlExecutorTestAccess::StoppingCount(harness.kernel), 1U);
  stage.Release();
  EXPECT_TRUE(wait_phase(harness.kernel, operation.value().operation_id, 0U, StopPhase::kStopped));
}

TEST(StopStateCoreTest, BootstrapEmptyCellThenTwoRobotApplyThenStopBoth) {
  // N_max is fixed at construction (64 in production), so a cell that grows
  // at Apply gets a lane index for each robot. The fixture cannot bootstrap a
  // truly empty cell (it needs empty-runtime product identities); it starts
  // from one robot with N_max = 64.
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine first_engine;
  FakeParticipantEngine second_engine;
  FakeOperationJournal journal;
  const auto first = make_binding("arm", "execution.trajectory.execute", CoordinateKind::kAngular);
  ResourceAddressedExecutionKernelConfig config{kTestApplicationStartId, CellId("cell.test"), 8U};
  config.prequalified_max_live_stop_participants = 64U;
  auto owner = make_claimed_kernel({registration(first, first_engine)}, clock, scheduler, journal,
                                   std::move(config));
  auto& kernel = *owner;
  const auto first_registration = registration(first, first_engine);
  const auto second_registration = registration(coordinated_occurrence(first, "2"), second_engine);
  ASSERT_TRUE(
      kernel.reconfigure_runtime_participants({first_registration, second_registration}).ok());
  // The added robot's new control domain admits work once its route is
  // published, and its component then reports enabled (as at an Apply).
  const auto published = kernel.publish_runtime_route_configuration(
      route_configuration_for(second_registration), {first_registration, second_registration});
  ASSERT_TRUE(published.ok()) << published.error().message;
  report_component_lifecycles(
      kernel, {{first.value().controllable_component_id, ComponentLifecycle::kEnabled},
               {second_registration.binding.value().controllable_component_id,
                ComponentLifecycle::kEnabled}});
  // One motion per robot (coordinated multi-motion is refused at admission).
  const auto first_operation = kernel.execute(motion_operation("request.boot.execute.1"));
  ASSERT_TRUE(first_operation.ok()) << first_operation.error().message;
  auto second_intent = motion_operation("request.boot.execute.2");
  second_intent.participants.front() =
      coordinated_motion_operation("request.boot.execute.2.shape", 2U).participants.back();
  scope_default_program_participant(second_intent.participants.front());
  const auto second_operation = kernel.execute(second_intent);
  ASSERT_TRUE(second_operation.ok()) << second_operation.error().message;
  const auto stopped = kernel.stop_all(full("request.boot.stop"));
  ASSERT_EQ(stopped.covered.size(), 2U);
  EXPECT_TRUE(stopped.covered[0].started);
  EXPECT_TRUE(stopped.covered[1].started);
  // The event record keeps each covered entry's lane index after its
  // participant ends (the participant's own index is released at P4).
  const auto record = StopControlExecutorTestAccess::Record(
      kernel, static_cast<std::uint32_t>((stopped.event_id.value() - 1U) % 8U));
  ASSERT_EQ(record.covered_count, 2U);
  EXPECT_NE(record.covered[0].lane_index, kNoStopLane);
  EXPECT_NE(record.covered[1].lane_index, kNoStopLane);
  EXPECT_NE(record.covered[0].lane_index, record.covered[1].lane_index);
}

TEST(StopStateCoreTest, FailedStopOnOneRobotHaltsTheOtherAndFreezesProgram) {
  TwoRobotHarness harness;
  const auto a = harness.kernel.execute(harness.single("request.failed.a", "1"));
  const auto b = harness.kernel.execute(harness.single("request.failed.b", "2"));
  ASSERT_TRUE(a.ok() && b.ok());
  harness.first_engine.next_stop_result = StopResultDTO{false, std::nullopt};
  const auto state = harness.kernel.get_operation_state(a.value().operation_id).value();
  const auto stopped = harness.kernel.stop(
      stop_request("request.failed.stop", a.value(), state.operation_revision.value()));
  ASSERT_EQ(stopped.covered.size(), 1U);  // only robot A
  // A's failure raises the failed-Stop Halt: robot B is started by it.
  ASSERT_TRUE(wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(harness.kernel);
    return StopControlExecutorTestAccess::ParticipantStop(harness.kernel, b.value().operation_id, 0U)
               .attempt != 0U;
  }));
  const auto b_stop =
      StopControlExecutorTestAccess::ParticipantStop(harness.kernel, b.value().operation_id, 0U);
  const auto halt = harness.kernel.read_stop(b_stop.started_by);
  ASSERT_TRUE(halt.ok());
  EXPECT_EQ(halt.value().applied, StopKind::kStopFailedHalt);
  const auto facts = harness.kernel.get_stop_stream_facts();
  ASSERT_TRUE(facts.program_run_hold.has_value());
  EXPECT_EQ(facts.program_run_hold->frozen, RunFrozenLevel::kHalt);
  // Admission is not closed by it (no internal fault).
  EXPECT_FALSE(harness.kernel.get_internal_fault().has_value());
}

TEST(StopStateCoreTest, StopUnconfirmedHoldsLatchUntilControllerIdle) {
  TwoRobotHarness harness;
  const auto a = harness.kernel.execute(harness.single("request.unconfirmed.a", "1"));
  ASSERT_TRUE(a.ok());
  harness.first_engine.next_stop_result = StopResultDTO{false, std::nullopt};
  harness.second_engine.next_stop_result = StopResultDTO{false, std::nullopt};
  const auto component = std::string(harness.first.value().controllable_component_id.value());
  const auto other = std::string(
      coordinated_occurrence(harness.first, "2").value().controllable_component_id.value());
  // The cell's components report their states, as the coordinator does in a
  // running cell: robot A moves, robot B is idle. The latch derivation (F2)
  // reads these states.
  harness.kernel.observe_component_states(
      {CellComponentState(component, ComponentLifecycle::kEnabled, ComponentActivity::kBusy),
       CellComponentState(other, ComponentLifecycle::kEnabled, ComponentActivity::kIdle)});
  const auto state = harness.kernel.get_operation_state(a.value().operation_id).value();
  static_cast<void>(harness.kernel.stop(
      stop_request("request.unconfirmed.stop", a.value(), state.operation_revision.value())));
  ASSERT_TRUE(wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(harness.kernel);
    const auto marked = harness.kernel.stop_unconfirmed_components();
    return std::find(marked.begin(), marked.end(), ControllableComponentId(component)) !=
           marked.end();
  }));
  // The controller reports busy: the latch holds with the stop_unconfirmed reason.
  harness.kernel.observe_component_states(
      {CellComponentState(component, ComponentLifecycle::kEnabled, ComponentActivity::kBusy)});
  auto latch = harness.kernel.get_cell_latch();
  ASSERT_TRUE(latch.latched);
  ASSERT_FALSE(latch.causes.empty());
  ASSERT_TRUE(latch.causes.front().reason.has_value());
  EXPECT_EQ(latch.causes.front().reason->code, kStopUnconfirmedReason);
  // Its first idle report arriving after the failure clears the mark.
  harness.kernel.observe_component_states(
      {CellComponentState(component, ComponentLifecycle::kEnabled, ComponentActivity::kIdle)});
  EXPECT_TRUE(harness.kernel.stop_unconfirmed_components().empty());
  // The failed-Stop Halt's stage attempts may still run on the stop-stage
  // executor; they read the scripted results, so change them only once every
  // participant Stop has ended.
  ASSERT_TRUE(wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(harness.kernel);
    return StopControlExecutorTestAccess::StoppingCount(harness.kernel) == 0U;
  }));
  harness.first_engine.next_stop_result = StopResultDTO{true, std::nullopt};
  harness.second_engine.next_stop_result = StopResultDTO{true, std::nullopt};
}

TEST(StopStateCoreTest, FailedStopDuringFailureHaltDoesNotRecurse) {
  TwoRobotHarness harness;
  const auto a = harness.kernel.execute(harness.single("request.recurse.a", "1"));
  const auto b = harness.kernel.execute(harness.single("request.recurse.b", "2"));
  ASSERT_TRUE(a.ok() && b.ok());
  harness.first_engine.next_stop_result = StopResultDTO{false, std::nullopt};
  harness.second_engine.next_stop_result = StopResultDTO{false, std::nullopt};
  const auto state = harness.kernel.get_operation_state(a.value().operation_id).value();
  const auto first = harness.kernel.stop(
      stop_request("request.recurse.stop", a.value(), state.operation_revision.value()));
  // A fails -> one failed-Stop Halt; its attempts on A and B fail too and
  // raise nothing further.
  ASSERT_TRUE(wait_until([&] {
    StopControlExecutorTestAccess::WakeAfterClockAdvance(harness.kernel);
    return StopControlExecutorTestAccess::StoppingCount(harness.kernel) == 0U &&
           StopControlExecutorTestAccess::ParticipantStop(harness.kernel, b.value().operation_id, 0U)
                   .phase == StopPhase::kFailed;
  }));
  const auto last = harness.kernel.cell_stop_action_projection().last_event_id;
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->value(), first.event_id.value() + 1U);  // exactly one Halt
}

TEST(StopStateCoreTest, OperationRowsAfterAStopCarryStoppingThenTerminalState) {
  CoreHarness harness;
  const auto operation = harness.kernel.execute(motion_operation("request.rows.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  HeldSettlementCall stage;
  harness.engine.translate_stop_stage_hook = stage.Blocker();
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  const auto stopped = harness.kernel.stop(
      stop_request("request.rows.stop", operation.value(), state.operation_revision.value()));
  // Written while the stage is held: a later state would supersede it in the queue.
  ASSERT_TRUE(stage.Entered());
  ASSERT_TRUE(StopControlExecutorTestAccess::DrainJournal(harness.kernel));
  stage.Release();
  ASSERT_TRUE(wait_phase(harness.kernel, operation.value().operation_id, 0U, StopPhase::kStopped));
  ASSERT_TRUE(StopControlExecutorTestAccess::DrainJournal(harness.kernel));
  std::lock_guard lock(harness.journal.mutex);
  const auto& calls = harness.journal.calls;
  const auto stopping = std::find(calls.begin(), calls.end(), "state:stop_accepted");
  const auto terminal = std::find(calls.begin(), calls.end(), "state:stop_terminal_committed");
  ASSERT_NE(stopping, calls.end());
  ASSERT_NE(terminal, calls.end());
  EXPECT_LT(stopping - calls.begin(), terminal - calls.begin());
  const auto rows = std::count_if(harness.journal.states.begin(), harness.journal.states.end(),
                                  [&](const auto& row) {
                                    return row.operation_id == operation.value().operation_id &&
                                           row.stop_event_id == stopped.event_id;
                                  });
  EXPECT_GE(rows, 2);
  const auto& last = harness.journal.states.back();
  ASSERT_FALSE(last.participants.empty());
  ASSERT_TRUE(last.participants.front().stop.has_value());
  EXPECT_EQ(last.participants.front().stop->stop_event_id, stopped.event_id);
  EXPECT_EQ(last.participants.front().stop->phase, StopPhase::kStopped);
}


TEST(StopStateCoreTest, FenceCallTwoRunsBeforeLanePublish) {
  CoreHarness harness;
  const auto operation = harness.kernel.execute(motion_operation("request.publish.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  static_cast<void>(harness.kernel.stop(
      stop_request("request.publish.stop", operation.value(), state.operation_revision.value())));
  EXPECT_EQ(harness.engine.addressed_stop_fence_completions.load(), 1);
  // No Stop stage had been issued when call (2) ran: lanes publish after it.
  EXPECT_EQ(harness.engine.stage_calls_at_fence_completion, 0);
}

TEST(StopStateCoreTest, FenceCallTwoRunsWhenNoGateClosed) {
  CoreHarness harness;  // dispatch already committed: call (1) closes no gate
  const auto operation = harness.kernel.execute(motion_operation("request.nogate.execute"));
  ASSERT_TRUE(operation.ok()) << operation.error().message;
  const auto state = harness.kernel.get_operation_state(operation.value().operation_id).value();
  static_cast<void>(harness.kernel.stop(
      stop_request("request.nogate.stop", operation.value(), state.operation_revision.value())));
  EXPECT_EQ(harness.engine.addressed_stop_fence_calls.load(), 1);
  EXPECT_EQ(harness.engine.addressed_stop_fence_completions.load(), 1);
  EXPECT_EQ(harness.engine.last_fence_completion_gates, 0U);
}

TEST(StopStateCoreTest, PlannedApplicationStopIsRecordedAsPlannedStop) {
  CoreHarness harness;
  const auto stopped = harness.kernel.halt(FullStopIntentDTO{
      {TestRequestKey("planned.1"), "kernel.planned_application_stop"},
      RequestAttributionDTO{RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified,
                            std::nullopt}});
  EXPECT_EQ(stopped.requested, StopKind::kPlannedStop);
  EXPECT_EQ(stopped.applied, StopKind::kPlannedStop);
}

// §8 case 31 (E2 + E13): a Stop arrives while a moving replacement (a
// takeover candidate with a pending prepared session) is ready to commit.
// The fake engine models no separate pending-replacement dispatch gate, so
// the test asserts what E13 guarantees through the Kernel: the covering Stop
// cancels the candidate, which never commits, and the incumbent alone is
// stopped and keeps its resources.
TEST(StopStateCoreTest, StopWithMovingReplacementPendingCancelsIt) {
  FakeClock clock;
  ManualScheduler scheduler;
  FakeParticipantEngine engine;
  FakeOperationJournal journal;
  engine.supports_motion_replacement = true;
  auto row = registration(
      make_binding("arm", "execution.trajectory.execute", CoordinateKind::kAngular), engine);
  row.motion_takeover_preparation_budget_s = 1.0;
  ResourceAddressedExecutionKernelConfig config{kTestApplicationStartId, CellId("cell.test"), 8U};
  config.manager_liveness_required_set = EmptyManagerLivenessSet();
  auto owner = make_uncaptured_kernel({row}, clock, scheduler, journal, std::move(config));
  auto& kernel = *owner;
  ASSERT_TRUE(claim_program_execution(kernel).ok());
  auto capture = program_execution_capture();
  {
    auto table = std::make_shared<ProgramExecutionTableDTO>();
    const auto prototype = capture.table->nodes.front();
    for (std::size_t index = 0U; index != 2U; ++index) {
      auto node = prototype;
      node.instance_id = ProgramNodeInstanceId("move." + std::to_string(index));
      table->nodes.push_back(std::move(node));
    }
    table->named_selections = {{"arm", {0U, 1U}}};
    capture.table = std::move(table);
  }
  ASSERT_TRUE(kernel.register_program_run_capture(capture).ok());
  start_program_motion(kernel);
  const auto intent_for = [](const std::size_t node, std::string request) {
    auto intent = scoped_motion_operation(std::move(request));
    intent.participants.front().execution_scope = ProgramNodeExecutionScopeDTO{
        ProgramNodeInstanceId("move." + std::to_string(node)), NodeAttemptNumber(1U)};
    return intent;
  };
  const auto incumbent = kernel.execute(intent_for(0U, "replacement.incumbent"));
  ASSERT_TRUE(incumbent.ok()) << incumbent.error().message;
  engine.emit_transition(ExecutionState::kPlanning, ExecutionState::kExecuting);
  const auto candidate = kernel.execute(intent_for(1U, "replacement.candidate"));
  ASSERT_TRUE(candidate.ok()) << candidate.error().message;
  ASSERT_TRUE(kernel.reconcile_subordinate_state().ok());
  engine.replacement_phase.store(PreparedMotionReplacementPhaseDTO::kReadyToCommit);

  const auto state = kernel.get_operation_state(incumbent.value().operation_id).value();
  const auto stopped = kernel.stop(
      stop_request("replacement.stop", incumbent.value(), state.operation_revision.value()));
  ASSERT_EQ(stopped.covered.size(), 1U);
  EXPECT_EQ(stopped.covered.front().operation_id, incumbent.value().operation_id);
  // Call (1) fenced the incumbent's execution under the Kernel lock and call
  // (2) ran after the unlock (the engine cancels the pending session there).
  EXPECT_EQ(engine.addressed_stop_fence_calls.load(), 1);
  EXPECT_EQ(engine.addressed_stop_fence_completions.load(), 1);

  ASSERT_TRUE(wait_until([&] {
    static_cast<void>(kernel.reconcile_subordinate_state());
    const auto current = kernel.get_operation_state(incumbent.value().operation_id);
    return current.ok() && current.value().state != OperationStateKind::kStopping;
  }));
  // E13: the candidate lost its right to commit.
  EXPECT_EQ(engine.replacement_owner_commits.load(), 0);
  for (const auto& slot : kernel.resource_snapshot()) {
    ASSERT_TRUE(slot.owner);
    EXPECT_EQ(slot.owner->operation_id, incumbent.value().operation_id);
  }
}

}  // namespace
}  // namespace manyforge::core
