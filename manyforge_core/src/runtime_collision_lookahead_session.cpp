#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>
#include <manyforge_core/collision/collision_resource_manager.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/runtime_collision_lookahead_session.hpp>
#include <stdexcept>
#include <utility>

namespace manyforge::core {
namespace {

const char* runtime_snapshot_prepare_status_name(
    RuntimeCollisionQueryPrepareStatus status) noexcept {
  switch (status) {
    case RuntimeCollisionQueryPrepareStatus::kReady:
      return "ready";
    case RuntimeCollisionQueryPrepareStatus::kUnsupported:
      return "unsupported";
    case RuntimeCollisionQueryPrepareStatus::kJointCountExceeded:
      return "joint_count_exceeded";
    case RuntimeCollisionQueryPrepareStatus::kInvalidJointOrder:
      return "invalid_joint_order";
    case RuntimeCollisionQueryPrepareStatus::kEngineBusy:
      return "engine_busy";
    case RuntimeCollisionQueryPrepareStatus::kEngineError:
      return "engine_error";
  }
  return "unknown";
}

std::int64_t seconds_to_ns(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0.0 ||
      seconds > static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1e9) {
    return -1;
  }
  return static_cast<std::int64_t>(seconds * 1e9);
}

bool revisions_equal(const CollisionWorldRevision& lhs, const CollisionWorldRevision& rhs) {
  return lhs.revision_id == rhs.revision_id && lhs.content_hash == rhs.content_hash;
}

std::optional<CollisionPairId> first_failure_pair(const CollisionValidationDecisionBundle& bundle) {
  if (!bundle.decision.first_failing_waypoint) {
    return std::nullopt;
  }
  for (const auto& record : bundle.waypoint_records) {
    if (record.waypoint_index != *bundle.decision.first_failing_waypoint) {
      continue;
    }
    for (const auto& pair : record.result.normalized_pairs_classified) {
      if (pair.is_first_failure ||
          pair.classification == CollisionPairClassification::kBlockingCollision ||
          pair.classification == CollisionPairClassification::kSupportContactRecontactViolation) {
        return pair.pair;
      }
    }
    if (!record.result.normalized_pairs.empty()) {
      return record.result.normalized_pairs.front();
    }
  }
  return std::nullopt;
}

}  // namespace

const char* runtime_collision_lookahead_termination_name(
    RuntimeCollisionLookaheadTermination termination) noexcept {
  switch (termination) {
    case RuntimeCollisionLookaheadTermination::kTrajectoryEnd:
      return "trajectory_end";
    case RuntimeCollisionLookaheadTermination::kTimeHorizon:
      return "time_horizon";
    case RuntimeCollisionLookaheadTermination::kNoFutureSamples:
      return "no_future_samples";
    case RuntimeCollisionLookaheadTermination::kInvalidInput:
      return "invalid_input";
  }
  return "invalid_input";
}

const char* runtime_collision_lookahead_error_message(
    std::string_view reason) noexcept {
  if (reason == "runtime_lookahead_deadline_exceeded") {
    return "Runtime trajectory lookahead missed its cycle deadline";
  }
  if (reason == "runtime_lookahead_query_timeout") {
    return "Runtime trajectory lookahead collision query exceeded its time budget";
  }
  if (reason == "runtime_lookahead_engine_error") {
    return "Runtime trajectory lookahead collision engine failed";
  }
  if (reason == "runtime_lookahead_unsupported_geometry") {
    return "Runtime trajectory lookahead encountered unsupported collision geometry";
  }
  if (reason == "runtime_lookahead_diagnostic_incomplete") {
    return "Runtime trajectory lookahead collision evidence was incomplete";
  }
  return "Runtime trajectory lookahead could not certify the bounded future";
}

