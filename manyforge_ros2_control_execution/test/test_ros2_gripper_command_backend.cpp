// Copyright 2026 Marco Pastorio
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ros2_transport_test_support.hpp"
// Test-only invocation of the actual Core request producer; do not copy its
// logic.
#include "../../manyforge_core/src/resource_addressed_execution_kernel_internal.hpp"
#include <chrono>
#include <future>
#include <thread>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>
namespace manyforge::ros2_control_execution {
class Ros2GripperCommandBackendTestPeer final {
public:
  static void add_failed_handoff(Ros2GripperCommandBackend &backend,
      std::function<core::Result<void>(std::uint64_t, std::uint64_t, std::uint64_t,
                                       const core::CommandTargetId&, const std::string&)> hook) {
    auto slot = std::make_shared<core::GripperLateCancelSlot>();
    slot->state = core::GripperLateCancelState::kFailed;
    slot->preparation_id = 1;
    slot->command_id = 2;
    slot->generation = 3;
    slot->gripper_id = "tool";
    slot->target = core::CommandTargetId("target");
    slot->receipt_installed = true;
    slot->handoff = std::move(hook);
    slot->state_changed = [weak = std::weak_ptr(backend.late_cancel_tracking_)] {
      if (const auto tracking = weak.lock()) {
        std::lock_guard lock(tracking->mutex);
        tracking->condition.notify_all();
      }
    };
    std::lock_guard lock(backend.late_cancel_tracking_->mutex);
    backend.late_cancel_tracking_->entries.push_back({"tool", slot, {}, {}, {}});
  }
  static bool inject_late_cancel_failure(Ros2GripperCommandBackend &backend,
                                         const std::string &id) {
    std::shared_ptr<core::GripperLateCancelSlot> slot;
    {
      std::lock_guard map_lock(backend.gripper_map_mutex_);
      const auto found = backend.grippers_.find(id);
      if (found == backend.grippers_.end()) return false;
      std::lock_guard runtime_lock(found->second->mutex);
      slot = found->second->goal_late_cancel_slot;
    }
    if (!slot) return false;
    std::lock_guard slot_lock(slot->mutex);
    if (slot->state != core::GripperLateCancelState::kPending || !slot->failure_epoch) return false;
    slot->state = core::GripperLateCancelState::kFailed;
    slot->failure_epoch->fetch_add(1, std::memory_order_release);
    return true;
  }
  static detail::SceneProjectionWorker &worker(Ros2GripperCommandBackend &backend) {
    return *backend.scene_projection_worker_;
  }
  static auto publisher_leaf(Ros2GripperCommandBackend &backend, const std::string &id) {
    std::lock_guard map_lock(backend.gripper_map_mutex_);
    const auto found = backend.grippers_.find(id);
    if (found == backend.grippers_.end())
      return std::shared_ptr<Ros2GripperCommandBackend::RosEntities::PublisherLeaf>{};
    std::lock_guard runtime_lock(found->second->mutex);
    return found->second->ros->publisher_leaf;
  }
  static bool publisher_open(const auto &leaf) {
    return leaf->state.load() == Ros2GripperCommandBackend::RosEntities::PublisherLeaf::State::kOpen;
  }
  static bool publisher_fenced(const auto &leaf) {
    return leaf->state.load() == Ros2GripperCommandBackend::RosEntities::PublisherLeaf::State::kRebindFenced;
  }
  static bool publisher_retired(const auto &leaf) {
    return leaf->state.load() == Ros2GripperCommandBackend::RosEntities::PublisherLeaf::State::kRetired;
  }
  static bool goal_accepted(Ros2GripperCommandBackend &backend, const std::string &id) {
    std::lock_guard map_lock(backend.gripper_map_mutex_);
    const auto found = backend.grippers_.find(id);
    if (found == backend.grippers_.end()) return false;
    std::lock_guard runtime_lock(found->second->mutex);
    return found->second->goal_accepted && found->second->active_goal_handle;
  }
  static bool submit_projection(Ros2GripperCommandBackend &backend,
                                const std::string &id, double position,
                                bool terminal = true) {
    std::lock_guard map_lock(backend.gripper_map_mutex_);
    const auto found = backend.grippers_.find(id);
    if (found == backend.grippers_.end()) return false;
    std::lock_guard runtime_lock(found->second->mutex);
    return backend.schedule_gripper_scene_projection_locked(found->second, position, terminal);
  }
  static bool projection_submitted(Ros2GripperCommandBackend &backend, const std::string &id) {
    std::lock_guard map_lock(backend.gripper_map_mutex_);
    const auto found = backend.grippers_.find(id);
    if (found == backend.grippers_.end()) return false;
    std::lock_guard runtime_lock(found->second->mutex);
    return found->second->scene_projection_submitted;
  }
};
namespace {
bool wait_for_gripper_fact(const std::function<bool()> &predicate);
TEST(Ros2GripperCommandBackendTest, CloseWithNoTrackedGoalsIsSettled) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  EXPECT_EQ(backend.CloseGripperLateCancels(std::chrono::steady_clock::now() + std::chrono::seconds(1)),
            Ros2GripperCommandBackend::LateCancelShutdownStatus::kSettled);
}

TEST(Ros2GripperCommandBackendTest, TerminalCloseRefusesReauthorizationAndPneumaticDisplay) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  auto config = isaac_gripper_config("/manyforge_terminal_close_" + unique_suffix());
  config.actuation_kind = core::GripperActuationKind::kPneumatic;
  config.completion_policy = "output_delivery";
  std::atomic<unsigned> displays{0};
  config.publish_display_position = [&](double) {
    ++displays;
    return core::Result<void>::Ok();
  };
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  lifetime.start();
  ASSERT_EQ(backend.CloseGripperLateCancels(std::chrono::steady_clock::now() + std::chrono::seconds(1)),
            Ros2GripperCommandBackend::LateCancelShutdownStatus::kSettled);
  for (const auto reason : {core::GripperInvalidationReason::kReset,
                            core::GripperInvalidationReason::kStop}) {
    backend.invalidate_gripper_commands(reason);
    const auto reopened = backend.try_authorize_gripper_commands();
    ASSERT_FALSE(reopened.ok());
    EXPECT_EQ(reopened.error().code, "ros2_control_gripper_bridge_not_ready");
  }
  const auto lifecycle = backend.authorize_lifecycle_gripper("tool");
  EXPECT_FALSE(lifecycle.succeeded);
  ASSERT_TRUE(lifecycle.error.has_value());
  EXPECT_EQ(lifecycle.error->code, "ros2_control_gripper_bridge_not_ready");
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "output_delivery"));
  EXPECT_FALSE(command.accepted);
  EXPECT_EQ(displays.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest, CloseDoesNotWaitPastDeadlineForJournalHandoff) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  std::promise<void> entered_promise;
  auto entered = entered_promise.get_future();
  std::promise<void> release_promise;
  auto release = release_promise.get_future().share();
  Ros2GripperCommandBackendTestPeer::add_failed_handoff(backend,
      [&](std::uint64_t, std::uint64_t, std::uint64_t,
          const core::CommandTargetId&, const std::string&) {
        entered_promise.set_value();
        release.wait();
        return core::Result<void>::Ok();
      });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
  auto close = std::async(std::launch::async, [&] { return backend.CloseGripperLateCancels(deadline); });
  const auto queue_limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!scheduler.pending_count(core::SchedulerPriority::StopControl) &&
         std::chrono::steady_clock::now() < queue_limit) std::this_thread::yield();
  const bool queued = scheduler.pending_count(core::SchedulerPriority::StopControl) != 0;
  auto worker = std::async(std::launch::async, [&] { scheduler.advance_to(clock.now()); });
  const bool writer_entered = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  const bool returned_while_blocked = close.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  release_promise.set_value();
  worker.wait();
  EXPECT_TRUE(queued);
  EXPECT_TRUE(writer_entered);
  ASSERT_TRUE(returned_while_blocked);
  EXPECT_EQ(close.get(), Ros2GripperCommandBackend::LateCancelShutdownStatus::kPendingOrFailed);
}

TEST(Ros2GripperCommandBackendTest, BackendPinsItsExactTransportOwner) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  const std::weak_ptr<Ros2TransportResources> weak_owner = resources;
  auto backend = std::make_unique<Ros2GripperCommandBackend>(
      resources, sink, clock, scheduler, false);
  resources.reset();
  ASSERT_FALSE(weak_owner.expired());
  backend->shutdown();
  backend.reset();
  EXPECT_TRUE(weak_owner.expired());
}

TEST(Ros2GripperCommandBackendTest, StagingRebindKeepsCurrentRuntimeAndOwner) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> owner =
      test_transport_resources(sink, clock, "arm");
  auto candidate = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "arm"));
  Ros2GripperCommandBackend backend(owner, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*owner, backend);
  Ros2GripperActionConfig config;
  config.state_freshness_timeout_s = 1.0;
  config.action_name = "/manyforge_rebind_stage_" + unique_suffix();
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  const auto before = backend.get_gripper_status("tool");
  auto stage = backend.stage_ros_rebind(candidate);
  ASSERT_TRUE(stage);
  EXPECT_FALSE(backend.commit_ros_rebind(stage).ok());
  EXPECT_FALSE(backend.activate_ros_rebind(stage).ok());
  const auto after = backend.get_gripper_status("tool");
  EXPECT_EQ(after.command_id, before.command_id);
  EXPECT_EQ(after.generation, before.generation);
  EXPECT_EQ(after.revision, before.revision);
  EXPECT_EQ(after.state, before.state);
  EXPECT_THROW(backend.stage_ros_rebind(owner), std::invalid_argument);
  stage.reset();
  candidate->quiesce();
}

TEST(Ros2GripperCommandBackendTest, RebindFeedbackIsGatedUntilActivation) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::atomic<std::size_t> observed{0};
  std::shared_ptr<Ros2TransportResources> owner =
      test_transport_resources(sink, clock, "arm");
  auto candidate = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "arm"));
  owner->set_owner_notification([] { throw std::runtime_error("test owner fault"); });
  Ros2GripperCommandBackend backend(owner, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*owner, backend);
  const auto topic = "/manyforge_rebind_feedback_" + unique_suffix();
  auto config = isaac_gripper_config(topic + "/commands");
  config.state_topic = topic;
  config.state_joint_name = "finger_joint";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  auto stage = backend.stage_ros_rebind(candidate);
  const auto old_leaf = Ros2GripperCommandBackendTestPeer::publisher_leaf(backend, "tool");
  ASSERT_TRUE(old_leaf);
  auto publisher = candidate->node()->create_publisher<sensor_msgs::msg::JointState>(
      topic, rclcpp::SystemDefaultsQoS());
  auto probe = candidate->node()->create_subscription<sensor_msgs::msg::JointState>(
      topic, rclcpp::SystemDefaultsQoS(),
      [&](const sensor_msgs::msg::JointState::SharedPtr) { ++observed; });
  struct CandidateStop final {
    std::shared_ptr<Ros2TransportResources> owner;
    ~CandidateStop() { owner->quiesce(); }
  } candidate_stop{candidate};
  candidate->start();
  owner->start();
  ASSERT_TRUE(owner->notify_owner());
  ASSERT_TRUE(wait_for_gripper_fact([&] { return owner->executor_fault().has_value(); }));
  owner->quiesce();
  bool invoked = false;
  EXPECT_FALSE(backend.commit_ros_rebind_with(stage, [](void *context) noexcept {
    *static_cast<bool *>(context) = true;
    return false;
  }, &invoked));
  EXPECT_TRUE(invoked);
  EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::publisher_open(old_leaf));
  ASSERT_TRUE(backend.commit_ros_rebind(stage).ok());
  EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::publisher_retired(old_leaf));
  sensor_msgs::msg::JointState sample;
  sample.name = {"finger_joint"};
  sample.position = {0.25};
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    publisher->publish(sample);
    return observed.load() > 0;
  }));
  EXPECT_EQ(backend.gripper_transport_readiness("tool").feedback_messages_received, 0U);
  ASSERT_TRUE(backend.activate_ros_rebind(stage).ok());
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    publisher->publish(sample);
    return backend.gripper_transport_readiness("tool").feedback_messages_received > 0;
  }));
  backend.shutdown();
  candidate->quiesce();
}

TEST(Ros2GripperCommandBackendTest, ProjectionWorkerSurvivesTransportQuiesce) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  std::promise<void> completed;
  auto future = completed.get_future();
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  auto &worker = Ros2GripperCommandBackendTestPeer::worker(backend);
  const auto slot = worker.register_joint_slot(
      "gripper", "finger_joint", [&](const detail::SceneProjectionCompletion &result) {
        if (result.outcome == detail::SceneProjectionOutcome::kApplied)
          completed.set_value();
      });

  resources->quiesce();
  ASSERT_TRUE(worker.submit_terminal_joint(slot, 0.25, clock.now(), {1, 1, 1}));
  ASSERT_EQ(future.wait_for(1s), std::future_status::ready);
  ASSERT_TRUE(worker.drain_for(1s));
  ASSERT_FALSE(sink->single_updates().empty());
  EXPECT_DOUBLE_EQ(sink->single_updates().back().position_rad, 0.25);
}

TEST(Ros2GripperCommandBackendTest,
     GripperProjectionReadsStartPositionFromExecutionStateSink) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  Ros2GripperActionConfig gripper;
  gripper.state_freshness_timeout_s = 1.0;
  gripper.action_name = "/manyforge_missing_gripper_" + unique_suffix();
  gripper.group_id = "gripper";
  gripper.joint_name = "finger_joint";
  gripper.wait_for_action_server_timeout_s = 0.0;
  backend.configure_gripper_action("tool", gripper, 1'000'000'000);

  const auto result = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 1.0, "ros_gripper_command", "position_reached"));

  EXPECT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code,
            ros2_control_fault_reason::kGripperNotConfigured);
  const auto reads = sink->reads();
  ASSERT_EQ(reads.size(), 1u);
  EXPECT_EQ(reads.front().group_id, "gripper");
  EXPECT_EQ(reads.front().joint_name, "finger_joint");
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     GripperTransportReadinessIsNonBlockingAndFailClosed) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  const auto absent = backend.gripper_transport_readiness("missing");
  EXPECT_FALSE(absent.configured);
  EXPECT_FALSE(absent.ready);
  EXPECT_EQ(absent.reason_codes,
            std::vector<std::string>{"gripper_transport_not_configured"});

  Ros2GripperActionConfig gripper;
  gripper.state_freshness_timeout_s = 1.0;
  gripper.action_name =
      "/manyforge_missing_gripper_readiness_" + unique_suffix();
  backend.configure_gripper_action("tool", gripper, 1'000'000'000);
  const auto configured = backend.gripper_transport_readiness("tool");
  EXPECT_TRUE(configured.configured);
  EXPECT_EQ(configured.transport_kind, "ros_gripper_command");
  EXPECT_FALSE(configured.command_path_ready);
  EXPECT_FALSE(configured.feedback_path_ready);
  EXPECT_FALSE(configured.ready);
  EXPECT_EQ(configured.reason_codes,
            std::vector<std::string>{"gripper_action_server_unavailable"});
}

TEST(Ros2GripperCommandBackendTest,
     IsaacGripperFeedbackUsesRawSourceJointIdentity) {
  const std::string suffix = unique_suffix();
  const std::string state_topic =
      "/manyforge_gripper_source_feedback_" + suffix;

  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  auto gripper =
      isaac_gripper_config("/manyforge_gripper_source_command_" + suffix);
  gripper.state_topic = state_topic;
  gripper.command_joint_names = {"right_finger_joint"};
  gripper.state_joint_name = "right_finger_joint";
  gripper.joint_name = "robot_1__right_finger_joint";
  backend.configure_gripper_action("tool", std::move(gripper), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();

  JointStatePublisher publisher(state_topic);
  ASSERT_TRUE(publisher.wait_for_subscription());
  for (int attempt = 0; attempt < 5; ++attempt) {
    publisher.publish({"compiled_only_finger_joint"}, {0.0081});
    std::this_thread::sleep_for(20ms);
  }
  const auto unmatched = backend.gripper_transport_readiness("tool");
  EXPECT_GT(unmatched.feedback_publisher_count, 0U);
  EXPECT_GT(unmatched.feedback_messages_received, 0U);
  EXPECT_EQ(unmatched.feedback_samples_matched, 0U);
  EXPECT_FALSE(unmatched.feedback_path_ready);
  EXPECT_EQ(unmatched.reason_codes,
            (std::vector<std::string>{"gripper_command_subscriber_unavailable",
                                      "gripper_state_feedback_unavailable",
                                      "gripper_state_joint_unobserved"}));

  for (int attempt = 0; attempt < 5; ++attempt) {
    publisher.publish({"right_finger_joint"}, {0.0081});
    std::this_thread::sleep_for(20ms);
  }

  const auto readiness = backend.gripper_transport_readiness("tool");
  EXPECT_TRUE(readiness.configured);
  EXPECT_TRUE(readiness.feedback_path_ready);
  EXPECT_EQ(readiness.state_topic, state_topic);
  EXPECT_EQ(readiness.state_joint_name, "right_finger_joint");
  EXPECT_GT(readiness.feedback_publisher_count, 0U);
  EXPECT_GT(readiness.feedback_messages_received, 0U);
  EXPECT_GT(readiness.feedback_samples_matched, 0U);
  EXPECT_FALSE(readiness.command_path_ready);
  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.reason_codes,
            std::vector<std::string>{"gripper_command_subscriber_unavailable"});
}

