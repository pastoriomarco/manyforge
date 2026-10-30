#pragma once

#include <cstdint>
#include <functional>
#include <manyforge_core/component_lifecycle_admission_query.hpp>
#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <optional>
#include <stop_token>
#include <vector>

namespace manyforge::core {

/// One substep's outcome, already mapped to the three outcomes by the
/// adapter. `nested` lists substeps it ran inside this one (their `within`
/// names it), for the record.
struct LifecycleSubstepDispatchOutcome final {
  LifecycleTransitionOutcomeDTO outcome;
  std::optional<ErrorDTO> error;
  std::vector<LifecycleSubstepResultDTO> nested{};
};

/// The adapter's operations the runner uses.
struct LifecycleTransitionRunnerIo final {
  std::function<Result<BrandLifecycleObservationDTO>()> observe;
  /// Runs one substep with the generation of the observation the decision
  /// used and the transaction's remaining time.
  std::function<LifecycleSubstepDispatchOutcome(
      const LifecyclePrimitiveId& substep, std::uint64_t generation, std::int64_t remaining_ns,
      const LifecycleSubstepFeedbackSink& feedback, std::stop_token cancellation)>
      dispatch_substep;
  /// Test seam; production: `WaitLifecycleInterruptibly`.
  std::function<void(std::int64_t ns, std::stop_token cancellation)> wait;
  IClock* clock{};
  std::int64_t poll_period_ns{};
};

/// Sleeps `ns`, returning early when `cancellation` is requested.
void WaitLifecycleInterruptibly(std::int64_t ns, std::stop_token cancellation);

/// Runs one transition action (`intent.lifecycle_primitive_id` is
/// `transition.<intent>`) within `intent.remaining_duration_ns`: observe,
/// ask `admission`, dispatch the selected substep, wait for its
/// postcondition on a newer fresh observation, repeat. Already done ends
/// succeeded; not ready, a failed substep, the deadline, the substep bound or
/// cancellation end failed; a lost connection ends connection lost. Nothing
/// is retried or reconciled. `substeps` lists every substep run; started and
/// finished events go to `feedback`.
[[nodiscard]] BrandLifecyclePrimitiveResultDTO RunLifecycleTransition(
    const BrandLifecyclePrimitiveIntentDTO& intent, const ILifecycleAdmission& admission,
    const LifecycleTransitionRunnerIo& io, const LifecycleSubstepFeedbackSink& feedback,
    std::stop_token cancellation);

}  // namespace manyforge::core