namespace {

void reset_lookahead_selection(RuntimeCollisionLookaheadSelection& out,
                               std::int64_t cursor_time_ns) {
  out.trajectory.joint_names.clear();
  out.trajectory.waypoints.clear();
  out.source_waypoint_indices.clear();
  out.original_trajectory_sample_fractions.clear();
  out.sample_times_ns.clear();
  out.eligible_source_waypoints = 0;
  out.sampling_reduced = false;
  out.termination = RuntimeCollisionLookaheadTermination::kInvalidInput;
  out.reaches_trajectory_end = false;
  out.cursor_time_ns = cursor_time_ns;
  out.covered_until_time_ns = cursor_time_ns;
  out.error_reason.clear();
}

bool build_lookahead_waypoint_time_index(
    const TrajectoryDTO& trajectory, std::vector<std::int64_t>& waypoint_times,
    std::string& error_reason, IClock* clock = nullptr,
    std::optional<TimePoint> absolute_deadline = std::nullopt) {
  waypoint_times.clear();
  waypoint_times.reserve(trajectory.waypoints.size());
  std::int64_t previous_time_ns = -1;
  for (std::size_t index = 0; index < trajectory.waypoints.size(); ++index) {
    if (clock != nullptr && absolute_deadline && (index & 63u) == 0u &&
        clock->now() >= *absolute_deadline) {
      error_reason = "runtime_lookahead_deadline_exceeded";
      return false;
    }
    const auto& waypoint = trajectory.waypoints[index];
    const auto time_ns = seconds_to_ns(waypoint.time_from_start_s);
    if (time_ns < 0 || time_ns <= previous_time_ns ||
        waypoint.positions_rad.size() != trajectory.joint_names.size() ||
        !std::all_of(waypoint.positions_rad.begin(), waypoint.positions_rad.end(),
                     [](double value) { return std::isfinite(value); })) {
      error_reason = "lookahead_trajectory_malformed";
      return false;
    }
    waypoint_times.push_back(time_ns);
    previous_time_ns = time_ns;
  }
  return true;
}

std::int64_t quadratic_target_time(std::int64_t cursor_time_ns,
                                   std::int64_t window_end_ns,
                                   std::size_t index,
                                   std::size_t sample_count) {
  const auto span = static_cast<std::uint64_t>(window_end_ns - cursor_time_ns);
  const auto denominator = static_cast<std::uint64_t>(sample_count) * sample_count;
  const auto numerator = static_cast<std::uint64_t>(index) * index;
  const auto quotient = span / denominator;
  const auto remainder = span % denominator;
  const auto offset = quotient * numerator + (remainder * numerator) / denominator;
  return cursor_time_ns + static_cast<std::int64_t>(offset);
}

void build_quadratic_targets(std::int64_t cursor_time_ns,
                             std::int64_t window_end_ns,
                             std::size_t max_samples,
                             std::vector<std::int64_t>& targets) {
  targets.clear();
  const auto span = window_end_ns - cursor_time_ns;
  if (span <= 0 || max_samples == 0) {
    return;
  }
  const auto count = static_cast<std::size_t>(
      std::min<std::int64_t>(span, static_cast<std::int64_t>(max_samples)));
  targets.reserve(count);
  std::int64_t previous = cursor_time_ns;
  for (std::size_t index = 1; index <= count; ++index) {
    auto target = quadratic_target_time(cursor_time_ns, window_end_ns, index, count);
    const auto slots_after = static_cast<std::int64_t>(count - index);
    target = std::max(target, previous + 1);
    target = std::min(target, window_end_ns - slots_after);
    targets.push_back(target);
    previous = target;
  }
  targets.back() = window_end_ns;
}

void merge_required_and_fill_targets(
    const std::vector<std::int64_t>& required,
    const std::vector<std::int64_t>& quadratic,
    std::size_t desired_count,
    std::vector<std::int64_t>& selected) {
  selected.clear();
  selected.reserve(desired_count);
  const auto extras_needed = desired_count - required.size();
  if (extras_needed == 0) {
    selected = required;
    return;
  }
  selected.insert(selected.end(), required.begin(), required.end());
  std::size_t extras_added = 0;
  for (const auto candidate : quadratic) {
    if (std::binary_search(required.begin(), required.end(), candidate)) {
      continue;
    }
    selected.push_back(candidate);
    if (++extras_added == extras_needed) {
      break;
    }
  }
  std::sort(selected.begin(), selected.end());
}

void select_runtime_collision_lookahead_samples_from_index(
    const TrajectoryDTO& trajectory, const std::vector<std::int64_t>& waypoint_times,
    std::int64_t cursor_time_ns, std::int64_t horizon_ns,
    std::size_t max_samples, RuntimeCollisionLookaheadSelection& out,
    std::vector<std::int64_t>& required_times,
    std::vector<std::int64_t>& target_times,
    TrajectorySampleResult& sample_scratch) {
  reset_lookahead_selection(out, cursor_time_ns);
  if (trajectory.joint_names.empty() || trajectory.waypoints.empty() ||
      waypoint_times.size() != trajectory.waypoints.size() || cursor_time_ns < 0 ||
      horizon_ns <= 0 || max_samples < 2 || max_samples > 4096) {
    out.error_reason = "lookahead_selection_invalid_arguments";
    return;
  }

  const auto trajectory_end_ns = waypoint_times.back();
  if (cursor_time_ns < waypoint_times.front()) {
    out.error_reason = "lookahead_cursor_precedes_trajectory_anchor";
    return;
  }
  if (cursor_time_ns >= trajectory_end_ns) {
    out.termination = RuntimeCollisionLookaheadTermination::kNoFutureSamples;
    out.reaches_trajectory_end = true;
    out.covered_until_time_ns = trajectory_end_ns;
    return;
  }
  const auto requested_end_ns =
      cursor_time_ns > std::numeric_limits<std::int64_t>::max() - horizon_ns
          ? std::numeric_limits<std::int64_t>::max()
          : cursor_time_ns + horizon_ns;
  const auto window_end_ns = std::min(requested_end_ns, trajectory_end_ns);
  const auto fraction_denominator =
      static_cast<double>(std::max<std::size_t>(trajectory.waypoints.size() - 1, 1));
  const auto first_source = static_cast<std::size_t>(
      std::upper_bound(waypoint_times.begin(), waypoint_times.end(), cursor_time_ns) -
      waypoint_times.begin());
  const auto after_window = static_cast<std::size_t>(
      std::upper_bound(waypoint_times.begin(), waypoint_times.end(), window_end_ns) -
      waypoint_times.begin());
  out.eligible_source_waypoints = after_window - first_source;

  required_times.clear();
  required_times.reserve(std::min(max_samples, out.eligible_source_waypoints + 1));
  const auto endpoint_is_source =
      after_window > first_source && waypoint_times[after_window - 1] == window_end_ns;
  const auto required_count = out.eligible_source_waypoints + (endpoint_is_source ? 0u : 1u);
  if (required_count <= max_samples) {
    required_times.insert(required_times.end(), waypoint_times.begin() + first_source,
                          waypoint_times.begin() + after_window);
    if (!endpoint_is_source) {
      required_times.push_back(window_end_ns);
    }
    const auto available_nanoseconds = static_cast<std::uint64_t>(
        window_end_ns - cursor_time_ns);
    const auto desired_count = static_cast<std::size_t>(
        std::min<std::uint64_t>(max_samples, available_nanoseconds));
    // Generate enough deterministic candidates that even complete overlap
    // with every mandatory source knot still leaves enough distinct times to
    // fill the bounded selection whenever the nanosecond domain permits it.
    const auto candidate_count = static_cast<std::size_t>(
        std::min<std::uint64_t>(available_nanoseconds,
                                max_samples + required_times.size()));
    build_quadratic_targets(cursor_time_ns, window_end_ns, candidate_count,
                            target_times);
    merge_required_and_fill_targets(required_times, target_times, desired_count,
                                    out.sample_times_ns);
  } else {
    out.sampling_reduced = true;
    build_quadratic_targets(cursor_time_ns, window_end_ns, max_samples,
                            target_times);
    out.sample_times_ns = target_times;
  }

  if (out.sample_times_ns.empty() || out.sample_times_ns.back() != window_end_ns ||
      !std::is_sorted(out.sample_times_ns.begin(), out.sample_times_ns.end()) ||
      std::adjacent_find(out.sample_times_ns.begin(), out.sample_times_ns.end()) !=
          out.sample_times_ns.end()) {
    out.error_reason = "lookahead_sample_times_invalid";
    return;
  }

  out.trajectory.joint_names = trajectory.joint_names;
  out.trajectory.waypoints.reserve(out.sample_times_ns.size());
  out.source_waypoint_indices.reserve(out.sample_times_ns.size());
  out.original_trajectory_sample_fractions.reserve(out.sample_times_ns.size());
  auto upper_index = static_cast<std::size_t>(
      std::lower_bound(waypoint_times.begin(), waypoint_times.end(),
                       out.sample_times_ns.front()) -
      waypoint_times.begin());
  for (const auto sample_time_ns : out.sample_times_ns) {
    while (upper_index < waypoint_times.size() &&
           waypoint_times[upper_index] < sample_time_ns) {
      ++upper_index;
    }
    if (upper_index < waypoint_times.size() &&
        waypoint_times[upper_index] == sample_time_ns) {
      out.trajectory.waypoints.push_back(trajectory.waypoints[upper_index]);
      out.source_waypoint_indices.push_back(static_cast<std::int64_t>(upper_index));
      out.original_trajectory_sample_fractions.push_back(
          static_cast<double>(upper_index) / fraction_denominator);
      continue;
    }
    if (upper_index == 0 || upper_index >= waypoint_times.size()) {
      out.error_reason = "lookahead_sample_outside_trajectory";
      out.termination = RuntimeCollisionLookaheadTermination::kInvalidInput;
      return;
    }
    const auto sample_s = static_cast<double>(sample_time_ns) / 1e9;
    const auto sampled = sample_trajectory_prevalidated(
        trajectory, sample_s, upper_index - 1, sample_scratch);
    if (!sampled.ok() ||
        !std::all_of(sample_scratch.positions_rad.begin(),
                     sample_scratch.positions_rad.end(),
                     [](double value) { return std::isfinite(value); })) {
      out.error_reason = "lookahead_trajectory_sample_failed";
      out.termination = RuntimeCollisionLookaheadTermination::kInvalidInput;
      return;
    }
    TrajectoryWaypointDTO waypoint;
    waypoint.time_from_start_s = sample_s;
    waypoint.positions_rad = sample_scratch.positions_rad;
    waypoint.velocities_rad_s = sample_scratch.velocities_rad_s;
    waypoint.accelerations_rad_s2 = sample_scratch.accelerations_rad_s2;
    out.trajectory.waypoints.push_back(std::move(waypoint));
    out.source_waypoint_indices.push_back(-1);
    const auto lower_index = upper_index - 1;
    const auto segment_ns = waypoint_times[upper_index] - waypoint_times[lower_index];
    const auto alpha = static_cast<double>(sample_time_ns - waypoint_times[lower_index]) /
                       static_cast<double>(segment_ns);
    out.original_trajectory_sample_fractions.push_back(
        (static_cast<double>(lower_index) + alpha) / fraction_denominator);
  }
  out.covered_until_time_ns = window_end_ns;
  out.reaches_trajectory_end = window_end_ns == trajectory_end_ns;
  out.termination = out.reaches_trajectory_end
                        ? RuntimeCollisionLookaheadTermination::kTrajectoryEnd
                        : RuntimeCollisionLookaheadTermination::kTimeHorizon;
}

}  // namespace

