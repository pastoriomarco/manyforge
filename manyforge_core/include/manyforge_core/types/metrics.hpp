#pragma once

#include <cstdint>
#include <string>

namespace manyforge::core {

struct MetricsSnapshot {
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  uint64_t cache_invalidations = 0;
  uint64_t plans_succeeded = 0;
  uint64_t plans_failed = 0;
  uint64_t validations_passed = 0;
  uint64_t validations_failed = 0;
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
