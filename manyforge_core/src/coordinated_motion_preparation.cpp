#include <algorithm>
#include <cmath>
#include <limits>
#include <manyforge_core/detail/coordinated_execution_clock.hpp>
#include <manyforge_core/detail/coordinated_motion_preparation.hpp>
#include <stdexcept>

namespace manyforge::core::detail {

CoordinatedMotionPreparation::CoordinatedMotionPreparation(
    std::vector<OperationParticipantId> participants)
    : common_{1.0, {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()}},
      remaining_(participants.size()) {
  std::sort(participants.begin(), participants.end());
  if (participants.size() < 2U ||
      std::adjacent_find(participants.begin(), participants.end()) != participants.end())
    throw std::invalid_argument("Common preparation requires distinct motion participants");
  slots_.reserve(participants.size());
  for (auto& participant : participants) slots_.push_back({std::move(participant), false});
}

std::optional<RuntimeScalePreparation> CoordinatedMotionPreparation::arrive_and_wait(
    const OperationParticipantId& participant, const TrajectoryRuntimeScaleBoundsDTO& bounds,
    std::stop_token stop) {
  std::unique_lock lock(mutex_);
  auto slot =
      std::lower_bound(slots_.begin(), slots_.end(), participant,
                       [](const auto& row, const auto& id) { return row.participant < id; });
  const auto positive = [](double value) { return std::isfinite(value) && value > 0.0; };
  if (cancelled_ || stop.stop_requested() || slot == slots_.end() ||
      slot->participant != participant || slot->arrived || !bounds.nominal_time_multiplier ||
      !positive(*bounds.nominal_time_multiplier) || *bounds.nominal_time_multiplier < 1.0 ||
      !positive(bounds.max_rate_per_s) || !positive(bounds.max_rate_change_per_s2)) {
    cancelled_ = true;
    changed_.notify_all();
    return std::nullopt;
  }
  slot->arrived = true;
  common_.trajectory_time_multiplier =
      std::max(common_.trajectory_time_multiplier, *bounds.nominal_time_multiplier);
  common_.scale_bounds.max_rate_per_s =
      std::min(common_.scale_bounds.max_rate_per_s, bounds.max_rate_per_s);
  common_.scale_bounds.max_rate_change_per_s2 =
      std::min(common_.scale_bounds.max_rate_change_per_s2, bounds.max_rate_change_per_s2);
  --remaining_;
  if (remaining_ == 0U) {
    try {
      std::vector<OperationParticipantId> participants;
      participants.reserve(slots_.size());
      for (const auto& row : slots_) participants.push_back(row.participant);
      execution_clock_ = std::make_shared<CoordinatedExecutionClock>(std::move(participants),
                                                                     common_.scale_bounds);
    } catch (...) {
      cancelled_ = true;
      changed_.notify_all();
      throw;
    }
  }
  changed_.notify_all();
  if (!changed_.wait(lock, stop, [&] { return cancelled_ || remaining_ == 0U; })) {
    cancelled_ = true;
    changed_.notify_all();
  }
  return cancelled_ ? std::nullopt : std::optional(common_);
}

void CoordinatedMotionPreparation::cancel() noexcept {
  std::shared_ptr<CoordinatedExecutionClock> execution_clock;
  {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    execution_clock = execution_clock_;
    changed_.notify_all();
  }
  if (execution_clock) execution_clock->cancel_pending_start();
}

std::shared_ptr<CoordinatedExecutionClock> CoordinatedMotionPreparation::execution_clock() {
  std::lock_guard lock(mutex_);
  return cancelled_ ? nullptr : execution_clock_;
}

}  // namespace manyforge::core::detail
