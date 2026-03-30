#include <gtest/gtest.h>
#include <manyforge_core/collision/collision_policy.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

class CollisionPolicyTest : public ::testing::Test {
 protected:
  FakeClock clock_;
  ManualScheduler scheduler_;
  int cache_invalidation_count_ = 0;
  std::vector<std::pair<CollisionEvent, CollisionResponse>> collision_events_;

  void SetUp() override {
    cache_invalidation_count_ = 0;
    collision_events_.clear();
  }

  void advance_to_executing(ExecutionStateMachine& sm) {
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kExecuting);
  }

  CollisionEvent make_event(CollisionSeverity severity) {
    CollisionEvent e;
    e.object_a = "link1";
    e.object_b = "obstacle1";
    e.severity = severity;
    e.timestamp_ns = clock_.now();
    return e;
  }
};

TEST_F(CollisionPolicyTest, HardStopOnHardCollision) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);
  advance_to_executing(sm);

  auto result = policy.handle_collision(make_event(CollisionSeverity::kHardStop));
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(CollisionPolicyTest, SoftStopOnSoftCollision) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);
  advance_to_executing(sm);

  auto result =
      policy.handle_collision(make_event(CollisionSeverity::kSoftStop));
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

  // Complete after deceleration
  scheduler_.advance_to(clock_.now() + 100'000'000);
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(CollisionPolicyTest, WarningNoStop) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);
  advance_to_executing(sm);

  auto result =
      policy.handle_collision(make_event(CollisionSeverity::kWarning));
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

TEST_F(CollisionPolicyTest, CacheInvalidatedOnStop) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(
      stop, [this]() { ++cache_invalidation_count_; });
  advance_to_executing(sm);

  policy.handle_collision(make_event(CollisionSeverity::kHardStop));
  EXPECT_EQ(cache_invalidation_count_, 1);
}

TEST_F(CollisionPolicyTest, NoCacheInvalidationOnWarning) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(
      stop, [this]() { ++cache_invalidation_count_; });
  advance_to_executing(sm);

  policy.handle_collision(make_event(CollisionSeverity::kWarning));
  EXPECT_EQ(cache_invalidation_count_, 0);
}

TEST_F(CollisionPolicyTest, CollisionCallbackFired) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(
      stop, nullptr,
      [this](const CollisionEvent& e, const CollisionResponse& r) {
        collision_events_.emplace_back(e, r);
      });
  advance_to_executing(sm);

  policy.handle_collision(make_event(CollisionSeverity::kHardStop));
  ASSERT_EQ(collision_events_.size(), 1u);
  EXPECT_EQ(collision_events_[0].first.severity, CollisionSeverity::kHardStop);
  EXPECT_TRUE(collision_events_[0].second.should_stop);
}

TEST_F(CollisionPolicyTest, CustomResponseConfig) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);
  advance_to_executing(sm);

  // Override warning to trigger a soft stop with 50ms deceleration
  policy.set_response(
      CollisionSeverity::kWarning,
      {true, StopType::kSoft, 50'000'000, false, ErrorCode::kCollisionDetected});

  auto result =
      policy.handle_collision(make_event(CollisionSeverity::kWarning));
  EXPECT_TRUE(result.ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);

  scheduler_.advance_to(clock_.now() + 50'000'000);
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(CollisionPolicyTest, HandleCollisionFromIdleFails) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);

  auto result =
      policy.handle_collision(make_event(CollisionSeverity::kHardStop));
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kIdle);
}

TEST_F(CollisionPolicyTest, DefaultResponses) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);

  auto warning = policy.get_response(CollisionSeverity::kWarning);
  EXPECT_FALSE(warning.should_stop);
  EXPECT_FALSE(warning.invalidate_cache);

  auto soft = policy.get_response(CollisionSeverity::kSoftStop);
  EXPECT_TRUE(soft.should_stop);
  EXPECT_EQ(soft.stop_type, StopType::kSoft);
  EXPECT_EQ(soft.deceleration_ns, 100'000'000);
  EXPECT_TRUE(soft.invalidate_cache);

  auto hard = policy.get_response(CollisionSeverity::kHardStop);
  EXPECT_TRUE(hard.should_stop);
  EXPECT_EQ(hard.stop_type, StopType::kHard);
  EXPECT_TRUE(hard.invalidate_cache);
}

TEST_F(CollisionPolicyTest, PreExecuteCheckNoCollision) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);

  auto result = policy.pre_execute_check(nullptr);
  EXPECT_TRUE(result.ok());
}

TEST_F(CollisionPolicyTest, PreExecuteCheckWithCollision) {
  ExecutionStateMachine sm(clock_);
  StopPolicy stop(sm, scheduler_, clock_);
  CollisionPolicy policy(stop);

  auto event = make_event(CollisionSeverity::kHardStop);
  auto result = policy.pre_execute_check(&event);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error(), ErrorCode::kCollisionDetected);
}

}  // namespace
}  // namespace manyforge::core
