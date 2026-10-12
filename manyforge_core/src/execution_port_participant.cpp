#include <algorithm>
#include <cmath>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/execution_sample_association.hpp>
#include <manyforge_core/types/prepared_execution.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

std::string format(double value) { return std::to_string(value); }

/// Item 28 / Q11: the speed factor slews at most 4 per second.
constexpr double kMaxRuntimeScaleSlewPerS = 4.0;

/// `main`'s backend fault shape: a Core error code with the stable reason
/// token, kept also under `reason` for the Kernel's own readers.
ErrorDTO fault(ErrorCode code, std::string message, const std::string& reason,
               std::map<std::string, std::string> details = {}) {
  details["reason_code"] = reason;
  details["reason"] = reason;
  details["surface"] = "execution_port_participant";
  return ErrorDTO{std::string(to_string(code)), std::move(message), std::move(details)};
}

const char* refusal_name(ExecutionPortRefusal refusal) {
  switch (refusal) {
    case ExecutionPortRefusal::kStaleKey:
      return "stale_key";
    case ExecutionPortRefusal::kInvalidInput:
      return "invalid_input";
    case ExecutionPortRefusal::kUnsupported:
      return "unsupported";
    case ExecutionPortRefusal::kNotReady:
      return "not_ready";
  }
  return "unknown";
}

bool ready(const ExecutionReadinessDTO& readiness) {
  return readiness.controller_active && readiness.controller_bound && readiness.hardware_active;
}

bool finite(const std::vector<double>& values, std::size_t size) {
  return values.size() == size &&
         std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

/// `values` in lane joint order, reordered to `to`; nullopt when incomplete.
std::optional<std::vector<double>> reorder(const std::vector<double>& values,
                                           const std::vector<std::string>& from,
                                           const std::vector<std::string>& to) {
  if (values.size() != from.size()) return std::nullopt;
  std::vector<double> result;
  result.reserve(to.size());
  for (const auto& name : to) {
    const auto found = std::find(from.begin(), from.end(), name);
    if (found == from.end()) return std::nullopt;
    result.push_back(values[static_cast<std::size_t>(found - from.begin())]);
  }
  return result;
}

double duration_s(const TrajectoryDTO& trajectory) {
  return trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
}

}  // namespace

/// One dispatched goal and the policy state that follows it.
struct ExecutionPortParticipant::Execution {
  std::uint64_t generation{};
  ExecutionGoalKey key;
  TrajectoryDTO trajectory;
  std::shared_ptr<IExecutionPort> port;
  ExecutionBindingEpoch epoch{0U};
  /// Q50: the goal's sample association, shared with the Stop-stage helper.
  std::shared_ptr<ExecutionSampleAssociation> association;
  bool recognition_recorded{false};
  /// Q52: a controller cycle later than the lane margin; reported to the
  /// Kernel as a critical fault (item 17 path).
  std::optional<ErrorDTO> critical_fault;
  std::optional<TimePoint> tracking_breach_since;
  /// When the successful result was first observed (measured completion gate).
  std::optional<TimePoint> success_observed_at;
  /// A Stop stage was entered: the Kernel's Stop owns the motion from here on.
  bool stopping{false};
  std::optional<ParticipantStopStageOutcomeDTO> last_stage_outcome;
  std::optional<ErrorDTO> last_stage_error;
  bool terminal{false};
  ExecutionSnapshot snapshot;
  /// Original-path cursor retained at the end (continuation, §3.3).
  std::optional<double> stopped_cursor_s;
  double cursor_s{0.0};
};

ExecutionPortParticipant::ExecutionPortParticipant(
    RuntimeParticipantBinding binding, std::shared_ptr<IExecutionPort> port, const IClock& clock,
    IScheduler& scheduler, StopStageWaiter waiter,
    ExecutionPortParticipantStateAuthority state_authority, ExecutionPortParticipantConfig config)
    : clock_(clock),
      scheduler_(scheduler),
      waiter_(std::move(waiter)),
      state_authority_(std::move(state_authority)),
      config_(std::move(config)),
      // The controller's own cursor on every lane (§3.3); a measured lane
      // publishes it under the measured-physical authority, whose cursor
      // expires after the freshness budget.
      progress_(config_.stop.trip_authority == StateAuthorityClass::kMeasured
                    ? ExecutionProgressSourceKind::kMeasuredProjection
                    : ExecutionProgressSourceKind::kBackendCommanded,
                config_.stop.trip_authority == StateAuthorityClass::kMeasured
                    ? ExecutionProgressAuthorityProfile::kMeasuredPhysical
                    : ExecutionProgressAuthorityProfile::kCommandedSimulation,
                config_.stop.trip_authority == StateAuthorityClass::kMeasured
                    ? "execution_port_measured_progress_v1"
                    : "execution_port_commanded_progress_v1",
                config_.progress_freshness_budget_ns),
      binding_(std::move(binding)) {
  const auto non_negative = [](double value) { return std::isfinite(value) && value >= 0.0; };
  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  if (!port || !waiter_ || state_authority_.publication == nullptr ||
      !state_authority_.sync_before_planning ||
      (config_.stop.trip_authority != StateAuthorityClass::kMeasured &&
       config_.stop.trip_authority != StateAuthorityClass::kCommanded) ||
      (measured && (!config_.final_tolerance_rad.has_value() ||
                    !non_negative(*config_.final_tolerance_rad) ||
                    !config_.start_tolerance_rad.has_value() ||
                    !non_negative(*config_.start_tolerance_rad) ||
                    config_.progress_freshness_budget_ns <= 0 ||
                    config_.completion_state_timeout_ns <= 0)) ||
      (config_.tracking.has_value() && (!non_negative(config_.tracking->tolerance_rad) ||
                                        !non_negative(config_.tracking->timeout_s))) ||
      !std::isfinite(config_.late_cycle_margin_s) || config_.late_cycle_margin_s <= 0.0 ||
      !std::isfinite(config_.initial_speed_factor) || config_.initial_speed_factor < 0.0 ||
      config_.initial_speed_factor > 1.0 || config_.progress_freshness_budget_ns < 0) {
    throw std::invalid_argument("ExecutionPortParticipant requires complete lane facts");
  }
  // The Stop-stage helper's resources are the binding's.
  config_.stop.resources = binding_.value().resources;
  target_scale_ = config_.initial_speed_factor;
  if (auto rebound = RebindPort(std::move(port)); !rebound.ok()) {
    throw std::invalid_argument(rebound.error().message);
  }
}

