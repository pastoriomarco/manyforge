#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <manyforge_core/execution_progress_publication.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/participant_stop_stage_helper.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/prepared_execution_membership.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

// ---------------------------------------------------------------------------
// Core robot participant over `IExecutionPort`.
//
// The ROS-free execution engine the participant Kernel (`ExecutionKernel`)
// drives through its existing `IExecutionBackend` seam on every lane. It owns
// the execution policy that today's native and ROS backends each implement:
// prepared membership and replay protection, dispatch (Submit with complete
// P/V/A in path time), the progress cursor from controller samples, the
// tracking-error policy, completion per state authority,
// controller-result mapping, the speed-factor ramp, timing
// observation and the arbitration of completion against a runtime
// collision hazard (through `ExecutionProgressPublication`). Stop stages are
// delegated to the one Core Stop-stage helper. Transport, goal UUIDs,
// callback retirement and observed-state publication stay with the adapter
// and the host.
//
// Binding: one port instance per binding epoch. The host constructs a new
// port on rebinding and hands it over with `RebindPort`; facts of different
// epochs are never combined.
// ---------------------------------------------------------------------------

/// `main`'s host tracking monitor: the controller-reported
/// position error compared against the tolerance; a breach lasting the timeout
/// ends the motion with `tracking_error_exceeded`. Isaac and real lanes only.
struct ExecutionTrackingPolicy final {
  double tolerance_rad{0.0};
  /// Zero disables the check, as `main`.
  double timeout_s{0.0};
};

/// Lane and deployment facts the participant needs. None has a participant
/// default: each comes from the lane configuration, the characterization or
/// the deployment.
struct ExecutionPortParticipantConfig final {
  /// Stop-stage helper inputs. `stop.trip_authority` is also the completion
  /// authority: kMeasured on real robots, kCommanded on Isaac and offline.
  StopStageHelperConfig stop;
  /// Measured authority only (`main`'s terminal state gate): the lane's
  /// arrival tolerance (also the controller's JTC `goal` constraint). The
  /// final measured position must lie within it of the trajectory end,
  /// observed in a sample received after the successful result within
  /// `completion_state_timeout_ns` (`main`'s `wait_for_state_timeout_s`);
  /// the check is `robot_at_target`.
  std::optional<double> final_tolerance_rad;
  Duration completion_state_timeout_ns{0};
  /// Measured authority only (`main`'s start-state check): before the
  /// goal is sent, a fresh complete measured sample (received within
  /// `progress_freshness_budget_ns`, `main`'s `joint_state_max_age_s`, waited
  /// for up to `completion_state_timeout_ns`) must lie within this tolerance
  /// of the trajectory's first waypoint. Commanded authority has no measured
  /// start check (as `main`'s commanded simulation).
  std::optional<double> start_tolerance_rad;
  /// Present on Isaac and real lanes, absent offline.
  std::optional<ExecutionTrackingPolicy> tracking;
  /// The per-lane controller period margin. A controller period
  /// beyond it is a critical fault: the Kernel's lane hard stop, then
  /// InterventionRequired with `late_control_cycle`. Required; no default
  /// exists until live qualification fixes it.
  double late_cycle_margin_s{0.0};
  /// The lane controller's configured initial speed factor (JTC
  /// `speed_scaling.initial_scaling_factor`): every binding epoch starts at it.
  double initial_speed_factor{1.0};
  /// Freshness budget of the published progress cursor: positive on measured
  /// lanes; 0 (non-expiring) is admitted only on commanded lanes.
  Duration progress_freshness_budget_ns{0};
  // Observation only: failures must never change dispatch or authorization.
  std::function<void(const ExecutionGoalKey&, const PreparedOperationParticipantDTO&,
                     ExecutionBindingEpoch)> observe_goal_context{};
  /// The separate apply-tool-context action (`ApplyToolContext`):
  /// run before a new goal is sent and before it counts as running, with no
  /// goal or Stop in flight (the robot at rest), never for a replacement. It
  /// runs without the participant lock; a Stop that starts meanwhile refuses
  /// the goal (`execution_port_stop_in_flight`). Active TCP: the lane
  /// sets the robot's Active TCP to the move's TCP entry,
  /// `prepared_motion_tcp_entry(participant)`: nothing when it already is,
  /// otherwise its confirmed TCP step, then the commit. An error refuses the
  /// motion before effect (`tool_context_not_applied`). Absent: nothing to
  /// apply. A moving replacement never reaches it: the Kernel refuses a
  /// preempt naming another TCP (`tool_context_change_during_motion`).
  std::function<Result<void>(const PreparedOperationParticipantDTO&)> apply_tool_context{};
};

