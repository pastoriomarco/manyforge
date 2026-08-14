#pragma once

#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <vector>

namespace manyforge::roboplan_backend {

/// Lifetime owner for the exact product lifecycle catalogs and brand ports
/// selected from one verified retained native bundle.
class RetainedNativeLifecycleProduct final {
public:
  [[nodiscard]] static core::Result<
      std::unique_ptr<RetainedNativeLifecycleProduct>>
  Create(
      std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
      const deployment::IVerifiedRetainedProviderManifestSource
          &provider_manifest_source,
      const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
      core::IClock &clock, core::Duration observation_freshness_ns);

  [[nodiscard]] const core::BuildOwnedLifecyclePlanCatalogSource &
  catalog_source() const noexcept;

  [[nodiscard]] const std::vector<core::BrandLifecyclePortRegistrationDTO> &
  brand_ports() const noexcept;

private:
  RetainedNativeLifecycleProduct(
      core::BuildOwnedLifecyclePlanCatalogSource catalog_source,
      std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports);

  core::BuildOwnedLifecyclePlanCatalogSource catalog_source_;
  std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports_;
};

} // namespace manyforge::roboplan_backend

