#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

#include <manyforge_core/types/plan_request.hpp>

namespace manyforge::roboplan_backend {

// Owner-neutral qualification contract extracted from
// manyforge_roboplan/include/manyforge_roboplan/roboplan_planner_backend.hpp at
// parent c4b075fa6dfb69e463c61f3efa8cfce8de681e8a (deleted by 0822218). A
// planning adapter may consume this contract, but no planner owner is
// restored here.
struct RoboPlanCollisionProbeSnapshot {
  bool available = false;
  std::uint64_t query_count = 0;
  std::uint64_t query_duration_ns = 0;
};

struct RoboPlanAllocationProbeSnapshot {
  bool available = false;
  std::uint64_t allocation_count = 0;
  std::uint64_t allocation_bytes = 0;
};

struct RoboPlanPlanningStageMetrics {
  std::uint64_t duration_ns = 0;
  std::uint64_t collision_query_count = 0;
  std::uint64_t collision_query_duration_ns = 0;
  std::uint64_t allocation_count = 0;
  std::uint64_t allocation_bytes = 0;
};

/// One immutable planning invocation measurement. It intentionally contains
/// backend-stage facts only; dispatch, authoritative validation, and runtime
/// collision evidence remain owned by their respective ManyForge surfaces.
struct RoboPlanPlanningPipelineMetrics {
  std::map<std::string, RoboPlanPlanningStageMetrics> stages;
  std::uint64_t total_duration_ns = 0;
  std::uint64_t total_collision_query_count = 0;
  std::uint64_t total_collision_query_duration_ns = 0;
  bool collision_probe_available = false;
  bool measurement_complete = true;
  bool succeeded = false;
  std::string error_code;
  std::size_t waypoint_count = 0;
  double trajectory_duration_s = 0.0;
  std::optional<std::uint32_t> rng_base_seed;
  std::optional<std::uint32_t> rng_seed;
  std::optional<int> shortcut_rng_seed_argument;
  std::uint32_t planning_attempt_index = 0;
};

/// NRT qualification observer. Implementations must be thread-safe and must
/// not throw. Future planning adapters call this surface only when explicitly
/// enabled; production composition does not install an observer.
class IRoboPlanPlanningPerformanceObserver {
 public:
  virtual ~IRoboPlanPlanningPerformanceObserver() = default;
  virtual RoboPlanCollisionProbeSnapshot collision_probe_snapshot()
      const noexcept = 0;
  virtual RoboPlanAllocationProbeSnapshot allocation_probe_snapshot()
      const noexcept {
    return {};
  }
  virtual bool try_begin_invocation(const core::PlanRequestDTO&) noexcept {
    return true;
  }
  virtual void record(
      const core::PlanRequestDTO& request,
      const core::PlanResultDTO& result,
      const RoboPlanPlanningPipelineMetrics& metrics) noexcept = 0;
};

}  // namespace manyforge::roboplan_backend
