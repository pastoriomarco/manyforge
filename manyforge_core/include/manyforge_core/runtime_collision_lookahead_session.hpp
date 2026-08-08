#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_core/execution_progress_publication.hpp>
#include <manyforge_core/runtime_collision_query_session.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace manyforge::core {

enum class RuntimeCollisionLookaheadTermination {
  kTrajectoryEnd,
  kTimeHorizon,
  kNoFutureSamples,
  kInvalidInput,
};

const char* runtime_collision_lookahead_termination_name(
    RuntimeCollisionLookaheadTermination termination) noexcept;

const char* runtime_collision_lookahead_error_message(std::string_view reason) noexcept;

struct RuntimeCollisionLookaheadSelection {
  TrajectoryDTO trajectory;
  /// Original waypoint index, or -1 for an interpolated time boundary.
  std::vector<std::int64_t> source_waypoint_indices;
  /// Fractional index in the complete accepted trajectory, normalized by
  /// max(original_waypoint_count - 1, 1). Interpolated horizon samples retain
  /// their fractional original index instead of becoming a local endpoint.
  std::vector<double> original_trajectory_sample_fractions;
  /// Exact accepted-trajectory times selected for collision queries.
  std::vector<std::int64_t> sample_times_ns;
  /// Number of source knots in (cursor, window_end], including a source knot
  /// exactly at window_end. This excludes an interpolated endpoint.
  std::size_t eligible_source_waypoints = 0;
  /// True when eligible source knots plus a required interpolated endpoint did
  /// not fit in the sample cap and deterministic time sampling was required.
  bool sampling_reduced = false;
  RuntimeCollisionLookaheadTermination termination =
      RuntimeCollisionLookaheadTermination::kInvalidInput;
  bool reaches_trajectory_end = false;
  std::int64_t cursor_time_ns = 0;
  std::int64_t covered_until_time_ns = 0;
  std::string error_reason;
};

RuntimeCollisionLookaheadSelection select_runtime_collision_lookahead_samples(
    const TrajectoryDTO& trajectory, std::int64_t cursor_time_ns, std::int64_t horizon_ns,
    std::size_t max_samples);
void select_runtime_collision_lookahead_samples(const TrajectoryDTO& trajectory,
                                                std::int64_t cursor_time_ns,
                                                std::int64_t horizon_ns, std::size_t max_samples,
                                                RuntimeCollisionLookaheadSelection& output);

enum class RuntimeCollisionLookaheadResultStatus {
  kClear,
  kCollision,
  kInconclusive,
  kSupersededRevision,
  kNoFutureSamples,
};

enum class RuntimeCollisionLookaheadAuthorityDisposition {
  kAccepted,
  kStale,
};

struct RuntimeCollisionLookaheadResult {
  RuntimeCollisionLookaheadResultStatus status =
      RuntimeCollisionLookaheadResultStatus::kInconclusive;
  std::string reason;
  std::uint64_t execution_id = 0;
  std::uint64_t execution_generation = 0;
  std::uint64_t arm_generation = 0;
  std::uint64_t permit_generation = 0;
  std::uint64_t runtime_bundle_epoch = 0;
  std::optional<ProjectionRevisionDTO> projection_revision;
  CollisionWorldRevision collision_world_revision;
  std::int64_t requested_at_ns = 0;
  std::int64_t cycle_deadline_ns = 0;
  std::int64_t cursor_time_ns = 0;
  std::int64_t covered_until_time_ns = 0;
  std::size_t checked_samples = 0;
  std::size_t eligible_source_waypoints = 0;
  bool sampling_reduced = false;
  std::vector<std::int64_t> sample_times_ns;
  RuntimeCollisionLookaheadTermination termination =
      RuntimeCollisionLookaheadTermination::kInvalidInput;
  int workers_used = 0;
  std::int64_t cycle_duration_ns = 0;
  std::int64_t worker_active_duration_ns = 0;
  std::int64_t start_jitter_ns = 0;
  bool cycle_deadline_exceeded = false;
  bool stop_authority_signalled = false;
  bool authority_rejected = false;
  std::string authority_rejection_reason;
  std::optional<std::int64_t> first_failing_waypoint;
  std::optional<std::int64_t> first_failing_time_ns;
  std::optional<CollisionPairId> first_failing_pair;
  /// Bounded diagnostics from the first failing collision-engine record.
  /// Populated only for a collision or inconclusive result, off the immediate
  /// runtime-monitor path.
  std::map<std::string, std::string> failure_details;
};

