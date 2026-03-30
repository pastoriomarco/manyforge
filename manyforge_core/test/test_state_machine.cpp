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
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
}

TEST_F(StateMachineTest, PlanningToFailed) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kFailed).ok());
}

TEST_F(StateMachineTest, PlanningToStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, ValidatingToExecuting) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, ValidatingToFailed) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kFailed).ok());
}

TEST_F(StateMachineTest, ValidatingToStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, ExecutingToStopping) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
}

TEST_F(StateMachineTest, ExecutingToCompleted) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kCompleted).ok());
}

TEST_F(StateMachineTest, ExecutingToFailed) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kFailed).ok());
}

TEST_F(StateMachineTest, PausedToValidating) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  sm.try_transition(ExecutionState::kPaused);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kValidating).ok());
}

TEST_F(StateMachineTest, PausedToStopping) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  sm.try_transition(ExecutionState::kPaused);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
}

TEST_F(StateMachineTest, StoppingToStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, StoppingToPaused) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kPaused).ok());
}

// --- Invalid transitions (at least one per state) ---
TEST_F(StateMachineTest, IdleToExecutingInvalid) {
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, IdleToStoppedInvalid) {
  EXPECT_FALSE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, PlanningToExecutingInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, PlanningToPausedInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPaused).ok());
}

TEST_F(StateMachineTest, ValidatingToPlanningInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
}

TEST_F(StateMachineTest, ExecutingToPlanningInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
}

TEST_F(StateMachineTest, ExecutingToPausedInvalid) {
  // Executing → Paused is NOT direct — must go through Stopping
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPaused).ok());
}

TEST_F(StateMachineTest, PausedToExecutingInvalid) {
  // Paused → Executing is NOT direct — must go through Validating
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  sm.try_transition(ExecutionState::kPaused);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, StoppingToExecutingInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, StoppedToAnyInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kStopped);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
  EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
}

TEST_F(StateMachineTest, CompletedToAnyInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kCompleted);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
}

TEST_F(StateMachineTest, FailedToAnyInvalid) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kFailed);
  EXPECT_FALSE(sm.try_transition(ExecutionState::kPlanning).ok());
}

// --- Reset ---
TEST_F(StateMachineTest, ResetFromIdle_Fails) {
  EXPECT_FALSE(sm.reset().ok());
}

TEST_F(StateMachineTest, ResetFromPlanning) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromValidating) {
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromExecuting) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromPaused) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  sm.try_transition(ExecutionState::kPaused);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromStopping) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kStopped);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromCompleted) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kCompleted);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromFailed) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kFailed);
  EXPECT_TRUE(sm.reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetClearsProgressAndError) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.set_progress(0.5);
  sm.set_last_error(ErrorCode::kExecutionAborted);
  sm.try_transition(ExecutionState::kFailed);
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
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, StopFromValidatingPath) {
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, StopFromExecutingPath) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
  EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

TEST_F(StateMachineTest, PausePath) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  sm.try_transition(ExecutionState::kStopping);
  sm.try_transition(ExecutionState::kPaused);
  sm.try_transition(ExecutionState::kValidating);
  sm.try_transition(ExecutionState::kExecuting);
  EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

// --- Callbacks ---
TEST_F(StateMachineTest, TransitionCallbackFiresWithCorrectStates) {
  sm.try_transition(ExecutionState::kPlanning);
  ASSERT_EQ(transitions.size(), 1u);
  EXPECT_EQ(transitions[0].first, ExecutionState::kIdle);
  EXPECT_EQ(transitions[0].second, ExecutionState::kPlanning);
}

TEST_F(StateMachineTest, CallbackDoesNotFireOnInvalidTransition) {
  sm.try_transition(ExecutionState::kExecuting);  // invalid from Idle
  EXPECT_TRUE(transitions.empty());
}

TEST_F(StateMachineTest, ResetFiresCallback) {
  sm.try_transition(ExecutionState::kPlanning);
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
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_EQ(sm.status().last_transition_ns, 1000);
  clock.set(2000);
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_EQ(sm.status().last_transition_ns, 2000);
}

// --- Thread safety ---
TEST_F(StateMachineTest, ConcurrentTransitionsDontCorrupt) {
  FakeClock shared_clock;
  ExecutionStateMachine shared_sm(shared_clock);

  shared_sm.try_transition(ExecutionState::kPlanning);
  shared_sm.try_transition(ExecutionState::kValidating);
  shared_sm.try_transition(ExecutionState::kExecuting);

  std::vector<std::thread> threads;
  for (int i = 0; i < 10; ++i) {
    threads.emplace_back([&shared_sm]() {
      shared_sm.try_transition(ExecutionState::kCompleted);
    });
  }
  for (auto& t : threads) t.join();

  auto state = shared_sm.status().state;
  EXPECT_EQ(state, ExecutionState::kCompleted);
}

}  // namespace
}  // namespace manyforge::core
