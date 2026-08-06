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
};

}  // namespace manyforge::core
