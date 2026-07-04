#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace manyforge::core {
namespace {

CollisionValidationDecisionBundle make_runtime_collision_bundle(
    const CollisionPairId& pair,
    double signed_clearance_m) {
  CollisionResult result;
  result.in_collision = signed_clearance_m < 0.0;
  result.decision = result.in_collision ? CollisionDecisionResult::kFail
                                        : CollisionDecisionResult::kPass;
  result.engine_status = CollisionEngineStatus::kOk;
  if (result.in_collision) {
    result.normalized_pairs = {pair};
    result.normalized_pairs_classified.push_back(CollisionPairResult{
        pair, CollisionPairClassification::kBlockingCollision, true});
  }
  result.pair_clearances = {CollisionPairClearance{pair, signed_clearance_m}};

  CollisionValidationWaypointRecord record;
  record.waypoint_index = 0;
  record.result = std::move(result);

  CollisionValidationDecisionBundle bundle;
  bundle.decision.accepted = !record.result.in_collision;
  bundle.decision.severity = record.result.in_collision
                                 ? CollisionDecisionSeverity::kBlock
                                 : CollisionDecisionSeverity::kNone;
  bundle.decision.action = record.result.in_collision
                               ? CollisionDecisionAction::kReject
                               : CollisionDecisionAction::kDispatch;
  bundle.borrowed_world_revision = CollisionWorldRevision{17, "runtime-17"};
  bundle.waypoint_records.push_back(std::move(record));
  return bundle;
}

CollisionPolicySnapshot make_runtime_contact_grasp_policy() {
  CollisionPolicySnapshot policy;
  policy.require_full_diagnostics = true;
  ContactGraspPolicyEntry entry;
  entry.policy_id = "contact_grasp/part";
  entry.selected_object_id = "part";
  entry.allowed_robot_links = {"finger_left", "finger_right"};
  entry.max_penetration_m = 0.003;
  entry.terminal_window_start_fraction = 0.75;
  policy.contact_grasps.push_back(std::move(entry));
  return policy;
}

CollisionPolicySnapshot make_runtime_contact_release_policy(
    const CollisionPairId& pair) {
  CollisionPolicySnapshot policy;
  SupportContactPolicyEntry entry;
  entry.pair = pair;
  entry.mode = CollisionContactPolicyMode::kContactRelease;
  entry.release_tolerance_m = 1e-5;
  entry.max_start_penetration_m = 0.01;
  entry.min_separation_clearance_m = 0.0;
  policy.support_contacts.push_back(std::move(entry));
  return policy;
}

TEST(RuntimeCollisionMonitorTest, StartSchedulesTickAndReschedulesWhileActive) {
  FakeClock clock;
  ManualScheduler scheduler;
  int tick_count = 0;
  bool keep_running = true;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        return Result<void>::Ok();
      },
      [&]() { return keep_running; }};

  monitor.start();
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 2);

  keep_running = false;
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 3);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 3);
}

TEST(RuntimeCollisionMonitorTest, StopCancelsPendingTick) {
  FakeClock clock;
  ManualScheduler scheduler;
  int tick_count = 0;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        return Result<void>::Ok();
      },
      []() { return true; }};

  monitor.start();
  monitor.stop();

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 0);
}

TEST(RuntimeCollisionMonitorTest, RestartInvalidatesPreviouslyScheduledTick) {
  FakeClock clock;
  ManualScheduler scheduler;
  int tick_count = 0;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        return Result<void>::Ok();
      },
      []() { return false; }};

  monitor.start();
  monitor.start();

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);
}

