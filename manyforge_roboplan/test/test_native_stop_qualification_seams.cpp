#include "native_stop_qualification_fixture.hpp"
#include "native_stop_qualification_probe.hpp"

#include <gtest/gtest.h>

#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

using namespace std::chrono_literals;
namespace fixture = qualification_fixture;

class RecordingProbe final : public detail::INativeStopQualificationProbe {
public:
  explicit RecordingProbe(std::optional<detail::NativeStopQualificationEvent>
                              blocked = std::nullopt)
      : blocked_(blocked) {}

  void observe(detail::NativeStopQualificationEvent event,
               std::string_view detail_text) override {
    std::unique_lock lock(mutex_);
    events_.push_back({event, std::string(detail_text)});
    cv_.notify_all();
    if (blocked_.has_value() && *blocked_ == event) {
      cv_.wait(lock, [this] { return released_; });
    }
  }

  [[nodiscard]] bool wait_for(detail::NativeStopQualificationEvent event,
                              std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [&] { return first_index_locked(event).has_value(); });
  }

  [[nodiscard]] std::optional<std::size_t>
  first_index(detail::NativeStopQualificationEvent event) const {
    std::lock_guard lock(mutex_);
    return first_index_locked(event);
  }

  [[nodiscard]] std::string
  first_detail(detail::NativeStopQualificationEvent event) const {
    std::lock_guard lock(mutex_);
    const auto found =
        std::find_if(events_.begin(), events_.end(),
                     [event](const auto &row) { return row.first == event; });
    return found == events_.end() ? std::string{} : found->second;
  }

  void release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

private:
  [[nodiscard]] std::optional<std::size_t>
  first_index_locked(detail::NativeStopQualificationEvent event) const {
    for (std::size_t index = 0U; index < events_.size(); ++index) {
      if (events_[index].first == event) {
        return index;
      }
    }
    return std::nullopt;
  }

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::optional<detail::NativeStopQualificationEvent> blocked_;
  std::vector<std::pair<detail::NativeStopQualificationEvent, std::string>>
      events_;
  bool released_{};
};

[[nodiscard]] bool wait_until_executing(RoboPlanExecutionBackend &backend,
                                        double minimum_progress = 0.0) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = backend.get_execution_snapshot();
    if (snapshot.state == core::ExecutionState::kExecuting &&
        snapshot.progress >= minimum_progress) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

