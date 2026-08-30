#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

namespace manyforge::roboplan_backend {

namespace detail {
class INativeStopQualificationProbe;
class NativeStopQualificationAccess;
enum class NativeStopQualificationEvent : std::uint8_t;
struct MotionIdleHeartbeatState;
} // namespace detail

struct RoboPlanExecutionBackendConfig {
  double reset_completion_timeout_s = 5.0;
  /// Retained simulated-controller update cadence. Production obtains this
  /// from the selected runtime-unit product; it is not a caller override.
  core::Duration execution_tick_period_ns = 10'000'000;
  /// Native-sim handoff budget for soft_on_path. This is deliberately
  /// small but non-zero so tests exercise the same wall-time contract
  /// as controller deployments.
  double soft_on_path_stop_traj_delta_s = 0.02;
  double soft_on_path_sample_period_s = 0.01;
  std::optional<double> soft_on_path_max_initial_path_deviation_rad;
  std::optional<double> soft_on_path_max_initial_velocity_deviation_rad_s;
  std::map<std::string, double> soft_on_path_max_jerk_rad_s3;
};

struct RoboPlanSceneProjectionMetrics {
  std::uint64_t misses_total = 0;
  std::uint64_t successful_updates_total = 0;
  std::uint64_t lag_max_ns = 0;
  std::int64_t last_success_timestamp_ns = 0;
  std::uint64_t last_success_age_ns = 0;
  std::uint64_t terminal_retries_scheduled_total = 0;
  std::uint64_t terminal_retry_successes_total = 0;
  std::uint64_t terminal_retry_exhausted_total = 0;
};

/// Simulated native execution backend for one exact all-angular participant.
/// Mature lifecycle/controlled-stop behavior is retained from the previous
/// native backend, while state publication and RoboPlan projection are routed
/// exclusively through the sealed addressed state channel.
class RoboPlanExecutionBackend final : public core::IExecutionBackend {
public:
  RoboPlanExecutionBackend(
      core::RuntimeParticipantBinding binding,
      std::shared_ptr<IRetainedObservedStateChannel> state_channel,
      core::IClock &clock, RoboPlanExecutionBackendConfig config = {});
  RoboPlanExecutionBackend(
      core::RuntimeParticipantBinding binding,
      std::shared_ptr<IRetainedObservedStateChannel> state_channel,
      core::IClock &clock, core::IScheduler &scheduler,
      RoboPlanExecutionBackendConfig config = {});
  ~RoboPlanExecutionBackend() override;

  core::PrepareParticipantResultDTO
  Prepare(const core::PreparedOperationParticipantDTO &participant) override;
  core::ExecutionHandle
  Dispatch(const core::PreparedOperationParticipantDTO &participant,
           const core::ControllerGoalId &controller_goal_id,
           core::ControllerGoalGeneration controller_goal_generation) override;
  core::ExecutionHandle DispatchResume(
      const core::PreparedOperationParticipantDTO &participant,
      const core::ControllerGoalId &controller_goal_id,
      core::ControllerGoalGeneration controller_goal_generation) override;

  core::ExecutionHandle
  execute_trajectory(const core::TrajectoryDTO &trajectory) override;
  core::Result<void> sync_observed_state_before_planning() override;
  core::StopResultDTO stop() override;
  core::PauseResultDTO pause() override;
  core::ExecutionHandle resume(const core::TrajectoryDTO &trajectory) override;
  core::ResetResultDTO reset() override;
  core::ExecutionSnapshot get_execution_snapshot() const override;
  const core::ObservedRobotStatePublication *
  observed_robot_state_publication() const noexcept override {
    const auto *publication =
        state_channel_ ? state_channel_->addressed_publication() : nullptr;
    return publication ? &publication->numeric_publication() : nullptr;
  }
  const core::ExecutionProgressPublication *
  execution_progress_publication() const noexcept override {
    return execution_progress_publication_.get();
  }
  void retire_execution_progress_generation(
      std::uint64_t execution_generation) noexcept override {
    if (execution_progress_publication_) {
      (void)execution_progress_publication_->invalidate_generation(
          execution_generation);
    }
  }
  core::ExecutionProgressHazardClaimResult claim_execution_progress_hazard(
      std::uint64_t execution_generation) noexcept override {
    return execution_progress_publication_
               ? execution_progress_publication_->claim_hazard_authority(
                     execution_generation)
               : core::ExecutionProgressHazardClaimResult::kGenerationMismatch;
  }
  // ADR-0018: atomic per-tick read of the runtime velocity scale so the UI
  // slider retimes in-flight discrete motion without stopping.
  void set_runtime_velocity_scale(double scale) override;
  core::RuntimeScaleMechanismStatusDTO
  runtime_velocity_scale_status() const override;