TEST(Ros2GripperCommandBackendTest,
     IsaacGripperFeedbackPushesItsDeclaredStreamWhichOwnsStaleness) {
  const std::string suffix = unique_suffix();
  const std::string state_topic = "/manyforge_gripper_stream_feedback_" + suffix;
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  auto clock_owner = std::make_shared<core::FakeClock>();
  auto &clock = *clock_owner;
  clock.set(100'000'000'000LL);
  core::ManualScheduler scheduler;
  core::ComponentStreamHealth health(clock_owner, scheduler);
  auto declared = health.Declare(
      core::ControllableComponentId("robot_1"),
      {"gripper_feedback", 1'000'000'000LL, core::StreamSeverityDTO::kFault,
       core::StreamRecoveryDTO::kAck, core::StreamTimestampSourceDTO::kArrival});
  ASSERT_TRUE(declared.ok());
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  auto gripper = isaac_gripper_config("/manyforge_gripper_stream_command_" + suffix);
  gripper.state_topic = state_topic;
  gripper.command_joint_names = {"right_finger_joint"};
  gripper.state_joint_name = "right_finger_joint";
  gripper.joint_name = "robot_1__right_finger_joint";
  gripper.feedback_stream = std::move(declared.value());
  backend.configure_gripper_action("tool", std::move(gripper), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();

  JointStatePublisher publisher(state_topic);
  ASSERT_TRUE(publisher.wait_for_subscription());
  for (int attempt = 0; attempt < 5; ++attempt) {
    publisher.publish({"right_finger_joint"}, {0.0081});
    std::this_thread::sleep_for(20ms);
  }
  health.Evaluate();
  auto streams = health.Read(core::ControllableComponentId("robot_1"));
  ASSERT_EQ(streams.size(), 1U);
  EXPECT_EQ(streams.front().state, core::StreamLivenessStateDTO::kLive);
  EXPECT_TRUE(backend.gripper_transport_readiness("tool").feedback_path_ready);
  // The stream judges staleness; the readiness snapshot no longer ages it.
  std::this_thread::sleep_for(1100ms);
  clock.advance(2'000'000'000LL);
  health.Evaluate();
  streams = health.Read(core::ControllableComponentId("robot_1"));
  EXPECT_EQ(streams.front().state, core::StreamLivenessStateDTO::kStale);
  EXPECT_TRUE(backend.gripper_transport_readiness("tool").feedback_path_ready);
}

TEST(Ros2GripperCommandBackendTest, ReadyGripperTransportAdmitsCommandWithoutLegacyArmGate) {
  const std::string suffix = unique_suffix();
  const std::string command_topic = "/manyforge_gripper_command_" + suffix;
  const std::string state_topic = "/manyforge_gripper_state_" + suffix;

  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  auto gripper = isaac_gripper_config(command_topic);
  gripper.state_topic = state_topic;
  gripper.state_joint_name = "finger_joint";
  backend.configure_gripper_action("tool", std::move(gripper), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_EQ(backend.cached_gripper_local_owner_ready("tool"), false);

  auto command_subscriber = std::make_unique<JointStateSubscriber>(command_topic);
  JointStatePublisher state_publisher(state_topic);
  ASSERT_TRUE(command_subscriber->wait_for_publisher());
  ASSERT_TRUE(state_publisher.wait_for_subscription());
  for (int attempt = 0; attempt < 100 &&
       backend.cached_gripper_local_owner_ready("tool") != true; ++attempt) {
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_EQ(backend.cached_gripper_local_owner_ready("tool"), true);
  for (int attempt = 0; attempt < 5; ++attempt) {
    state_publisher.publish({"finger_joint"}, {0.7});
    std::this_thread::sleep_for(20ms);
  }

  const auto readiness = backend.gripper_transport_readiness("tool");
  EXPECT_TRUE(readiness.configured);
  EXPECT_TRUE(readiness.command_path_ready);
  EXPECT_TRUE(readiness.feedback_path_ready);
  EXPECT_TRUE(readiness.ready);
  EXPECT_TRUE(readiness.reason_codes.empty());

  const auto accepted = backend.command_gripper(
      make_gripper_request(0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_TRUE(accepted.accepted);
  EXPECT_FALSE(accepted.error.has_value());
  command_subscriber.reset();
  for (int attempt = 0; attempt < 100 &&
       backend.cached_gripper_local_owner_ready("tool") != false; ++attempt) {
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_EQ(backend.cached_gripper_local_owner_ready("tool"), false);
}

TEST(Ros2GripperCommandBackendTest,
     GripperPreflightRejectsCrossedSemanticsWithoutAdmittingAnEffect) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_preflight_gripper_" + unique_suffix()), 1'000'000'000);

  const auto valid = make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed");
  EXPECT_TRUE(backend.validate_gripper_command(valid).ok());
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
  EXPECT_TRUE(sink->single_updates().empty());

  auto crossed = valid;
  crossed.joint_name = "another_finger";
  const auto rejected = backend.validate_gripper_command(crossed);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code,
            core::to_string(core::ErrorCode::kInvalidRequest));
  EXPECT_EQ(rejected.error().details.at("reason"),
            ros2_control_fault_reason::kInvalidGripperSemantics);
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     GripperAcceptsResolvedBindingSemanticsAtThePhysicalTransportBoundary) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_resolved_gripper_" + unique_suffix()), 1'000'000'000);

  const auto request = make_gripper_request(0.0, 0.2, 0.0, "resolved_binding",
                                            "goal_fenced_target_reached");
  EXPECT_TRUE(backend.validate_gripper_command(request).ok());

  const auto accepted = backend.command_gripper(request);
  ASSERT_TRUE(accepted.accepted)
      << (accepted.error ? accepted.error->message : "");
  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_EQ(status.transport_kind, "isaac_joint_state_effort");
  EXPECT_EQ(status.completion_policy, "goal_fenced_target_reached");
}

TEST(Ros2GripperCommandBackendTest,
     GripperPreflightRejectsUnavailableStateAndClosedLifecycle) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_state_gripper_" +
                                   unique_suffix()), 1'000'000'000);
  const auto request = make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed");

  const auto unavailable = backend.validate_gripper_command(request);
  ASSERT_FALSE(unavailable.ok());
  EXPECT_EQ(unavailable.error().details.at("reason"),
            ros2_control_fault_reason::kGripperNotConfigured);

  sink->set_joint_position("gripper", "finger_joint", 0.7);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  const auto closed = backend.validate_gripper_command(request);
  ASSERT_FALSE(closed.ok());
  EXPECT_EQ(closed.error().details.at("reason"),
            "gripper_command_not_authorized");
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     GripperRejectsNonFiniteStartPositionBeforeDispatch) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint",
                           std::numeric_limits<double>::quiet_NaN());
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  auto gripper = isaac_gripper_config("/manyforge_non_finite_start_gripper_" +
                                      unique_suffix());
  backend.configure_gripper_action("tool", gripper, 1'000'000'000);

  const auto result = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));

  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code,
            ros2_control_fault_reason::kGripperStateNotFinite);
  EXPECT_TRUE(sink->single_updates().empty());
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
}

TEST(Ros2GripperCommandBackendTest,
     GripperGraspLossInvalidatesBackendOwnedStatus) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  Ros2GripperActionConfig gripper;
  gripper.state_freshness_timeout_s = 1.0;
  gripper.transport_kind = "isaac_joint_state_effort";
  gripper.command_topic = "/manyforge_lost_grasp_gripper_" + unique_suffix();
  gripper.command_joint_names = {"finger_joint"};
  gripper.group_id = "gripper";
  gripper.joint_name = "finger_joint";
  gripper.completion_policy = "dwell_elapsed";
  gripper.open_position = 0.7;
  gripper.closed_position = 0.0;
  gripper.open_effort = 5.0;
  gripper.close_effort = -5.0;
  gripper.publish_nominal_position = true;
  backend.configure_gripper_action("tool", gripper, 1'000'000'000);

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted)
      << (command.error ? command.error->message : "");

  const auto lost = backend.mark_gripper_grasp_lost(
      "tool", command.command_id, command.generation, "attached_drift_monitor",
      "Attached object drift exceeded measured TCP/object threshold",
      "grasp_confirmation_lost");
  ASSERT_TRUE(lost.accepted) << (lost.error ? lost.error->message : "");

  const auto failed = backend.get_gripper_status("tool");
  EXPECT_EQ(failed.state, core::GripperCommandState::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  EXPECT_EQ(failed.error->code, "grasp_confirmation_lost");
  EXPECT_EQ(failed.evidence_state, "pressure_or_part_lost");
  ASSERT_EQ(failed.evidence.size(), 1u);
  EXPECT_EQ(failed.evidence.front(), "attached_drift_monitor");
  EXPECT_NE(failed.error->message.find("Attached object drift exceeded"),
            std::string::npos);

  const auto next = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(next.accepted) << (next.error ? next.error->message : "");
  scheduler.advance_to(clock.now());
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::worker(backend).drain_for(1s));
  const auto reset = backend.get_gripper_status("tool");
  EXPECT_EQ(reset.state, core::GripperCommandState::kCompleted);
  EXPECT_TRUE(reset.evidence.empty());
  EXPECT_TRUE(reset.evidence_state.empty());
  EXPECT_FALSE(reset.error.has_value());
}

TEST(Ros2GripperCommandBackendTest,
     StallWindowGraspEvidenceIsOptInAndFollowsTheCompletedPosition) {
  const auto complete = [](std::optional<Ros2GripperActionConfig::GraspStallWindow> window,
                           double target) {
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    sink->set_joint_position("gripper", "finger_joint", 0.7);
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
    TestTransportLifetime lifetime(*resources, backend);
    auto config = isaac_gripper_config("/manyforge_stall_window_" + unique_suffix());
    config.grasp_stall_window = window;
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    const auto command = backend.command_gripper(make_gripper_request(
        target, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
    EXPECT_TRUE(command.accepted) << (command.error ? command.error->message : "");
    scheduler.advance_to(clock.now());
    EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::worker(backend).drain_for(1s));
    auto status = backend.get_gripper_status("tool");
    EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
    if (status.grasp_evidence) {
      EXPECT_EQ(status.grasp_evidence->observation.command_id, command.command_id);
      EXPECT_EQ(status.grasp_evidence->source_kind,
                core::GraspConfirmationSourceKind::kForceCurrent);
    }
    return status.grasp_evidence;
  };
  // Default: no window, no grasp evidence (the unchanged status).
  EXPECT_FALSE(complete(std::nullopt, 0.0).has_value());
  const Ros2GripperActionConfig::GraspStallWindow window{0.0, 0.05};
  const auto inside = complete(window, 0.0);
  ASSERT_TRUE(inside.has_value());
  EXPECT_TRUE(inside->observation.confirmed);
  const auto outside = complete(window, 0.7);
  ASSERT_TRUE(outside.has_value());
  EXPECT_FALSE(outside->observation.confirmed);
}

TEST(Ros2GripperCommandBackendTest,
     StaleGripperGraspLossCannotFailReplacementGeneration) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_stale_grasp_loss_" + unique_suffix()), 1'000'000'000);

  const auto old_command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(old_command.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  backend.authorize_gripper_commands();
  const auto replacement = backend.command_gripper(make_gripper_request(
      0.7, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(replacement.accepted);

  const auto stale = backend.mark_gripper_grasp_lost(
      "tool", old_command.command_id, old_command.generation,
      "attached_drift_monitor", "Delayed object drift",
      "grasp_confirmation_lost");
  ASSERT_FALSE(stale.accepted);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "stale_gripper_state");

  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.command_id, replacement.command_id);
  EXPECT_EQ(status.generation, replacement.generation);
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_FALSE(status.error.has_value());
}

TEST(Ros2GripperCommandBackendTest,
     IsaacGripperSchedulerFailureRemainsAdmittedAndPublishesFailure) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  RejectingScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_schedule_failure_" + unique_suffix()), 1'000'000'000);

  const auto result = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(result.accepted)
      << "the physical command crossed the transport boundary";

  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.command_id, result.command_id);
  EXPECT_EQ(status.generation, result.generation);
  EXPECT_EQ(status.state, core::GripperCommandState::kFailed);
  ASSERT_TRUE(status.error.has_value());
  EXPECT_EQ(status.error->code,
            ros2_control_fault_reason::kGripperRequestFailed);
}

TEST(Ros2GripperCommandBackendTest,
     IsaacGripperLifecycleProgressesWithoutReadsAndHonorsDwell) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_autonomous_gripper_" + unique_suffix()), 1'000'000'000);

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted)
      << (command.error ? command.error->message : "");
  EXPECT_EQ(scheduler.last_priority(),
            core::SchedulerPriority::ControllerCommand);

  const auto admitted = backend.get_gripper_status("tool");
  const auto repeated = backend.get_gripper_status("tool");
  EXPECT_EQ(admitted.revision, repeated.revision);
  EXPECT_EQ(admitted.state, core::GripperCommandState::kExecuting);

  clock.advance(199'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kExecuting);

  clock.advance(1'000'000);
  scheduler.advance_to(clock.now());
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::worker(backend).drain_for(1s));
  const auto completed = backend.get_gripper_status("tool");
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_DOUBLE_EQ(completed.progress, 1.0);
  ASSERT_FALSE(sink->single_updates().empty());
  EXPECT_DOUBLE_EQ(sink->single_updates().back().position_rad, 0.0);
}

TEST(Ros2GripperCommandBackendTest,
     GripperResetFencesQueuedLifecycleBeforeProjection) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_reset_fenced_gripper_" +
                                   unique_suffix()), 1'000'000'000);

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);

  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kIdle);
  EXPECT_NE(status.generation, command.generation);
  EXPECT_TRUE(sink->single_updates().empty());

  const auto blocked = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_FALSE(blocked.accepted);
  ASSERT_TRUE(blocked.error.has_value());
  EXPECT_EQ(blocked.error->code, "gripper_command_not_authorized");
  backend.authorize_gripper_commands();
  EXPECT_TRUE(
      backend
          .command_gripper(make_gripper_request(
              0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"))
          .accepted);
}

TEST(Ros2GripperCommandBackendTest,
     GripperTerminalProjectionRetryIsBoundedAndGenerationOwned) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->fail_next_single_updates(2);
  sink->block_single_update();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_projection_retry_gripper_" +
                                   unique_suffix()), 1'000'000'000);

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  scheduler.advance_to(clock.now());
  ASSERT_TRUE(sink->wait_for_single_update());
  auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_EQ(status.projection_state, core::GripperProjectionState::kPending);
  const auto while_pending = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_FALSE(while_pending.accepted);
  ASSERT_TRUE(while_pending.error.has_value());
  EXPECT_EQ(while_pending.error->code, ros2_control_fault_reason::kGripperBusy);

  sink->release_single_update();
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::worker(backend).drain_for(1s));
  status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(status.projection_state, core::GripperProjectionState::kApplied);
  ASSERT_EQ(sink->single_updates().size(), 3u);
  EXPECT_DOUBLE_EQ(sink->single_updates().back().position_rad, 0.0);
  const auto metrics = Ros2GripperCommandBackendTestPeer::worker(backend).metrics();
  EXPECT_EQ(metrics.terminal_retries_scheduled_total, 1u);
  EXPECT_EQ(metrics.terminal_retry_successes_total, 1u);
  EXPECT_EQ(metrics.terminal_retry_exhausted_total, 0u);
  const auto after_projection = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_TRUE(after_projection.accepted);
}

TEST(
    Ros2GripperCommandBackendTest,
    GripperInvalidationDoesNotWaitForDequeuedNrtProjectionAndFencesCompletion) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_single_update();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_projection_race_gripper_" +
                                   unique_suffix()), 1'000'000'000);

  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  auto callback = std::async(std::launch::async,
                             [&]() { scheduler.advance_to(clock.now()); });
  ASSERT_TRUE(sink->wait_for_single_update());
  EXPECT_EQ(callback.wait_for(20ms), std::future_status::ready)
      << "controller-command progression must only enqueue scene projection";

  auto invalidate = std::async(std::launch::async, [&]() {
    backend.invalidate_gripper_commands(
        core::GripperInvalidationReason::kReset);
  });
  EXPECT_EQ(invalidate.wait_for(20ms), std::future_status::ready)
      << "stop/reset invalidation must not wait behind NRT scene projection";

  sink->release_single_update();
  callback.get();
  invalidate.get();
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::worker(backend).drain_for(1s));
  const auto updates_after_invalidation = sink->single_updates().size();
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(sink->single_updates().size(), updates_after_invalidation);
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
}

TEST(Ros2GripperCommandBackendTest,
     GripperConfigurationReplacementFencesOldScheduledGeneration) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_replaced_gripper_a_" + unique_suffix()), 1'000'000'000);
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);

  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_replaced_gripper_b_" + unique_suffix()), 1'000'000'000);
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());

  const auto replacement = backend.get_gripper_status("tool");
  EXPECT_EQ(replacement.state, core::GripperCommandState::kIdle);
  EXPECT_NE(replacement.generation, command.generation);
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     GripperConfigurationIsClosedBeforeExecutorStartup) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  Ros2GripperActionConfig initial;
  initial.actuation_kind = core::GripperActuationKind::kPneumatic;
  initial.state_freshness_timeout_s = 1.0;
  initial.action_name = "/manyforge_prestart_gripper_initial_" + unique_suffix();
  initial.completion_policy = "output_delivery";
  backend.configure_gripper_action("tool", std::move(initial), 1'000'000'000);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_prestart_gripper_" + unique_suffix()), 1'000'000'000);
  EXPECT_EQ(backend.gripper_actuation_kind("tool"), core::GripperActuationKind::kPositionCommanded);

  backend.complete_ros_configuration();
  lifetime.start();

  EXPECT_THROW(backend.configure_gripper_action(
                   "tool", isaac_gripper_config("/manyforge_late_gripper_" +
                                                unique_suffix()), 1'000'000'000),
               std::logic_error);
  EXPECT_EQ(backend.gripper_actuation_kind("tool"), core::GripperActuationKind::kPositionCommanded);
  EXPECT_THROW(backend.complete_ros_configuration(), std::logic_error);
}