ExecutionPortParticipant::~ExecutionPortParticipant() {
  {
    std::lock_guard lock(scale_mutex_);
    ++ramp_generation_;
    if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
    ramp_handle_ = 0U;
  }
  scheduler_.drain(SchedulerPriority::ControllerCommand);
}

std::shared_ptr<ParticipantStopStageHelper> ExecutionPortParticipant::MakeHelper(
    IExecutionPort& port) {
  return std::make_shared<ParticipantStopStageHelper>(
      port, clock_, waiter_, [this] { return FreezeSpeedFactor(); }, config_.stop);
}

Result<void> ExecutionPortParticipant::RebindPort(std::shared_ptr<IExecutionPort> port) {
  const auto refuse = [](std::string reason, std::string message) {
    return Result<void>::Err(
        fault(ErrorCode::kExecutionRejected, std::move(message), std::move(reason)));
  };
  if (!port) return refuse("execution_port_missing", "A rebinding requires its port instance");
  const auto lane = port->ReadLaneFacts();
  std::vector<std::string> compiled;
  for (const auto& resource : binding_.value().resources) {
    compiled.push_back(resource.compiled_resource_name);
  }
  if (lane.joint_names.size() != compiled.size() ||
      !std::is_permutation(lane.joint_names.begin(), lane.joint_names.end(), compiled.begin()) ||
      (lane.supports_scale && !(lane.controller_update_rate_hz > 0.0))) {
    return refuse("execution_port_lane_facts_mismatch",
                  "The port's lane facts do not cover the participant binding");
  }
  std::lock_guard lock(mutex_);
  if ((execution_ && !execution_->terminal) || stop_stages_in_flight_ != 0U) {
    return refuse("execution_port_rebind_busy",
                  "A port cannot be rebound while motion or a Stop is in flight");
  }
  auto helper = MakeHelper(*port);
  {
    std::lock_guard scale_lock(scale_mutex_);
    ++ramp_generation_;
    if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
    ramp_handle_ = 0U;
    ramp_.reset();
    // A new binding epoch starts at the configured initial factor (§3.2).
    last_sent_scale_.reset();
    scale_frozen_ = false;
  }
  port_ = std::move(port);
  helper_ = std::move(helper);
  lane_ = lane;
  timing_ = {};
  return Result<void>::Ok();
}

Result<void> ExecutionPortParticipant::RebindRoute(
    RuntimeParticipantBinding binding, std::vector<RuntimeParticipantBinding> program_aliases) {
  std::lock_guard lock(mutex_);
  if (prepared_membership_.active_execution_id() || (execution_ && !execution_->terminal) ||
      stop_stages_in_flight_ != 0U) {
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Route publication cannot rebind an active participant",
                                   "execution_port_route_rebind_busy"));
  }
  if (binding.value().resources != binding_.value().resources ||
      program_aliases.size() > 64U ||
      std::any_of(program_aliases.begin(), program_aliases.end(), [&](const auto& alias) {
        return !runtime_participant_binding_physically_contains(binding, alias);
      })) {
    return Result<void>::Err(fault(ErrorCode::kInvalidRequest,
                                   "Route publication crosses the participant's physical binding",
                                   "execution_port_route_rebind_mismatch"));
  }
  binding_ = std::move(binding);
  program_aliases_ = std::move(program_aliases);
  return Result<void>::Ok();
}

ExecutionPortTimingMetrics ExecutionPortParticipant::timing_metrics() const {
  std::lock_guard lock(mutex_);
  return timing_;
}

void ExecutionPortParticipant::ReportCriticalFault(ErrorDTO error) {
  std::lock_guard lock(mutex_);
  if (!execution_ || execution_->terminal || execution_->critical_fault) return;
  execution_->critical_fault = std::move(error);
}

bool ExecutionPortParticipant::GoalInFlight() const {
  std::lock_guard lock(mutex_);
  return (execution_ && !execution_->terminal) || stop_stages_in_flight_ != 0U ||
         (helper_ && helper_->HasLiveGoal());
}

std::optional<std::string_view> ExecutionPortParticipant::BindingFieldError(
    const PreparedOperationParticipantDTO& participant) const {
  const auto kind = PreparedParticipantPayloadKindDTO::kMotionExecution;
  if (!prepared_participant_backend_binding_field_error(participant, binding_, kind)) {
    return std::nullopt;
  }
  if (std::any_of(program_aliases_.begin(), program_aliases_.end(), [&](const auto& alias) {
        return !prepared_participant_backend_binding_field_error(participant, alias, kind);
      })) {
    return std::nullopt;
  }
  return prepared_participant_backend_physical_binding_field_error(participant, binding_, kind);
}

