#pragma once

#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/execution_correlation_tracker.hpp>
#include <manyforge_core/execution_snapshot_publisher.hpp>
#include <manyforge_core/execution_worker_signals.hpp>
#include <manyforge_core/interrupt/interrupt_manager.hpp>
#include <manyforge_core/kernel_event_publisher.hpp>
#include <manyforge_core/motion_trip_stop_coordinator.hpp>
#include <manyforge_core/operator_stop_worker.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/terminal_committer.hpp>
#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/kernel_event.hpp>
#include <manyforge_core/types/metrics.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::core {

struct ExecutionKernelConfig {
  Duration collision_monitor_period_ns = 50'000'000;  // 50 ms / 20 Hz

  // Hard per-tick budget for the observed runtime collision query after the
  // live collision-world snapshot has been acquired. This is intentionally
  // separate from the monitor cadence: the industrial performance target is
  // p95 <= 50% of `collision_monitor_period_ns`, but the fail-closed timeout
  // must allow contact-rich distance queries to complete within the full
  // configured monitor period before preempting execution.
  Duration runtime_collision_query_timeout_ns = 50'000'000;  // 50 ms

  // When true, a collision-validation rejection re-runs the failing waypoint
  // through the planning context's normalized collision query and appends
  // `legacy_collision_compare_*` engine-parity keys to the error details.
  // Default OFF: the duplicate check costs a full collision query on every
  // rejection and the parity evidence belongs to qualification runs, not
  // production rejections.
  bool enable_legacy_collision_compare_diagnostics = false;

  // Native execution-backend lifecycle drain. ROS adapters expose their own
  // `stop_completion_timeout_s`; this field provides the equivalent reset
  // drain knob for the non-ROS RoboPlan backend used by Composer/native
  // simulation.
  double native_reset_completion_timeout_s = 5.0;

  // Per-call budget (seconds) for single-shot IK resolution
  // (`IPlannerBackend::solve_ik`), used by the RoboPlan/ros2_control factories
  // to size the dedicated single-shot SimpleIk solver. This is deliberately
  // tighter than the planning-IK budget so interactive solve_ik calls fail
  // fast rather than stalling the caller; deployments needing more restart
  // headroom on hard targets can raise it. Must be > 0.
  double single_shot_ik_timeout_s = 0.05;

  // Authoritative collision validation worker-pool sizing. The concrete
  // engines are injected through core-owned ports; this knob only selects how
  // many injected workers the kernel asks the pool to use. Four workers is the
  // production default: it gives useful trajectory-validation speedup on Jetson
  // Orin Nano-class systems while leaving CPU headroom for ROS/controller,
  // observer, and Composer lanes. Values < 1 are normalized to 1 by the kernel;
  // deployments can override with MANYFORGE_VALIDATION_COLLISION_WORKERS.
  int validation_collision_workers = 4;
  int validation_collision_min_waypoints_for_parallel = 8;

  // Deployment-configured RRT planning budget ceiling. Per-request
  // `planning_timeout_s` can lower this budget; deployments that need longer
  // planning windows must raise this value explicitly so the authority surface
  // is visible in the deployment artifact.
  double planner_max_planning_time_s = 5.0;

  // Additional planning attempts allowed when the authoritative
  // manyforge_collision validator rejects a freshly planned candidate for a
  // blocking collision. RRT planning is probabilistic and its internal
  // validity checker is not yet the same object as the final validation
  // authority, so the kernel treats an authoritative rejection as "candidate
  // invalid" and asks the planner for another candidate before failing
  // closed. Zero preserves single-shot behavior.
  int authoritative_validation_collision_replan_attempts = 2;

  // Optional deployment-baseline collision policy. Factories synthesize the
  // historical `default_free_space` policy only when this is absent.
  std::optional<CollisionPolicySnapshot> baseline_collision_policy_snapshot;

  // Runtime collision monitoring must run against live observed robot state,
  // not authored/planning-context state. A backend snapshot older than this
  // budget causes a runtime collision fault with a freshness reason before
  // the collision engine is queried. Zero disables the age check for
  // deterministic tests only; production deployments must keep this positive.
  Duration runtime_observed_state_max_age_ns = 100'000'000;  // 100 ms

