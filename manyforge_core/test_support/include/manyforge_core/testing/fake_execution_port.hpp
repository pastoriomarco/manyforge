#pragma once

#include <manyforge_core/ports/iexecution_port.hpp>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core::testing {

/// Scriptable `IExecutionPort` test double.
///
/// Records every command and returns the scripted result (default
/// `kDispatched`). A dispatched `Submit`/`Replace` registers the new key as a
/// `kPending` goal on the current binding epoch; every later phase, result,
/// sample, readiness and timing fact is scripted by the test. The fake never
/// infers facts: a `Cancel` does not end a goal, a `Replace` does not preempt
/// the active goal, and samples are never attributed to a goal.
///
/// Goal records follow the port's ring: beyond `kExecutionGoalRecordCapacity`
/// records the oldest terminal record is dropped (reads as `nullopt`);
/// non-terminal records are never dropped, and a new goal is refused as
/// `kNotReady` while every record is unfinished.
class FakeExecutionPort final : public IExecutionPort {
 public:
  struct SubmitCall {
    ExecutionGoalKey key;
    TrajectoryDTO trajectory;
  };
  struct ReplaceCall {
    ExecutionGoalKey active_key;
    ExecutionGoalKey next_key;
    TrajectoryDTO trajectory;
    std::optional<ExecutionReplacementStartDTO> start{};
  };
  struct CancelCall {
    ExecutionGoalKey key;
  };
  struct SetScaleCall {
    double value;
  };
  using Command = std::variant<SubmitCall, ReplaceCall, CancelCall, SetScaleCall>;

  // --- IExecutionPort -----------------------------------------------------

  ExecutionPortCommandResultDTO Submit(const ExecutionGoalKey& key,
                                       const TrajectoryDTO& trajectory) override {
    std::lock_guard lock(mutex_);
    commands_.emplace_back(SubmitCall{key, trajectory});
    if (RingFullLocked()) return NotReady();
    const auto result = NextResultLocked();
    if (result.dispatch != ExecutionPortDispatch::kNotDispatched) {
      RegisterPendingLocked(key);
    }
    return result;
  }

  ExecutionPortCommandResultDTO Replace(const ExecutionGoalKey& active_key,
                                        const ExecutionGoalKey& next_key,
                                        const TrajectoryDTO& trajectory) override {
    std::lock_guard lock(mutex_);
    commands_.emplace_back(ReplaceCall{active_key, next_key, trajectory});
    if (RingFullLocked()) return NotReady();
    const auto result = NextResultLocked();
    if (result.dispatch != ExecutionPortDispatch::kNotDispatched) {
      RegisterPendingLocked(next_key);
    }
    return result;
  }

  ExecutionPortCommandResultDTO ReplaceScheduled(
      const ExecutionGoalKey& active_key, const ExecutionGoalKey& next_key,
      const TrajectoryDTO& trajectory, const ExecutionReplacementStartDTO& start) override {
    std::lock_guard lock(mutex_);
    commands_.emplace_back(ReplaceCall{active_key, next_key, trajectory, start});
    if (RingFullLocked()) return NotReady();
    const auto result = NextResultLocked();
    if (result.dispatch != ExecutionPortDispatch::kNotDispatched) RegisterPendingLocked(next_key);
    return result;
  }

  ExecutionPortCommandResultDTO Cancel(const ExecutionGoalKey& key) override {
    std::lock_guard lock(mutex_);
    commands_.emplace_back(CancelCall{key});
    return NextResultLocked();
  }

  ExecutionPortCommandResultDTO SetScale(double value) override {
    std::lock_guard lock(mutex_);
    commands_.emplace_back(SetScaleCall{value});
    return NextResultLocked();
  }