PrepareParticipantResultDTO ExecutionPortParticipant::Prepare(
    const PreparedOperationParticipantDTO& participant, const std::uint64_t execution_id) {
  const auto make = [&](PrepareParticipantResultStateDTO state, std::optional<ErrorDTO> error) {
    auto result = MakePrepareParticipantResult(participant, state, std::move(error));
    if (!result.ok()) throw std::invalid_argument(result.error().message);
    return std::move(result.value());
  };
  const auto reject = [&](std::string reason) {
    return make(PrepareParticipantResultStateDTO::kRejected,
                fault(ErrorCode::kExecutionRejected, "Execution port participant rejected the "
                                                     "prepared participant",
                      reason));
  };
  std::lock_guard lock(mutex_);
  if (const auto invalid = BindingFieldError(participant)) return reject(std::string(*invalid));
  const auto& motion = std::get<PreparedMotionExecutionDTO>(participant.payload);
  if (motion.trajectory.resources != participant.resources) {
    return reject("prepared_trajectory_resource_scope_mismatch");
  }
  if (motion.trajectory.waypoints.empty()) return reject("empty_trajectory");
  // Q11: the speed ramp runs at the trajectory's own prepared bounds.
  const auto& bounds = motion.trajectory.runtime_scale_bounds;
  if (!bounds || !std::isfinite(bounds->max_rate_per_s) || bounds->max_rate_per_s <= 0.0) {
    return reject("prepared_runtime_scale_bounds_missing");
  }
  if (const auto invalid = prepared_membership_.prepare(execution_id, participant)) {
    return reject(std::string(*invalid));
  }
  return make(PrepareParticipantResultStateDTO::kPrepared, std::nullopt);
}

Result<void> ExecutionPortParticipant::RetirePreparedExecution(const std::uint64_t execution_id) {
  std::lock_guard lock(mutex_);
  const auto active = prepared_membership_.active_execution_id();
  if (active == execution_id &&
      ((execution_ && !execution_->terminal) || stop_stages_in_flight_ != 0U)) {
    return Result<void>::Err(fault(ErrorCode::kExecutionRejected,
                                   "Prepared execution still has active motion or Stop work",
                                   "prepared_execution_not_quiescent"));
  }
  if (!prepared_membership_.retire(execution_id)) {
    return Result<void>::Err(fault(ErrorCode::kInvalidRequest,
                                   "Prepared retirement names an unknown execution",
                                   "prepared_execution_stale"));
  }
  if (active == execution_id) prepared_goal_.reset();
  return Result<void>::Ok();
}

ExecutionHandle ExecutionPortParticipant::RejectedHandle(
    const PreparedOperationParticipantDTO& participant, const ControllerGoalId& goal_id,
    const ControllerGoalGeneration generation, ErrorDTO error) const {
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.state = ExecutionState::kFailed;
  snapshot.error = std::move(error);
  return ExecutionHandle{[](std::int64_t) { return true; },
                         [snapshot = std::move(snapshot)] { return snapshot; },
                         0U,
                         goal_id,
                         generation,
                         participant.prepared_contract_digest};
}

ExecutionHandle ExecutionPortParticipant::Dispatch(
    const PreparedOperationParticipantDTO& participant, const ControllerGoalId& controller_goal_id,
    const ControllerGoalGeneration controller_goal_generation) {
  const auto rejected = [&](std::string reason, std::map<std::string, std::string> details = {}) {
    return RejectedHandle(participant, controller_goal_id, controller_goal_generation,
                          fault(ErrorCode::kExecutionRejected,
                                "Execution port participant rejected the prepared dispatch",
                                reason, std::move(details)));
  };
  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  if (measured) AwaitFreshStartSample();
  std::unique_lock lock(mutex_);
  if (controller_goal_id.empty() || !controller_goal_generation.nonzero()) {
    return rejected("controller_goal_identity_invalid");
  }
  if (const auto invalid = BindingFieldError(participant)) return rejected(std::string(*invalid));
  if (!prepared_membership_.matches(participant)) {
    return rejected("prepared_participant_not_admitted");
  }
  // Replay protection: an exact repetition returns the same handle and never
  // sends a second goal; any other use of the goal identity is refused.
  if (prepared_goal_ && prepared_goal_->goal_id == controller_goal_id) {
    if (prepared_goal_->generation == controller_goal_generation &&
        prepared_goal_->prepared_contract_digest == participant.prepared_contract_digest) {
      return prepared_goal_->handle;
    }
    return rejected("controller_goal_identity_conflict");
  }
  if (execution_ && !execution_->terminal) return rejected("execution_port_execution_in_flight");
  if (stop_stages_in_flight_ != 0U) return rejected("execution_port_stop_in_flight");

  // §3.2: positions, velocities and accelerations in path time. A prepared
  // trajectory carries its runtime-scale bounds (item 6, checked at Prepare),
  // which the prepared codec admits only with complete derivatives.
  const auto& motion = std::get<PreparedMotionExecutionDTO>(participant.payload);
  auto& port = *port_;
  const auto readiness = port.ReadReadiness();
  if (!ready(readiness)) return rejected("execution_port_not_ready");
  const ExecutionGoalKey key{controller_goal_id, controller_goal_generation};
  const auto baseline = port.ReadSample();
  if (measured) {
    if (auto refusal = StartStateError(baseline, readiness.binding_epoch, motion.trajectory)) {
      return RejectedHandle(participant, controller_goal_id, controller_goal_generation,
                            std::move(*refusal));
    }
  }
  const auto dispatched_at = clock_.now();
  const auto sent = port.Submit(key, motion.trajectory);
  if (sent.dispatch == ExecutionPortDispatch::kNotDispatched) {
    std::map<std::string, std::string> details;
    if (sent.refusal) details["port_refusal"] = refusal_name(*sent.refusal);
    if (sent.error) details["port_error"] = sent.error->message;
    return rejected("execution_port_submit_refused", std::move(details));
  }
  // Dispatched (or possibly sent): the goal is observed through the port.
  auto execution = std::make_shared<Execution>();
  execution->generation = next_execution_generation_++;
  execution->key = key;
  execution->trajectory = motion.trajectory;
  execution->port = port_;
  execution->epoch = readiness.binding_epoch;
  execution->association = std::make_shared<ExecutionSampleAssociation>(
      readiness.binding_epoch, baseline, dispatched_at);
  // Item 10: Executing is published at dispatch.
  execution->snapshot.timestamp_ns = dispatched_at;
  execution->snapshot.state = ExecutionState::kExecuting;
  execution->snapshot.state_source = "execution_port";
  if (!execution->trajectory.waypoints.empty()) {
    execution->snapshot.joint_positions_rad = execution->trajectory.waypoints.front().positions_rad;
  }
  progress_.invalidate();
  (void)progress_.publish(execution->generation, 0, dispatched_at, dispatched_at);

  std::vector<PreparedParticipantStopPlanDTO> stop_plans{participant.ordinary_stop_plan};
  if (participant.fault_stop_plan) stop_plans.push_back(*participant.fault_stop_plan);
  for (const auto& scope : motion.interrupt_scopes) stop_plans.push_back(scope.stop_plan);
  helper_->BindDispatchedGoal(StopStageDispatchedGoalDTO{
      key,
      execution->trajectory,
      participant.operation_id,
      participant.operation_revision,
      participant.participant_id,
      participant.controllable_component_id,
      participant.command_target_id,
      participant.capability_id,
      participant.control_domain_id,
      participant.control_domain_revision,
      participant.control_binding_id,
      participant.control_binding_revision,
      participant.controller_topology_id,
      participant.topology_revision,
      participant.control_route_id,
      participant.active_control_configuration_id,
      participant.active_control_configuration_generation,
      participant.control_provider,
      participant.resources,
      participant.stop_evidence_policy_id,
      participant.stop_evidence_policy_revision,
      std::move(stop_plans)},
      execution->association);

  if (lane_.supports_scale) {
    // Q11: the ramp of this motion starts from the factor last sent on this
    // epoch (the lane's initial factor when none was) at the trajectory's
    // prepared bounds; after a Stop the frozen factor is released here.
    std::lock_guard scale_lock(scale_mutex_);
    scale_frozen_ = false;
    // Item 28/Q11: linear slew at the trajectory's rate, at most 4/s; no
    // separate rate-change limit.
    ramp_ = ScaleRamp{last_sent_scale_.value_or(config_.initial_speed_factor), target_scale_,
                      std::min(execution->trajectory.runtime_scale_bounds->max_rate_per_s,
                               kMaxRuntimeScaleSlewPerS)};
    if (ramp_->value != ramp_->target) {
      last_ramp_tick_ = clock_.now();
      ScheduleRampTickLocked();
    }
  }

  const auto generation = execution->generation;
  execution_ = execution;
  ExecutionHandle handle{
      [this, execution](std::int64_t timeout_ns) { return WaitForCompletion(execution, timeout_ns); },
      [this, execution] {
        std::lock_guard snapshot_lock(mutex_);
        return execution->snapshot;
      },
      generation,
      controller_goal_id,
      controller_goal_generation,
      participant.prepared_contract_digest};
  prepared_goal_ = PreparedGoalRecord{controller_goal_id, controller_goal_generation,
                                      participant.prepared_contract_digest, handle};
  return handle;
}

