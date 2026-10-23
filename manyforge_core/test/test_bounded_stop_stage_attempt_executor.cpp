#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <manyforge_core/bounded_stop_stage_attempt_executor.hpp>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace manyforge::core {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::shared_ptr<const ResolvedParticipantStopStageDTO> stage(
    std::string participant) {
  const auto operation_id = OperationId("operation.origin");
  const auto operation_revision = OperationRevision(1U);
  const auto participant_id = OperationParticipantId(std::move(participant));
  return std::make_shared<ResolvedParticipantStopStageDTO>(ResolvedParticipantStopStageDTO{
      StopId("stop.origin"),
      StopGeneration(1U),
      operation_id,
      operation_revision,
      participant_id,
      ControllerGoalId("goal.origin"),
      ControllerGoalGeneration(1U),
      ControllableComponentId("component.fixture"),
      CommandTargetId("target.fixture"),
      CapabilityId("capability.fixture"),
      ControlDomainId("domain.fixture"),
      ControlDomainRevision("domain.revision"),
      ControlBindingId("binding.fixture"),
      ControlBindingRevision("binding.revision"),
      ActiveControlConfigurationId("configuration.fixture"),
      ActiveControlConfigurationGeneration(1U),
      ControllerTopologyId("topology.fixture"),
      TopologyRevision("topology.revision"),
      ControlRouteId("route.fixture"),
      std::nullopt,
      std::nullopt,
      RuntimeEpoch("runtime.epoch"),
      {},
      ContainmentGranularity::kResource,
      StopEvidencePolicyId("policy.fixture"),
      StopEvidencePolicyRevision("policy.revision"),
      PreparedParticipantStopPlanDTO{ContentDigest("plan.digest"),
                                     ResolvedStopBasePlanId("base.plan"),
                                     operation_id,
                                     operation_revision,
                                     participant_id,
                                     ProgramId("program.fixture"),
                                     ProgramRevision("program.revision"),
                                     ProgramBindingId("program.binding"),
                                     ProgramRunId("program.run"),
                                     std::nullopt,
                                     ParticipantStopProfilePurposeDTO::kOrdinary,
                                     std::nullopt,
                                     {}},
      0U,
      ContentDigest("scope.digest")});
}

[[nodiscard]] std::shared_ptr<const RemainingStopBudgetDTO> budget() {
  return std::make_shared<RemainingStopBudgetDTO>(
      RemainingStopBudgetDTO{StopId("stop.origin"), StopGeneration(1U), 0, std::nullopt});
}

[[nodiscard]] ParticipantStopStageResultDTO completed(
    const ResolvedParticipantStopStageDTO& request) {
  return ParticipantStopStageResultDTO{request,
                                       StopStageAttemptDispositionDTO::kNotDispatched,
                                       ParticipantStopStageOutcomeDTO::kFailed,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       std::nullopt,
                                       0,
                                       {},
                                       ErrorDTO{"fixture_failure", "fixture terminal result", {}}};
}

class IsolatedPort final : public IControlDomainExecutionPort {
 public:
  ParticipantStopStageResultDTO TranslateStopStage(const ResolvedParticipantStopStageDTO& request,
                                                   const RemainingStopBudgetDTO&,
                                                   std::stop_token cancellation) override {
    if (request.participant_id.value() == "blocked") {
      std::unique_lock lock(mutex_);
      blocked_entered_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this] { return release_blocked_; });
    } else if (request.participant_id.value() == "cancelled") {
      std::unique_lock lock(mutex_);
      cancelled_entered_ = true;
      changed_.notify_all();
      while (!cancellation.stop_requested()) {
        changed_.wait_for(lock, 1ms);
      }
    }
    return completed(request);
  }

  [[nodiscard]] bool WaitBlocked() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [this] { return blocked_entered_; });
  }

  [[nodiscard]] bool WaitCancelled() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [this] { return cancelled_entered_; });
  }

  void ReleaseBlocked() {
    {
      std::lock_guard lock(mutex_);
      release_blocked_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool blocked_entered_{};
  bool cancelled_entered_{};
  bool release_blocked_{};
};

class CompletionReceiver final : public IStopStageAttemptCompletionReceiver {
 public:
  void StageAttemptCompleted(const StopStageAttemptHandle handle) noexcept override {
    std::lock_guard lock(mutex_);
    ++counts_[handle];
    changed_.notify_all();
  }

  [[nodiscard]] bool Wait(const StopStageAttemptHandle handle) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [&] { return counts_[handle] == 1U; });
  }

  [[nodiscard]] std::size_t Count(const StopStageAttemptHandle handle) {
    std::lock_guard lock(mutex_);
    return counts_[handle];
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::map<StopStageAttemptHandle, std::size_t> counts_;
};