  std::optional<ExecutionGoalObservationDTO> ReadGoal(
      const ExecutionGoalKey& key) const override {
    std::lock_guard lock(mutex_);
    const auto it = goals_.find(key);
    if (it == goals_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  std::optional<ExecutionControllerSampleDTO> ReadSample() const override {
    std::lock_guard lock(mutex_);
    if (on_read_sample_) on_read_sample_(sample_);
    return sample_;
  }

  ExecutionReadinessDTO ReadReadiness() const override {
    std::lock_guard lock(mutex_);
    return readiness_;
  }

  ExecutionTimingDTO ReadTiming() override {
    std::lock_guard lock(mutex_);
    ++timing_reads_;
    return timing_;
  }

  ExecutionLaneFactsDTO ReadLaneFacts() const override {
    std::lock_guard lock(mutex_);
    return lane_facts_;
  }

  // --- Scripting ----------------------------------------------------------

  /// Results returned by the next commands, in order; `kDispatched` when empty.
  void QueueCommandResult(ExecutionPortCommandResultDTO result) {
    std::lock_guard lock(mutex_);
    queued_results_.push_back(std::move(result));
  }

  void SetGoalPhase(const ExecutionGoalKey& key, ExecutionGoalPhase phase,
                    std::optional<TimePoint> accepted_stamp_ns = std::nullopt) {
    std::lock_guard lock(mutex_);
    auto& goal = GoalLocked(key);
    goal.phase = phase;
    if (accepted_stamp_ns.has_value()) {
      goal.accepted_stamp_ns = accepted_stamp_ns;
    }
  }

  /// Script a terminal result. `preempted_by` is required with, and only
  /// meaningful for, `kPreemptedBy`.
  void FinishGoal(const ExecutionGoalKey& key, ExecutionGoalResult result,
                  std::optional<ExecutionGoalKey> preempted_by = std::nullopt,
                  std::optional<TimePoint> terminal_local_receipt_stamp = std::nullopt) {
    std::lock_guard lock(mutex_);
    auto& goal = GoalLocked(key);
    goal.phase = ExecutionGoalPhase::kTerminal;
    goal.result = result;
    goal.preempted_by = std::move(preempted_by);
    goal.terminal_local_receipt_stamp = terminal_local_receipt_stamp;
    TrimLocked();
  }

  void SetGoal(ExecutionGoalObservationDTO goal) {
    std::lock_guard lock(mutex_);
    const auto key = goal.key;
    StoreLocked(key, std::move(goal));
  }

  /// Script the goal's latest action feedback.
  void SetGoalFeedback(const ExecutionGoalKey& key, ExecutionGoalFeedbackDTO feedback) {
    std::lock_guard lock(mutex_);
    GoalLocked(key).latest_feedback = std::move(feedback);
  }

  /// Script the time the cancel was actually sent. Does not end the goal.
  void MarkCancelSent(const ExecutionGoalKey& key, TimePoint local_stamp) {
    std::lock_guard lock(mutex_);
    GoalLocked(key).cancel_sent_local_stamp = local_stamp;
  }

  /// Script the receipt of a cancel acknowledgement. Does not end the goal.
  void AcknowledgeCancel(const ExecutionGoalKey& key, TimePoint local_receipt_stamp) {
    std::lock_guard lock(mutex_);
    GoalLocked(key).cancel_ack_local_receipt_stamp = local_receipt_stamp;
  }

  void SetSample(std::optional<ExecutionControllerSampleDTO> sample) {
    std::lock_guard lock(mutex_);
    sample_ = std::move(sample);
  }

  /// Runs inside every `ReadSample`, under the port lock, before the stored
  /// sample is returned; it may change the stored sample (e.g. a receipt that
  /// lands between a reader's clock read and its sample read). Must not call
  /// back into the port.
  void SetOnReadSample(std::function<void(std::optional<ExecutionControllerSampleDTO>&)> hook) {
    std::lock_guard lock(mutex_);
    on_read_sample_ = std::move(hook);
  }

  void SetReadiness(ExecutionReadinessDTO readiness) {
    std::lock_guard lock(mutex_);
    readiness_ = readiness;
  }

  void SetTiming(ExecutionTimingDTO timing) {
    std::lock_guard lock(mutex_);
    timing_ = timing;
  }

  /// Scripted lane facts, including the controller joint order that scripted
  /// samples must follow. Not checked against submitted trajectories.
  void SetLaneFacts(ExecutionLaneFactsDTO facts) {
    std::lock_guard lock(mutex_);
    lane_facts_ = std::move(facts);
  }

  // --- Inspection ---------------------------------------------------------

  [[nodiscard]] std::vector<Command> commands() const {
    std::lock_guard lock(mutex_);
    return commands_;
  }

  [[nodiscard]] std::size_t timing_reads() const {
    std::lock_guard lock(mutex_);
    return timing_reads_;
  }

 private:
  /// The ring is full when every record is still unfinished; a new goal is
  /// then refused as not ready instead of dropping a goal Core may still need.
  bool RingFullLocked() const {
    return std::count_if(goals_.begin(), goals_.end(), [](const auto& entry) {
             return entry.second.phase != ExecutionGoalPhase::kTerminal;
           }) >= static_cast<std::ptrdiff_t>(kExecutionGoalRecordCapacity);
  }

  static ExecutionPortCommandResultDTO NotReady() {
    return ExecutionPortCommandResultDTO{ExecutionPortDispatch::kNotDispatched,
                                         ExecutionPortRefusal::kNotReady, std::nullopt};
  }

  ExecutionPortCommandResultDTO NextResultLocked() {
    if (queued_results_.empty()) {
      return ExecutionPortCommandResultDTO{ExecutionPortDispatch::kDispatched, std::nullopt,
                                           std::nullopt};
    }
    auto result = std::move(queued_results_.front());
    queued_results_.erase(queued_results_.begin());
    return result;
  }

  void RegisterPendingLocked(const ExecutionGoalKey& key) {
    ExecutionGoalObservationDTO goal;
    goal.key = key;
    goal.mechanism_goal_id =
        MechanismGoalId("fake-goal-" + std::to_string(++mechanism_goal_counter_));
    goal.binding_epoch = readiness_.binding_epoch;
    goal.phase = ExecutionGoalPhase::kPending;
    StoreLocked(key, std::move(goal));
  }

  ExecutionGoalObservationDTO& GoalLocked(const ExecutionGoalKey& key) {
    if (goals_.find(key) == goals_.end()) {
      ExecutionGoalObservationDTO goal;
      goal.key = key;
      goal.binding_epoch = readiness_.binding_epoch;
      StoreLocked(key, std::move(goal));
    }
    return goals_.at(key);
  }

  /// Insert or replace a record; a new key joins the ring as the newest.
  void StoreLocked(const ExecutionGoalKey& key, ExecutionGoalObservationDTO goal) {
    if (goals_.insert_or_assign(key, std::move(goal)).second) {
      record_order_.push_back(key);
    }
    TrimLocked();
  }

  /// Drop the oldest terminal records beyond the ring capacity.
  void TrimLocked() {
    while (goals_.size() > kExecutionGoalRecordCapacity) {
      const auto oldest_terminal =
          std::find_if(record_order_.begin(), record_order_.end(), [this](const auto& key) {
            return goals_.at(key).phase == ExecutionGoalPhase::kTerminal;
          });
      if (oldest_terminal == record_order_.end()) {
        return;
      }
      goals_.erase(*oldest_terminal);
      record_order_.erase(oldest_terminal);
    }
  }

  mutable std::mutex mutex_;
  std::vector<Command> commands_;
  std::vector<ExecutionPortCommandResultDTO> queued_results_;
  std::map<ExecutionGoalKey, ExecutionGoalObservationDTO> goals_;
  /// Keys of `goals_`, oldest first.
  std::deque<ExecutionGoalKey> record_order_;
  mutable std::optional<ExecutionControllerSampleDTO> sample_;
  std::function<void(std::optional<ExecutionControllerSampleDTO>&)> on_read_sample_;
  ExecutionReadinessDTO readiness_{true, true, true, ExecutionBindingEpoch{1U}};
  ExecutionTimingDTO timing_{};
  ExecutionLaneFactsDTO lane_facts_{};
  std::size_t timing_reads_{0U};
  std::uint64_t mechanism_goal_counter_{0U};
};

}  // namespace manyforge::core::testing
