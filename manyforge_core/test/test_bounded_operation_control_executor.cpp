#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <manyforge_core/bounded_operation_control_executor.hpp>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace manyforge::core {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] ErrorDTO unsupported() {
  return ErrorDTO{std::string(to_string(ErrorCode::kUnsupportedCapability)),
                  "fixture operation is unsupported",
                  {}};
}

class FakeEngine final : public IResolvedParticipantExecutionEngine,
                         public IResolvedPreparedParticipantExecutionEngine {
 public:
  Result<std::uint64_t> execute(const ExecuteRequestDTO&) override {
    return Result<std::uint64_t>::Err(unsupported());
  }
  PauseResultDTO pause() override {
    std::unique_lock lock(mutex_);
    ++pause_calls_;
    entered_ = true;
    changed_.notify_all();
    if (throw_pause_) {
      throw std::runtime_error("fixture pause failure");
    }
    changed_.wait(lock, [this] { return !block_pause_; });
    return {pause_accepted_, pause_accepted_ ? std::nullopt : std::optional(unsupported())};
  }
  ResumeResultDTO resume(std::optional<std::uint64_t>) override {
    std::lock_guard lock(mutex_);
    ++resume_calls_;
    return {true, std::nullopt};
  }
  ResetResultDTO reset(std::optional<std::uint64_t>) override { return {false, unsupported()}; }
  ResetResultDTO reset_after_certified_containment(std::uint64_t,
                                                   std::optional<std::uint64_t>) override {
    std::lock_guard lock(mutex_);
    ++reset_after_containment_calls_;
    return {true, std::nullopt};
  }
  AcknowledgeCompletedResultDTO acknowledge_completed(const std::uint64_t execution_id,
                                                      std::optional<std::uint64_t>) override {
    std::lock_guard lock(mutex_);
    ++acknowledge_calls_;
    acknowledged_execution_id_ = execution_id;
    return {true, std::nullopt};
  }
  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO&) override {
    return Result<ResolvedRuntimeScaleAcceptedDTO>::Err(unsupported());
  }
  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId&, const ControlBindingId&) const override {
    return Result<ResolvedRuntimeScaleStatusDTO>::Err(unsupported());
  }
  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO&) override { return {}; }
  Result<void> validate_gripper_command(const GripperCommandRequestDTO&) const override {
    return Result<void>::Err(unsupported());
  }
  GripperCommandStatusDTO get_gripper_status(const std::string&) const override { return {}; }
  Result<std::uint64_t> stop_gripper(const std::string&, std::uint64_t, std::uint64_t) override {
    return Result<std::uint64_t>::Err(unsupported());
  }
  Result<void> reset_gripper(const std::string& target, std::uint64_t command_id,
                             std::uint64_t generation) override {
    std::lock_guard lock(mutex_);
    ++reset_gripper_calls_;
    reset_gripper_target_ = target;
    reset_gripper_command_id_ = command_id;
    reset_gripper_generation_ = generation;
    return Result<void>::Ok();
  }
  ExecutionStatus get_status() const override { return {}; }
  SubscriptionHandle subscribe_events(EventCallback) override { return SubscriptionHandle{1U}; }
  void unsubscribe_events(SubscriptionHandle) override {}

  Result<std::uint64_t> PrepareMotion(ResolvedPreparedMotionExecutionRequestDTO) override {
    return Result<std::uint64_t>::Err(unsupported());
  }
  Result<ResolvedPreparedMotionExecutionSnapshotDTO> ObservePreparedMotion(
      std::uint64_t) const override {
    return Result<ResolvedPreparedMotionExecutionSnapshotDTO>::Err(unsupported());
  }
  Result<void> DispatchPreparedMotion(std::uint64_t, const ContentDigest&) override {
    return Result<void>::Err(unsupported());
  }
  Result<void> PrepareResumeMotion(std::uint64_t, ResolvedPreparedMotionResumeRequestDTO) override {
    std::unique_lock lock(mutex_);
    ++prepare_resume_calls_;
    prepare_resume_entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return !block_prepare_resume_; });
    return Result<void>::Ok();
  }
  Result<void> DispatchPreparedResumeMotion(std::uint64_t, const ContentDigest&) override {
    return Result<void>::Err(unsupported());
  }
  Result<void> CancelPreparedResumeMotion(std::uint64_t) override {
    std::lock_guard lock(mutex_);
    ++cancel_prepared_resume_calls_;
    return Result<void>::Ok();
  }
  Result<void> CancelPreparedMotion(std::uint64_t) override {
    return Result<void>::Err(unsupported());
  }
  Result<std::uint64_t> PrepareCapabilityCommand(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO) override {
    return Result<std::uint64_t>::Err(unsupported());
  }
  Result<ResolvedPreparedCapabilityCommandSnapshotDTO> ObservePreparedCapabilityCommand(
      std::uint64_t) const override {
    return Result<ResolvedPreparedCapabilityCommandSnapshotDTO>::Err(unsupported());
  }
  Result<GripperCommandResultDTO> DispatchPreparedCapabilityCommand(
      std::uint64_t, const ContentDigest&, const ControllerGoalId&,
      ControllerGoalGeneration) override {
    return Result<GripperCommandResultDTO>::Err(unsupported());
  }
  Result<void> CancelPreparedCapabilityCommand(std::uint64_t) override {
    return Result<void>::Err(unsupported());
  }

  void BlockPause() {
    std::lock_guard lock(mutex_);
    block_pause_ = true;
    entered_ = false;
  }
  void ReleasePause() {
    {
      std::lock_guard lock(mutex_);
      block_pause_ = false;
    }
    changed_.notify_all();
  }
  void ThrowPause() {
    std::lock_guard lock(mutex_);
    throw_pause_ = true;
  }
  void BlockPrepareResume() {
    std::lock_guard lock(mutex_);
    block_prepare_resume_ = true;
    prepare_resume_entered_ = false;
  }
  void ReleasePrepareResume() {
    {
      std::lock_guard lock(mutex_);
      block_prepare_resume_ = false;
    }
    changed_.notify_all();
  }
  [[nodiscard]] bool WaitForPrepareResume() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [this] { return prepare_resume_entered_; });
  }
  [[nodiscard]] bool WaitForPause() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [this] { return entered_; });
  }
  [[nodiscard]] std::size_t pause_calls() const {
    std::lock_guard lock(mutex_);
    return pause_calls_;
  }
  [[nodiscard]] std::size_t resume_calls() const {
    std::lock_guard lock(mutex_);
    return resume_calls_;
  }
  [[nodiscard]] std::size_t acknowledge_calls() const {
    std::lock_guard lock(mutex_);
    return acknowledge_calls_;
  }
  [[nodiscard]] std::uint64_t acknowledged_execution_id() const {
    std::lock_guard lock(mutex_);
    return acknowledged_execution_id_;
  }
  [[nodiscard]] std::size_t reset_after_containment_calls() const {
    std::lock_guard lock(mutex_);
    return reset_after_containment_calls_;
  }
  [[nodiscard]] std::size_t reset_gripper_calls() const {
    std::lock_guard lock(mutex_);
    return reset_gripper_calls_;
  }
  [[nodiscard]] std::size_t prepare_resume_calls() const {
    std::lock_guard lock(mutex_);
    return prepare_resume_calls_;
  }
  [[nodiscard]] std::size_t cancel_prepared_resume_calls() const {
    std::lock_guard lock(mutex_);
    return cancel_prepared_resume_calls_;
  }
  [[nodiscard]] std::string reset_gripper_target() const {
    std::lock_guard lock(mutex_);
    return reset_gripper_target_;
  }
  [[nodiscard]] std::uint64_t reset_gripper_command_id() const {
    std::lock_guard lock(mutex_);
    return reset_gripper_command_id_;
  }
  [[nodiscard]] std::uint64_t reset_gripper_generation() const {
    std::lock_guard lock(mutex_);
    return reset_gripper_generation_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool block_pause_{};
  bool entered_{};
  bool throw_pause_{};
  bool block_prepare_resume_{};
  bool prepare_resume_entered_{};
  bool pause_accepted_{true};
  std::size_t pause_calls_{};
  std::size_t resume_calls_{};
  std::size_t acknowledge_calls_{};
  std::size_t reset_after_containment_calls_{};
  std::size_t reset_gripper_calls_{};
  std::size_t prepare_resume_calls_{};
  std::size_t cancel_prepared_resume_calls_{};
  std::string reset_gripper_target_;
  std::uint64_t reset_gripper_command_id_{};
  std::uint64_t reset_gripper_generation_{};
  std::uint64_t acknowledged_execution_id_{};
};

