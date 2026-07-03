#include <gtest/gtest.h>
#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

class StateMachineTest : public ::testing::Test {
 protected:
  FakeClock clock;
  std::vector<std::pair<ExecutionState, ExecutionState>> transitions;
  ExecutionStateMachine sm{clock, [this](ExecutionState from, ExecutionState to) {
    transitions.emplace_back(from, to);
  }};
};

// --- Initial state ---
TEST_F(StateMachineTest, InitialStateIsIdle) {
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
  EXPECT_EQ(sm.status().last_error, ErrorCode::kSuccess);
  EXPECT_EQ(sm.status().progress, 0.0);
  EXPECT_EQ(sm.status().execution_id, 0u);
}

// --- Valid transitions (one test per row of spec 230 §6.3) ---
TEST_F(StateMachineTest, IdleToPlanning) {
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kPlanning);
}

TEST_F(StateMachineTest, IdleToValidating) {
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kValidating);
}

TEST_F(StateMachineTest, PlanningToValidating) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
}

TEST_F(StateMachineTest, PlanningToFailed) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kFailed).ok());
}

TEST_F(StateMachineTest, PlanningToStopped) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, ValidatingToExecuting) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, ValidatingToFailed) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kFailed).ok());
}

TEST_F(StateMachineTest, ValidatingToStopped) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, ExecutingToStopping) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
}

TEST_F(StateMachineTest, ExecutingToCompleted) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kCompleted).ok());
}

TEST_F(StateMachineTest, ExecutingToFailed) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kFailed).ok());
}

TEST_F(StateMachineTest, PausedToValidating) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  (void)sm.try_transition(ExecutionState::kPaused);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
}

TEST_F(StateMachineTest, PausedToStopping) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  (void)sm.try_transition(ExecutionState::kPaused);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
}

TEST_F(StateMachineTest, StoppingToStopped) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, StoppingToPaused) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPaused).ok());
}

TEST_F(StateMachineTest, PlanningToInterrupted) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kInterrupted).ok());
}

TEST_F(StateMachineTest, ValidatingToInterrupted) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kInterrupted).ok());
}

TEST_F(StateMachineTest, ExecutingToInterrupted) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kInterrupted).ok());
}

TEST_F(StateMachineTest, InterruptedToInterventionRequired) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kInterrupted);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kInterventionRequired).ok());
}

// --- Invalid transitions (at least one per state) ---
TEST_F(StateMachineTest, IdleToExecutingInvalid) {
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, IdleToStoppedInvalid) {
  EXPECT_FALSE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, PlanningToExecutingInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, PlanningToPausedInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPaused).ok());
}

TEST_F(StateMachineTest, ValidatingToPlanningForAuthoritativeReplan) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kPlanning);
}

TEST_F(StateMachineTest, ExecutingToPlanningInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
}

TEST_F(StateMachineTest, ExecutingToPausedInvalid) {
  // Executing → Paused is NOT direct — must go through Stopping
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPaused).ok());
}

TEST_F(StateMachineTest, PausedToExecutingInvalid) {
  // Paused → Executing is NOT direct — must go through Validating
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  (void)sm.try_transition(ExecutionState::kPaused);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, StoppingToExecutingInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, StoppedToAnyInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kStopped);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, CompletedToAnyInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kCompleted);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
}

TEST_F(StateMachineTest, FailedToAnyInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kFailed);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
}

TEST_F(StateMachineTest, IdleToInterruptedInvalid) {
  EXPECT_FALSE(sm.try_transition(ExecutionState::kInterrupted).ok());
}

TEST_F(StateMachineTest, InterruptedToExecutingInvalid) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kInterrupted);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

// --- Reset ---
TEST_F(StateMachineTest, ResetFromIdle_Fails) {
  EXPECT_FALSE(sm.reset().ok());
}

TEST_F(StateMachineTest, ResetFromPlanning) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromValidating) {
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromExecuting) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromPaused) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  (void)sm.try_transition(ExecutionState::kPaused);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromStopping) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromStopped) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kStopped);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromCompleted) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kCompleted);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromFailed) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kFailed);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromInterrupted) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kInterrupted);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetClearsProgressAndError) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  sm.set_progress(0.5);
  sm.set_last_error(ErrorCode::kExecutionAborted);
  (void)sm.try_transition(ExecutionState::kFailed);
  sm.reset();
  EXPECT_EQ(sm.status().progress, 0.0);
  EXPECT_EQ(sm.status().last_error, ErrorCode::kSuccess);
}