  // Dynamic bodies in CollisionWorldSnapshot may carry their own freshness
  // budget. Static authored/fixture bodies leave that metadata unset and do
  // not expire. This kernel-level cap prevents a dynamic source from declaring
  // an unbounded budget silently; zero disables only the cap, not body-local
  // freshness checks.
  Duration runtime_dynamic_body_max_age_cap_ns = 500'000'000;  // 500 ms

  // Terminal motion-outcome retention. Outcomes are queryable via
  // `get_execution_outcome(execution_id)` until the store reaches this
  // depth; oldest entries are evicted FIFO. Audit-grade deployments
  // should raise this and pair with an audit archive subscriber.
  std::size_t motion_execution_outcome_retention = 256;

  // Deployment stop-confirmation tolerance. When a controlled-stop profile
  // leaves `max_stop_time_s` unset, the effective stop deadline is the
  // requested/default deceleration time plus this tolerance.
  Duration complete_move_on_signal_timeout_ns = 500'000'000;  // 500 ms

  // Deployment-default fault-stop profile. Used by
  // `apply_latched_interrupt_preemption`
  // when a latched fault policy has `preemption_action == kPreemptTrajectory`
  // AND no `stop_profile` of its own. Deployments should normally configure
  // `kSpringBackHold`, matching the operator's stated intent ("default on
  // interrupts / collision-during-motion is spring-back"). Leave unset to
  // preserve the legacy `execution_backend_.stop()` behavior.
  //
  // The per-policy `InterruptPolicyDefinition.stop_profile` wins when
  // present. This kernel-level default applies only when the policy
  // is otherwise unconfigured, so deployments can either opt the whole
  // kernel into controlled-stop semantics here or override on a per-
  // policy basis.
  std::shared_ptr<MotionInterruptStopProfileDTO> default_fault_stop_profile;

  // Deployment-default operator-stop profile. Used by
  // `ExecutionKernel::stop()` (the operator stop-button path) to route
  // active physical motion through `execute_controlled_stop` instead of the
  // legacy `execution_backend_.stop()`. Deployments should normally configure
  // `kSoftOnPath`, matching the operator's stated intent ("default on stop
  // button press is soft stop on path"). Leave unset to keep the legacy
  // behavior.
  //
  // The two defaults are intentionally distinct because the operator
  // stop button and a fault interrupt are operationally different
  // events: a fault is "the system noticed something wrong, stop now
  // safely" (default spring-back to known state), while an operator
  // stop is "I am taking back control, decelerate cleanly along the
  // current path" (default soft-on-path).
  std::shared_ptr<MotionInterruptStopProfileDTO> operator_stop_profile;
};

/// Execution Kernel — single authority for motion execution governance.
/// Ties together: state machine, stop/collision policies, trajectory cache,
/// planner backend, planning context, and execution backend.
///
/// Spec 230: all operations are ROS-free. This is an async v0.1
/// implementation with event subscription.
class ExecutionKernel {
 public:
  ExecutionKernel(IPlannerBackend& planner,
                  IPlanningContext& context,
                  IExecutionBackend& execution_backend,
                  IClock& clock,
                  IScheduler& scheduler,
                  TrajectoryCache& cache,
                  ExecutionKernelConfig config = {});

  ~ExecutionKernel();

  /// Execute a motion request. Only valid from Idle.
  /// Returns execution_id immediately; plan → validate → dispatch runs
  /// on a worker thread. Callers observe progress via get_status() and events.
  Result<uint64_t> execute(const ExecuteRequestDTO& request);

  /// Non-blocking stop from any non-terminal state.
  /// Accepted stop invalidates older lifecycle/recovery commands by bumping
  /// `control_command_epoch()`. Stop is never rejectable by epoch staleness.
  StopResultDTO stop();

  /// Non-blocking pause. Only valid from Executing.
  PauseResultDTO pause();

  /// Non-blocking resume. Only valid from Paused.
  ResumeResultDTO resume(
      std::optional<std::uint64_t> expected_epoch = std::nullopt);