/// Host-owned state authority: the addressed observed-state publication the
/// adapter writes, and the pre-planning synchronization gate over it.
struct ExecutionPortParticipantStateAuthority final {
  const ObservedRobotStatePublication* publication{};
  std::function<Result<void>()> sync_before_planning;
};

/// Timing observation: controller lateness and telemetry loss are kept
/// apart. Counts are of `ReadTiming()` windows, not of controller cycles.
struct ExecutionPortTimingMetrics final {
  std::uint64_t windows{0U};
  std::uint64_t windows_late_beyond_margin{0U};
  double max_period_s{0.0};
  double max_state_stamp_gap_s{0.0};
  std::uint64_t received_samples{0U};
  /// Recognition delay of dispatched goals (dispatch to their
  /// first anchor, local clock). Stop goals report theirs in the Stop evidence.
  std::uint64_t goals_recognized{0U};
  Duration last_recognition_delay_ns{0};
  Duration max_recognition_delay_ns{0};

  friend bool operator==(const ExecutionPortTimingMetrics&,
                         const ExecutionPortTimingMetrics&) = default;
};

class ExecutionPortParticipant final : public IExecutionBackend {
 public:
  /// `waiter` blocks until a deadline, a cancellation, or earlier when the
  /// port may have new facts; the participant and its Stop-stage helper wait
  /// only through it. `scheduler` paces the speed ramp (one value per host
  /// tick at the controller's nominal rate).
  ExecutionPortParticipant(RuntimeParticipantBinding binding,
                           std::shared_ptr<IExecutionPort> port, const IClock& clock,
                           IScheduler& scheduler, StopStageWaiter waiter,
                           ExecutionPortParticipantStateAuthority state_authority,
                           ExecutionPortParticipantConfig config);
  ~ExecutionPortParticipant() override;
  [[nodiscard]] Result<void> DetachSchedulerForHostShutdown(
      std::chrono::steady_clock::time_point deadline);

  ExecutionPortParticipant(const ExecutionPortParticipant&) = delete;
  ExecutionPortParticipant& operator=(const ExecutionPortParticipant&) = delete;

  /// A new port instance for a new binding epoch, constructed by the host.
  /// Accepted only while no motion or Stop is in flight; the previous port and
  /// every fact read from it are dropped.
  [[nodiscard]] Result<void> RebindPort(std::shared_ptr<IExecutionPort> port);

  /// Rebind at the validated fault-owner commit. Observations already begun
  /// before this boundary cannot qualify the retired goal's recovery.
  [[nodiscard]] Result<void> RebindPortAfterOwnerRebuild(std::shared_ptr<IExecutionPort> port);

  /// Staged fault-owner rebind. The host retains the locked stage through all
  /// companion owner swaps. Retire the exact old timer only after the host has
  /// verified and closed that failed owner; no scheduler work occurs during
  /// the combined owner publication.
  struct OwnerRebindStage;
  [[nodiscard]] Result<std::shared_ptr<OwnerRebindStage>> StageOwnerRebind(
      std::shared_ptr<IExecutionPort> port);
  [[nodiscard]] Result<void> RetireOwnerRebindTimer(const std::shared_ptr<OwnerRebindStage>& stage);
  [[nodiscard]] Result<void> LockOwnerRebind(const std::shared_ptr<OwnerRebindStage>& stage);
  /// Read the validated retained-motion fact only while the stage is locked.
  [[nodiscard]] bool LockedOwnerRebindNeedsMotionProof(
      const std::shared_ptr<OwnerRebindStage>& stage) const noexcept;
  void CommitLockedOwnerRebind(const std::shared_ptr<OwnerRebindStage>& stage) noexcept;
  void UnlockOwnerRebind(const std::shared_ptr<OwnerRebindStage>& stage) noexcept;
  std::optional<std::pair<std::uint64_t, std::uint64_t>>
  owner_rebuild_observation() const override;
  void record_owner_rebuild_rest(std::uint64_t execution_id) noexcept override;
  bool owner_rebuild_unknown_candidate(std::uint64_t source_id,
                                       std::uint64_t candidate_id) const override;