TEST(Ros2GripperCommandBackendTest,
     GripperConfigurationReplacementRejectsInFlightPreflight) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_gripper_a_" +
                                   unique_suffix()), 1'000'000'000);

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_gripper_b_" +
                                   unique_suffix()), 1'000'000'000);
  sink->release_read();

  const auto stale = in_flight.get();
  ASSERT_FALSE(stale.accepted);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "gripper_command_not_authorized");
  const auto replacement = backend.get_gripper_status("tool");
  EXPECT_EQ(replacement.state, core::GripperCommandState::kIdle);
  EXPECT_EQ(replacement.command_id, 0u);

  const auto fresh = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_TRUE(fresh.accepted) << (fresh.error ? fresh.error->message : "");
}

TEST(Ros2GripperCommandBackendTest,
     LifecycleInvalidationDoesNotWaitForBlockedGripperPreflight) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_invalidation_" +
                                   unique_suffix()), 1'000'000'000);

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  const auto started = std::chrono::steady_clock::now();
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 50ms);
  sink->release_read();

  const auto stale = in_flight.get();
  ASSERT_FALSE(stale.accepted);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "gripper_command_not_authorized");
}

TEST(Ros2GripperCommandBackendTest,
     UnrelatedGripperConfigurationDoesNotRejectInFlightPreflight) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_independent_gripper_a_" +
                                   unique_suffix()), 1'000'000'000);

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "auxiliary", isaac_gripper_config("/manyforge_independent_gripper_b_" +
                                        unique_suffix()), 1'000'000'000);
  sink->release_read();

  const auto admitted = in_flight.get();
  EXPECT_TRUE(admitted.accepted)
      << (admitted.error ? admitted.error->message : "");
}
class ThrowingCancelScheduler final : public core::IScheduler {
public:
  core::ScheduleHandle schedule(core::TimePoint, std::function<void()> callback,
                                core::SchedulerPriority) override {
    queued.push_back(std::move(callback));
    return queued.size();
  }
  void cancel(core::ScheduleHandle) override {
    ++cancel_attempts;
    throw std::runtime_error("injected cancellation failure");
  }
  std::vector<std::function<void()>> queued;
  std::size_t cancel_attempts = 0;
};

bool wait_for_gripper_fact(const std::function<bool()> &predicate);
Ros2GripperActionConfig pneumatic_mock_config(const std::string &endpoint);
bool has_gripper_fact(const core::GripperCommandStatusDTO &status,
                      const std::string &fact);

TEST(Ros2GripperCommandBackendTest,
     SchedulerCancelFailurePreservesTheInvalidatedGenerationFence) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  ThrowingCancelScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_throwing_cancel_" + unique_suffix()), 1'000'000'000);
  const auto admitted = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(admitted.accepted);
  ASSERT_EQ(scheduler.queued.size(), 1U);

  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  EXPECT_EQ(scheduler.cancel_attempts, 1U);
  clock.advance(1'000'000'000);
  scheduler.queued.front()();

  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     QueuedLifecycleCallbackIsInertAfterAdapterDestruction) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  ThrowingCancelScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  {
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler,
                                      false);
    TestTransportLifetime lifetime(*resources, backend);
    backend.configure_gripper_action(
        "tool",
        isaac_gripper_config("/manyforge_late_callback_" + unique_suffix()), 1'000'000'000);
    ASSERT_TRUE(
        backend
            .command_gripper(make_gripper_request(
                0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"))
            .accepted);
    ASSERT_EQ(scheduler.queued.size(), 1U);
  }
  EXPECT_EQ(scheduler.cancel_attempts, 1U);
  clock.advance(1'000'000'000);
  scheduler.queued.front()();
  EXPECT_TRUE(sink->single_updates().empty());
}

class DequeuedCallbackScheduler final : public core::IScheduler {
public:
  core::ScheduleHandle schedule(core::TimePoint, std::function<void()> callback,
                                core::SchedulerPriority priority) override {
    EXPECT_EQ(priority, core::SchedulerPriority::ControllerCommand);
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
    return 1U;
  }
  void cancel(core::ScheduleHandle) override {
    // A dequeued callback is executing lane work, and cannot be removed here.
  }
  bool drain_for(core::SchedulerPriority priority,
                 std::chrono::nanoseconds timeout) override {
    EXPECT_EQ(priority, core::SchedulerPriority::ControllerCommand);
    std::unique_lock<std::mutex> lock(mutex_);
    drain_entered_ = true;
    cv_.notify_all();
    return cv_.wait_for(lock, timeout, [&] { return !active_; });
  }
  void run_dequeued() {
    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      callback = callback_;
      active_ = true;
      cv_.notify_all();
      cv_.wait(lock, [&] { return released_; });
    }
    callback();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_ = false;
      cv_.notify_all();
    }
  }
  bool wait_for_dequeued() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 500ms, [&] { return active_; });
  }
  bool wait_for_drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 500ms, [&] { return drain_entered_; });
  }
  void release_dequeued() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::function<void()> callback_;
  bool active_ = false;
  bool released_ = false;
  bool drain_entered_ = false;
};

TEST(Ros2GripperCommandBackendTest,
     OwnerDestructionWaitsForADequeuedControllerCommandCallback) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  DequeuedCallbackScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  auto backend = std::make_unique<Ros2GripperCommandBackend>(
      resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, *backend);
  backend->configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_dequeued_callback_" + unique_suffix()), 1'000'000'000);
  ASSERT_TRUE(
      backend
          ->command_gripper(make_gripper_request(
              0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"))
          .accepted);
  auto callback =
      std::async(std::launch::async, [&] { scheduler.run_dequeued(); });
  const bool dequeued = scheduler.wait_for_dequeued();
  if (!dequeued) {
    scheduler.release_dequeued();
    callback.get();
    FAIL() << "callback was not dequeued";
  }
  auto destroy = std::async(std::launch::async, [&] {
    lifetime.close();
    backend.reset();
  });
  EXPECT_TRUE(scheduler.wait_for_drain());
  EXPECT_EQ(destroy.wait_for(0ns), std::future_status::timeout);
  scheduler.release_dequeued();
  callback.get();
  destroy.get();
  EXPECT_EQ(backend, nullptr);
  EXPECT_TRUE(sink->single_updates().empty());
}

// Real ROS delivery is controlled separately from the injected result deadline
// clock.
class ControlledPneumaticServer final {
public:
  using Action = control_msgs::action::ParallelGripperCommand;
  using ServerGoal = rclcpp_action::ServerGoalHandle<Action>;
  ControlledPneumaticServer(Ros2TransportResources &resources,
                            const std::string &name) {
    server_ = rclcpp_action::create_server<Action>(
        resources.node(), name,
        [this](const rclcpp_action::GoalUUID &,
               std::shared_ptr<const Action::Goal> goal) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            received_goals_.push_back(*goal);
          }
          ++requests;
          while (hold_goal_response.load() && !release_goal_response.load())
            std::this_thread::sleep_for(1ms);
          return reject.load()
                     ? rclcpp_action::GoalResponse::REJECT
                     : rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        },
        [this](std::shared_ptr<ServerGoal>) {
          ++cancels;
          return accept_cancel.load() ? rclcpp_action::CancelResponse::ACCEPT
                                      : rclcpp_action::CancelResponse::REJECT;
        },
        [this](std::shared_ptr<ServerGoal> goal) {
          std::lock_guard<std::mutex> lock(mutex_);
          goals_.push_back(std::move(goal));
          cv_.notify_all();
        });
  }
  bool wait_for_goal(std::size_t count = 1U) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 2s, [&] { return goals_.size() >= count; });
  }
  void result(std::size_t index, bool succeeded) {
    std::shared_ptr<ServerGoal> goal;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal = goals_.at(index);
    }
    auto result = std::make_shared<Action::Result>();
    // A pneumatic delivery never depends on finger position/reached_goal.
    result->state.position = {finite_result.load()
                                  ? 0.0
                                  : std::numeric_limits<double>::quiet_NaN()};
    result->reached_goal = false;
    if (succeeded)
      goal->succeed(result);
    else
      goal->abort(result);
  }
  void canceled(std::size_t index) {
    std::shared_ptr<ServerGoal> goal;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal = goals_.at(index);
    }
    goal->canceled(std::make_shared<Action::Result>());
  }
  Action::Goal received_goal(std::size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_goals_.at(index);
  }
  void feedback(std::size_t index, double position) {
    std::shared_ptr<ServerGoal> goal;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      goal = goals_.at(index);
    }
    auto feedback = std::make_shared<Action::Feedback>();
    feedback->state.position = {position};
    goal->publish_feedback(feedback);
  }
  std::atomic<bool> reject{false};
  std::atomic<bool> accept_cancel{true};
  std::atomic<bool> finite_result{false};
  std::atomic<bool> hold_goal_response{false};
  std::atomic<bool> release_goal_response{false};
  std::atomic<std::size_t> requests{0};
  std::atomic<std::size_t> cancels{0};

private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::shared_ptr<ServerGoal>> goals_;
  std::vector<Action::Goal> received_goals_;
  rclcpp_action::Server<Action>::SharedPtr server_;
};

class NotifyOnNextClock final : public core::IClock {
public:
  core::TimePoint now() const override {
    if (armed_.exchange(false)) {
      (void)owner_->notify_owner();
      if (!owner_->on_owner_thread()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!owner_->executor_failure_started() &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
      }
    }
    return value_.load();
  }
  void attach(Ros2TransportResources &owner) { owner_ = &owner; }
  void arm() { armed_.store(true); }
  void set(core::TimePoint value) { value_.store(value); }
private:
  Ros2TransportResources *owner_{};
  mutable std::atomic<bool> armed_{false};
  std::atomic<core::TimePoint> value_{0};
};

struct ScopedTestRosOwnerStop final {
  std::shared_ptr<Ros2TransportResources> owner;
  ~ScopedTestRosOwnerStop() { owner->quiesce(); }
};

TEST(Ros2GripperCommandBackendTest, FaultBetweenAdmissionAndSendIsKnownNoEffect) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  NotifyOnNextClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  clock.attach(*resources);
  resources->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_fault_before_send_" + unique_suffix();
  auto server_resources = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "server"));
  ControlledPneumaticServer server(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  clock.arm();
  const auto result = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result"));
  EXPECT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, ros2_control_fault_reason::kGripperGoalDispatchFailed);
  EXPECT_EQ(server.requests.load(), 0U);
  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.command_id, result.command_id);
  EXPECT_EQ(status.state, core::GripperCommandState::kFailed);
  EXPECT_EQ(status.projection_state, core::GripperProjectionState::kNotRequired);
}

TEST(Ros2GripperCommandBackendTest, ActionReadinessCacheTracksServerDiscovery) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  const auto endpoint = "/manyforge_action_readiness_" + unique_suffix();
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  EXPECT_EQ(backend.cached_gripper_local_owner_ready("tool"), false);

  std::shared_ptr<Ros2TransportResources> server_resources = test_transport_resources(sink, clock, "server");
  auto server = std::make_unique<ControlledPneumaticServer>(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.cached_gripper_local_owner_ready("tool") == true;
  }));
  server_resources->invoke([&] { server.reset(); });
  EXPECT_TRUE(wait_for_gripper_fact([&] {
    return backend.cached_gripper_local_owner_ready("tool") == false;
  }));
}

TEST(Ros2GripperCommandBackendTest, FaultedOwnerRetiresTimeoutWithoutSettlingGoal) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  resources->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_faulted_gripper_timeout_" + unique_suffix();
  auto server_resources = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "server"));
  ControlledPneumaticServer server(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto sent = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result"));
  ASSERT_TRUE(sent.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return Ros2GripperCommandBackendTestPeer::goal_accepted(backend, "tool"); }));
  ASSERT_TRUE(resources->notify_owner());
  ASSERT_TRUE(wait_for_gripper_fact([&] { return resources->executor_fault().has_value(); }));
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  const auto after_stop = backend.get_gripper_status("tool");
  EXPECT_EQ(after_stop.command_id, sent.command_id);
  EXPECT_EQ(after_stop.generation, sent.generation);
  EXPECT_EQ(after_stop.state, core::GripperCommandState::kExecuting);
  clock.advance(200'000'000);
  scheduler.advance_to(clock.now());
  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.command_id, sent.command_id);
  EXPECT_EQ(status.generation, sent.generation);
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_FALSE(status.error.has_value());
  EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::goal_accepted(backend, "tool"));
  EXPECT_EQ(server.cancels.load(), 0U);
  const auto disable = backend.disable_lifecycle_gripper("tool", {});
  EXPECT_FALSE(disable.succeeded);
  ASSERT_TRUE(disable.error.has_value());
  EXPECT_EQ(disable.error->code, "ros2_control_gripper_bridge_disable_unsettled");
  EXPECT_EQ(server.cancels.load(), 0U);
  ASSERT_EQ(status.projection_state, core::GripperProjectionState::kPending);
  auto candidate = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "replacement"));
  ScopedTestRosOwnerStop candidate_stop{candidate};
  auto rebind = backend.stage_ros_rebind(candidate);
  candidate->start();
  resources->quiesce();
  ASSERT_TRUE(backend.commit_ros_rebind(rebind).ok());
  ASSERT_TRUE(backend.activate_ros_rebind(rebind).ok());
  ASSERT_TRUE(backend.rebuilt_unknown_pending("tool", sent.command_id, sent.generation));
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::inject_late_cancel_failure(backend, "tool"));
  ASSERT_TRUE(backend.gripper_late_cancel_failed());
  const auto rebuilt_owner = backend.rebuilt_unknown_owner("tool", sent.command_id, sent.generation);
  ASSERT_TRUE(rebuilt_owner);
  EXPECT_FALSE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, resources).ok());
  EXPECT_TRUE(backend.rebuilt_unknown_pending("tool", sent.command_id, sent.generation));
  ASSERT_TRUE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, rebuilt_owner).ok());
  const auto retired = backend.get_gripper_status("tool");
  EXPECT_EQ(retired.state, core::GripperCommandState::kIdle);
  EXPECT_FALSE(backend.gripper_late_cancel_failed());
  EXPECT_EQ(retired.projection_state, core::GripperProjectionState::kNotRequired);
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest, SubmittedProjectionSettlesBeforeUnknownRetirement) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  resources->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_submitted_projection_" + unique_suffix();
  auto server_resources = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "server"));
  ControlledPneumaticServer server(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto sent = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result"));
  ASSERT_TRUE(sent.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  sink->block_single_update();
  struct Release final {
    std::shared_ptr<RecordingExecutionStateSink> sink;
    ~Release() { sink->release_single_update(); }
  } release{sink};
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::submit_projection(backend, "tool", 0.4));
  ASSERT_TRUE(sink->wait_for_single_update());
  ASSERT_TRUE(resources->notify_owner());
  ASSERT_TRUE(wait_for_gripper_fact([&] { return resources->executor_fault().has_value(); }));
  auto candidate = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "replacement"));
  ScopedTestRosOwnerStop candidate_stop{candidate};
  auto rebind = backend.stage_ros_rebind(candidate);
  candidate->start();
  resources->quiesce();
  ASSERT_TRUE(backend.commit_ros_rebind(rebind).ok());
  ASSERT_TRUE(backend.activate_ros_rebind(rebind).ok());
  ASSERT_TRUE(backend.rebuilt_unknown_pending("tool", sent.command_id, sent.generation));
  const auto rebuilt_owner = backend.rebuilt_unknown_owner("tool", sent.command_id, sent.generation);
  ASSERT_TRUE(rebuilt_owner);
  EXPECT_FALSE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, rebuilt_owner).ok());
  sink->release_single_update();
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").projection_state == core::GripperProjectionState::kApplied;
  }));
  EXPECT_TRUE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, rebuilt_owner).ok());
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest, CompletedMissIsRecordedWhenUnknownRetires) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  resources->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_missed_projection_" + unique_suffix();
  auto server_resources = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "server"));
  ControlledPneumaticServer server(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto sent = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result"));
  ASSERT_TRUE(sent.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  sink->fail_next_single_updates(1U);
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::submit_projection(backend, "tool", 0.4, false));
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").projection_reason == "scene_sink_busy";
  }));
  ASSERT_TRUE(resources->notify_owner());
  ASSERT_TRUE(wait_for_gripper_fact([&] { return resources->executor_fault().has_value(); }));
  auto candidate = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "replacement"));
  ScopedTestRosOwnerStop candidate_stop{candidate};
  auto rebind = backend.stage_ros_rebind(candidate);
  candidate->start();
  resources->quiesce();
  ASSERT_TRUE(backend.commit_ros_rebind(rebind).ok());
  ASSERT_TRUE(backend.activate_ros_rebind(rebind).ok());
  const auto rebuilt_owner = backend.rebuilt_unknown_owner("tool", sent.command_id, sent.generation);
  ASSERT_TRUE(rebuilt_owner);
  ASSERT_TRUE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, rebuilt_owner).ok());
  const auto retired = backend.get_gripper_status("tool");
  EXPECT_EQ(retired.state, core::GripperCommandState::kIdle);
  EXPECT_EQ(retired.projection_reason, "scene_sink_busy");
  EXPECT_NE(std::find(retired.evidence.begin(), retired.evidence.end(), "scene_projection_missed"),
            retired.evidence.end());
  EXPECT_EQ(sink->single_updates().size(), 1U);
  EXPECT_EQ(server.cancels.load(), 0U);
}

class SequencedProjectionSink final : public core::IExecutionStateSink {
 public:
  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string &, const std::vector<double> &, core::ExecutionStateUpdateMode) override {
    return {core::ExecutionStateSinkStatus::kApplied};
  }
  std::optional<double> group_joint_position(const std::string &, const std::string &) const override {
    return 0.7;
  }
  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string &, const std::string &, double, core::ExecutionStateUpdateMode) override {
    std::unique_lock lock(mutex_);
    const auto index = ++entered_;
    condition_.notify_all();
    condition_.wait_for(lock, 2s, [&] { return released_ >= index; });
    return {core::ExecutionStateSinkStatus::kApplied};
  }
  bool wait_entered(std::size_t count) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 2s, [&] { return entered_ >= count; });
  }
  void release(std::size_t count) {
    std::lock_guard lock(mutex_);
    released_ = count;
    condition_.notify_all();
  }
 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t entered_{};
  std::size_t released_{};
};