  /// Non-blocking reset. Valid from any state except Idle.
  ResetResultDTO reset(
      std::optional<std::uint64_t> expected_epoch = std::nullopt);

  /// Monotonic lifecycle/recovery command epoch. Python control surfaces use
  /// this only to stamp bounded control calls; the C++ kernel owns the actual
  /// stale-command rejection at side-effect boundaries.
  std::uint64_t control_command_epoch() const;
  std::uint64_t bump_control_command_epoch();

  /// Backend-agnostic accessor for the active execution backend's
  /// motion-interrupt stop capability (the advertised supported stop modes
  /// and the soft_on_path planner config it would use). This is the same
  /// capability the arm-time gate validates against; exposing it lets a
  /// deployment load-time check fail closed — disabling execution before any
  /// motion is armed — when the configured operator/fault stop profile names
  /// a mode the backend does not advertise. Delegates to
  /// IExecutionBackend::get_motion_interrupt_stop_capability.
  MotionInterruptStopCapabilityDTO get_motion_interrupt_stop_capability() const;

  /// Acknowledge a successful terminal execution and return to Idle.
  ///
  /// Valid only when the kernel is currently Completed for exactly the supplied
  /// execution_id. Failed and Stopped executions require Reset/recovery instead.
  AcknowledgeCompletedResultDTO acknowledge_completed(
      uint64_t execution_id,
      std::optional<std::uint64_t> expected_epoch = std::nullopt);

  /// Acknowledge an `kInterventionRequired` state and return to Idle.
  ///
  /// Valid only from `kInterventionRequired`. Emits a traceable
  /// `kInterventionAcknowledged` event with the supplied `principal_id` and
  /// `reason`. This is the reset/resync recovery path. `execute()` rejects
  /// with `kErrorInvalidRequest` while `kInterventionRequired`, requiring an
  /// explicit recovery action first. See `spec/200` and ADR-0005.
  AcknowledgeInterventionResultDTO acknowledge_intervention(
      std::string principal_id,
      std::string reason,
      std::optional<std::uint64_t> expected_epoch = std::nullopt);

  /// Acknowledge a stop/fault intervention for retry without backend reset.
  ///
  /// Valid only from `kInterventionRequired` when the latched error is a
  /// classified stop/fault intervention and fresh backend state proves the
  /// robot is readable and stopped when measured velocity evidence exists.
  /// Unlike `acknowledge_intervention`, this path MUST NOT call
  /// `execution_backend.reset()`: it preserves the current robot pose so the
  /// next execute replans from where the robot actually stopped.
  AcknowledgeInterventionResultDTO acknowledge_intervention_for_retry(
      std::string principal_id,
      std::string reason,
      std::optional<std::uint64_t> expected_epoch = std::nullopt);

  /// Acknowledge a kernel-owned runtime collision interrupt for retry.
  ///
  /// Valid only from `kInterrupted` with the runtime-collision fault latch
  /// active. Clears the latch, resets kernel execution bookkeeping to Idle,
  /// and republishes the current backend snapshot without calling
  /// `execution_backend.reset()`. This preserves the robot's actual stopped
  /// pose so the next execute plans from that pose instead of commanding the
  /// simulator/controller back to its reset/home state.
  AcknowledgeInterruptedResultDTO acknowledge_interrupted_for_retry(
      std::string principal_id,
      std::string reason,
      std::optional<std::uint64_t> expected_epoch = std::nullopt);

  /// ADR-0018: update the runtime velocity scale for active execution.
  ///
  /// The UI velocity slider drives this through cycle_manager → kernel →
  /// backend. Backends retime discrete trajectory execution continuously;
  /// changing the scale never stops an action by itself. `scale` is validated
  /// finite and clamped to [0, 1]. NOT serialized through lifecycle_mutex_:
  /// this is a high-rate operator surface and the backend's atomic store
  /// handles concurrency. `principal_id` is optional because lower-level
  /// backend/direct callers may not have a supervisory actor identity.
  void set_runtime_velocity_scale(
      double scale, std::optional<std::string> principal_id = std::nullopt);

  /// Get current kernel status.
  ExecutionStatus get_status() const;

