#include <manyforge_roboplan/roboplan_planning_qualification.hpp>

#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

#if defined(__GNUC__)
extern "C" bool manyforge_roboplan_collision_probe_snapshot(
    std::uint64_t* query_count,
    std::uint64_t* query_duration_ns) noexcept __attribute__((weak));
#endif

}  // namespace

RoboPlanPlanningQualificationRecorder::RoboPlanPlanningQualificationRecorder(
    std::size_t capacity)
    : capacity_(capacity) {
  if (capacity_ == 0) {
    throw std::invalid_argument(
        "planning qualification recorder capacity must be positive");
  }
}

RoboPlanCollisionProbeSnapshot
RoboPlanPlanningQualificationRecorder::collision_probe_snapshot()
    const noexcept {
  RoboPlanCollisionProbeSnapshot snapshot;
#if defined(__GNUC__)
  if (manyforge_roboplan_collision_probe_snapshot != nullptr) {
    snapshot.available = manyforge_roboplan_collision_probe_snapshot(
        &snapshot.query_count, &snapshot.query_duration_ns);
  }
#endif
  return snapshot;
}

bool RoboPlanPlanningQualificationRecorder::try_begin_invocation(
    const core::PlanRequestDTO&) noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) {
      return false;
    }
    ++in_flight_;
  }
  active_cv_.notify_all();
  return true;
}

void RoboPlanPlanningQualificationRecorder::record(
  const core::PlanRequestDTO& request, const core::PlanResultDTO& result,
  const RoboPlanPlanningPipelineMetrics& metrics) noexcept {
  try {
    RoboPlanPlanningQualificationRecord record{0U, request, result, metrics};

    std::lock_guard<std::mutex> lock(mutex_);
    record.sequence = next_sequence_++;
    if (records_.size() == capacity_) {
      records_.pop_front();
      ++dropped_total_;
    }
    records_.push_back(std::move(record));
    if (in_flight_ > 0) {
      --in_flight_;
    } else {
      ++dropped_total_;
    }
    if (in_flight_ == 0) {
      idle_cv_.notify_all();
    }
  } catch (...) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_total_;
    if (in_flight_ > 0) {
      --in_flight_;
    }
    if (in_flight_ == 0) {
      idle_cv_.notify_all();
    }
  }
}

std::vector<RoboPlanPlanningQualificationRecord>
RoboPlanPlanningQualificationRecorder::drain() {
  return drain_with_status().records;
}

void RoboPlanPlanningQualificationRecorder::close_admission() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_ = false;
  }
  active_cv_.notify_all();
}

RoboPlanPlanningQualificationBatch
RoboPlanPlanningQualificationRecorder::drain_with_status() {
  std::lock_guard<std::mutex> lock(mutex_);
  RoboPlanPlanningQualificationBatch batch;
  batch.records.reserve(records_.size());
  while (!records_.empty()) {
    batch.records.push_back(std::move(records_.front()));
    records_.pop_front();
  }
  batch.dropped_total = dropped_total_;
  batch.in_flight = in_flight_;
  return batch;
}

bool RoboPlanPlanningQualificationRecorder::wait_until_idle(
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return idle_cv_.wait_for(lock, timeout, [&] { return in_flight_ == 0; });
}

bool RoboPlanPlanningQualificationRecorder::wait_until_active(
    std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  active_cv_.wait_for(lock, timeout,
                      [&] { return in_flight_ > 0 || !accepting_; });
  return in_flight_ > 0;
}

std::uint64_t RoboPlanPlanningQualificationRecorder::dropped_total()
    const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return dropped_total_;
}

std::size_t RoboPlanPlanningQualificationRecorder::in_flight() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return in_flight_;
}

}  // namespace manyforge::roboplan_backend
