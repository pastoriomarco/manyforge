#include <manyforge_roboplan/roboplan_gripper_command_backend.hpp>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

class RejectingScheduler final : public core::IScheduler {
 public:
  core::ScheduleHandle schedule(
      core::TimePoint, std::function<void()>,
      core::SchedulerPriority) override {
    throw std::runtime_error("injected schedule failure");
  }

  void cancel(core::ScheduleHandle) override {}
};

class InlineScheduler final : public core::IScheduler {
 public:
  core::ScheduleHandle schedule(
      core::TimePoint, std::function<void()> callback,
      core::SchedulerPriority) override {
    const auto handle = next_handle_++;
    callback();
    return handle;
  }

  void cancel(core::ScheduleHandle) override {}

 private:
  core::ScheduleHandle next_handle_ = 1;
};

class InlineThenThrowScheduler final : public core::IScheduler {
 public:
  core::ScheduleHandle schedule(
      core::TimePoint, std::function<void()> callback,
      core::SchedulerPriority) override {
    callback();
    throw std::runtime_error("injected post-callback registration failure");
  }

  void cancel(core::ScheduleHandle) override {}
};

class TestExecutionStateSink final : public core::IExecutionStateSink {
 public:
  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string&, const std::vector<double>&,
      core::ExecutionStateUpdateMode) override {
    return {core::ExecutionStateSinkStatus::kApplied};
  }

  std::optional<double> group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (block_read_) {
      read_entered_ = true;
      read_cv_.notify_all();
      read_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
        return release_read_;
      });
    }
    const auto it = positions_.find({group_id, joint_name});
    return it == positions_.end() ? std::nullopt
                                  : std::optional<double>(it->second);
  }

  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string& group_id, const std::string& joint_name,
      double position_rad, core::ExecutionStateUpdateMode) override {
    std::unique_lock<std::mutex> lock(mutex_);
    ++apply_calls_;
    if (block_update_) {
      update_entered_ = true;
      update_cv_.notify_all();
      update_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
        return release_update_;
      });
    }
    if (busy_updates_ > 0) {
      --busy_updates_;
      return {core::ExecutionStateSinkStatus::kBusy};
    }
    positions_[{group_id, joint_name}] = position_rad;
    return {core::ExecutionStateSinkStatus::kApplied};
  }

  void set_position(double value) {
    std::lock_guard<std::mutex> lock(mutex_);
    positions_[{"gripper", "finger_joint"}] = value;
  }

  double position() const {
    return *group_joint_position("gripper", "finger_joint");
  }

  void make_busy(int count) {
    std::lock_guard<std::mutex> lock(mutex_);
    busy_updates_ = count;
  }

  int apply_calls() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return apply_calls_;
  }

  void block_update() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_update_ = true;
    update_entered_ = false;
    release_update_ = false;
  }

  void block_read() {
    std::lock_guard<std::mutex> lock(mutex_);
    block_read_ = true;
    read_entered_ = false;
    release_read_ = false;
  }

  bool wait_for_read() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return read_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
      return read_entered_;
    });
  }

  void release_read() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_read_ = true;
      block_read_ = false;
    }
    read_cv_.notify_all();
  }

  bool wait_for_update() {
    std::unique_lock<std::mutex> lock(mutex_);
    return update_cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
      return update_entered_;
    });
  }

  void release_update() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_update_ = true;
      block_update_ = false;
    }
    update_cv_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::pair<std::string, std::string>, double> positions_;
  int busy_updates_ = 0;
  int apply_calls_ = 0;
  std::condition_variable update_cv_;
  bool block_update_ = false;
  bool update_entered_ = false;
  bool release_update_ = false;
  mutable std::condition_variable read_cv_;
  mutable bool block_read_ = false;
  mutable bool read_entered_ = false;
  mutable bool release_read_ = false;
};

core::GripperCommandRequestDTO request(double target, double duration_s) {
  core::GripperCommandRequestDTO request;
  request.gripper_id = "primary";
  request.group_id = "gripper";
  request.joint_name = "finger_joint";
  request.target_position = target;
  request.duration_s = duration_s;
  request.state_authority = "native_backend";
  request.completion_policy = "position_reached";
  request.transport_kind = "native_kinematic";
  request.evidence = {"native_backend_state"};
  return request;
}

