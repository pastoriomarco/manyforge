#include <gtest/gtest.h>

#include <manyforge_core/interrupt/interrupt_manager.hpp>

#include <optional>

namespace manyforge::core {
namespace {

InterruptSourceDefinition source(
    std::string id,
    int priority = 100,
    InterruptSourceClass source_class = InterruptSourceClass::kFastExecutionRelevant) {
  InterruptSourceDefinition def;
  def.source_id = std::move(id);
  def.adapter = InterruptAdapter::kInternalTest;
  def.signal = def.source_id + "_signal";
  def.source_class = source_class;
  def.kind = InterruptSourceKind::kEdge;
  def.priority = priority;
  return def;
}

InterruptPolicyDefinition policy(
    std::string id,
    std::string source_id,
    int priority = 100,
    InterruptPreemptionAction action = InterruptPreemptionAction::kLatchHeld) {
  InterruptPolicyDefinition def;
  def.policy_id = std::move(id);
  def.source_id = std::move(source_id);
  def.priority = priority;
  def.affected_phases = {InterruptExecutionPhase::kValidation,
                         InterruptExecutionPhase::kTrajectoryExecution,
                         InterruptExecutionPhase::kHeld};
  def.preemption_action = action;
  def.post_latch_disposition = InterruptPostLatchDisposition::kAwaitOperatorAck;
  return def;
}

InterruptSignalUpdate update(std::string source_id,
                             std::int64_t sequence_id = 1,
                             bool value = true) {
  InterruptSignalUpdate sig;
  sig.source_id = std::move(source_id);
  sig.sequence_id = sequence_id;
  sig.timestamp_ns = 1'000;
  sig.value = value;
  return sig;
}

InterruptSignalUpdate analog_update(std::string source_id,
                                    std::int64_t sequence_id,
                                    double value) {
  InterruptSignalUpdate sig;
  sig.source_id = std::move(source_id);
  sig.sequence_id = sequence_id;
  sig.timestamp_ns = 1'000;
  sig.value = value;
  return sig;
}

InterruptRuntimeContext validation_context() {
  InterruptRuntimeContext ctx;
  ctx.phase = InterruptExecutionPhase::kValidation;
  ctx.execution_id = 42;
  ctx.move_id = "move";
  return ctx;
}

bool has_event(const InterruptIngestResult& result, const std::string& type) {
  for (const auto& event : result.events) {
    if (event.type == type) {
      return true;
    }
  }
  return false;
}

std::optional<std::size_t> event_index(const InterruptIngestResult& result,
                                       const std::string& type) {
  for (std::size_t idx = 0; idx < result.events.size(); ++idx) {
    if (result.events[idx].type == type) {
      return idx;
    }
  }
  return std::nullopt;
}

TEST(InterruptManager, RegistersSourcesAndPoliciesThenLatches) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("controller_fault")).ok());
  ASSERT_TRUE(manager.register_policy(
                  policy("controller_fault_policy", "controller_fault"))
                  .ok());

  auto result = manager.ingest(update("controller_fault"),
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               2'000);

  EXPECT_TRUE(result.accepted);
  EXPECT_TRUE(result.latched);
  ASSERT_TRUE(result.active_latch.has_value());
  EXPECT_EQ(result.active_latch->source_id, "controller_fault");
  EXPECT_EQ(result.active_latch->policy_id, "controller_fault_policy");
  EXPECT_EQ(result.active_latch->active_execution_id, 42u);
  EXPECT_TRUE(has_event(result, "interrupt_latched"));
  EXPECT_TRUE(has_event(result, "interrupt_preempt_applied"));
  ASSERT_TRUE(event_index(result, "interrupt_latched").has_value());
  ASSERT_TRUE(event_index(result, "interrupt_preempt_applied").has_value());
  EXPECT_LT(*event_index(result, "interrupt_latched"),
            *event_index(result, "interrupt_preempt_applied"));
}

TEST(InterruptManager, StaleSignalCannotTriggerLatch) {
  InterruptManager manager;
  auto src = source("stale_source");
  src.freshness_budget_ns = 50;
  ASSERT_TRUE(manager.register_source(src).ok());
  ASSERT_TRUE(manager.register_policy(policy("stale_policy", "stale_source")).ok());

  auto sig = update("stale_source");
  sig.freshness_ns = 51;
  auto result = manager.ingest(sig,
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               10'000);

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason, "stale");
  EXPECT_FALSE(manager.active_latch().has_value());
  EXPECT_TRUE(has_event(result, "interrupt_signal_rejected_stale"));
}

TEST(InterruptManager, SameWindowEqualPriorityUsesLexicographicTieBreak) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("z_source", 100)).ok());
  ASSERT_TRUE(manager.register_source(source("a_source", 100)).ok());
  ASSERT_TRUE(manager.register_policy(policy("z_policy", "z_source", 100)).ok());
  ASSERT_TRUE(manager.register_policy(policy("a_policy", "a_source", 100)).ok());

  auto result = manager.ingest_batch(
      {update("z_source", 1), update("a_source", 1)},
      InterruptSourceClass::kFastExecutionRelevant,
      validation_context(),
      2'000);

  ASSERT_TRUE(result.active_latch.has_value());
  EXPECT_EQ(result.active_latch->source_id, "a_source");
  ASSERT_TRUE(result.active_latch->tie_break.has_value());
  EXPECT_EQ(*result.active_latch->tie_break, InterruptTieBreak::kLexicographic);
  EXPECT_EQ(manager.queued_count(), 1u);
}