void select_runtime_collision_lookahead_samples(
    const TrajectoryDTO& trajectory, std::int64_t cursor_time_ns,
    std::int64_t horizon_ns, std::size_t max_samples,
    RuntimeCollisionLookaheadSelection& out) {
  reset_lookahead_selection(out, cursor_time_ns);
  if (trajectory.joint_names.empty() || trajectory.waypoints.empty() || cursor_time_ns < 0 ||
      horizon_ns <= 0 || max_samples < 2 || max_samples > 4096) {
    out.error_reason = "lookahead_selection_invalid_arguments";
    return;
  }
  std::vector<std::int64_t> waypoint_times;
  if (!build_lookahead_waypoint_time_index(trajectory, waypoint_times, out.error_reason)) {
    return;
  }
  std::vector<std::int64_t> target_times;
  std::vector<std::int64_t> required_times;
  TrajectorySampleResult sample_scratch;
  select_runtime_collision_lookahead_samples_from_index(
      trajectory, waypoint_times, cursor_time_ns, horizon_ns, max_samples, out,
      required_times, target_times, sample_scratch);
}

RuntimeCollisionLookaheadSelection select_runtime_collision_lookahead_samples(
    const TrajectoryDTO& trajectory, std::int64_t cursor_time_ns, std::int64_t horizon_ns,
    std::size_t max_samples) {
  RuntimeCollisionLookaheadSelection out;
  select_runtime_collision_lookahead_samples(trajectory, cursor_time_ns, horizon_ns,
                                             max_samples, out);
  return out;
}