TEST(RoboPlanGripperCommandBackendTest,
     ProgressesOnControllerLaneWithoutObserverReads) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto admitted = backend.command_gripper(request(1.0, 1.0));
  ASSERT_TRUE(admitted.accepted);
  EXPECT_EQ(scheduler.last_priority(),
            core::SchedulerPriority::ControllerCommand);

  const auto first = backend.get_gripper_status("primary");
  const auto repeated = backend.get_gripper_status("primary");
  EXPECT_EQ(first.revision, repeated.revision);
  EXPECT_DOUBLE_EQ(sink->position(), 0.0);

  clock.advance(500'000'000);
  scheduler.advance_to(clock.now());
  const auto halfway = backend.get_gripper_status("primary");
  EXPECT_EQ(halfway.state, core::GripperCommandState::kExecuting);
  EXPECT_NEAR(halfway.progress, 0.5, 1e-12);
  EXPECT_NEAR(sink->position(), 0.5, 1e-12);

  clock.advance(500'000'000);
  scheduler.advance_to(clock.now());
  const auto completed = backend.get_gripper_status("primary");
  EXPECT_EQ(completed.state, core::GripperCommandState::kCompleted);
  EXPECT_DOUBLE_EQ(completed.progress, 1.0);
  EXPECT_DOUBLE_EQ(sink->position(), 1.0);
}

TEST(RoboPlanGripperCommandBackendTest,
     ResetInvalidatesQueuedGenerationBeforeItCanProject) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto first = backend.command_gripper(request(1.0, 1.0));
  ASSERT_TRUE(first.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());

  const auto reset = backend.get_gripper_status("primary");
  EXPECT_EQ(reset.state, core::GripperCommandState::kIdle);
  EXPECT_NE(reset.generation, first.generation);
  EXPECT_DOUBLE_EQ(sink->position(), 0.0);
  EXPECT_EQ(sink->apply_calls(), 0);

  const auto blocked = backend.command_gripper(request(0.5, 0.0));
  ASSERT_FALSE(blocked.accepted);
  ASSERT_TRUE(blocked.error.has_value());
  EXPECT_EQ(blocked.error->code, "gripper_command_not_authorized");

  backend.authorize_gripper_commands();
  const auto second = backend.command_gripper(request(0.5, 0.0));
  ASSERT_TRUE(second.accepted);
  scheduler.advance_to(clock.now());
  EXPECT_NE(second.generation, first.generation);
  EXPECT_DOUBLE_EQ(sink->position(), 0.5);
}

TEST(RoboPlanGripperCommandBackendTest,
     TerminalProjectionRetriesAreBoundedAndLossIsVisible) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  sink->make_busy(10);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(
      sink, clock, scheduler, /*update_period_ns=*/1);

  const auto admitted = backend.command_gripper(request(1.0, 0.0));
  ASSERT_TRUE(admitted.accepted);
  for (int attempt = 0; attempt < 12; ++attempt) {
    clock.advance(1);
    scheduler.advance_to(clock.now());
  }

  const auto failed = backend.get_gripper_status("primary");
  EXPECT_EQ(failed.state, core::GripperCommandState::kFailed);
  EXPECT_EQ(failed.projection_state, core::GripperProjectionState::kFailed);
  ASSERT_TRUE(failed.error.has_value());
  EXPECT_EQ(failed.error->code, "gripper_projection_failed");
  EXPECT_EQ(sink->apply_calls(), 10);
}

TEST(RoboPlanGripperCommandBackendTest,
     ResetDuringPreflightCannotResurrectCommand) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  sink->block_read();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  auto command = std::async(std::launch::async, [&]() {
    return backend.command_gripper(request(1.0, 1.0));
  });
  ASSERT_TRUE(sink->wait_for_read());
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  sink->release_read();

  const auto result = command.get();
  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "gripper_command_not_authorized");
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_DOUBLE_EQ(sink->position(), 0.0);
}

TEST(RoboPlanGripperCommandBackendTest,
     RejectsNonFiniteStartPositionBeforeScheduling) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(std::numeric_limits<double>::quiet_NaN());
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto result = backend.command_gripper(request(1.0, 1.0));

  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "gripper_state_not_finite");
  EXPECT_EQ(sink->apply_calls(), 0);
  EXPECT_EQ(backend.get_gripper_status("primary").state,
            core::GripperCommandState::kIdle);
}

TEST(RoboPlanGripperCommandBackendTest,
     InvalidationLinearizesAgainstDequeuedProjection) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  sink->block_update();
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto admitted = backend.command_gripper(request(1.0, 0.0));
  ASSERT_TRUE(admitted.accepted);
  auto callback = std::async(std::launch::async, [&]() {
    scheduler.advance_to(clock.now());
  });
  ASSERT_TRUE(sink->wait_for_update());

  auto invalidate = std::async(std::launch::async, [&]() {
    backend.invalidate_gripper_commands(
        core::GripperInvalidationReason::kReset);
  });
  EXPECT_EQ(invalidate.wait_for(std::chrono::milliseconds(20)),
            std::future_status::timeout);

  sink->release_update();
  callback.get();
  invalidate.get();
  const auto calls_after_invalidation = sink->apply_calls();
  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(sink->apply_calls(), calls_after_invalidation);
  EXPECT_EQ(backend.get_gripper_status("primary").state,
            core::GripperCommandState::kIdle);
}

