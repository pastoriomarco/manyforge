#pragma once

#include <manyforge_core/ports/icontroller_lifecycle_port.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <memory>

namespace manyforge::core {

/// Resolve the exact retained controller-product source incarnation frozen in
/// an accepted route transaction. Resolution performs no controller effect.
class ICCLCControllerLifecyclePortResolver {
 public:
  virtual ~ICCLCControllerLifecyclePortResolver() = default;

  [[nodiscard]] virtual Result<std::shared_ptr<IControllerLifecyclePort>>
  ResolveExactControllerLifecyclePort(
      const ControllerLifecycleObservationQueryDTO& query) const = 0;
};

}  // namespace manyforge::core