TEST(SceneProjectionWorkerLifetime, StoppedWorkerMayOutliveClock) {
  auto clock = std::make_unique<core::FakeClock>();
  auto sink = std::make_shared<SequencedProjectionSink>();
  auto worker = std::make_unique<detail::SceneProjectionWorker>(sink, "arm", *clock);
  ASSERT_TRUE(worker->stop_and_join_for(2s));
  clock.reset();
  EXPECT_FALSE(worker->submit_active({0.0}, 0));
  EXPECT_NO_THROW(worker.reset());
}

TEST(SceneProjectionWorkerLifetime, DeadlineRetainsWorkerUntilBlockedSinkExits) {
  core::FakeClock clock;
  auto sink = std::make_shared<SequencedProjectionSink>();
  detail::SceneProjectionWorker worker(sink, "arm", clock);
  const auto slot = worker.register_joint_slot("arm", "joint", {});
  ASSERT_TRUE(worker.submit_active_joint(slot, 0.2, 0, {}));
  const bool entered = sink->wait_entered(1);
  if (!entered) sink->release(1);
  ASSERT_TRUE(entered);
  EXPECT_FALSE(worker.stop_and_join_for(std::chrono::nanoseconds::zero()));
  EXPECT_FALSE(worker.submit_active({0.0}, 0));
  sink->release(1);
  EXPECT_TRUE(worker.stop_and_join_for(2s));
}

TEST(SceneProjectionWorkerLifetime, ConcurrentStopCallersKeepTheirOwnDeadlines) {
  core::FakeClock clock;
  auto sink = std::make_shared<SequencedProjectionSink>();
  detail::SceneProjectionWorker worker(sink, "arm", clock);
  const auto slot = worker.register_joint_slot("arm", "joint", {});
  ASSERT_TRUE(worker.submit_active_joint(slot, 0.2, 0, {}));
  const bool entered = sink->wait_entered(1);
  if (!entered) sink->release(1);
  ASSERT_TRUE(entered);
  auto longer = std::async(std::launch::async, [&] { return worker.stop_and_join_for(2s); });
  auto shorter = std::async(std::launch::async, [&] { return worker.stop_and_join_for(20ms); });
  const auto short_status = shorter.wait_for(500ms);
  if (short_status != std::future_status::ready) sink->release(1);
  ASSERT_EQ(short_status, std::future_status::ready);
  EXPECT_FALSE(shorter.get());
  sink->release(1);
  EXPECT_TRUE(longer.get());
}

void exercise_projection_submission_overlap(bool reject_second) {
  auto sink = std::make_shared<SequencedProjectionSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  resources->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_projection_overlap_" + unique_suffix();
  auto server_resources = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "server"));
  ControlledPneumaticServer server(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  TestTransportLifetime lifetime(*resources, backend);
  struct Release final {
    std::shared_ptr<SequencedProjectionSink> sink;
    ~Release() { sink->release(std::numeric_limits<std::size_t>::max()); }
  } release{sink};
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto sent = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result"));
  ASSERT_TRUE(sent.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(Ros2GripperCommandBackendTestPeer::submit_projection(backend, "tool", 0.4));
  ASSERT_TRUE(sink->wait_entered(1));
  detail::SceneProjectionWorker::Suspension suspension;
  if (reject_second) {
    // Test-only rejection injection; recovery itself never suspends the worker.
    suspension = Ros2GripperCommandBackendTestPeer::worker(backend).suspend();
  }
  EXPECT_EQ(Ros2GripperCommandBackendTestPeer::submit_projection(backend, "tool", 0.5),
            !reject_second);
  ASSERT_TRUE(resources->notify_owner());
  ASSERT_TRUE(wait_for_gripper_fact([&] { return resources->executor_fault().has_value(); }));
  auto candidate = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "replacement"));
  ScopedTestRosOwnerStop candidate_stop{candidate};
  auto rebind = backend.stage_ros_rebind(candidate);
  candidate->start();
  resources->quiesce();
  ASSERT_TRUE(backend.commit_ros_rebind(rebind).ok());
  ASSERT_TRUE(backend.activate_ros_rebind(rebind).ok());
  const auto rebuilt_owner = backend.rebuilt_unknown_owner("tool", sent.command_id, sent.generation);
  ASSERT_TRUE(rebuilt_owner);
  EXPECT_FALSE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, rebuilt_owner).ok());
  sink->release(1);
  if (!reject_second) {
    ASSERT_TRUE(sink->wait_entered(2));
    EXPECT_TRUE(Ros2GripperCommandBackendTestPeer::projection_submitted(backend, "tool"));
    EXPECT_FALSE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, rebuilt_owner).ok());
    sink->release(2);
  }
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return !Ros2GripperCommandBackendTestPeer::projection_submitted(backend, "tool");
  }));
  EXPECT_TRUE(backend.retire_rebuilt_unknown("tool", sent.command_id, sent.generation, rebuilt_owner).ok());
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest, SubmittedProjectionThenRejectedProjection) {
  exercise_projection_submission_overlap(true);
}

TEST(Ros2GripperCommandBackendTest, SubmittedProjectionThenAcceptedProjection) {
  exercise_projection_submission_overlap(false);
}

TEST(Ros2GripperCommandBackendTest, OwnerFaultWakesPendingLifecycleDisable) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  resources->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_fault_during_disable_" + unique_suffix();
  auto server_resources = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "server"));
  ControlledPneumaticServer server(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  ASSERT_TRUE(backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result")).accepted);
  ASSERT_TRUE(server.wait_for_goal());
  std::stop_source cleanup;
  auto pending = std::async(std::launch::async, [&] {
    return backend.disable_lifecycle_gripper("tool", cleanup.get_token());
  });
  const bool cancel_observed = wait_for_gripper_fact([&] { scheduler.advance_to(clock.now()); return server.cancels.load() == 1U; });
  if (!cancel_observed) cleanup.request_stop();
  ASSERT_TRUE(cancel_observed);
  // The backend's own notification may consume the injected fault first;
  // either way the owner must fault and the pending disable must settle.
  (void)resources->notify_owner();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return resources->executor_fault().has_value(); }));
  const auto ready = pending.wait_for(2s);
  if (ready != std::future_status::ready) cleanup.request_stop();
  ASSERT_EQ(ready, std::future_status::ready);
  const auto result = pending.get();
  EXPECT_FALSE(result.succeeded);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "ros2_control_gripper_bridge_disable_unsettled");
  EXPECT_EQ(server.cancels.load(), 1U);
}

TEST(Ros2GripperCommandBackendTest, FaultDuringTimeoutClockReadPreservesUnknown) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  NotifyOnNextClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  clock.attach(*resources);
  resources->set_owner_notification([] { throw std::runtime_error("injected owner fault"); });
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_timeout_before_fault_" + unique_suffix();
  auto server_resources = std::shared_ptr<Ros2TransportResources>(
      test_transport_resources(sink, clock, "server"));
  ControlledPneumaticServer server(*server_resources, endpoint);
  ScopedTestRosOwnerStop server_stop{server_resources};
  server_resources->start();
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto sent = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result"));
  ASSERT_TRUE(sent.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return Ros2GripperCommandBackendTestPeer::goal_accepted(backend, "tool"); }));
  clock.set(200'000'000);
  clock.arm();
  scheduler.advance_to(200'000'000);
  ASSERT_TRUE(wait_for_gripper_fact([&] { return resources->executor_fault().has_value(); }));
  const auto status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_FALSE(status.error.has_value());
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest, QueuedTimeoutCannotOverwriteTerminalGoal) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  QueuedCallbackScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources =
      test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_terminal_before_timeout_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  server.finite_result = true;
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto sent = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 1.0, "ros_gripper_command", "driver_result"));
  ASSERT_TRUE(sent.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  auto queued = scheduler.take_callbacks();
  ASSERT_FALSE(queued.empty());
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state == core::GripperCommandState::kCompleted;
  }));
  clock.advance(200'000'000);
  for (auto &callback : queued) callback();
  EXPECT_EQ(backend.get_gripper_status("tool").state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest,
     LifecycleDisableAndGenericInvalidationShareOneExactCancel) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_lifecycle_cancel_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  server.finite_result = true;
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  ASSERT_TRUE(backend.command_gripper(make_gripper_request(
      0.0, 0.2, 1.0, "ros_gripper_command", "driver_result")).accepted);
  ASSERT_TRUE(server.wait_for_goal());
  std::stop_source stop;
  auto disable = std::async(std::launch::async, [&] {
    return backend.disable_lifecycle_gripper("tool", stop.get_token());
  });
  ASSERT_TRUE(wait_for_gripper_fact([&] { scheduler.advance_to(clock.now()); return server.cancels.load() == 1U; }));
  EXPECT_EQ(disable.wait_for(20ms), std::future_status::timeout);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(server.cancels.load(), 1U);
  server.result(0, true);
  const auto interrupted = disable.get();
  EXPECT_TRUE(interrupted.effect_started);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.disable_lifecycle_gripper("tool", {}).succeeded;
  }));
  EXPECT_FALSE(backend.command_gripper(make_gripper_request(
      0.7, 0.2, 1.0, "ros_gripper_command", "driver_result")).accepted);
  EXPECT_TRUE(backend.authorize_lifecycle_gripper("tool").succeeded);
  EXPECT_FALSE(backend.get_gripper_status("tool").admission_closed);
}

// Stock GripperActionController quirk: a Stop that finds the fingers within
// the stop finish tolerance lets the goal finish (no cancel) and settles on
// the controller's own result; mid-motion it cancels as before.
TEST(Ros2GripperCommandBackendTest,
     StopWithinFinishToleranceLetsTheGoalFinishInsteadOfCancelling) {
  for (const bool at_target : {true, false}) {
    SCOPED_TRACE(at_target ? "at target" : "mid-motion");
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    sink->set_joint_position("gripper", "finger_joint", 0.7);
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
    const auto endpoint = "/manyforge_stop_finish_" + unique_suffix();
    ControlledPneumaticServer server(*resources, endpoint);
    server.finite_result = true;
    TestTransportLifetime lifetime(*resources, backend);
    auto config = pneumatic_mock_config(endpoint);
    config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
    config.completion_policy = "driver_result";
    config.stop_finish_tolerance = 0.05;
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    backend.complete_ros_configuration();
    lifetime.start();
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return backend.gripper_transport_readiness("tool").ready; }));
    // The fingers are at 0.7: 0.68 is within the tolerance, 0.0 is not.
    ASSERT_TRUE(backend.command_gripper(make_gripper_request(
        at_target ? 0.68 : 0.0, 0.2, 1.0, "ros_gripper_command",
        "driver_result")).accepted);
    ASSERT_TRUE(server.wait_for_goal());
    backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
    const auto stopped = backend.get_gripper_status("tool");
    EXPECT_NE(stopped.state, core::GripperCommandState::kExecuting);
    EXPECT_TRUE(stopped.cancellation_pending);
    EXPECT_EQ(std::find(stopped.evidence.begin(), stopped.evidence.end(),
                        "completed_by_natural_completion") !=
                  stopped.evidence.end(),
              at_target);
    if (at_target) {
      std::this_thread::sleep_for(20ms);
      EXPECT_EQ(server.cancels.load(), 0U);
      server.result(0, true);
    } else {
      ASSERT_TRUE(wait_for_gripper_fact([&] { scheduler.advance_to(clock.now()); return server.cancels.load() == 1U; }));
      server.canceled(0);
    }
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return !backend.get_gripper_status("tool").cancellation_pending; }));
    EXPECT_TRUE(backend.try_authorize_gripper_commands().ok());
  }
}

// ParallelGripperCommand (stock parallel_gripper_action_controller): the goal
// carries the one finger joint's position, and an effort only when the request
// sets max_effort. A cancelled goal's result has an empty state, which reports
// no finger position.
TEST(Ros2GripperCommandBackendTest,
     ParallelGripperGoalCarriesOneJointPositionAndOptionalEffort) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_parallel_goal_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  server.finite_result = true;
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));

  ASSERT_TRUE(backend.command_gripper(make_gripper_request(
      0.3, 0.2, 1.5, "ros_gripper_command", "driver_result")).accepted);
  ASSERT_TRUE(server.wait_for_goal(1U));
  const auto with_effort = server.received_goal(0);
  EXPECT_EQ(with_effort.command.position, std::vector<double>{0.3});
  EXPECT_EQ(with_effort.command.effort, std::vector<double>{1.5});
  EXPECT_TRUE(with_effort.command.velocity.empty());
  // A goal is cancelled only after its cancel request (rcl_action refuses
  // EXECUTING -> CANCELED): a Stop sends it, the server then cancels.
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    scheduler.advance_to(clock.now());
    return server.cancels.load() == 1U;
  }));
  server.canceled(0);
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return !backend.get_gripper_status("tool").cancellation_pending; }));
  EXPECT_NE(backend.get_gripper_status("tool").current_position,
            std::optional<double>{0.0});
  ASSERT_TRUE(backend.try_authorize_gripper_commands().ok());

  auto no_effort = make_gripper_request(0.0, 0.2, 0.0, "ros_gripper_command",
                                        "driver_result");
  no_effort.max_effort.reset();
  ASSERT_TRUE(backend.command_gripper(no_effort).accepted);
  ASSERT_TRUE(server.wait_for_goal(2U));
  const auto without_effort = server.received_goal(1);
  EXPECT_EQ(without_effort.command.position, std::vector<double>{0.0});
  EXPECT_TRUE(without_effort.command.effort.empty());
  server.result(1, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kCompleted;
  }));
}

// wait_for_duration completes only when the controller reported done
// and command_duration_s has elapsed since the start, in either order; false
// completes on the controller's result. A Stop during the wait ends the
// command like a goal finished in its last moment (no cancel).
TEST(Ros2GripperCommandBackendTest,
     WaitForDurationCompletesAfterControllerResultAndDuration) {
  enum class Case { kControllerFirst, kDurationFirst, kNoWait, kStopWhileWaiting };
  for (const auto order : {Case::kControllerFirst, Case::kDurationFirst,
                           Case::kNoWait, Case::kStopWhileWaiting}) {
    SCOPED_TRACE(static_cast<int>(order));
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    sink->set_joint_position("gripper", "finger_joint", 0.7);
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
    const auto endpoint = "/manyforge_wait_for_duration_" + unique_suffix();
    ControlledPneumaticServer server(*resources, endpoint);
    server.finite_result = true;
    TestTransportLifetime lifetime(*resources, backend);
    auto config = pneumatic_mock_config(endpoint);
    config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
    config.completion_policy = "driver_result";
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    backend.complete_ros_configuration();
    lifetime.start();
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return backend.gripper_transport_readiness("tool").ready; }));
    auto request = make_gripper_request(0.0, 0.2, 1.0, "ros_gripper_command",
                                        "driver_result");
    request.wait_for_duration = order != Case::kNoWait;
    ASSERT_TRUE(backend.command_gripper(request).accepted);
    ASSERT_TRUE(server.wait_for_goal());
    const auto state = [&] { return backend.get_gripper_status("tool").state; };
    if (order == Case::kDurationFirst) {
      // Within the result deadline (duration + 0.1 s result timeout).
      clock.advance(250'000'000);
      scheduler.advance_to(clock.now());
      EXPECT_EQ(state(), core::GripperCommandState::kExecuting);
    }
    server.result(0, true);
    // The result's position (0.0) shows the controller's report arrived.
    ASSERT_TRUE(wait_for_gripper_fact([&] {
      const auto position = backend.get_gripper_status("tool").current_position;
      return position.has_value() && *position == 0.0;
    }));
    if (order == Case::kDurationFirst || order == Case::kNoWait) {
      EXPECT_TRUE(wait_for_gripper_fact(
          [&] { return state() == core::GripperCommandState::kCompleted; }));
      continue;
    }
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(state(), core::GripperCommandState::kExecuting);
    if (order == Case::kStopWhileWaiting) {
      backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
      const auto stopped = backend.get_gripper_status("tool");
      EXPECT_NE(stopped.state, core::GripperCommandState::kExecuting);
      EXPECT_FALSE(stopped.cancellation_pending);
      EXPECT_TRUE(has_gripper_fact(stopped, "completed_by_natural_completion"));
      std::this_thread::sleep_for(20ms);
      EXPECT_EQ(server.cancels.load(), 0U);
      EXPECT_TRUE(backend.try_authorize_gripper_commands().ok());
      continue;
    }
    clock.advance(150'000'000);
    scheduler.advance_to(clock.now());
    EXPECT_EQ(state(), core::GripperCommandState::kExecuting);
    clock.advance(50'000'000);
    scheduler.advance_to(clock.now());
    EXPECT_TRUE(wait_for_gripper_fact(
        [&] { return state() == core::GripperCommandState::kCompleted; }));
  }
}