class Receiver final : public IOperationControlEffectCompletionReceiver {
 public:
  void OperationControlEffectCompleted(
      BoundedOperationControlExecutor& executor,
      const OperationControlEffectHandle handle) noexcept override {
    std::lock_guard lock(mutex_);
    completions_.emplace_back(&executor, handle);
    changed_.notify_all();
  }

  [[nodiscard]] bool Wait(BoundedOperationControlExecutor& executor,
                          const OperationControlEffectHandle handle) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [&] {
      return std::find(completions_.begin(), completions_.end(), std::pair{&executor, handle}) !=
             completions_.end();
    });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::pair<BoundedOperationControlExecutor*, OperationControlEffectHandle>>
      completions_;
};

[[nodiscard]] OperationControlEffectRequest request(
    FakeEngine& engine, const std::string& operation, const std::string& participant,
    const OperationControlKind control = OperationControlKind::kPause,
    const std::uint64_t subordinate_id = 0U) {
  return OperationControlEffectRequest{&engine,
                                       OperationId(operation),
                                       OperationParticipantId(participant),
                                       OperationRevision(2U),
                                       control,
                                       subordinate_id,
                                       OperationParticipantPayloadKind::kMotion,
                                       {},
                                       0U,
                                       nullptr,
                                       {},
                                       {}};
}

