#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/collision/collision_resource_manager.hpp>
#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/execution_correlation_tracker.hpp>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/execution_snapshot_publisher.hpp>
#include <manyforge_core/execution_worker_signals.hpp>
#include <manyforge_core/interrupt/interrupt_manager.hpp>
#include <manyforge_core/kernel_event_publisher.hpp>
#include <manyforge_core/kernel_flight_recorder.hpp>
#include <manyforge_core/motion_trip_stop_coordinator.hpp>
#include <manyforge_core/operator_stop_worker.hpp>
#include <manyforge_core/ports/icollision_trajectory_projector.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/ports/imotion_preview.hpp>
#include <manyforge_core/ports/iobserved_robot_state_provider.hpp>
#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/iplanning_scene_synchronizer.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/ports/irobot_kinematics.hpp>
#include <manyforge_core/ports/itrajectory_validator.hpp>
#include <manyforge_core/runtime_collision_lookahead_session.hpp>
#include <manyforge_core/runtime_collision_monitor.hpp>
#include <manyforge_core/runtime_collision_query_session.hpp>
#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/stop_policy.hpp>
#include <manyforge_core/terminal_committer.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/kernel_event.hpp>
#include <manyforge_core/types/metrics.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/motion_preview.hpp>
#include <manyforge_core/types/recovery.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace manyforge::core {

inline constexpr std::string_view kExecutionKernelValidationRevision =
    "manyforge.core.trajectory-validation.v1";

struct ExecutionKernelConfig {
  Duration collision_monitor_period_ns = 50'000'000;  // 50 ms / 20 Hz

  // Lower-priority, bounded future-trajectory collision checking. This never
  // replaces or borrows from the reserved current-state monitor above.
  Duration runtime_collision_lookahead_cycle_budget_ns = 200'000'000;
  Duration runtime_collision_lookahead_horizon_ns = 2'000'000'000;  // 2 s
  std::size_t runtime_collision_lookahead_max_samples = 32;
  int runtime_collision_lookahead_workers = 2;
  bool runtime_collision_lookahead_workers_declared = false;
  int runtime_collision_lookahead_min_waypoints_for_parallel = 8;
  Duration runtime_collision_lookahead_query_budget_ns = 75'000'000;

  // RT-8B recovery readiness policy (spec/230 §3.7.1). Measured-observation
  // enforcement is fail-closed before RT-8C wires the dedicated observed-state
  // publication: observation-dependent recovery remains waiting rather than
  // substituting lifecycle state. The dwell/sample values are conservative
  // placeholders and are qualified in RT-8C evidence.
  bool recovery_require_measured_observation = true;
  std::int64_t recovery_ready_dwell_ns = 200'000'000;  // 200 ms
  std::uint32_t recovery_ready_min_samples = 2;
  std::string recovery_observed_joint_order_token;
  double recovery_at_rest_velocity_threshold_rad_s = 0.01;
  Duration recovery_observation_poll_period_ns = 20'000'000;  // 20 ms

  // Hard per-tick budget for the observed runtime collision query after the
  // live collision-world snapshot has been acquired. This is intentionally
  // separate from the monitor cadence: the industrial performance target is
  // p95 <= 50% of `collision_monitor_period_ns`, but the fail-closed timeout
  // must allow contact-rich distance queries to complete within the full
  // configured monitor period before preempting execution.
  Duration runtime_collision_immediate_query_budget_ns = 50'000'000;  // 50 ms

  // Pre-dispatch trajectory collision validation may perform cold
  // collision-world snapshot synchronization before any robot motion is
  // dispatched. Keep this separate from the runtime monitor budget: the
  // runtime monitor runs while motion may be active, while pre-dispatch
  // validation is allowed a larger bounded proof window.
  Duration validation_collision_query_timeout_ns = 250'000'000;  // 250 ms

  // Wall-clock budget for one authoritative pre-dispatch collision validation
  // attempt, including worker scheduling and all per-waypoint engine queries.
  // This is deliberately distinct from `validation_collision_query_timeout_ns`:
  // that field is a per-query budget handed to engines, while this field is
  // the caller liveness bound that lets stop/reset/intervention avoid waiting
  // forever on a wedged validation worker. Must be > 0.
  Duration validation_collision_wall_timeout_ns = 5'000'000'000;  // 5 s

  // Maximum age of the observed-scene snapshot lease captured at execute
  // admission. The lease bounds how long validation may tolerate live
  // pose-source revisions diverging from the immutable world+policy bundle
  // actually used for planning/validation; structural and policy divergence
  // still fail closed immediately.
  Duration scene_snapshot_lease_max_age_ns = 5'000'000'000;  // 5 s

  // Default bounded drift tolerated for request-target observed objects during
  // one lease window. Composer/deployment metadata can refine roles; the
  // kernel default is deliberately small and applies only to target pose drift,
  // never to topology, shape, attachment, support-contact, or policy changes.
  double scene_snapshot_default_observed_drift_tolerance_m = 0.02;
  double scene_snapshot_default_observed_angular_tolerance_rad = 0.17453292519943295;  // 10 deg

  // When true, a collision-validation rejection re-runs the failing waypoint
  // through the dedicated non-authoritative diagnostic comparator and appends
  // `collision_diagnostic_compare_*` engine-parity keys to the error details.
  // Default OFF: the duplicate check costs a full collision query on every
  // rejection and the parity evidence belongs to qualification runs, not
  // production rejections.
  bool enable_collision_diagnostic_compare = false;

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
  // production default pending target qualification. A deployment declaration
  // is authoritative and validated in [1, 64]. The environment override is a
  // development/CI input only and is considered solely when no deployment
  // value was declared.
  int validation_collision_workers = 4;
  bool validation_collision_workers_declared = false;
  int validation_collision_min_waypoints_for_parallel = 8;

  // Deployment-configured RRT planning budget ceiling. Per-request
  // `planning_timeout_s` can lower this budget; deployments that need longer
  // planning windows must raise this value explicitly so the authority surface
  // is visible in the deployment artifact.
  double planner_max_planning_time_s = 5.0;

