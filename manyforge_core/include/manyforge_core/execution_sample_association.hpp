#pragma once

#include <algorithm>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <mutex>
#include <optional>

namespace manyforge::core {

/// The one owner of the association of controller evidence
/// with one goal (Submit, Replace or level-3 brake). Kept isolated so the rule
/// can be reverted in one place.
///
/// - Within the goal's binding epoch every piece of controller evidence (state
///   samples and the goal's action feedback) is ordered by controller source
///   stamp; nothing is compared across epochs or with local clocks.
/// - The goal's first anchor is its own feedback, or its own terminal result
///   when that arrives first. A bare cursor restart never anchors.
/// - Once anchored by feedback, a state sample belongs to the goal when its
///   source stamp is not older than the anchor and not older than the newest
///   sample already associated (later samples follow; no other goal is sent
///   meanwhile). A stamp after acceptance alone never associates a sample.
/// - The terminal result carries a local receipt only: it proves the result,
///   never a source stamp, and never assigns earlier samples to the goal. A
///   completion gate after the result takes a sample newer than the newest
///   evidence associated with the goal or, with none, than the newest sample
///   seen before the goal was sent (`NewerThanEvidence`).
///
/// Thread-safe: the participant and its Stop-stage helper share one instance
/// per goal.
class ExecutionSampleAssociation final {
 public:
  /// `pre_dispatch` is the newest sample seen before the goal was sent (it
  /// counts only on the goal's own epoch); `dispatched_at` the local send time.
  ExecutionSampleAssociation(ExecutionBindingEpoch epoch,
                             const std::optional<ExecutionControllerSampleDTO>& pre_dispatch,
                             TimePoint dispatched_at)
      : epoch_(epoch),
        pre_dispatch_source_(pre_dispatch && pre_dispatch->binding_epoch == epoch
                                 ? std::optional(pre_dispatch->source_stamp_ns)
                                 : std::nullopt),
        dispatched_at_(dispatched_at) {}

  ExecutionSampleAssociation(const ExecutionSampleAssociation&) = delete;
  ExecutionSampleAssociation& operator=(const ExecutionSampleAssociation&) = delete;

  /// Records the goal's facts: its first anchor and the source stamps of its
  /// own feedback. Returns whether the goal is anchored.
  bool ObserveGoal(const std::optional<ExecutionGoalObservationDTO>& goal) {
    std::lock_guard lock(mutex_);
    ObserveGoalLocked(goal);
    return anchor_.has_value();
  }

  /// Whether `sample` belongs to the goal's trajectory (see above); an
  /// associated sample becomes the goal's newest evidence.
  bool Observe(const ExecutionControllerSampleDTO& sample,
               const std::optional<ExecutionGoalObservationDTO>& goal) {
    std::lock_guard lock(mutex_);
    ObserveGoalLocked(goal);
    if (!anchor_ || !anchor_->source_stamp || sample.binding_epoch != epoch_ ||
        sample.source_stamp_ns < *anchor_->source_stamp ||
        (newest_sample_ && sample.source_stamp_ns < *newest_sample_)) {
      return false;
    }
    newest_sample_ = sample.source_stamp_ns;
    Raise(sample.source_stamp_ns);
    return true;
  }

  /// Whether `sample` (same epoch) is newer by source stamp than the newest
  /// evidence associated with the goal or, with none, than the newest sample
  /// seen before the goal was sent. Never associates the sample.
  [[nodiscard]] bool NewerThanEvidence(const ExecutionControllerSampleDTO& sample) const {
    std::lock_guard lock(mutex_);
    if (sample.binding_epoch != epoch_) return false;
    const auto bar = newest_evidence_ ? newest_evidence_ : pre_dispatch_source_;
    return !bar || sample.source_stamp_ns > *bar;
  }

  [[nodiscard]] bool anchored() const {
    std::lock_guard lock(mutex_);
    return anchor_.has_value();
  }

  /// Recognition delay: dispatch to anchor on the local clock.
  [[nodiscard]] std::optional<Duration> recognition_delay_ns() const {
    std::lock_guard lock(mutex_);
    if (!anchor_) return std::nullopt;
    return std::max<Duration>(0, anchor_->local_receipt - dispatched_at_);
  }

  [[nodiscard]] TimePoint recognition_origin_ns() const noexcept { return dispatched_at_; }
  [[nodiscard]] std::optional<TimePoint> recognition_receipt_ns() const {
    std::lock_guard lock(mutex_);
    return anchor_ ? std::optional(anchor_->local_receipt) : std::nullopt;
  }

 private:
  struct Anchor {
    /// Feedback anchors carry its controller stamp; a result anchor has none.
    std::optional<TimePoint> source_stamp;
    TimePoint local_receipt;
  };

  void ObserveGoalLocked(const std::optional<ExecutionGoalObservationDTO>& goal) {
    if (!goal || goal->binding_epoch != epoch_) return;
    if (!anchor_) {
      if (goal->latest_feedback) {
        anchor_ = Anchor{goal->latest_feedback->source_stamp_ns,
                         goal->latest_feedback->local_receipt_stamp_ns};
      } else if (goal->phase == ExecutionGoalPhase::kTerminal &&
                 goal->terminal_local_receipt_stamp) {
        anchor_ = Anchor{std::nullopt, *goal->terminal_local_receipt_stamp};
      }
    }
    // Feedback of a goal anchored by its result is never used (no
    // retroactive assignment).
    if (anchor_ && anchor_->source_stamp && goal->latest_feedback) {
      Raise(goal->latest_feedback->source_stamp_ns);
    }
  }

  void Raise(TimePoint source_stamp) {
    newest_evidence_ = newest_evidence_ ? std::max(*newest_evidence_, source_stamp) : source_stamp;
  }

  const ExecutionBindingEpoch epoch_;
  const std::optional<TimePoint> pre_dispatch_source_;
  const TimePoint dispatched_at_;
  mutable std::mutex mutex_;
  std::optional<Anchor> anchor_;
  std::optional<TimePoint> newest_sample_;
  std::optional<TimePoint> newest_evidence_;
};

}  // namespace manyforge::core