struct RuntimeCollisionLookaheadStatus {
  std::uint64_t execution_id = 0;
  std::uint64_t execution_generation = 0;
  std::uint64_t arm_generation = 0;
  std::uint64_t permit_generation = 0;
  std::int64_t requested_cycle_budget_ns = 0;
  std::int64_t requested_query_budget_ns = 0;
  std::int64_t requested_horizon_ns = 0;
  std::size_t requested_max_samples = 0;
  int requested_workers = 0;
  int effective_workers = 0;
  bool armed = false;
  bool job_active = false;
  bool permit_pending = false;
  bool accepting_clear_permits = false;
  bool cursor_valid = false;
  std::string cursor_source_kind;
  std::string cursor_authority_profile;
  std::int64_t cursor_age_ns = 0;
  std::uint64_t jobs_started_total = 0;
  std::uint64_t jobs_clear_total = 0;
  std::uint64_t jobs_collision_total = 0;
  std::uint64_t jobs_inconclusive_total = 0;
  std::uint64_t jobs_query_timeout_total = 0;
  std::uint64_t jobs_cycle_timeout_total = 0;
  std::uint64_t jobs_superseded_total = 0;
  std::uint64_t reruns_coalesced_total = 0;
  std::uint64_t clear_permits_invalidated_total = 0;
  std::uint64_t clear_permit_try_lock_misses_total = 0;
  std::uint64_t stale_generation_results_total = 0;
  std::int64_t last_cycle_duration_ns = 0;
  std::int64_t max_cycle_duration_ns = 0;
  std::int64_t last_worker_active_duration_ns = 0;
  std::int64_t max_worker_active_duration_ns = 0;
  std::int64_t max_start_jitter_ns = 0;
  std::int64_t last_inter_job_gap_ns = 0;
  std::int64_t active_time_total_ns = 0;
  std::int64_t duty_window_ns = 0;
  std::uint64_t duty_cycle_ppm = 0;
  std::int64_t last_cursor_time_ns = 0;
  std::int64_t last_covered_until_time_ns = 0;
  std::size_t last_checked_samples = 0;
  std::size_t last_eligible_source_waypoints = 0;
  bool last_sampling_reduced = false;
  std::vector<std::int64_t> last_sample_times_ns;
  int last_workers_used = 0;
  RuntimeCollisionLookaheadTermination last_termination =
      RuntimeCollisionLookaheadTermination::kInvalidInput;
};

class RuntimeCollisionLookaheadSession final {
 public:
  struct Config {
    std::int64_t cycle_budget_ns = 200'000'000;
    std::int64_t horizon_ns = 2'000'000'000;
    std::size_t max_samples = 32;
    int workers = 2;
    int min_waypoints_for_parallel = 8;
    std::int64_t query_budget_ns = 75'000'000;
    std::int64_t dynamic_body_max_age_cap_ns = 500'000'000;
  };

  /// Observer callback invoked only after any required authority callback and
  /// requested-to-authority timing/status publication are complete.
  using ResultCallback =
      std::function<void(const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult)>;
  /// Kernel authority boundary for collision/inconclusive results. It must
  /// apply stop authority before returning and report a generation-stale
  /// result without side effects. Optional only for focused session tests.
  using AuthorityCallback = std::function<RuntimeCollisionLookaheadAuthorityDisposition(
      const RuntimeCollisionLookaheadSession*, RuntimeCollisionLookaheadResult&)>;

  RuntimeCollisionLookaheadSession(Config config, IClock& clock, IScheduler& scheduler,
                                   ICollisionWorldSnapshotPublisher& publisher,
                                   const ExecutionProgressPublication& progress_publication,
                                   std::vector<std::shared_ptr<ICollisionQueryEngine>> engines,
                                   ResultCallback callback, AuthorityCallback authority_callback);
  ~RuntimeCollisionLookaheadSession();

  RuntimeCollisionLookaheadSession(const RuntimeCollisionLookaheadSession&) = delete;
  RuntimeCollisionLookaheadSession& operator=(const RuntimeCollisionLookaheadSession&) = delete;

  void arm(std::uint64_t execution_id, std::uint64_t execution_generation,
           std::shared_ptr<const TrajectoryDTO> trajectory, std::string contact_policy_id,
           std::optional<ProjectionRevisionDTO> admission_projection_revision = std::nullopt);
  void disarm() noexcept;

