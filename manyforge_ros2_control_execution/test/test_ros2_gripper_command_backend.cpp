// Copyright 2026 Flexin Group SRL
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
#include <future>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>
namespace manyforge::ros2_control_execution {
namespace {
TEST(Ros2GripperCommandBackendTest,
     GripperProjectionReadsStartPositionFromExecutionStateSink) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  Ros2GripperActionConfig gripper;
  gripper.action_name = "/manyforge_missing_gripper_" + unique_suffix();
  gripper.group_id = "gripper";
  gripper.joint_name = "finger_joint";
  gripper.wait_for_action_server_timeout_s = 0.0;
  backend.configure_gripper_action("tool", gripper);

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  const auto absent = backend.gripper_transport_readiness("missing");
  EXPECT_FALSE(absent.configured);
  EXPECT_FALSE(absent.ready);
  EXPECT_EQ(absent.reason_codes,
            std::vector<std::string>{"gripper_transport_not_configured"});

  Ros2GripperActionConfig gripper;
  gripper.action_name =
      "/manyforge_missing_gripper_readiness_" + unique_suffix();
  backend.configure_gripper_action("tool", gripper);
  const auto configured = backend.gripper_transport_readiness("tool");
  EXPECT_TRUE(configured.configured);
  EXPECT_EQ(configured.transport_kind, "ros_gripper_command");
  EXPECT_FALSE(configured.command_path_ready);
  EXPECT_FALSE(configured.feedback_path_ready);
  EXPECT_TRUE(configured.command_gate_open);
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  auto gripper =
      isaac_gripper_config("/manyforge_gripper_source_command_" + suffix);
  gripper.state_topic = state_topic;
  gripper.command_joint_names = {"right_finger_joint"};
  gripper.state_joint_name = "right_finger_joint";
  gripper.joint_name = "robot_1__right_finger_joint";
  backend.configure_gripper_action("tool", std::move(gripper));
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
     ClosedSimulatorGateDoesNotHideReadyGripperTransportOrAdmitCommand) {
  const std::string suffix = unique_suffix();
  const std::string command_topic =
      "/manyforge_gated_gripper_command_" + suffix;
  const std::string state_topic = "/manyforge_gated_gripper_state_" + suffix;

  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto config = base_config();
  config.command_gate_topic = "/manyforge_command_gate_" + suffix;
  auto resources = test_transport_resources(sink, clock, "arm", config);
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  auto gripper = isaac_gripper_config(command_topic);
  gripper.state_topic = state_topic;
  gripper.state_joint_name = "finger_joint";
  backend.configure_gripper_action("tool", std::move(gripper));
  backend.complete_ros_configuration();
  lifetime.start();

  JointStateSubscriber command_subscriber(command_topic);
  JointStatePublisher state_publisher(state_topic);
  ASSERT_TRUE(command_subscriber.wait_for_publisher());
  ASSERT_TRUE(state_publisher.wait_for_subscription());
  for (int attempt = 0; attempt < 5; ++attempt) {
    state_publisher.publish({"finger_joint"}, {0.7});
    std::this_thread::sleep_for(20ms);
  }

  const auto closed = backend.gripper_transport_readiness("tool");
  EXPECT_TRUE(closed.configured);
  EXPECT_TRUE(closed.command_path_ready);
  EXPECT_TRUE(closed.feedback_path_ready);
  EXPECT_FALSE(closed.command_gate_open);
  EXPECT_TRUE(closed.ready);
  EXPECT_TRUE(closed.reason_codes.empty());

  const auto rejected = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  EXPECT_FALSE(rejected.accepted);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code,
            ros2_control_fault_reason::kGripperGoalRejected);

  EXPECT_TRUE(resources->request_command_gate(true));
  const auto open = backend.gripper_transport_readiness("tool");
  EXPECT_TRUE(open.command_gate_open);
  EXPECT_TRUE(open.ready);
  EXPECT_TRUE(open.reason_codes.empty());
}