void ExecutionPortParticipant::AwaitFreshStartSample() {
  std::shared_ptr<IExecutionPort> port;
  std::size_t joints = 0U;
  {
    std::lock_guard lock(mutex_);
    port = port_;
    joints = lane_.joint_names.size();
  }
  if (!port) return;
  const auto deadline = clock_.now() + config_.completion_state_timeout_ns;
  while (true) {
    const auto sample = port->ReadSample();
    const auto now = clock_.now();  // after the sample, as in Evaluate
    if (sample && sample->binding_epoch == port->ReadReadiness().binding_epoch &&
        finite(sample->measured_positions_rad, joints) && sample->local_receipt_stamp_ns <= now &&
        now - sample->local_receipt_stamp_ns <= config_.progress_freshness_budget_ns) {
      return;
    }
    if (now >= deadline) return;
    waiter_(deadline, std::stop_token{});
  }
}

std::optional<ErrorDTO> ExecutionPortParticipant::StartStateError(
    const std::optional<ExecutionControllerSampleDTO>& sample, const ExecutionBindingEpoch epoch,
    const TrajectoryDTO& trajectory) const {
  // As `main`'s observed-robot dispatch: a fresh complete sample first, then
  // the first waypoint within the start tolerance.
  const auto now = clock_.now();
  const bool fresh = sample && sample->binding_epoch == epoch &&
                     finite(sample->measured_positions_rad, lane_.joint_names.size()) &&
                     sample->local_receipt_stamp_ns <= now &&
                     now - sample->local_receipt_stamp_ns <= config_.progress_freshness_budget_ns;
  if (!fresh) {
    return fault(ErrorCode::kExecutionRejected,
                 "No fresh complete controller state sample before dispatch",
                 "stale_controller_state",
                 {{"wait_for_state_timeout_s",
                   format(static_cast<double>(config_.completion_state_timeout_ns) / 1e9)},
                  {"required_joint_count", std::to_string(lane_.joint_names.size())}});
  }
  if (trajectory.waypoints.empty()) return std::nullopt;
  const auto observed =
      reorder(sample->measured_positions_rad, lane_.joint_names, trajectory.joint_names);
  if (!observed) {
    return fault(ErrorCode::kStartStateMismatch,
                 "Start state mismatch: the controller sample does not cover the trajectory joints",
                 "start_state_mismatch");
  }
  const auto& start = trajectory.waypoints.front().positions_rad;
  const auto count = std::min(observed->size(), start.size());
  double max_error = 0.0;
  std::size_t max_index = 0U;
  for (std::size_t index = 0U; index < count; ++index) {
    const double error = std::abs((*observed)[index] - start[index]);
    if (error > max_error) {
      max_error = error;
      max_index = index;
    }
  }
  if (max_error <= *config_.start_tolerance_rad) return std::nullopt;
  return fault(ErrorCode::kStartStateMismatch,
               "Start state mismatch: max joint error " + format(max_error) +
                   " rad exceeds tolerance " + format(*config_.start_tolerance_rad),
               "start_state_mismatch",
               {{"max_joint_error_rad", format(max_error)},
                {"start_tolerance_rad", format(*config_.start_tolerance_rad)},
                {"observed_position_rad", format((*observed)[max_index])},
                {"expected_position_rad", format(start[max_index])},
                {"joint_name", max_index < trajectory.joint_names.size()
                                   ? trajectory.joint_names[max_index]
                                   : "joint[" + std::to_string(max_index) + "]"}});
}

