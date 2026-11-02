#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/observed_state.hpp>

namespace manyforge::core {

/// Resource-addressed observed-state owner port. Implementations select only
/// deployment-qualified sources and never fall back to planner scene state.
class IObservedStateProvider {
 public:
  virtual ~IObservedStateProvider() = default;

  [[nodiscard]] virtual Result<ObservedStateSnapshotDTO> current_state(
      const ObservedStateQueryContextDTO& context,
      std::int64_t now_ns) const = 0;

  [[nodiscard]] virtual Result<ObservedStatePublicationDTO> publication(
      const ControlBindingId& control_binding_id,
      const ControlBindingRevision& control_binding_revision) const = 0;

  /// Acquire the publication valid at a receiver-local evaluation time.
  /// Legacy providers retain a safe compatibility fallback for non-future
  /// latest publications; bounded owners override this to retain prior samples.
  [[nodiscard]] virtual Result<ObservedStatePublicationDTO> publication_at(
      const ControlBindingId& control_binding_id,
      const ControlBindingRevision& control_binding_revision,
      std::int64_t now_ns) const {
    auto result = publication(control_binding_id, control_binding_revision);
    if (!result.ok() || result.value().received_timestamp_ns <= now_ns) {
      return result;
    }
    return Result<ObservedStatePublicationDTO>::Err(ErrorDTO{
        to_string(ErrorCode::kInvalidRequest),
        "observed state is unavailable at the requested evaluation time",
        {{"reason", "observed_state_unavailable_at_evaluation_time"}}});
  }
};

}  // namespace manyforge::core