  /// Publish a verified route binding (and its Program-scoped aliases) while
  /// the owning Kernel fence is held; refused while motion or Stop is active.
  [[nodiscard]] Result<void> RebindRoute(RuntimeParticipantBinding binding,
                                         std::vector<RuntimeParticipantBinding> program_aliases);

  [[nodiscard]] ExecutionPortTimingMetrics timing_metrics() const;

  /// A fault of the bound mechanism observed by the host (the
  /// Isaac command-age guard trip). Motion in flight ends through the Kernel's
  /// critical-fault path (the lane hard stop, then InterventionRequired with
  /// `error`); without motion in flight nothing happens here (the host keeps
  /// motion gated until its Reset sequence).
  void ReportCriticalFault(ErrorDTO error);

  /// A goal of this participant (or a Stop stage of it) is still running.
  [[nodiscard]] bool GoalInFlight() const;

  // --- IExecutionBackend ----------------------------------------------------

  PrepareParticipantResultDTO Prepare(const PreparedOperationParticipantDTO& participant,
                                      std::uint64_t execution_id) override;
  Result<void> RetirePreparedExecution(std::uint64_t execution_id) override;
  Result<PreparedMotionReplacementSourceDTO> CaptureReplacementSource(
      std::uint64_t execution_id, std::uint64_t execution_generation, const ExecutionGoalKey& goal,
      const ContentDigest& digest) const override;
  Result<void> BeginReplacementPreparationScaleHold(
      const PreparedMotionReplacementSourceDTO& source, std::uint64_t candidate_id,
      std::int64_t preparation_deadline_ns, std::stop_token cancellation) override;
  Result<void> EndReplacementPreparationScaleHold(std::uint64_t candidate_id) override;
  PrepareParticipantResultDTO PrepareReplacement(
      const PreparedOperationParticipantDTO& participant, std::uint64_t candidate_id,
      const PreparedMotionReplacementSourceDTO& source) override;
  PreparedMotionReplacementDispatchDTO DispatchReplacement(
      const PreparedMotionReplacementSourceDTO& source, std::uint64_t candidate_id,
      const PreparedOperationParticipantDTO& participant, const ExecutionGoalKey& successor_goal,
      std::int64_t preparation_deadline_ns,
      const ReplacementSendRecheck& recheck_before_send = {}) override;
  Result<PreparedMotionReplacementObservationDTO> ObserveReplacement(
      const PreparedMotionReplacementSourceDTO& source, std::uint64_t candidate_id) const override;
  Result<void> AdoptReplacement(const PreparedMotionReplacementSourceDTO& source,
                                std::uint64_t candidate_id,
                                PreparedMotionReplacementCommitDTO commit) override;
  Result<void> ApplyToolContext(const PreparedOperationParticipantDTO& participant) override;
  ExecutionHandle Dispatch(const PreparedOperationParticipantDTO& participant,
                           const ControllerGoalId& controller_goal_id,
                           ControllerGoalGeneration controller_goal_generation) override;
  /// Prepared dispatch only; the trajectory-only path is refused.
  ExecutionHandle execute_trajectory(const TrajectoryDTO& trajectory) override;
  Result<void> sync_observed_state_before_planning() override;
  /// The lane's hard-stop primitive for the Kernel's own paths (execution
  /// timeout, worker exception, teardown): `main`'s goal cancel, never waits.
  StopResultDTO stop() override;
  /// Reset belongs to the Kernel (its Stop stages, then motion-free
  /// retirement); the backend Reset is refused.
  ResetResultDTO reset() override;
  ExecutionSnapshot get_execution_snapshot() const override;
  const ObservedRobotStatePublication* observed_robot_state_publication()
      const noexcept override {
    return state_authority_.publication;
  }
  const ExecutionProgressPublication* execution_progress_publication() const noexcept override {
    return &progress_;
  }
  std::optional<double> stopped_trajectory_time_s(
      std::uint64_t execution_generation) const override;
  void retire_execution_progress_generation(std::uint64_t execution_generation) noexcept override;
  ExecutionProgressHazardClaimResult claim_execution_progress_hazard(
      std::uint64_t execution_generation) noexcept override;
  void set_runtime_velocity_scale(double scale) override;
  void apply_runtime_velocity_scale(double scale, std::uint64_t sequence) override;
  RuntimeScaleMechanismStatusDTO runtime_velocity_scale_status() const override;
  [[nodiscard]] RuntimeScaleMechanismAvailability runtime_velocity_scale_availability()
      const noexcept override;

  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStage(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation) override;

  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStageForTakeover(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation, IStopStageSpringReturnObserver& observer) override;

