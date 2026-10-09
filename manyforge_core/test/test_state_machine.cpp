#include <gtest/gtest.h>

#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/time/fake_clock.hpp>

#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

ParticipantExecutionFact local_failure() {
  return ParticipantExecutionFact{ParticipantExecutionFactKind::kLocalFailure,
                                  ParticipantEffectKnowledge::kNoEffect,
                                  ErrorDTO{"planning_failed", "no path", {}}};
}

class StateMachineTest : public ::testing::Test {
 protected:
  FakeClock clock;
  ExecutionStateMachine sm{clock};
};

TEST_F(StateMachineTest, InitialStateIsIdleWithoutFact) {
  const auto status = sm.status();
  EXPECT_EQ(status.state, ExecutionState::kIdle);
  EXPECT_EQ(status.last_error, ErrorCode::kSuccess);
  EXPECT_EQ(status.progress, 0.0);
  EXPECT_EQ(status.execution_id, 0u);
  EXPECT_FALSE(status.fact.has_value());
  EXPECT_FALSE(sm.settled());
}

TEST_F(StateMachineTest, WorkingPhaseEdges) {
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());  // replan
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

TEST_F(StateMachineTest, CacheHitEntersValidatingFromIdle) {
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kValidating);
}

TEST_F(StateMachineTest, InvalidWorkingEdgesAreRejected) {
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
  ASSERT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
  ASSERT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  ASSERT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_FALSE(sm.try_transition(ExecutionState::kValidating).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

// The participant machine has no operation terminals: none of them is a
// reachable state, from any phase.
TEST_F(StateMachineTest, OperationTerminalsAreNeverParticipantStates) {
  const std::vector<ExecutionState> terminals{
      ExecutionState::kInterrupted, ExecutionState::kStopping,  ExecutionState::kStopped,
      ExecutionState::kCompleted,   ExecutionState::kFailed,    ExecutionState::kInterventionRequired,
      ExecutionState::kIdle};
  for (const auto phase : {ExecutionState::kIdle, ExecutionState::kPlanning,
                           ExecutionState::kValidating, ExecutionState::kExecuting}) {
    ExecutionStateMachine machine{clock};
    if (phase != ExecutionState::kIdle) {
      ASSERT_TRUE(machine.try_transition(ExecutionState::kValidating).ok());
      if (phase == ExecutionState::kPlanning) {
        ASSERT_TRUE(machine.try_transition(ExecutionState::kPlanning).ok());
      }
      if (phase == ExecutionState::kExecuting) {
        ASSERT_TRUE(machine.try_transition(ExecutionState::kExecuting).ok());
      }
    }
    for (const auto terminal : terminals) {
      if (terminal == phase) continue;
      EXPECT_FALSE(machine.try_transition(terminal).ok())
          << to_string(phase) << " -> " << to_string(terminal);
    }
    EXPECT_EQ(machine.status().state, phase);
  }
}

TEST_F(StateMachineTest, SettleRecordsFactAndKeepsPhase) {
  ASSERT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  ASSERT_TRUE(sm.settle(local_failure()).ok());
  const auto status = sm.status();
  EXPECT_EQ(status.state, ExecutionState::kPlanning);
  ASSERT_TRUE(status.fact.has_value());
  EXPECT_EQ(status.fact->kind, ParticipantExecutionFactKind::kLocalFailure);
  EXPECT_EQ(status.fact->effect, ParticipantEffectKnowledge::kNoEffect);
  EXPECT_TRUE(sm.settled());
}

TEST_F(StateMachineTest, FirstFactWinsAndBlocksFurtherPhases) {
  ASSERT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  ASSERT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
  ASSERT_TRUE(sm.settle(ParticipantExecutionFact{
                            ParticipantExecutionFactKind::kPreemptedByAddressedStop,
                            ParticipantEffectKnowledge::kEffectSettled, std::nullopt})
                  .ok());
  EXPECT_FALSE(sm.settle(ParticipantExecutionFact{
                             ParticipantExecutionFactKind::kComputedCompletion,
                             ParticipantEffectKnowledge::kEffectSettled, std::nullopt})
                   .ok());
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
  EXPECT_EQ(sm.status().fact->kind, ParticipantExecutionFactKind::kPreemptedByAddressedStop);
}

TEST_F(StateMachineTest, CompletionFactSetsFullProgress) {
  ASSERT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  ASSERT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
  sm.set_progress(0.4);
  ASSERT_TRUE(sm.settle(ParticipantExecutionFact{
                            ParticipantExecutionFactKind::kComputedCompletion,
                            ParticipantEffectKnowledge::kEffectSettled, std::nullopt})
                  .ok());
  EXPECT_EQ(sm.status().progress, 1.0);
  EXPECT_EQ(sm.status().last_error, ErrorCode::kSuccess);
}

TEST_F(StateMachineTest, IdleCannotSettle) {
  EXPECT_FALSE(sm.settle(local_failure()).ok());
  EXPECT_FALSE(sm.status().fact.has_value());
}

TEST_F(StateMachineTest, RetireReturnsToIdleAndClearsFact) {
  ASSERT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  sm.set_progress(0.5);
  ASSERT_TRUE(sm.settle(local_failure()).ok());
  sm.retire();
  const auto status = sm.status();
  EXPECT_EQ(status.state, ExecutionState::kIdle);
  EXPECT_FALSE(status.fact.has_value());
  EXPECT_EQ(status.last_error, ErrorCode::kSuccess);
  EXPECT_EQ(status.progress, 0.0);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
}

TEST_F(StateMachineTest, TransitionTimestampsUseInjectedClock) {
  clock.set(1000);
  ASSERT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_EQ(sm.status().last_transition_ns, 1000);
  clock.set(2000);
  ASSERT_TRUE(sm.settle(local_failure()).ok());
  EXPECT_EQ(sm.status().last_transition_ns, 2000);
}

TEST_F(StateMachineTest, ConcurrentSettleRecordsExactlyOneFact) {
  ASSERT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  ASSERT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
  std::atomic<int> accepted{0};
  std::vector<std::thread> threads;
  for (int index = 0; index < 8; ++index) {
    threads.emplace_back([&] {
      if (sm.settle(local_failure()).ok()) accepted.fetch_add(1);
    });
  }
  for (auto& thread : threads) thread.join();
  EXPECT_EQ(accepted.load(), 1);
}

}  // namespace
}  // namespace manyforge::core
