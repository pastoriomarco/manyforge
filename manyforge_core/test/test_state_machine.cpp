#include <gtest/gtest.h>
#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <vector>
#include <utility>

namespace manyforge::core {
namespace {

class StateMachineTest : public ::testing::Test {
 protected:
    FakeClock clock_;
    std::vector<std::pair<ExecutionState, ExecutionState>> transitions_;

    void record_transition(ExecutionState from, ExecutionState to) {
        transitions_.emplace_back(from, to);
    }
};

TEST_F(StateMachineTest, StartsInIdle) {
    ExecutionStateMachine sm(clock_);
    EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, IdleToPlanning) {
    ExecutionStateMachine sm(clock_);
    auto result = sm.try_transition(ExecutionState::kPlanning);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kPlanning);
}

TEST_F(StateMachineTest, InvalidFromIdle) {
    ExecutionStateMachine sm(clock_);
    EXPECT_FALSE(sm.try_transition(ExecutionState::kExecuting).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kPaused).ok());
    EXPECT_FALSE(sm.try_transition(ExecutionState::kStopping).ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, FullHappyPath) {
    ExecutionStateMachine sm(clock_);
    EXPECT_TRUE(sm.try_transition(ExecutionState::kPlanning).ok());
    EXPECT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
    EXPECT_TRUE(sm.try_transition(ExecutionState::kCompleted).ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kCompleted);
}

TEST_F(StateMachineTest, PauseAndResume) {
    ExecutionStateMachine sm(clock_);
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kExecuting);
    EXPECT_TRUE(sm.try_transition(ExecutionState::kPaused).ok());
    EXPECT_TRUE(sm.try_transition(ExecutionState::kExecuting).ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

TEST_F(StateMachineTest, StopFromExecuting) {
    ExecutionStateMachine sm(clock_);
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kExecuting);
    EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
    EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StateMachineTest, FailFromPlanning) {
    ExecutionStateMachine sm(clock_);
    sm.try_transition(ExecutionState::kPlanning);
    EXPECT_TRUE(sm.try_transition(ExecutionState::kFailed).ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kFailed);
}

TEST_F(StateMachineTest, ResetFromTerminal) {
    ExecutionStateMachine sm(clock_);
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kExecuting);
    sm.try_transition(ExecutionState::kCompleted);
    EXPECT_TRUE(sm.reset().ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StateMachineTest, ResetFromNonTerminalFails) {
    ExecutionStateMachine sm(clock_);
    EXPECT_FALSE(sm.reset().ok());
    sm.try_transition(ExecutionState::kPlanning);
    EXPECT_FALSE(sm.reset().ok());
}

TEST_F(StateMachineTest, TransitionCallback) {
    ExecutionStateMachine sm(clock_, [this](ExecutionState from, ExecutionState to) {
        record_transition(from, to);
    });
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kExecuting);
    ASSERT_EQ(transitions_.size(), 2u);
    EXPECT_EQ(transitions_[0].first, ExecutionState::kIdle);
    EXPECT_EQ(transitions_[0].second, ExecutionState::kPlanning);
    EXPECT_EQ(transitions_[1].first, ExecutionState::kPlanning);
    EXPECT_EQ(transitions_[1].second, ExecutionState::kExecuting);
}

TEST_F(StateMachineTest, TimestampUpdatesOnTransition) {
    ExecutionStateMachine sm(clock_);
    clock_.set(1000);
    sm.try_transition(ExecutionState::kPlanning);
    EXPECT_EQ(sm.status().last_transition_ns, 1000);
    clock_.set(2000);
    sm.try_transition(ExecutionState::kExecuting);
    EXPECT_EQ(sm.status().last_transition_ns, 2000);
}

TEST_F(StateMachineTest, SetProgressAndError) {
    ExecutionStateMachine sm(clock_);
    sm.set_progress(0.5);
    sm.set_last_error(ErrorCode::kPlanningFailed);
    sm.set_execution_id(42);
    auto s = sm.status();
    EXPECT_DOUBLE_EQ(s.progress, 0.5);
    EXPECT_EQ(s.last_error, ErrorCode::kPlanningFailed);
    EXPECT_EQ(s.execution_id, 42u);
}

TEST_F(StateMachineTest, ResetClearsProgressAndError) {
    ExecutionStateMachine sm(clock_);
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kFailed);
    sm.set_progress(0.7);
    sm.set_last_error(ErrorCode::kExecutionFailed);
    sm.reset();
    auto s = sm.status();
    EXPECT_DOUBLE_EQ(s.progress, 0.0);
    EXPECT_EQ(s.last_error, ErrorCode::kSuccess);
}

TEST_F(StateMachineTest, StopFromPaused) {
    ExecutionStateMachine sm(clock_);
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kExecuting);
    sm.try_transition(ExecutionState::kPaused);
    EXPECT_TRUE(sm.try_transition(ExecutionState::kStopping).ok());
    EXPECT_TRUE(sm.try_transition(ExecutionState::kStopped).ok());
}

}  // namespace
}  // namespace manyforge::core
