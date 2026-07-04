#pragma once

#include <cstdint>

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
  uint64_t snapshots_published = 0;
  uint64_t event_subscriber_exceptions = 0;
  uint64_t observer_lag_max_ns = 0;
};

}  // namespace manyforge::core