[[nodiscard]] core::ResolvedParticipantStopStageDTO direct_stage_fixture() {
  const core::OperationId operation_id("operation.direct");
  const core::OperationRevision operation_revision(1U);
  const core::OperationParticipantId participant_id("participant.direct");
  return core::ResolvedParticipantStopStageDTO{
      core::StopId("stop.direct"),
      core::StopGeneration(1U),
      operation_id,
      operation_revision,
      participant_id,
      core::ControllerGoalId("goal.direct"),
      core::ControllerGoalGeneration(1U),
      core::ControllableComponentId("robot.1"),
      core::CommandTargetId("arm.target"),
      core::CapabilityId("execution.trajectory.execute"),
      core::ControlDomainId("domain.arm"),
      core::ControlDomainRevision("domain-revision"),
      core::ControlBindingId("binding.arm"),
      core::ControlBindingRevision("binding-revision"),
      core::ActiveControlConfigurationId("active.native"),
      core::ActiveControlConfigurationGeneration(1U),
      core::ControllerTopologyId("topology.native"),
      core::TopologyRevision("topology-revision"),
      core::ControlRouteId("route.native"),
      std::nullopt,
      std::nullopt,
      core::RuntimeEpoch("runtime-epoch.1"),
      {},
      core::ContainmentGranularity::kResource,
      core::StopEvidencePolicyId("stop.native"),
      core::StopEvidencePolicyRevision("stop-revision"),
      core::PreparedParticipantStopPlanDTO{
          core::ContentDigest("plan.digest"),
          core::ResolvedStopBasePlanId("base.plan"),
          operation_id,
          operation_revision,
          participant_id,
          core::ProgramId("program.direct"),
          core::ProgramRevision("program.revision"),
          core::ProgramBindingId("program.binding"),
          core::ProgramRunId("program.run"),
          std::nullopt,
          core::ParticipantStopProfilePurposeDTO::kOrdinary,
          std::nullopt,
          core::StopDeadlineBudgetDTO{1'000'000'000, 1, {}},
          {}},
      0U,
      core::ContentDigest("scope.digest")};
}

TEST(NativeStopQualificationSeamTest,
     SoftwareHardStopDoesNotWaitForTheRealNominalAdapterMutex) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));

  auto nominal_adapter_lock =
      detail::NativeStopQualificationAccess::lock_nominal_adapter(*backend);
  auto nominal = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });
  const bool nominal_reached_adapter = probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalAdapterLockAttempt);
  if (!nominal_reached_adapter) {
    nominal_adapter_lock.unlock();
  }
  ASSERT_TRUE(nominal_reached_adapter);

  auto hard = std::async(std::launch::async, [&] { return backend->stop(); });
  const auto hard_status = hard.wait_for(100ms);
  nominal_adapter_lock.unlock();
  ASSERT_EQ(hard_status, std::future_status::ready);
  EXPECT_TRUE(hard.get().accepted);
  ASSERT_EQ(nominal.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(nominal.get().accepted);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     DirectStageCancellationReturnsExactSupersededNoEffectWithoutDispatch) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, RoboPlanExecutionBackendConfig{}, probe);
  std::stop_source cancellation;
  cancellation.request_stop();
  const auto stage = direct_stage_fixture();
  const core::RemainingStopBudgetDTO budget{
      stage.stop_id, stage.stop_generation, 1'000'000'000, {}};

  const auto result =
      backend->TranslateStopStage(stage, budget, cancellation.get_token());

  EXPECT_EQ(result.request.stop_id, stage.stop_id);
  EXPECT_EQ(result.attempt_disposition,
            core::StopStageAttemptDispositionDTO::kNotDispatched);
  EXPECT_EQ(result.outcome,
            core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "stop_stage_superseded_before_dispatch");
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kIdle);
}

TEST(NativeStopQualificationSeamTest,
     ControlledStopStartedAfterHardIssueCannotResurrectTheExecution) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, RoboPlanExecutionBackendConfig{}, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  const auto hard = backend->stop();
  if (!hard.accepted) {
    probe.release();
  }
  ASSERT_TRUE(hard.accepted);

  const auto controlled =
      backend->execute_controlled_stop(fixture::soft_profile(binding, 0.30));
  EXPECT_TRUE(controlled.accepted);
  EXPECT_FALSE(controlled.backend_stop_confirmed);
  ASSERT_TRUE(controlled.error.has_value());
  EXPECT_EQ(controlled.error->details.at("phase"), "controlled_stop_entry");
  EXPECT_FALSE(
      probe.first_index(detail::NativeStopQualificationEvent::kPhysicalDispatch)
          .has_value());
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     HardStopInControlledHandoffIsAcceptedAndPreventsReplacementRearm) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe(
      detail::NativeStopQualificationEvent::kNominalRearmAttempt);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  auto controlled = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });

  const bool reached_handoff = probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalRearmAttempt);
  if (!reached_handoff) {
    probe.release();
  }
  ASSERT_TRUE(reached_handoff);
  EXPECT_FALSE(backend->get_execution_snapshot().state ==
               core::ExecutionState::kExecuting);
  const auto hard = backend->stop();
  EXPECT_TRUE(hard.accepted);
  probe.release();

  ASSERT_EQ(controlled.wait_for(2s), std::future_status::ready);
  const auto result = controlled.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->details.at("phase"), "soft_on_path_before_rearm");
  EXPECT_NE(backend->get_execution_snapshot().state,
            core::ExecutionState::kExecuting);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

