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
#include <manyforge_core/kernel_event_publisher.hpp>
#include <manyforge_core/kernel_flight_recorder.hpp>
#include <manyforge_core/resource_slot_table.hpp>
#include <manyforge_core/ports/icollision_trajectory_projector.hpp>
#include <manyforge_core/ports/iexecution_backend.hpp>
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
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
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
#include <utility>
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
  IPlanningSceneSynchronizer* planning_scene_synchronizer = nullptr;
};

/// Per-participant motion execution: planning, validation, preview,
/// collision look-ahead, prepared-command lifetime, execution realization and
/// participant evidence. It keeps working phases only and reports facts about
/// how each execution ended (`ExecutionStatus::fact`, kParticipantSettled);
/// the addressed Kernel owns operation identity, outcomes and Stop.
///
/// Spec 230: all operations are ROS-free.
class ExecutionKernel : public IResolvedParticipantExecutionEngine,
                        public IResolvedPreparedParticipantExecutionEngine,
                        public IMotionPreview {
 public:
  ExecutionKernel(IPlannerBackend& planner, ExecutionKernelPorts ports,
                  IExecutionBackend& execution_backend, IClock& clock, IScheduler& scheduler,
                  TrajectoryCache& cache, ExecutionKernelConfig config = {});

  ~ExecutionKernel() override;

  AddressedParticipantStopFenceDisposition fence_addressed_participant_stop(
      std::uint64_t execution_id) noexcept override;

  Result<AddressedParticipantStopSettlementDTO> settle_addressed_participant_stop(
      const AddressedParticipantStopSettlementRequestDTO& request) override;

  Result<std::uint64_t> PrepareMotion(ResolvedPreparedMotionExecutionRequestDTO request) override;
  Result<ResolvedPreparedMotionExecutionSnapshotDTO> ObservePreparedMotion(
      std::uint64_t execution_id) const override;
  Result<void> DispatchPreparedMotion(std::uint64_t execution_id,
                                      const ContentDigest& prepared_contract_digest) override;
  Result<void> CancelPreparedMotion(std::uint64_t execution_id) override;
  Result<bool> RetireCancelledPreparedMotion(std::uint64_t candidate_id) override;
  Result<PreparedMotionReplacementSourceDTO> CaptureMotionReplacementSource(
      std::uint64_t execution_id) const override;
  Result<bool> CommitPreparedMotionReplacement(std::uint64_t candidate_id,
                                               const PreparedMotionReplacementSourceDTO& source,
                                               PreparedMotionReplacementCommitDTO commit) override;
  Result<bool> PrepareColdMotionAfterStop(std::uint64_t candidate_id,
                                          const StopCertificateDTO& certificate) override;

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

  /// Motion-free retirement after the addressed owner certified containment:
  /// retires the prepared membership and local bookkeeping; never resets the
  /// backend or commands motion, so positions are preserved.
  ResetResultDTO reset_after_certified_containment(
      std::uint64_t execution_id,
      std::optional<std::uint64_t> expected_epoch = std::nullopt) override;

  /// Gripper commands belong to the separate gripper participant; this motion
  /// participant reports them as not configured.
  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override;
  Result<void> validate_gripper_command(const GripperCommandRequestDTO& request) const override;
  GripperCommandStatusDTO get_gripper_status(const std::string& gripper_id) const override;
  Result<std::uint64_t> stop_gripper(const std::string& gripper_id,
                                     std::uint64_t expected_command_id,
                                     std::uint64_t expected_generation) override;
  Result<void> reset_gripper(const std::string& gripper_id, std::uint64_t expected_command_id,
                             std::uint64_t expected_generation) override;
  /// Motion-free retirement of a computed-completion fact; returns to Idle.
  ///
  /// Valid only when the current execution is exactly `execution_id` and its
  /// fact is computed completion. Other facts retire through Reset/recovery.
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

  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO& command) override;
  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId& operation_id, const ControlBindingId& control_binding_id) const override;

  /// Get current kernel status.
  ExecutionStatus get_status() const override;

  /// Get the latest observer-safe execution snapshot published by the kernel.
  ExecutionSnapshot get_execution_snapshot() const;

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
  /// use this once while constructing a private Kernel. Retained Kernel
  /// replacements require the exact held Apply-fence API below. The bootstrap
  /// publisher is borrowed and its composition owner must keep it alive until
  /// an owned replacement has completed (and through Kernel teardown if none
  /// is performed).
  void set_collision_resources(
      ICollisionWorldSnapshotPublisher& publisher,
      std::shared_ptr<ICollisionQueryEngine> runtime_monitor_engine,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> validation_engines,
      std::vector<std::shared_ptr<ICollisionQueryEngine>> runtime_lookahead_engines = {});

  /// Replace collision resources on a retained Kernel while its exact Apply
  /// fence is held. The call is accepted only at Idle with no active monitor
  /// or operation, and candidate validation completes before publication.
  /// Returned shared owners are the installed objects; generation identifies
  /// this exact Kernel-local publication for immediate readback comparison.
  [[nodiscard]] Result<CollisionPipelineResourceReadbackDTO>
  replace_collision_resources_under_apply_fence(
      const ConfigurationFenceId& apply_fence_id,
      const ConfigurationFenceScopeDTO& apply_fence_scope,
      const IResourceOperationAdmissionGate& admission_gate,
      CollisionPipelineResourceOwners candidate);

  /// Return the currently installed exact owned resource set. Initial legacy
  /// bootstrap resources without a shared publisher owner cannot be returned
  /// through this API.
  [[nodiscard]] Result<CollisionPipelineResourceReadbackDTO>
  collision_resource_set_readback() const;

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

  /// Subscribe to kernel events.
  /// @param callback Called for each event. Must be thread-safe.
  /// @return Handle for unsubscribing.
  SubscriptionHandle subscribe_events(EventCallback callback) override;

  /// Unsubscribe from kernel events.
  void unsubscribe_events(SubscriptionHandle handle) override;

  /// Block until the current execution worker has finished (its fact is reported).
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
  struct ActiveMotionExecution;
  [[nodiscard]] std::shared_ptr<PreparedMotionSession> failed_preparation_never_dispatched_locked();
  [[nodiscard]] bool failed_preparation_settled_locked();
  [[nodiscard]] std::optional<ErrorDTO>
  collision_resource_replacement_refusal_locked();
  /// Motion-free retirement of the prepared membership through the backend's
  /// RetirePreparedExecution; never moves the robot or resets the backend.
  [[nodiscard]] Result<void> retire_prepared_motion_membership(
      const PreparedMotionSession* preserve_pending = nullptr);
  /// Recovery classification input derived from the participant facts and
  /// the addressed Stop evidence. Recovery moves to the addressed Kernel next.
  [[nodiscard]] ExecutionState recovery_execution_state_locked(const ExecutionStatus& status) const;
  struct PlanningSuccess;
  struct ValidatedMotionPlan;
  struct ExecuteWorkerPlanPhaseResult;
  struct ExecuteWorkerValidationPhaseResult;

  Result<std::uint64_t> execute_impl(const ExecuteRequestDTO& request,
                                     std::shared_ptr<PreparedMotionSession> prepared_session);
  Result<std::uint64_t> prepare_motion_replacement(std::shared_ptr<PreparedMotionSession> session);
  Result<std::uint64_t> prepare_spring_motion_attachment(
      ResolvedPreparedMotionExecutionRequestDTO request);
  Result<void> launch_motion_replacement_preparation(
      const std::shared_ptr<PreparedMotionSession>& session);
  void compute_motion_replacement(std::stop_token stop_token,
                                  std::shared_ptr<PreparedMotionSession> session);
  Result<void> compute_spring_motion_replacement(
      std::stop_token stop_token, const std::shared_ptr<PreparedMotionSession>& session);
  Result<void> qualify_spring_motion_replacement(
      ResolvedPreparedMotionExecutionRequestDTO& candidate, const PreparedMotionSession& source);
  bool service_motion_replacement(std::stop_token stop_token, ActiveMotionExecution& current);
  Result<void> check_motion_replacement_scene(const PreparedMotionSession& session) const;

  Result<PlanRequestDTO> make_plan_request(const ExecuteRequestDTO& request,
                                           const CollisionWorldLeaseInventory& projection_inventory,
                                           std::uint32_t planning_attempt_index,
                                           const RobotStateDTO* start_state_override = nullptr,
                                           const MovingReplacementInputDTO* replacement = nullptr);
  Result<PlanningSuccess> plan_trajectory_with_evidence(
      const ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
      std::uint32_t planning_attempt_index = 0, const RobotStateDTO* start_state_override = nullptr,
      const MovingReplacementInputDTO* replacement = nullptr,
      std::optional<std::int64_t> suffix_planning_deadline_ns = std::nullopt);
  Result<PlanningSuccess> plan_motion_continuation(
      ExecuteRequestDTO& request, const CollisionWorldLeaseInventory& projection_inventory,
      const detail::MotionResumeIntent& continuation);
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
      const RobotStateDTO* start_state_override = nullptr, std::stop_token external_stop_token = {},
      bool* conclusive_collision_failure = nullptr,
      std::shared_ptr<CollisionCancellationToken> validation_cancellation = {},
      std::optional<std::uint64_t> validation_execution_id = std::nullopt,
      const MovingReplacementInputDTO* replacement = nullptr);

  /// Shared computation only. The caller owns planner serialization, the
  /// deadline/cancellation registration, projection lifetime and admission.
  /// Explicit validation ownership never replaces an incumbent's token.
  Result<ValidatedMotionPlan> compute_validated_motion_plan(
      const ExecuteRequestDTO& request, CollisionWorldLeaseInventory& projection_inventory,
      const RobotStateDTO& start_state, std::uint32_t planning_attempt_index,
      std::int64_t planning_start_ns, std::int64_t planning_timeout_ns, std::stop_token stop_token,
      std::shared_ptr<CollisionCancellationToken> validation_cancellation = {},
      std::optional<std::uint64_t> validation_execution_id = std::nullopt,
      const MovingReplacementInputDTO* replacement = nullptr,
      std::optional<std::int64_t> suffix_planning_deadline_ns = std::nullopt);

  /// Async execution worker function.
  void async_execute_worker(std::stop_token st, ExecuteRequestDTO request, uint64_t exec_id,
                            std::optional<TrajectoryDTO> cached_trajectory,
                            bool cache_revalidation_candidate,
                            RetainedSceneSnapshotLease retained_scene,
                            std::optional<SceneSnapshotLease> scene_snapshot_lease,
                            std::shared_ptr<PreparedMotionSession> prepared_session = {});

  /// Build immutable authority/evidence only; no backend Prepare, session,
  /// lifecycle, snapshot or monitoring mutation belongs in this computation.
  static Result<std::shared_ptr<const PreparedOperationParticipantDTO>>
  assemble_prepared_motion_participant(
      const ResolvedPreparedMotionExecutionRequestDTO& authority,
      const ExecuteRequestDTO& request, const TrajectoryDTO& trajectory,
      const PlanRequestDTO& plan_request, const PlanResultDTO& planning_result,
      const PreparedMotionValidationEvidenceDTO& validation_evidence);

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

  /// Record the participant fact of the current execution (first fact wins),
  /// publish its final snapshot and emit kParticipantSettled. Returns false
  /// when the execution already carries a fact.
  bool settle_execution(std::uint64_t exec_id, const std::string& move_id,
                        ParticipantExecutionFact fact, const char* reason,
                        std::optional<ExecutionSnapshot> final_snapshot = std::nullopt);

  /// Publish an observer-safe execution snapshot.
  void publish_execution_snapshot(ExecutionSnapshot snapshot);
  std::uint64_t start_collision_monitoring(std::string reason,
                                           const CollisionWorldLeaseInventory* admission_inventory);
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
  void request_active_validation_cancel(std::string reason);
  void request_active_planning_cancel();
  /// Planning-deadline watchdog: records the expiry and cancels planning. The
  /// execute worker alone reports the resulting fact.
  void expire_planning_deadline();

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
  void try_refresh_recovery_assessment(const char* trigger);
  void schedule_recovery_assessment_refresh(const char* trigger) noexcept;
  std::uint64_t begin_operator_stop_owner_generation(std::uint64_t execution_id);
  std::uint64_t operator_stop_owner_generation_for_execution(std::uint64_t execution_id) const;
  void schedule_recovery_freshness_deadline_locked(const KernelRecoveryAssessmentDTO& assessment);
  void schedule_recovery_observation_poll_locked(const KernelRecoveryAssessmentDTO& assessment);
  KernelRecoveryActionResultDTO apply_recovery_action_impl(KernelRecoveryActionRequestDTO request);
  bool control_command_epoch_matches(std::optional<std::uint64_t> expected_epoch) const;

  // Worker-local terminal record. Every worker terminal exit records exactly
  // once (first record wins); a recorded error is the exact cause handed to
  // the prepared-motion owner when the worker exits before dispatch.
  struct ActiveMotionExecution {
    explicit ActiveMotionExecution(ExecuteRequestDTO supplied) : request(std::move(supplied)) {}

    // One worker's current context. Replacement adopts these existing values
    // only at that worker's safe point, never by starting another presenter.
    ExecuteRequestDTO request;
    std::uint64_t execution_id{};
    RetainedSceneSnapshotLease retained_scene;
    std::optional<SceneSnapshotLease> scene_snapshot_lease;
    std::shared_ptr<PreparedMotionSession> prepared_session;
    ExecutionHandle execution_handle;
    std::int64_t exec_start_ns{};
    std::int64_t exec_timeout_ns{};
    ExecutionSnapshot latest_backend_snapshot;
    std::int64_t last_runtime_scale_sample_ns{};
    double runtime_scaled_execution_ns{};
    std::int64_t zero_speed_hold_timeout_ns{};
    bool zero_speed_holding{};
    std::int64_t zero_speed_hold_started_ns{};
    bool zero_speed_hold_stop_requested{};
    bool terminal_recorded = false;
    std::optional<ErrorDTO> terminal_error;
    std::uint64_t runtime_collision_protection_generation = 0;
    std::uint64_t execution_progress_generation = 0;

    void record_terminal(std::optional<ErrorDTO> error) {
      if (terminal_recorded) {
        return;
      }
      terminal_recorded = true;
      terminal_error = std::move(error);
    }
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

  /// Report an exact pre-dispatch cancellation as the participant's
  /// cancelled-before-dispatch fact before a cancelled planner/validator can
  /// be mistaken for a local failure. Returns true when cancellation was
  /// requested; the execution then has ended.
  bool finalize_prepared_motion_cancellation_if_requested(
      const std::shared_ptr<PreparedMotionSession>& prepared_session,
      const ExecuteRequestDTO& request, std::uint64_t exec_id, ActiveMotionExecution& motion_state,
      const char* reason);

  /// Report a local failure fact for the current execution.
  void settle_local_failure(const ExecuteRequestDTO& request, std::uint64_t exec_id,
                            ActiveMotionExecution& motion_state, ErrorDTO error,
                            ParticipantEffectKnowledge effect, const char* reason,
                            std::optional<ExecutionSnapshot> final_snapshot = std::nullopt);

  ExecuteWorkerPlanPhaseResult execute_worker_plan_phase(
      std::stop_token st, ExecuteRequestDTO& request,
      const CollisionWorldLeaseInventory& projection_inventory, std::uint64_t exec_id,
      std::optional<TrajectoryDTO> cached_trajectory, bool cache_revalidation_candidate,
      ActiveMotionExecution& motion_state,
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

  void store_validated_candidate(const ExecuteRequestDTO& request, std::uint64_t exec_id,
                                 const TrajectoryDTO& trajectory,
                                 const ProjectionRevisionDTO& projection_revision,
                                 const CollisionWorldRevision& validation_revision,
                                 bool revalidated_cache_candidate);

  void execute_worker_finalize_phase(const ExecuteRequestDTO& request, std::uint64_t exec_id,
                                     ExecutionHandle& execution_handle,
                                     ActiveMotionExecution& motion_state);

  IPlannerBackend& planner_;
  IObservedRobotStateProvider& observed_state_;
  IRobotKinematics& kinematics_;
  IReferencePoseResolver& reference_resolver_;
  ITrajectoryValidator& trajectory_validator_;
  ICollisionTrajectoryProjector& collision_projector_;
  const ICollisionDiagnosticComparator* collision_diagnostic_comparator_;
  IPlanningSceneSynchronizer* planning_scene_synchronizer_;
  IExecutionBackend& execution_backend_;
  IClock& clock_;
  IScheduler& scheduler_;
  TrajectoryCache& cache_;
  ExecutionKernelConfig config_;

  ExecutionStateMachine sm_;

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
  mutable std::mutex collision_resource_set_mutex_;
  std::uint64_t collision_resource_set_generation_{};
  std::unique_ptr<ParallelCollisionValidationWorkerPool> collision_validation_worker_pool_;
  std::atomic<std::uint64_t> validation_collision_workers_effective_{0};
  mutable std::mutex runtime_collision_protection_mutex_;
  RuntimeCollisionProtectionSnapshot runtime_collision_protection_;
  std::uint64_t runtime_collision_protection_next_generation_ = 0;

  // RT-8B recovery authority state. Lock order (documented at the admission
  // site): lifecycle_mutex_ -> exactly one of
  // {recovery_certificate_mutex_, recovery_assessment_mutex_,
  //  recovery_refresh_mutex_, operator_stop_identity_mutex_}; the leaf
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
  // Exact process-local eligibility fence installed synchronously by the
  // addressed owner after Stop acceptance and before physical Stop dispatch.
  // It prevents late work from the replaced trajectory from competing with
  // the aggregate Stop terminal; it is not containment evidence.
  std::atomic<std::uint64_t> addressed_stop_fence_execution_id_{0};
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

  // Serializes low-rate lifecycle authority transitions across admission,
  // certified-containment reset, recovery and acknowledge.
  std::mutex lifecycle_mutex_;
  /// A native preview holds lifecycle_mutex_ for exclusive planner access
  /// while it plans. Lifecycle commands that must not wait for that planning
  /// (certified-containment reset, recovery) announce themselves here and cancel the active
  /// preview before blocking on lifecycle_mutex_.
  struct LifecyclePreemption;
  std::atomic<std::uint32_t> lifecycle_preemption_waiters_{0U};
  std::mutex preview_cancellation_mutex_;
  std::optional<std::stop_source> active_preview_cancellation_;
  // Lifecycle/recovery command epoch, bumped by each settled addressed Stop and
  // captured into the recovery assessment.
  std::atomic<std::uint64_t> control_command_epoch_{0};
  std::atomic<uint64_t> next_execution_id_{1};
  // Exact normal-completion event batch currently being published. Matching
  // observer reads retain the prior status and acknowledgement is transiently
  // rejected until terminal evidence precedes externally visible Completed.
  std::atomic<std::uint64_t> completion_publication_execution_id_{0};
  mutable std::mutex completion_publication_status_mutex_;
  ExecutionStatus completion_publication_previous_status_;
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
  // Retirement's cancellation of an admitted planner is owned by retirement,
  // not by the worker's local planning-failure fact. It remains asserted until
  // the old worker is retired.
  std::atomic<bool> planning_reset_requested_{false};
  // Set by the planning-deadline watchdog for the current planning attempt.
  std::atomic<bool> planning_deadline_expired_{false};
  std::atomic<bool> lifecycle_worker_retirement_in_progress_{false};
  // Set before requesting worker retirement only after the addressed owner has
  // committed its immutable Stop certificate. The retiring worker must then
  // release local state without issuing a duplicate backend Stop.
  std::atomic<bool> certified_containment_retirement_{false};
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

  mutable std::mutex prepared_motion_session_mutex_;
  std::shared_ptr<PreparedMotionSession> active_prepared_motion_session_;
  std::shared_ptr<PreparedMotionSession> pending_prepared_motion_session_;
  ActiveMotionExecution* active_motion_execution_{};  // same mutex; existing worker's stack
  std::jthread replacement_preparation_thread_;  // computation only; never a physical presenter

  // Declared last so its destructor runs first after the kernel destructor body.
  // The destructor body already stops/drains it explicitly; this member order is
  // a second guard that prevents scheduled callbacks from observing partially
  // destroyed kernel state if future teardown code changes.
  RuntimeCollisionMonitor runtime_collision_monitor_;
};

}  // namespace manyforge::core