  /// Called only after a clear immediate current-state tick. It never waits:
  /// a busy session coalesces one rerun and returns.
  void request(std::int64_t requested_at_ns, RuntimeCollisionQueryBundlePtr bundle,
               RuntimeContactReleaseStateSnapshot contact_state) noexcept;

  /// Withdraw all clear permits without waiting for the lookahead worker. The
  /// permit generation makes queued and in-flight work stale even if the leaf
  /// mutex is momentarily held by the worker.
  void revoke_clear_permits() noexcept;

  /// Revalidates a completed result at the kernel authority boundary. The
  /// caller must serialize this check with arm()/disarm() using its owning
  /// protection mutex so a same-execution resume cannot admit an old result.
  [[nodiscard]] bool accept_result_for_current_arm(std::uint64_t execution_id,
                                                   std::uint64_t execution_generation,
                                                   std::uint64_t arm_generation,
                                                   std::uint64_t permit_generation) noexcept;

  [[nodiscard]] RuntimeCollisionLookaheadStatus status() const;

 private:
  struct ArmState {
    std::uint64_t execution_id = 0;
    std::uint64_t execution_generation = 0;
    std::uint64_t arm_generation = 0;
    std::shared_ptr<const TrajectoryDTO> trajectory;
    std::string contact_policy_id;
    std::optional<ProjectionRevisionDTO> admission_projection_revision;
    std::optional<ProjectionRevisionDTO> latest_projection_revision;
  };

  struct Job {
    ArmState arm;
    std::int64_t requested_at_ns = 0;
    RuntimeCollisionQueryBundlePtr bundle;
    RuntimeContactReleaseStateSnapshot contact_state;
    ExecutionProgressCursorSnapshotDTO cursor;
    std::shared_ptr<CollisionCancellationToken> cancel;
    std::uint64_t permit_generation = 0;
  };

  void worker_loop(std::stop_token stop_token);
  RuntimeCollisionLookaheadResult run_job(const Job& job);
  void record_result(const RuntimeCollisionLookaheadResult& result);
  [[nodiscard]] bool progress_generation_is_terminal(
      std::uint64_t execution_generation) const noexcept;

  Config config_;
  IClock* clock_ = nullptr;
  ICollisionWorldSnapshotPublisher* publisher_ = nullptr;
  const ExecutionProgressPublication* progress_publication_ = nullptr;
  std::vector<std::shared_ptr<ICollisionQueryEngine>> engines_;
  ResultCallback callback_;
  AuthorityCallback authority_callback_;
  ParallelCollisionValidationWorkerPool pool_;
  // Single-worker scratch retained across jobs. Capacity is bounded by the
  // deployment sample cap; no immediate-monitor thread accesses it.
  RuntimeCollisionLookaheadSelection selection_scratch_;
  std::vector<std::int64_t> required_times_scratch_;
  std::vector<std::int64_t> target_times_scratch_;
  TrajectorySampleResult sample_scratch_;
  // Built once for each immutable armed trajectory, then reused for O(log N)
  // cursor location. This prevents a huge trajectory from turning every
  // bounded lookahead tick into an O(N) pre-scan.
  std::vector<std::int64_t> waypoint_times_scratch_;
  std::uint64_t indexed_arm_generation_ = 0;

  mutable std::mutex mutex_;
  std::condition_variable_any cv_;
  ArmState arm_;
  std::optional<Job> pending_job_;
  std::atomic<std::shared_ptr<CollisionCancellationToken>> active_cancel_{nullptr};
  bool job_active_ = false;
  std::atomic<bool> accepting_clear_permits_{false};
  std::atomic<std::uint64_t> permit_generation_{1};
  std::atomic<std::uint64_t> reruns_coalesced_total_{0};
  std::atomic<std::uint64_t> clear_permits_invalidated_total_{0};
  std::atomic<std::uint64_t> clear_permit_try_lock_misses_total_{0};
  std::uint64_t next_arm_generation_ = 1;
  std::int64_t duty_window_started_at_ns_ = -1;
  std::int64_t active_job_started_at_ns_ = 0;
  std::int64_t last_job_completed_at_ns_ = -1;
  std::jthread worker_;

  mutable std::mutex status_mutex_;
  RuntimeCollisionLookaheadStatus status_;
};

}  // namespace manyforge::core
