#include <gtest/gtest.h>
#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

namespace manyforge::core {
namespace {

class StopPolicyTest : public ::testing::Test {
 protected:
    FakeClock clock_;
    ManualScheduler scheduler_;
    int invalidation_count_ = 0;
    int stop_callback_count_ = 0;

    void SetUp() override {
        invalidation_count_ = 0;
        stop_callback_count_ = 0;
    }

    void advance_to_executing(ExecutionStateMachine& sm) {
        sm.try_transition(ExecutionState::kPlanning);
        sm.try_transition(ExecutionState::kExecuting);
    }
};

TEST_F(StopPolicyTest, HardStopFromExecuting) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_,
                      [this]() { ++invalidation_count_; });
    advance_to_executing(sm);

    auto result = policy.request_hard_stop();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
    EXPECT_TRUE(policy.stop_requested());
    EXPECT_EQ(invalidation_count_, 1);
}

TEST_F(StopPolicyTest, SoftStopFromExecuting) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_,
                      [this]() { ++invalidation_count_; });
    advance_to_executing(sm);
    clock_.set(1000);

    auto result = policy.request_soft_stop(500);
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kStopping);
    EXPECT_TRUE(policy.stop_requested());
    EXPECT_EQ(invalidation_count_, 1);

    // Deferred stop fires after deceleration
    scheduler_.advance_to(1500);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, SoftStopDoesNotCompleteBeforeDeadline) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    advance_to_executing(sm);
    clock_.set(1000);

    policy.request_soft_stop(500);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

    // Not yet at deadline
    scheduler_.advance_to(1499);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

    // At deadline
    scheduler_.advance_to(1500);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, SoftStopFromPausedIsImmediate) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_,
                      [this]() { ++invalidation_count_; });
    advance_to_executing(sm);
    policy.request_pause();

    auto result = policy.request_soft_stop(1'000'000);
    EXPECT_TRUE(result.ok());
    // Paused means already stationary — stopped immediately, no deceleration.
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
    EXPECT_TRUE(policy.stop_requested());
    EXPECT_EQ(invalidation_count_, 1);
}

TEST_F(StopPolicyTest, HardStopCancelsPendingSoftStop) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    advance_to_executing(sm);
    clock_.set(1000);

    policy.request_soft_stop(500);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

    // Hard stop upgrades the soft stop
    auto result = policy.request_hard_stop();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);

    // Advancing scheduler past soft stop deadline has no effect (cancelled)
    scheduler_.advance_to(2000);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, StopFromIdleFails) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);

    EXPECT_FALSE(policy.request_hard_stop().ok());
    EXPECT_FALSE(policy.request_soft_stop(1000).ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(StopPolicyTest, PauseFromExecuting) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    advance_to_executing(sm);

    auto result = policy.request_pause();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kPaused);
}

TEST_F(StopPolicyTest, ResumeFromPaused) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    advance_to_executing(sm);
    policy.request_pause();

    auto result = policy.request_resume();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

TEST_F(StopPolicyTest, PauseFromIdleFails) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);

    auto result = policy.request_pause();
    EXPECT_FALSE(result.ok());
}

TEST_F(StopPolicyTest, ResumeFromExecutingFails) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    advance_to_executing(sm);

    auto result = policy.request_resume();
    EXPECT_FALSE(result.ok());
}

TEST_F(StopPolicyTest, HardStopFromPaused) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_,
                      [this]() { ++invalidation_count_; });
    advance_to_executing(sm);
    policy.request_pause();

    auto result = policy.request_hard_stop();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
    EXPECT_EQ(invalidation_count_, 1);
}

TEST_F(StopPolicyTest, NoInvalidationCallbackIsOk) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    advance_to_executing(sm);

    auto result = policy.request_hard_stop();
    EXPECT_TRUE(result.ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, StopRequestedInitiallyFalse) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    EXPECT_FALSE(policy.stop_requested());
}

TEST_F(StopPolicyTest, StopCallbackFiresOnSoftStop) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    policy.set_stop_callback([this]() { ++stop_callback_count_; });
    advance_to_executing(sm);
    clock_.set(1000);

    policy.request_soft_stop(500);
    EXPECT_EQ(stop_callback_count_, 0);  // Not yet

    scheduler_.advance_to(1500);
    EXPECT_EQ(stop_callback_count_, 1);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, StopCallbackFiresOnHardStop) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    policy.set_stop_callback([this]() { ++stop_callback_count_; });
    advance_to_executing(sm);

    policy.request_hard_stop();
    EXPECT_EQ(stop_callback_count_, 1);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(StopPolicyTest, PauseResumeSequenceThenSoftStop) {
    ExecutionStateMachine sm(clock_);
    StopPolicy policy(sm, scheduler_, clock_);
    advance_to_executing(sm);
    clock_.set(1000);

    EXPECT_TRUE(policy.request_pause().ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kPaused);

    EXPECT_TRUE(policy.request_resume().ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);

    EXPECT_TRUE(policy.request_soft_stop(200).ok());
    EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

    scheduler_.advance_to(1200);
    EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

}  // namespace
}  // namespace manyforge::core
