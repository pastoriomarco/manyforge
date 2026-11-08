// Isolated timing-only build; reuse the real gripper fixtures.
#include "test_ros2_gripper_command_backend.cpp"
#include <manyforge_ros2_control_execution/detail/leaf_timing.hpp>
#include <future>

namespace manyforge::ros2_control_execution {
namespace {
class ExactDelayedGripperServer final {
 public:
  using Action = control_msgs::action::ParallelGripperCommand;
  using Handle = rclcpp_action::ServerGoalHandle<Action>;
  ExactDelayedGripperServer(Ros2TransportResources &owner, const std::string &name) {
    server_ = rclcpp_action::create_server<Action>(owner.node(), name,
      [this](const rclcpp_action::GoalUUID &id, std::shared_ptr<const Action::Goal>) {
        { std::lock_guard lock(mutex_); goal_id_ = id; }
        received = true;
        const auto limit = std::chrono::steady_clock::now() + 3s;
        while (!release.load() && std::chrono::steady_clock::now() < limit)
          std::this_thread::sleep_for(1ms);
        return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
      },
      [this](const std::shared_ptr<Handle> goal) {
        { std::lock_guard lock(mutex_); cancel_id_ = goal->get_goal_id(); }
        ++cancels;
        return rclcpp_action::CancelResponse::ACCEPT;
      }, [this](const std::shared_ptr<Handle> goal) {
        // Keep the goal executing: a dropped server handle aborts the goal.
        std::lock_guard lock(mutex_);
        executing_.push_back(goal);
      });
  }
  bool exact_cancel() {
    std::lock_guard lock(mutex_);
    return goal_id_ && cancel_id_ && *goal_id_ == *cancel_id_;
  }
  std::atomic<bool> received{false}, release{false};
  std::atomic<unsigned> cancels{0};
 private:
  std::mutex mutex_;
  std::optional<rclcpp_action::GoalUUID> goal_id_, cancel_id_;
  std::vector<std::shared_ptr<Handle>> executing_;
  rclcpp_action::Server<Action>::SharedPtr server_;
};

TEST(Od55GripperShutdown, LateAcceptanceBeforeDeadlineCancelsExactGoal) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto owner = test_transport_resources(sink, clock, "arm");
  auto server_owner = test_transport_resources(sink, clock, "server");
  const auto endpoint = "/manyforge_shutdown_late_" + unique_suffix();
  ExactDelayedGripperServer server(*server_owner, endpoint);
  ScopedTestRosOwnerStop server_stop{server_owner};
  server_owner->start();
  Ros2GripperCommandBackend backend(owner, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*owner, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  struct Release { ExactDelayedGripperServer &server; ~Release() { server.release = true; } } release{server};
  ASSERT_TRUE(wait_for_gripper_fact([&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 1.0, "ros_gripper_command", "driver_result"));
  ASSERT_TRUE(command.accepted);
  ASSERT_TRUE(wait_for_gripper_fact([&] { return server.received.load(); }));
  auto close = std::async(std::launch::async, [&] {
    return backend.CloseGripperLateCancels(std::chrono::steady_clock::now() + 2s);
  });
  ASSERT_TRUE(wait_for_gripper_fact([&] { return backend.get_gripper_status("tool").admission_closed; }));
  EXPECT_EQ(server.cancels.load(), 0U);
  server.release = true;
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    scheduler.advance_to(clock.now(), core::SchedulerPriority::StopControl);
    return server.cancels.load() == 1U;
  }));
  EXPECT_TRUE(server.exact_cancel());
  ASSERT_EQ(close.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(close.get(), Ros2GripperCommandBackend::LateCancelShutdownStatus::kSettled);
  EXPECT_EQ(server.cancels.load(), 1U);
}

TEST(Od55GripperShutdown, BlockedActionLeafReturnsStuckAndRetainsOwner) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto owner = test_transport_resources(sink, clock, "arm");
  auto server_owner = test_transport_resources(sink, clock, "server");
  const auto endpoint = "/manyforge_shutdown_leaf_" + unique_suffix();
  ExactDelayedGripperServer server(*server_owner, endpoint);
  server.release = true;
  ScopedTestRosOwnerStop server_stop{server_owner};
  server_owner->start();
  Ros2GripperCommandBackend backend(owner, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*owner, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact([&] { return backend.gripper_transport_readiness("tool").ready; }));
  detail::gripper_goal_entered = false;
  detail::release_gripper_goal = false;
  detail::block_gripper_goal = true;
  struct Release { ~Release() { detail::release_gripper_goal = true; detail::block_gripper_goal = false; } } release;
  auto send = std::async(std::launch::async, [&] {
    return backend.command_gripper(make_gripper_request(0.0, 0.2, 1.0,
                                                       "ros_gripper_command", "driver_result"));
  });
  ASSERT_TRUE(wait_for_gripper_fact([] { return detail::gripper_goal_entered.load(); }));
  const std::weak_ptr<Ros2TransportResources> pinned = owner;
  EXPECT_EQ(backend.CloseGripperLateCancels(std::chrono::steady_clock::now() + 150ms),
            Ros2GripperCommandBackend::LateCancelShutdownStatus::kStuck);
  EXPECT_FALSE(pinned.expired());
  EXPECT_FALSE(owner->joined());
  EXPECT_TRUE(owner->cached_context_valid());
  EXPECT_FALSE(server.received.load());
  detail::release_gripper_goal = true;
  ASSERT_EQ(send.wait_for(3s), std::future_status::ready);
  EXPECT_FALSE(send.get().accepted);
  EXPECT_FALSE(server.received.load());
}