 private:
  [[nodiscard]] ParticipantStopStageResultDTO TranslateStopStageImpl(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation, IStopStageSpringReturnObserver* observer);

  struct Execution;
  struct PendingDispatch;
  [[nodiscard]] Result<void> RebindPortImpl(std::shared_ptr<IExecutionPort> port,
                                           bool owner_rebuilt);
  struct Replacement;
  [[nodiscard]] bool SourceMatchesLocked(const PreparedMotionReplacementSourceDTO& source) const;
  [[nodiscard]] PreparedMotionReplacementObservationDTO ObserveReplacementLocked() const;
  [[nodiscard]] ExecutionPortCommandResultDTO CancelExecution(Execution& execution,
      std::unique_lock<std::mutex>& lock, std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] bool ReplacementScaleCompatibleLocked(const ExecutionControllerSampleDTO& sample,
                                                      const TrajectoryDTO& candidate) const;
  struct PreparedGoalRecord {
    ControllerGoalId goal_id;
    ControllerGoalGeneration generation;
    ContentDigest prepared_contract_digest;
    ExecutionHandle handle;
  };

  [[nodiscard]] ExecutionHandle RejectedHandle(const PreparedOperationParticipantDTO& participant,
                                               const ControllerGoalId& goal_id,
                                               ControllerGoalGeneration generation,
                                               ErrorDTO error) const;
  [[nodiscard]] std::optional<std::string_view> BindingFieldError(
      const PreparedOperationParticipantDTO& participant) const;
  /// Measured authority: waits (outside the participant lock) until the
  /// current port holds a fresh complete sample or the state timeout passes.
  void AwaitFreshStartSample();
  /// Measured authority: `main`'s start-state check of `trajectory` against
  /// `sample`; the refusal, or nullopt when the robot is at the start.
  [[nodiscard]] std::optional<ErrorDTO> StartStateError(
      const std::optional<ExecutionControllerSampleDTO>& sample, ExecutionBindingEpoch epoch,
      const TrajectoryDTO& trajectory) const;
  /// One evaluation of the port's facts for `execution` (the Kernel worker
  /// drives it through the handle).
  void Evaluate(Execution& execution, std::unique_lock<std::mutex>& lock);
  void Finish(Execution& execution, ExecutionState state, std::optional<ErrorDTO> error);
  /// Progress and snapshot from one associated sample at `original_cursor_s`.
  void ObserveSample(Execution& execution, const ExecutionControllerSampleDTO& sample,
                     double original_cursor_s, TimePoint now, bool publish_progress = true);
  [[nodiscard]] bool WaitForCompletion(const std::shared_ptr<Execution>& execution,
                                       std::int64_t timeout_ns);
  /// Records one timing window; returns the controller period when it is
  /// beyond the lane margin.
  std::optional<double> ObserveTiming(IExecutionPort& port, ExecutionBindingEpoch epoch);
  void RecordRecognition(Execution& execution);
  /// Speed ramp: publish one value now and schedule the next tick while
  /// the ramp runs. Caller holds `scale_mutex_`.
  void ScheduleRampTickLocked();
  void RampTick(std::uint64_t ramp_generation);
  /// The Stop-stage helper's speed freeze.
  std::optional<double> FreezeSpeedFactor(IExecutionPort& port,
                                          std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] std::shared_ptr<ParticipantStopStageHelper> MakeHelper(IExecutionPort& port);

