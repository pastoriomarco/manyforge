#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace manyforge::core {

struct MetricsSnapshot {
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t cache_revalidation_candidates = 0;
  uint64_t cache_revalidations_succeeded = 0;
  uint64_t cache_revalidations_failed = 0;
  uint64_t cache_invalidations = 0;
  uint64_t plans_succeeded = 0;
  uint64_t plans_failed = 0;
  uint64_t validations_passed = 0;
  uint64_t validations_failed = 0;
  uint64_t validation_collision_workers_requested = 0;
  uint64_t validation_collision_workers_effective = 0;
  uint64_t validation_collision_jobs_queued = 0;
  uint64_t validation_collision_workers_active = 0;
  uint64_t validation_collision_jobs_completed = 0;
  uint64_t validation_collision_jobs_cancelled = 0;
  uint64_t validation_collision_jobs_dropped = 0;
  uint64_t validation_collision_jobs_abandoned = 0;
  uint64_t executions_completed = 0;
  uint64_t executions_failed = 0;
  uint64_t stops_issued = 0;
  uint64_t collision_detections = 0;
  uint64_t runtime_collision_ticks_started = 0;
  uint64_t runtime_collision_ticks_completed = 0;
  uint64_t runtime_collision_ticks_faulted = 0;
  uint64_t runtime_collision_tick_max_ns = 0;
  uint64_t runtime_collision_tick_start_jitter_max_ns = 0;
  uint64_t runtime_collision_tick_late_periods = 0;
  uint64_t runtime_collision_tick_missed_periods = 0;
  uint64_t runtime_collision_tick_max_consecutive_late_periods = 0;
  uint64_t runtime_collision_stop_windows_started = 0;
  uint64_t runtime_collision_stop_windows_completed = 0;
  uint64_t runtime_collision_stop_ticks_attempted = 0;
  uint64_t runtime_collision_stop_queries_executed = 0;
  uint64_t runtime_collision_stop_unavailable_ticks = 0;
  uint64_t runtime_collision_stop_longest_unavailable_ns = 0;
  uint64_t runtime_collision_stop_zero_check_windows = 0;
  uint64_t runtime_collision_last_stop_ticks_attempted = 0;
  uint64_t runtime_collision_last_stop_queries_executed = 0;
  uint64_t runtime_collision_last_stop_unavailable_ticks = 0;
  uint64_t runtime_collision_last_stop_longest_unavailable_ns = 0;
  uint64_t runtime_collision_freshest_observed_source_age_ns = 0;
  std::string runtime_collision_freshest_observed_source_id;
  uint64_t runtime_collision_unavailable_events_suppressed = 0;
  uint64_t observed_state_reader_retry_exhausted_total = 0;
  uint64_t validation_deferred_runtime_monitor_active = 0;
  uint64_t runtime_collision_snapshot_refresh_failures = 0;
  uint64_t runtime_collision_monitor_period_ns = 0;
  uint64_t runtime_collision_immediate_query_budget_ns = 0;
  uint64_t runtime_collision_lookahead_cycle_budget_ns = 0;
  uint64_t runtime_collision_lookahead_query_budget_ns = 0;
  uint64_t runtime_collision_lookahead_horizon_ns = 0;
  uint64_t runtime_collision_lookahead_max_samples = 0;
  uint64_t runtime_collision_lookahead_workers_requested = 0;
  uint64_t runtime_collision_lookahead_workers_effective = 0;
  bool runtime_collision_lookahead_armed = false;
  bool runtime_collision_lookahead_job_active = false;
  bool runtime_collision_lookahead_permit_pending = false;
  uint64_t runtime_collision_lookahead_execution_id = 0;
  uint64_t runtime_collision_lookahead_execution_generation = 0;
  bool runtime_collision_lookahead_cursor_valid = false;
  std::string runtime_collision_lookahead_cursor_source_kind;
  std::string runtime_collision_lookahead_cursor_authority_profile;
  uint64_t runtime_collision_lookahead_cursor_age_ns = 0;
  uint64_t runtime_collision_lookahead_jobs_started = 0;
  uint64_t runtime_collision_lookahead_jobs_clear = 0;
  uint64_t runtime_collision_lookahead_jobs_collision = 0;
  uint64_t runtime_collision_lookahead_jobs_inconclusive = 0;
  uint64_t runtime_collision_lookahead_jobs_query_timeout = 0;
  uint64_t runtime_collision_lookahead_jobs_cycle_timeout = 0;
  uint64_t runtime_collision_lookahead_jobs_superseded = 0;
  uint64_t runtime_collision_lookahead_reruns_coalesced = 0;
  uint64_t runtime_collision_lookahead_clear_permits_invalidated = 0;
  uint64_t runtime_collision_lookahead_clear_permit_try_lock_misses = 0;
  uint64_t runtime_collision_lookahead_stale_results = 0;
  uint64_t runtime_collision_lookahead_last_cycle_duration_ns = 0;
  uint64_t runtime_collision_lookahead_max_cycle_duration_ns = 0;
  uint64_t runtime_collision_lookahead_last_worker_active_duration_ns = 0;
  uint64_t runtime_collision_lookahead_max_worker_active_duration_ns = 0;
  uint64_t runtime_collision_lookahead_max_start_jitter_ns = 0;
  uint64_t runtime_collision_lookahead_last_inter_job_gap_ns = 0;
  uint64_t runtime_collision_lookahead_active_time_total_ns = 0;
  uint64_t runtime_collision_lookahead_duty_window_ns = 0;
  uint64_t runtime_collision_lookahead_duty_cycle_ppm = 0;
  uint64_t runtime_collision_lookahead_last_cursor_time_ns = 0;
  uint64_t runtime_collision_lookahead_last_covered_until_time_ns = 0;
  uint64_t runtime_collision_lookahead_last_checked_samples = 0;
  uint64_t runtime_collision_lookahead_last_eligible_source_waypoints = 0;
  bool runtime_collision_lookahead_last_sampling_reduced = false;
  std::vector<std::int64_t> runtime_collision_lookahead_last_sample_times_ns;
  uint64_t runtime_collision_lookahead_last_workers_used = 0;
  std::string runtime_collision_lookahead_last_termination;
  uint64_t snapshots_published = 0;
  uint64_t event_subscriber_exceptions = 0;
  uint64_t observer_lag_max_ns = 0;
  // Total events dispatched (== the dispatch-order sequence high-water mark)
  // and total records absorbed by the flight ring. Populated at
  // get_metrics() time from their owning counters — no additional hot-path
  // work (observability plan OW6).
  uint64_t events_emitted = 0;
  uint64_t flight_recorder_records = 0;
};

}  // namespace manyforge::core