TEST(NativeStopQualificationSeamTest,
     CancelledNominalWaitDoesNotPublishStoppedBeforeTheWorkerCeases) {
  WallClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe(
      detail::NativeStopQualificationEvent::kWorkerStopObservedBeforeTerminal);
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 5.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  auto controlled = std::async(std::launch::async, [&] {
    return backend->execute_controlled_stop(
        fixture::soft_profile(binding, 0.30));
  });

  const bool worker_parked = probe.wait_for(
      detail::NativeStopQualificationEvent::kWorkerStopObservedBeforeTerminal);
  const bool caller_waiting = probe.wait_for(
      detail::NativeStopQualificationEvent::kNominalResultWaitEnter);
  if (!worker_parked || !caller_waiting) {
    probe.release();
  }
  ASSERT_TRUE(worker_parked);
  ASSERT_TRUE(caller_waiting);
  const auto hard = backend->stop();
  if (!hard.accepted) {
    probe.release();
  }
  ASSERT_TRUE(hard.accepted);
  const auto nominal_status = controlled.wait_for(100ms);
  if (nominal_status != std::future_status::ready) {
    probe.release();
  }
  ASSERT_EQ(nominal_status, std::future_status::ready);
  const auto result = controlled.get();
  EXPECT_TRUE(result.accepted);
  EXPECT_FALSE(result.backend_stop_confirmed);
  EXPECT_NE(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);

  probe.release();
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
  EXPECT_EQ(backend->get_execution_snapshot().state,
            core::ExecutionState::kStopped);
}

TEST(NativeStopQualificationSeamTest,
     RealLateSoftOnPathSelectsSpringBackBeforeDispatchWithoutScopeDrift) {
  core::FakeClock clock;
  auto binding = fixture::make_binding();
  auto channel = std::make_shared<fixture::StateChannel>(binding);
  RoboPlanExecutionBackendConfig config;
  config.soft_on_path_stop_traj_delta_s = 0.02;
  config.soft_on_path_sample_period_s = 0.01;
  config.soft_on_path_max_jerk_rad_s3 = {{"joint_1", 1'000'000.0},
                                         {"joint_2", 1'000'000.0}};
  RecordingProbe probe;
  auto backend = detail::NativeStopQualificationAccess::construct(
      binding, channel, clock, config, probe);
  const auto original_scope =
      detail::NativeStopQualificationAccess::scope(*backend);
  auto execution =
      backend->execute_trajectory(fixture::trajectory(binding, 1.0));
  ASSERT_TRUE(wait_until_executing(*backend));
  ASSERT_TRUE(probe.wait_for(
      detail::NativeStopQualificationEvent::kExecutionLoopArmed));
  clock.advance(950'000'000);
  ASSERT_TRUE(wait_until_executing(*backend, 0.94));

  const auto started = std::chrono::steady_clock::now();
  const auto controlled =
      backend->execute_controlled_stop(fixture::soft_profile(binding, 0.15));
  const auto elapsed = std::chrono::steady_clock::now() - started;

  ASSERT_TRUE(controlled.accepted);
  EXPECT_TRUE(controlled.backend_stop_confirmed);
  EXPECT_EQ(controlled.executed_stop_mode,
            core::MotionInterruptStopMode::kSpringBackHold);
  ASSERT_TRUE(controlled.fallback_reason.has_value());
  EXPECT_EQ(*controlled.fallback_reason, "soft_on_path_remaining_time_short");
  EXPECT_EQ(core::interrupt_structured_scalar_text(
                controlled.diagnostics.at("requested_stop_mode")),
            "soft_on_path");
  EXPECT_EQ(core::interrupt_structured_scalar_text(
                controlled.diagnostics.at("fallback_reason")),
            *controlled.fallback_reason);
  const auto selection = probe.first_index(
      detail::NativeStopQualificationEvent::kSubstitutionSelected);
  const auto dispatch = probe.first_index(
      detail::NativeStopQualificationEvent::kPhysicalDispatch);
  ASSERT_TRUE(selection.has_value());
  ASSERT_TRUE(dispatch.has_value());
  EXPECT_LT(*selection, *dispatch);
  EXPECT_EQ(probe.first_detail(
                detail::NativeStopQualificationEvent::kSubstitutionSelected),
            "soft_on_path_remaining_time_short");
  EXPECT_EQ(detail::NativeStopQualificationAccess::scope(*backend),
            original_scope);
  EXPECT_LT(elapsed, 500ms);
  EXPECT_TRUE(execution.wait_for_completion(2'000'000'000LL));
}

} // namespace
} // namespace manyforge::roboplan_backend