TEST(Ros2GripperCommandBackendTest,
     GripperPreflightRejectsCrossedSemanticsWithoutAdmittingAnEffect) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_preflight_gripper_" + unique_suffix()));

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_resolved_gripper_" + unique_suffix()));

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_state_gripper_" +
                                   unique_suffix()));
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  auto gripper = isaac_gripper_config("/manyforge_non_finite_start_gripper_" +
                                      unique_suffix());
  backend.configure_gripper_action("tool", gripper);

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);

  Ros2GripperActionConfig gripper;
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
  backend.configure_gripper_action("tool", gripper);

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
  ASSERT_TRUE(resources->projection_worker().drain_for(1s));
  const auto reset = backend.get_gripper_status("tool");
  EXPECT_EQ(reset.state, core::GripperCommandState::kCompleted);
  EXPECT_TRUE(reset.evidence.empty());
  EXPECT_TRUE(reset.evidence_state.empty());
  EXPECT_FALSE(reset.error.has_value());
}

TEST(Ros2GripperCommandBackendTest,
     StaleGripperGraspLossCannotFailReplacementGeneration) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_stale_grasp_loss_" + unique_suffix()));

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_schedule_failure_" + unique_suffix()));

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_autonomous_gripper_" + unique_suffix()));

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
  ASSERT_TRUE(resources->projection_worker().drain_for(1s));
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_reset_fenced_gripper_" +
                                   unique_suffix()));

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_projection_retry_gripper_" +
                                   unique_suffix()));

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
  ASSERT_TRUE(resources->projection_worker().drain_for(1s));
  status = backend.get_gripper_status("tool");
  EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(status.projection_state, core::GripperProjectionState::kApplied);
  ASSERT_EQ(sink->single_updates().size(), 3u);
  EXPECT_DOUBLE_EQ(sink->single_updates().back().position_rad, 0.0);
  const auto metrics = resources->projection_worker().metrics();
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_projection_race_gripper_" +
                                   unique_suffix()));

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
  ASSERT_TRUE(resources->projection_worker().drain_for(1s));
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_replaced_gripper_a_" + unique_suffix()));
  const auto command = backend.command_gripper(make_gripper_request(
      0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  ASSERT_TRUE(command.accepted);

  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_replaced_gripper_b_" + unique_suffix()));
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_prestart_gripper_" + unique_suffix()));

  backend.complete_ros_configuration();
  lifetime.start();

  EXPECT_THROW(backend.configure_gripper_action(
                   "late", isaac_gripper_config("/manyforge_late_gripper_" +
                                                unique_suffix())),
               std::logic_error);
  EXPECT_THROW(backend.complete_ros_configuration(), std::logic_error);
}

TEST(Ros2GripperCommandBackendTest,
     GripperConfigurationReplacementRejectsInFlightPreflight) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_gripper_a_" +
                                   unique_suffix()));

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_gripper_b_" +
                                   unique_suffix()));
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_preflight_invalidation_" +
                                   unique_suffix()));

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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool", isaac_gripper_config("/manyforge_independent_gripper_a_" +
                                   unique_suffix()));

  auto in_flight = std::async(std::launch::async, [&]() {
    return backend.command_gripper(make_gripper_request(
        0.0, 0.2, 0.0, "isaac_joint_state_effort", "dwell_elapsed"));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.configure_gripper_action(
      "auxiliary", isaac_gripper_config("/manyforge_independent_gripper_b_" +
                                        unique_suffix()));
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

TEST(Ros2GripperCommandBackendTest,
     SchedulerCancelFailurePreservesTheInvalidatedGenerationFence) {
  auto sink = std::make_shared<RecordingExecutionStateSink>();
  sink->set_joint_position("gripper", "finger_joint", 0.7);
  core::FakeClock clock;
  ThrowingCancelScheduler scheduler;
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, backend);
  backend.configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_throwing_cancel_" + unique_suffix()));
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  {
    Ros2GripperCommandBackend backend(*resources, sink, clock, scheduler,
                                      false);
    TestTransportLifetime lifetime(*resources, backend);
    backend.configure_gripper_action(
        "tool",
        isaac_gripper_config("/manyforge_late_callback_" + unique_suffix()));
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
  auto resources = test_transport_resources(sink, clock, "arm", base_config());
  auto backend = std::make_unique<Ros2GripperCommandBackend>(
      *resources, sink, clock, scheduler, false);
  TestTransportLifetime lifetime(*resources, *backend);
  backend->configure_gripper_action(
      "tool",
      isaac_gripper_config("/manyforge_dequeued_callback_" + unique_suffix()));
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

} // namespace
} // namespace manyforge::ros2_control_execution
