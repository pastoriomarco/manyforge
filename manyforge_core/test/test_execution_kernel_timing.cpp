// Isolated timing build reuses the production Kernel fixtures; product is uninstrumented.
#include "test_execution_kernel.cpp"
#include <manyforge_core/detail/lookahead_retirement_timing.hpp>

namespace manyforge::core {
namespace {
class ExecutionKernelTiming : public ExecutionKernelTest {};
bool WaitTiming(const std::function<bool()> &predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::yield();
  }
  return predicate();
}

TEST_F(ExecutionKernelTiming, DetachWaitsForBothRetiredLookaheadSessions) {
  detail::lookahead_retirement_entries = 0;
  detail::lookahead_retirement_release[0] = false;
  detail::lookahead_retirement_release[1] = false;
  detail::lookahead_retirement_latch_timeout = false;
  std::atomic<unsigned> callbacks{0};
  auto controlled = std::make_shared<ControlledExecution>();
  controlled->enable_execution_progress_publication();
  ExecutionKernelConfig config;
  config.runtime_collision_lookahead_workers = 1;
  config.runtime_collision_lookahead_workers_declared = true;
  ExecutionKernel owner{planner_owner, test_execution_kernel_ports(context_owner), controlled,
                        clock_owner, scheduler, cache_owner, config};
  auto initial = make_owned_collision_pipeline_resources();
  initial.runtime_lookahead_engines.push_back(std::make_shared<MockCollisionEngine>());
  owner.set_collision_validation_policy_snapshot(make_test_collision_policy_snapshot());
  owner.set_collision_resources(initial.snapshot_publisher, initial.runtime_monitor_engine,
                                initial.validation_engines, initial.runtime_lookahead_engines);
  const ConfigurationFenceId fence_id{"apply:two-retired-lookahead"};
  const auto scope = make_collision_apply_fence_scope();
  FakeCollisionApplyFenceAdmissionGate gate;
  gate.held_fence_id = fence_id;
  gate.held_scope = scope;
  auto candidate_a = make_owned_collision_pipeline_resources();
  auto candidate_b = make_owned_collision_pipeline_resources();
  candidate_a.runtime_lookahead_engines.push_back(std::make_shared<MockCollisionEngine>());
  candidate_b.runtime_lookahead_engines.push_back(std::make_shared<MockCollisionEngine>());
  owner.subscribe_events([&](const KernelEvent &) { ++callbacks; });
  using ReplacementResult = decltype(owner.replace_collision_resources_under_apply_fence(
      fence_id, scope, gate, candidate_a));
  using DetachResult = decltype(owner.detach(std::chrono::steady_clock::now()));
  std::future<ReplacementResult> first, second;
  std::future<DetachResult> detached;
  struct ReleaseAll {
    ~ReleaseAll() {
      detail::lookahead_retirement_release[0] = true;
      detail::lookahead_retirement_release[1] = true;
    }
  } release;
  first = std::async(std::launch::async, [&] {
    return owner.replace_collision_resources_under_apply_fence(fence_id, scope, gate, candidate_a);
  });
  ASSERT_TRUE(WaitTiming([] { return detail::lookahead_retirement_entries.load() >= 1U; }));
  second = std::async(std::launch::async, [&] {
    return owner.replace_collision_resources_under_apply_fence(fence_id, scope, gate, candidate_b);
  });
  ASSERT_TRUE(WaitTiming([] { return detail::lookahead_retirement_entries.load() >= 2U; }));
  detached = std::async(std::launch::async, [&] {
    return owner.detach(std::chrono::steady_clock::now() + std::chrono::seconds(3));
  });
  EXPECT_EQ(detached.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  detail::lookahead_retirement_release[0] = true;
  ASSERT_EQ(first.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(first.get().ok());
  EXPECT_EQ(detached.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  detail::lookahead_retirement_release[1] = true;
  ASSERT_EQ(second.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(second.get().ok());
  ASSERT_EQ(detached.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(detached.get().ok());
  EXPECT_FALSE(detail::lookahead_retirement_latch_timeout.load());
  const auto after_detach = callbacks.load();
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(callbacks.load(), after_detach);
}

TEST_F(ExecutionKernelTiming, DetachedKernelRefusesParkedLauncher) {
  detail::pre_execute_enabled = true;
  detail::pre_execute_entered = false;
  detail::pre_execute_release = false;
  detail::pre_execute_timeout = false;
  using LaunchResult = decltype(kernel.PrepareMotion(
      prepared_motion_request(make_test_request("detached_parked_launcher"))));
  std::future<LaunchResult> launch;
  struct Release {
    ~Release() { detail::pre_execute_release = true; detail::pre_execute_enabled = false; }
  } release;
  launch = std::async(std::launch::async, [&] {
    return kernel.PrepareMotion(prepared_motion_request(make_test_request("detached_parked_launcher")));
  });
  ASSERT_TRUE(WaitTiming([] { return detail::pre_execute_entered.load(); }));
  const auto detached = kernel.detach(std::chrono::steady_clock::now() + std::chrono::seconds(2));
  EXPECT_TRUE(detached.ok());
  detail::pre_execute_release = true;
  ASSERT_EQ(launch.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  const auto result = launch.get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), "scheduler_not_ready");
  EXPECT_EQ(planner.plan_count, 0);
  EXPECT_EQ(execution.execute_count, 0);
  EXPECT_FALSE(detail::pre_execute_timeout.load());
}
} // namespace
} // namespace manyforge::core