RuntimeCollisionLookaheadSession::RuntimeCollisionLookaheadSession(
    Config config, IClock& clock, IScheduler& scheduler,
    ICollisionWorldSnapshotPublisher& publisher,
    const ExecutionProgressPublication& progress_publication,
    std::vector<std::shared_ptr<ICollisionQueryEngine>> engines,
    ResultCallback callback, AuthorityCallback authority_callback)
    : config_(config),
      clock_(&clock),
      publisher_(&publisher),
      progress_publication_(&progress_publication),
      engines_(std::move(engines)),
      callback_(std::move(callback)),
      authority_callback_(std::move(authority_callback)),
      pool_([&]() {
        ParallelCollisionValidationWorkerPool::Config pool_config;
        pool_config.worker_count = config.workers;
        pool_config.min_waypoints_for_parallel = config.min_waypoints_for_parallel;
        pool_config.validation_wall_timeout =
            std::chrono::nanoseconds(config.cycle_budget_ns);
        pool_config.clock = &clock;
        pool_config.scheduler = &scheduler;
        return pool_config;
      }()) {
  if (config_.cycle_budget_ns <= 0 || config_.horizon_ns <= 0 ||
      config_.max_samples < 2 || config_.max_samples > 4096 ||
      config_.workers < 1 || config_.workers > 64 ||
      config_.query_budget_ns <= 0 ||
      config_.cycle_budget_ns < config_.query_budget_ns ||
      !callback_ || !authority_callback_) {
    throw std::invalid_argument("runtime collision lookahead config invalid");
  }
  const auto capacity = CollisionResourceManager::inspect_validation_worker_capacity(engines_);
  if (capacity.mismatch_reason ||
      capacity.effective_contexts != static_cast<std::size_t>(config_.workers)) {
    throw std::invalid_argument("runtime lookahead workers are not backed by independent contexts");
  }
  status_.requested_cycle_budget_ns = config_.cycle_budget_ns;
  status_.requested_query_budget_ns = config_.query_budget_ns;
  status_.requested_horizon_ns = config_.horizon_ns;
  status_.requested_max_samples = config_.max_samples;
  status_.requested_workers = config_.workers;
  status_.effective_workers = static_cast<int>(capacity.effective_contexts);
  selection_scratch_.trajectory.waypoints.reserve(config_.max_samples);
  selection_scratch_.source_waypoint_indices.reserve(config_.max_samples);
  selection_scratch_.original_trajectory_sample_fractions.reserve(config_.max_samples);
  selection_scratch_.sample_times_ns.reserve(config_.max_samples);
  required_times_scratch_.reserve(config_.max_samples);
  target_times_scratch_.reserve(config_.max_samples * 2);
  worker_ = std::jthread([this](std::stop_token stop_token) { worker_loop(stop_token); });
}

RuntimeCollisionLookaheadSession::~RuntimeCollisionLookaheadSession() {
  disarm();
  worker_.request_stop();
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void RuntimeCollisionLookaheadSession::arm(std::uint64_t execution_id,
                                           std::uint64_t execution_generation,
                                           std::shared_ptr<const TrajectoryDTO> trajectory,
                                           std::string contact_policy_id) {
  if (execution_id == 0 || execution_generation == 0 || !trajectory ||
      trajectory->waypoints.empty()) {
    throw std::invalid_argument("runtime collision lookahead arm invalid");
  }
  accepting_clear_permits_.store(false, std::memory_order_release);
  const auto arm_permit_generation =
      permit_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto active_cancel = active_cancel_.load(std::memory_order_acquire)) {
    active_cancel->request_cancel("lookahead_rearmed");
  }
  if (pending_job_ && pending_job_->cancel) {
    pending_job_->cancel->request_cancel("lookahead_rearmed");
  }
  arm_ = ArmState{execution_id, execution_generation, next_arm_generation_++, std::move(trajectory),
                  std::move(contact_policy_id)};
  pending_job_.reset();
  indexed_arm_generation_ = 0;
  // A stop/reset revocation can invalidate the permit generation without
  // waiting for this leaf mutex. Reopen only when no such authority event
  // raced the arm; otherwise the next execution arm must authorize work.
  const bool permit_authorized =
      permit_generation_.load(std::memory_order_acquire) ==
      arm_permit_generation;
  accepting_clear_permits_.store(permit_authorized,
                                 std::memory_order_release);
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_.execution_id = execution_id;
    status_.execution_generation = execution_generation;
    status_.arm_generation = arm_.arm_generation;
    status_.armed = true;
    status_.permit_pending = false;
    status_.accepting_clear_permits = permit_authorized;
  }
}

void RuntimeCollisionLookaheadSession::disarm() noexcept {
  accepting_clear_permits_.store(false, std::memory_order_release);
  permit_generation_.fetch_add(1, std::memory_order_acq_rel);
  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto active_cancel = active_cancel_.load(std::memory_order_acquire)) {
    active_cancel->request_cancel("lookahead_disarmed");
  }
  arm_ = {};
  pending_job_.reset();
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_.armed = false;
    status_.job_active = job_active_;
    status_.permit_pending = false;
    status_.accepting_clear_permits = false;
  }
}

void RuntimeCollisionLookaheadSession::request(
    std::int64_t requested_at_ns, RuntimeCollisionQueryBundlePtr bundle,
    RuntimeContactReleaseStateSnapshot contact_state) noexcept {
  if (!accepting_clear_permits_.load(std::memory_order_acquire) || !bundle) {
    return;
  }
  const auto permit_generation = permit_generation_.load(std::memory_order_acquire);
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    clear_permit_try_lock_misses_total_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!accepting_clear_permits_.load(std::memory_order_acquire) ||
      permit_generation_.load(std::memory_order_acquire) != permit_generation ||
      !arm_.trajectory || arm_.execution_generation == 0) {
    return;
  }
  if (job_active_ || pending_job_) {
    reruns_coalesced_total_.fetch_add(1, std::memory_order_relaxed);
  }
  // Keep this immediate-monitor path allocation-free. The worker copies the
  // trajectory/policy arm state and allocates its cancellation owner after it
  // dequeues this fixed-size generation-bound request.
  Job pending;
  pending.arm.execution_id = arm_.execution_id;
  pending.arm.execution_generation = arm_.execution_generation;
  pending.arm.arm_generation = arm_.arm_generation;
  pending.requested_at_ns = requested_at_ns;
  pending.bundle = std::move(bundle);
  pending.contact_state = std::move(contact_state);
  // The immediate-clear observation, scene bundle, and authoritative progress
  // cursor form one permit. Re-reading progress on the lower-priority worker
  // could otherwise apply an old clear observation to a newer robot state.
  pending.cursor = progress_publication_->acquire();
  if (pending.cursor.valid && pending.cursor.terminal &&
      pending.cursor.execution_generation == arm_.execution_generation) {
    return;
  }
  pending.permit_generation = permit_generation;
  pending_job_.emplace(std::move(pending));
  {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_.permit_pending = true;
  }
  cv_.notify_one();
}