[[nodiscard]] OperationControlEffectTakeResult WaitForResult(
    BoundedOperationControlExecutor& executor, const OperationControlEffectHandle handle) {
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = executor.TryTake(handle);
    if (result.status != OperationControlEffectTakeStatus::kPending) {
      return result;
    }
    std::this_thread::yield();
  }
  return {OperationControlEffectTakeStatus::kPending, std::nullopt};
}

[[nodiscard]] OperationControlEffectRequest resume_request(
    FakeEngine& engine, std::shared_ptr<std::atomic_bool> cancellation_requested) {
  auto authority = std::make_shared<const ResolvedPreparedMotionResumeRequestDTO>(
      ResolvedPreparedMotionResumeRequestDTO{OperationId("operation.resume"),
                                             OperationRevision(2U),
                                             OperationParticipantId("participant.resume"),
                                             ControllerGoalId("goal.resume"),
                                             ControllerGoalGeneration(2U),
                                             {},
                                             {}});
  return OperationControlEffectRequest{&engine,
                                       authority->operation_id,
                                       authority->participant_id,
                                       authority->operation_revision,
                                       OperationControlKind::kResume,
                                       77U,
                                       OperationParticipantPayloadKind::kMotion,
                                       {},
                                       0U,
                                       &engine,
                                       std::move(authority),
                                       std::move(cancellation_requested)};
}

TEST(BoundedOperationControlExecutorTest, ConstructionRequiresCanonicalDistinctEngineLanes) {
  Receiver receiver;
  FakeEngine engine;
  EXPECT_THROW(BoundedOperationControlExecutor({}, receiver), std::invalid_argument);
  EXPECT_THROW(BoundedOperationControlExecutor({nullptr}, receiver), std::invalid_argument);
  EXPECT_THROW(BoundedOperationControlExecutor({&engine, &engine}, receiver),
               std::invalid_argument);
}