  /// Get the latest observer-safe execution snapshot published by the kernel.
  ExecutionSnapshot get_execution_snapshot() const;

  /// Refresh the observer-facing snapshot from the current planning-context
  /// state. Intended for explicit simulator/bootstrap paths, not motion
  /// execution semantics.
  void sync_snapshot_from_context();

  /// Run a single collision monitoring tick.
  /// Called externally (e.g., by a scheduler) during Executing.
  Result<void> collision_check_tick();

  /// Install (or replace) the collision policy snapshot used by the
  /// authoritative CollisionValidationCoordinator path.
  ///
  /// The kernel takes ownership of the snapshot via copy; per-pass support-
  /// contact state lives in a coordinator instance constructed inside
  /// validate_trajectory(). Passing nullopt is allowed only for tests or
  /// bootstrap error paths; execution validation then fails closed because
  /// there is no authoritative collision policy.
  void set_collision_validation_policy_snapshot(
      std::optional<CollisionPolicySnapshot> snapshot);

  /// Install the authoritative collision validation world publisher and
  /// query-engine workers. The publisher and engines are backend-agnostic
  /// ManyForge-owned ports; RoboPlan / MoveIt / Isaac may publish snapshots or
  /// implement the port, but the kernel never falls back to planner-internal
  /// collision checks.
  ///
  /// `publisher` must outlive the kernel. Engines are retained by shared_ptr
  /// so the composition root can hand them to the kernel without additional
  /// lifetime coupling.
  void set_collision_validation_pipeline(
      ICollisionWorldSnapshotPublisher& publisher,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> engines);

  /// Optional pose-source / per-state retry configuration for the
  /// CollisionValidationCoordinator. Has no effect until a policy snapshot is
  /// installed via set_collision_validation_policy_snapshot.
  void set_collision_validation_retry_config(
      PoseSourceRetryConfig retry, std::int64_t per_state_timeout_ns = 0);

  // ---------------------------------------------------------------
  // Kernel-native interrupt registration and latch surface (spec/260,
  // sprint Chunk I-2). This chunk owns source/policy registration, signal
  // ingestion, priority arbitration, latch visibility, disposition validation,
  // and event emission.
  // ---------------------------------------------------------------

  Result<void> register_interrupt_source(
      const InterruptSourceDefinition& source);
  Result<void> register_interrupt_policy(
      const InterruptPolicyDefinition& policy);
  InterruptIngestResult publish_fast_interrupt_signal(
      const InterruptSignalUpdate& update);
  InterruptIngestResult publish_supervisory_interrupt_signal(
      const InterruptSignalUpdate& update);
  InterruptIngestResult publish_fast_interrupt_signals(
      const std::vector<InterruptSignalUpdate>& updates);
  std::optional<InterruptLatch> get_active_interrupt_latch() const;
  Result<InterruptIngestResult> apply_interrupt_disposition(
      const InterruptDisposition& disposition);

  // ---------------------------------------------------------------
  // Motion-scoped terminal outcome surface (spec/210 §3.11,
  // spec/230 §3.4). Read-only observer; Composer and the behavior
  // runtime consume outcomes here instead of polling signal state.
  // ---------------------------------------------------------------

  /// Return the immutable terminal outcome record for `execution_id`, if
  /// the kernel has emitted one. Outcomes survive subsequent executions
  /// so callers (BT, MCP, audit) can correlate by id; the bound is the
  /// `motion_execution_outcome_retention` config knob.
  std::optional<MotionExecutionOutcomeDTO> get_execution_outcome(
      std::uint64_t execution_id) const;

  /// Diagnostics-only observer for armed move-scoped bindings
  /// (spec/260 §B.3). BT/policy code MUST NOT poll this for control flow.
  std::vector<MotionInterruptScopeStatusDTO> active_motion_interrupt_scopes()
      const;

  /// Subscribe to kernel events.
  /// @param callback Called for each event. Must be thread-safe.
  /// @return Handle for unsubscribing.
  SubscriptionHandle subscribe_events(EventCallback callback);

  /// Unsubscribe from kernel events.
  void unsubscribe_events(SubscriptionHandle handle);