[[nodiscard]] StopStageAttemptTakeResult WaitForResult(BoundedStopStageAttemptExecutor& executor,
                                                       const StopStageAttemptHandle handle) {
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline) {
    auto result = executor.TryTake(handle);
    if (result.status != StopStageAttemptTakeStatus::kPending) {
      return result;
    }
    std::this_thread::yield();
  }
  return {StopStageAttemptTakeStatus::kPending, std::nullopt};
}

TEST(BoundedStopStageAttemptExecutorTest,
     BlockedLaneDoesNotStarveDisjointCompletionOrCancellation) {
  IsolatedPort port;
  CompletionReceiver receiver;
  BoundedStopStageAttemptExecutor executor(8U, 1U, receiver);
  EXPECT_EQ(executor.capacity(), 8U);
  EXPECT_EQ(executor.prequalified_max_live_participants(), 1U);
  EXPECT_TRUE(executor.prequalified_ready());

  const auto blocked = executor.TryStart({&port, stage("blocked"), budget()});
  EXPECT_EQ(blocked.status, StopStageAttemptStartStatus::kStarted);
  EXPECT_TRUE(blocked.handle.has_value());
  EXPECT_TRUE(port.WaitBlocked());

  const auto fast = executor.TryStart({&port, stage("fast"), budget()});
  EXPECT_EQ(fast.status, StopStageAttemptStartStatus::kStarted);
  EXPECT_TRUE(fast.handle.has_value());
  EXPECT_TRUE(receiver.Wait(*fast.handle));
  EXPECT_EQ(receiver.Count(*fast.handle), 1U);
  auto fast_result = WaitForResult(executor, *fast.handle);
  EXPECT_EQ(fast_result.status, StopStageAttemptTakeStatus::kCompleted);
  EXPECT_EQ(fast_result.result->request.participant_id, OperationParticipantId("fast"));
  EXPECT_EQ(executor.Release(*fast.handle), StopStageAttemptReleaseStatus::kReleased);

  const auto cancelled = executor.TryStart({&port, stage("cancelled"), budget()});
  EXPECT_EQ(cancelled.status, StopStageAttemptStartStatus::kStarted);
  EXPECT_TRUE(cancelled.handle.has_value());
  EXPECT_TRUE(port.WaitCancelled());
  EXPECT_TRUE(executor.RequestCancel(*cancelled.handle));
  auto cancelled_result = WaitForResult(executor, *cancelled.handle);
  EXPECT_EQ(cancelled_result.status, StopStageAttemptTakeStatus::kCompleted);
  EXPECT_EQ(executor.Release(*cancelled.handle), StopStageAttemptReleaseStatus::kReleased);

  executor.CloseIngress();
  EXPECT_FALSE(executor.prequalified_ready());
  EXPECT_FALSE(executor.DrainFor(10ms));
  port.ReleaseBlocked();
  auto blocked_result = WaitForResult(executor, *blocked.handle);
  EXPECT_EQ(blocked_result.status, StopStageAttemptTakeStatus::kCompleted);
  EXPECT_EQ(executor.Release(*blocked.handle), StopStageAttemptReleaseStatus::kReleased);
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedStopStageAttemptExecutorTest,
     ResultTransfersOnceAndStaleGenerationCannotConsumeReusedLane) {
  IsolatedPort port;
  CompletionReceiver receiver;
  BoundedStopStageAttemptExecutor executor(8U, 1U, receiver);
  const auto first = executor.TryStart({&port, stage("first"), budget()});
  EXPECT_TRUE(first.handle.has_value());
  auto first_result = WaitForResult(executor, *first.handle);
  EXPECT_EQ(first_result.status, StopStageAttemptTakeStatus::kCompleted);
  EXPECT_EQ(executor.TryTake(*first.handle).status, StopStageAttemptTakeStatus::kAlreadyTaken);
  EXPECT_EQ(executor.Release(*first.handle), StopStageAttemptReleaseStatus::kReleased);

  const auto second = executor.TryStart({&port, stage("second"), budget()});
  EXPECT_TRUE(second.handle.has_value());
  EXPECT_EQ(second.handle->slot, first.handle->slot);
  EXPECT_GT(second.handle->generation, first.handle->generation);
  EXPECT_EQ(executor.TryTake(*first.handle).status, StopStageAttemptTakeStatus::kStaleHandle);
  EXPECT_FALSE(executor.RequestCancel(*first.handle));
  EXPECT_EQ(executor.Release(*first.handle), StopStageAttemptReleaseStatus::kStaleHandle);
  auto second_result = WaitForResult(executor, *second.handle);
  EXPECT_EQ(second_result.status, StopStageAttemptTakeStatus::kCompleted);
  EXPECT_EQ(executor.Release(*second.handle), StopStageAttemptReleaseStatus::kReleased);
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedStopStageAttemptExecutorTest, FixedCapacityExhaustionAndClosedIngressAreExplicit) {
  IsolatedPort port;
  CompletionReceiver receiver;
  BoundedStopStageAttemptExecutor executor(8U, 1U, receiver);
  std::vector<StopStageAttemptHandle> handles;
  handles.reserve(8U);
  for (std::size_t index = 0U; index < 8U; ++index) {
    const auto started = executor.TryStart(
        {&port, stage(index == 0U ? "blocked" : "p" + std::to_string(index)), budget()});
    EXPECT_EQ(started.status, StopStageAttemptStartStatus::kStarted);
    handles.push_back(*started.handle);
  }
  EXPECT_EQ(executor.TryStart({&port, stage("overflow"), budget()}).status,
            StopStageAttemptStartStatus::kCapacityExhausted);
  executor.CloseIngress();
  EXPECT_EQ(executor.TryStart({&port, stage("closed"), budget()}).status,
            StopStageAttemptStartStatus::kClosed);
  port.ReleaseBlocked();
  for (const auto handle : handles) {
    auto result = WaitForResult(executor, handle);
    EXPECT_EQ(result.status, StopStageAttemptTakeStatus::kCompleted);
    EXPECT_EQ(executor.Release(handle), StopStageAttemptReleaseStatus::kReleased);
  }
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedStopStageAttemptExecutorTest,
     ConstructionRejectsCapacityThatDoesNotCoverParticipantTimesEight) {
  CompletionReceiver receiver;
  EXPECT_THROW(BoundedStopStageAttemptExecutor(7U, 1U, receiver), std::invalid_argument);
  EXPECT_THROW(BoundedStopStageAttemptExecutor(16U, 1U, receiver), std::invalid_argument);
}