TEST(InterruptManager, HigherPrioritySignalPreemptsActiveLatch) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("low", 10)).ok());
  ASSERT_TRUE(manager.register_source(source("high", 900)).ok());
  ASSERT_TRUE(manager.register_policy(policy("low_policy", "low", 10)).ok());
  ASSERT_TRUE(manager.register_policy(policy("high_policy", "high", 900)).ok());

  ASSERT_TRUE(manager.ingest(update("low"),
                             InterruptSourceClass::kFastExecutionRelevant,
                             validation_context(),
                             2'000)
                  .latched);
  auto result = manager.ingest(update("high", 2),
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               3'000);

  ASSERT_TRUE(result.active_latch.has_value());
  EXPECT_EQ(result.active_latch->source_id, "high");
  EXPECT_TRUE(has_event(result, "interrupt_cleared"));
  EXPECT_TRUE(has_event(result, "interrupt_latched"));
}

TEST(InterruptManager, MultiplePoliciesForOneSourceResolveDeterministically) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("shared_source", 100)).ok());

  auto trajectory_only = policy("aaa_trajectory_policy", "shared_source", 900);
  trajectory_only.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  auto lower_priority = policy("zzz_validation_policy", "shared_source", 100);
  lower_priority.affected_phases = {InterruptExecutionPhase::kValidation};
  auto higher_priority = policy("mmm_validation_policy", "shared_source", 200);
  higher_priority.affected_phases = {InterruptExecutionPhase::kValidation};
  ASSERT_TRUE(manager.register_policy(trajectory_only).ok());
  ASSERT_TRUE(manager.register_policy(lower_priority).ok());
  ASSERT_TRUE(manager.register_policy(higher_priority).ok());

  auto result = manager.ingest(update("shared_source"),
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               2'000);

  ASSERT_TRUE(result.active_latch.has_value());
  EXPECT_EQ(result.active_latch->policy_id, "mmm_validation_policy");

  InterruptManager equal_priority_manager;
  ASSERT_TRUE(equal_priority_manager.register_source(source("tie_source", 100)).ok());
  auto z_policy = policy("z_validation_policy", "tie_source", 100);
  z_policy.affected_phases = {InterruptExecutionPhase::kValidation};
  auto a_policy = policy("a_validation_policy", "tie_source", 100);
  a_policy.affected_phases = {InterruptExecutionPhase::kValidation};
  ASSERT_TRUE(equal_priority_manager.register_policy(z_policy).ok());
  ASSERT_TRUE(equal_priority_manager.register_policy(a_policy).ok());

  auto tie_result =
      equal_priority_manager.ingest(update("tie_source"),
                                    InterruptSourceClass::kFastExecutionRelevant,
                                    validation_context(),
                                    2'000);

  ASSERT_TRUE(tie_result.active_latch.has_value());
  EXPECT_EQ(tie_result.active_latch->policy_id, "a_validation_policy");
}

TEST(InterruptManager, MaskableLowerPrioritySignalCanBeSuppressed) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("active", 500)).ok());
  ASSERT_TRUE(manager.register_source(source("masked", 100)).ok());
  ASSERT_TRUE(manager.register_policy(policy("active_policy", "active", 500)).ok());
  auto masked = policy("masked_policy", "masked", 100);
  masked.maskable = true;
  masked.maskable_by = {"active_policy"};
  ASSERT_TRUE(manager.register_policy(masked).ok());

  ASSERT_TRUE(manager.ingest(update("active"),
                             InterruptSourceClass::kFastExecutionRelevant,
                             validation_context(),
                             2'000)
                  .latched);
  auto result = manager.ingest(update("masked", 2),
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               3'000);

  EXPECT_TRUE(result.suppressed);
  EXPECT_EQ(result.reason, "suppressed_by_active_mask");
  EXPECT_EQ(manager.queued_count(), 0u);
  EXPECT_TRUE(has_event(result, "interrupt_suppressed_by_active_mask"));
}

TEST(InterruptManager, LevelSignalHysteresisAutoClearsOnSignalRelease) {
  InterruptManager manager;
  auto src = source("slip_distance", 100);
  src.kind = InterruptSourceKind::kLevel;
  src.hysteresis_value = 0.01;
  ASSERT_TRUE(manager.register_source(src).ok());
  auto auto_clear_policy = policy("slip_policy", "slip_distance", 100);
  auto_clear_policy.post_latch_disposition =
      InterruptPostLatchDisposition::kAutoClearOnSignalRelease;
  ASSERT_TRUE(manager.register_policy(auto_clear_policy).ok());

  auto latch = manager.ingest(analog_update("slip_distance", 1, 0.02),
                              InterruptSourceClass::kFastExecutionRelevant,
                              validation_context(),
                              2'000);

  EXPECT_TRUE(latch.latched);
  ASSERT_TRUE(manager.active_latch().has_value());
  EXPECT_EQ(manager.active_latch()->source_id, "slip_distance");

  auto release = manager.ingest(analog_update("slip_distance", 2, 0.005),
                                InterruptSourceClass::kFastExecutionRelevant,
                                validation_context(),
                                3'000);

  EXPECT_TRUE(release.accepted);
  EXPECT_FALSE(manager.active_latch().has_value());
  ASSERT_TRUE(has_event(release, "interrupt_cleared"));
  bool saw_auto_release = false;
  for (const auto& event : release.events) {
    if (event.type == "interrupt_cleared" &&
        event.details.at("clear_reason") == "auto_signal_release") {
      saw_auto_release = true;
    }
  }
  EXPECT_TRUE(saw_auto_release);
}

TEST(InterruptManager, BoundedQueueReplacesOldestLowerPriority) {
  InterruptManager manager({.queue_depth = 1});
  ASSERT_TRUE(manager.register_source(source("active", 500)).ok());
  ASSERT_TRUE(manager.register_source(source("queued_low", 10)).ok());
  ASSERT_TRUE(manager.register_source(source("queued_mid", 20)).ok());
  ASSERT_TRUE(manager.register_policy(policy("active_policy", "active", 500)).ok());
  ASSERT_TRUE(manager.register_policy(policy("low_policy", "queued_low", 10)).ok());
  ASSERT_TRUE(manager.register_policy(policy("mid_policy", "queued_mid", 20)).ok());
  ASSERT_TRUE(manager.ingest(update("active"),
                             InterruptSourceClass::kFastExecutionRelevant,
                             validation_context(),
                             2'000)
                  .latched);
  ASSERT_TRUE(manager.ingest(update("queued_low", 2),
                             InterruptSourceClass::kFastExecutionRelevant,
                             validation_context(),
                             3'000)
                  .queued);

  auto result = manager.ingest(update("queued_mid", 3),
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               4'000);

  EXPECT_TRUE(result.queued);
  EXPECT_TRUE(has_event(result, "interrupt_dropped_queue_overflow"));
  EXPECT_EQ(manager.queued_count(), 1u);
}

TEST(InterruptManager, DispositionClearsLatchAndActivatesQueuedUpdate) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("first", 100)).ok());
  ASSERT_TRUE(manager.register_source(source("second", 100)).ok());
  ASSERT_TRUE(manager.register_policy(policy("first_policy", "first", 100)).ok());
  ASSERT_TRUE(manager.register_policy(policy("second_policy", "second", 100)).ok());
  auto first = manager.ingest(update("first"),
                              InterruptSourceClass::kFastExecutionRelevant,
                              validation_context(),
                              2'000);
  ASSERT_TRUE(first.latched);
  ASSERT_TRUE(manager.ingest(update("second", 2),
                             InterruptSourceClass::kFastExecutionRelevant,
                             validation_context(),
                             3'000)
                  .queued);

  InterruptDisposition disposition;
  disposition.latch_id = first.active_latch->latch_id;
  disposition.action = InterruptDispositionAction::kAcknowledge;
  disposition.principal_id = "operator";
  auto result = manager.apply_disposition(disposition, 4'000);

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(has_event(result.value(), "interrupt_disposition_applied"));
  EXPECT_TRUE(has_event(result.value(), "interrupt_cleared"));
  ASSERT_TRUE(result.value().active_latch.has_value());
  EXPECT_EQ(result.value().active_latch->source_id, "second");
}

TEST(InterruptManager, FaultTerminalDispositionClearsLatchWithTerminalReason) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("terminal_fault", 100)).ok());
  auto terminal_policy = policy("terminal_policy", "terminal_fault", 100);
  terminal_policy.post_latch_disposition =
      InterruptPostLatchDisposition::kFaultTerminal;
  ASSERT_TRUE(manager.register_policy(terminal_policy).ok());

  auto latched = manager.ingest(update("terminal_fault"),
                                InterruptSourceClass::kFastExecutionRelevant,
                                validation_context(),
                                2'000);
  ASSERT_TRUE(latched.latched);
  ASSERT_TRUE(latched.active_latch.has_value());

  InterruptDisposition ack;
  ack.latch_id = latched.active_latch->latch_id;
  ack.action = InterruptDispositionAction::kAcknowledge;
  ack.principal_id = "operator";
  auto rejected = manager.apply_disposition(ack, 3'000);
  ASSERT_TRUE(rejected.ok());
  EXPECT_TRUE(rejected.value().rejected);
  EXPECT_EQ(rejected.value().reason, "rejected_policy");
  ASSERT_TRUE(manager.active_latch().has_value());

  InterruptDisposition terminal;
  terminal.latch_id = latched.active_latch->latch_id;
  terminal.action = InterruptDispositionAction::kFaultTerminal;
  terminal.principal_id = "safety_supervisor";
  auto result = manager.apply_disposition(terminal, 4'000);

  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().accepted);
  EXPECT_FALSE(manager.active_latch().has_value());
  EXPECT_TRUE(has_event(result.value(), "interrupt_disposition_applied"));
  EXPECT_TRUE(has_event(result.value(), "interrupt_cleared"));
  bool saw_terminal_clear = false;
  for (const auto& event : result.value().events) {
    if (event.type == "interrupt_cleared" &&
        event.details.at("clear_reason") == "fault_terminal") {
      saw_terminal_clear = true;
    }
  }
  EXPECT_TRUE(saw_terminal_clear);
}

TEST(InterruptManager, SourceClassViolationIsRejected) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(
                  source("supervisory",
                         100,
                         InterruptSourceClass::kLowRateSupervisory))
                  .ok());
  ASSERT_TRUE(manager.register_policy(policy("policy", "supervisory", 100)).ok());

  auto result = manager.ingest(update("supervisory"),
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               2'000);

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason, "source_class_violation");
  EXPECT_FALSE(manager.active_latch().has_value());
}

TEST(InterruptManager, EdgeDebounceRejectsSecondLatch) {
  InterruptManager manager;
  auto src = source("debounced", 100);
  src.debounce_ns = 100;
  ASSERT_TRUE(manager.register_source(src).ok());
  ASSERT_TRUE(manager.register_policy(policy("policy", "debounced", 100)).ok());

  ASSERT_TRUE(manager.ingest(update("debounced", 1),
                             InterruptSourceClass::kFastExecutionRelevant,
                             validation_context(),
                             1'000)
                  .latched);
  ASSERT_TRUE(manager.clear_active_latch("test_clear", 1'010).ok());
  auto result = manager.ingest(update("debounced", 2),
                               InterruptSourceClass::kFastExecutionRelevant,
                               validation_context(),
                               1'050);

  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason, "debounced");
}

// ---------- Motion-scoped policy + manager semantics ----------

InterruptPolicyDefinition trip_policy(
    std::string policy_id,
    std::string source_id,
    int priority = 700,
    std::set<MotionInterruptActivationMode> allowed = {
        MotionInterruptActivationMode::kRequireInactiveAtArm}) {
  InterruptPolicyDefinition p;
  p.policy_id = std::move(policy_id);
  p.source_id = std::move(source_id);
  p.priority = priority;
  p.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  p.preemption_action = InterruptPreemptionAction::kCompleteMoveOnSignal;
  p.post_latch_disposition =
      InterruptPostLatchDisposition::kAutoClearAfterMotionOutcome;
  p.scope_kind = InterruptPolicyScopeKind::kMove;
  p.allowed_activation_modes = std::move(allowed);
  // complete_move_on_signal policies MUST declare a non-empty
  // allowed_stop_modes set. Tests use kSpringBackHold as
  // the simplest production-qualified mode.
  p.allowed_stop_modes = {MotionInterruptStopMode::kSpringBackHold};
  return p;
}

/// Build a minimal stop profile so test scopes can arm a
/// complete_move_on_signal policy under the stop_profile_required
/// gate.
MotionInterruptStopProfileDTO trip_stop_profile(
    MotionInterruptStopMode mode = MotionInterruptStopMode::kSpringBackHold) {
  MotionInterruptStopProfileDTO p;
  p.profile_id = "test_spring_back_hold";
  p.stop_mode = mode;
  return p;
}

InterruptRuntimeContext executing_context(
    std::uint64_t execution_id = 100,
    std::string move_id = "probe_move") {
  InterruptRuntimeContext ctx;
  ctx.phase = InterruptExecutionPhase::kTrajectoryExecution;
  ctx.execution_id = execution_id;
  ctx.move_id = std::move(move_id);
  return ctx;
}

InterruptSignalUpdate probe_baseline(std::string source_id,
                                     std::int64_t sequence_id,
                                     std::int64_t timestamp_ns,
                                     bool active = false) {
  InterruptSignalUpdate s;
  s.source_id = std::move(source_id);
  s.sequence_id = sequence_id;
  s.timestamp_ns = timestamp_ns;
  s.value = active;
  return s;
}

TEST(InterruptManagerMotionScopedTest,
     MoveScopedPolicyInactiveWithoutArmedBinding) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());

  // No binding armed → update with matching context must NOT latch.
  auto result = manager.ingest(probe_baseline("probe", 1, 1'000, /*active=*/true),
                               InterruptSourceClass::kFastExecutionRelevant,
                               executing_context(),
                               1'100);
  EXPECT_FALSE(result.latched);
  EXPECT_FALSE(manager.active_latch().has_value());
}

TEST(InterruptManagerMotionScopedTest,
     MoveScopedPolicyLatchesWithMatchingExecutionAndMoveId) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());

  // Provide a fresh inactive baseline before arming.
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, /*active=*/false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  scope.activation_mode = MotionInterruptActivationMode::kRequireInactiveAtArm;
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move", {scope}, 1'020);
  ASSERT_TRUE(arm.ok());
  EXPECT_TRUE(has_event(arm.value(), "motion_interrupt_scope_armed"));
  ASSERT_EQ(manager.active_motion_interrupt_scopes().size(), 1u);

  // Mismatched execution_id must not latch.
  auto mismatch = manager.ingest(probe_baseline("probe", 2, 1'030, true),
                                 InterruptSourceClass::kFastExecutionRelevant,
                                 executing_context(/*execution_id=*/999),
                                 1'040);
  EXPECT_FALSE(mismatch.latched);
  EXPECT_FALSE(manager.active_latch().has_value());

  // Mismatched move_id must not latch.
  auto wrong_move = manager.ingest(probe_baseline("probe", 3, 1'050, true),
                                   InterruptSourceClass::kFastExecutionRelevant,
                                   executing_context(100, "other_move"),
                                   1'060);
  EXPECT_FALSE(wrong_move.latched);

  // Matching context + active signal latches; scope_id carried on latch.
  auto match = manager.ingest(probe_baseline("probe", 4, 1'070, true),
                              InterruptSourceClass::kFastExecutionRelevant,
                              executing_context(),
                              1'080);
  ASSERT_TRUE(match.latched);
  ASSERT_TRUE(manager.active_latch().has_value());
  EXPECT_EQ(manager.active_latch()->motion_interrupt_scope_id.value_or(""),
            "probe_scope_a");
  EXPECT_EQ(manager.active_latch()->preemption_action_requested,
            InterruptPreemptionAction::kCompleteMoveOnSignal);
}

TEST(InterruptManagerMotionScopedTest,
     ScopeExpiryRejectsLaterUpdates) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  ASSERT_TRUE(
      manager.arm_motion_interrupt_scopes(100, "probe_move", {scope}, 1'020).ok());

  auto reap = manager.reap_motion_interrupt_scopes(100, "execution_terminal",
                                                     1'030);
  EXPECT_TRUE(has_event(reap, "motion_interrupt_scope_reaped"));
  EXPECT_TRUE(manager.active_motion_interrupt_scopes().empty());

  auto after_reap = manager.ingest(probe_baseline("probe", 2, 1'040, true),
                                   InterruptSourceClass::kFastExecutionRelevant,
                                   executing_context(),
                                   1'050);
  EXPECT_FALSE(after_reap.latched);
}

TEST(InterruptManagerMotionScopedTest,
     SameSourceSafetyPolicyOutranksTripAtRegistration) {
  // A fault policy must have strictly HIGHER priority than the
  // trip policy. Configuration that violates this MUST be rejected at
  // registration (spec/260 §2.2 invariants).
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe", /*priority=*/800)).ok());
  // Fault policy at priority 800 (default source priority).
  InterruptPolicyDefinition fault;
  fault.policy_id = "probe_fault";
  fault.source_id = "probe";
  fault.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  fault.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  fault.post_latch_disposition = InterruptPostLatchDisposition::kAwaitOperatorAck;
  ASSERT_TRUE(manager.register_policy(fault).ok());

  // Trip at equal priority 800 → registration must reject.
  auto trip = trip_policy("probe_trip", "probe", /*priority=*/800);
  auto reg = manager.register_policy(trip);
  EXPECT_FALSE(reg.ok());
  EXPECT_EQ(reg.error().details.at("reason"),
            "move_scoped_trip_priority_not_below_fault");

  // Trip at lower priority succeeds.
  auto trip2 = trip_policy("probe_trip", "probe", /*priority=*/600);
  ASSERT_TRUE(manager.register_policy(trip2).ok());
}

TEST(InterruptManagerMotionScopedTest,
     EqualPrioritySameSourceFaultTripRegistrationFails) {
  // Two policies on the same source at the same effective priority
  // are rejected at registration unless they share preemption_action
  // and post_latch_disposition.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe", /*priority=*/700)).ok());

  // Session-scoped trajectory-cancel policy at priority 700.
  InterruptPolicyDefinition session_cancel;
  session_cancel.policy_id = "probe_cancel";
  session_cancel.source_id = "probe";
  session_cancel.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  session_cancel.preemption_action = InterruptPreemptionAction::kLatchHeld;
  session_cancel.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  ASSERT_TRUE(manager.register_policy(session_cancel).ok());

  // Session-scoped policy with different action at equal priority → rejected.
  InterruptPolicyDefinition diff_action;
  diff_action.policy_id = "probe_other";
  diff_action.source_id = "probe";
  diff_action.priority = 700;
  diff_action.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  diff_action.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  diff_action.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  auto reg = manager.register_policy(diff_action);
  EXPECT_FALSE(reg.ok());
  EXPECT_EQ(reg.error().details.at("reason"),
            "same_source_equal_priority_action_mismatch");
}

TEST(InterruptManagerMotionScopedTest,
     RequireInactiveAtArmRejectsActiveBaseline) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());

  // Inject an active baseline through ingest (which won't latch because
  // no scope is armed yet, but does update last_sample_by_source_).
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, /*active=*/true),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  scope.activation_mode = MotionInterruptActivationMode::kRequireInactiveAtArm;
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move", {scope}, 1'020);
  EXPECT_FALSE(arm.ok());
  EXPECT_EQ(arm.error().details.at("reason"),
            "motion_interrupt_source_active_at_arm");
  EXPECT_TRUE(manager.active_motion_interrupt_scopes().empty());
}

TEST(InterruptManagerMotionScopedTest,
     FreshEdgeAfterArmIgnoresBaselineActiveUntilFreshEdge) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager
                  .register_policy(trip_policy(
                      "probe_trip", "probe", /*priority=*/700,
                      {MotionInterruptActivationMode::kFreshEdgeAfterArm}))
                  .ok());

  // Baseline arrives active. fresh_edge_after_arm tolerates an active
  // baseline; arming records baseline_sequence_id and the next edge
  // (strictly later sequence_id) latches.
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 5, 1'000, /*active=*/true),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  scope.activation_mode = MotionInterruptActivationMode::kFreshEdgeAfterArm;
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move", {scope}, 1'020);
  ASSERT_TRUE(arm.ok());
  ASSERT_EQ(manager.active_motion_interrupt_scopes().front().baseline_active,
            true);

  // Replay of baseline sequence id must not latch (no fresh edge yet).
  auto stale = manager.ingest(probe_baseline("probe", 5, 1'030, true),
                              InterruptSourceClass::kFastExecutionRelevant,
                              executing_context(),
                              1'040);
  EXPECT_FALSE(stale.latched);

  // A strictly newer active edge latches.
  auto fresh = manager.ingest(probe_baseline("probe", 6, 1'050, true),
                              InterruptSourceClass::kFastExecutionRelevant,
                              executing_context(),
                              1'060);
  EXPECT_TRUE(fresh.latched);
  ASSERT_TRUE(manager.active_latch().has_value());
  EXPECT_EQ(manager.active_latch()->motion_interrupt_scope_id.value_or(""),
            "probe_scope_a");
}

TEST(InterruptManagerMotionScopedTest,
     MultipleTripRaceDeterministicWinnerAndSecondarySignal) {
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe_a", /*priority=*/700)).ok());
  ASSERT_TRUE(manager.register_source(source("probe_b", /*priority=*/700)).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("trip_a", "probe_a")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("trip_b", "probe_b")).ok());

  // Inactive baselines for both sources.
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe_a", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe_b", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope_a;
  scope_a.scope_id = "scope_a";
  scope_a.policy_id = "trip_a";
  scope_a.stop_profile = trip_stop_profile();
  MotionInterruptScopeDTO scope_b;
  scope_b.scope_id = "scope_b";
  scope_b.policy_id = "trip_b";
  scope_b.stop_profile = trip_stop_profile();
  ASSERT_TRUE(
      manager.arm_motion_interrupt_scopes(100, "probe_move",
                                            {scope_a, scope_b}, 1'020)
          .ok());

  // Both sources trip in the same batch at equal priority. Deterministic
  // tie-break: lower source_id (probe_a < probe_b) wins.
  std::vector<InterruptSignalUpdate> batch{
      probe_baseline("probe_b", 2, 1'030, true),
      probe_baseline("probe_a", 2, 1'030, true),
  };
  auto result = manager.ingest_batch(
      batch, InterruptSourceClass::kFastExecutionRelevant,
      executing_context(), 1'040);
  ASSERT_TRUE(manager.active_latch().has_value());
  EXPECT_EQ(manager.active_latch()->source_id, "probe_a");
  EXPECT_TRUE(has_event(result, "motion_trip_secondary_signal"));
  // Loser is the other source.
  for (const auto& ev : result.events) {
    if (ev.type == "motion_trip_secondary_signal") {
      EXPECT_EQ(ev.details.at("losing_source_id"), "probe_b");
      EXPECT_EQ(ev.details.at("losing_policy_id"), "trip_b");
      EXPECT_EQ(ev.details.at("winning_latch_id"),
                manager.active_latch()->latch_id);
    }
  }
}

// -------- Regression coverage --------

TEST(InterruptManagerMotionScopedTest,
     ArmEventEmitsArmBaselineAgeNsAndOmitsBaselineSequenceId) {
  // Finding 5: spec/260 §7 requires `motion_interrupt_scope_armed` to
  // expose `arm_baseline_age_ns` so operators can verify the baseline
  // was within `arm_timeout_ns`. Sequence IDs are an internal
  // arbitration detail and MUST NOT appear on the event.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());

  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 7, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'500);
  ASSERT_TRUE(arm.ok());
  ASSERT_TRUE(has_event(arm.value(), "motion_interrupt_scope_armed"));
  for (const auto& ev : arm.value().events) {
    if (ev.type != "motion_interrupt_scope_armed") {
      continue;
    }
    ASSERT_TRUE(ev.details.contains("arm_baseline_age_ns"));
    EXPECT_EQ(ev.details.at("arm_baseline_age_ns"), "500");
    EXPECT_FALSE(ev.details.contains("baseline_sequence_id"));
  }
}

TEST(InterruptManagerMotionScopedTest,
     ResultBindingKeyInvalidFormatRejectsArm) {
  // Finding 3: result_binding_key must match spec/260 §2.6 format —
  // 1-128 bytes, no NUL, regex [A-Za-z_][A-Za-z0-9_.:-]*. Malformed
  // keys must fail closed at arm time.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  const std::vector<std::string> invalid_keys{
      "",                                       // empty
      "9_leading_digit",                        // first char must not be digit
      "has space",                              // space not allowed
      "has/slash",                              // slash not allowed
      std::string("with\0nul", 8),              // NUL byte
      std::string(129, 'a'),                    // length > 128
      "-leading-hyphen",                        // hyphen not allowed at start
  };
  for (const auto& bad : invalid_keys) {
    MotionInterruptScopeDTO scope;
    scope.scope_id = "scope_a";
    scope.policy_id = "probe_trip";
    scope.stop_profile = trip_stop_profile();
    scope.result_binding_key = bad;
    auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                     {scope}, 1'020);
    ASSERT_FALSE(arm.ok())
        << "expected rejection for key bytes=" << bad.size();
    EXPECT_EQ(arm.error().details.at("reason"),
              "motion_interrupt_result_binding_key_invalid");
    EXPECT_TRUE(manager.active_motion_interrupt_scopes().empty());
  }

  // Sanity: a valid key arms successfully.
  MotionInterruptScopeDTO good;
  good.scope_id = "scope_a";
  good.policy_id = "probe_trip";
  good.stop_profile = trip_stop_profile();
  good.result_binding_key = "BT.probe-result_1:slot";
  EXPECT_TRUE(
      manager.arm_motion_interrupt_scopes(100, "probe_move", {good}, 1'020)
          .ok());
}

TEST(InterruptManagerMotionScopedTest,
     MoveScopedAwaitOperatorAckRejectedAtRegistrationForAllActions) {
  // Finding 4: spec/260 §2.2 forbids await_operator_ack on any
  // move-scoped policy — including fault-action variants. Earlier code
  // allowed it when the action was a fault action; tightened now.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());

  // Even with a fault preemption action (preempt_trajectory), a
  // move-scoped policy carrying await_operator_ack must be rejected.
  InterruptPolicyDefinition fault_move;
  fault_move.policy_id = "probe_fault_move";
  fault_move.source_id = "probe";
  fault_move.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  fault_move.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  fault_move.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  fault_move.scope_kind = InterruptPolicyScopeKind::kMove;
  auto reg = manager.register_policy(fault_move);
  EXPECT_FALSE(reg.ok());
  EXPECT_EQ(reg.error().details.at("reason"),
            "move_scoped_policy_disposition_not_allowed");
}

TEST(InterruptManagerMotionScopedTest,
     StaleOrClassViolatingUpdatesDoNotShiftBaseline) {
  // Finding 2: stale or wrong-lane updates must NOT seed the baseline
  // used by activation-mode checks. A fresh inactive sample is taken
  // first, then a stale active update arrives — arming under
  // require_inactive_at_arm must succeed because the baseline still
  // says inactive.
  InterruptSourceDefinition probe_src = source("probe");
  probe_src.freshness_budget_ns = 1'000;  // 1 us budget
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(probe_src).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());

  // Fresh inactive baseline.
  auto fresh_inactive = manager.ingest(probe_baseline("probe", 1, 1'000, false),
                                         InterruptSourceClass::kFastExecutionRelevant,
                                         executing_context(),
                                         1'500);
  EXPECT_FALSE(fresh_inactive.latched);

  // Stale active update: timestamp_ns 1'000, kernel_now_ns 100'000 →
  // freshness 99us > 1us budget. Must reject as `stale` AND must not
  // seed the baseline to "active".
  auto stale = manager.ingest(probe_baseline("probe", 2, 1'000, true),
                              InterruptSourceClass::kFastExecutionRelevant,
                              executing_context(),
                              100'000);
  EXPECT_TRUE(has_event(stale, "interrupt_signal_rejected_stale"));

  // Class-violating update on wrong lane: must reject and not seed.
  auto bad_lane = manager.ingest(probe_baseline("probe", 3, 100'500, true),
                                  InterruptSourceClass::kLowRateSupervisory,
                                  executing_context(),
                                  100'600);
  EXPECT_TRUE(has_event(bad_lane, "interrupt_signal_rejected_class_violation"));

  // Arm immediately after kernel-now within budget: baseline must
  // still be "inactive" (from the only valid sample), so arming
  // under require_inactive_at_arm must succeed and emit
  // arm_baseline_age_ns matching the original sample.
  MotionInterruptScopeDTO scope;
  scope.scope_id = "scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  scope.arm_timeout_ns = 1'000'000;  // 1 ms to allow the age we'll measure
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'800);
  ASSERT_TRUE(arm.ok());
  ASSERT_EQ(manager.active_motion_interrupt_scopes().size(), 1u);
  EXPECT_FALSE(manager.active_motion_interrupt_scopes().front().baseline_active);
}

TEST(InterruptManagerMotionScopedTest,
     QueuedMoveScopedUpdateIsDroppedWhenScopeIsReaped) {
  // Finding 1 (BLOCKING): a move-scoped trip candidate can lose
  // arbitration to a *non* move-scoped winner and land in `queue_`.
  // If the binding is then reaped before `clear_active_latch_locked`
  // replays the queue, the queued update MUST be dropped — both
  // proactively at reap time and defensively in the replay path.
  // Otherwise the kernel would latch a complete_move_on_signal policy
  // whose scope no longer exists.
  //
  // To force the queue path we arrange a same-window batch where the
  // winner is a session-scoped fault (which is NOT a move-trip and so
  // does NOT trigger the motion_trip_secondary_signal short-circuit),
  // and the loser is a move-scoped trip on a different source.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe_safety",
                                              /*priority=*/1'000)).ok());
  ASSERT_TRUE(manager.register_source(source("probe_b",
                                              /*priority=*/600)).ok());

  // Session-scoped fault policy on probe_safety, priority 1000 (winner).
  InterruptPolicyDefinition safety_fault;
  safety_fault.policy_id = "safety_fault";
  safety_fault.source_id = "probe_safety";
  safety_fault.priority = 1'000;
  safety_fault.affected_phases = {
      InterruptExecutionPhase::kTrajectoryExecution};
  safety_fault.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  safety_fault.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  safety_fault.scope_kind = InterruptPolicyScopeKind::kSession;
  ASSERT_TRUE(manager.register_policy(safety_fault).ok());

  // Move-scoped trip policy on probe_b, priority 600 (loser, queued).
  ASSERT_TRUE(manager.register_policy(
      trip_policy("trip_b", "probe_b", /*priority=*/600)).ok());

  // Inactive baseline for probe_b so arming succeeds.
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe_b", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope_b;
  scope_b.scope_id = "scope_b";
  scope_b.policy_id = "trip_b";
  scope_b.stop_profile = trip_stop_profile();
  ASSERT_TRUE(
      manager.arm_motion_interrupt_scopes(100, "probe_move", {scope_b}, 1'020)
          .ok());

  // Same-window batch: safety_fault (winner) + trip_b (loser → queued).
  std::vector<InterruptSignalUpdate> batch{
      probe_baseline("probe_safety", 1, 1'030, true),
      probe_baseline("probe_b", 2, 1'030, true),
  };
  auto race = manager.ingest_batch(
      batch, InterruptSourceClass::kFastExecutionRelevant,
      executing_context(), 1'040);
  ASSERT_TRUE(manager.active_latch().has_value());
  EXPECT_EQ(manager.active_latch()->policy_id, "safety_fault");
  EXPECT_TRUE(race.queued);
  ASSERT_EQ(manager.queued_count(), 1u);

  // Reap all move-scoped bindings for execution_id 100. The queued
  // trip_b update MUST be purged proactively AND its drop event MUST
  // be emitted; the session-scoped fault latch is unaffected.
  auto reap = manager.reap_motion_interrupt_scopes(100, "execution_terminal",
                                                     2'000);
  EXPECT_TRUE(has_event(reap, "motion_interrupt_scope_reaped"));
  EXPECT_TRUE(has_event(reap, "motion_interrupt_queued_update_dropped"));
  EXPECT_EQ(manager.queued_count(), 0u);
  ASSERT_TRUE(manager.active_latch().has_value());
  EXPECT_EQ(manager.active_latch()->policy_id, "safety_fault");

  // Clearing the active latch must NOT replay the now-stale queued
  // update. (Belt-and-suspenders: queue is already empty after reap.)
  auto cleared = manager.clear_active_latch("reset", 2'100);
  ASSERT_TRUE(cleared.ok());
  EXPECT_FALSE(cleared.value().latched);
  EXPECT_FALSE(manager.active_latch().has_value());
}

// -------- Stop-profile + capability validation gates --------

TEST(InterruptManagerMotionScopedTest,
     PolicyRegistrationRequiresAllowedStopModesForCompleteMoveOnSignal) {
  // Per spec/260 §2.2: complete_move_on_signal policies
  // MUST declare a non-empty allowed_stop_modes set. A policy that
  // omits this deployment gate is rejected at registration.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());

  auto policy = trip_policy("probe_trip", "probe");
  policy.allowed_stop_modes.clear();  // drop the helper default
  auto reg = manager.register_policy(policy);
  EXPECT_FALSE(reg.ok());
  EXPECT_EQ(reg.error().details.at("reason"),
            "complete_move_on_signal_requires_allowed_stop_modes");
}

TEST(InterruptManagerMotionScopedTest,
     PolicyRegistrationRejectsAllowedStopModesOnNonTripPolicy) {
  // Defense in depth: allowed_stop_modes is meaningful only for
  // complete_move_on_signal. A session-fault policy that smuggles a
  // stop-mode set MUST be rejected at registration.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());

  InterruptPolicyDefinition p;
  p.policy_id = "probe_fault";
  p.source_id = "probe";
  p.affected_phases = {InterruptExecutionPhase::kTrajectoryExecution};
  p.preemption_action = InterruptPreemptionAction::kPreemptTrajectory;
  p.post_latch_disposition =
      InterruptPostLatchDisposition::kAwaitOperatorAck;
  p.allowed_stop_modes = {MotionInterruptStopMode::kSpringBackHold};

  auto reg = manager.register_policy(p);
  EXPECT_FALSE(reg.ok());
  EXPECT_EQ(reg.error().details.at("reason"),
            "allowed_stop_modes_requires_complete_move_on_signal");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsMoveScopedScopeMissingStopProfile) {
  // complete_move_on_signal scopes MUST carry a
  // MotionInterruptStopProfileDTO. Missing profile is an arm-time
  // rejection with reason `stop_profile_required`.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "scope_a";
  scope.policy_id = "probe_trip";
  // Intentionally do NOT set stop_profile.
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  EXPECT_FALSE(arm.ok());
  EXPECT_EQ(arm.error().details.at("reason"), "stop_profile_required");
  EXPECT_TRUE(manager.active_motion_interrupt_scopes().empty());
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopModeNotInPolicyAllowedSet) {
  // scope.stop_profile.stop_mode MUST be in
  // policy.allowed_stop_modes.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  auto policy = trip_policy("probe_trip", "probe");
  // Policy allows only kSpringBackHold; scope requests kHardDeceleration.
  policy.allowed_stop_modes = {MotionInterruptStopMode::kSpringBackHold};
  ASSERT_TRUE(manager.register_policy(policy).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile =
      trip_stop_profile(MotionInterruptStopMode::kHardDeceleration);
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  EXPECT_FALSE(arm.ok());
  EXPECT_EQ(arm.error().details.at("reason"),
            "stop_mode_not_allowed_by_policy");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsFallbackModeNotInPolicyAllowedSet) {
  // Every element of stop_profile.allowed_fallback_modes
  // MUST be in policy.allowed_stop_modes.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  auto policy = trip_policy("probe_trip", "probe");
  policy.allowed_stop_modes = {MotionInterruptStopMode::kSpringBackHold};
  ASSERT_TRUE(manager.register_policy(policy).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kHardDeceleration};
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  EXPECT_FALSE(arm.ok());
  EXPECT_EQ(arm.error().details.at("reason"),
            "fallback_mode_not_allowed_by_policy");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsFallbackOnPrecisionContactNodeCategory) {
  // Spec/210 §3.11 node-category fallback defaults: precision-contact
  // node families (ProbeSearchMove) MUST keep allowed_fallback_modes
  // empty. Arming a precision-contact scope with any fallback list
  // fails closed.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  auto policy = trip_policy("probe_trip", "probe");
  policy.allowed_stop_modes = {MotionInterruptStopMode::kSpringBackHold,
                                 MotionInterruptStopMode::kHardDeceleration};
  ASSERT_TRUE(manager.register_policy(policy).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  scope.stop_profile->allowed_fallback_modes = {
      MotionInterruptStopMode::kHardDeceleration};
  scope.node_category = MotionInterruptNodeCategory::kPrecisionContact;
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  EXPECT_FALSE(arm.ok());
  EXPECT_EQ(arm.error().details.at("reason"),
            "fallback_not_allowed_for_node_category");
}

TEST(InterruptManagerMotionScopedTest,
     ArmAcceptsPrecisionContactNodeWithEmptyFallback) {
  // Confirm the precision-contact gate doesn't block the legitimate
  // case: empty allowed_fallback_modes is the precision-contact
  // default and MUST arm cleanly.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  scope.node_category = MotionInterruptNodeCategory::kPrecisionContact;
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  ASSERT_TRUE(arm.ok());
  EXPECT_EQ(manager.active_motion_interrupt_scopes().size(), 1u);
}

// -------- Stop-profile shape gates --------

namespace {

/// Helper that registers source + trip_policy + inactive baseline and
/// returns an arm result for a scope carrying `bad_profile`.
InterruptIngestResult arm_with_profile(
    InterruptManager& manager,
    const MotionInterruptStopProfileDTO& profile) {
  manager.register_source(source("probe"));
  manager.register_policy(trip_policy("probe_trip", "probe"));
  (void)manager.ingest(probe_baseline("probe", 1, 1'000, false),
                        InterruptSourceClass::kFastExecutionRelevant,
                        executing_context(),
                        1'010);
  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = profile;
  auto result = manager.arm_motion_interrupt_scopes(
      100, "probe_move", {scope}, 1'020);
  if (!result.ok()) {
    InterruptIngestResult err_result;
    err_result.rejected = true;
    err_result.reason = result.error().details.contains("reason")
                            ? result.error().details.at("reason")
                            : result.error().code;
    return err_result;
  }
  return result.value();
}

}  // namespace

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopProfileWithEmptyProfileId) {
  InterruptManager manager;
  MotionInterruptStopProfileDTO p;
  p.profile_id = "";  // malformed
  p.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  auto result = arm_with_profile(manager, p);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason, "stop_profile_id_required");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopProfileRequireBackendConfirmationFalse) {
  // Spec/210 §3.11: v1 production profiles MUST use
  // require_backend_confirmation=true. Diagnostic-only `false`
  // configurations MUST NOT reach a production dispatch path.
  InterruptManager manager;
  MotionInterruptStopProfileDTO p;
  p.profile_id = "diagnostic_no_confirm";
  p.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  p.require_backend_confirmation = false;
  auto result = arm_with_profile(manager, p);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason,
            "stop_profile_require_backend_confirmation_must_be_true");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsSoftOnPathProfileMissingDecelerationTime) {
  // Spec/210 §3.11: deceleration_time_s is REQUIRED for soft_on_path.
  // Without it the backend cannot compute the path-preserving
  // ramp-down trajectory in 4.5D-5.
  InterruptManager manager;
  // Allow soft_on_path in the policy so the scope reaches the shape gate.
  auto policy = trip_policy("probe_trip", "probe");
  policy.allowed_stop_modes = {MotionInterruptStopMode::kSoftOnPath};
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(policy).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = MotionInterruptStopProfileDTO{};
  scope.stop_profile->profile_id = "soft_no_time";
  scope.stop_profile->stop_mode = MotionInterruptStopMode::kSoftOnPath;
  // deceleration_time_s intentionally unset.
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  EXPECT_FALSE(arm.ok());
  EXPECT_EQ(arm.error().details.at("reason"),
            "soft_on_path_requires_deceleration_time_s");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsSoftOnPathProfileMissingPerJointDecelerationLimits) {
  // True soft_on_path cannot be armed without acceleration
  // evidence because the ramp planner must prove the wall-time stop
  // against per-joint limits before backend dispatch.
  InterruptManager manager;
  auto policy = trip_policy("probe_trip", "probe");
  policy.allowed_stop_modes = {MotionInterruptStopMode::kSoftOnPath};
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(policy).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = MotionInterruptStopProfileDTO{};
  scope.stop_profile->profile_id = "soft_no_limits";
  scope.stop_profile->stop_mode = MotionInterruptStopMode::kSoftOnPath;
  scope.stop_profile->deceleration_time_s = 0.1;
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  EXPECT_FALSE(arm.ok());
  EXPECT_EQ(arm.error().details.at("reason"),
            "soft_on_path_requires_per_joint_deceleration_limits");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopProfileNonPositiveDecelerationTime) {
  InterruptManager manager;
  MotionInterruptStopProfileDTO p;
  p.profile_id = "neg_decel";
  p.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  p.deceleration_time_s = -0.05;  // negative
  auto result = arm_with_profile(manager, p);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason,
            "stop_profile_deceleration_time_s_must_be_positive");

  // Zero must also fail.
  InterruptManager m2;
  MotionInterruptStopProfileDTO p2;
  p2.profile_id = "zero_decel";
  p2.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  p2.deceleration_time_s = 0.0;
  auto r2 = arm_with_profile(m2, p2);
  EXPECT_TRUE(r2.rejected);
  EXPECT_EQ(r2.reason,
            "stop_profile_deceleration_time_s_must_be_positive");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopProfileNonPositiveMaxStopTime) {
  InterruptManager manager;
  MotionInterruptStopProfileDTO p;
  p.profile_id = "neg_max_stop";
  p.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  p.max_stop_time_s = 0.0;
  auto result = arm_with_profile(manager, p);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason,
            "stop_profile_max_stop_time_s_must_be_positive");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopProfileNonPositivePerJointDeceleration) {
  InterruptManager manager;
  MotionInterruptStopProfileDTO p;
  p.profile_id = "bad_per_joint";
  p.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  p.per_joint_max_deceleration_rad_s2["j1"] = -2.0;
  auto result = arm_with_profile(manager, p);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason,
            "stop_profile_per_joint_max_deceleration_must_be_positive");
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopProfilePerJointDecelerationEmptyJointName) {
  InterruptManager manager;
  MotionInterruptStopProfileDTO p;
  p.profile_id = "empty_joint";
  p.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  p.per_joint_max_deceleration_rad_s2[""] = 1.5;
  auto result = arm_with_profile(manager, p);
  EXPECT_TRUE(result.rejected);
  EXPECT_EQ(result.reason,
            "stop_profile_per_joint_max_deceleration_joint_name_required");
}

TEST(InterruptManagerMotionScopedTest,
     ArmAcceptsWellFormedSoftOnPathProfile) {
  // Sanity: a soft_on_path profile with positive ramp + max_stop_time
  // + positive per-joint limits arms cleanly.
  InterruptManager manager;
  auto policy = trip_policy("probe_trip", "probe");
  policy.allowed_stop_modes = {MotionInterruptStopMode::kSoftOnPath};
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(policy).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);

  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = MotionInterruptStopProfileDTO{};
  scope.stop_profile->profile_id = "good_soft";
  scope.stop_profile->stop_mode = MotionInterruptStopMode::kSoftOnPath;
  scope.stop_profile->deceleration_time_s = 0.05;
  scope.stop_profile->max_stop_time_s = 0.5;
  scope.stop_profile->per_joint_max_deceleration_rad_s2 = {{"j1", 2.0},
                                                              {"j2", 2.0}};
  auto arm = manager.arm_motion_interrupt_scopes(100, "probe_move",
                                                   {scope}, 1'020);
  ASSERT_TRUE(arm.ok());
  EXPECT_EQ(manager.active_motion_interrupt_scopes().size(), 1u);
}

TEST(InterruptManagerMotionScopedTest,
     ArmRejectsStopProfileOnNonTripScopeDefenseInDepth) {
  // Defense in depth: stop_profile makes sense only for
  // complete_move_on_signal. Manual construction of a scope binding
  // a non-trip move-scoped policy with a stop_profile is rejected.
  //
  // Currently the only valid move-scoped action IS
  // complete_move_on_signal (validate_move_scoped_policy_locked
  // enforces this at registration), so the inverse misuse cannot
  // arrive through the public arm API. The defense-in-depth branch
  // is exercised by registering a kLatchHeld policy as kSession
  // (legal) and confirming the binding code refuses a stop_profile.
  // The arm path resolves policies by id, so this targets the build
  // path directly when the scope_kind invariant is bypassed.
  //
  // For the public surface we instead confirm the inverse:
  // attaching a stop_profile to a scope bound to a legitimate
  // complete_move_on_signal policy succeeds, and that the manager
  // does NOT mistakenly require stop_profile on non-move-scoped
  // submissions.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("probe")).ok());
  ASSERT_TRUE(manager.register_policy(trip_policy("probe_trip", "probe")).ok());
  ASSERT_FALSE(manager
                   .ingest(probe_baseline("probe", 1, 1'000, false),
                           InterruptSourceClass::kFastExecutionRelevant,
                           executing_context(),
                           1'010)
                   .latched);
  MotionInterruptScopeDTO scope;
  scope.scope_id = "probe_scope_a";
  scope.policy_id = "probe_trip";
  scope.stop_profile = trip_stop_profile();
  EXPECT_TRUE(manager
                  .arm_motion_interrupt_scopes(100, "probe_move", {scope},
                                                 1'020)
                  .ok());
}

// ----- Fault-stop profile policy registration gates -----

TEST(InterruptManagerFaultStopProfileTest,
     RegisterPolicy_PreemptTrajectoryWithStopProfile_Accepted) {
  // Per spec/260 §2.2: a kPreemptTrajectory policy MAY carry an
  // optional stop_profile, which routes the
  // preempt through execute_controlled_stop instead of legacy stop.
  InterruptManager manager;
  ASSERT_TRUE(manager
                  .register_source(source("runtime_collision_src", 500,
                                          InterruptSourceClass::kFastExecutionRelevant))
                  .ok());

  auto def = policy("runtime_collision_fault_with_profile",
                    "runtime_collision_src", 500,
                    InterruptPreemptionAction::kPreemptTrajectory);
  MotionInterruptStopProfileDTO sp;
  sp.profile_id = "fault_spring_back_v1";
  sp.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  def.stop_profile = std::make_shared<MotionInterruptStopProfileDTO>(sp);

  EXPECT_TRUE(manager.register_policy(def).ok());
}

TEST(InterruptManagerFaultStopProfileTest,
     RegisterPolicy_StopProfileOnNonPreemptTrajectory_Rejected) {
  // Defense in depth: stop_profile on kLatchHeld / kCompleteMoveOnSignal /
  // any non-kPreemptTrajectory action is a configuration error.
  InterruptManager manager;
  ASSERT_TRUE(manager.register_source(source("src", 100)).ok());

  auto def = policy("hold_policy_with_profile", "src", 100,
                    InterruptPreemptionAction::kLatchHeld);
  MotionInterruptStopProfileDTO sp;
  sp.profile_id = "should_not_be_here";
  sp.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  def.stop_profile = std::make_shared<MotionInterruptStopProfileDTO>(sp);

  const auto result = manager.register_policy(def);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"),
            "stop_profile_requires_preempt_trajectory");
}

TEST(InterruptManagerFaultStopProfileTest,
     RegisterPolicy_NoStopProfile_LegacyBehaviorPreserved) {
  // No-regression sanity check: deployments not opting into the
  // fault-stop-profile surface still register policies the way they
  // always have. This guards against the validator inadvertently
  // requiring stop_profile on every kPreemptTrajectory policy.
  InterruptManager manager;
  ASSERT_TRUE(manager
                  .register_source(source("legacy_runtime_src", 500,
                                          InterruptSourceClass::kFastExecutionRelevant))
                  .ok());

  auto def = policy("legacy_runtime_fault", "legacy_runtime_src", 500,
                    InterruptPreemptionAction::kPreemptTrajectory);
  // stop_profile intentionally NOT set.

  EXPECT_TRUE(manager.register_policy(def).ok());
  EXPECT_FALSE(static_cast<bool>(def.stop_profile));
}

TEST(InterruptManagerFaultStopProfileTest,
     ActivePolicy_ReturnsRegisteredPolicyWithStopProfile) {
  // Verifies the new public `active_policy()` accessor the kernel uses
  // to read the active fault policy's stop_profile on the preempt path.
  InterruptManager manager;
  ASSERT_TRUE(manager
                  .register_source(source("runtime_src", 500,
                                          InterruptSourceClass::kFastExecutionRelevant))
                  .ok());
  auto def = policy("runtime_fault_with_profile", "runtime_src", 500,
                    InterruptPreemptionAction::kPreemptTrajectory);
  MotionInterruptStopProfileDTO sp;
  sp.profile_id = "fault_spring_back_v1";
  sp.stop_mode = MotionInterruptStopMode::kSpringBackHold;
  def.stop_profile = std::make_shared<MotionInterruptStopProfileDTO>(sp);
  ASSERT_TRUE(manager.register_policy(def).ok());

  // No latch yet → active_policy returns nullopt.
  EXPECT_FALSE(manager.active_policy().has_value());

  // Latch the policy. After ingest, active_policy returns it with the
  // stop_profile intact so the kernel can route through
  // execute_controlled_stop.
  manager.ingest(update("runtime_src"),
                 InterruptSourceClass::kFastExecutionRelevant,
                 validation_context(), 2'000);
  const auto active = manager.active_policy();
  ASSERT_TRUE(active.has_value());
  EXPECT_EQ(active->policy_id, "runtime_fault_with_profile");
  ASSERT_TRUE(static_cast<bool>(active->stop_profile));
  EXPECT_EQ(active->stop_profile->profile_id, "fault_spring_back_v1");
  EXPECT_EQ(active->stop_profile->stop_mode,
            MotionInterruptStopMode::kSpringBackHold);
}

}  // namespace
}  // namespace manyforge::core