  // Optional deployment-side planner RNG seed. This is a reproducibility
  // input for stochastic candidate producers, not part of portable program
  // intent and not a safety authority. Backends that can honor it must reset
  // their per-request RNG from this value; unsupported backends must reject a
  // declared seed rather than silently ignoring it.
  std::optional<std::uint32_t> planner_rng_seed;

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
  // not authored/observed-state provider sample. A backend snapshot older than this
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

  // Transitional deployment stop-confirmation tolerance. Until the complete
  // retained participant Stop plan supplies the one outer deadline, the
  // effective envelope is requested/default deceleration plus this tolerance.
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

  // The maintained addressed runtime owns the sole public Stop transaction.
  // In that composition the subordinate still detects and publishes runtime
  // collision/interrupt truth, but it MUST NOT call a backend Stop surface;
  // ResourceAddressedExecutionKernel accepts the exact scoped Stop instead.
  // Standalone, not-yet-restored lanes retain their historical local
  // preemption behavior until their later product gate.
  bool runtime_collision_stop_owned_by_addressed_kernel = false;

  /// Exact implementation revision of the Kernel-owned validation pipeline
  /// recorded in every prepared motion participant. A changed pipeline
  /// contract rotates this value; operation callers never supply it.
  ValidationRevision validation_revision{std::string(kExecutionKernelValidationRevision)};
};

enum class RuntimeCollisionMonitorRelease {
  kTerminalQuiesced,
  kResetOrTeardown,
  kFailClosedTerminal,
  kWorkerRetirement,
};

struct RuntimeCollisionProtectionSnapshot {
  bool active = false;
  std::uint64_t generation = 0;
  std::uint64_t execution_id = 0;
  ExecutionState state_at_acquire = ExecutionState::kIdle;
  std::string reason;
};

/// Focused, non-owning kernel dependencies for planning admission and
/// validation. Each reference has one authority; no member is a combined
/// scene/state/planner facade. Owners must outlive ExecutionKernel.
struct ExecutionKernelPorts {
  IObservedRobotStateProvider& observed_state;
  IRobotKinematics& kinematics;
  IReferencePoseResolver& reference_resolver;
  ITrajectoryValidator& trajectory_validator;
  ICollisionTrajectoryProjector& collision_projector;
  const ICollisionDiagnosticComparator* collision_diagnostic_comparator = nullptr;
  IGripperCommandBackend* gripper_command_backend = nullptr;
  IPlanningSceneSynchronizer* planning_scene_synchronizer = nullptr;
};