// A Stop sends no cancel when the node's command_duration_s has elapsed
// (ParallelGripperCommand feedback carries no reached_goal); that command ends on the goal's
// own result. Mid-motion the Stop cancels and the server's ACCEPT ends
// the command even when no result ever follows (the stock controller's gap
// case); the next command is admitted and a late result is ignored.
TEST(Ros2GripperCommandBackendTest,
     StopEndsOnFinishedResultOrCancelAcceptAndIgnoresLateResults) {
  enum class Case { kReachedGoal, kDurationElapsed, kAcceptWithoutResult,
                    kLateResultIgnored };
  for (const auto which : {Case::kDurationElapsed, Case::kAcceptWithoutResult,
                           Case::kLateResultIgnored}) {
    SCOPED_TRACE(static_cast<int>(which));
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    sink->set_joint_position("gripper", "finger_joint", 0.7);
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
    const auto endpoint = "/manyforge_stop_no_cancel_" + unique_suffix();
    ControlledPneumaticServer server(*resources, endpoint);
    server.finite_result = true;
    TestTransportLifetime lifetime(*resources, backend);
    auto config = pneumatic_mock_config(endpoint);
    config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
    config.completion_policy = "driver_result";
    config.stop_finish_tolerance = 0.05;
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    backend.complete_ros_configuration();
    lifetime.start();
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return backend.gripper_transport_readiness("tool").ready; }));
    // The fingers are at 0.7; 0.0 is far outside the finish tolerance.
    ASSERT_TRUE(backend.command_gripper(make_gripper_request(
        0.0, 0.2, 1.0, "ros_gripper_command", "driver_result")).accepted);
    ASSERT_TRUE(server.wait_for_goal());
    if (which == Case::kReachedGoal) {
      // Feedback before the client knows its goal handle is dropped: repeat.
      ASSERT_TRUE(wait_for_gripper_fact([&] {
        server.feedback(0, 0.0);
        return backend.get_gripper_status("tool").state ==
               core::GripperCommandState::kCompleted;
      }));
    } else if (which == Case::kDurationElapsed) {
      // The result deadline stays unscheduled: only the clock moves.
      clock.advance(200'000'000);
    }
    backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
    const auto stopped = backend.get_gripper_status("tool");
    EXPECT_NE(stopped.state, core::GripperCommandState::kExecuting);
    if (which == Case::kReachedGoal) {
      EXPECT_FALSE(stopped.cancellation_pending);
      std::this_thread::sleep_for(20ms);
      EXPECT_EQ(server.cancels.load(), 0U);
      EXPECT_TRUE(backend.try_authorize_gripper_commands().ok());
      continue;
    }
    if (which == Case::kDurationElapsed) {
      EXPECT_TRUE(stopped.cancellation_pending);
      EXPECT_TRUE(has_gripper_fact(stopped, "completed_by_natural_completion"));
      std::this_thread::sleep_for(20ms);
      EXPECT_EQ(server.cancels.load(), 0U);
      EXPECT_TRUE(backend.get_gripper_status("tool").cancellation_pending);
      server.result(0, true);
      ASSERT_TRUE(wait_for_gripper_fact(
          [&] { return !backend.get_gripper_status("tool").cancellation_pending; }));
      EXPECT_TRUE(backend.try_authorize_gripper_commands().ok());
      continue;
    }
    // The server accepts the cancel and never sends a result.
    ASSERT_TRUE(wait_for_gripper_fact([&] { scheduler.advance_to(clock.now()); return server.cancels.load() == 1U; }));
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return !backend.get_gripper_status("tool").cancellation_pending; }));
    if (which == Case::kLateResultIgnored) {
      server.result(0, false);  // a late abort must not unsettle the Stop
      std::this_thread::sleep_for(50ms);
    }
    const auto settled = backend.get_gripper_status("tool");
    EXPECT_FALSE(settled.cancellation_pending);
    EXPECT_EQ(settled.state, core::GripperCommandState::kIdle);
    EXPECT_FALSE(has_gripper_fact(settled, "completed_by_natural_completion"));
    ASSERT_TRUE(backend.try_authorize_gripper_commands().ok());
    ASSERT_TRUE(backend.command_gripper(make_gripper_request(
        0.7, 0.2, 1.0, "ros_gripper_command", "driver_result")).accepted);
    ASSERT_TRUE(server.wait_for_goal(2U));
  }
}

TEST(Ros2GripperCommandBackendTest,
     LifecycleDisableRetainsLateAcceptedGoalAfterWaiterCancellation) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_lifecycle_late_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  server.finite_result = true;
  server.hold_goal_response = true;
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  ASSERT_TRUE(backend.command_gripper(make_gripper_request(
      0.0, 0.2, 1.0, "ros_gripper_command", "driver_result")).accepted);
  ASSERT_TRUE(wait_for_gripper_fact([&] { return server.requests.load() == 1U; }));
  std::stop_source stop;
  stop.request_stop();
  const auto waiting = backend.disable_lifecycle_gripper("tool", stop.get_token());
  EXPECT_FALSE(waiting.succeeded);
  EXPECT_TRUE(waiting.effect_started);
  server.release_goal_response = true;
  ASSERT_TRUE(server.wait_for_goal());
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    scheduler.advance_to(clock.now());
    return server.cancels.load() == 1U;
  }));
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.disable_lifecycle_gripper("tool", {}).succeeded;
  }));
}

TEST(Ros2GripperCommandBackendTest,
     ResetAuthorizationConsumesExactGenericInvalidationTerminal) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_reset_cancel_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  server.finite_result = true;
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  ASSERT_TRUE(backend.command_gripper(make_gripper_request(
      0.0, 0.2, 1.0, "ros_gripper_command", "driver_result")).accepted);
  ASSERT_TRUE(server.wait_for_goal());

  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  ASSERT_TRUE(wait_for_gripper_fact([&] { scheduler.advance_to(clock.now()); return server.cancels.load() == 1U; }));
  // The server's ACCEPT of the fenced command's cancel is its terminal;
  // no result is needed, and a later one changes nothing.
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.try_authorize_gripper_commands().ok();
  }));
  server.result(0, false);
  std::this_thread::sleep_for(50ms);
  EXPECT_TRUE(backend.try_authorize_gripper_commands().ok());
  EXPECT_FALSE(backend.get_gripper_status("tool").admission_closed);
  EXPECT_EQ(server.cancels.load(), 1U);
}

TEST(Ros2GripperCommandBackendTest,
     LifecycleDisableCancelRejectionRemainsUnsettledUntilNaturalTerminal) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_lifecycle_reject_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  server.accept_cancel = false;
  server.finite_result = true;
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "driver_result";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  ASSERT_TRUE(backend.command_gripper(make_gripper_request(
      0.0, 0.2, 1.0, "ros_gripper_command", "driver_result")).accepted);
  ASSERT_TRUE(server.wait_for_goal());
  // A cancel issued from the goal response is queued on the StopControl lane
  // (DX-4018); the disable returns only after the manual lane runs it.
  auto disable = std::async(std::launch::async, [&] {
    return backend.disable_lifecycle_gripper("tool", {});
  });
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    scheduler.advance_to(clock.now(), core::SchedulerPriority::StopControl);
    return disable.wait_for(0ms) == std::future_status::ready;
  }));
  const auto refused = disable.get();
  EXPECT_FALSE(refused.succeeded);
  EXPECT_TRUE(refused.effect_started);
  EXPECT_FALSE(backend.prepare_lifecycle_gripper("tool").succeeded);
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    const auto retried = backend.disable_lifecycle_gripper("tool", {});
    return retried.succeeded;
  }));
}

bool wait_for_gripper_fact(const std::function<bool()> &predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

Ros2GripperActionConfig pneumatic_mock_config(const std::string &endpoint) {
  Ros2GripperActionConfig config;
  config.state_freshness_timeout_s = 1.0;
  config.action_name = endpoint;
  config.actuation_kind = core::GripperActuationKind::kPneumatic;
  config.completion_policy = "output_delivery";
  config.group_id = "gripper";
  config.joint_name = "finger_joint";
  config.wait_for_action_server_timeout_s = 1.0;
  config.result_timeout_s = 0.1;
  return config;
}

bool has_gripper_fact(const core::GripperCommandStatusDTO &status,
                      const std::string &fact) {
  return std::find(status.evidence.begin(), status.evidence.end(), fact) !=
         status.evidence.end();
}

// The server has its own node and executor. All action-server operations run
// in its callbacks; no test thread holds a server callback open.
class IndependentPneumaticServer final {
public:
  using Action = control_msgs::action::ParallelGripperCommand;
  using Goal = rclcpp_action::ServerGoalHandle<Action>;

  IndependentPneumaticServer(std::shared_ptr<core::IExecutionStateSink> sink,
                             core::IClock &clock, const std::string &endpoint)
      : resources_(test_transport_resources(std::move(sink), clock, "server")) {
    server_ = rclcpp_action::create_server<Action>(
        resources_->node(), endpoint,
        [this](const rclcpp_action::GoalUUID &,
               std::shared_ptr<const Action::Goal>) {
          if (closing_)
            return rclcpp_action::GoalResponse::REJECT;
          return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        },
        [this](std::shared_ptr<Goal>) {
          ++cancel_requests;
          return rclcpp_action::CancelResponse::ACCEPT;
        },
        [this](std::shared_ptr<Goal> goal) {
          if (closing_)
            return;
          const auto count = ++accepted_goals;
          if (count == 1U) {
            auto result = std::make_shared<Action::Result>();
            result->state.position = {0.0};
            result->reached_goal = true;
            goal->succeed(result);
          } else {
            pending_goal_ = std::move(goal);
          }
        });
    timer_ = resources_->node()->create_wall_timer(5ms, [this] {
      if (pending_goal_ && pending_goal_->is_canceling()) {
        pending_goal_->canceled(std::make_shared<Action::Result>());
        pending_goal_.reset();
        ++canceled_goals;
      }
    });
    resources_->start();
  }

  ~IndependentPneumaticServer() {
    resources_->invoke([this] {
      closing_ = true;
      pending_goal_.reset();
      timer_.reset();
      server_.reset();
    });
    resources_->quiesce();
  }

  std::atomic<std::size_t> accepted_goals{0};
  std::atomic<std::size_t> cancel_requests{0};
  std::atomic<std::size_t> canceled_goals{0};

private:
  std::shared_ptr<Ros2TransportResources> resources_;
  rclcpp_action::Server<Action>::SharedPtr server_;
  rclcpp::TimerBase::SharedPtr timer_;
  std::shared_ptr<Goal> pending_goal_; // Accessed only on the server owner.
  bool closing_ = false; // Accessed only on the server owner.
};

TEST(Ros2GripperCommandBackendTest,
     IndependentServerCompletesAndCancelsPneumaticGoals) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  const auto endpoint = "/manyforge_independent_pneumatic_" + unique_suffix();
  IndependentPneumaticServer server(sink, clock, endpoint);
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));

  const auto request = make_gripper_request(
      0.0, 0.0, 0.0, "ros_gripper_command", "output_delivery");
  const auto first = backend.command_gripper(request);
  ASSERT_TRUE(first.accepted);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    const auto status = backend.get_gripper_status("tool");
    return status.command_id == first.command_id &&
           status.state == core::GripperCommandState::kCompleted &&
           has_gripper_fact(status, "pneumatic_output_delivery_confirmed");
  }));

  const auto second = backend.command_gripper(request);
  ASSERT_TRUE(second.accepted);
  EXPECT_NE(second.command_id, first.command_id);
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return server.accepted_goals.load() == 2U; }));
  // Pneumatic output is exempt from motion Stop. Reset closes its lifecycle
  // and cancels this goal whether its response has arrived or is still pending.
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  // The lifecycle cancel is queued on the StopControl lane (DX-4018), so the
  // manual scheduler must drive that lane for the cancel to be issued.
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    scheduler.advance_to(clock.now(), core::SchedulerPriority::StopControl);
    return server.cancel_requests.load() == 1U &&
           server.canceled_goals.load() == 1U &&
           !backend.get_gripper_status("tool").cancellation_pending;
  }));
  EXPECT_TRUE(backend.get_gripper_status("tool").admission_closed);
  EXPECT_TRUE(backend.try_authorize_gripper_commands().ok());
  lifetime.close();
  EXPECT_FALSE(
      resources->node()->get_node_base_interface()->get_context()->is_valid());
}

TEST(
    Ros2GripperCommandBackendTest,
    PneumaticMockStopDuringDeliveryPreservesIdentityAndCompletesAtResultWithoutProgramDwell) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_pneumatic_delivery_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "ros_gripper_command", "output_delivery"));
  ASSERT_TRUE(command.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  clock.advance(50'000'000);
  scheduler.advance_to(clock.now());
  const auto before = backend.get_gripper_status("tool");
  EXPECT_EQ(before.state, core::GripperCommandState::kExecuting);
  EXPECT_FALSE(has_gripper_fact(before, "pneumatic_output_delivery_confirmed"));
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  const auto held = backend.get_gripper_status("tool");
  EXPECT_FALSE(held.admission_closed);
  EXPECT_GT(held.admission_generation, before.admission_generation);
  EXPECT_EQ(held.command_id, command.command_id);
  EXPECT_EQ(held.generation, command.generation);
  EXPECT_EQ(server.cancels.load(), 0U);
  EXPECT_FALSE(backend
                   .command_gripper(make_gripper_request(
                       0.7, 0.0, 0.0, "ros_gripper_command", "output_delivery"))
                   .accepted);
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return has_gripper_fact(backend.get_gripper_status("tool"),
                            "pneumatic_output_delivery_confirmed");
  }));
  EXPECT_EQ(clock.now(), 50'000'000);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return scheduler.pending_count(
               core::SchedulerPriority::ControllerCommand) == 0U;
  }));
  const auto completed = backend.get_gripper_status("tool");
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(completed.command_id, command.command_id);
  EXPECT_EQ(completed.generation, command.generation);
  EXPECT_FALSE(completed.admission_closed);
  EXPECT_FALSE(completed.current_position.has_value());
  EXPECT_FALSE(completed.observed_position.has_value());
  EXPECT_EQ(server.requests.load(), 1U);
  EXPECT_EQ(server.cancels.load(), 0U);
  EXPECT_TRUE(sink->reads().empty());
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticMockZeroDwellCompletesAtActualResultAndAllowsNextOutput) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_pneumatic_zero_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "ros_gripper_command", "output_delivery"));
  ASSERT_TRUE(command.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kExecuting);
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kCompleted;
  }));
  EXPECT_EQ(clock.now(), 0);
  EXPECT_TRUE(backend
                  .command_gripper(make_gripper_request(
                      0.7, 0.0, 0.0, "ros_gripper_command", "output_delivery"))
                  .accepted);
  ASSERT_TRUE(server.wait_for_goal(2));
  server.result(1, true);
  EXPECT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kCompleted;
  }));
  EXPECT_EQ(server.requests.load(), 2U);
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticDeliveryTimeoutIsStickyFailureWithoutCancelOrResetBeforeRewrite) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_pneumatic_timeout_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  ASSERT_TRUE(backend
                  .command_gripper(make_gripper_request(
                      0.0, 15.0, 0.0, "ros_gripper_command", "output_delivery"))
                  .accepted);
  ASSERT_TRUE(server.wait_for_goal());
  clock.advance(100'000'000);
  scheduler.advance_to(clock.now());
  const auto failed = backend.get_gripper_status("tool");
  ASSERT_EQ(failed.state, core::GripperCommandState::kFailed);
  ASSERT_TRUE(failed.error);
  EXPECT_EQ(failed.error->code,
            ros2_control_fault_reason::kGripperResultTimeout);
  EXPECT_TRUE(has_gripper_fact(failed, "ros_gripper_command_enqueued"));
  EXPECT_TRUE(
      has_gripper_fact(failed, "pneumatic_output_delivery_unconfirmed"));
  EXPECT_FALSE(failed.admission_closed);
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return has_gripper_fact(backend.get_gripper_status("tool"),
                            "ros_gripper_command_late_result_succeeded");
  }));
  const auto late = backend.get_gripper_status("tool");
  EXPECT_EQ(late.state, core::GripperCommandState::kFailed);
  ASSERT_TRUE(late.error.has_value());
  EXPECT_EQ(late.error->code, failed.error->code);
  EXPECT_EQ(late.error->message, failed.error->message);
  EXPECT_EQ(late.revision, failed.revision + 1U);
  EXPECT_FALSE(has_gripper_fact(late, "pneumatic_output_delivery_confirmed"));
  const auto next = backend.command_gripper(make_gripper_request(
      0.7, 0.0, 0.0, "ros_gripper_command", "output_delivery"));
  ASSERT_TRUE(next.accepted);
  EXPECT_NE(next.generation, failed.generation);
  ASSERT_TRUE(server.wait_for_goal(2));
  server.result(1, true);
  EXPECT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kCompleted;
  }));
  EXPECT_EQ(server.cancels.load(), 0U);
  EXPECT_EQ(server.requests.load(), 2U);
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticRejectedAndAbortedDeliveryFailWithoutDwellOrBlockingNextCommand) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_pneumatic_rejected_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  server.reject = true;
  ASSERT_TRUE(backend
                  .command_gripper(make_gripper_request(
                      0.0, 0.2, 0.0, "ros_gripper_command", "output_delivery"))
                  .accepted);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kFailed;
  }));
  const auto rejected = backend.get_gripper_status("tool");
  ASSERT_TRUE(rejected.error);
  EXPECT_EQ(rejected.error->code,
            ros2_control_fault_reason::kGripperGoalRejected);
  EXPECT_TRUE(has_gripper_fact(rejected, "ros_gripper_command_goal_rejected"));
  server.reject = false;
  ASSERT_TRUE(backend
                  .command_gripper(make_gripper_request(
                      0.7, 0.2, 0.0, "ros_gripper_command", "output_delivery"))
                  .accepted);
  ASSERT_TRUE(server.wait_for_goal());
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  server.result(0, false);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kFailed;
  }));
  const auto aborted = backend.get_gripper_status("tool");
  ASSERT_TRUE(aborted.error);
  EXPECT_EQ(aborted.error->code,
            ros2_control_fault_reason::kGripperGoalAborted);
  EXPECT_FALSE(aborted.admission_closed);
  EXPECT_FALSE(
      has_gripper_fact(aborted, "pneumatic_output_delivery_confirmed"));
  EXPECT_EQ(clock.now(), 0);
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticIsaacPublishCompletesBeforeProgramDwellAndStopPreservesDelivery) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, true);
  TestTransportLifetime lifetime(*resources, backend);
  auto config =
      isaac_gripper_config("/manyforge_pneumatic_isaac_" + unique_suffix());
  config.actuation_kind = core::GripperActuationKind::kPneumatic;
  config.completion_policy = "output_delivery";
  std::vector<double> display;
  config.publish_display_position = [&](double position) {
    display.push_back(position);
    return core::Result<void>::Ok();
  };
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  EXPECT_EQ(backend.gripper_actuation_kind("tool"),
            core::GripperActuationKind::kPneumatic);
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "output_delivery"));
  ASSERT_TRUE(command.accepted);
  const auto delivered = backend.get_gripper_status("tool");
  EXPECT_TRUE(has_gripper_fact(delivered, "isaac_gripper_command_published"));
  EXPECT_TRUE(
      has_gripper_fact(delivered, "pneumatic_output_delivery_confirmed"));
  ASSERT_EQ(display.size(), 1U);
  EXPECT_DOUBLE_EQ(display.front(), 0.0);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  EXPECT_EQ(clock.now(), 0);
  EXPECT_EQ(scheduler.pending_count(core::SchedulerPriority::ControllerCommand),
            0U);
  const auto complete = backend.get_gripper_status("tool");
  EXPECT_EQ(complete.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(complete.command_id, command.command_id);
  EXPECT_EQ(complete.generation, command.generation);
  EXPECT_EQ(complete.projection_state,
            core::GripperProjectionState::kNotRequired);
  EXPECT_FALSE(complete.admission_closed);
  EXPECT_EQ(display.size(), 1U); // No timed aesthetic ramp.
  EXPECT_TRUE(sink->reads().empty());
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(
    Ros2GripperCommandBackendTest,
    PneumaticPublishCompletesWithoutSchedulerDespiteProgramDwellAndDisplayFailure) {
  for (double dwell : {0.0, 0.2}) {
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    core::FakeClock clock;
    RejectingScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler,
                                      false);
    TestTransportLifetime lifetime(*resources, backend);
    auto config = isaac_gripper_config("/manyforge_pneumatic_schedule_" +
                                       unique_suffix());
    config.actuation_kind = core::GripperActuationKind::kPneumatic;
    config.completion_policy = "output_delivery";
    config.publish_display_position = [](double) -> core::Result<void> {
      throw std::runtime_error("display unavailable");
    };
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    ASSERT_TRUE(
        backend
            .command_gripper(make_gripper_request(
                0.0, dwell, 0.0, "isaac_joint_state_effort", "output_delivery"))
            .accepted);
    auto status = backend.get_gripper_status("tool");
    EXPECT_TRUE(
        has_gripper_fact(status, "pneumatic_output_delivery_confirmed"));
    EXPECT_FALSE(status.error);
    EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
    EXPECT_EQ(clock.now(), 0);
    EXPECT_FALSE(has_gripper_fact(status, "gripper_lifecycle_schedule_failed"));
    status = backend.get_gripper_status("tool");
    EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
    EXPECT_FALSE(status.error);
    EXPECT_TRUE(sink->single_updates().empty());
  }
}

