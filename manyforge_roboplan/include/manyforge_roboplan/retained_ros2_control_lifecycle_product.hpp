#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::roboplan_backend {

inline constexpr std::string_view kRos2ControlLifecycleProviderManifestId =
    "manyforge.provider.ros2_control";

/// Closed observer-only lifecycle plans for the externally managed
/// ros2_control product. The plans can accept already-observed external truth,
/// but contain no primitive that could mutate controller_manager.
[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
retained_ros2_control_lifecycle_plans();

/// Construct and verify the catalog anchored by the exact ROS provider
/// manifest revision.
[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_retained_ros2_control_lifecycle_catalog(
    const core::ProviderManifestDTO &manifest);

/// Lifetime owner for the ROS provider catalog and observer-only brand ports
/// selected from one exact retained runtime.
class RetainedRos2ControlLifecycleProduct final {
public:
  [[nodiscard]] static core::Result<
      std::unique_ptr<RetainedRos2ControlLifecycleProduct>>
  Create(
      std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
      const deployment::IVerifiedRetainedProviderManifestSource
          &provider_manifest_source,
      const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
      std::shared_ptr<RetainedRos2ControlRuntimeHandle> runtime_handle,
      core::IClock &clock, core::Duration observation_freshness_ns);

  [[nodiscard]] const core::BuildOwnedLifecyclePlanCatalogSource &
  catalog_source() const noexcept;

  [[nodiscard]] const std::vector<core::BrandLifecyclePortRegistrationDTO> &
  brand_ports() const noexcept;

private:
  RetainedRos2ControlLifecycleProduct(
      core::BuildOwnedLifecyclePlanCatalogSource catalog_source,
      std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports);

  core::BuildOwnedLifecyclePlanCatalogSource catalog_source_;
  std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports_;
};

} // namespace manyforge::roboplan_backend
