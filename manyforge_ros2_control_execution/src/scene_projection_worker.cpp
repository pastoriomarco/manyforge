#include "scene_projection_worker.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace manyforge::ros2_control_execution::detail {
namespace {

constexpr std::size_t kTerminalAttempts = 10;
constexpr auto kTerminalRetryDelay = std::chrono::milliseconds(20);

std::uint64_t nonnegative_ns(std::chrono::steady_clock::duration duration) {
  return static_cast<std::uint64_t>(std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()));
}

}  // namespace

SceneProjectionWorker::Suspension::Suspension(Suspension&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

SceneProjectionWorker::Suspension&
SceneProjectionWorker::Suspension::operator=(Suspension&& other) noexcept {
  if (this != &other) {
    release();
    owner_ = std::exchange(other.owner_, nullptr);
  }
  return *this;
}

SceneProjectionWorker::Suspension::~Suspension() {
  release();
}

void SceneProjectionWorker::Suspension::release() noexcept {
  if (owner_ != nullptr) {
    owner_->resume_one_suspension();
    owner_ = nullptr;
  }
}

SceneProjectionWorker::SceneProjectionWorker(
    std::shared_ptr<core::IExecutionStateSink> sink,
    std::string arm_group_id,
    core::IClock& clock)
    : sink_(std::move(sink)), clock_(clock) {
  if (!sink_) {
    throw std::invalid_argument("scene projection worker requires a sink");
  }
  if (arm_group_id.empty()) {
    throw std::invalid_argument("scene projection worker requires an arm group");
  }
  slots_.push_back(Slot{
      TargetKind::kGroup,
      std::move(arm_group_id),
      {},
      {},
      std::nullopt,
  });
  worker_ = std::jthread([this](std::stop_token stop_token) {
    run(stop_token);
  });
}

SceneProjectionWorker::~SceneProjectionWorker() {
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

SceneProjectionSlotId SceneProjectionWorker::register_joint_slot(
    std::string group_id,
    std::string joint_name,
    SceneProjectionCompletionCallback completion_callback) {
  if (group_id.empty() || joint_name.empty()) {
    throw std::invalid_argument(
        "joint scene projection slot requires group and joint names");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (std::size_t index = 1; index < slots_.size(); ++index) {
    auto& slot = slots_[index];
    if (slot.target_kind == TargetKind::kJoint &&
        slot.group_id == group_id && slot.joint_name == joint_name) {
      if (slot.pending.has_value()) {
        ++metrics_.requests_discarded_total;
        slot.pending.reset();
      }
      slot.completion_callback = std::move(completion_callback);
      drain_cv_.notify_all();
      return index;
    }
  }
  slots_.push_back(Slot{
      TargetKind::kJoint,
      std::move(group_id),
      std::move(joint_name),
      std::move(completion_callback),
      std::nullopt,
  });
  return slots_.size() - 1;
}

bool SceneProjectionWorker::submit_active(
    std::vector<double> positions_rad,
    std::int64_t source_timestamp_ns) {
  return submit_group(RequestKind::kActive, std::move(positions_rad),
                      source_timestamp_ns);
}

bool SceneProjectionWorker::submit_terminal(
    std::vector<double> positions_rad,
    std::int64_t source_timestamp_ns) {
  return submit_group(RequestKind::kTerminal, std::move(positions_rad),
                      source_timestamp_ns);
}

bool SceneProjectionWorker::submit_active_joint(
    SceneProjectionSlotId slot_id,
    double position_rad,
    std::int64_t source_timestamp_ns,
    SceneProjectionToken token) {
  return submit_joint(RequestKind::kActive, slot_id, position_rad,
                      source_timestamp_ns, token);
}

bool SceneProjectionWorker::submit_terminal_joint(
    SceneProjectionSlotId slot_id,
    double position_rad,
    std::int64_t source_timestamp_ns,
    SceneProjectionToken token) {
  return submit_joint(RequestKind::kTerminal, slot_id, position_rad,
                      source_timestamp_ns, token);
}

bool SceneProjectionWorker::submit_group(
    RequestKind kind,
    std::vector<double> positions_rad,
    std::int64_t source_timestamp_ns) {
  if (positions_rad.empty()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return submit_locked(
      0,
      Request{
          kind,
          next_sequence_++,
          std::move(positions_rad),
          0.0,
          source_timestamp_ns,
          {},
          std::chrono::steady_clock::now(),
      });
}

bool SceneProjectionWorker::submit_joint(
    RequestKind kind,
    SceneProjectionSlotId slot_id,
    double position_rad,
    std::int64_t source_timestamp_ns,
    SceneProjectionToken token) {
  if (!std::isfinite(position_rad)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (slot_id >= slots_.size() ||
      slots_[slot_id].target_kind != TargetKind::kJoint) {
    return false;
  }
  ++metrics_.joint_requests_total;
  return submit_locked(
      slot_id,
      Request{
          kind,
          next_sequence_++,
          {},
          position_rad,
          source_timestamp_ns,
          token,
          std::chrono::steady_clock::now(),
      });
}

bool SceneProjectionWorker::submit_locked(
    SceneProjectionSlotId slot_id,
    Request request) {
  if (exited_ || worker_.get_stop_token().stop_requested()) {
    return false;
  }
  if (suspension_count_ > 0) {
    ++metrics_.requests_rejected_while_suspended_total;
    return false;
  }
  if (request.kind == RequestKind::kActive) {
    ++metrics_.active_requests_total;
  }
  auto& slot = slots_[slot_id];
  if (slot.pending.has_value()) {
    ++metrics_.requests_coalesced_total;
  }
  slot.pending = std::move(request);
  work_cv_.notify_one();
  return true;
}

SceneProjectionWorker::Suspension SceneProjectionWorker::suspend() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++suspension_count_;
  if (suspension_count_ == 1) {
    for (auto& slot : slots_) {
      if (slot.pending.has_value()) {
        ++metrics_.requests_discarded_total;
        slot.pending.reset();
      }
    }
  }
  work_cv_.notify_all();
  drain_cv_.notify_all();
  return Suspension(this);
}

SceneProjectionWorkerMetrics SceneProjectionWorker::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_;
}

bool SceneProjectionWorker::drain_for(std::chrono::nanoseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return drain_cv_.wait_for(lock, timeout, [this]() {
    return !has_pending_locked() && !in_flight_;
  });
}

bool SceneProjectionWorker::stop_and_join_for(
    std::chrono::nanoseconds timeout) noexcept {
  if (!worker_.joinable()) {
    return true;
  }
  worker_.request_stop();
  {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!drain_cv_.wait_for(lock, timeout, [this]() { return exited_; })) {
      return false;
    }
  }
  worker_.join();
  return true;
}

bool SceneProjectionWorker::has_pending_locked() const {
  return std::any_of(slots_.begin(), slots_.end(), [](const Slot& slot) {
    return slot.pending.has_value();
  });
}

SceneProjectionWorker::WorkItem SceneProjectionWorker::take_next_locked() {
  SceneProjectionSlotId selected = slots_.size();
  std::uint64_t selected_sequence = std::numeric_limits<std::uint64_t>::max();
  for (std::size_t index = 0; index < slots_.size(); ++index) {
    if (slots_[index].pending.has_value() &&
        slots_[index].pending->sequence < selected_sequence) {
      selected = index;
      selected_sequence = slots_[index].pending->sequence;
    }
  }
  if (selected == slots_.size()) {
    throw std::logic_error("scene projection worker woke without pending work");
  }
  auto& slot = slots_[selected];
  WorkItem item;
  item.slot_id = selected;
  item.target_kind = slot.target_kind;
  item.group_id = slot.group_id;
  item.joint_name = slot.joint_name;
  item.completion_callback = slot.completion_callback;
  item.request = std::move(*slot.pending);
  slot.pending.reset();
  return item;
}

bool SceneProjectionWorker::request_superseded_locked(
    SceneProjectionSlotId slot_id,
    std::uint64_t sequence) const {
  return suspension_count_ > 0 ||
         (slot_id < slots_.size() && slots_[slot_id].pending.has_value() &&
          slots_[slot_id].pending->sequence > sequence);
}

void SceneProjectionWorker::run(std::stop_token stop_token) noexcept {
#if defined(__linux__)
  (void)pthread_setname_np(pthread_self(), "mf_scene_proj");
#endif
  while (!stop_token.stop_requested()) {
    WorkItem item;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock, stop_token, [this]() {
        return suspension_count_ == 0 && has_pending_locked();
      });
      if (stop_token.stop_requested()) {
        break;
      }
      try {
        item = take_next_locked();
      } catch (...) {
        std::terminate();
      }
      in_flight_ = true;
    }

    const auto queue_lag_ns = nonnegative_ns(
        std::chrono::steady_clock::now() - item.request.submitted_at);
    const std::size_t max_attempts =
        item.request.kind == RequestKind::kTerminal ? kTerminalAttempts : 1;
    core::ExecutionStateSinkResult result{
        core::ExecutionStateSinkStatus::kException};
    std::size_t attempts = 0;
    bool superseded = false;
    for (std::size_t attempt = 0;
         attempt < max_attempts && !stop_token.stop_requested(); ++attempt) {
      const auto apply_started_at = std::chrono::steady_clock::now();
      try {
        if (item.target_kind == TargetKind::kGroup) {
          result = sink_->apply_group_joint_positions(
              item.group_id, item.request.positions_rad,
              core::ExecutionStateUpdateMode::kBestEffort);
        } else {
          result = sink_->apply_group_joint_position(
              item.group_id, item.joint_name, item.request.position_rad,
              core::ExecutionStateUpdateMode::kBestEffort);
        }
      } catch (...) {
        result.status = core::ExecutionStateSinkStatus::kException;
      }
      ++attempts;
      const auto apply_duration_ns = nonnegative_ns(
          std::chrono::steady_clock::now() - apply_started_at);
      record_attempt(item.request, result, apply_duration_ns, queue_lag_ns,
                     attempt);
      if (result.applied()) {
        break;
      }
      if (item.request.kind != RequestKind::kTerminal ||
          attempt + 1 >= max_attempts) {
        break;
      }
      if (!wait_before_terminal_retry(stop_token, item.slot_id,
                                      item.request.sequence)) {
        // A false non-stop return means the token-aware wait observed either a
        // newer request for this target or an authoritative suspension. Do not
        // re-read the transient predicate after releasing the wait lock: the
        // suspension may already have resumed, which would misclassify the
        // interrupted terminal retry as an ordinary miss and strand it pending.
        superseded = !stop_token.stop_requested();
        break;
      }
    }

    if (!stop_token.stop_requested()) {
      SceneProjectionCompletion completion;
      completion.token = item.request.token;
      completion.sink_status = result.status;
      completion.attempts = attempts;
      if (result.applied()) {
        completion.outcome = SceneProjectionOutcome::kApplied;
      } else if (superseded) {
        completion.outcome = SceneProjectionOutcome::kSuperseded;
      } else if (item.request.kind == RequestKind::kTerminal &&
                 attempts == max_attempts) {
        completion.outcome = SceneProjectionOutcome::kRetryExhausted;
        record_retry_exhausted();
      } else {
        completion.outcome = SceneProjectionOutcome::kMissed;
      }
      invoke_completion(item.completion_callback, completion);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      in_flight_ = false;
    }
    drain_cv_.notify_all();
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
      slot.pending.reset();
    }
    in_flight_ = false;
    exited_ = true;
  }
  drain_cv_.notify_all();
}