class SpringPointPort final : public IControlDomainExecutionPort {
 public:
  ParticipantStopStageResultDTO TranslateStopStage(
      const ResolvedParticipantStopStageDTO& request, const RemainingStopBudgetDTO&,
      std::stop_token) override {
    return completed(request);
  }
  ParticipantStopStageResultDTO TranslateStopStageForTakeover(
      const ResolvedParticipantStopStageDTO& request, const RemainingStopBudgetDTO&,
      std::stop_token cancellation, IStopStageSpringReturnObserver& observer) override {
    SpringBackReturnPointDTO point;
    point.source_goal = {ControllerGoalId("source.goal"), ControllerGoalGeneration(7U)};
    point.binding_epoch = ExecutionBindingEpoch(11U);
    point.trip.source_stamp_ns = 100;
    point.initial.command_path_time_s = 2.5;
    point.initial.path_time_rate = 0.4;
    point.initial.runtime_velocity_scale_at_trip = 0.4;
    point.initial.joint_names = {"joint"};
    point.initial.spring_back_return_point = {0.0, {1.0}, {0.0}, {0.0}};
    const bool first = observer.SpringReturnPointReady(point);
    point.initial.spring_back_return_point.positions_rad.front() = 99.0;
    const bool duplicate = observer.SpringReturnPointReady(point);
    {
      std::unique_lock lock(mutex_);
      first_accepted_ = first;
      duplicate_accepted_ = duplicate;
      entered_ = true;
      changed_.notify_all();
      while (!cancellation.stop_requested()) changed_.wait_for(lock, 1ms);
    }
    return completed(request);
  }
  [[nodiscard]] bool WaitEntered() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 1s, [&] { return entered_; });
  }
  [[nodiscard]] bool FirstAccepted() {
    std::lock_guard lock(mutex_);
    return first_accepted_;
  }
  [[nodiscard]] bool DuplicateAccepted() {
    std::lock_guard lock(mutex_);
    return duplicate_accepted_;
  }
 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_{};
  bool first_accepted_{};
  bool duplicate_accepted_{};
};