TEST(RuntimeCollisionMonitorTest, StopDrainWaitsForInFlightCallback) {
  FakeClock clock;
  ManualScheduler scheduler;
  std::promise<void> callback_entered;
  auto callback_entered_future = callback_entered.get_future();
  std::promise<void> release_callback;
  auto release_callback_future = release_callback.get_future();
  std::atomic<bool> stop_returned{false};

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        callback_entered.set_value();
        release_callback_future.wait();
        return Result<void>::Ok();
      },
      []() { return true; }};

  monitor.start();
  clock.advance(10'000'000);
  std::thread scheduler_thread([&]() { scheduler.advance_to(clock.now()); });
  ASSERT_EQ(callback_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  std::thread stop_thread([&]() {
    monitor.stop(/*drain_callbacks=*/true);
    stop_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(stop_returned.load(std::memory_order_acquire));

  release_callback.set_value();
  stop_thread.join();
  scheduler_thread.join();
  EXPECT_TRUE(stop_returned.load(std::memory_order_acquire));
}

TEST(RuntimeCollisionMonitorTest, StopFromInsideCallbackDoesNotDeadlock) {
  FakeClock clock;
  ManualScheduler scheduler;
  RuntimeCollisionMonitor* monitor_ptr = nullptr;
  int tick_count = 0;

  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      [&]() {
        ++tick_count;
        monitor_ptr->stop(/*drain_callbacks=*/true);
        return Result<void>::Ok();
      },
      []() { return true; }};
  monitor_ptr = &monitor;

  monitor.start();
  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);

  clock.advance(10'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(tick_count, 1);
}

TEST(RuntimeCollisionMonitorTest,
     ContactGraspPolicyAllowsMatchingBoundedRuntimeContact) {
  FakeClock clock;
  ManualScheduler scheduler;
  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      []() { return Result<void>::Ok(); },
      []() { return true; }};

  const CollisionPairId pair{"object/part/shape/0",
                             "robot/robot1/link/finger_left/collision/0"};
  auto policy = make_runtime_contact_grasp_policy();
  auto bundle = make_runtime_collision_bundle(pair, -0.001);

  monitor.set_active_contact_policy_id("contact_grasp/part");
  auto fault = monitor.apply_active_contact_grasp_policy(policy, bundle);

  ASSERT_FALSE(fault.has_value());
  ASSERT_EQ(bundle.waypoint_records.size(), 1);
  EXPECT_TRUE(bundle.decision.accepted);
  EXPECT_EQ(bundle.decision.action, CollisionDecisionAction::kDispatch);
  EXPECT_EQ(bundle.waypoint_records.front()
                .result.normalized_pairs_classified.front()
                .classification,
            CollisionPairClassification::kAllowedContactGrasp);
  EXPECT_EQ(bundle.waypoint_records.front().result.raw_diagnostics.at(
                "contact_grasp_allowed_pair"),
            "object/part/shape/0 <-> "
            "robot/robot1/link/finger_left/collision/0");
}

TEST(RuntimeCollisionMonitorTest,
     ContactReleasePolicyRejectsIncreasingPenetration) {
  FakeClock clock;
  ManualScheduler scheduler;
  RuntimeCollisionMonitor monitor{
      clock,
      scheduler,
      RuntimeCollisionMonitor::Config{10'000'000},
      []() { return Result<void>::Ok(); },
      []() { return true; }};

  const CollisionPairId pair{"object/part/shape/0",
                             "robot/robot1/link/finger_left/collision/0"};
  auto policy = make_runtime_contact_release_policy(pair);
  auto first = make_runtime_collision_bundle(pair, -0.001);
  auto second = make_runtime_collision_bundle(pair, -0.003);

  EXPECT_FALSE(monitor.update_contact_release_state(policy, first).has_value());
  auto fault = monitor.update_contact_release_state(policy, second);

  ASSERT_TRUE(fault.has_value());
  EXPECT_EQ(fault->at("reason"),
            "runtime_contact_release_penetration_increased");
  EXPECT_EQ(fault->at("contact_release_pair"),
            "object/part/shape/0 <-> "
            "robot/robot1/link/finger_left/collision/0");
}

}  // namespace
}  // namespace manyforge::core
