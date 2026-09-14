#pragma once

#include <condition_variable>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <memory>
#include <mutex>
#include <stop_token>
#include <vector>

namespace manyforge::core::detail {
class CoordinatedExecutionClock;

// One transient numerical join in the existing prepare-all lifecycle, not an
// execution owner. The addressed owner supplies its exact motion participants.
// Stop/cancellation wakes all waiters; this object never dispatches or stops.
class CoordinatedMotionPreparation final {
 public:
  explicit CoordinatedMotionPreparation(std::vector<OperationParticipantId> participants);
  [[nodiscard]] std::optional<RuntimeScalePreparation> arrive_and_wait(
      const OperationParticipantId& participant, const TrajectoryRuntimeScaleBoundsDTO& bounds,
      std::stop_token stop);
  void cancel() noexcept;
  [[nodiscard]] std::shared_ptr<CoordinatedExecutionClock> execution_clock();
  // The sealed candidate must carry the actual common result, not merely
  // compatible independent bounds. Does not arm or dispatch the participant.
  [[nodiscard]] bool accepts_prepared(const OperationParticipantId& participant,
                                      const TrajectoryRuntimeScaleBoundsDTO& bounds);

 private:
  struct Slot {
    OperationParticipantId participant;
    bool arrived{};
  };
  std::mutex mutex_;
  std::condition_variable_any changed_;
  std::vector<Slot> slots_;
  RuntimeScalePreparation common_;
  std::size_t remaining_{};
  bool cancelled_{};
  std::shared_ptr<CoordinatedExecutionClock> execution_clock_;
};

}  // namespace manyforge::core::detail