void RuntimeCollisionLookaheadSession::revoke_clear_permits() noexcept {
  const bool permission_was_open =
      accepting_clear_permits_.exchange(false, std::memory_order_acq_rel);
  permit_generation_.fetch_add(1, std::memory_order_acq_rel);
  if (permission_was_open) {
    clear_permits_invalidated_total_.fetch_add(1,
                                               std::memory_order_relaxed);
  }
  if (const auto active_cancel = active_cancel_.load(std::memory_order_acquire)) {
    active_cancel->request_cancel("lookahead_clear_permit_revoked");
  }
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock()) {
    return;
  }
  pending_job_.reset();
  std::lock_guard<std::mutex> status_lock(status_mutex_);
  status_.permit_pending = false;
  status_.accepting_clear_permits = false;
}

void RuntimeCollisionLookaheadSession::worker_loop(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    Job job;
    auto cancel = std::make_shared<CollisionCancellationToken>();
    bool admitted = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, stop_token, [this]() { return pending_job_.has_value(); });
      if (stop_token.stop_requested()) {
        return;
      }
      job = std::move(*pending_job_);
      pending_job_.reset();
      admitted = arm_.arm_generation == job.arm.arm_generation &&
                 arm_.execution_generation == job.arm.execution_generation &&
                 arm_.trajectory != nullptr &&
                 accepting_clear_permits_.load(std::memory_order_acquire) &&
                 permit_generation_.load(std::memory_order_acquire) ==
                     job.permit_generation &&
                 !progress_generation_is_terminal(
                     job.arm.execution_generation);
      if (admitted) {
        job.arm.trajectory = arm_.trajectory;
        job.arm.contact_policy_id = arm_.contact_policy_id;
        job.cancel = std::move(cancel);
        active_cancel_.store(job.cancel, std::memory_order_release);
        job_active_ = true;
        active_job_started_at_ns_ = clock_->now();
        if (duty_window_started_at_ns_ < 0) {
          duty_window_started_at_ns_ = active_job_started_at_ns_;
        }
      }
      {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_.job_active = admitted;
        status_.permit_pending = false;
        status_.accepting_clear_permits =
            accepting_clear_permits_.load(std::memory_order_acquire);
        if (admitted) {
          ++status_.jobs_started_total;
          if (last_job_completed_at_ns_ >= 0) {
            status_.last_inter_job_gap_ns =
                std::max<std::int64_t>(0, active_job_started_at_ns_ -
                                             last_job_completed_at_ns_);
          }
        } else {
          ++status_.stale_generation_results_total;
        }
      }
    }

    if (!admitted) {
      continue;
    }

    auto result = run_job(job);
    const auto requires_authority = [&result]() {
      return result.status == RuntimeCollisionLookaheadResultStatus::kCollision ||
             result.status == RuntimeCollisionLookaheadResultStatus::kInconclusive;
    };
    const auto refresh_completion_deadline = [&]() {
      const auto now_ns = clock_->now();
      result.cycle_duration_ns =
          std::max<std::int64_t>(0, now_ns - job.requested_at_ns);
      if (now_ns < result.cycle_deadline_ns) {
        return;
      }
      result.cycle_deadline_exceeded = true;
      if (!requires_authority()) {
        result.status = RuntimeCollisionLookaheadResultStatus::kInconclusive;
        result.reason = "runtime_lookahead_deadline_exceeded";
      }
    };
    const auto current_arm = [&]() {
      return arm_.arm_generation == job.arm.arm_generation &&
             arm_.execution_generation == job.arm.execution_generation &&
             permit_generation_.load(std::memory_order_acquire) ==
                 job.permit_generation &&
             accepting_clear_permits_.load(std::memory_order_acquire) &&
             !progress_generation_is_terminal(job.arm.execution_generation);
    };

    bool stale_arm = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stale_arm = !current_arm();
    }

    // The complete-cycle deadline includes any admission delay before the
    // kernel authority handoff. Convert an otherwise-clear result before that
    // handoff so a late clear can never be published as authoritative.
    refresh_completion_deadline();
    bool authority_completed = false;
    const auto apply_authority_if_required = [&]() {
      if (stale_arm || authority_completed || !requires_authority()) {
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stale_arm = !current_arm();
        if (!stale_arm) {
          if (accepting_clear_permits_.exchange(
                  false, std::memory_order_acq_rel)) {
            clear_permits_invalidated_total_.fetch_add(
                1, std::memory_order_relaxed);
          }
          pending_job_.reset();
        }
      }
      authority_completed = !stale_arm;
      if (!stale_arm && authority_callback_) {
        try {
          stale_arm = authority_callback_(this, result) ==
                      RuntimeCollisionLookaheadAuthorityDisposition::kStale;
        } catch (...) {
          // Authority callbacks are part of the fail-closed stop path. An
          // escaped exception means the process can no longer prove that the
          // detected/inconclusive hazard reached stop authority.
          std::terminate();
        }
      }
    };
    apply_authority_if_required();

    // This is the requested-to-authority boundary. Collision/inconclusive
    // results have returned from kernel authority; clear results needed no
    // authority call. Observer formatting and event fan-out are deliberately
    // excluded and happen only after this value is committed.
    refresh_completion_deadline();
    // A result can cross the absolute deadline in the narrow interval between
    // the pre-authority check and this completion stamp. If it was otherwise
    // clear, apply fail-closed authority exactly once before exposing it.
    apply_authority_if_required();
    refresh_completion_deadline();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!requires_authority()) {
        stale_arm = stale_arm || !current_arm();
      }
      active_cancel_.store(nullptr, std::memory_order_release);
      job_active_ = false;
      const auto completed_at_ns = clock_->now();
      const auto active_duration_ns = std::max<std::int64_t>(
          0, completed_at_ns - active_job_started_at_ns_);
      result.worker_active_duration_ns = active_duration_ns;
      last_job_completed_at_ns_ = completed_at_ns;
      {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_.job_active = false;
        status_.permit_pending = pending_job_.has_value();
        status_.accepting_clear_permits =
            accepting_clear_permits_.load(std::memory_order_acquire);
        status_.active_time_total_ns += active_duration_ns;
        status_.duty_window_ns = std::max<std::int64_t>(
            0, completed_at_ns - duty_window_started_at_ns_);
        status_.duty_cycle_ppm = status_.duty_window_ns > 0
                                     ? std::min<std::uint64_t>(
                                           1'000'000,
                                           static_cast<std::uint64_t>(
                                               static_cast<long double>(
                                                   status_.active_time_total_ns) *
                                               1'000'000.0L /
                                               static_cast<long double>(
                                                   status_.duty_window_ns)))
                                     : 0;
        if (stale_arm) {
          ++status_.stale_generation_results_total;
        }
      }
    }
    if (!stale_arm) {
      // Stop authority is complete before either metrics or event evidence is
      // observable. Recording first also gives result observers one coherent
      // status snapshot when their callback returns.
      record_result(result);
      try {
        callback_(this, result);
      } catch (...) {
        // Production result observers emit kernel evidence. Losing that
        // evidence after authority is a process-integrity failure, matching
        // the existing kernel event-publisher policy.
        std::terminate();
      }
    }
  }
}