  /// Block until the current execution completes (reaches terminal state).
  /// Useful for tests and synchronous callers.
  /// @param timeout_ns Maximum time to wait (0 = wait forever).
  /// @return true if execution completed, false if timed out.
  bool wait_for_completion(int64_t timeout_ns = 0);

  /// Get current metrics snapshot.
  MetricsSnapshot get_metrics() const;

 private:
  Result<MotionSpecDTO> resolve_motion_spec(const ExecuteRequestDTO& request);
  Result<TrajectoryDTO> plan_trajectory(const ExecuteRequestDTO& request);
  Result<TrajectoryDTO> plan_resume_trajectory(
      const ExecuteRequestDTO& request);
  Result<CollisionWorldRevision> validate_trajectory(
      const TrajectoryDTO& trajectory,
      const ExecuteRequestDTO& request);

  /// Async execution worker function.
  void async_execute_worker(std::stop_token st,
                            ExecuteRequestDTO request,
                            uint64_t exec_id,
                            std::optional<TrajectoryDTO> cached_trajectory);

  /// Emit an event to all subscribers.
  void emit_event(KernelEvent event);
  void emit_event_immediate(KernelEvent event);
  void prepare_event(KernelEvent& event) const;
  void dispatch_prepared_event(const KernelEvent& event);
  class EventDeferralScope;
  static thread_local EventDeferralScope* active_event_deferral_;
  void set_active_request_correlation(const ExecuteRequestDTO& request);
  void clear_active_request_correlation();
  void stamp_active_request_correlation(
      std::map<std::string, std::string>& details) const;
  void stamp_active_request_correlation(ExecutionSnapshot& snapshot) const;

  /// K3: route a terminal decision through the TerminalCommitter.
  /// Returns the outcome; `outcome.committed` is the state that must
  /// appear in any snapshot/event published for this commit.
  TerminalCommitter::CommitOutcome commit_terminal(
      ExecutionState proposed,
      const char* reason,
      TerminalCommitter::Origin origin = TerminalCommitter::Origin::kGeneric);

  /// Publish an observer-safe execution snapshot.
  void publish_execution_snapshot(ExecutionSnapshot snapshot);
  void start_collision_monitoring();
  void stop_collision_monitoring(bool drain_callbacks = false);
  CollisionWorldRevision current_authoritative_collision_world_revision() const;
  std::shared_ptr<CollisionCancellationToken> install_validation_cancel_token();
  void clear_validation_cancel_token(
      const std::shared_ptr<CollisionCancellationToken>& token);
  void request_validation_cancel_from_interrupt(
      const InterruptIngestResult& result);
  void request_planning_cancel_from_interrupt(
      const InterruptIngestResult& result);
  void record_terminal_fault_authority(
      const InterruptIngestResult& result);
  void mark_active_interrupt_preemption_result(
      InterruptPreemptionResult result,
      const std::map<std::string, std::string>& extra_details = {});
  void mark_interrupt_preemption_result(
      const InterruptLatch& latch,
      InterruptPreemptionResult result,
      const std::map<std::string, std::string>& extra_details = {});
  void apply_interrupt_ingest_effects(
      const InterruptIngestResult& result,
      bool apply_preemption_inline);
  void install_builtin_interrupt_sources();
  InterruptIngestResult publish_runtime_collision_fault_interrupt(
      const CollisionCheckResultDTO& check,
      const std::map<std::string, std::string>& metadata = {});
  InterruptIngestResult publish_fast_interrupt_signals_internal(
      const std::vector<InterruptSignalUpdate>& updates,
      bool apply_preemption_inline);
  void schedule_latched_interrupt_preemption(
      InterruptIngestResult result);
  void apply_latched_interrupt_preemption(
      const InterruptIngestResult& result,
      std::stop_token worker_stop_token = {});
  void request_fault_preempt_controlled_stop();
  bool control_command_epoch_matches(
      std::optional<std::uint64_t> expected_epoch) const;
  InterruptRuntimeContext current_interrupt_context() const;
  void stamp_interrupt_latch_collision_world_revision(
      InterruptIngestResult& result);
  void emit_interrupt_events(const InterruptIngestResult& result);