TEST(BoundedStopStageAttemptExecutorTest,
     SpringPointIsImmutableProgressBeforeCompletionAndCannotCrossLaneGeneration) {
  SpringPointPort port;
  CompletionReceiver receiver;
  BoundedStopStageAttemptExecutor executor(8U, 1U, receiver);
  const auto started = executor.TryStart({&port, stage("spring"), budget(), true});
  ASSERT_TRUE(started.handle);
  EXPECT_TRUE(port.WaitEntered());
  EXPECT_TRUE(port.FirstAccepted());
  EXPECT_FALSE(port.DuplicateAccepted());
  const auto point = executor.ReadSpringReturnPoint(*started.handle);
  EXPECT_TRUE(point);
  if (point) {
    EXPECT_EQ(point->source_goal.controller_goal_id, ControllerGoalId("source.goal"));
    EXPECT_EQ(point->source_goal.controller_goal_generation, ControllerGoalGeneration(7U));
    EXPECT_EQ(point->binding_epoch, ExecutionBindingEpoch(11U));
    EXPECT_EQ(point->initial.command_path_time_s, 2.5);
    EXPECT_EQ(point->initial.spring_back_return_point.positions_rad,
              std::vector<double>{1.0});
    EXPECT_EQ(executor.ReadSpringReturnPoint(*started.handle), point);
  }
  EXPECT_EQ(receiver.Count(*started.handle), 1U);
  EXPECT_EQ(executor.TryTake(*started.handle).status, StopStageAttemptTakeStatus::kPending);
  EXPECT_TRUE(executor.RequestCancel(*started.handle));
  EXPECT_FALSE(executor.ReadSpringReturnPoint(*started.handle));
  EXPECT_EQ(WaitForResult(executor, *started.handle).status,
            StopStageAttemptTakeStatus::kCompleted);
  EXPECT_EQ(executor.Release(*started.handle), StopStageAttemptReleaseStatus::kReleased);
  EXPECT_FALSE(executor.ReadSpringReturnPoint(*started.handle));

  IsolatedPort ordinary;
  const auto next = executor.TryStart({&ordinary, stage("ordinary"), budget()});
  ASSERT_TRUE(next.handle);
  EXPECT_EQ(next.handle->slot, started.handle->slot);
  EXPECT_GT(next.handle->generation, started.handle->generation);
  EXPECT_FALSE(executor.ReadSpringReturnPoint(*started.handle));
  EXPECT_FALSE(executor.ReadSpringReturnPoint(*next.handle));
  EXPECT_EQ(WaitForResult(executor, *next.handle).status,
            StopStageAttemptTakeStatus::kCompleted);
  EXPECT_EQ(executor.Release(*next.handle), StopStageAttemptReleaseStatus::kReleased);
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedStopStageAttemptExecutorTest,
     TakeoverWithoutProgressSupportKeepsOrdinaryContainmentWithoutInventingAnAnchor) {
  IsolatedPort port;
  CompletionReceiver receiver;
  BoundedStopStageAttemptExecutor executor(8U, 1U, receiver);
  const auto started = executor.TryStart({&port, stage("ordinary"), budget(), true});
  ASSERT_TRUE(started.handle);
  const auto result = WaitForResult(executor, *started.handle);
  EXPECT_EQ(result.status, StopStageAttemptTakeStatus::kCompleted);
  EXPECT_TRUE(result.result);
  EXPECT_FALSE(executor.ReadSpringReturnPoint(*started.handle));
  EXPECT_EQ(executor.Release(*started.handle), StopStageAttemptReleaseStatus::kReleased);
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}


