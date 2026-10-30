#pragma once

#include <functional>
#include <manyforge_core/component_lifecycle_admission_query.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <stop_token>

namespace manyforge::core {

/// Receives one feedback event while a transition action runs, on the
/// dispatching thread.
using LifecycleSubstepFeedbackSink = std::function<void(const LifecycleSubstepEventDTO&)>;

/// ROS-independent boundary for one exact lifecycle provider occurrence.
///
/// CCLC owns transaction sequencing, deadlines, reservations, and terminal
/// results. A brand adapter exposes normalized observations, answers
/// admission as a pure query, and runs one transition action per dispatch
/// with its own substeps (V-B2).
class IBrandLifecyclePort {
 public:
  virtual ~IBrandLifecyclePort() = default;

  [[nodiscard]] virtual Result<BrandLifecycleObservationDTO> Observe(
      const BrandLifecycleObservationQueryDTO& query) = 0;

  /// Pure: no I/O, no observation bookkeeping. Lives as long as the port.
  [[nodiscard]] virtual const ILifecycleAdmission& Admission() const = 0;

  [[nodiscard]] virtual BrandLifecyclePrimitiveResultDTO Dispatch(
      const BrandLifecyclePrimitiveIntentDTO& intent, const LifecycleSubstepFeedbackSink& feedback,
      std::stop_token cancellation) = 0;
};

}  // namespace manyforge::core