RuntimeCollisionLookaheadResult RuntimeCollisionLookaheadSession::run_job(const Job& job) {
  RuntimeCollisionLookaheadResult result;
  result.execution_id = job.arm.execution_id;
  result.execution_generation = job.arm.execution_generation;
  result.arm_generation = job.arm.arm_generation;
  result.permit_generation = job.permit_generation;
  result.requested_at_ns = job.requested_at_ns;
  result.start_jitter_ns = std::max<std::int64_t>(0, clock_->now() - job.requested_at_ns);
  const auto absolute_deadline_ns =
      job.requested_at_ns >
              std::numeric_limits<std::int64_t>::max() - config_.cycle_budget_ns
          ? std::numeric_limits<std::int64_t>::max()
          : job.requested_at_ns + config_.cycle_budget_ns;
  result.cycle_deadline_ns = absolute_deadline_ns;
  const auto finish = [&](RuntimeCollisionLookaheadResult output) {
    // Complete-cycle latency starts at clear-permit admission. Worker-active
    // time and start jitter are tracked independently for CPU/interference
    // analysis.
    output.cycle_duration_ns =
        std::max<std::int64_t>(0, clock_->now() - job.requested_at_ns);
    return output;
  };
  const auto deadline_exceeded = [&]() { return clock_->now() >= absolute_deadline_ns; };

  if (deadline_exceeded()) {
    result.reason = "runtime_lookahead_deadline_exceeded";
    return finish(std::move(result));
  }

  const auto& cursor = job.cursor;
  if (!cursor.valid || cursor.execution_generation != job.arm.execution_generation) {
    result.reason = !cursor.valid ? "progress_cursor_invalid" : "progress_generation_mismatch";
    return finish(std::move(result));
  }
  const auto now_ns = clock_->now();
  if (cursor.freshness_budget_ns > 0 &&
      (cursor.received_timestamp_ns < 0 || cursor.received_timestamp_ns > now_ns ||
       now_ns - cursor.received_timestamp_ns > cursor.freshness_budget_ns)) {
    result.reason = "progress_cursor_stale";
    return finish(std::move(result));
  }
  result.cursor_time_ns = cursor.trajectory_time_ns;

  if (!job.bundle || !job.bundle->world || !job.bundle->policy || !job.bundle->policy_index ||
      !job.bundle->lookup_order_valid) {
    result.reason = "runtime_lookahead_bundle_invalid";
    return finish(std::move(result));
  }
  result.runtime_bundle_epoch = job.bundle->epoch;
  result.collision_world_revision = job.bundle->world->revision;
  const auto freshness = check_runtime_dynamic_body_freshness(
      *job.bundle->world, job.bundle->body_freshness, now_ns, config_.dynamic_body_max_age_cap_ns,
      job.bundle->lookup_order_valid);
  if (freshness.status != RuntimeDynamicBodyFreshnessStatus::kFresh) {
    result.reason = runtime_dynamic_body_freshness_reason(freshness.status);
    return finish(std::move(result));
  }

  if (indexed_arm_generation_ != job.arm.arm_generation) {
    std::string index_error;
    if (!build_lookahead_waypoint_time_index(*job.arm.trajectory, waypoint_times_scratch_,
                                             index_error, clock_, absolute_deadline_ns)) {
      result.reason = std::move(index_error);
      return finish(std::move(result));
    }
    indexed_arm_generation_ = job.arm.arm_generation;
  }
  select_runtime_collision_lookahead_samples_from_index(
      *job.arm.trajectory, waypoint_times_scratch_, cursor.trajectory_time_ns,
      config_.horizon_ns, config_.max_samples, selection_scratch_,
      required_times_scratch_, target_times_scratch_, sample_scratch_);
  auto& selection = selection_scratch_;
  result.termination = selection.termination;
  result.covered_until_time_ns = selection.covered_until_time_ns;
  result.checked_samples = selection.trajectory.waypoints.size();
  result.eligible_source_waypoints = selection.eligible_source_waypoints;
  result.sampling_reduced = selection.sampling_reduced;
  result.sample_times_ns = selection.sample_times_ns;
  if (selection.termination == RuntimeCollisionLookaheadTermination::kInvalidInput) {
    result.reason = selection.error_reason;
    return finish(std::move(result));
  }
  if (selection.termination == RuntimeCollisionLookaheadTermination::kNoFutureSamples) {
    result.status = RuntimeCollisionLookaheadResultStatus::kNoFutureSamples;
    result.reason = "no_future_samples";
    return finish(std::move(result));
  }

  // Publish a changed immutable scene revision once before worker fan-out. The
  // ManyForge collision contexts share this prepared-scene cache but retain
  // independent mutable query data, so workers bind the same geometry without
  // racing to rebuild it. Generic engines may leave the hook unsupported and
  // continue through their existing check() path.
  if (engines_.empty() || !engines_.front()) {
    result.reason = "runtime_lookahead_engine_unavailable";
    return finish(std::move(result));
  }
  const auto prepare_status =
      engines_.front()->prepare_runtime_snapshot(*job.bundle->world);
  if (prepare_status != RuntimeCollisionQueryPrepareStatus::kReady &&
      prepare_status != RuntimeCollisionQueryPrepareStatus::kUnsupported) {
    result.reason = "runtime_lookahead_snapshot_prepare_failed";
    result.failure_details["runtime_lookahead_prepare_status"] =
        runtime_snapshot_prepare_status_name(prepare_status);
    return finish(std::move(result));
  }
  if (deadline_exceeded()) {
    result.reason = "runtime_lookahead_deadline_exceeded";
    return finish(std::move(result));
  }

  CollisionValidationPassConfig pass;
  pass.policy_snapshot_ref = job.bundle->policy;
  pass.policy_index = job.bundle->policy_index;
  pass.require_pose_source_borrow = false;
  pass.per_state_timeout_ns = config_.query_budget_ns;
  pass.surface = CollisionSurface::kObservedRuntimeLookahead;
  pass.contact_policy_id = job.arm.contact_policy_id;
  if (!job.arm.contact_policy_id.empty() && !job.bundle->policy->contact_grasps.empty()) {
    pass.original_trajectory_sample_fractions =
        selection.original_trajectory_sample_fractions;
  }
  pass.enforce_contact_release_trajectory_end = selection.reaches_trajectory_end;
  if (!job.bundle->policy->support_contacts.empty()) {
    if (!job.contact_state.valid ||
        job.contact_state.count != job.bundle->policy->support_contacts.size()) {
      result.reason = "runtime_contact_state_unavailable";
      return finish(std::move(result));
    }
    SupportContactEvaluationStateMap support_state;
    ContactReleaseEvaluationStateMap release_state;
    for (std::size_t index = 0; index < job.contact_state.count; ++index) {
      const auto& entry = job.contact_state.entries[index];
      if (entry.pair == nullptr) {
        result.reason = "runtime_contact_state_identity_invalid";
        return finish(std::move(result));
      }
      support_state.emplace(*entry.pair, entry.support_state);
      release_state.emplace(*entry.pair, entry.release_state);
    }
    pass.entering_support_contact_state = std::move(support_state);
    pass.entering_contact_release_state = std::move(release_state);
  }

  if (deadline_exceeded()) {
    result.reason = "runtime_lookahead_deadline_exceeded";
    return finish(std::move(result));
  }

  auto decision = pool_.validate_trajectory(job.bundle->world, engines_, selection.trajectory,
                                            job.cancel, std::move(pass), absolute_deadline_ns);
  const auto telemetry = pool_.last_telemetry();
  result.workers_used = telemetry.workers_actually_used;
  result.first_failing_waypoint = decision.decision.first_failing_waypoint;
  result.first_failing_time_ns = decision.decision.first_failing_time_ns;
  result.first_failing_pair = first_failure_pair(decision);
  if (decision.cancelled || !decision.decision.accepted ||
      decision.decision.action == CollisionDecisionAction::kInconclusive) {
    append_collision_failure_diagnostics(result.failure_details, decision);
  }
  if (deadline_exceeded()) {
    result.reason = "runtime_lookahead_deadline_exceeded";
    return finish(std::move(result));
  }
  if (result.first_failing_waypoint) {
    const auto local_index = *result.first_failing_waypoint;
    if (local_index < 0 ||
        static_cast<std::size_t>(local_index) >=
            selection.source_waypoint_indices.size()) {
      result.first_failing_waypoint.reset();
    } else {
      const auto source_index =
          selection.source_waypoint_indices[static_cast<std::size_t>(local_index)];
      if (source_index < 0) {
        result.first_failing_waypoint.reset();
      } else {
        result.first_failing_waypoint = source_index;
      }
    }
  }
  if (decision.cancelled || decision.decision.action == CollisionDecisionAction::kInconclusive) {
    if (telemetry.fallback_reason == "validation_wall_timeout") {
      result.reason = "runtime_lookahead_deadline_exceeded";
    } else if (decision.cancellation_reason.has_value()) {
      result.reason = *decision.cancellation_reason;
    } else if (const auto status =
                   result.failure_details.find("collision_result_engine_status");
               status != result.failure_details.end() && status->second == "timeout") {
      result.reason = "runtime_lookahead_query_timeout";
    } else if (status != result.failure_details.end() &&
               status->second != "ok") {
      result.reason = "runtime_lookahead_" + status->second;
    } else {
      result.reason = "runtime_lookahead_inconclusive";
    }
    return finish(std::move(result));
  }
  if (!decision.decision.accepted) {
    result.status = RuntimeCollisionLookaheadResultStatus::kCollision;
    result.reason = "runtime_lookahead_collision";
    return finish(std::move(result));
  }

  RuntimeCollisionQueryBundlePtr latest;
  try {
    latest = publisher_->published_runtime_collision_query_bundle();
  } catch (...) {
    result.reason = "runtime_lookahead_revision_probe_failed";
    return finish(std::move(result));
  }
  if (deadline_exceeded()) {
    result.reason = "runtime_lookahead_deadline_exceeded";
    return finish(std::move(result));
  }
  if (!latest || latest->epoch != job.bundle->epoch || !latest->world ||
      !revisions_equal(latest->world->revision, job.bundle->world->revision)) {
    result.status = RuntimeCollisionLookaheadResultStatus::kSupersededRevision;
    result.reason = "runtime_lookahead_revision_superseded";
    return finish(std::move(result));
  }
  const auto completion_freshness = check_runtime_dynamic_body_freshness(
      *job.bundle->world, job.bundle->body_freshness, clock_->now(),
      config_.dynamic_body_max_age_cap_ns, job.bundle->lookup_order_valid);
  if (completion_freshness.status !=
      RuntimeDynamicBodyFreshnessStatus::kFresh) {
    result.reason =
        runtime_dynamic_body_freshness_reason(completion_freshness.status);
    return finish(std::move(result));
  }
  result.status = RuntimeCollisionLookaheadResultStatus::kClear;
  result.reason = "runtime_lookahead_clear";
  return finish(std::move(result));
}

