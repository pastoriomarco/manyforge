#pragma once

#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <memory>

namespace manyforge::core {

/// Exact occurrence-to-adapter resolver used by the generic CCLC owner.
///
/// The returned shared ownership freezes one adapter incarnation across an
/// Observe/Dispatch/reconciliation sequence. Resolution performs no lifecycle
/// effect and never selects a latest provider by manifest name alone.
class ICCLCBrandLifecyclePortResolver {
 public:
  virtual ~ICCLCBrandLifecyclePortResolver() = default;

  [[nodiscard]] virtual Result<std::shared_ptr<IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const ResolvedLifecycleProviderOccurrenceDTO& provider_occurrence) const = 0;

  /// Read only the product-owned pure projection for the complete occurrence.
  /// Absence is unavailable; never fall back to Observe or select a latest
  /// provider. The default preserves unavailable behavior for registrations
  /// whose producer has no qualified pure projection.
  [[nodiscard]] virtual Result<LifecycleDomainHealthDTO> ReadExactLifecycleHealth(
      const ResolvedLifecycleProviderOccurrenceDTO&, std::int64_t) const {
    return Result<LifecycleDomainHealthDTO>::Err(
        ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                 "no pure lifecycle health projection is registered",
                 {{"reason", "lifecycle_health_projection_missing"}}});
  }
};

}  // namespace manyforge::core