ExecutionHandle ExecutionPortParticipant::execute_trajectory(const TrajectoryDTO&) {
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = clock_.now();
  snapshot.state = ExecutionState::kFailed;
  snapshot.error = fault(ErrorCode::kExecutionRejected,
                         "The execution port participant executes prepared dispatches only",
                         "execution_port_prepared_dispatch_required");
  return ExecutionHandle{[](std::int64_t) { return true; }, [snapshot] { return snapshot; }, 0U};
}

Result<void> ExecutionPortParticipant::sync_observed_state_before_planning() {
  return state_authority_.sync_before_planning();
}

StopResultDTO ExecutionPortParticipant::stop() {
  std::lock_guard lock(mutex_);
  if (!execution_ || execution_->terminal) {
    return StopResultDTO{false, ErrorDTO{kErrorInvalidRequest, "No execution in flight", {}}};
  }
  const auto sent = execution_->port->Cancel(execution_->key);
  if (execution_->critical_fault) {
    // Q52 (item 17 path): the Kernel's hard stop after a critical fault ends
    // this execution InterventionRequired with the fault.
    Finish(*execution_, ExecutionState::kInterventionRequired, execution_->critical_fault);
  }
  if (sent.dispatch == ExecutionPortDispatch::kNotDispatched) {
    return StopResultDTO{
        false, fault(ErrorCode::kStopFailed, "The controller cancel was not dispatched",
                     "hard_stop_cancel_not_dispatched",
                     {{"port_refusal", sent.refusal ? refusal_name(*sent.refusal) : "none"}})};
  }
  return StopResultDTO{true, std::nullopt};
}

ResetResultDTO ExecutionPortParticipant::reset() {
  return ResetResultDTO{false,
                        fault(ErrorCode::kExecutionRejected,
                              "Reset is the Kernel's: its Stop stages, then motion-free retirement",
                              "execution_port_backend_reset_unsupported")};
}

ExecutionSnapshot ExecutionPortParticipant::get_execution_snapshot() const {
  std::lock_guard lock(mutex_);
  if (!execution_) {
    ExecutionSnapshot idle;
    idle.timestamp_ns = clock_.now();
    return idle;
  }
  return execution_->snapshot;
}

std::optional<double> ExecutionPortParticipant::stopped_trajectory_time_s(
    const std::uint64_t execution_generation) const {
  std::lock_guard lock(mutex_);
  if (!execution_ || execution_generation == 0U ||
      execution_->generation != execution_generation || !execution_->terminal ||
      stop_stages_in_flight_ != 0U) {
    return std::nullopt;
  }
  return execution_->stopped_cursor_s;
}

void ExecutionPortParticipant::retire_execution_progress_generation(
    const std::uint64_t execution_generation) noexcept {
  (void)progress_.invalidate_generation(execution_generation);
}

ExecutionProgressHazardClaimResult ExecutionPortParticipant::claim_execution_progress_hazard(
    const std::uint64_t execution_generation) noexcept {
  return progress_.claim_hazard_authority(execution_generation);
}

// --- Monitoring ---------------------------------------------------------------

bool ExecutionPortParticipant::WaitForCompletion(const std::shared_ptr<Execution>& execution,
                                                 const std::int64_t timeout_ns) {
  {
    std::lock_guard lock(mutex_);
    Evaluate(*execution);
    if (execution->terminal) return true;
  }
  waiter_(clock_.now() + std::max<std::int64_t>(0, timeout_ns), std::stop_token{});
  std::lock_guard lock(mutex_);
  Evaluate(*execution);
  return execution->terminal;
}

void ExecutionPortParticipant::Finish(Execution& execution, const ExecutionState state,
                                      std::optional<ErrorDTO> error) {
  execution.terminal = true;
  execution.snapshot.timestamp_ns = clock_.now();
  execution.snapshot.state = state;
  execution.snapshot.error = std::move(error);
  ++execution.snapshot.sequence_number;
  if (state == ExecutionState::kCompleted) {
    execution.snapshot.progress = 1.0;
    execution.stopped_cursor_s = duration_s(execution.trajectory);
    // Completion against a runtime collision hazard: the first authority
    // boundary wins (`ExecutionProgressPublication`).
    const auto now = clock_.now();
    (void)progress_.publish_terminal(
        execution.generation, static_cast<std::int64_t>(*execution.stopped_cursor_s * 1e9), now,
        now);
  } else {
    (void)progress_.invalidate_generation(execution.generation);
  }
  std::lock_guard scale_lock(scale_mutex_);
  ++ramp_generation_;
  if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
  ramp_handle_ = 0U;
  ramp_.reset();
}

std::optional<double> ExecutionPortParticipant::ObserveTiming(IExecutionPort& port,
                                                              const ExecutionBindingEpoch epoch) {
  // Q4: controller lateness (controller-manager periodicity) is kept apart
  // from telemetry loss (`controller_state` stamp gaps); best effort.
  const auto timing = port.ReadTiming();
  if (timing.binding_epoch != epoch) return std::nullopt;
  ++timing_.windows;
  std::optional<double> late;
  if (timing.period_max_s && std::isfinite(*timing.period_max_s)) {
    timing_.max_period_s = std::max(timing_.max_period_s, *timing.period_max_s);
    if (*timing.period_max_s > config_.late_cycle_margin_s) {
      ++timing_.windows_late_beyond_margin;
      late = *timing.period_max_s;
    }
  }
  if (timing.max_state_stamp_gap_s && std::isfinite(*timing.max_state_stamp_gap_s)) {
    timing_.max_state_stamp_gap_s =
        std::max(timing_.max_state_stamp_gap_s, *timing.max_state_stamp_gap_s);
  }
  if (timing.received_samples) timing_.received_samples += *timing.received_samples;
  return late;
}

