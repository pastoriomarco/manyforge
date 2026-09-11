#include <algorithm>
#include <cmath>
#include <manyforge_core/detail/coordinated_execution_clock.hpp>
#include <stdexcept>

namespace manyforge::core::detail {

CoordinatedExecutionClock::CoordinatedExecutionClock(
    std::vector<OperationParticipantId> participants, RuntimeScaleBounds bounds)
    : bounds_(bounds), remaining_(participants.size()) {
  (void)RuntimeScaleGovernor(1.0, bounds);
  std::sort(participants.begin(), participants.end());
  if (participants.size() < 2U || participants.front().empty() ||
      std::adjacent_find(participants.begin(), participants.end()) != participants.end())
    throw std::invalid_argument("Common execution requires distinct motion participants");
  participants_.reserve(participants.size());
  for (auto& participant : participants)
    participants_.push_back({std::move(participant), false, {}});
}

bool CoordinatedExecutionClock::accepts(const OperationParticipantId& participant,
                                        RuntimeScaleBounds bounds) const {
  std::lock_guard lock(mutex_);
  return !cancelled_ && !anchor_ && bounds.max_rate_per_s == bounds_.max_rate_per_s &&
         bounds.max_rate_change_per_s2 == bounds_.max_rate_change_per_s2 &&
         std::any_of(participants_.begin(), participants_.end(),
                     [&](const auto& row) { return row.id == participant && !row.armed; });
}

std::optional<RuntimeScaleTimeAnchor> CoordinatedExecutionClock::start_and_wait(
    const OperationParticipantId& participant, IClock& clock, double initial_scale,
    std::stop_token stop) {
  std::unique_lock lock(mutex_);
  const auto found = std::lower_bound(participants_.begin(), participants_.end(), participant,
                                      [](const auto& row, const auto& id) { return row.id < id; });
  if (anchor_) return std::nullopt;
  if (cancelled_ || stop.stop_requested() || found == participants_.end() ||
      found->id != participant || found->armed || !std::isfinite(initial_scale) ||
      initial_scale < 0.0 || initial_scale > 1.0 || (clock_ && clock_ != &clock) ||
      (initial_scale_ && *initial_scale_ != initial_scale)) {
    cancelled_ = true;
    changed_.notify_all();
    return std::nullopt;
  }
  clock_ = &clock;
  initial_scale_ = initial_scale;
  found->armed = true;
  found->stop = stop;
  if (--remaining_ == 0U) {
    if (std::any_of(participants_.begin(), participants_.end(),
                    [](const auto& row) { return row.stop.stop_requested(); })) {
      cancelled_ = true;
      changed_.notify_all();
      return std::nullopt;
    }
    anchor_.emplace(
        RuntimeScaleTimeAnchor{0.0, RuntimeScaleGovernor(initial_scale, bounds_), clock.now()});
    changed_.notify_all();
  }
  if (!changed_.wait(lock, stop, [&] { return cancelled_ || anchor_.has_value(); })) {
    cancelled_ = true;
    changed_.notify_all();
  }
  return anchor_;
}

std::optional<RuntimeScaleTimeAnchor> CoordinatedExecutionClock::sample(
    std::int64_t timestamp_ns) const {
  const auto anchor = [&] {
    std::lock_guard lock(mutex_);
    return anchor_;
  }();
  return anchor ? sample_runtime_scale_time(*anchor, timestamp_ns) : std::nullopt;
}

std::optional<RuntimeScaleTimeAnchor> CoordinatedExecutionClock::sample_now() const {
  std::lock_guard lock(mutex_);
  return anchor_ ? sample_runtime_scale_time(*anchor_, clock_->now()) : std::nullopt;
}

bool CoordinatedExecutionClock::retarget(double scale) {
  std::lock_guard lock(mutex_);
  if (!anchor_ || retargeting_frozen_ || command_) return false;
  auto next = sample_runtime_scale_time(*anchor_, clock_->now());
  if (!next || !next->governor.retarget(scale)) return false;
  anchor_ = std::move(next);
  return true;
}

std::optional<double> CoordinatedExecutionClock::target_scale() const {
  const auto value = target();
  return value ? std::optional{value->scale} : std::nullopt;
}

std::optional<CoordinatedExecutionClock::Target> CoordinatedExecutionClock::target() const {
  std::lock_guard lock(mutex_);
  const auto scale = anchor_ ? std::optional{anchor_->governor.target_scale()} : initial_scale_;
  return scale ? std::optional{Target{*scale, command_}} : std::nullopt;
}

bool CoordinatedExecutionClock::retarget(double scale,
                                         const CoordinatedRuntimeScaleCommandDTO& command) {
  std::lock_guard lock(mutex_);
  if (!anchor_ || retargeting_frozen_ || command.operation_id.empty() ||
      !command.operation_revision.nonzero() || command.generation.value() == 0U)
    return false;
  if (command_) {
    if (*command_ == command) return scale == anchor_->governor.target_scale();
    if (command.operation_id != command_->operation_id ||
        command.operation_revision < command_->operation_revision ||
        command.generation <= command_->generation)
      return false;
  }
  auto next = sample_runtime_scale_time(*anchor_, clock_->now());
  if (!next || !next->governor.retarget(scale)) return false;
  command_ = command;
  anchor_ = std::move(next);
  return true;
}

std::optional<std::int64_t> CoordinatedExecutionClock::freeze_retargeting() noexcept {
  std::lock_guard lock(mutex_);
  retargeting_frozen_ = true;
  if (!anchor_) {
    cancelled_ = true;
    changed_.notify_all();
  }
  return anchor_ ? std::optional{anchor_->timestamp_ns} : std::nullopt;
}

void CoordinatedExecutionClock::cancel_pending_start() noexcept {
  std::lock_guard lock(mutex_);
  if (anchor_) return;
  cancelled_ = true;
  changed_.notify_all();
}

bool CoordinatedExecutionClock::stop_prevented_start() const noexcept {
  std::lock_guard lock(mutex_);
  return retargeting_frozen_ && !anchor_;
}

}  // namespace manyforge::core::detail