// Pause at the worker's clock read, after TryStart has queued the request.
// Advancing time here proves age is captured on the worker, not at enqueue.
TEST(BoundedStopStageAttemptExecutorTest, CapturesIngressAgeAfterQueueWithoutChangingDeadlines) {
  class GatedClock final : public IClock {
   public:
    TimePoint now() const override {
      std::unique_lock lock(mutex);
      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] { return released; });
      return current;
    }
    bool WaitForRead() {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, 1s, [&] { return entered; });
    }
    void Release() {
      std::lock_guard lock(mutex);
      current = 9'075'000'000;
      released = true;
      changed.notify_all();
    }
    mutable std::mutex mutex;
    mutable std::condition_variable changed;
    mutable bool entered{};
    bool released{};
    TimePoint current{9'000'000'000};
  } clock;
  class CapturePort final : public IControlDomainExecutionPort {
   public:
    ParticipantStopStageResultDTO TranslateStopStage(
        const ResolvedParticipantStopStageDTO& request, const RemainingStopBudgetDTO& timing,
        std::stop_token) override {
      received = timing;
      return completed(request);
    }
    std::optional<RemainingStopBudgetDTO> received;
  } port;
  CompletionReceiver receiver;
  BoundedStopStageAttemptExecutor executor(8U, 1U, receiver);
  struct FinishExecutor {
    GatedClock& clock;
    BoundedStopStageAttemptExecutor& executor;
    ~FinishExecutor() {
      clock.Release();
      executor.CloseIngress();
      EXPECT_TRUE(executor.DrainFor(1s));
    }
  } guard{clock, executor};
  auto timing = std::make_shared<RemainingStopBudgetDTO>(
      RemainingStopBudgetDTO{StopId("stop.origin"), StopGeneration(1U), 40'000'000, 800'000'000});
  const auto started = executor.TryStart(
      {&port, stage("capture.age"), timing, false, &clock, 9'000'000'000});
  ASSERT_EQ(started.status, StopStageAttemptStartStatus::kStarted);
  ASSERT_TRUE(started.handle);
  ASSERT_TRUE(clock.WaitForRead());
  clock.Release();
  const auto result = WaitForResult(executor, *started.handle);
  ASSERT_EQ(result.status, StopStageAttemptTakeStatus::kCompleted);
  ASSERT_TRUE(port.received);
  EXPECT_EQ(port.received->stop_ingress_age_ns, 75'000'000);
  EXPECT_EQ(port.received->elapsed_since_stop_ns, 40'000'000);
  EXPECT_EQ(port.received->remaining_controlled_deadline_ns, 800'000'000);
  EXPECT_FALSE(timing->stop_ingress_age_ns);
  EXPECT_EQ(timing->elapsed_since_stop_ns, 40'000'000);
  EXPECT_EQ(timing->remaining_controlled_deadline_ns, 800'000'000);
  EXPECT_EQ(executor.Release(*started.handle), StopStageAttemptReleaseStatus::kReleased);
}

}  // namespace
}  // namespace manyforge::core
