#include <gtest/gtest.h>
#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

namespace manyforge::core {
namespace {

class StopPolicyTest : public ::testing::Test {
 protected:
  FakeClock clock;
  ManualScheduler scheduler;
  ExecutionStateMachine sm{clock};
  int invalidate_count = 0;
  int stop_callback_count = 0;
  StopPolicy policy{sm, scheduler, clock, [this]() { ++invalidate_count; }};

  void SetUp() override {
    policy.set_stop_callback([this]() { ++stop_callback_count; });
  }

  void go_to_executing() {
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kValidating);
    sm.try_transition(ExecutionState::kExecuting);
  }

  void go_to_paused() {
    go_to_executing();
    policy.request_pause();
    scheduler.advance_to(clock.now());  // fires Stopping→Paused
  }
};

// --- Soft stop ---
TEST_F(StopPolicyTest, SoftStopFromExecuting_DecelerationViaScheduler) {
  go_to_executing();
  clock.set(1000);
  EXPECT_TRUE(policy.request_soft_stop(500).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);
  EXPECT_TRUE(policy.stop_requested());
  EXPECT_EQ(invalidate_count, 1);

  // Deceleration not complete yet — still Stopping.
  scheduler.advance_to(1400);
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

  // Deceleration complete.
  scheduler.advance_to(1500);
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
  EXPECT_EQ(stop_callback_count, 1);
}

TEST_F(StopPolicyTest, SoftStopFromPaused_Immediate) {
  go_to_paused();
  EXPECT_TRUE(policy.request_soft_stop(500).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
  EXPECT_TRUE(policy.stop_requested());
  EXPECT_EQ(invalidate_count, 1);
  EXPECT_EQ(stop_callback_count, 1);
}

TEST_F(StopPolicyTest, SoftStopFromPlanning_DirectToStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(policy.request_soft_stop(500).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
  EXPECT_EQ(invalidate_count, 1);
  EXPECT_EQ(stop_callback_count, 1);
}

TEST_F(StopPolicyTest, SoftStopFromValidating_DirectToStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(policy.request_soft_stop(500).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
  EXPECT_EQ(invalidate_count, 1);
}

TEST_F(StopPolicyTest, SoftStopFromIdle_Fails) {
  EXPECT_FALSE(policy.request_soft_stop(500).ok());
}

// --- Hard stop ---
TEST_F(StopPolicyTest, HardStopFromExecuting) {
  go_to_executing();
  EXPECT_TRUE(policy.request_hard_stop().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
  EXPECT_TRUE(policy.stop_requested());
  EXPECT_EQ(invalidate_count, 1);
  EXPECT_EQ(stop_callback_count, 1);
}

TEST_F(StopPolicyTest, HardStopCancelsPendingSoftStop) {
  go_to_executing();
  clock.set(1000);
  policy.request_soft_stop(500);  // schedules at 1500
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

  EXPECT_TRUE(policy.request_hard_stop().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);

  // Advancing past the soft stop deadline should NOT fire a second callback.
  stop_callback_count = 0;
  scheduler.advance_to(2000);
  EXPECT_EQ(stop_callback_count, 0);
}

TEST_F(StopPolicyTest, HardStopFromStopping_UpgradesSoftToHard) {
  go_to_executing();
  policy.request_soft_stop(500);
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);
  EXPECT_TRUE(policy.request_hard_stop().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, HardStopFromPlanning_DirectToStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_TRUE(policy.request_hard_stop().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, HardStopFromValidating_DirectToStopped) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kValidating);
  EXPECT_TRUE(policy.request_hard_stop().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, HardStopFromIdle_Fails) {
  EXPECT_FALSE(policy.request_hard_stop().ok());
}

// --- Pause ---
TEST_F(StopPolicyTest, PauseFromExecuting_DecelerationToPaused) {
  go_to_executing();
  EXPECT_TRUE(policy.request_pause().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

  scheduler.advance_to(clock.now());
  EXPECT_EQ(sm.status().state, ExecutionState::kPaused);
}

TEST_F(StopPolicyTest, PauseFromNonExecuting_Fails) {
  sm.try_transition(ExecutionState::kPlanning);
  EXPECT_FALSE(policy.request_pause().ok());
}

TEST_F(StopPolicyTest, PauseFromIdle_Fails) {
  EXPECT_FALSE(policy.request_pause().ok());
}

// --- Resume ---
TEST_F(StopPolicyTest, ResumeFromPaused_ToValidating) {
  go_to_paused();
  EXPECT_TRUE(policy.request_resume().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kValidating);
}

TEST_F(StopPolicyTest, ResumeFromNonPaused_Fails) {
  go_to_executing();
  EXPECT_FALSE(policy.request_resume().ok());
}

// --- Reset ---
TEST_F(StopPolicyTest, ResetFromTerminalState_ToIdle) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kStopped);
  EXPECT_TRUE(policy.request_reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StopPolicyTest, ResetFromCompleted_ToIdle) {
  go_to_executing();
  sm.try_transition(ExecutionState::kCompleted);
  EXPECT_TRUE(policy.request_reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StopPolicyTest, ResetFromFailed_ToIdle) {
  sm.try_transition(ExecutionState::kPlanning);
  sm.try_transition(ExecutionState::kFailed);
  EXPECT_TRUE(policy.request_reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StopPolicyTest, ResetFromExecuting_ToIdle) {
  go_to_executing();
  EXPECT_TRUE(policy.request_reset().ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StopPolicyTest, ResetFromIdle_Fails) {
  EXPECT_FALSE(policy.request_reset().ok());
}

TEST_F(StopPolicyTest, ResetClearsStopRequested) {
  go_to_executing();
  policy.request_hard_stop();
  EXPECT_TRUE(policy.stop_requested());
  policy.request_reset();
  EXPECT_FALSE(policy.stop_requested());
}

// --- Stop callback ---
TEST_F(StopPolicyTest, StopCallbackFiresOnHardStop) {
  go_to_executing();
  policy.request_hard_stop();
  EXPECT_EQ(stop_callback_count, 1);
}

TEST_F(StopPolicyTest, StopCallbackFiresOnSoftStopComplete) {
  go_to_executing();
  policy.request_soft_stop(100);
  EXPECT_EQ(stop_callback_count, 0);  // not yet
  scheduler.advance_to(100);
  EXPECT_EQ(stop_callback_count, 1);
}

// --- Cache invalidation ---
TEST_F(StopPolicyTest, CacheInvalidationFiresOnSoftStop) {
  go_to_executing();
  policy.request_soft_stop(100);
  EXPECT_EQ(invalidate_count, 1);
}

TEST_F(StopPolicyTest, CacheInvalidationFiresOnHardStop) {
  go_to_executing();
  policy.request_hard_stop();
  EXPECT_EQ(invalidate_count, 1);
}

}  // namespace
}  // namespace manyforge::core