  /// Operator-stop worker lifecycle. Operator stops can run a
  /// deployment-configured controlled-stop profile; that backend call may take
  /// the soft-stop deceleration window. Keep the API admission path short and
  /// run the long backend work on this cancellable worker instead.
  bool cancel_operator_stop_worker(bool invalidate_generation);
  void join_finished_operator_stop_worker();
  bool wait_for_operator_stop_worker(std::chrono::steady_clock::duration timeout);
  bool start_operator_stop_worker(MotionInterruptStopProfileDTO profile,
                                  ExecutionState pre_completion_state);
  void run_operator_stop_worker(std::uint64_t generation,
                                MotionInterruptStopProfileDTO profile,
                                ExecutionState pre_completion_state,
                                std::stop_token stop_token);
  bool operator_stop_worker_is_current(std::uint64_t generation,
                                       std::stop_token stop_token) const;
  void begin_runtime_velocity_scale_freeze();
  void end_runtime_velocity_scale_freeze();

  // Motion-scope lifecycle plumbing on the worker thread.
  // `active_motion_execution_` is reset at execute() entry and populated
  // by `arm_motion_scopes_for_dispatch_locked` after validation passes.
  // The worker MUST call `finalize_motion_execution_outcome` exactly
  // once on every terminal exit so the outcome store has a 1:1
  // correspondence with terminal executions.
  struct ActiveMotionExecution {
    std::uint64_t execution_id = 0;
    std::string move_id;
    bool had_scopes = false;
    bool arm_succeeded = false;
    bool outcome_emitted = false;
    bool correlation_captured = false;
    std::optional<ActionSpanDTO> action_span;
    std::string contact_policy_id;
    std::optional<InterruptLatch> motion_trip_latch;
  };

  enum class ExecuteWorkerPhaseStatus {
    kContinue,
    kTerminal,
  };

  struct ExecuteWorkerPlanPhaseResult {
    ExecuteWorkerPhaseStatus status = ExecuteWorkerPhaseStatus::kContinue;
    TrajectoryDTO trajectory;
    bool active_trajectory_from_cache = false;
  };

  struct ExecuteWorkerValidationPhaseResult {
    ExecuteWorkerPhaseStatus status = ExecuteWorkerPhaseStatus::kContinue;
    CollisionWorldRevision collision_world_revision;
  };

  struct ExecuteWorkerDispatchPhaseResult {
    ExecuteWorkerPhaseStatus status = ExecuteWorkerPhaseStatus::kContinue;
    ExecutionHandle execution_handle;
    std::int64_t exec_start_ns = 0;
    std::int64_t exec_timeout_ns = 0;
    ExecutionSnapshot latest_backend_snapshot;
  };

  void finish_execute_worker();
  void notify_execution_waiters();
  bool retire_execute_worker(std::unique_lock<std::mutex>& lifecycle_lock);

  bool finalize_overridden_terminal_authority(
      const ExecuteRequestDTO& request,
      std::uint64_t exec_id,
      ActiveMotionExecution& motion_state,
      ExecutionState committed,
      const char* reason,
      std::optional<ErrorDTO> error = std::nullopt);

  bool finalize_competing_terminal_authority(
      const ExecuteRequestDTO& request,
      std::uint64_t exec_id,
      ActiveMotionExecution& motion_state,
      const char* reason);

  ExecuteWorkerPlanPhaseResult execute_worker_plan_phase(
      std::stop_token st,
      const ExecuteRequestDTO& request,
      std::uint64_t exec_id,
      std::optional<TrajectoryDTO> cached_trajectory,
      ExecutionState worker_entry_state,
      ActiveMotionExecution& motion_state);

  ExecuteWorkerValidationPhaseResult execute_worker_validate_replan_phase(
      std::stop_token st,
      const ExecuteRequestDTO& request,
      std::uint64_t exec_id,
      TrajectoryDTO& trajectory,
      bool& active_trajectory_from_cache,
      ActiveMotionExecution& motion_state);

  ExecuteWorkerDispatchPhaseResult execute_worker_dispatch_phase(
      const ExecuteRequestDTO& request,
      std::uint64_t exec_id,
      const TrajectoryDTO& trajectory,
      bool active_trajectory_from_cache,
      const CollisionWorldRevision& validation_world_revision,
      ActiveMotionExecution& motion_state);