void RuntimeCollisionLookaheadSession::record_result(
    const RuntimeCollisionLookaheadResult& result) {
  std::lock_guard<std::mutex> lock(status_mutex_);
  status_.last_cycle_duration_ns = result.cycle_duration_ns;
  status_.max_cycle_duration_ns =
      std::max(status_.max_cycle_duration_ns, result.cycle_duration_ns);
  status_.last_worker_active_duration_ns = result.worker_active_duration_ns;
  status_.max_worker_active_duration_ns =
      std::max(status_.max_worker_active_duration_ns,
               result.worker_active_duration_ns);
  status_.max_start_jitter_ns = std::max(status_.max_start_jitter_ns, result.start_jitter_ns);
  status_.last_cursor_time_ns = result.cursor_time_ns;
  status_.last_covered_until_time_ns = result.covered_until_time_ns;
  status_.last_checked_samples = result.checked_samples;
  status_.last_eligible_source_waypoints = result.eligible_source_waypoints;
  status_.last_sampling_reduced = result.sampling_reduced;
  status_.last_sample_times_ns = result.sample_times_ns;
  status_.last_workers_used = result.workers_used;
  status_.last_termination = result.termination;
  if (result.cycle_deadline_exceeded) {
    ++status_.jobs_cycle_timeout_total;
  }
  switch (result.status) {
    case RuntimeCollisionLookaheadResultStatus::kClear:
    case RuntimeCollisionLookaheadResultStatus::kNoFutureSamples:
      ++status_.jobs_clear_total;
      break;
    case RuntimeCollisionLookaheadResultStatus::kCollision:
      ++status_.jobs_collision_total;
      break;
    case RuntimeCollisionLookaheadResultStatus::kInconclusive:
      ++status_.jobs_inconclusive_total;
      if (result.reason == "runtime_lookahead_query_timeout") {
        ++status_.jobs_query_timeout_total;
      }
      break;
    case RuntimeCollisionLookaheadResultStatus::kSupersededRevision:
      ++status_.jobs_superseded_total;
      break;
  }
}