  const IClock& clock_;
  IScheduler& scheduler_;
  StopStageWaiter waiter_;
  ExecutionPortParticipantStateAuthority state_authority_;
  ExecutionPortParticipantConfig config_;
  ExecutionProgressPublication progress_;

  mutable std::mutex mutex_;
  RuntimeParticipantBinding binding_;
  std::vector<RuntimeParticipantBinding> program_aliases_;
  std::shared_ptr<IExecutionPort> port_;
  /// Lock-free view of the current port and lane scale fact, republished at
  /// every rebind, for the owner's availability reads under its own lock.
  struct ScaleAvailabilityView final {
    std::shared_ptr<IExecutionPort> port;
    bool supports_scale{};
  };
  std::atomic<std::shared_ptr<const ScaleAvailabilityView>> scale_availability_view_;
  std::shared_ptr<PendingDispatch> pending_dispatch_;
  std::shared_ptr<ParticipantStopStageHelper> helper_;
  // Preserve the retired owner's bounded goal records until their prepared
  // memberships retire; the helper borrows this exact port.
  std::shared_ptr<IExecutionPort> retired_owner_port_;
  std::shared_ptr<ParticipantStopStageHelper> retired_owner_helper_;
  PreparedExecutionMembership prepared_membership_;
  std::optional<PreparedGoalRecord> prepared_goal_;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> owner_rebuild_observation_;
  std::optional<std::uint64_t> retired_unknown_candidate_;
  bool retired_owner_rest_qualified_{false};
  std::shared_ptr<Execution> execution_;
  std::unique_ptr<Replacement> replacement_;
  std::uint64_t next_execution_generation_{1U};
  std::uint32_t stop_stages_in_flight_{0U};
  /// Stop stages ever entered: compared across the unlocked tool-context
  /// action up to Dispatch, so a Stop that started (and maybe ended) meanwhile wins.
  std::uint64_t stop_stages_started_{0U};
  /// The Stop count when the latest tool-context action started; the
  /// next Dispatch refuses its goal when a Stop started since.
  std::optional<std::uint64_t> tool_context_stops_before_;
  /// The goal a Stop stage found unsent and contained; Dispatch refuses it.
  std::optional<ExecutionGoalKey> stopped_before_dispatch_;
  ExecutionPortTimingMetrics timing_{};
  ExecutionLaneFactsDTO lane_;

  mutable std::mutex scale_mutex_;
  double target_scale_{1.0};
  /// OD-72: the sequence of the latest applied speed value (guarded by
  /// `scale_mutex_`); an older value arriving later is superseded.
  std::uint64_t applied_scale_sequence_{0U};
  std::optional<double> last_sent_scale_;
  bool scale_frozen_{false};
  struct PreparationScaleHold {
    std::uint64_t candidate_id;
    ExecutionGoalKey source_goal;
    double factor;
    bool confirmed{false};
  };
  std::optional<PreparationScaleHold> preparation_scale_hold_;

  /// The factor moves linearly toward its target.
  struct ScaleRamp {
    double value;
    double target;
    double rate_per_s;
  };
  std::optional<ScaleRamp> ramp_;
  TimePoint last_ramp_tick_{0};
  std::uint64_t ramp_generation_{0U};
  ScheduleHandle ramp_handle_{0U};
  bool scheduler_detach_started_{false};
  bool scheduler_detach_complete_{false};
};

}  // namespace manyforge::core