void ExecutionPortParticipant::RecordRecognition(Execution& execution) {
  if (execution.recognition_recorded) return;
  const auto delay = execution.association->recognition_delay_ns();
  if (!delay) return;
  execution.recognition_recorded = true;
  ++timing_.goals_recognized;
  timing_.last_recognition_delay_ns = *delay;
  timing_.max_recognition_delay_ns = std::max(timing_.max_recognition_delay_ns, *delay);
}

void ExecutionPortParticipant::ObserveSample(Execution& execution,
                                             const ExecutionControllerSampleDTO& sample,
                                             const double original_cursor_s, const TimePoint now) {
  const auto total = duration_s(execution.trajectory);
  execution.cursor_s = std::clamp(original_cursor_s, 0.0, total);
  (void)progress_.publish(execution.generation, static_cast<std::int64_t>(execution.cursor_s * 1e9),
                          sample.source_stamp_ns, sample.local_receipt_stamp_ns);
  execution.snapshot.timestamp_ns = now;
  ++execution.snapshot.sequence_number;
  execution.snapshot.progress = total > 0.0 ? execution.cursor_s / total : 1.0;
  // Item 20: commanded state on commanded lanes, measured on real ones.
  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  if (auto positions =
          reorder(measured ? sample.measured_positions_rad : sample.reference_positions_rad,
                  lane_.joint_names, execution.trajectory.joint_names)) {
    execution.snapshot.joint_positions_rad = std::move(*positions);
  }
  execution.snapshot.joint_velocities_rad_s.clear();
  if (measured && sample.measured_velocities_rad_s) {
    if (auto velocities = reorder(*sample.measured_velocities_rad_s, lane_.joint_names,
                                  execution.trajectory.joint_names)) {
      execution.snapshot.joint_velocities_rad_s = std::move(*velocities);
    }
  }
  execution.snapshot.source_timestamp_ns = sample.local_receipt_stamp_ns;
  execution.snapshot.source_age_ns = std::max<std::int64_t>(0, now - sample.local_receipt_stamp_ns);
}