/// Execution Kernel — single authority for motion execution governance.
/// Ties together: state machine, stop/collision policies, trajectory cache,
/// planner backend, observed-state provider, and execution backend.
///
/// Spec 230: all operations are ROS-free. This is an async v0.1
/// implementation with event subscription.
class ExecutionKernel : public IResolvedParticipantExecutionEngine,
                        public IResolvedPreparedParticipantExecutionEngine,
                        public IMotionPreview {
 public:
  ExecutionKernel(IPlannerBackend& planner, ExecutionKernelPorts ports,
                  IExecutionBackend& execution_backend, IClock& clock, IScheduler& scheduler,
                  TrajectoryCache& cache, ExecutionKernelConfig config = {});

  ~ExecutionKernel() override;

  /// Execute a motion request. Only valid from Idle.
  /// Returns execution_id immediately; plan → validate → dispatch runs
  /// on a worker thread. Callers observe progress via get_status() and events.
  Result<uint64_t> execute(const ExecuteRequestDTO& request) override;

  Result<AddressedParticipantStopSettlementDTO> settle_addressed_participant_stop(
      const AddressedParticipantStopSettlementRequestDTO& request) override;

  Result<std::uint64_t> PrepareMotion(ResolvedPreparedMotionExecutionRequestDTO request) override;
  Result<ResolvedPreparedMotionExecutionSnapshotDTO> ObservePreparedMotion(
      std::uint64_t execution_id) const override;
  Result<void> DispatchPreparedMotion(std::uint64_t execution_id,
                                      const ContentDigest& prepared_contract_digest) override;
  Result<void> PrepareResumeMotion(std::uint64_t execution_id,
                                   ResolvedPreparedMotionResumeRequestDTO request) override;
  Result<void> DispatchPreparedResumeMotion(std::uint64_t execution_id,
                                            const ContentDigest& prepared_contract_digest) override;
  Result<void> CancelPreparedResumeMotion(std::uint64_t execution_id) override;
  Result<void> CancelPreparedMotion(std::uint64_t execution_id) override;
  Result<std::uint64_t> PrepareCapabilityCommand(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO request) override;
  Result<ResolvedPreparedCapabilityCommandSnapshotDTO> ObservePreparedCapabilityCommand(
      std::uint64_t preparation_id) const override;
  Result<GripperCommandResultDTO> DispatchPreparedCapabilityCommand(
      std::uint64_t preparation_id, const ContentDigest& prepared_contract_digest,
      const ControllerGoalId& controller_goal_id,
      ControllerGoalGeneration controller_goal_generation) override;
  Result<void> CancelPreparedCapabilityCommand(std::uint64_t preparation_id) override;

  /// Plan and authoritatively validate one already-resolved motion without
  /// dispatching, journaling, caching, publishing execution state, or
  /// mutating the retained scene. The caller supplies both the exact start
  /// state and retained projection so chained candidate legs cannot reread a
  /// newer mutable state or scene between phases.
  ///
  /// Preview is admitted only while this participant is Idle and is
  /// serialized by the ordinary lifecycle boundary. Cancellation requests
  /// the same planner and collision-validation cancellation paths used by
  /// execution.
  Result<RobotStateDTO> current_preview_state(std::int64_t now_ns) const override;

  Result<std::vector<Pose3DTO>> resolve_preview_reference_poses(
      ExecuteRequestDTO request, std::vector<ReferenceDTO> references,
      const CollisionWorldLeaseInventory& projection_inventory,
      const RobotStateDTO& start_state) override;

  Result<MotionPreviewResultDTO> preview_motion(ExecuteRequestDTO request,
                                                CollisionWorldLeaseInventory projection_inventory,
                                                RobotStateDTO start_state,
                                                std::uint32_t planning_attempt_index = 0U,
                                                std::stop_token stop_token = {}) override;

  /// Non-blocking stop from any non-terminal state.
  /// Accepted stop invalidates older lifecycle/recovery commands by bumping
  /// `control_command_epoch()`. Stop is never rejectable by epoch staleness.
  StopResultDTO stop();

  /// Non-blocking pause. Only valid from Executing.
  PauseResultDTO pause() override;

  /// Non-blocking resume. Only valid from Paused.
  ResumeResultDTO resume(std::optional<std::uint64_t> expected_epoch = std::nullopt) override;

  /// Non-blocking reset. Valid from any state except Idle.
  ResetResultDTO reset(std::optional<std::uint64_t> expected_epoch = std::nullopt) override;
  ResetResultDTO reset_after_certified_containment(
      std::uint64_t execution_id,
      std::optional<std::uint64_t> expected_epoch = std::nullopt) override;

  /// Backend-neutral gripper command surface. The injected adapter owns all
  /// progression and publishes immutable observer status.
  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override;
  Result<void> validate_gripper_command(const GripperCommandRequestDTO& request) const override;
  GripperCommandStatusDTO get_gripper_status(const std::string& gripper_id) const override;
  Result<std::uint64_t> stop_gripper(const std::string& gripper_id,
                                     std::uint64_t expected_command_id,
                                     std::uint64_t expected_generation) override;
  Result<void> reset_gripper(const std::string& gripper_id, std::uint64_t expected_command_id,
                             std::uint64_t expected_generation) override;
  GripperCommandResultDTO mark_gripper_grasp_lost(const std::string& gripper_id,
                                                  std::uint64_t expected_command_id,
                                                  std::uint64_t expected_generation,
                                                  const std::string& evidence_label,
                                                  const std::string& message,
                                                  const std::string& reason_code);

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
      uint64_t execution_id, std::optional<std::uint64_t> expected_epoch = std::nullopt) override;

  /// RT-8B (spec/200, spec/230 §3.7.1): the only kernel statement of
  /// recovery admissibility. Pure bounded read of the latest published
  /// assessment revision.
  KernelRecoveryAssessmentDTO get_recovery_assessment() const;

  /// RT-8B single generation-bound recovery command. Holds the lifecycle
  /// admission boundary across the coherent re-check and interrupt
  /// disposition; rejections are side-effect-free and return the newest
  /// assessment. `reset_cycle` is deliberately not an action here.
  KernelRecoveryActionResultDTO apply_recovery_action(
      const KernelRecoveryActionRequestDTO& request);

  /// Bounded copy of the stop-certificate evidence ring (tests/diagnostics).
  std::vector<KernelStopCertificateDTO> stop_certificate_evidence() const;

  /// ADR-0018: update the runtime velocity scale for active execution.
  ///
  /// The UI velocity slider drives this through cycle_manager → kernel →
  /// backend. Backends retime discrete trajectory execution continuously;
  /// changing the scale never stops an action by itself. `scale` is validated
  /// finite and clamped to [0, 1]. NOT serialized through lifecycle_mutex_:
  /// this is a high-rate operator surface and the backend's atomic store
  /// handles concurrency. `principal_id` is optional because lower-level
  /// backend/direct callers may not have a supervisory actor identity.
  void set_runtime_velocity_scale(double scale,
                                  std::optional<std::string> principal_id = std::nullopt);

  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO& command) override;
  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId& operation_id, const ControlBindingId& control_binding_id) const override;

  /// Get current kernel status.
  ExecutionStatus get_status() const override;

  /// Get the latest observer-safe execution snapshot published by the kernel.
  ExecutionSnapshot get_execution_snapshot() const;

  /// Explicitly ask the execution adapter to publish its current observed
  /// state, then refresh the observer-facing kernel snapshot. Intended for
  /// simulator/bootstrap commands only; status reads never call it.
  Result<void> refresh_observed_state_snapshot();

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
  void set_collision_validation_policy_snapshot(std::optional<CollisionPolicySnapshot> snapshot);

  /// Install role-owned collision resources. Production composition roots should
  /// use this API: runtime monitoring owns `runtime_monitor_engine`, while
  /// authoritative validation owns `validation_engines`.
  void set_collision_resources(
      ICollisionWorldSnapshotPublisher& publisher,
      std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> runtime_lookahead_engines = {});

  /// Optional pose-source / per-state retry configuration for the
  /// CollisionValidationCoordinator. Has no effect until a policy snapshot is
  /// installed via set_collision_validation_policy_snapshot.
  void set_collision_validation_retry_config(PoseSourceRetryConfig retry,
                                             std::int64_t per_state_timeout_ns = 0);

  /// Check whether a non-dispatch validation surface may run against the
  /// current collision resources. Preview callers use this before direct
  /// planner/context work so they defer loudly instead of borrowing a runtime
  /// monitor resource.
  std::optional<ErrorDTO> collision_validation_admission_error() const;

  // ---------------------------------------------------------------
  // Kernel-native interrupt registration and latch surface (spec/260,
  // sprint Chunk I-2). This chunk owns source/policy registration, signal
  // ingestion, priority arbitration, latch visibility, disposition validation,
  // and event emission.
  // ---------------------------------------------------------------

  Result<void> register_interrupt_source(const InterruptSourceDefinition& source);
  Result<void> register_interrupt_policy(const InterruptPolicyDefinition& policy);
  InterruptIngestResult publish_fast_interrupt_signal(const InterruptSignalUpdate& update);
  InterruptIngestResult publish_supervisory_interrupt_signal(const InterruptSignalUpdate& update);
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
  std::optional<MotionExecutionOutcomeDTO> get_execution_outcome(std::uint64_t execution_id) const;

  /// Diagnostics-only observer for armed move-scoped bindings
  /// (spec/260 §B.3). BT/policy code MUST NOT poll this for control flow.
  std::vector<MotionInterruptScopeStatusDTO> active_motion_interrupt_scopes() const;

  /// Subscribe to kernel events.
  /// @param callback Called for each event. Must be thread-safe.
  /// @return Handle for unsubscribing.
  SubscriptionHandle subscribe_events(EventCallback callback) override;

  /// Unsubscribe from kernel events.
  void unsubscribe_events(SubscriptionHandle handle) override;

  /// Block until the current execution completes (reaches terminal state).
  /// Useful for tests and synchronous callers.
  /// @param timeout_ns Maximum time to wait (0 = wait forever).
  /// @return true if execution completed, false if timed out.
  bool wait_for_completion(int64_t timeout_ns = 0);

  /// Get current metrics snapshot.
  MetricsSnapshot get_metrics() const;

  /// Configure where the in-process flight ring is dumped when the kernel
  /// dies fail-closed (empty = stderr). Startup wiring, not a hot path.
  void set_flight_recorder_directory(std::string directory);

  /// Best-effort JSONL dump of the flight ring (the fail-closed teardown
  /// paths call this automatically; exposed for bindings, diagnostics, and
  /// tests). Returns the written path, or empty when the dump went to
  /// stderr.
  std::string dump_flight_record(const char* reason) noexcept;

 private:
  struct PreparedMotionSession;
  struct PreparedCapabilityCommandSession;
  struct PlanningSuccess;
  struct ExecuteWorkerPlanPhaseResult;
  struct ExecuteWorkerValidationPhaseResult;

  Result<std::uint64_t> execute_impl(const ExecuteRequestDTO& request,
                                     std::shared_ptr<PreparedMotionSession> prepared_session);
  Result<PlanRequestDTO> make_plan_request(const ExecuteRequestDTO& request,
                                           const CollisionWorldLeaseInventory& projection_inventory,
                                           std::uint32_t planning_attempt_index,
                                           const RobotStateDTO* start_state_override = nullptr);
  Result<PlanningSuccess> plan_trajectory_with_evidence(
      const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
      std::uint32_t planning_attempt_index = 0,
      const RobotStateDTO* start_state_override = nullptr);
  Result<TrajectoryDTO> plan_trajectory(const ExecuteRequestDTO& request,
                                        const CollisionWorldLeaseInventory& projection_inventory,
                                        std::uint32_t planning_attempt_index = 0,
                                        const RobotStateDTO* start_state_override = nullptr);
  Result<TrajectoryDTO> plan_resume_trajectory(
      const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory);
  Result<PlanningSuccess> plan_resume_trajectory_with_evidence(
      const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory);
  Result<std::size_t> discover_dynamic_carried_start_contacts(
      CollisionWorldLeaseInventory& projection_inventory, const RobotStateDTO& observed_start,
      std::uint64_t execution_id);

  /// Move-only ownership of one retained scene/projection revision. Destruction
  /// releases the publisher lookup entry on every success, rejection,
  /// cancellation, exception, and worker-start race; immutable snapshot handles
  /// already copied out of the inventory remain valid by contract.
  class RetainedSceneSnapshotLease {
   public:
    RetainedSceneSnapshotLease() = default;
    RetainedSceneSnapshotLease(ICollisionWorldSnapshotPublisher& publisher,
                               CollisionWorldLeaseInventory inventory);
    ~RetainedSceneSnapshotLease();
    RetainedSceneSnapshotLease(const RetainedSceneSnapshotLease&) = delete;
    RetainedSceneSnapshotLease& operator=(const RetainedSceneSnapshotLease&) = delete;
    RetainedSceneSnapshotLease(RetainedSceneSnapshotLease&& other) noexcept;
    RetainedSceneSnapshotLease& operator=(RetainedSceneSnapshotLease&& other) noexcept;

    [[nodiscard]] const CollisionWorldLeaseInventory& inventory() const { return inventory_; }
    [[nodiscard]] CollisionWorldLeaseInventory& mutable_inventory() { return inventory_; }
    [[nodiscard]] ProjectionLeaseBindingDTO binding() const;

   private:
    void release() noexcept;
    ICollisionWorldSnapshotPublisher* publisher_ = nullptr;
    CollisionWorldLeaseInventory inventory_;
  };

  struct CollisionValidationSuccess {
    CollisionWorldRevision collision_world_revision;
    CollisionPolicyRevision collision_policy_revision;
    ValidationResultDTO noncollision_result;
    CollisionDecision collision_decision;
    TrajectoryDTO collision_trajectory;
    std::optional<SceneSnapshotLease> scene_snapshot_lease;
    ParallelValidationTelemetry telemetry;
    std::size_t dynamically_discovered_start_contact_count = 0;
  };

  Result<CollisionValidationSuccess> validate_trajectory(
      const TrajectoryDTO& trajectory, const ExecuteRequestDTO& request,
      CollisionWorldLeaseInventory& scene_snapshot_lease_inventory,
      const std::optional<SceneSnapshotLease>& scene_snapshot_lease,
      const RobotStateDTO* start_state_override = nullptr,
      std::stop_token external_stop_token = {});

  /// Async execution worker function.
  void async_execute_worker(std::stop_token st, ExecuteRequestDTO request, uint64_t exec_id,
                            std::optional<TrajectoryDTO> cached_trajectory,
                            bool cache_revalidation_candidate,
                            RetainedSceneSnapshotLease retained_scene,
                            std::optional<SceneSnapshotLease> scene_snapshot_lease,
                            std::shared_ptr<PreparedMotionSession> prepared_session = {});

  Result<std::shared_ptr<const PreparedOperationParticipantDTO>>
  make_and_prepare_motion_participant(
      const ExecuteRequestDTO& request, const TrajectoryDTO& trajectory,
      const ExecuteWorkerPlanPhaseResult& plan_phase,
      const ExecuteWorkerValidationPhaseResult& validation_phase,
      const std::shared_ptr<PreparedMotionSession>& prepared_session);

  /// Emit an event to all subscribers.
  void emit_event(KernelEvent event);
  void emit_event_immediate(KernelEvent event);
  void prepare_event(KernelEvent& event) const;
  void dispatch_prepared_event(KernelEvent& event);
  class EventDeferralScope;
  static thread_local EventDeferralScope* active_event_deferral_;
  void set_active_request_correlation(const ExecuteRequestDTO& request);
  void clear_active_request_correlation();
  void stamp_active_request_correlation(std::map<std::string, std::string>& details) const;
  void stamp_active_request_correlation(ExecutionSnapshot& snapshot) const;

  /// K3: route a terminal decision through the TerminalCommitter.
  /// Returns the outcome; `outcome.committed` is the state that must
  /// appear in any snapshot/event published for this commit.
  TerminalCommitter::CommitOutcome commit_terminal(
      ExecutionState proposed, const char* reason,
      TerminalCommitter::Origin origin = TerminalCommitter::Origin::kGeneric);

  /// Publish an observer-safe execution snapshot.
  void publish_execution_snapshot(ExecutionSnapshot snapshot);
  enum class RuntimeCollisionProtectionPurpose {
    kMotion,
    kControlledStop,
  };
  std::uint64_t start_collision_monitoring(
      std::string reason,
      RuntimeCollisionProtectionPurpose purpose = RuntimeCollisionProtectionPurpose::kMotion,
      const CollisionWorldLeaseInventory* admission_inventory = nullptr);
  void stop_collision_monitoring(RuntimeCollisionMonitorRelease release,
                                 std::optional<std::uint64_t> expected_generation = std::nullopt,
                                 bool drain_callbacks = false);
  void arm_runtime_collision_lookahead(std::uint64_t execution_id,
                                       std::uint64_t execution_generation,
                                       std::shared_ptr<const TrajectoryDTO> collision_trajectory,
                                       const std::string& contact_policy_id,
                                       const CollisionWorldLeaseInventory& admission_inventory);
  void revoke_runtime_collision_lookahead_clear_permits() noexcept;
  void apply_runtime_collision_lookahead_result(
      RuntimeCollisionLookaheadResult result,
      const RuntimeCollisionLookaheadSession* source_session = nullptr);
  RuntimeCollisionLookaheadAuthorityDisposition apply_runtime_collision_lookahead_authority(
      RuntimeCollisionLookaheadResult& result,
      const RuntimeCollisionLookaheadSession* source_session);
  void emit_runtime_collision_lookahead_evidence(RuntimeCollisionLookaheadResult result);
  void emit_contact_policy_violation_event(
      std::uint64_t execution_id, const std::map<std::string, std::string>& source_details,
      std::optional<std::string_view> response_override = std::nullopt);
  RuntimeCollisionProtectionSnapshot runtime_collision_protection_snapshot() const;
  bool refresh_published_runtime_collision_snapshot(
      std::map<std::string, std::string>* details = nullptr,
      RuntimeCollisionQueryBundlePtr* bundle = nullptr);
  void set_runtime_collision_state_projection(RuntimeCollisionStateProjection projection);
  void clear_runtime_collision_state_projection();
  void clear_runtime_collision_state_projection_unlocked();
  CollisionWorldRevision current_authoritative_collision_world_revision() const;
  Result<RetainedSceneSnapshotLease> retain_current_scene_snapshot_lease_inventory(
      std::uint64_t exec_id, const std::string& move_id);
  Result<std::optional<ProjectionRevisionDTO>> synchronize_scene_before_planning();
  std::optional<SceneSnapshotLease> build_scene_snapshot_lease(
      const CollisionWorldLeaseInventory& inventory, const ExecuteRequestDTO& request,
      std::uint64_t exec_id) const;
  std::optional<SceneSnapshotLeaseValidationContext>
  current_scene_snapshot_lease_validation_context() const;
  std::shared_ptr<CollisionCancellationToken> install_validation_cancel_token();
  void clear_validation_cancel_token(const std::shared_ptr<CollisionCancellationToken>& token);
  void request_validation_cancel_from_interrupt(const InterruptIngestResult& result);
  void request_planning_cancel_from_interrupt(const InterruptIngestResult& result);
  void request_active_validation_cancel(std::string reason);
  void request_active_planning_cancel();
  void record_terminal_fault_authority(const InterruptIngestResult& result);
  void mark_active_interrupt_preemption_result(
      InterruptPreemptionResult result,
      const std::map<std::string, std::string>& extra_details = {});
  void mark_interrupt_preemption_result(
      const InterruptLatch& latch, InterruptPreemptionResult result,
      const std::map<std::string, std::string>& extra_details = {});
  bool active_trajectory_preempt_pending() const;
  void apply_interrupt_ingest_effects(const InterruptIngestResult& result,
                                      bool apply_preemption_inline);
  void install_builtin_interrupt_sources();
  InterruptIngestResult publish_runtime_collision_fault_interrupt(
      const CollisionCheckResultDTO& check,
      const std::map<std::string, std::string>& metadata = {});
  InterruptIngestResult publish_fast_interrupt_signals_internal(
      const std::vector<InterruptSignalUpdate>& updates, bool apply_preemption_inline);
  void schedule_latched_interrupt_preemption(InterruptIngestResult result);
  void apply_latched_interrupt_preemption(const InterruptIngestResult& result,
                                          std::stop_token worker_stop_token = {});
  bool request_fault_preempt_controlled_stop();
  bool fault_preempt_stop_active() const;
  std::uint64_t fault_preempt_stop_generation() const;

  // RT-8B internals.
  RecoveryAuthorityCapture capture_recovery_authority_locked();
  Result<void> apply_acknowledge_and_continue_locked(const std::string& principal_id,
                                                     const std::string& reason);
  Result<void> apply_retry_after_scene_repair_locked(const std::string& principal_id,
                                                     const std::string& reason);
  RecoveryReadinessPolicy recovery_readiness_policy() const;
  KernelRecoveryAssessmentDTO refresh_recovery_assessment_locked(const char* trigger);
  KernelRecoveryAssessmentDTO publish_recovery_assessment_locked(RecoveryAuthorityCapture capture,
                                                                 const char* trigger);
  KernelRecoveryAssessmentDTO publish_terminal_recovery_assessment_locked(
      ExecutionState terminal_state, const char* trigger);
  void try_refresh_recovery_assessment(const char* trigger);
  void schedule_recovery_assessment_refresh(const char* trigger) noexcept;
  bool publish_stop_certificate(KernelStopCertificateDTO certificate,
                                bool coalesce_owner_session = false);
  std::uint64_t begin_operator_stop_owner_generation(std::uint64_t execution_id);
  std::uint64_t ensure_operator_stop_owner_generation(std::uint64_t execution_id);
  std::uint64_t operator_stop_owner_generation_for_execution(std::uint64_t execution_id) const;
  void publish_operator_stop_certificate(std::uint64_t execution_id,
                                         StopCertificateOutcome completion_result,
                                         bool backend_stop_confirmed, std::string command_source,
                                         std::string requested_stop_mode = "hard_stop",
                                         std::string executed_stop_mode = "hard_stop");
  void schedule_recovery_freshness_deadline_locked(const KernelRecoveryAssessmentDTO& assessment);
  void schedule_recovery_observation_poll_locked(const KernelRecoveryAssessmentDTO& assessment);
  KernelRecoveryActionResultDTO apply_recovery_action_impl(KernelRecoveryActionRequestDTO request);
  bool signal_fault_preempt_stop_path_collision(std::map<std::string, std::string> details);
  std::optional<std::map<std::string, std::string>> fault_preempt_stop_path_collision_details()
      const;
  void clear_fault_preempt_stop_path_collision();
  void set_fault_preempt_intervention_error(ErrorDTO error);
  std::optional<ErrorDTO> fault_preempt_intervention_error_for_active_latch() const;
  void clear_fault_preempt_intervention_error();
  bool request_operator_stop_controlled_stop();
  bool signal_operator_stop_path_collision(std::map<std::string, std::string> details);
  std::optional<std::map<std::string, std::string>> operator_stop_path_collision_details() const;
  bool control_command_epoch_matches(std::optional<std::uint64_t> expected_epoch) const;
  InterruptRuntimeContext current_interrupt_context() const;
  void stamp_interrupt_latch_collision_world_revision(InterruptIngestResult& result);
  void emit_interrupt_events(const InterruptIngestResult& result);

  /// Operator-stop worker lifecycle. Operator stops can run a
  /// deployment-configured controlled-stop profile; that backend call may take
  /// the soft-stop deceleration window. Keep the API admission path short and
  /// run the long backend work on this cancellable worker instead.
  bool cancel_operator_stop_worker(bool invalidate_generation);
  void join_finished_operator_stop_worker();
  bool wait_for_operator_stop_worker(std::chrono::steady_clock::duration timeout);
  bool start_operator_stop_worker(MotionInterruptStopProfileDTO profile,
                                  ExecutionState pre_completion_state,
                                  std::optional<std::uint64_t> runtime_collision_generation,
                                  std::uint64_t stop_owner_generation);
  void run_operator_stop_worker(std::uint64_t generation, std::uint64_t stop_owner_generation,
                                MotionInterruptStopProfileDTO profile,
                                ExecutionState pre_completion_state,
                                std::optional<std::uint64_t> runtime_collision_generation,
                                std::stop_token stop_token);
  bool operator_stop_worker_is_current(std::uint64_t generation, std::stop_token stop_token) const;
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
    std::uint64_t runtime_collision_protection_generation = 0;
    std::uint64_t execution_progress_generation = 0;
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
    std::optional<PlanRequestDTO> plan_request;
    std::optional<PlanResultDTO> planning_result;
    bool active_trajectory_from_cache = false;
    bool cache_revalidation_candidate = false;
  };

  struct ExecuteWorkerValidationPhaseResult {
    ExecuteWorkerPhaseStatus status = ExecuteWorkerPhaseStatus::kContinue;
    CollisionWorldRevision collision_world_revision;
    std::optional<SceneSnapshotLease> scene_snapshot_lease;
    std::shared_ptr<const TrajectoryDTO> collision_trajectory;
    std::optional<PreparedMotionValidationEvidenceDTO> validation_evidence;
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
  bool retire_execute_worker(std::unique_lock<std::mutex>& lifecycle_lock,
                             bool containment_already_certified = false);

  void invalidate_gripper_authority(GripperInvalidationReason reason) noexcept;
  void authorize_gripper_authority() noexcept;

  bool finalize_overridden_terminal_authority(const ExecuteRequestDTO& request,
                                              std::uint64_t exec_id,
                                              ActiveMotionExecution& motion_state,
                                              ExecutionState committed, const char* reason,
                                              std::optional<ErrorDTO> error = std::nullopt);

  /// Convert an exact pre-dispatch cancellation into the participant's local
  /// Stopped terminal before a cancelled planner/validator can be mistaken for
  /// an execution failure. Returns true only after terminal outcome ownership
  /// has been completed by this cancellation or a stronger terminal authority.
  bool finalize_prepared_motion_cancellation_if_requested(
      const std::shared_ptr<PreparedMotionSession>& prepared_session,
      const ExecuteRequestDTO& request, std::uint64_t exec_id, ActiveMotionExecution& motion_state,
      const char* reason);

  bool finalize_competing_terminal_authority(const ExecuteRequestDTO& request,
                                             std::uint64_t exec_id,
                                             ActiveMotionExecution& motion_state,
                                             const char* reason);

  ExecuteWorkerPlanPhaseResult execute_worker_plan_phase(
      std::stop_token st, const ExecuteRequestDTO& request,
      const CollisionWorldLeaseInventory& projection_inventory, std::uint64_t exec_id,
      std::optional<TrajectoryDTO> cached_trajectory, bool cache_revalidation_candidate,
      ExecutionState worker_entry_state, ActiveMotionExecution& motion_state,
      const std::shared_ptr<PreparedMotionSession>& prepared_session);

  ExecuteWorkerValidationPhaseResult execute_worker_validate_replan_phase(
      std::stop_token st, const ExecuteRequestDTO& request, std::uint64_t exec_id,
      TrajectoryDTO& trajectory, std::optional<PlanRequestDTO>& plan_request,
      std::optional<PlanResultDTO>& planning_result, bool& active_trajectory_from_cache,
      bool& cache_revalidation_candidate,
      CollisionWorldLeaseInventory& scene_snapshot_lease_inventory,
      const std::optional<SceneSnapshotLease>& scene_snapshot_lease,
      ActiveMotionExecution& motion_state,
      const std::shared_ptr<PreparedMotionSession>& prepared_session);

  ExecuteWorkerDispatchPhaseResult execute_worker_dispatch_phase(
      const ExecuteRequestDTO& request, std::uint64_t exec_id, const TrajectoryDTO& trajectory,
      bool active_trajectory_from_cache, bool cache_revalidation_candidate,
      const ProjectionLeaseBindingDTO& projection_binding,
      const CollisionWorldLeaseInventory& scene_snapshot_lease_inventory,
      const CollisionWorldRevision& validation_world_revision,
      const std::optional<SceneSnapshotLease>& scene_snapshot_lease,
      std::shared_ptr<const TrajectoryDTO> collision_trajectory,
      std::shared_ptr<const PreparedOperationParticipantDTO> prepared_participant,
      ActiveMotionExecution& motion_state);

  void execute_worker_finalize_phase(const ExecuteRequestDTO& request, std::uint64_t exec_id,
                                     ExecutionHandle& execution_handle,
                                     ActiveMotionExecution& motion_state);

  /// Arm scopes carried by `request.motion_interrupt_scopes` atomically
  /// with dispatch (spec/230 §3.4): all scopes succeed or none do. On
  /// success, populates `motion_state.had_scopes` /
  /// `motion_state.arm_succeeded` and emits
  /// `motion_interrupt_scope_armed` kernel events. Returns Err with the
  /// manager's failure reason; the caller fails the execution closed.
  Result<void> arm_motion_scopes_for_dispatch(const ExecuteRequestDTO& request,
                                              std::uint64_t execution_id,
                                              ActiveMotionExecution& motion_state);

  /// Build, validate, store, and emit the immutable
  /// `MotionExecutionOutcomeDTO` for `execution_id`. Always reaps any
  /// remaining armed bindings tied to `execution_id`. Safe to call from
  /// every terminal exit; subsequent calls for the same execution are
  /// no-ops so worker code can be defensive without producing duplicate
  /// records.
  void finalize_motion_execution_outcome(ActiveMotionExecution& motion_state,
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
  void handle_motion_trip_latch(const ExecuteRequestDTO& request, const InterruptLatch& latch,
                                std::uint64_t exec_id,
                                const ExecutionSnapshot& latest_backend_snapshot,
                                ActiveMotionExecution& motion_state,
                                std::stop_token worker_stop_token);

  void emit_interrupt_manager_events(const std::vector<InterruptManagerEvent>& events,
                                     std::uint64_t execution_id);

  IPlannerBackend& planner_;
  IObservedRobotStateProvider& observed_state_;
  IRobotKinematics& kinematics_;
  IReferencePoseResolver& reference_resolver_;
  ITrajectoryValidator& trajectory_validator_;
  ICollisionTrajectoryProjector& collision_projector_;
  const ICollisionDiagnosticComparator* collision_diagnostic_comparator_;
  IGripperCommandBackend* gripper_command_backend_;
  IPlanningSceneSynchronizer* planning_scene_synchronizer_;
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
  CollisionResourceManager collision_resources_;
  std::unique_ptr<ParallelCollisionValidationWorkerPool> collision_validation_worker_pool_;
  std::atomic<std::uint64_t> validation_collision_workers_effective_{0};
  mutable std::mutex runtime_collision_protection_mutex_;
  RuntimeCollisionProtectionSnapshot runtime_collision_protection_;
  std::uint64_t runtime_collision_protection_next_generation_ = 0;

  // RT-8B recovery authority state. Lock order (documented at the admission
  // site): lifecycle_mutex_ -> interrupt-manager leaf -> exactly one of
  // {recovery_certificate_mutex_, recovery_assessment_mutex_,
  //  recovery_refresh_mutex_,
  //  operator_stop_identity_mutex_, fault_preempt_stop_mutex_}; the leaf
  // locks are never nested and no
  // publisher takes lifecycle_mutex_ while holding one. The refresh scheduler
  // may take recovery_refresh_mutex_ alone before its callback later acquires
  // lifecycle_mutex_; it never holds the leaf across callback execution.
  mutable std::mutex recovery_certificate_mutex_;
  RecoveryCertificateStore recovery_certificates_;
  mutable std::mutex recovery_assessment_mutex_;
  KernelRecoveryAssessmentDTO recovery_assessment_;
  std::atomic<bool> recovery_assessment_dirty_{false};
  mutable std::mutex recovery_refresh_mutex_;
  ScheduleHandle recovery_refresh_handle_ = 0;
  bool recovery_refresh_scheduled_ = false;
  ObservedReadinessTracker recovery_readiness_tracker_;
  ScheduleHandle recovery_freshness_deadline_handle_ = 0;  // lifecycle-guarded
  std::int64_t recovery_freshness_deadline_ns_ = 0;        // lifecycle-guarded
  ScheduleHandle recovery_observation_poll_handle_ = 0;    // lifecycle-guarded
  std::uint64_t fault_preempt_stop_generation_ = 0;        // fault_preempt_stop_mutex_
  // One coherent operator-stop identity. The generation counter is global,
  // while the selected generation is bound to exactly one execution. Keeping
  // the pair under a leaf mutex prevents a capture from combining a new
  // generation with an older execution during deferred stop admission.
  mutable std::mutex operator_stop_identity_mutex_;
  StopOwnerIdentityTracker operator_stop_identity_;
  // Process-local reference to the addressed owner's aggregate Stop. This is
  // recovery evidence only; the participant never publishes a second
  // certificate for the same physical containment.
  std::optional<AddressedMotionStopRecoveryEvidenceDTO>
      addressed_motion_stop_recovery_evidence_;  // lifecycle-guarded
  // Retain the exact process-local handoff result so replaying the same
  // addressed request returns the same fact rather than resampling recovery
  // state from a later lifecycle generation.
  std::optional<AddressedParticipantStopSettlementDTO>
      addressed_participant_stop_settlement_;  // lifecycle-guarded
  std::atomic<std::uint64_t> motion_trip_stop_generation_{0};
  std::atomic<bool> operator_stop_command_active_{false};
  mutable std::mutex runtime_collision_state_projection_mutex_;
  std::optional<RuntimeCollisionStateProjection> runtime_collision_state_projection_;
  std::atomic<std::uint64_t> runtime_collision_snapshot_invalidation_epoch_{0};
  std::atomic<std::shared_ptr<RuntimeCollisionQuerySession>> runtime_collision_query_session_;
  std::atomic<std::shared_ptr<RuntimeCollisionLookaheadSession>>
      runtime_collision_lookahead_session_;
  mutable std::mutex runtime_collision_query_prepare_error_mutex_;
  std::optional<ErrorDTO> runtime_collision_query_prepare_error_;
  mutable std::mutex validation_cancel_mutex_;
  std::weak_ptr<CollisionCancellationToken> active_validation_cancel_token_;

  // Serializes low-rate lifecycle authority transitions across execute, stop,
  // pause, resume, reset, and acknowledge.
  std::mutex lifecycle_mutex_;
  std::atomic<std::uint64_t> control_command_epoch_{0};
  std::atomic<uint64_t> next_execution_id_{1};
  // Exact normal-completion event batch currently being published. A matching
  // acknowledgement is transiently rejected so it cannot overtake
  // execute_completed -> state_transition(Completed) in the native sequence.
  std::atomic<std::uint64_t> completion_publication_execution_id_{0};
  std::atomic<double> runtime_velocity_scale_{1.0};
  mutable std::mutex runtime_velocity_scale_mutex_;
  int runtime_velocity_scale_freeze_depth_ = 0;
  std::optional<double> deferred_runtime_velocity_scale_;
  std::optional<std::string> deferred_runtime_velocity_scale_principal_id_;
  ExecutionCorrelationTracker active_correlation_;
  std::mutex resolved_runtime_scale_submission_mutex_;
  mutable std::mutex resolved_runtime_scale_mutex_;
  std::optional<ResolvedRuntimeScaleCommandDTO> accepted_resolved_runtime_scale_command_;
  std::optional<ResolvedRuntimeScaleAcceptedDTO> accepted_resolved_runtime_scale_result_;
  std::uint64_t runtime_scale_acceptance_sequence_ = 0;

  // Event subscription and copy-on-write callback dispatch.
  KernelEventPublisher event_publisher_;
  // Dispatch-order event sequence (loss detection downstream); wait-free.
  std::atomic<std::uint64_t> event_sequence_{0};

  // Observer-facing execution snapshot storage and sequence owner.
  ExecutionSnapshotPublisher snapshot_publisher_;

  // Bounded in-process flight ring of recent events, dumped by the
  // fail-closed teardown paths. Telemetry only: fixed-cost recording behind
  // its own leaf mutex, never held across I/O or callbacks.
  KernelFlightRecorder flight_recorder_;

  // Async execution state
  std::jthread worker_thread_;
  std::uint64_t execute_worker_generation_ = 0;  // guarded by lifecycle_mutex_
  ExecutionWorkerSignals worker_signals_;
  std::atomic<bool> execute_admission_interruptible_{false};
  std::atomic<bool> lifecycle_stop_requested_while_busy_{false};
  std::atomic<bool> lifecycle_stop_requested_event_emitted_{false};
  std::atomic<bool> lifecycle_reset_requested_while_busy_{false};
  // Linearizes Reset's ownership claim against the execute worker's first
  // post-plan state decision. Lifecycle commands take lifecycle_mutex_ first
  // and then this leaf; the worker takes only this leaf. Never wait for a
  // planner, backend stop, or worker while holding it.
  std::mutex planning_result_mutex_;
  // Reset cancellation of an admitted planner is owned by Reset, not by the
  // worker's generic planning-failure path. It remains asserted until the old
  // worker is retired before a later execute admission.
  std::atomic<bool> planning_reset_requested_{false};
  std::atomic<bool> lifecycle_reset_waiting_for_stop_{false};
  std::atomic<bool> lifecycle_worker_retirement_in_progress_{false};
  // Set before requesting worker retirement only after the addressed owner has
  // committed its immutable Stop certificate. The retiring worker must then
  // release local state without issuing a duplicate backend Stop.
  std::atomic<bool> certified_containment_retirement_{false};
  // Monotonic per-execution terminal authority. Active interrupt latches can
  // be consumed by preemption/stop machinery before the backend finalizer runs;
  // this flag preserves the fact that safety authority existed so a later
  // normal completion cannot launder it.
  std::atomic<bool> terminal_stop_authority_asserted_{false};
  std::atomic<bool> terminal_fault_authority_asserted_{false};
  OperatorStopWorker operator_stop_worker_;
  std::mutex operator_stop_source_mutex_;
  std::shared_ptr<std::stop_source> operator_stop_source_;
  std::atomic<bool> operator_stop_path_collision_detected_{false};
  mutable std::mutex operator_stop_path_collision_mutex_;
  std::map<std::string, std::string> operator_stop_path_collision_details_;

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
  mutable std::mutex fault_preempt_stop_mutex_;
  std::shared_ptr<std::stop_source> fault_preempt_stop_source_;
  std::atomic<bool> fault_preempt_stop_path_collision_detected_{false};
  mutable std::mutex fault_preempt_stop_path_collision_mutex_;
  std::map<std::string, std::string> fault_preempt_stop_path_collision_details_;
  mutable std::mutex fault_preempt_intervention_error_mutex_;
  std::optional<ErrorDTO> fault_preempt_intervention_error_;
  // Metrics. `metrics_` covers low-rate counters. Runtime-monitor cadence
  // counters stay atomic so telemetry reads cannot block the safety lane.
  mutable std::mutex metrics_mutex_;
  mutable MetricsSnapshot metrics_;
  std::atomic<std::uint64_t> runtime_collision_ticks_started_{0};
  std::atomic<std::uint64_t> runtime_collision_ticks_completed_{0};
  std::atomic<std::uint64_t> runtime_collision_ticks_faulted_{0};
  std::atomic<std::uint64_t> runtime_collision_tick_max_ns_{0};
  RuntimeCollisionStopCoverageTracker runtime_collision_stop_coverage_;
  std::atomic<std::uint64_t> validation_deferred_runtime_monitor_active_{0};

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

  mutable std::mutex prepared_motion_session_mutex_;
  std::shared_ptr<PreparedMotionSession> active_prepared_motion_session_;
  mutable std::mutex prepared_command_session_mutex_;
  std::shared_ptr<PreparedCapabilityCommandSession> active_prepared_command_session_;

  // Declared last so its destructor runs first after the kernel destructor body.
  // The destructor body already stops/drains it explicitly; this member order is
  // a second guard that prevents scheduled callbacks from observing partially
  // destroyed kernel state if future teardown code changes.
  RuntimeCollisionMonitor runtime_collision_monitor_;
};

}  // namespace manyforge::core