// --- Happy paths ---
TEST_F(StateMachineTest, FullHappyPath) {
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kCompleted).ok());
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, CacheHitPath) {
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kCompleted).ok());
}

TEST_F(StateMachineTest, StopFromPlanningPath) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, StopFromValidatingPath) {
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, StopFromExecutingPath) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, PausePath) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  (void)sm.try_transition(ExecutionState::kStopping);
  (void)sm.try_transition(ExecutionState::kPaused);
  (void)sm.try_transition(ExecutionState::kValidating);
  (void)sm.try_transition(ExecutionState::kExecuting);
  EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

// --- Callbacks ---
TEST_F(StateMachineTest, TransitionCallbackFiresWithCorrectStates) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
}

TEST_F(StateMachineTest, CallbackDoesNotFireOnInvalidTransition) {
  (void)sm.try_transition(ExecutionState::kExecuting);  // invalid from Idle
  EXPECT_TRUE(transitions.empty());
}

TEST_F(StateMachineTest, ResetFiresCallback) {
  (void)sm.try_transition(ExecutionState::kPlanning);
  transitions.clear();
  sm.reset();
  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kPlanning);
  EXPECT_EQ(transitions[0].second, ExecutionState::kIdle);
}

// --- Setters ---
TEST_F(StateMachineTest, SetProgress) {
  sm.set_progress(0.75);
  EXPECT_EQ(sm.status().progress, 0.75);
}

TEST_F(StateMachineTest, SetLastError) {
  sm.set_last_error(ErrorCode::kPlanningFailed);
  EXPECT_EQ(sm.status().last_error, ErrorCode::kPlanningFailed);
}

TEST_F(StateMachineTest, SetExecutionId) {
  sm.set_execution_id(42);
  EXPECT_EQ(sm.status().execution_id, 42u);
}

// --- Timestamp ---
TEST_F(StateMachineTest, TransitionUpdatesTimestamp) {
  clock.set(1000);
  (void)sm.try_transition(ExecutionState::kPlanning);
  EXPECT_EQ(sm.status().last_transition_ns, 1000);
  clock.set(2000);
  (void)sm.try_transition(ExecutionState::kValidating);
  EXPECT_EQ(sm.status().last_transition_ns, 2000);
}

// --- Thread safety ---
TEST_F(StateMachineTest, ConcurrentTransitionsDontCorrupt) {
  FakeClock shared_clock;
  ExecutionStateMachine shared_sm(shared_clock);

  (void)shared_sm.try_transition(ExecutionState::kPlanning);
  (void)shared_sm.try_transition(ExecutionState::kValidating);
  (void)shared_sm.try_transition(ExecutionState::kExecuting);

  std::vector<std::jthread> threads;
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([&shared_sm]() {
      (void)shared_sm.try_transition(ExecutionState::kCompleted);
    });
  }
  // jthreads auto-join when destroyed

  auto state = shared_sm.status().state;
  EXPECT_EQ(state, ExecutionState::kCompleted);
}

}  // namespace
}  // namespace manyforge::core

// K1 characterization: normal terminals must be unrepresentable from safety
// terminals at the transition-table level. A laundering bug therefore has to
// happen BEFORE the safety terminal commits — which is exactly the window the
// TerminalCommitter closes; these pins guarantee the table half stays true.
TEST(StateMachineTerminalAuthority, NormalTerminalCannotOverwriteSafetyTerminal) {
  using manyforge::core::ExecutionState;
  manyforge::core::FakeClock clock;
  {
    manyforge::core::ExecutionStateMachine sm{clock};
    ASSERT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
    ASSERT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kFailed).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kCompleted).ok());
  }
  {
    manyforge::core::ExecutionStateMachine sm{clock};
    ASSERT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
    ASSERT_TRUE(sm.try_transition(ExecutionState::kInterrupted).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kCompleted).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kStopped).ok());
  }
  {
    manyforge::core::ExecutionStateMachine sm{clock};
    ASSERT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
    ASSERT_TRUE(
        sm.try_transition(ExecutionState::kInterventionRequired).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kCompleted).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kStopped).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kFailed).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kInterrupted).ok());
  }
}