TEST(
    Ros2GripperCommandBackendTest,
    PneumaticStopBeforeAdmissionAllowsManualOutputAndMissingOutputStillRefuses) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_pneumatic_unadmitted_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  auto invalid = pneumatic_mock_config("");
  EXPECT_THROW(backend.configure_gripper_action("invalid", invalid, 1'000'000'000),
               std::invalid_argument);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto idle = backend.get_gripper_status("tool");
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  const auto stopped_idle = backend.get_gripper_status("tool");
  EXPECT_EQ(stopped_idle.generation, idle.generation);
  EXPECT_EQ(stopped_idle.revision, idle.revision);
  EXPECT_EQ(stopped_idle.lifecycle_generation, idle.lifecycle_generation);
  EXPECT_EQ(stopped_idle.evidence, idle.evidence);
  const auto result = backend.command_gripper(make_gripper_request(
      0.0, 0.0, 0.0, "ros_gripper_command", "output_delivery"));
  ASSERT_TRUE(result.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  EXPECT_EQ(server.requests.load(), 1U);
  EXPECT_FALSE(backend.get_gripper_status("tool").admission_closed);
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kCompleted;
  }));
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest,
     PositionActuationKeepsItsExistingStopGenerationFence) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_position_stop_" + unique_suffix()), 1'000'000'000);
  EXPECT_EQ(backend.gripper_actuation_kind("tool"),
            core::GripperActuationKind::kPositionCommanded);
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  const auto stopped = backend.get_gripper_status("tool");
  EXPECT_EQ(stopped.state, core::GripperCommandState::kIdle);
  EXPECT_NE(stopped.generation, command.generation);
  EXPECT_TRUE(stopped.admission_closed);
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(
    Ros2GripperCommandBackendTest,
    PneumaticMockFingerFeedbackOnlyFeedsDisplayCacheIncludingAdmissionFallback) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto suffix = unique_suffix();
  const auto endpoint = "/manyforge_pneumatic_display_action_" + suffix;
  const auto topic = "/manyforge_pneumatic_display_state_" + suffix;
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  std::atomic<double> displayed{-1.0};
  auto config = pneumatic_mock_config(endpoint);
  config.state_topic = topic;
  config.state_joint_name = "raw_finger_joint";
  config.publish_display_position = [&](double value) {
    displayed = value;
    return core::Result<void>::Ok();
  };
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  JointStatePublisher publisher(topic);
  ASSERT_TRUE(publisher.wait_for_subscription());
  const auto before = backend.get_gripper_status("tool");
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    publisher.publish({"raw_finger_joint"}, {0.4});
    return displayed.load() == 0.4;
  }));
  const auto observed = backend.get_gripper_status("tool");
  EXPECT_EQ(observed.revision, before.revision);
  EXPECT_FALSE(observed.observed_position);
  EXPECT_FALSE(observed.current_position);
  ASSERT_TRUE(backend
                  .command_gripper(make_gripper_request(
                      0.0, 0.0, 0.0, "ros_gripper_command", "output_delivery"))
                  .accepted);
  EXPECT_DOUBLE_EQ(displayed.load(), 0.4);
  ASSERT_TRUE(server.wait_for_goal());
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kExecuting);
  server.result(0, true);
  EXPECT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kCompleted;
  }));
  EXPECT_TRUE(sink->reads().empty());
  EXPECT_TRUE(sink->single_updates().empty());
}

TEST(Ros2GripperCommandBackendTest,
     PositionActionStopKeepsExistingAsyncCancelAndIdleGenerationSemantics) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_position_action_stop_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "position_reached";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "ros_gripper_command", "position_reached"));
  ASSERT_TRUE(command.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  const auto stopped = backend.get_gripper_status("tool");
  EXPECT_EQ(stopped.state, core::GripperCommandState::kIdle);
  EXPECT_NE(stopped.generation, command.generation);
  EXPECT_TRUE(stopped.admission_closed);
  EXPECT_TRUE(
      wait_for_gripper_fact([&] { scheduler.advance_to(clock.now()); return server.cancels.load() == 1U; }));
  EXPECT_TRUE(stopped.evidence.empty());
}

// A Stop sends its cancel on the calling thread under the action leaf: the
// fake server sees it although the ManualScheduler is never advanced.
TEST(Ros2GripperCommandBackendTest, PositionActionStopSendsCancelWithoutSchedulerTick) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_position_stop_direct_cancel_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "position_reached";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "ros_gripper_command", "position_reached"));
  ASSERT_TRUE(command.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  // The Stop cancels the accepted goal handle (a goal response that arrives
  // after the Stop is a different, callback-side path).
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return Ros2GripperCommandBackendTestPeer::goal_accepted(backend, "tool"); }));
  const auto stop_control_before =
      scheduler.pending_count(core::SchedulerPriority::StopControl);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  EXPECT_EQ(scheduler.pending_count(core::SchedulerPriority::StopControl),
            stop_control_before);
  EXPECT_TRUE(wait_for_gripper_fact([&] { return server.cancels.load() == 1U; }));
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(server.cancels.load(), 1U);
  const auto stopped = backend.get_gripper_status("tool");
  EXPECT_EQ(stopped.state, core::GripperCommandState::kIdle);
  EXPECT_NE(stopped.generation, command.generation);
}

// Holds the gripper's owner thread inside one dispatched task until released
// (or 3 s), so a call that still dispatched to the owner would not return.
class BlockedGripperOwner final {
public:
  explicit BlockedGripperOwner(Ros2TransportResources &owner)
      : thread_([this, &owner] {
          owner.invoke([this] {
            entered_.set_value();
            (void)release_future_.wait_for(3s);
          });
          finished_.store(true);
        }) {
    entered = entered_future_.wait_for(3s) == std::future_status::ready;
  }
  ~BlockedGripperOwner() {
    release();
    thread_.join();
  }
  void release() {
    std::call_once(released_, [this] { release_.set_value(); });
  }
  bool finished() const { return finished_.load(); }
  bool entered{false};

private:
  std::promise<void> entered_;
  std::future<void> entered_future_ = entered_.get_future();
  std::promise<void> release_;
  std::future<void> release_future_ = release_.get_future();
  std::once_flag released_;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};

// Readiness graph reads and authority/lifecycle state changes run on the
// calling thread: none waits on the gripper's owner thread.
TEST(Ros2GripperCommandBackendTest,
     ReadinessAuthorizePrepareAndShutdownDoNotWaitForBlockedOwner) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_blocked_owner_lifecycle_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  config.completion_policy = "position_reached";
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));

  BlockedGripperOwner blocked(*resources);
  ASSERT_TRUE(blocked.entered);
  const auto started = std::chrono::steady_clock::now();
  const auto readiness = backend.gripper_transport_readiness("tool");
  const auto authorized = backend.try_authorize_gripper_commands();
  const auto prepared = backend.prepare_lifecycle_gripper("tool");
  const auto lifecycle_authorized = backend.authorize_lifecycle_gripper("tool");
  backend.shutdown();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const bool returned_while_blocked = !blocked.finished();
  blocked.release();

  EXPECT_TRUE(returned_while_blocked);
  EXPECT_LT(elapsed, 1s);
  EXPECT_TRUE(readiness.ready);
  EXPECT_TRUE(authorized.ok());
  EXPECT_TRUE(prepared.succeeded);
  EXPECT_TRUE(lifecycle_authorized.succeeded);
  EXPECT_TRUE(backend.get_gripper_status("tool").admission_closed);
}

core::AddressedGripperCommandIdentityDTO committed_identity() {
  return {core::OperationId{"pneumatic_operation"}, core::OperationRevision{1U},
          core::OperationParticipantId{"pneumatic_participant"},
          core::ControllerGoalId{"pneumatic_goal"},
          core::ControllerGoalGeneration{1U}};
}

TEST(Ros2GripperCommandBackendTest,
     GenuineCommittedPneumaticOwnerAndManualAdmissionSurviveStop) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_pneumatic_committed_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto before = backend.get_gripper_status("tool");
  auto request = make_gripper_request(0.0, 0.2, 0.0, "ros_gripper_command",
                                      "output_delivery");
  request.addressed_identity = committed_identity();
  request.owner_lifecycle_generation = before.lifecycle_generation;
  auto admission_token = std::make_shared<std::atomic<std::uint64_t>>(1);
  request.admission_token = admission_token;
  request.admission_token_expected = 1;
  const auto identity = *request.addressed_identity;
  std::atomic<std::size_t> checks{0};
  request.committed_owner_is_current = [&](const auto &supplied) {
    ++checks;
    return supplied == identity;
  };
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  const auto held = backend.get_gripper_status("tool");
  EXPECT_EQ(held.lifecycle_generation, before.lifecycle_generation);
  auto address_only = request;
  address_only.committed_owner_is_current = {};
  address_only.owner_lifecycle_generation.reset();
  EXPECT_TRUE(backend.validate_gripper_command(address_only).ok());
  auto invalid_proof = request;
  invalid_proof.committed_owner_is_current = [](const auto &) { return false; };
  EXPECT_FALSE(backend.command_gripper(invalid_proof).accepted);
  admission_token->store(2);
  EXPECT_FALSE(backend.command_gripper(request).accepted);
  EXPECT_EQ(server.requests.load(), 0U);
  admission_token->store(1);
  const auto command = backend.command_gripper(request);
  ASSERT_TRUE(command.accepted);
  ASSERT_TRUE(server.wait_for_goal());
  EXPECT_GE(checks.load(), 3U); // Entry, owner allocation and pre-send.
  const auto checks_at_return = checks.load();
  // The proof closure may be destroyed as soon as the synchronous call returns.
  request.committed_owner_is_current = {};
  server.result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return has_gripper_fact(backend.get_gripper_status("tool"),
                            "pneumatic_output_delivery_confirmed");
  }));
  EXPECT_EQ(clock.now(), 0);
  const auto completed = backend.get_gripper_status("tool");
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_FALSE(completed.admission_closed);
  EXPECT_EQ(completed.generation, command.generation);
  EXPECT_EQ(checks.load(), checks_at_return);
  EXPECT_EQ(server.requests.load(), 1U);
  EXPECT_EQ(server.cancels.load(), 0U);
}

TEST(
    Ros2GripperCommandBackendTest,
    CommittedOwnerProofCannotBypassPriorDestructiveFenceOrFailedSessionValidation) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint =
      "/manyforge_pneumatic_destructive_fence_" + unique_suffix();
  ControlledPneumaticServer server(*resources, endpoint);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  auto request = make_gripper_request(0.0, 0.0, 0.0, "ros_gripper_command",
                                      "output_delivery");
  request.addressed_identity = committed_identity();
  request.owner_lifecycle_generation =
      backend.get_gripper_status("tool").lifecycle_generation;
  request.committed_owner_is_current = [](const auto &) { return true; };
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  EXPECT_GT(backend.get_gripper_status("tool").lifecycle_generation,
            *request.owner_lifecycle_generation);
  EXPECT_FALSE(backend.command_gripper(request).accepted);
  // The later Stop must never mask an earlier Reset fence.
  request.owner_lifecycle_generation =
      backend.get_gripper_status("tool").lifecycle_generation;
  request.committed_owner_is_current = [](const auto &) { return true; };
  EXPECT_FALSE(backend.command_gripper(request).accepted);
  EXPECT_TRUE(backend.get_gripper_status("tool").admission_closed);
  backend.authorize_gripper_commands();
  request.committed_owner_is_current = [](const auto &) { return false; };
  EXPECT_FALSE(backend.command_gripper(request).accepted);
  request.committed_owner_is_current = [](const auto &) -> bool {
    throw std::runtime_error("owner proof unavailable");
  };
  EXPECT_FALSE(backend.command_gripper(request).accepted);
  EXPECT_EQ(server.requests.load(), 0U);
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
}

TEST(
    Ros2GripperCommandBackendTest,
    PneumaticUnavailableServerRefusesImmediatelyAndReadyOwnerCanSendDuringStop) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint =
      "/manyforge_pneumatic_preflight_stop_" + unique_suffix();
  std::unique_ptr<ControlledPneumaticServer> server;
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_mock_config(endpoint);
  config.wait_for_action_server_timeout_s = 3.0;
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  auto request = make_gripper_request(0.0, 0.0, 0.0, "ros_gripper_command",
                                      "output_delivery");
  request.addressed_identity = committed_identity();
  request.owner_lifecycle_generation =
      backend.get_gripper_status("tool").lifecycle_generation;
  request.admission_token = std::make_shared<std::atomic<std::uint64_t>>(1);
  request.admission_token_expected = 1;
  request.committed_owner_is_current = [](const auto &) { return true; };
  const auto readiness = backend.gripper_transport_readiness("tool");
  EXPECT_FALSE(readiness.ready);
  EXPECT_EQ(readiness.reason_codes,
            std::vector<std::string>{"gripper_action_server_unavailable"});
  auto in_flight = std::async(std::launch::async,
                              [&] { return backend.command_gripper(request); });
  ASSERT_EQ(in_flight.wait_for(500ms), std::future_status::ready);
  const auto rejected = in_flight.get();
  EXPECT_FALSE(rejected.accepted);
  ASSERT_TRUE(rejected.error);
  EXPECT_EQ(rejected.error->code,
            ros2_control_fault_reason::kGripperActionServerUnavailable);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  server = std::make_unique<ControlledPneumaticServer>(*resources, endpoint);
  ASSERT_TRUE(wait_for_gripper_fact(
      [&] { return backend.gripper_transport_readiness("tool").ready; }));
  const auto command = backend.command_gripper(request);
  ASSERT_TRUE(command.accepted);
  ASSERT_TRUE(server->wait_for_goal());
  server->result(0, true);
  ASSERT_TRUE(wait_for_gripper_fact([&] {
    return backend.get_gripper_status("tool").state ==
           core::GripperCommandState::kCompleted;
  }));
  EXPECT_FALSE(backend.get_gripper_status("tool").admission_closed);
  EXPECT_EQ(server->requests.load(), 1U);
  EXPECT_EQ(server->cancels.load(), 0U);
}