bool RuntimeCollisionLookaheadSession::accept_result_for_current_arm(
    std::uint64_t execution_id, std::uint64_t execution_generation,
    std::uint64_t arm_generation, std::uint64_t permit_generation) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool current = arm_.trajectory != nullptr && arm_.execution_id == execution_id &&
                       arm_.execution_generation == execution_generation &&
                       arm_.arm_generation == arm_generation &&
                       permit_generation_.load(std::memory_order_acquire) ==
                           permit_generation &&
                       !progress_generation_is_terminal(execution_generation);
  if (!current) {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    ++status_.stale_generation_results_total;
  }
  return current;
}

bool RuntimeCollisionLookaheadSession::progress_generation_is_terminal(
    std::uint64_t execution_generation) const noexcept {
  const auto cursor = progress_publication_->acquire();
  // Only a coherent, explicit terminal marker suppresses authority. A
  // transiently unreadable or invalid cursor remains fail-closed and is
  // handled by the normal inconclusive path.
  return cursor.valid && cursor.terminal &&
         cursor.execution_generation == execution_generation;
}

RuntimeCollisionLookaheadStatus RuntimeCollisionLookaheadSession::status() const {
  RuntimeCollisionLookaheadStatus snapshot;
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    snapshot = status_;
  }
  snapshot.reruns_coalesced_total = reruns_coalesced_total_.load(std::memory_order_relaxed);
  snapshot.permit_generation = permit_generation_.load(std::memory_order_acquire);
  snapshot.clear_permits_invalidated_total =
      clear_permits_invalidated_total_.load(std::memory_order_relaxed);
  snapshot.clear_permit_try_lock_misses_total =
      clear_permit_try_lock_misses_total_.load(std::memory_order_relaxed);
  snapshot.accepting_clear_permits =
      accepting_clear_permits_.load(std::memory_order_acquire);
  const auto cursor = progress_publication_->acquire();
  snapshot.cursor_source_kind = execution_progress_source_kind_name(cursor.source_kind);
  snapshot.cursor_authority_profile = std::string(cursor.authority_profile);
  const auto now_ns = clock_->now();
  const bool timestamp_valid =
      cursor.received_timestamp_ns >= 0 && cursor.received_timestamp_ns <= now_ns;
  const bool fresh =
      timestamp_valid && (cursor.freshness_budget_ns == 0 ||
                          now_ns - cursor.received_timestamp_ns <= cursor.freshness_budget_ns);
  snapshot.cursor_valid = snapshot.armed && cursor.valid && !cursor.terminal && fresh &&
                          cursor.execution_generation == snapshot.execution_generation;
  snapshot.cursor_age_ns = timestamp_valid ? now_ns - cursor.received_timestamp_ns : 0;
  return snapshot;
}

}  // namespace manyforge::core
