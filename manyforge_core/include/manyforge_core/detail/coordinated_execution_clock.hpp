#pragma once

#include <condition_variable>
#include <manyforge_core/detail/runtime_scale_governor.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <mutex>
#include <stop_token>
#include <vector>

namespace manyforge::core::detail {

// One process-local law for explicitly coordinated, already-authorized native
// workers. No thread, dispatch authority, physical Stop or retained history.
class CoordinatedExecutionClock final {
 public:
  CoordinatedExecutionClock(std::vector<OperationParticipantId> participants,
                            RuntimeScaleBounds bounds);
  [[nodiscard]] bool accepts(const OperationParticipantId& participant,
                             RuntimeScaleBounds bounds) const;
  [[nodiscard]] std::optional<RuntimeScaleTimeAnchor> start_and_wait(
      const OperationParticipantId& participant, IClock& clock, double initial_scale,
      std::stop_token stop);
  [[nodiscard]] std::optional<RuntimeScaleTimeAnchor> sample(std::int64_t timestamp_ns) const;
  // Read the injected clock inside the same lock as target publication, so a
  // concurrent update cannot put a reader behind the latest retained anchor.
  [[nodiscard]] std::optional<RuntimeScaleTimeAnchor> sample_now() const;
  [[nodiscard]] std::optional<double> target_scale() const;
  struct Target {
    double scale;
    std::optional<CoordinatedRuntimeScaleCommandDTO> command;
  };
  [[nodiscard]] std::optional<Target> target() const;
  [[nodiscard]] bool retarget(double scale);
  [[nodiscard]] bool retarget(double scale, const CoordinatedRuntimeScaleCommandDTO& command);
  // Stop owns this occurrence from here on. Preserve its current time law,
  // but reject further speed targets; a resumed occurrence gets a fresh clock.
  // Before release, prevent the original motion from starting instead.
  // Returns the final law's origin, if released. A worker publication strictly
  // after it cannot predate that law, including equal-timestamp retarget races.
  std::optional<std::int64_t> freeze_retargeting() noexcept;
  [[nodiscard]] bool stop_prevented_start() const noexcept;
  // Only pending start is cancelled. Once released, existing owner Stop
  // semantics contain motion; completion of one participant must not erase
  // the others' clock.
  void cancel_pending_start() noexcept;

 private:
  struct Participant {
    OperationParticipantId id;
    bool armed{};
    std::stop_token stop;
  };
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::vector<Participant> participants_;
  RuntimeScaleBounds bounds_;
  std::size_t remaining_;
  const IClock* clock_{};
  std::optional<double> initial_scale_;
  std::optional<RuntimeScaleTimeAnchor> anchor_;
  bool cancelled_{};
  bool retargeting_frozen_{};
  std::optional<CoordinatedRuntimeScaleCommandDTO> command_;
};

}  // namespace manyforge::core::detail