TEST(RoboPlanGripperCommandBackendTest,
     GraspLossInvalidatesCommandGenerationAndEvidence) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto admitted = backend.command_gripper(request(1.0, 1.0));
  ASSERT_TRUE(admitted.accepted);
  const auto lost = backend.mark_gripper_grasp_lost(
      "primary", admitted.command_id, admitted.generation, "pressure_switch",
      "Pressure was lost",
      "grasp_confirmation_lost");
  ASSERT_TRUE(lost.accepted);
  EXPECT_NE(lost.generation, admitted.generation);

  clock.advance(1'000'000'000);
  scheduler.advance_to(clock.now());
  const auto status = backend.get_gripper_status("primary");
  EXPECT_EQ(status.state, core::GripperCommandState::kFailed);
  EXPECT_EQ(status.evidence_state, "pressure_or_part_lost");
  EXPECT_EQ(status.evidence.back(), "pressure_switch");
  EXPECT_DOUBLE_EQ(sink->position(), 0.0);
}

TEST(RoboPlanGripperCommandBackendTest,
     StaleGraspLossCannotFailReplacementGeneration) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto old_command = backend.command_gripper(request(1.0, 1.0));
  ASSERT_TRUE(old_command.accepted);
  backend.invalidate_gripper_commands(core::GripperInvalidationReason::kReset);
  backend.authorize_gripper_commands();
  const auto replacement = backend.command_gripper(request(0.5, 1.0));
  ASSERT_TRUE(replacement.accepted);

  const auto stale = backend.mark_gripper_grasp_lost(
      "primary", old_command.command_id, old_command.generation,
      "pressure_switch", "Delayed pressure loss",
      "grasp_confirmation_lost");
  ASSERT_FALSE(stale.accepted);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "stale_gripper_state");

  const auto status = backend.get_gripper_status("primary");
  EXPECT_EQ(status.command_id, replacement.command_id);
  EXPECT_EQ(status.generation, replacement.generation);
  EXPECT_EQ(status.state, core::GripperCommandState::kExecuting);
  EXPECT_FALSE(status.error.has_value());
}

TEST(RoboPlanGripperCommandBackendTest,
     SchedulerFailureRejectsAdmissionAndPublishesTerminalFailure) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  core::FakeClock clock;
  RejectingScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto result = backend.command_gripper(request(1.0, 1.0));
  ASSERT_FALSE(result.accepted);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "gripper_request_failed");

  const auto status = backend.get_gripper_status("primary");
  EXPECT_EQ(status.state, core::GripperCommandState::kFailed);
  ASSERT_TRUE(status.error.has_value());
  EXPECT_EQ(status.error->code, "gripper_request_failed");
  EXPECT_DOUBLE_EQ(sink->position(), 0.0);
}

TEST(RoboPlanGripperCommandBackendTest,
     InlineCallbackRetryCannotDivergeFromCommandAdmission) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  sink->make_busy(1);
  core::FakeClock clock;
  InlineScheduler scheduler;
  RoboPlanGripperCommandBackend backend(
      sink, clock, scheduler, /*update_period_ns=*/1);

  const auto result = backend.command_gripper(request(1.0, 0.0));

  ASSERT_TRUE(result.accepted)
      << (result.error ? result.error->message : "unexpected rejection");
  const auto status = backend.get_gripper_status("primary");
  EXPECT_EQ(status.state, core::GripperCommandState::kCompleted);
  EXPECT_EQ(status.projection_state, core::GripperProjectionState::kApplied);
  EXPECT_EQ(sink->apply_calls(), 2);
  EXPECT_DOUBLE_EQ(sink->position(), 1.0);
}

TEST(RoboPlanGripperCommandBackendTest,
     PostCallbackSchedulerFailureCannotReportSideEffectingRejection) {
  auto sink = std::make_shared<TestExecutionStateSink>();
  sink->set_position(0.0);
  core::FakeClock clock;
  InlineThenThrowScheduler scheduler;
  RoboPlanGripperCommandBackend backend(sink, clock, scheduler);

  const auto result = backend.command_gripper(request(1.0, 0.0));

  ASSERT_TRUE(result.accepted)
      << (result.error ? result.error->message : "unexpected rejection");
  EXPECT_EQ(backend.get_gripper_status("primary").state,
            core::GripperCommandState::kCompleted);
  EXPECT_DOUBLE_EQ(sink->position(), 1.0);
}

}  // namespace
}  // namespace manyforge::roboplan_backend