  void execute_worker_finalize_phase(
      const ExecuteRequestDTO& request,
      std::uint64_t exec_id,
      ExecutionHandle& execution_handle,
      ActiveMotionExecution& motion_state);

  /// Arm scopes carried by `request.motion_interrupt_scopes` atomically
  /// with dispatch (spec/230 §3.4): all scopes succeed or none do. On
  /// success, populates `motion_state.had_scopes` /
  /// `motion_state.arm_succeeded` and emits
  /// `motion_interrupt_scope_armed` kernel events. Returns Err with the
  /// manager's failure reason; the caller fails the execution closed.
  Result<void> arm_motion_scopes_for_dispatch(
      const ExecuteRequestDTO& request,
      std::uint64_t execution_id,
      ActiveMotionExecution& motion_state);

  /// Build, validate, store, and emit the immutable
  /// `MotionExecutionOutcomeDTO` for `execution_id`. Always reaps any
  /// remaining armed bindings tied to `execution_id`. Safe to call from
  /// every terminal exit; subsequent calls for the same execution are
  /// no-ops so worker code can be defensive without producing duplicate
  /// records.
  void finalize_motion_execution_outcome(
      ActiveMotionExecution& motion_state,
      MotionExecutionTerminalCause terminal_cause,
      std::optional<ErrorDTO> error,
      std::optional<MotionTripOutcomeDTO> trip,
      std::string reap_reason);

  /// Handle a `complete_move_on_signal` latch (spec/260 §5.6) by
  /// running the controlled-stop flow end to end. The
  /// function takes the worker's `motion_state`, the originating
  /// `request` (so the matching `MotionInterruptScopeDTO.stop_profile`
  /// can be resolved from `latch.motion_interrupt_scope_id`), the
  /// latch itself, and the worker's `exec_id` / `latest_backend_snapshot`.
  /// It performs the controlled-stop call, captures the stop snapshot,
  /// enforces the conjunctive fallback rule, transitions the state
  /// machine Stopping->Completed (signal_trip success) or
  /// Stopping->Failed (failure path), and finalizes the outcome.
  void handle_motion_trip_latch(
      const ExecuteRequestDTO& request,
      const InterruptLatch& latch,
      std::uint64_t exec_id,
      const ExecutionSnapshot& latest_backend_snapshot,
      ActiveMotionExecution& motion_state,
      std::stop_token worker_stop_token);

  void emit_interrupt_manager_events(
      const std::vector<InterruptManagerEvent>& events,
      std::uint64_t execution_id);

  IPlannerBackend& planner_;
  IPlanningContext& context_;
  IExecutionBackend& execution_backend_;
  IClock& clock_;
  IScheduler& scheduler_;
  TrajectoryCache& cache_;
  ExecutionKernelConfig config_;

  ExecutionStateMachine sm_;
  StopPolicy stop_policy_;
  InterruptManager interrupt_manager_;
  /// K3: the only path to terminal states. Declared after the objects its
  /// probes read so construction order matches the authority dependency.
  TerminalCommitter terminal_committer_;
  std::atomic<std::int64_t> runtime_collision_fault_sequence_{0};

  // Authoritative collision validation pipeline. Missing policy,
  // publisher, snapshot, or engines are configuration errors and fail closed;
  // the previous planner-context collision_check fallback is not a supported
  // execution path.
  mutable std::mutex coordinator_mutex_;
  std::shared_ptr<const CollisionPolicySnapshot> coordinator_policy_snapshot_;
  std::shared_ptr<const CollisionPolicyIndex> coordinator_policy_index_;
  PoseSourceRetryConfig coordinator_retry_config_{};
  std::int64_t coordinator_per_state_timeout_ns_ = 0;
  ICollisionWorldSnapshotPublisher* collision_snapshot_publisher_ = nullptr;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> collision_validation_engines_;
  std::unique_ptr<ParallelCollisionValidationWorkerPool>
      collision_validation_worker_pool_;
  mutable std::mutex validation_cancel_mutex_;
  std::weak_ptr<CollisionCancellationToken> active_validation_cancel_token_;