TEST(Ros2GripperCommandBackendTest,
     SharedRosPneumaticFinalResultDoesNotReopenPositionAdmissionAfterStop) {
  for (bool position_was_active : {false, true}) {
    SCOPED_TRACE(position_was_active);
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    sink->set_joint_position("position_gripper", "position_finger_joint", 0.7);
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler,
                                      false);
    const auto endpoint = "/manyforge_shared_pneumatic_" + unique_suffix();
    const auto position_endpoint =
        "/manyforge_shared_position_" + unique_suffix();
    ControlledPneumaticServer pneumatic(*resources, endpoint);
    ControlledPneumaticServer position(*resources, position_endpoint);
    TestTransportLifetime lifetime(*resources, backend);
    backend.configure_gripper_action("tool", pneumatic_mock_config(endpoint), 1'000'000'000);
    auto position_config = pneumatic_mock_config(position_endpoint);
    position_config.group_id = "position_gripper";
    position_config.joint_name = "position_finger_joint";
    position_config.completion_policy = "position_reached";
    position_config.actuation_kind =
        core::GripperActuationKind::kPositionCommanded;
    backend.configure_gripper_action("position", position_config, 1'000'000'000);
    backend.complete_ros_configuration();
    lifetime.start();
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return backend.gripper_transport_readiness("tool").ready; }));
    auto position_request = make_gripper_request(
        0.0, 0.2, 0.0, "ros_gripper_command", "position_reached");
    position_request.gripper_id = "position";
    position_request.group_id = "position_gripper";
    position_request.joint_name = "position_finger_joint";
    if (position_was_active) {
      ASSERT_TRUE(backend.command_gripper(position_request).accepted);
      ASSERT_TRUE(position.wait_for_goal());
    }
    const auto command = backend.command_gripper(make_gripper_request(
        0.0, 0.0, 0.0, "ros_gripper_command", "output_delivery"));
    ASSERT_TRUE(command.accepted);
    ASSERT_TRUE(pneumatic.wait_for_goal());
    backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
    const auto position_fenced = backend.get_gripper_status("position");
    EXPECT_TRUE(position_fenced.admission_closed);
    EXPECT_EQ(position_fenced.state, core::GripperCommandState::kIdle);
    pneumatic.result(0, true);
    ASSERT_TRUE(wait_for_gripper_fact([&] {
      return backend.get_gripper_status("tool").state ==
             core::GripperCommandState::kCompleted;
    }));
    // Real transport/owner complement to E's genuine Engine/Kernel ACK test.
    // Reading the final result is not an emulated acknowledge_completed call.
    const auto consumed = backend.get_gripper_status("tool");
    EXPECT_EQ(consumed.command_id, command.command_id);
    EXPECT_FALSE(consumed.admission_closed);
    EXPECT_TRUE(
        backend
            .validate_gripper_command(make_gripper_request(
                0.7, 0.0, 0.0, "ros_gripper_command", "output_delivery"))
            .ok());
    ASSERT_TRUE(backend
                    .command_gripper(make_gripper_request(0.7, 0.0, 0.0,
                                                          "ros_gripper_command",
                                                          "output_delivery"))
                    .accepted);
    ASSERT_TRUE(pneumatic.wait_for_goal(2));
    pneumatic.result(1, true);
    ASSERT_TRUE(wait_for_gripper_fact([&] {
      return backend.get_gripper_status("tool").state ==
             core::GripperCommandState::kCompleted;
    }));
    EXPECT_TRUE(backend.get_gripper_status("position").admission_closed);
    EXPECT_FALSE(backend.command_gripper(position_request).accepted);
    EXPECT_EQ(position.requests.load(), position_was_active ? 1U : 0U);
    EXPECT_EQ(pneumatic.cancels.load(), 0U);
    if (position_was_active) {
      // The position cancel is queued on the StopControl lane (DX-4018).
      EXPECT_TRUE(wait_for_gripper_fact([&] {
        scheduler.advance_to(clock.now(), core::SchedulerPriority::StopControl);
        return position.cancels.load() == 1U;
      }));
    }
    // Explicit existing recovery reopens the shared backend. In a real active
    // position session its existing Stop settlement may perform this reset;
    // this isolated transport proof intentionally has no position Engine.
    // A cancelled position goal must reach its terminal before Reset can
    // re-authorize (lifecycle-disable settlement).
    if (position_was_active) position.canceled(0);
    backend.invalidate_gripper_commands(
        core::GripperInvalidationReason::kReset);
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return backend.try_authorize_gripper_commands().ok(); }));
    const auto recovered = backend.command_gripper(position_request);
    ASSERT_TRUE(recovered.accepted);
    EXPECT_FALSE(backend.get_gripper_status("position").admission_closed);
  }
}

Ros2GripperActionConfig pneumatic_none_config() {
  Ros2GripperActionConfig config;
  config.state_freshness_timeout_s = 1.0;
  config.actuation_kind = core::GripperActuationKind::kPneumatic;
  config.output_mode = core::GripperOutputMode::kNone;
  config.transport_kind.clear();
  config.action_name.clear();
  config.completion_policy = "no_output";
  config.group_id = "gripper";
  config.joint_name = "finger_joint";
  return config;
}