void RunStagedRebindPublisherCase(bool commit_succeeds) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto old_owner = test_transport_resources(sink, clock, "old_gripper");
  auto replacement = test_transport_resources(sink, clock, "new_gripper");
  old_owner->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(old_owner, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*old_owner, backend);
  const auto topic = "/manyforge_rebind_publisher_" + unique_suffix();
  backend.configure_gripper_action("tool", isaac_gripper_config(topic), 1'000'000'000);
  backend.complete_ros_configuration();
  auto stage = backend.stage_ros_rebind(replacement);
  ASSERT_TRUE(stage);
  struct CandidateStop { std::shared_ptr<Ros2TransportResources> owner; ~CandidateStop() { owner->quiesce(); } } candidate_stop{replacement};
  replacement->start();
  lifetime.start();
  const auto old_leaf = Ros2GripperCommandBackendTestPeer::publisher_leaf(backend, "tool");
  ASSERT_TRUE(old_leaf);
  detail::gripper_publish_attempts = 0;
  detail::gripper_publish_sdk_entries = 0;
  detail::gripper_publish_entered = false;
  detail::release_gripper_publish = false;
  detail::block_gripper_publish = true;
  struct Release { ~Release() { detail::release_gripper_publish = true; detail::block_gripper_publish = false; } } release;
  auto first = std::async(std::launch::async, [&] {
    return backend.command_gripper(make_gripper_request(0.0, 0.2, 0.0,
                                                       "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(wait_for_gripper_fact([] { return detail::gripper_publish_entered.load(); }));
  auto readiness = std::async(std::launch::async, [&] { return backend.gripper_transport_readiness("tool"); });
  const bool readiness_returned = readiness.wait_for(200ms) == std::future_status::ready;
  EXPECT_TRUE(readiness_returned) << "cached readiness waited for the publisher leaf";
  if (readiness_returned) (void)readiness.get();
  // Invalidate the first exact generation, then issue a new one while the
  // original caller holds the leaf. Both calls are real backend submissions.
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  ASSERT_TRUE(backend.try_authorize_gripper_commands().ok());
  auto contender = std::async(std::launch::async, [&] {
    return backend.command_gripper(make_gripper_request(0.7, 0.2, 0.0,
                                                       "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(wait_for_gripper_fact([] { return detail::gripper_publish_attempts.load() >= 2U; }));
  ASSERT_TRUE(old_owner->notify_owner());
  ASSERT_TRUE(wait_for_gripper_fact([&] { return old_owner->executor_fault().has_value(); }));
  old_owner->quiesce();
  ASSERT_TRUE(old_owner->joined());
  bool invoked = false;
  EXPECT_FALSE(backend.commit_ros_rebind_with(stage, [](void *context) noexcept {
    *static_cast<bool *>(context) = true;
    return true;
  }, &invoked));
  EXPECT_FALSE(invoked) << "precommit must not cross an in-flight old publisher";
  EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::publisher_open(old_leaf)) << "failed precommit must restore the old leaf fence";
  auto commit = std::async(std::launch::async, [&] {
    return backend.commit_ros_rebind_with(stage, [](void *context) noexcept {
      return *static_cast<bool *>(context);
    }, &commit_succeeds);
  });
  ASSERT_TRUE(wait_for_gripper_fact([&] { return Ros2GripperCommandBackendTestPeer::publisher_fenced(old_leaf); }));
  detail::release_gripper_publish = true;
  ASSERT_EQ(first.wait_for(3s), std::future_status::ready);
  (void)first.get();  // The failed owner may leave the admitted outcome uncertain.
  ASSERT_EQ(contender.wait_for(3s), std::future_status::ready);
  const auto parked_result = contender.get();
  EXPECT_FALSE(parked_result.accepted);
  ASSERT_TRUE(parked_result.error.has_value());
  EXPECT_TRUE(parked_result.error->code == "ros2_control_gripper_bridge_not_ready" ||
              (!commit_succeeds && parked_result.error->code ==
                  ros2_control_fault_reason::kGripperGoalDispatchFailed) ||
              parked_result.error->code == "gripper_command_not_authorized");
  ASSERT_EQ(commit.wait_for(3s), std::future_status::ready);
  EXPECT_EQ(commit.get(), commit_succeeds);
  if (!commit_succeeds) {
    EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::publisher_open(old_leaf));
    EXPECT_TRUE(backend.commit_ros_rebind(stage).ok());
  }
  EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::publisher_retired(old_leaf));
  EXPECT_EQ(detail::gripper_publish_sdk_entries.load(), 0U);
  if (!readiness_returned) { ASSERT_EQ(readiness.wait_for(3s), std::future_status::ready); (void)readiness.get(); }
}

TEST(Od55GripperShutdown, StagedRebindFencesBlockedIsaacPublisherAndRestoresOnFailure) {
  RunStagedRebindPublisherCase(true);
}

TEST(Od55GripperShutdown, ParkedIsaacSenderSurvivesFailedCommitRollback) {
  RunStagedRebindPublisherCase(false);
}

TEST(Od55GripperShutdown, ConcurrentCloseCannotBeUndoneByFailedRebindCommit) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto old_owner = test_transport_resources(sink, clock, "old_gripper");
  auto replacement = test_transport_resources(sink, clock, "new_gripper");
  old_owner->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(old_owner, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*old_owner, backend);
  backend.configure_gripper_action("tool", isaac_gripper_config("/manyforge_close_commit_" + unique_suffix()), 1'000'000'000);
  backend.complete_ros_configuration();
  auto stage = backend.stage_ros_rebind(replacement);
  const auto old_leaf = Ros2GripperCommandBackendTestPeer::publisher_leaf(backend, "tool");
  ASSERT_TRUE(stage);
  ASSERT_TRUE(old_leaf);
  struct CandidateStop { std::shared_ptr<Ros2TransportResources> owner; ~CandidateStop() { owner->quiesce(); } } candidate_stop{replacement};
  replacement->start();
  lifetime.start();
  ASSERT_TRUE(old_owner->notify_owner());
  ASSERT_TRUE(wait_for_gripper_fact([&] { return old_owner->executor_fault().has_value(); }));
  old_owner->quiesce();
  detail::block_gripper_rebind_rollback = true;
  detail::gripper_rebind_rollback_entered = false;
  detail::release_gripper_rebind_rollback = false;
  detail::gripper_rebind_rollback_cas_failed = false;
  struct Release { ~Release() {
    detail::release_gripper_rebind_rollback = true;
    detail::block_gripper_rebind_rollback = false;
  } } release;
  auto commit = std::async(std::launch::async, [&] {
    return backend.commit_ros_rebind_with(stage, [](void *) noexcept { return false; }, nullptr);
  });
  ASSERT_TRUE(wait_for_gripper_fact([] { return detail::gripper_rebind_rollback_entered.load(); }));
  EXPECT_EQ(backend.CloseGripperLateCancels(std::chrono::steady_clock::now() + 2s),
            Ros2GripperCommandBackend::LateCancelShutdownStatus::kSettled);
  EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::publisher_retired(old_leaf));
  EXPECT_FALSE(backend.gripper_transport_readiness("tool").command_path_ready);
  EXPECT_FALSE(backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed")).accepted);
  detail::release_gripper_rebind_rollback = true;
  ASSERT_EQ(commit.wait_for(3s), std::future_status::ready);
  EXPECT_FALSE(commit.get());
  EXPECT_TRUE(detail::gripper_rebind_rollback_cas_failed.load());
  EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::publisher_retired(old_leaf));
}

TEST(Od55GripperShutdown, ProjectionSubmitAfterStopIsDropped) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto owner = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(owner, sink, clock, scheduler, false);
  auto &worker = Ros2GripperCommandBackendTestPeer::worker(backend);
  std::atomic<unsigned> completions{0};
  const auto slot = worker.register_joint_slot(
      "gripper", "finger_joint", [&](const detail::SceneProjectionCompletion &) { ++completions; });
  ASSERT_TRUE(backend.StopSceneProjectionUntil(std::chrono::steady_clock::now() + 1s));
  EXPECT_FALSE(worker.submit_terminal_joint(slot, 0.25, clock.now(), {1, 1, 1}));
  EXPECT_TRUE(backend.StopSceneProjectionUntil(std::chrono::steady_clock::now() + 1s));
  EXPECT_TRUE(sink->single_updates().empty());
  EXPECT_EQ(completions.load(), 0U);
}

TEST(Od55GripperShutdown, ProjectionStopTimeoutRetainsWorkerAndRetriesAfterSinkRelease) {
  auto sink = std::make_shared<SequencedProjectionSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto owner = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(owner, sink, clock, scheduler, false);
  auto &worker = Ros2GripperCommandBackendTestPeer::worker(backend);
  const auto slot = worker.register_joint_slot("gripper", "finger_joint", {});
  struct Release { std::shared_ptr<SequencedProjectionSink> sink; ~Release() { sink->release(1); } } release{sink};
  ASSERT_TRUE(worker.submit_terminal_joint(slot, 0.25, clock.now(), {1, 1, 1}));
  ASSERT_TRUE(sink->wait_entered(1));
  EXPECT_FALSE(backend.StopSceneProjectionUntil(std::chrono::steady_clock::now() + 20ms));
  EXPECT_TRUE(owner->cached_context_valid());
  EXPECT_FALSE(owner->joined());
  EXPECT_FALSE(worker.submit_terminal_joint(slot, 0.3, clock.now(), {1, 1, 2}));
  sink->release(1);
  EXPECT_TRUE(backend.StopSceneProjectionUntil(std::chrono::steady_clock::now() + 2s));
  EXPECT_FALSE(worker.submit_terminal_joint(slot, 0.4, clock.now(), {1, 1, 3}));
}
} // namespace
} // namespace manyforge::ros2_control_execution
