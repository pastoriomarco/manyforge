#include <gtest/gtest.h>
#include <manyforge_core/collision/collision_policy.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

namespace manyforge::core {
namespace {

class CollisionPolicyTest : public ::testing::Test {
 protected:
  FakeClock clock;
  ManualScheduler scheduler;
  ExecutionStateMachine sm{clock};
  StopPolicy stop_policy{sm, scheduler, clock};

  int invalidate_count = 0;
  CollisionCheckResultDTO last_callback_check;
  bool callback_fired = false;

  CollisionPolicy policy{stop_policy,
      [this]() { ++invalidate_count; },
      [this](const CollisionCheckResultDTO& c) {
        last_callback_check = c;
        callback_fired = true;
      }};

  void go_to_executing() {
    sm.try_transition(ExecutionState::kPlanning);
    sm.try_transition(ExecutionState::kValidating);
    sm.try_transition(ExecutionState::kExecuting);
  }
};

// --- handle_collision ---
TEST_F(CollisionPolicyTest, HandleCollision_InCollision_TriggersHardStop) {
  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = true;
  check.contact_body_a = "link1";
  check.contact_body_b = "obstacle";

  EXPECT_TRUE(policy.handle_collision(check).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

TEST_F(CollisionPolicyTest, HandleCollision_NotInCollision_NoOp) {
  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = false;

  EXPECT_TRUE(policy.handle_collision(check).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kExecuting);
}

TEST_F(CollisionPolicyTest, HandleCollision_CacheInvalidationFires) {
  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = true;

  policy.handle_collision(check);
  EXPECT_EQ(invalidate_count, 1);
}

TEST_F(CollisionPolicyTest, HandleCollision_CallbackFires) {
  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = true;
  check.contact_body_a = "arm_link";
  check.contact_body_b = "table";

  policy.handle_collision(check);
  EXPECT_TRUE(callback_fired);
  EXPECT_EQ(last_callback_check.contact_body_a, "arm_link");
  EXPECT_EQ(last_callback_check.contact_body_b, "table");
}

TEST_F(CollisionPolicyTest, HandleCollision_NotInCollision_NoCallback) {
  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = false;

  policy.handle_collision(check);
  EXPECT_FALSE(callback_fired);
  EXPECT_EQ(invalidate_count, 0);
}

TEST_F(CollisionPolicyTest, HandleCollision_FromIdle_StopFails) {
  CollisionCheckResultDTO check;
  check.in_collision = true;

  auto result = policy.handle_collision(check);
  EXPECT_FALSE(result.ok());
}

// --- Custom response configuration ---
TEST_F(CollisionPolicyTest, SetAndGetResponse) {
  CollisionResponse r;
  r.stop_type = StopType::kSoft;
  r.deceleration_ns = 200;
  r.invalidate_cache = false;

  policy.set_response(r);
  auto got = policy.get_response();
  EXPECT_EQ(got.deceleration_ns, 200);
  EXPECT_FALSE(got.invalidate_cache);
}

TEST_F(CollisionPolicyTest, DefaultResponse_HardStop) {
  auto r = policy.get_response();
  EXPECT_EQ(r.stop_type, StopType::kHard);
  EXPECT_EQ(r.deceleration_ns, 0);
  EXPECT_TRUE(r.invalidate_cache);
}

TEST_F(CollisionPolicyTest, CustomSoftStopResponse) {
  CollisionResponse r;
  r.stop_type = StopType::kSoft;
  r.deceleration_ns = 300;
  r.invalidate_cache = true;
  policy.set_response(r);

  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = true;

  EXPECT_TRUE(policy.handle_collision(check).ok());
  EXPECT_EQ(sm.status().state, ExecutionState::kStopping);
  EXPECT_EQ(invalidate_count, 1);

  scheduler.advance_to(300);
  EXPECT_EQ(sm.status().state, ExecutionState::kStopped);
}

// --- Contact body information ---
TEST_F(CollisionPolicyTest, ContactBodyPreservedInCallback) {
  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = true;
  check.contact_body_a = "gripper";
  check.contact_body_b = "box";

  policy.handle_collision(check);
  EXPECT_TRUE(callback_fired);
  EXPECT_EQ(last_callback_check.contact_body_a.value(), "gripper");
  EXPECT_EQ(last_callback_check.contact_body_b.value(), "box");
}

TEST_F(CollisionPolicyTest, ContactBody_Optional_Absent) {
  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = true;

  policy.handle_collision(check);
  EXPECT_TRUE(callback_fired);
  EXPECT_FALSE(last_callback_check.contact_body_a.has_value());
  EXPECT_FALSE(last_callback_check.contact_body_b.has_value());
}

TEST_F(CollisionPolicyTest, NoInvalidationWhenConfiguredOff) {
  CollisionResponse r;
  r.stop_type = StopType::kHard;
  r.invalidate_cache = false;
  policy.set_response(r);

  go_to_executing();
  CollisionCheckResultDTO check;
  check.in_collision = true;

  policy.handle_collision(check);
  EXPECT_EQ(invalidate_count, 0);
}

}  // namespace
}  // namespace manyforge::core