  // Serializes low-rate lifecycle authority transitions across execute, stop,
  // pause, resume, reset, and acknowledge.
  std::mutex lifecycle_mutex_;
  std::atomic<std::uint64_t> control_command_epoch_{0};
  std::atomic<uint64_t> next_execution_id_{1};
  std::atomic<double> runtime_velocity_scale_{1.0};
  mutable std::mutex runtime_velocity_scale_mutex_;
  int runtime_velocity_scale_freeze_depth_ = 0;
  std::optional<double> deferred_runtime_velocity_scale_;
  std::optional<std::string> deferred_runtime_velocity_scale_principal_id_;
  ExecutionCorrelationTracker active_correlation_;

  // Event subscription and copy-on-write callback dispatch.
  KernelEventPublisher event_publisher_;

  // Observer-facing execution snapshot storage and sequence owner.
  ExecutionSnapshotPublisher snapshot_publisher_;

  // Async execution state
  std::jthread worker_thread_;
  std::uint64_t execute_worker_generation_ = 0;  // guarded by lifecycle_mutex_
  ExecutionWorkerSignals worker_signals_;
  std::atomic<bool> execute_admission_interruptible_{false};
  std::atomic<bool> lifecycle_stop_requested_while_busy_{false};
  std::atomic<bool> lifecycle_stop_requested_event_emitted_{false};
  std::atomic<bool> lifecycle_reset_requested_while_busy_{false};
  std::atomic<bool> lifecycle_reset_waiting_for_stop_{false};
  std::atomic<bool> lifecycle_worker_retirement_in_progress_{false};
  // Monotonic per-execution terminal authority. Active interrupt latches can
  // be consumed by preemption/stop machinery before the backend finalizer runs;
  // this flag preserves the fact that safety authority existed so a later
  // normal completion cannot launder it.
  std::atomic<bool> terminal_stop_authority_asserted_{false};
  std::atomic<bool> terminal_fault_authority_asserted_{false};
  OperatorStopWorker operator_stop_worker_;

  // Stop-path collision boundary. While a motion-trip controlled stop is in
  // flight, the runtime collision monitor keeps running over the decelerating
  // robot. On a collision the monitor does NOT latch a new fault or touch the
  // state machine; it only signals this coordinator. The trip handler then owns
  // the hard-stop escalation and terminal decision. Monitor signals; worker
  // acts.
  MotionTripStopCoordinator motion_trip_stop_;

  // Cancellable stop source for fault-latch preemption. Runtime collision /
  // supervisory faults can execute a configured controlled stop outside the
  // main execute-worker path, so teardown/reset/watchdogs need an explicit
  // owner they can signal without relying on the caller's thread lifetime.
  OperatorStopWorker fault_preempt_worker_;
  std::mutex fault_preempt_stop_mutex_;
  std::shared_ptr<std::stop_source> fault_preempt_stop_source_;
  // Metrics (atomic counters for thread safety)
  mutable std::mutex metrics_mutex_;
  mutable MetricsSnapshot metrics_;

  // Bounded outcome store keyed by execution_id. FIFO
  // eviction beyond `motion_execution_outcome_retention`. The eviction
  // order is captured in `motion_outcome_order_` because std::map keeps
  // execution_ids sorted ascending, not insertion-ordered; eviction
  // tracks the original publication order so retention behaves
  // intuitively under id reuse-prevention. The per-execution motion
  // state lives on the worker stack and is NOT a kernel field — two
  // worker threads must not share it across overlapping executions.
  mutable std::mutex motion_outcomes_mutex_;
  std::map<std::uint64_t, MotionExecutionOutcomeDTO> motion_execution_outcomes_;
  std::vector<std::uint64_t> motion_outcome_order_;

  // Declared last so its destructor runs first after the kernel destructor body.
  // The destructor body already stops/drains it explicitly; this member order is
  // a second guard that prevents scheduled callbacks from observing partially
  // destroyed kernel state if future teardown code changes.
  RuntimeCollisionMonitor runtime_collision_monitor_;
};

}  // namespace manyforge::core