bool SceneProjectionWorker::wait_before_terminal_retry(
    std::stop_token stop_token,
    SceneProjectionSlotId slot_id,
    std::uint64_t request_sequence) {
  std::unique_lock<std::mutex> lock(mutex_);
  const bool interrupted = work_cv_.wait_for(
      lock, stop_token, kTerminalRetryDelay,
      [this, slot_id, request_sequence]() {
        return request_superseded_locked(slot_id, request_sequence);
      });
  return !stop_token.stop_requested() && !interrupted;
}

void SceneProjectionWorker::record_attempt(
    const Request& request,
    core::ExecutionStateSinkResult result,
    std::uint64_t apply_duration_ns,
    std::uint64_t queue_lag_ns,
    std::size_t attempt) {
  std::lock_guard<std::mutex> lock(mutex_);
  metrics_.apply_duration_max_ns =
      std::max(metrics_.apply_duration_max_ns, apply_duration_ns);
  metrics_.queue_lag_max_ns =
      std::max(metrics_.queue_lag_max_ns, queue_lag_ns);
  if (result.applied()) {
    ++metrics_.successful_updates_total;
    const auto now_ns = clock_.now();
    metrics_.last_success_timestamp_ns = now_ns;
    if (request.source_timestamp_ns > 0 && now_ns >= request.source_timestamp_ns) {
      metrics_.lag_max_ns = std::max(
          metrics_.lag_max_ns,
          static_cast<std::uint64_t>(now_ns - request.source_timestamp_ns));
    }
    if (request.kind == RequestKind::kTerminal && attempt > 0) {
      ++metrics_.terminal_retry_successes_total;
    }
    return;
  }

  ++metrics_.misses_total;
  if (request.kind == RequestKind::kTerminal && attempt == 0) {
    ++metrics_.terminal_retries_scheduled_total;
  }
}

void SceneProjectionWorker::record_retry_exhausted() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++metrics_.terminal_retry_exhausted_total;
}

void SceneProjectionWorker::invoke_completion(
    const SceneProjectionCompletionCallback& callback,
    const SceneProjectionCompletion& completion) noexcept {
  if (!callback) {
    return;
  }
  try {
    callback(completion);
  } catch (...) {
    // Projection status is diagnostic/control admission state. A callback bug
    // must not kill the process or strand the worker; the next status read stays
    // conservatively pending and reset remains the recovery boundary.
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.completion_callback_exceptions_total;
  }
}

void SceneProjectionWorker::resume_one_suspension() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (suspension_count_ == 0) {
      return;
    }
    --suspension_count_;
  }
  work_cv_.notify_all();
}

}  // namespace manyforge::ros2_control_execution::detail