TEST(BoundedOperationControlExecutorTest, BlockedEngineDoesNotStarveDisjointEngine) {
  Receiver receiver;
  FakeEngine blocked_engine;
  FakeEngine fast_engine;
  blocked_engine.BlockPause();
  BoundedOperationControlExecutor executor({&blocked_engine, &fast_engine}, receiver);
  ASSERT_TRUE(executor.prequalified_ready());

  const auto blocked = executor.TryStart(request(blocked_engine, "operation.a", "participant.a"));
  ASSERT_EQ(blocked.status, OperationControlEffectStartStatus::kStarted);
  ASSERT_TRUE(blocked.handle.has_value());
  ASSERT_TRUE(blocked_engine.WaitForPause());

  const auto fast = executor.TryStart(request(fast_engine, "operation.b", "participant.b"));
  ASSERT_EQ(fast.status, OperationControlEffectStartStatus::kStarted);
  ASSERT_TRUE(fast.handle.has_value());
  ASSERT_TRUE(receiver.Wait(executor, *fast.handle));
  auto fast_result = WaitForResult(executor, *fast.handle);
  ASSERT_EQ(fast_result.status, OperationControlEffectTakeStatus::kCompleted);
  ASSERT_TRUE(fast_result.result.has_value());
  EXPECT_TRUE(fast_result.result->accepted);
  EXPECT_EQ(executor.Release(*fast.handle), OperationControlEffectReleaseStatus::kReleased);

  executor.CloseIngress();
  EXPECT_FALSE(executor.DrainFor(10ms));
  blocked_engine.ReleasePause();
  ASSERT_TRUE(receiver.Wait(executor, *blocked.handle));
  EXPECT_EQ(WaitForResult(executor, *blocked.handle).status,
            OperationControlEffectTakeStatus::kCompleted);
  EXPECT_EQ(executor.Release(*blocked.handle), OperationControlEffectReleaseStatus::kReleased);
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedOperationControlExecutorTest, OneEngineLaneIsBusyUntilResultIsTakenAndReleased) {
  Receiver receiver;
  FakeEngine engine;
  engine.BlockPause();
  BoundedOperationControlExecutor executor({&engine}, receiver);
  const auto first = executor.TryStart(request(engine, "operation.a", "participant.a"));
  ASSERT_TRUE(first.handle.has_value());
  ASSERT_TRUE(engine.WaitForPause());
  EXPECT_EQ(executor.TryStart(request(engine, "operation.b", "participant.b")).status,
            OperationControlEffectStartStatus::kLaneBusy);
  engine.ReleasePause();
  ASSERT_TRUE(receiver.Wait(executor, *first.handle));
  EXPECT_EQ(WaitForResult(executor, *first.handle).status,
            OperationControlEffectTakeStatus::kCompleted);
  EXPECT_EQ(executor.Release(*first.handle), OperationControlEffectReleaseStatus::kReleased);

  const auto second = executor.TryStart(request(engine, "operation.b", "participant.b",
                                                OperationControlKind::kAcknowledgeCompleted, 77U));
  ASSERT_TRUE(second.handle.has_value());
  EXPECT_EQ(second.handle->slot, first.handle->slot);
  EXPECT_GT(second.handle->generation, first.handle->generation);
  EXPECT_EQ(executor.TryTake(*first.handle).status, OperationControlEffectTakeStatus::kStaleHandle);
  auto second_result = WaitForResult(executor, *second.handle);
  ASSERT_EQ(second_result.status, OperationControlEffectTakeStatus::kCompleted);
  EXPECT_EQ(engine.acknowledge_calls(), 1U);
  EXPECT_EQ(engine.acknowledged_execution_id(), 77U);
  EXPECT_EQ(executor.Release(*second.handle), OperationControlEffectReleaseStatus::kReleased);
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedOperationControlExecutorTest, ThrowingEffectRetainsExactFailureIdentity) {
  Receiver receiver;
  FakeEngine engine;
  engine.ThrowPause();
  BoundedOperationControlExecutor executor({&engine}, receiver);
  const auto started = executor.TryStart(request(engine, "operation.throw", "participant.throw"));
  ASSERT_TRUE(started.handle.has_value());
  ASSERT_TRUE(receiver.Wait(executor, *started.handle));
  auto result = WaitForResult(executor, *started.handle);
  ASSERT_EQ(result.status, OperationControlEffectTakeStatus::kInfrastructureFailure);
  ASSERT_TRUE(result.result.has_value());
  EXPECT_EQ(result.result->operation_id, OperationId("operation.throw"));
  EXPECT_EQ(result.result->participant_id, OperationParticipantId("participant.throw"));
  EXPECT_FALSE(result.result->accepted);
  ASSERT_TRUE(result.result->error.has_value());
  EXPECT_EQ(result.result->error->details.at("reason"), "operation_control_effect_threw");
  EXPECT_EQ(executor.Release(*started.handle), OperationControlEffectReleaseStatus::kReleased);
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedOperationControlExecutorTest,
     ResumeUsesPreparedAuthorityAndCancelsWhenStopWinsDuringPreparation) {
  Receiver receiver;
  FakeEngine engine;
  engine.BlockPrepareResume();
  BoundedOperationControlExecutor executor({&engine}, receiver);
  auto cancellation_requested = std::make_shared<std::atomic_bool>(false);
  const auto started = executor.TryStart(resume_request(engine, cancellation_requested));
  ASSERT_TRUE(started.handle.has_value());
  ASSERT_TRUE(engine.WaitForPrepareResume());
  cancellation_requested->store(true, std::memory_order_release);
  engine.ReleasePrepareResume();
  ASSERT_TRUE(receiver.Wait(executor, *started.handle));
  const auto result = WaitForResult(executor, *started.handle);
  ASSERT_EQ(result.status, OperationControlEffectTakeStatus::kCompleted);
  ASSERT_TRUE(result.result.has_value());
  EXPECT_FALSE(result.result->accepted);
  EXPECT_TRUE(result.result->prepared_resume_created);
  EXPECT_TRUE(result.result->prepared_resume_cleanup_complete);
  ASSERT_TRUE(result.result->error.has_value());
  EXPECT_EQ(result.result->error->details.at("reason"),
            "prepared_resume_superseded_during_prepare");
  EXPECT_EQ(engine.prepare_resume_calls(), 1U);
  EXPECT_EQ(engine.cancel_prepared_resume_calls(), 1U);
  EXPECT_EQ(engine.resume_calls(), 0U);
  EXPECT_EQ(executor.Release(*started.handle), OperationControlEffectReleaseStatus::kReleased);
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedOperationControlExecutorTest,
     ResetUsesCertifiedMotionRetirementOrExactCapabilityCommandGeneration) {
  Receiver receiver;
  FakeEngine engine;
  BoundedOperationControlExecutor executor({&engine}, receiver);

  const auto motion = executor.TryStart(
      request(engine, "operation.motion", "participant.motion", OperationControlKind::kReset, 77U));
  ASSERT_TRUE(motion.handle.has_value());
  ASSERT_TRUE(receiver.Wait(executor, *motion.handle));
  const auto motion_result = WaitForResult(executor, *motion.handle);
  ASSERT_EQ(motion_result.status, OperationControlEffectTakeStatus::kCompleted);
  ASSERT_TRUE(motion_result.result.has_value());
  EXPECT_TRUE(motion_result.result->accepted);
  EXPECT_EQ(engine.reset_after_containment_calls(), 1U);
  EXPECT_EQ(executor.Release(*motion.handle), OperationControlEffectReleaseStatus::kReleased);

  const auto command = executor.TryStart(
      OperationControlEffectRequest{&engine,
                                    OperationId("operation.command"),
                                    OperationParticipantId("participant.command"),
                                    OperationRevision(4U),
                                    OperationControlKind::kReset,
                                    91U,
                                    OperationParticipantPayloadKind::kCapabilityCommand,
                                    "gripper.target",
                                    6U,
                                    nullptr,
                                    {},
                                    {}});
  ASSERT_TRUE(command.handle.has_value());
  ASSERT_TRUE(receiver.Wait(executor, *command.handle));
  const auto command_result = WaitForResult(executor, *command.handle);
  ASSERT_EQ(command_result.status, OperationControlEffectTakeStatus::kCompleted);
  ASSERT_TRUE(command_result.result.has_value());
  EXPECT_TRUE(command_result.result->accepted);
  EXPECT_EQ(engine.reset_gripper_calls(), 1U);
  EXPECT_EQ(engine.reset_gripper_target(), "gripper.target");
  EXPECT_EQ(engine.reset_gripper_command_id(), 91U);
  EXPECT_EQ(engine.reset_gripper_generation(), 6U);
  EXPECT_EQ(executor.Release(*command.handle), OperationControlEffectReleaseStatus::kReleased);

  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}

}  // namespace
}  // namespace manyforge::core