  [[nodiscard]] static constexpr double
  qualified_runtime_scale_slew_per_second() noexcept {
    return 4.0;
  }

  RoboPlanSceneProjectionMetrics scene_projection_metrics() const;

  /// Publish a verified route binding after the owning Kernel fence is held.
  /// No operation may be active while the physical backend identity changes.
  [[nodiscard]] core::Result<void> RebindRoute(
      core::RuntimeParticipantBinding binding,
      std::vector<core::RuntimeParticipantBinding> program_aliases = {});

  // Controlled-stop capability + result surface for motion-scoped
  // interrupt outcomes (spec/260 §5.6). The native simulator supports
  // `kSpringBackHold` and native `kSoftOnPath`. `kHardDeceleration`
  // remains a ros2_control/JTC-only mode in the current scope.
  core::MotionInterruptStopCapabilityDTO
  get_motion_interrupt_stop_capability() const override;
  [[nodiscard]] core::ParticipantStopStageResultDTO
  TranslateStopStage(const core::ResolvedParticipantStopStageDTO &stage,
                     const core::RemainingStopBudgetDTO &budget,
                     std::stop_token cancellation) override;
  core::ControlledStopResultDTO execute_controlled_stop(
      const core::MotionInterruptStopProfileDTO &profile) override;
  core::ControlledStopResultDTO
  execute_controlled_stop(const core::MotionInterruptStopProfileDTO &profile,
                          std::stop_token stop_token) override;

private:
  RoboPlanExecutionBackend(
      core::RuntimeParticipantBinding binding,
      std::shared_ptr<IRetainedObservedStateChannel> state_channel,
      core::IClock &clock, RoboPlanExecutionBackendConfig config,
      core::IScheduler *heartbeat_scheduler,
      detail::INativeStopQualificationProbe *qualification_probe);

  struct ExecutionStateData {
    core::ExecutionSnapshot snapshot;
    std::atomic<bool> running{false};
    std::atomic<bool> pause_requested{false};
    std::atomic<bool> natural_completion_stop_requested{false};
    std::shared_ptr<const core::TrajectoryDTO> active_trajectory;
    double active_trajectory_time_s = 0.0;
    double active_runtime_velocity_scale = 1.0;
  };

  enum class SceneUpdateMode {
    kBlocking,
    kBestEffort,
  };

  std::int64_t
  publish_snapshot(core::ExecutionState state, double progress,
                   const std::vector<double> &joint_positions,
                   std::optional<core::ErrorDTO> error = std::nullopt);
  RetainedStateProjectionResult
  publish_observed_state_authority(std::span<const double> positions,
                                   std::span<const double> velocities,
                                   std::int64_t timestamp_ns) noexcept;

  // Active execution must never block observed-state publication on the
  // retained projector's scene lock. During execution, call publish_snapshot()
  // first and project into RoboPlan with kBestEffort. The runtime collision
  // monitor reads the published snapshot as the authoritative robot state;
  // scene projection is for visualization/planning convergence and may skip
  // intermediate samples under contention.
  bool update_scene_positions(const std::vector<double> &joint_positions,
                              SceneUpdateMode mode = SceneUpdateMode::kBlocking,
                              std::int64_t source_timestamp_ns = 0);
  void execution_worker(std::stop_token st, core::TrajectoryDTO trajectory,
                        std::uint64_t execution_generation);
  void request_execution_worker_stop() noexcept;
  void publish_execution_inactive() noexcept;
  core::ControlledStopResultDTO execute_controlled_stop_impl(
      const core::MotionInterruptStopProfileDTO &profile,
      std::stop_token stop_token, std::uint64_t hard_stop_request_generation);
  void request_active_controlled_stop();
  void observe_stop_qualification(detail::NativeStopQualificationEvent event,
                                  std::string_view detail = {});
  void begin_controlled_stop_runtime_scale_freeze();
  bool end_controlled_stop_runtime_scale_freeze();
  void record_scene_projection_success(std::int64_t source_timestamp_ns);
  void record_scene_projection_miss();
  void
  schedule_terminal_scene_projection_retry(std::vector<double> joint_positions,
                                           std::int64_t source_timestamp_ns);
  void terminal_scene_projection_retry_worker(std::stop_token stop_token);
  void stop_terminal_scene_projection_retry_worker() noexcept;
  core::ExecutionHandle
  dispatch_prepared(const core::PreparedOperationParticipantDTO &participant,
                    const core::ControllerGoalId &controller_goal_id,
                    core::ControllerGoalGeneration controller_goal_generation,
                    bool resume_dispatch);

  struct PendingTerminalSceneProjection {
    std::vector<double> joint_positions;
    std::int64_t source_timestamp_ns = 0;
  };

