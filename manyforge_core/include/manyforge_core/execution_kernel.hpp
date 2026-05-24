#pragma once

#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/collision/collision_policy.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/interrupt/interrupt_manager.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/kernel_event.hpp>
#include <manyforge_core/types/metrics.hpp>
#include <manyforge_core/types/streaming_session.hpp>

#include <atomic>
#include <condition_variable>
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
  Duration collision_monitor_period_ns = 10'000'000;  // 10 ms

  // Direct-control Cartesian pushes must not inherit full planning IK
  // budgets. The kernel passes this value to the planner backend and rejects
  // any target whose IK resolution exceeds the budget. 50 ms is conservative
  // enough for operator feedback while allowing one missed 30 Hz frame.
  double streaming_ik_timeout_s = 0.050;

  // Direct-control collision validation has a tighter frame budget than
  // batch trajectory validation. A target that cannot be validated inside
  // this budget is rejected fail-closed instead of being dispatched late.
  double streaming_collision_timeout_s = 0.010;

  // Native execution-backend lifecycle drains. ROS adapters expose their own
  // `stop_completion_timeout_s`; these fields provide equivalent knobs for
  // the non-ROS RoboPlan backend used by Composer/native simulation.
  double native_reset_completion_timeout_s = 5.0;
  double native_streaming_stop_completion_timeout_s = 5.0;

  // Authoritative collision validation worker-pool sizing. The concrete
  // engines are injected through core-owned ports; this knob only selects how
  // many injected workers the kernel asks the pool to use. Values < 1 are
  // normalized to 1 by the kernel.
  int validation_collision_workers = 1;
  int validation_collision_min_waypoints_for_parallel = 8;
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
  StopResultDTO stop();

  /// Non-blocking pause. Only valid from Executing.
  PauseResultDTO pause();

  /// Non-blocking resume. Only valid from Paused.
  ResumeResultDTO resume();

  /// Non-blocking reset. Valid from any state except Idle.
  ResetResultDTO reset();

  /// Acknowledge a successful terminal execution and return to Idle.
  ///
  /// Valid only when the kernel is currently Completed for exactly the supplied
  /// execution_id. Failed and Stopped executions require Reset/recovery instead.
  AcknowledgeCompletedResultDTO acknowledge_completed(uint64_t execution_id);

  /// Acknowledge an `kInterventionRequired` state and return to Idle.
  ///
  /// Valid only from `kInterventionRequired`. Emits a traceable
  /// `kInterventionAcknowledged` event with the supplied `principal_id` and
  /// `reason`. The caller (cycle manager Run-side auto-ack, MCP, operator UI)
  /// holds the policy decision about whether the leftover state is safe to
  /// clear; the kernel only enforces fail-closed semantics: `execute()`
  /// rejects with `kErrorInvalidRequest` while `kInterventionRequired`,
  /// requiring an explicit ack first. See `spec/200` and ADR-0005.
  AcknowledgeInterventionResultDTO acknowledge_intervention(
      std::string principal_id, std::string reason);

  // ---------------------------------------------------------------
  // Streaming-session mode (ADR-0016).
  //
  // begin/push/end form the kernel-side surface for continuous direct-
  // control inputs. Push targets are not motion-blocking; they are
  // coalesced by the bound execution backend. stop() / reset() from
  // kStreaming map to the soft-stop and hard-cancel paths respectively.
  // Only one streaming session per kernel instance in v0.1.
  // ---------------------------------------------------------------

  /// Open a streaming session on the bound execution backend. Valid from
  /// kIdle. Transitions the kernel to kStreaming on accept.
  StreamingSessionBeginResultDTO begin_streaming_session(
      const StreamingSessionConfigDTO& config);

  /// Update the active streaming session's current target. Latest-wins
  /// coalescing applies at the backend; this call MUST NOT block on motion.
  StreamingPushResultDTO push_streaming_target(
      uint64_t session_id,
      const StreamingTargetDTO& target);

  /// End the active streaming session. The backend runs the soft-stop
  /// trajectory; the kernel transitions kStreaming -> kStopping -> kStopped.
  StreamingSessionEndResultDTO end_streaming_session(
      uint64_t session_id,
      SoftStopStrategy strategy);

  /// ADR-0018: update the runtime velocity scale for active execution.
  ///
  /// The HMI velocity slider drives this through cycle_manager → kernel →
  /// backend. Backends retime discrete trajectory execution and streaming
  /// sessions continuously; changing the scale never stops an action by itself.
  /// `scale` is validated finite and clamped to [0, 1]. NOT serialized through
  /// lifecycle_mutex_: this is a high-rate operator surface and the backend's
  /// atomic store handles concurrency.
  void set_runtime_velocity_scale(double scale);

  /// Compatibility alias for older callers.
  void set_streaming_velocity_scale(double scale);

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
  /// collision checks once Phase 3A is active.
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

  /// Run the validation coordinator predicted-path monitor (Chunk 6
  /// diagnostic mode) against the current planning context. Returns the
  /// decision bundle; emits a `kCollisionDetected` diagnostic event when
  /// the decision is not kPass. The kernel does not stop or fault on this
  /// path: production runtime-fault behavior remains gated on the future
  /// interrupt model. Pass nullopt for `predicted_state` to use the kernel's
  /// current state.
  CollisionValidationDecisionBundle
  run_predicted_path_monitor_diagnostic(
      const std::optional<RobotStateDTO>& predicted_state = std::nullopt);

  // ---------------------------------------------------------------
  // Kernel-native interrupt registration and latch surface (spec/260,
  // sprint Chunk I-2). This chunk owns source/policy registration, signal
  // ingestion, priority arbitration, latch visibility, disposition validation,
  // and event emission. Phase-specific cancellation / execution preemption is
  // wired in later Phase 3 chunks.
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
  StreamingSessionEndResultDTO end_streaming_session_unlocked(
      uint64_t session_id,
      SoftStopStrategy strategy);
  Result<TrajectoryDTO> plan_trajectory(const ExecuteRequestDTO& request);
  Result<CollisionWorldRevision> validate_trajectory(
      const TrajectoryDTO& trajectory,
      const ExecuteRequestDTO& request);
  Result<CollisionWorldRevision> validate_streaming_target_collision(
      const StreamingSessionConfigDTO& active_config,
      const StreamingTargetDTO& resolved_target,
      std::uint64_t validation_sequence,
      std::uint64_t session_id,
      std::uint64_t source_sequence);
  TrajectoryDTO build_remaining_trajectory(
      const TrajectoryDTO& trajectory,
      const ExecutionSnapshot& snapshot) const;
  std::vector<double> interpolate_joint_positions(
      const TrajectoryDTO& trajectory,
      double time_s) const;

  /// Async execution worker function.
  void async_execute_worker(std::stop_token st,
                            ExecuteRequestDTO request,
                            uint64_t exec_id,
                            std::optional<TrajectoryDTO> cached_trajectory);

  /// Emit an event to all subscribers.
  void emit_event(const KernelEvent& event);

  /// Publish an observer-safe execution snapshot.
  void publish_execution_snapshot(ExecutionSnapshot snapshot);
  void start_collision_monitoring();
  void stop_collision_monitoring();
  void schedule_next_collision_monitor_tick(uint64_t generation);
  CollisionWorldRevision current_authoritative_collision_world_revision() const;
  std::shared_ptr<CollisionCancellationToken> install_validation_cancel_token();
  void clear_validation_cancel_token(
      const std::shared_ptr<CollisionCancellationToken>& token);
  void request_validation_cancel_from_interrupt(
      const InterruptIngestResult& result);
  void mark_active_interrupt_preemption_result(
      InterruptPreemptionResult result);
  std::shared_ptr<CollisionCancellationToken>
  install_streaming_target_cancel_token(std::uint64_t validation_sequence);
  void clear_streaming_target_cancel_token(
      const std::shared_ptr<CollisionCancellationToken>& token);
  void cancel_active_streaming_target_validation(std::string reason);
  void install_builtin_interrupt_sources();
  InterruptIngestResult publish_runtime_collision_fault_interrupt(
      const CollisionCheckResultDTO& check);
  void apply_latched_interrupt_preemption(
      const InterruptIngestResult& result);
  InterruptRuntimeContext current_interrupt_context() const;
  void emit_interrupt_events(const InterruptIngestResult& result);

  IPlannerBackend& planner_;
  IPlanningContext& context_;
  IExecutionBackend& execution_backend_;
  IClock& clock_;
  IScheduler& scheduler_;
  TrajectoryCache& cache_;
  ExecutionKernelConfig config_;

  ExecutionStateMachine sm_;
  StopPolicy stop_policy_;
  CollisionPolicy collision_policy_;
  InterruptManager interrupt_manager_;
  std::atomic<std::int64_t> runtime_collision_fault_sequence_{0};

  // Authoritative collision validation pipeline (Phase 3A). Missing policy,
  // publisher, snapshot, or engines are configuration errors and fail closed;
  // the previous planner-context collision_check fallback is not a supported
  // execution path.
  mutable std::mutex coordinator_mutex_;
  std::optional<CollisionPolicySnapshot> coordinator_policy_snapshot_;
  PoseSourceRetryConfig coordinator_retry_config_{};
  std::int64_t coordinator_per_state_timeout_ns_ = 0;
  ICollisionWorldSnapshotPublisher* collision_snapshot_publisher_ = nullptr;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> collision_validation_engines_;
  mutable std::mutex validation_cancel_mutex_;
  std::weak_ptr<CollisionCancellationToken> active_validation_cancel_token_;
  mutable std::mutex streaming_collision_validation_mutex_;
  mutable std::mutex streaming_cancel_mutex_;
  std::weak_ptr<CollisionCancellationToken> active_streaming_target_cancel_token_;
  std::atomic<std::uint64_t> streaming_target_validation_sequence_{0};

  // Serializes low-rate lifecycle authority transitions across execute,
  // streaming begin/end, stop, pause, resume, reset, and acknowledge. High-rate
  // streaming target pushes intentionally do not take this lock; they are
  // protected by streaming_session_mutex_ and session generation checks.
  std::mutex lifecycle_mutex_;
  std::atomic<uint64_t> next_execution_id_{1};
  std::atomic<double> runtime_velocity_scale_{1.0};
  mutable std::mutex active_move_id_mutex_;
  std::string active_move_id_;

  // Event subscription
  std::mutex subscribers_mutex_;
  std::map<uint64_t, EventCallback> subscribers_;
  std::atomic<uint64_t> next_subscription_id_{1};

  // Observer-facing execution snapshot
  mutable std::mutex snapshot_mutex_;
  ExecutionSnapshot published_snapshot_;
  uint64_t next_snapshot_sequence_ = 1;

  // Async execution state
  std::jthread worker_thread_;
  std::condition_variable completion_cv_;
  std::mutex completion_mutex_;
  std::atomic<bool> execution_complete_{false};
  std::atomic<bool> resume_requested_{false};

  mutable std::mutex collision_monitor_mutex_;
  ScheduleHandle collision_monitor_handle_{0};
  uint64_t collision_monitor_generation_ = 0;
  bool collision_monitor_active_ = false;

  // Streaming-session bookkeeping. Only one active session at a time in
  // v0.1; `active_streaming_session_id_` is 0 when none is active.
  // `active_backend_session_id_` is the id the backend assigned in its
  // begin_streaming_session response; the kernel passes THIS id back to
  // push/end calls so kernel↔backend id mapping stays correct even if
  // the two counters diverge.
  mutable std::mutex streaming_session_mutex_;
  uint64_t next_streaming_session_id_ = 1;
  uint64_t active_streaming_session_id_ = 0;
  uint64_t active_backend_session_id_ = 0;
  uint64_t active_streaming_generation_ = 0;
  StreamingSessionConfigDTO active_streaming_config_;
  SoftStopStrategy active_soft_stop_strategy_ = SoftStopStrategy::kSpringBack;
  // Last accepted Cartesian target position (only meaningful when the
  // active session is cartesian-mode). Used to compute the implied
  // Cartesian speed between successive frames so the kernel can reject
  // frames that would exceed `max_tcp_linear_speed_mps`.
  std::array<double, 3> last_accepted_cartesian_pos_{0.0, 0.0, 0.0};
  bool last_accepted_cartesian_pos_valid_ = false;
  int64_t last_accepted_cartesian_ts_ns_ = 0;
  // Last successfully commanded full joint vector (session-order, per
  // `active_streaming_config_.joint_names`). Used to overlay partial
  // joint-mode targets onto the full vector before backend dispatch
  // — per spec/200 §2 partial-joint-map normalization rules. Seeded
  // from `context_.get_current_state()` on the first push of a
  // session; cleared at session begin/end. Updated on backend ACCEPT
  // for both cartesian (from IK result) and joint (from overlaid
  // vector) targets, so cartesian-then-joint or joint-then-joint
  // sequences within a session compose correctly.
  std::vector<double> last_commanded_joints_;
  bool last_commanded_joints_valid_ = false;

  // Metrics (atomic counters for thread safety)
  mutable std::mutex metrics_mutex_;
  mutable MetricsSnapshot metrics_;
};

}  // namespace manyforge::core
