#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <manyforge_roboplan/roboplan_planning_performance_contract.hpp>

namespace manyforge::roboplan_backend {

struct RoboPlanPlanningQualificationRecord {
  std::uint64_t sequence = 0;
  core::PlanRequestDTO request;
  core::PlanResultDTO result;
  RoboPlanPlanningPipelineMetrics metrics;
};

struct RoboPlanPlanningQualificationBatch {
  std::vector<RoboPlanPlanningQualificationRecord> records;
  std::uint64_t dropped_total = 0;
  std::size_t in_flight = 0;
};

/// Bounded, opt-in recorder for version-transition qualification.
///
/// Production composition does not enable it. A future planning adapter can
/// preserve the exact resolved request and retained scene binding it consumed;
/// capture tools must not reconstruct those facts from UI state or authored
/// YAML.
class RoboPlanPlanningQualificationRecorder final
    : public IRoboPlanPlanningPerformanceObserver {
 public:
  explicit RoboPlanPlanningQualificationRecorder(std::size_t capacity);

  RoboPlanCollisionProbeSnapshot collision_probe_snapshot()
      const noexcept override;
  bool try_begin_invocation(
      const core::PlanRequestDTO& request) noexcept override;
  void record(const core::PlanRequestDTO& request,
              const core::PlanResultDTO& result,
              const RoboPlanPlanningPipelineMetrics& metrics) noexcept override;

  void close_admission() noexcept;
  std::vector<RoboPlanPlanningQualificationRecord> drain();
  RoboPlanPlanningQualificationBatch drain_with_status();
  bool wait_until_active(std::chrono::milliseconds timeout);
  bool wait_until_idle(std::chrono::milliseconds timeout);
  std::uint64_t dropped_total() const noexcept;
  std::size_t in_flight() const noexcept;

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable active_cv_;
  std::condition_variable idle_cv_;
  std::deque<RoboPlanPlanningQualificationRecord> records_;
  std::uint64_t next_sequence_ = 1;
  std::uint64_t dropped_total_ = 0;
  std::size_t in_flight_ = 0;
  bool accepting_ = true;
};

}  // namespace manyforge::roboplan_backend