  std::atomic<std::shared_ptr<const core::RuntimeParticipantBinding>> binding_;
  std::atomic<
      std::shared_ptr<const std::vector<core::RuntimeParticipantBinding>>>
      program_aliases_;
  std::shared_ptr<IRetainedObservedStateChannel> state_channel_;
  std::shared_ptr<detail::MotionIdleHeartbeatState> idle_heartbeat_;
  core::IClock &clock_;
  RoboPlanExecutionBackendConfig config_;
  detail::INativeStopQualificationProbe *qualification_probe_{};
  std::unique_ptr<core::ExecutionProgressPublication>
      execution_progress_publication_;
  std::atomic<std::uint64_t> next_execution_progress_generation_{1};
  // Exact currently-running native execution occurrence. Prepared goal
  // cancellation compares against this generation before it can affect the
  // worker, so an old goal can never stop a later execution.
  std::atomic<std::uint64_t> active_execution_generation_{0};
  std::vector<double> observed_at_rest_velocities_;

  mutable std::mutex execution_mutex_;
  mutable std::mutex state_mutex_;
  std::condition_variable execution_cv_;
  std::jthread worker_thread_;
  ExecutionStateData state_;
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                "native software-hard Stop requires a lock-free generation");
  static_assert(
      std::atomic<std::uint32_t>::is_always_lock_free,
      "native software-hard Stop requires lock-free activity observation");
  std::atomic<std::uint64_t> hard_stop_request_generation_{0U};
  std::atomic<std::uint64_t> active_execution_hard_stop_generation_{0U};
  std::atomic<std::uint32_t> controlled_stop_activity_count_{0U};

  // Runtime velocity scale read on every worker tick. std::atomic<double> is
  // lock-free on all supported targets; memory_order_release on the setter
  // pairs with memory_order_acquire on the worker's per-tick read.
  std::atomic<double> runtime_velocity_scale_{1.0};
  mutable std::mutex runtime_velocity_scale_mutex_;
  int controlled_stop_runtime_scale_freeze_depth_ = 0;
  std::optional<double> deferred_runtime_velocity_scale_;
  std::atomic<std::uint64_t> scene_projection_misses_total_{0};
  std::atomic<std::uint64_t> scene_projection_successes_total_{0};
  std::atomic<std::uint64_t> scene_projection_lag_max_ns_{0};
  std::atomic<std::int64_t> scene_projection_last_success_timestamp_ns_{0};
  std::atomic<std::uint64_t> terminal_scene_projection_retries_scheduled_total_{
      0};
  std::atomic<std::uint64_t> terminal_scene_projection_retry_successes_total_{
      0};
  std::atomic<std::uint64_t> terminal_scene_projection_retry_exhausted_total_{
      0};
  mutable std::mutex terminal_scene_projection_retry_mutex_;
  std::condition_variable_any terminal_scene_projection_retry_cv_;
  std::optional<PendingTerminalSceneProjection>
      pending_terminal_scene_projection_;

  struct PreparedGoalRecord final {
    core::ControllerGoalGeneration generation;
    core::ContentDigest prepared_contract_digest;
    // Retain only the Stop-visible projection of the prepared participant.
    // Goal id/generation identifies the request, while these fields prove the
    // requested Stop still belongs to the exact participant that was
    // dispatched.
    core::OperationId operation_id;
    core::OperationRevision operation_revision;
    core::OperationParticipantId participant_id;
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    core::CapabilityId capability_id;
    core::ControlDomainId control_domain_id;
    core::ControlDomainRevision control_domain_revision;
    core::ControlBindingId control_binding_id;
    core::ControlBindingRevision control_binding_revision;
    core::ControllerTopologyId controller_topology_id;
    core::TopologyRevision topology_revision;
    core::ControlRouteId control_route_id;
    core::ActiveControlConfigurationId active_control_configuration_id;
    core::ActiveControlConfigurationGeneration
        active_control_configuration_generation;
    core::RuntimeEpoch provider_runtime_epoch;
    std::vector<core::ResourceId> resources;
    core::StopEvidencePolicyId stop_evidence_policy_id;
    core::StopEvidencePolicyRevision stop_evidence_policy_revision;
    std::vector<core::PreparedParticipantStopPlanDTO> stop_plans;
    core::ExecutionHandle handle;
  };
  static constexpr std::size_t kPreparedReplayCapacity = 4096U;
  mutable std::mutex prepared_execution_mutex_;
  std::map<std::tuple<core::OperationId, core::OperationRevision,
                      core::OperationParticipantId>,
           core::ContentDigest>
      prepared_participants_;
  std::map<core::ControllerGoalId, PreparedGoalRecord> prepared_goals_;
  std::jthread terminal_scene_projection_retry_thread_;

  friend class detail::NativeStopQualificationAccess;
};

} // namespace manyforge::roboplan_backend