TEST(Ros2GripperCommandBackendTest, PneumaticNoneNeedsNoBudgetWithoutFeedback) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);

  auto no_feedback = pneumatic_none_config();
  no_feedback.state_freshness_timeout_s = 0.0;
  EXPECT_NO_THROW(backend.configure_gripper_action("no_feedback", no_feedback, 1'000'000'000));

  auto with_feedback = no_feedback;
  with_feedback.state_topic = "/gripper/state";
  with_feedback.state_joint_name = "finger_joint";
  EXPECT_THROW(backend.configure_gripper_action("with_feedback", with_feedback, 1'000'000'000),
               std::invalid_argument);
  no_feedback.state_freshness_timeout_s = -1.0;
  EXPECT_THROW(backend.configure_gripper_action("invalid_budget", no_feedback, 1'000'000'000),
               std::invalid_argument);
}

core::GripperCommandRequestDTO pneumatic_none_request(double dwell) {
  auto request =
      make_gripper_request(0.0, dwell, 0.0, "resolved_binding", "no_output");
  request.grasp_confirmation_policy = "not_required";
  return request;
}

void expect_no_pneumatic_delivery(const core::GripperCommandStatusDTO &status,
                                  bool projected = true) {
  EXPECT_TRUE(has_gripper_fact(status, "pneumatic_no_output"));
  EXPECT_FALSE(has_gripper_fact(status, "pneumatic_output_delivery_confirmed"));
  EXPECT_FALSE(
      has_gripper_fact(status, "pneumatic_output_delivery_unconfirmed"));
  EXPECT_FALSE(has_gripper_fact(status, "ros_gripper_command_enqueued"));
  EXPECT_FALSE(has_gripper_fact(status, "isaac_gripper_command_published"));
  EXPECT_FALSE(status.current_position.has_value());
  EXPECT_FALSE(status.observed_position.has_value());
  EXPECT_EQ(status.transport_kind, "");
  EXPECT_EQ(status.projection_state,
            projected ? core::GripperProjectionState::kApplied
                      : core::GripperProjectionState::kNotRequired);
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneZeroCompletesLocallyWithNoPhysicalPathOrDeliveryFact) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_none_config(), 1'000'000'000);
  EXPECT_EQ(backend.gripper_output_mode("tool"),
            core::GripperOutputMode::kNone);
  EXPECT_EQ(backend.gripper_output_mode("missing"),
            core::GripperOutputMode::kCommanded);
  const auto local = backend.gripper_transport_readiness("tool");
  EXPECT_TRUE(local.configured);
  EXPECT_TRUE(local.logical_owner_ready);
  EXPECT_TRUE(local.ready);
  EXPECT_EQ(local.output_mode, core::GripperOutputMode::kNone);
  EXPECT_EQ(local.transport_kind, "");
  EXPECT_FALSE(local.command_path_ready);
  EXPECT_FALSE(local.feedback_path_ready);
  EXPECT_EQ(local.feedback_publisher_count, 0U);
  EXPECT_TRUE(local.reason_codes.empty());
  EXPECT_TRUE(
      backend.validate_gripper_command(pneumatic_none_request(0.0)).ok());
  const auto command = backend.command_gripper(pneumatic_none_request(0.0));
  ASSERT_TRUE(command.accepted);
  const auto completed = backend.get_gripper_status("tool");
  EXPECT_EQ(completed.command_id, command.command_id);
  EXPECT_EQ(completed.generation, command.generation);
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_DOUBLE_EQ(completed.progress, 1.0);
  EXPECT_EQ(completed.completion_policy, "no_output");
  EXPECT_EQ(completed.grasp_confirmation_policy, "not_required");
  EXPECT_EQ(clock.now(), 0);
  EXPECT_EQ(scheduler.pending_count(core::SchedulerPriority::ControllerCommand),
            0U);
  expect_no_pneumatic_delivery(completed);
  EXPECT_TRUE(sink->reads().empty());
  ASSERT_EQ(sink->single_updates().size(), 1U);
  EXPECT_DOUBLE_EQ(sink->single_updates().front().position_rad, 0.0);
  // A genuinely configured owner disappears on shutdown, not as a fake CM row.
  lifetime.close();
  EXPECT_FALSE(backend.gripper_transport_readiness("tool").logical_owner_ready);
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneIgnoresProgramDwellAndRemainsAdmissibleDuringStop) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_none_config();
  config.result_timeout_s = 0.001; // None has no delivery deadline.
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  clock.advance(150'000'000);
  const auto command = backend.command_gripper(pneumatic_none_request(0.2));
  ASSERT_TRUE(command.accepted);
  const auto completed = backend.get_gripper_status("tool");
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(completed.command_id, command.command_id);
  EXPECT_EQ(completed.generation, command.generation);
  EXPECT_EQ(clock.now(), 150'000'000);
  EXPECT_EQ(scheduler.pending_count(core::SchedulerPriority::ControllerCommand),
            0U);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  EXPECT_FALSE(backend.get_gripper_status("tool").admission_closed);
  EXPECT_TRUE(backend.command_gripper(pneumatic_none_request(0.0)).accepted);
  EXPECT_FALSE(completed.error.has_value());
  expect_no_pneumatic_delivery(completed);
  EXPECT_EQ(sink->single_updates().size(), 2U);
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneRequiresExplicitChoiceAndRejectsPhysicalOrFeedbackSemantics) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  auto config = pneumatic_none_config();
  config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
  EXPECT_THROW(backend.configure_gripper_action("position_none", config, 1'000'000'000),
               std::invalid_argument);
  config = pneumatic_none_config();
  config.action_name = "/must_not_create_action_client";
  EXPECT_THROW(backend.configure_gripper_action("action_none", config, 1'000'000'000),
               std::invalid_argument);
  config = pneumatic_none_config();
  config.command_topic = "/must_not_publish_none";
  EXPECT_THROW(backend.configure_gripper_action("topic_none", config, 1'000'000'000),
               std::invalid_argument);
  config = pneumatic_none_config();
  config.transport_kind = "ros_gripper_command";
  EXPECT_THROW(backend.configure_gripper_action("transport_none", config, 1'000'000'000),
               std::invalid_argument);
  config = pneumatic_none_config();
  config.output_mode = core::GripperOutputMode::kCommanded;
  EXPECT_THROW(
      backend.configure_gripper_action("missing_commanded_endpoint", config, 1'000'000'000),
      std::invalid_argument);
  config = pneumatic_none_config();
  config.completion_policy = "driver_result";
  EXPECT_THROW(backend.configure_gripper_action("fake_result_none", config, 1'000'000'000),
               std::invalid_argument);
  backend.configure_gripper_action("tool", pneumatic_none_config(), 1'000'000'000);
  auto crossed = pneumatic_none_request(0.0);
  crossed.grasp_confirmation_policy = "binding_feedback_required";
  EXPECT_FALSE(backend.validate_gripper_command(crossed).ok());
  EXPECT_FALSE(backend.command_gripper(crossed).accepted);
  crossed = pneumatic_none_request(0.0);
  crossed.completion_policy = "goal_fenced_target_reached";
  EXPECT_FALSE(backend.command_gripper(crossed).accepted);
  for (double invalid : {-0.1, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_FALSE(
        backend.command_gripper(pneumatic_none_request(invalid)).accepted);
  }
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneCommittedOwnerUsesSameStopAndDestructiveLifecycleProof) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_none_config(), 1'000'000'000);
  auto request = pneumatic_none_request(0.2);
  request.addressed_identity = committed_identity();
  request.owner_lifecycle_generation =
      backend.get_gripper_status("tool").lifecycle_generation;
  const auto exact = *request.addressed_identity;
  request.committed_owner_is_current = [exact](const auto &identity) {
    return identity == exact;
  };
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kStop);
  auto address_only = request;
  address_only.owner_lifecycle_generation.reset();
  address_only.committed_owner_is_current = {};
  EXPECT_TRUE(backend.validate_gripper_command(address_only).ok());
  const auto command = backend.command_gripper(request);
  ASSERT_TRUE(command.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  EXPECT_FALSE(backend.command_gripper(request).accepted);
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  const auto reset = backend.get_gripper_status("tool");
  EXPECT_EQ(reset.state, core::GripperCommandState::kIdle);
  EXPECT_NE(reset.generation, command.generation);
  EXPECT_TRUE(reset.admission_closed);
  EXPECT_FALSE(has_gripper_fact(reset, "pneumatic_output_delivery_confirmed"));
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneNeedsNoSchedulerAndNeverClaimsDelivery) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  RejectingScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_none_config(), 1'000'000'000);
  const auto command = backend.command_gripper(pneumatic_none_request(0.2));
  ASSERT_TRUE(command.accepted);
  const auto completed = backend.get_gripper_status("tool");
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(completed.generation, command.generation);
  EXPECT_EQ(clock.now(), 0);
  EXPECT_FALSE(
      has_gripper_fact(completed, "gripper_lifecycle_schedule_failed"));
  expect_no_pneumatic_delivery(completed);
  EXPECT_FALSE(completed.error.has_value());
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneResetCanRetireAnInFlightCommandedProjection) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_single_update();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_none_config(), 1'000'000'000);

  auto command = std::async(std::launch::async, [&] {
    return backend.command_gripper(pneumatic_none_request(0.0));
  });
  ASSERT_TRUE(sink->wait_for_single_update());
  const auto pending = backend.get_gripper_status("tool");
  EXPECT_EQ(pending.state, core::GripperCommandState::kExecuting);
  EXPECT_FALSE(pending.observed_position.has_value());
  EXPECT_EQ(command.wait_for(50ms), std::future_status::timeout);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  EXPECT_EQ(backend.get_gripper_status("tool").state,
            core::GripperCommandState::kIdle);
  EXPECT_TRUE(backend.try_authorize_gripper_commands().ok());

  sink->release_single_update();
  ASSERT_TRUE(command.get().accepted);
  const auto retired = backend.get_gripper_status("tool");
  EXPECT_EQ(retired.state, core::GripperCommandState::kIdle);
  EXPECT_FALSE(retired.observed_position.has_value());
  ASSERT_EQ(sink->single_updates().size(), 1U);
  EXPECT_DOUBLE_EQ(sink->single_updates().front().position_rad, 0.0);
  EXPECT_DOUBLE_EQ(*sink->group_joint_position("gripper", "finger_joint"), 0.0);
  auto next = pneumatic_none_request(0.0);
  next.target_position = 0.7;
  ASSERT_TRUE(backend.command_gripper(next).accepted);
  EXPECT_DOUBLE_EQ(*sink->group_joint_position("gripper", "finger_joint"), 0.7);
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneProjectionFailureIsReportedAsCommandFailure) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->fail_next_single_updates(1);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_none_config(), 1'000'000'000);

  const auto command = backend.command_gripper(pneumatic_none_request(0.0));
  ASSERT_TRUE(command.accepted);
  const auto failed = backend.get_gripper_status("tool");
  EXPECT_EQ(failed.state, core::GripperCommandState::kFailed);
  EXPECT_EQ(failed.projection_state, core::GripperProjectionState::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  EXPECT_EQ(failed.error->code, "gripper_projection_failed");
  EXPECT_FALSE(failed.observed_position.has_value());
}

TEST(
    Ros2GripperCommandBackendTest,
    PneumaticNoneLogicalReadinessCannotSupplyCommandedSiblingPhysicalReadiness) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action("tool", pneumatic_none_config(), 1'000'000'000);
  auto commanded = pneumatic_mock_config(
      "/manyforge_missing_commanded_sibling_" + unique_suffix());
  commanded.wait_for_action_server_timeout_s = 0.0;
  backend.configure_gripper_action("commanded", commanded, 1'000'000'000);
  const auto local = backend.gripper_transport_readiness("tool");
  const auto physical = backend.gripper_transport_readiness("commanded");
  EXPECT_TRUE(local.logical_owner_ready);
  EXPECT_TRUE(local.ready);
  EXPECT_FALSE(physical.logical_owner_ready);
  EXPECT_FALSE(physical.command_path_ready);
  EXPECT_FALSE(physical.ready);
  EXPECT_EQ(physical.output_mode, core::GripperOutputMode::kCommanded);
  EXPECT_EQ(physical.reason_codes,
            std::vector<std::string>{"gripper_action_server_unavailable"});
  EXPECT_TRUE(backend.command_gripper(pneumatic_none_request(0.0)).accepted);
  auto request = make_gripper_request(0.0, 0.0, 0.0, "ros_gripper_command",
                                      "output_delivery");
  request.gripper_id = "commanded";
  const auto rejected = backend.command_gripper(request);
  EXPECT_FALSE(rejected.accepted);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code,
            ros2_control_fault_reason::kGripperActionServerUnavailable);
}

core::Result<core::RuntimeParticipantBinding> producer_gripper_binding() {
  using namespace core;
  const auto digest = [](char digit) {
    return "sha256:" + std::string(64, digit);
  };
  // Reduced linear-gripper form of Core's existing make_binding fixture. The
  // normal Core codec seals it before the real producer consumes it.
  return verify_runtime_participant_binding(RuntimeParticipantBindingDTO{
      ProgramId("program.ros_pair"),
      ProgramRevision(digest('0')),
      ProgramBindingId(digest('1')),
      RetainedBundleId(digest('6')),
      GraphRevision(digest('7')),
      ControllableComponentId("robot.fixture"),
      ComponentTypeId("aggregate.robot"),
      ComponentTypeRevision("aggregate-revision"),
      CommandTargetId("tool"),
      CapabilityId("tool.gripper.command"),
      CapabilityDefinitionRevision("capability-revision"),
      ProgramCommandSchemaRefDTO{
          CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
          CommandSchemaRevision(
              std::string(kGripperPositionCommandSchemaRevision))},
      {ProgramModelRoleBindingDTO{ModelRoleId("body"),
                                  ModelInstanceId("robot.model"),
                                  ModelRevision("model-revision")}},
      {RuntimeModelGroupBindingDTO{ModelGroupRoleId("command_group"),
                                   ModelInstanceId("robot.model"),
                                   "gripper",
                                   {"finger_joint"}}},
      {ProgramFrameBindingDTO{
           {ControllableComponentId("robot.fixture"), FrameRoleId("base")}, ModelInstanceId("robot.model"),
           ModelRevision("model-revision"), "fixture_base"},
       ProgramFrameBindingDTO{
           {ControllableComponentId("robot.fixture"), FrameRoleId("tcp")}, ModelInstanceId("robot.model"),
           ModelRevision("model-revision"), "fixture_tcp"}},
      {RuntimeResourceBindingDTO{ResourceRoleId("fingers"),
                                 ResourceId("robot.finger"), "finger_joint",
                                 "finger_joint", CoordinateKind::kLinear}},
      {},
      RuntimeControlPathDTO{ControlDomainId("domain.gripper"),
                            ControlDomainRevision(digest('8')),
                            ControlBindingId("binding.gripper"),
                            ControlBindingRevision(digest('9')),
                            ControlInterfaceContractId("interface.fixture"),
                            ControlInterfaceRevision(digest('a')),
                            ControllerTopologyId("topology.fixture"),
                            TopologyRevision(digest('b')),
                            ControlRouteId("route.fixture"),
                            ActivationSetId("activation.fixture"),
                            ActiveControlConfigurationId("active.fixture"),
                            ActiveControlConfigurationGeneration(1U),
                            ProviderInstanceId("provider.fixture"),
                            ProviderManifestId("provider-manifest.fixture"),
                            ProviderManifestRevision(digest('c')),
                            RuntimeUnitId("runtime.fixture"),
                            RuntimeEpoch("runtime-epoch.1"),
                            StateAuthorityId("state.fixture"),
                            StateAuthorityClass::kPredicted,
                            StopEvidencePolicyId("stop.fixture"),
                            StopEvidencePolicyRevision(digest('d')),
                            ContainmentGranularity::kResource,
                            std::nullopt},
      std::nullopt,
      {},
      {RuntimeUnitEpochBindingDTO{RuntimeUnitId("runtime.fixture"),
                                  RuntimeEpoch("runtime-epoch.1")}},
      {}});
}

TEST(
    Ros2GripperCommandBackendTest,
    ActualCoreProducerRequestReachesRealBackendForCommandedPneumaticNoneAndPosition) {
  using namespace core;
  const auto binding = producer_gripper_binding();
  ASSERT_TRUE(binding.ok()) << binding.error().message;
  const CapabilityCommandIntentDTO command{
      CommandSchemaId(std::string(kGripperPositionCommandSchemaId)),
      CommandSchemaRevision(std::string(kGripperPositionCommandSchemaRevision)),
      GripperPositionCommandValueDTO{LinearPositionDTO{0.0}, std::nullopt},
      2.0,
      {CommandOptionDTO{CommandOptionId("command_duration_s"), 0.03}}};
  for (const auto &[actuation, output] :
       {std::pair{GripperActuationKind::kPneumatic,
                  GripperOutputMode::kCommanded},
        std::pair{GripperActuationKind::kPneumatic, GripperOutputMode::kNone},
        std::pair{GripperActuationKind::kPositionCommanded,
                  GripperOutputMode::kCommanded}}) {
    const auto produced =
        resource_addressed_kernel_detail::build_command_request(
            command, binding.value().value(), actuation, output);
    ASSERT_TRUE(produced.ok()) << produced.error().message;
    // No post-production policy/mode/metadata edits: the DTO goes verbatim.
    const auto &request = produced.value();
    EXPECT_EQ(request.gripper_id, "tool");
    EXPECT_EQ(request.group_id, "gripper");
    EXPECT_EQ(request.joint_name, "finger_joint");
    EXPECT_EQ(request.transport_kind, "resolved_binding");
    EXPECT_EQ(request.state_authority,
              binding.value().value().control_path->state_authority_id.value());
    EXPECT_EQ(request.evidence, std::vector<std::string>{"command_terminal"});
    EXPECT_EQ(request.evidence_state, "required_pending");
    EXPECT_DOUBLE_EQ(request.duration_s, 0.03);
    EXPECT_DOUBLE_EQ(request.completion_timeout_s, 2.0);
    EXPECT_FALSE(request.addressed_identity.has_value());
    EXPECT_FALSE(request.owner_lifecycle_generation.has_value());
    EXPECT_FALSE(static_cast<bool>(request.committed_owner_is_current));
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    sink->set_joint_position("gripper", "finger_joint", 0.7);
    FakeClock clock;
    ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler,
                                      false);
    const auto endpoint = "/manyforge_actual_core_producer_" + unique_suffix();
    std::unique_ptr<ControlledPneumaticServer> server;
    TestTransportLifetime lifetime(*resources, backend);
    auto config = output == GripperOutputMode::kNone
                      ? pneumatic_none_config()
                      : pneumatic_mock_config(endpoint);
    config.actuation_kind = actuation;
    config.output_mode = output;
    if (actuation == GripperActuationKind::kPositionCommanded)
      config.completion_policy = "position_reached";
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    if (output == GripperOutputMode::kCommanded)
      server =
          std::make_unique<ControlledPneumaticServer>(*resources, endpoint);
    backend.complete_ros_configuration();
    lifetime.start();
    if (server) {
      ASSERT_TRUE(wait_for_gripper_fact(
          [&] { return backend.gripper_transport_readiness("tool").ready; }));
    }
    EXPECT_TRUE(backend.validate_gripper_command(request).ok());
    const auto dispatched = backend.command_gripper(request);
    ASSERT_TRUE(dispatched.accepted)
        << (dispatched.error ? dispatched.error->message : "");
    if (server) {
      ASSERT_TRUE(server->wait_for_goal());
      server->result(0, true);
      ASSERT_TRUE(wait_for_gripper_fact([&] {
        const auto status = backend.get_gripper_status("tool");
        return actuation == GripperActuationKind::kPneumatic
                   ? has_gripper_fact(status,
                                      "pneumatic_output_delivery_confirmed")
                   : status.state == GripperCommandState::kFailed;
      }));
    }
    if (actuation == GripperActuationKind::kPneumatic) {
      EXPECT_EQ(backend.get_gripper_status("tool").state,
                GripperCommandState::kCompleted);
      EXPECT_EQ(request.completion_policy, output == GripperOutputMode::kNone
                                               ? "no_output"
                                               : "output_delivery");
      EXPECT_EQ(clock.now(), 0);
      ASSERT_TRUE(wait_for_gripper_fact([&] {
        return scheduler.pending_count(SchedulerPriority::ControllerCommand) ==
               0U;
      }));
      if (output == GripperOutputMode::kNone)
        expect_no_pneumatic_delivery(backend.get_gripper_status("tool"));
    } else {
      // Position retains its NaN action-position failure, proving that the
      // pneumatic delivery-only completion did not weaken the old guard.
      const auto failed = backend.get_gripper_status("tool");
      ASSERT_TRUE(failed.error.has_value());
      EXPECT_EQ(failed.error->code,
                ros2_control_fault_reason::kGripperStateNotFinite);
      EXPECT_EQ(request.completion_policy, "goal_fenced_target_reached");
      EXPECT_EQ(request.grasp_confirmation_policy, "binding_feedback_required");
    }
    auto wrong_version = command;
    wrong_version.command_schema_revision =
        CommandSchemaRevision("unsupported-revision");
    EXPECT_FALSE(resource_addressed_kernel_detail::build_command_request(
                     wrong_version, binding.value().value(), actuation, output)
                     .ok());
  }
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticNoneDisplaysNamedFeedbackOrInstantExpectedOnlyAfterAdmission) {
  for (bool named_feedback : {false, true}) {
    for (double dwell : {0.0, 0.2}) {
      SCOPED_TRACE(named_feedback);
      SCOPED_TRACE(dwell);
      auto sink = std::make_shared<RecordingExecutionStateSink>();
      core::FakeClock clock;
      core::ManualScheduler scheduler;
      std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
      Ros2GripperCommandBackend backend(resources, sink, clock, scheduler,
                                        false);
      std::mutex display_mutex;
      std::vector<double> display_positions;
      std::atomic<bool> callback_read_owner{false};
      TestTransportLifetime lifetime(*resources, backend);
      auto config = pneumatic_none_config();
      const auto state_topic =
          "/manyforge_none_display_feedback_" + unique_suffix();
      if (named_feedback) {
        config.state_topic = state_topic;
        config.state_joint_name = "raw_finger_joint";
      }
      config.publish_display_position = [&](double position) {
        // A reentrant read would deadlock if the display callback inherited
        // a backend map, runtime or authority lock.
        (void)backend.get_gripper_status("tool");
        callback_read_owner = true;
        std::lock_guard<std::mutex> lock(display_mutex);
        display_positions.push_back(position);
        return core::Result<void>::Ok();
      };
      backend.configure_gripper_action("tool", config, 1'000'000'000);
      backend.complete_ros_configuration();
      lifetime.start();
      backend.invalidate_gripper_commands(
          core::GripperInvalidationReason::kReset);
      EXPECT_FALSE(
          backend.command_gripper(pneumatic_none_request(dwell)).accepted);
      {
        std::lock_guard<std::mutex> lock(display_mutex);
        EXPECT_TRUE(display_positions.empty());
      }
      EXPECT_FALSE(callback_read_owner.load());
      backend.invalidate_gripper_commands(
          core::GripperInvalidationReason::kReset);
      backend.authorize_gripper_commands();
      std::unique_ptr<JointStatePublisher> publisher;
      if (named_feedback) {
        publisher = std::make_unique<JointStatePublisher>(state_topic);
        ASSERT_TRUE(publisher->wait_for_subscription());
        ASSERT_TRUE(wait_for_gripper_fact([&] {
          publisher->publish({"raw_finger_joint"}, {0.4});
          std::lock_guard<std::mutex> lock(display_mutex);
          return !display_positions.empty();
        }));
      }
      auto request = pneumatic_none_request(dwell);
      request.target_position = dwell == 0.0 ? 0.7 : 0.0;
      const auto command = backend.command_gripper(request);
      ASSERT_TRUE(command.accepted);
      EXPECT_EQ(callback_read_owner.load(), named_feedback);
      {
        std::lock_guard<std::mutex> lock(display_mutex);
        if (named_feedback)
          ASSERT_FALSE(display_positions.empty());
        else
          EXPECT_TRUE(display_positions.empty());
        for (double value : display_positions)
          EXPECT_DOUBLE_EQ(value, 0.4);
      }
      auto status = backend.get_gripper_status("tool");
      EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
      EXPECT_EQ(clock.now(), 0);
      backend.invalidate_gripper_commands(
          core::GripperInvalidationReason::kStop);
      status = backend.get_gripper_status("tool");
      EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
      EXPECT_EQ(status.command_id, command.command_id);
      EXPECT_EQ(status.generation, command.generation);
      expect_no_pneumatic_delivery(status, !named_feedback);
      const auto ready = backend.gripper_transport_readiness("tool");
      EXPECT_TRUE(ready.logical_owner_ready);
      EXPECT_FALSE(ready.command_path_ready);
      EXPECT_FALSE(ready.feedback_path_ready);
      EXPECT_TRUE(sink->reads().empty());
      EXPECT_EQ(sink->single_updates().size(), named_feedback ? 0U : 1U);
      if (!named_feedback) {
        EXPECT_DOUBLE_EQ(sink->single_updates().front().position_rad,
                         request.target_position);
      }
    }
  }
}

TEST(
    Ros2GripperCommandBackendTest,
    PneumaticTimeoutOmissionAndExplicitValuesRemainDistinctAndIgnoreProgramDwell) {
  for (const std::optional<double> timeout :
       {std::optional<double>{}, std::optional<double>{0.25},
        std::optional<double>{5.0}, std::optional<double>{15.0}}) {
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler,
                                      false);
    const auto endpoint =
        "/manyforge_pneumatic_timeout_values_" + unique_suffix();
    ControlledPneumaticServer server(*resources, endpoint);
    TestTransportLifetime lifetime(*resources, backend);
    auto config = pneumatic_mock_config(endpoint);
    config.result_timeout_s = timeout;
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    backend.complete_ros_configuration();
    lifetime.start();
    ASSERT_TRUE(wait_for_gripper_fact(
        [&] { return backend.gripper_transport_readiness("tool").ready; }));
    ASSERT_TRUE(backend
                    .command_gripper(make_gripper_request(0.0, 90.0, 0.0,
                                                          "ros_gripper_command",
                                                          "output_delivery"))
                    .accepted);
    ASSERT_TRUE(server.wait_for_goal());
    const auto deadline =
        static_cast<core::Duration>(timeout.value_or(1.0) * 1e9);
    clock.advance(deadline - 1);
    scheduler.advance_to(clock.now());
    EXPECT_EQ(backend.get_gripper_status("tool").state,
              core::GripperCommandState::kExecuting);
    clock.advance(1);
    scheduler.advance_to(clock.now());
    const auto failed = backend.get_gripper_status("tool");
    EXPECT_EQ(failed.state, core::GripperCommandState::kFailed);
    ASSERT_TRUE(failed.error);
    EXPECT_EQ(failed.error->code,
              ros2_control_fault_reason::kGripperResultTimeout);
    EXPECT_EQ(server.cancels.load(), 0U);
    server.result(0, true);
    ASSERT_TRUE(wait_for_gripper_fact([&] {
      return has_gripper_fact(backend.get_gripper_status("tool"),
                              "ros_gripper_command_late_result_succeeded");
    }));
    EXPECT_EQ(backend.get_gripper_status("tool").state,
              core::GripperCommandState::kFailed);
  }
}

TEST(Ros2GripperCommandBackendTest,
     PneumaticTimingRejectsZeroNegativeAndNonfiniteValuesIncludingNone) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  for (bool none : {false, true}) {
    for (double timeout : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
      auto config = none ? pneumatic_none_config()
                         : pneumatic_mock_config("/manyforge_invalid_timeout");
      config.result_timeout_s = timeout;
      EXPECT_THROW(backend.configure_gripper_action("invalid", config, 1'000'000'000),
                   std::invalid_argument);
    }
  }
  auto config = pneumatic_mock_config("/manyforge_stale_pneumatic_policy");
  for (const char *policy : {"position_reached", "driver_result",
                             "dwell_elapsed", "delivered_then_dwell"}) {
    config.completion_policy = policy;
    EXPECT_THROW(backend.configure_gripper_action("stale", config, 1'000'000'000),
                 std::invalid_argument);
  }
}

TEST(
    Ros2GripperCommandBackendTest,
    PositionTimeoutOmissionZeroFiveAndFifteenKeepDurationPlusTimeoutAndZeroDisable) {
  for (const std::optional<double> timeout :
       {std::optional<double>{}, std::optional<double>{0.0},
        std::optional<double>{5.0}, std::optional<double>{15.0}}) {
    auto sink = std::make_shared<RecordingExecutionStateSink>();
    sink->set_joint_position("gripper", "finger_joint", 0.7);
    core::FakeClock clock;
    core::ManualScheduler scheduler;
    std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
    Ros2GripperCommandBackend backend(resources, sink, clock, scheduler,
                                      false);
    const auto endpoint =
        "/manyforge_position_timeout_values_" + unique_suffix();
    ControlledPneumaticServer server(*resources, endpoint);
    TestTransportLifetime lifetime(*resources, backend);
    auto config = pneumatic_mock_config(endpoint);
    config.actuation_kind = core::GripperActuationKind::kPositionCommanded;
    config.completion_policy = "position_reached";
    config.result_timeout_s = timeout;
    backend.configure_gripper_action("tool", config, 1'000'000'000);
    backend.complete_ros_configuration();
    lifetime.start();
    ASSERT_TRUE(backend
                    .command_gripper(make_gripper_request(0.0, 0.25, 0.0,
                                                          "ros_gripper_command",
                                                          "position_reached"))
                    .accepted);
    ASSERT_TRUE(server.wait_for_goal());
    const double effective = timeout.value_or(5.0);
    if (effective == 0.0) {
      clock.advance(90'000'000'000);
      scheduler.advance_to(clock.now());
      EXPECT_EQ(backend.get_gripper_status("tool").state,
                core::GripperCommandState::kExecuting);
    } else {
      const auto deadline =
          static_cast<core::Duration>((effective + 0.25) * 1e9);
      clock.advance(deadline - 1);
      scheduler.advance_to(clock.now());
      EXPECT_EQ(backend.get_gripper_status("tool").state,
                core::GripperCommandState::kExecuting);
      clock.advance(1);
      scheduler.advance_to(clock.now());
      const auto failed = backend.get_gripper_status("tool");
      EXPECT_EQ(failed.state, core::GripperCommandState::kFailed);
      ASSERT_TRUE(failed.error);
      EXPECT_EQ(failed.error->code,
                ros2_control_fault_reason::kGripperResultTimeout);
    }
  }
}

TEST(
    Ros2GripperCommandBackendTest,
    PositionRetainsDefaultTwoSecondDiscoveryWaitWhenServerAppearsAfterCommand) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<Ros2TransportResources> resources = test_transport_resources(sink, clock, "arm");
  Ros2GripperCommandBackend backend(resources, sink, clock, scheduler, false);
  const auto endpoint = "/manyforge_position_discovery_wait_" + unique_suffix();
  std::unique_ptr<ControlledPneumaticServer> server;
  TestTransportLifetime lifetime(*resources, backend);
  Ros2GripperActionConfig config;
  config.state_freshness_timeout_s = 1.0;
  config.action_name = endpoint;
  config.group_id = "gripper";
  config.joint_name = "finger_joint";
  EXPECT_DOUBLE_EQ(config.wait_for_action_server_timeout_s, 2.0);
  backend.configure_gripper_action("tool", config, 1'000'000'000);
  backend.complete_ros_configuration();
  lifetime.start();
  auto in_flight = std::async(std::launch::async, [&] {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.25, 0.0, "ros_gripper_command", "position_reached"));
  });
  EXPECT_EQ(in_flight.wait_for(100ms), std::future_status::timeout);
  server = std::make_unique<ControlledPneumaticServer>(*resources, endpoint);
  const auto dispatched = in_flight.get();
  ASSERT_TRUE(dispatched.accepted);
  ASSERT_TRUE(server->wait_for_goal());
  EXPECT_EQ(server->requests.load(), 1U);
}

} // namespace
} // namespace manyforge::ros2_control_execution
