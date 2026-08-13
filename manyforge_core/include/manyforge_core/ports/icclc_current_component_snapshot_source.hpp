#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Read-only coherent current-state input for the lifecycle coordinator.
class ICCLCCurrentComponentSnapshotSource {
 public:
  virtual ~ICCLCCurrentComponentSnapshotSource() = default;

  [[nodiscard]] virtual Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponentSnapshot(
      const ControllableComponentId& controllable_component_id) const = 0;
};

}  // namespace manyforge::core