void ExecutionPortParticipant::Evaluate(Execution& execution) {
  if (execution.terminal) return;
  auto& port = *execution.port;
  // The sample before `now`: a receipt stamped (same monotonic clock) between
  // the two reads must not look newer than `now` and fail the freshness check.
  const auto joints = lane_.joint_names.size();
  const auto sample = port.ReadSample();
  const auto now = clock_.now();
  const bool usable = sample && sample->binding_epoch == execution.epoch &&
                      std::isfinite(sample->cursor_s) &&
                      finite(sample->reference_positions_rad, joints) &&
                      finite(sample->measured_positions_rad, joints);

  if (execution.stopping) {
    // The Kernel's Stop owns the motion: no tracking or completion verdict.
    // §3.3: progress continues on the original path, through the Stop
    // planner's own mapping once a replacement runs (frozen after its hold).
    if (usable) {
      if (const auto mapped = helper_->OriginalPathCursor(sample)) {
        ObserveSample(execution, *sample, *mapped, now);
      } else if (const auto goal = port.ReadGoal(execution.key);
                 goal && goal->phase != ExecutionGoalPhase::kTerminal &&
                 execution.association->Observe(*sample, goal)) {
        ObserveSample(execution, *sample, sample->cursor_s, now);
      }
    }
    RecordRecognition(execution);
    if (stop_stages_in_flight_ != 0U || !execution.last_stage_outcome.has_value()) return;
    if (*execution.last_stage_outcome == ParticipantStopStageOutcomeDTO::kSucceeded) {
      const auto original = port.ReadGoal(execution.key);
      Finish(execution, ExecutionState::kStopped, std::nullopt);
      // Continuation (§3.3): the original path's end when the original goal
      // itself completed (natural completion inside the Stop), else where the
      // on-path replacement left it; unknown after a hard stop.
      execution.stopped_cursor_s =
          original && original->result == ExecutionGoalResult::kSucceeded
              ? std::optional(duration_s(execution.trajectory))
              : helper_->OriginalPathCursor(std::nullopt);
    } else if (!helper_->HasLiveGoal()) {
      Finish(execution, ExecutionState::kInterventionRequired, execution.last_stage_error);
    }
    return;
  }

  if (execution.critical_fault) throw ExecutionCriticalFault(*execution.critical_fault);
  if (const auto late = ObserveTiming(port, execution.epoch)) {
    // Q52: a controller cycle later than the lane margin is a critical fault
    // handled like item 17: the Kernel issues the lane's hard stop and the
    // operation ends InterventionRequired.
    execution.critical_fault =
        fault(ErrorCode::kInternalError,
              "A controller cycle was later than the lane margin: " + format(*late) + " s",
              "late_control_cycle",
              {{"controller_period_s", format(*late)},
               {"late_cycle_margin_s", format(config_.late_cycle_margin_s)}});
    throw ExecutionCriticalFault(*execution.critical_fault);
  }
  const auto readiness = port.ReadReadiness();
  const auto goal = port.ReadGoal(execution.key);
  (void)execution.association->ObserveGoal(goal);
  RecordRecognition(execution);
  if (readiness.binding_epoch != execution.epoch || !ready(readiness)) {
    // As `main`: the controller binding was lost under an active goal.
    (void)port.Cancel(execution.key);
    Finish(execution, ExecutionState::kFailed,
           fault(ErrorCode::kExecutionAborted,
                 "Controller binding or lifecycle readiness was lost during execution",
                 "controller_binding_lost",
                 {{"binding_epoch", std::to_string(readiness.binding_epoch.value())},
                  {"dispatch_binding_epoch", std::to_string(execution.epoch.value())}}));
    return;
  }

  const bool measured = config_.stop.trip_authority == StateAuthorityClass::kMeasured;
  const bool goal_running = goal && goal->phase != ExecutionGoalPhase::kTerminal;
  // Q59 (`main`'s `joint_state_max_age_s`): during the motion the newest
  // controller state must be younger than the lane's maximum age; stale state
  // ends the motion like `main`'s other in-motion state faults (the goal is
  // cancelled and the execution fails).
  if (goal_running && config_.stop.controller_state_max_age_ns > 0 &&
      !(sample && sample->binding_epoch == execution.epoch &&
        sample->local_receipt_stamp_ns <= now &&
        now - sample->local_receipt_stamp_ns <= config_.stop.controller_state_max_age_ns)) {
    (void)port.Cancel(execution.key);
    Finish(execution, ExecutionState::kFailed,
           fault(ErrorCode::kExecutionAborted,
                 "The controller state is older than the lane's maximum age during execution",
                 "stale_controller_state",
                 {{"max_age_s",
                   format(static_cast<double>(config_.stop.controller_state_max_age_ns) / 1e9)},
                  {"sample_age_s",
                   sample ? format(static_cast<double>(now - sample->local_receipt_stamp_ns) / 1e9)
                          : std::string("none")}}));
    return;
  }
  if (usable && goal_running && execution.association->Observe(*sample, goal)) {
    // Progress cursor: the controller's own cursor of this goal (§3.3).
    ObserveSample(execution, *sample, sample->cursor_s, now);

    // Item 13 (as `main`'s host monitor): tolerance and timeout on the
    // controller-reported error, at the sample rate.
    if (config_.tracking && config_.tracking->timeout_s > 0.0 &&
        finite(sample->error_positions_rad, joints)) {
      std::size_t peak = 0U;
      for (std::size_t index = 1U; index < joints; ++index) {
        if (std::abs(sample->error_positions_rad[index]) >
            std::abs(sample->error_positions_rad[peak])) {
          peak = index;
        }
      }
      const double error = joints == 0U ? 0.0 : std::abs(sample->error_positions_rad[peak]);
      if (error > config_.tracking->tolerance_rad) {
        if (!execution.tracking_breach_since) execution.tracking_breach_since = now;
        if (static_cast<double>(now - *execution.tracking_breach_since) / 1e9 >=
            config_.tracking->timeout_s) {
          (void)port.Cancel(execution.key);
          Finish(execution, ExecutionState::kFailed,
                 fault(ErrorCode::kExecutionAborted,
                       "Controller tracking error exceeded tolerance: " + format(error) + " rad",
                       "tracking_error_exceeded",
                       {{"tracking_error_rad", format(error)},
                        {"tracking_error_tolerance_rad", format(config_.tracking->tolerance_rad)},
                        {"tracking_error_timeout_s", format(config_.tracking->timeout_s)},
                        {"trajectory_elapsed_s", format(sample->cursor_s)},
                        {"joint_name", lane_.joint_names[peak]},
                        {"signed_error_rad", format(sample->error_positions_rad[peak])}}));
          return;
        }
      } else {
        execution.tracking_breach_since.reset();
      }
    }
  }

  if (!goal) {
    Finish(execution, ExecutionState::kFailed,
           fault(ErrorCode::kExecutionAborted, "The controller no longer reports the goal",
                 "controller_result_failed", {{"goal_result", "unavailable"}}));
    return;
  }
  if (goal->phase != ExecutionGoalPhase::kTerminal || !goal->result) return;
  const auto result = *goal->result;
  if (result == ExecutionGoalResult::kCanceled) {
    // No addressed Stop owns it: the Kernel reports item 18.
    Finish(execution, ExecutionState::kStopped, std::nullopt);
    return;
  }
  if (result != ExecutionGoalResult::kSucceeded) {
    // As `main`: any unsuccessful controller result fails the motion; on real
    // lanes this includes a goal-tolerance violation (Q3).
    Finish(execution, ExecutionState::kFailed,
           fault(ErrorCode::kExecutionAborted,
                 std::string("FollowJointTrajectory goal failed: ") + to_string(result),
                 "controller_result_failed",
                 {{"goal_result", to_string(result)},
                  {"mechanism_goal_id", goal->mechanism_goal_id.value()}}));
    return;
  }
  // Completion (§4.1): kSucceeded plus `main`'s authority check.
  const auto& final_positions = execution.trajectory.waypoints.back().positions_rad;
  if (!measured) {
    execution.snapshot.joint_positions_rad = final_positions;
    Finish(execution, ExecutionState::kCompleted, std::nullopt);
    return;
  }
  if (!execution.success_observed_at) execution.success_observed_at = now;
  const auto result_at = goal->terminal_local_receipt_stamp.value_or(*execution.success_observed_at);
  // Q50: a sample received after the result (as `main`) and newer by source
  // stamp than the goal's newest evidence (or, with none, than the newest
  // sample seen before the goal was sent).
  if (usable && sample->local_receipt_stamp_ns > result_at &&
      execution.association->NewerThanEvidence(*sample)) {
    const auto positions =
        reorder(sample->measured_positions_rad, lane_.joint_names, execution.trajectory.joint_names);
    double max_error = 0.0;
    for (std::size_t index = 0U; positions && index < positions->size(); ++index) {
      max_error = std::max(max_error, std::abs((*positions)[index] - final_positions[index]));
    }
    if (positions) execution.snapshot.joint_positions_rad = *positions;
    if (positions && max_error <= *config_.final_tolerance_rad) {
      Finish(execution, ExecutionState::kCompleted, std::nullopt);
    } else {
      Finish(execution, ExecutionState::kFailed,
             fault(ErrorCode::kExecutionAborted,
                   "Final state mismatch: max joint error " + format(max_error) +
                       " rad exceeds tolerance " + format(*config_.final_tolerance_rad),
                   "tracking_error_exceeded",
                   {{"phase", "terminal_result_state_gate"},
                    {"final_tolerance_rad", format(*config_.final_tolerance_rad)}}));
    }
    return;
  }
  if (now - result_at >= config_.completion_state_timeout_ns) {
    Finish(execution, ExecutionState::kFailed,
           fault(ErrorCode::kExecutionAborted,
                 "No fresh controller sample was received after the successful result",
                 "stale_controller_state",
                 {{"phase", "terminal_result_state_gate"},
                  {"wait_for_state_timeout_s",
                   format(static_cast<double>(config_.completion_state_timeout_ns) / 1e9)},
                  {"final_tolerance_rad", format(*config_.final_tolerance_rad)}}));
  }
}

