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

}  // namespace
}  // namespace manyforge::core
