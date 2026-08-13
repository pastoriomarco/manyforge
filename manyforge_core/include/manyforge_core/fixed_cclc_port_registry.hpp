#pragma once

#include <manyforge_core/ports/icclc_brand_lifecycle_port_resolver.hpp>
#include <manyforge_core/ports/icclc_controller_lifecycle_port_resolver.hpp>
#include <memory>
#include <vector>

namespace manyforge::core {

struct BrandLifecyclePortRegistrationDTO final {
  ResolvedLifecycleProviderOccurrenceDTO provider_occurrence;
  std::shared_ptr<IBrandLifecyclePort> port;
};

struct ControllerLifecyclePortRegistrationDTO final {
  ControllerLifecycleObservationQueryDTO query;
  std::shared_ptr<IControllerLifecyclePort> port;
};

/// Immutable exact-key CCLC adapter registry.
///
/// Runtime composition constructs the complete table from retained/current
/// provider occurrences. Resolution performs no discovery, aliasing, latest
/// selection, adapter construction, or external call.
class FixedCCLCPortRegistry final : public ICCLCBrandLifecyclePortResolver,
                                    public ICCLCControllerLifecyclePortResolver {
 public:
  FixedCCLCPortRegistry(std::vector<BrandLifecyclePortRegistrationDTO> brand_ports,
                        std::vector<ControllerLifecyclePortRegistrationDTO> controller_ports);

  [[nodiscard]] Result<std::shared_ptr<IBrandLifecyclePort>> ResolveExactLifecyclePort(
      const ResolvedLifecycleProviderOccurrenceDTO& provider_occurrence) const override;

  [[nodiscard]] Result<std::shared_ptr<IControllerLifecyclePort>>
  ResolveExactControllerLifecyclePort(
      const ControllerLifecycleObservationQueryDTO& query) const override;

 private:
  std::vector<BrandLifecyclePortRegistrationDTO> brand_ports_;
  std::vector<ControllerLifecyclePortRegistrationDTO> controller_ports_;
};

}  // namespace manyforge::core