// --- Stop -----------------------------------------------------------------------

ParticipantStopStageResultDTO ExecutionPortParticipant::TranslateStopStage(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    std::stop_token cancellation) {
  std::shared_ptr<ParticipantStopStageHelper> helper;
  std::shared_ptr<Execution> execution;
  {
    std::lock_guard lock(mutex_);
    helper = helper_;
    execution = execution_;
    ++stop_stages_in_flight_;
    if (execution && !execution->terminal) execution->stopping = true;
  }
  auto result = helper->TranslateStopStage(stage, budget, std::move(cancellation));
  std::lock_guard lock(mutex_);
  --stop_stages_in_flight_;
  if (execution && execution->stopping) {
    execution->last_stage_outcome = result.outcome;
    execution->last_stage_error = result.error;
    Evaluate(*execution);
  }
  return result;
}

// --- Speed factor (Q11, Q17, Q35, Q38) ------------------------------------------

void ExecutionPortParticipant::set_runtime_velocity_scale(double scale) {
  scale = std::isfinite(scale) ? std::clamp(scale, 0.0, 1.0) : 0.0;
  std::lock_guard lock(mutex_);
  if (!lane_.supports_scale) return;
  std::lock_guard scale_lock(scale_mutex_);
  target_scale_ = scale;
  if (scale_frozen_) return;  // kept for the next motion (Q17)
  if (ramp_) {
    ramp_->target = scale;
    if (ramp_->value != ramp_->target && ramp_handle_ == 0U) {
      last_ramp_tick_ = clock_.now();
      ScheduleRampTickLocked();
    }
    return;
  }
  // No motion: nothing moves, the value applies to the next goal's start.
  PublishScaleLocked(scale);
}

void ExecutionPortParticipant::PublishScaleLocked(const double value) {
  if (!port_) return;
  if (port_->SetScale(value).dispatch != ExecutionPortDispatch::kNotDispatched) {
    last_sent_scale_ = value;
  }
}

void ExecutionPortParticipant::ScheduleRampTickLocked() {
  const auto period_ns = static_cast<Duration>(1e9 / lane_.controller_update_rate_hz);
  const auto generation = ramp_generation_;
  ramp_handle_ = scheduler_.schedule(
      clock_.now() + std::max<Duration>(1, period_ns),
      [this, generation] { RampTick(generation); }, SchedulerPriority::ControllerCommand);
}

void ExecutionPortParticipant::RampTick(const std::uint64_t ramp_generation) {
  std::lock_guard lock(mutex_);
  std::lock_guard scale_lock(scale_mutex_);
  if (ramp_generation != ramp_generation_) return;
  ramp_handle_ = 0U;
  if (scale_frozen_ || !ramp_) return;
  // One value per host tick, computed from the time at this tick (no backlog).
  const auto now = clock_.now();
  const double step =
      ramp_->rate_per_s * static_cast<double>(std::max<Duration>(0, now - last_ramp_tick_)) / 1e9;
  last_ramp_tick_ = now;
  ramp_->value = ramp_->value < ramp_->target ? std::min(ramp_->target, ramp_->value + step)
                                              : std::max(ramp_->target, ramp_->value - step);
  PublishScaleLocked(ramp_->value);
  if (ramp_->value != ramp_->target) ScheduleRampTickLocked();
}

double ExecutionPortParticipant::FreezeSpeedFactor() {
  std::lock_guard scale_lock(scale_mutex_);
  scale_frozen_ = true;
  ++ramp_generation_;
  if (ramp_handle_ != 0U) scheduler_.cancel(ramp_handle_);
  ramp_handle_ = 0U;
  return last_sent_scale_.value_or(config_.initial_speed_factor);
}

RuntimeScaleMechanismStatusDTO ExecutionPortParticipant::runtime_velocity_scale_status() const {
  std::lock_guard lock(mutex_);
  RuntimeScaleMechanismStatusDTO status;
  status.supported = lane_.supports_scale;
  status.active = execution_ && !execution_->terminal;
  {
    std::lock_guard scale_lock(scale_mutex_);
    status.commanded_scale = target_scale_;
  }
  // Published is not applied: the applied factor is the controller's own
  // report; before the first report the epoch's initial factor applies.
  status.applied_scale = config_.initial_speed_factor;
  const auto sample = port_ ? port_->ReadSample() : std::nullopt;
  if (sample && sample->applied_factor && std::isfinite(*sample->applied_factor) &&
      sample->binding_epoch == port_->ReadReadiness().binding_epoch) {
    status.applied_scale = std::clamp(*sample->applied_factor, 0.0, 1.0);
    status.acknowledgement_sequence = sample->local_receipt_count.value();
    status.acknowledgement_timestamp_ns = sample->local_receipt_stamp_ns;
  }
  if (execution_) status.virtual_trajectory_time_s = execution_->cursor_s;
  return status;
}

}  // namespace manyforge::core
