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
namespace host_detail {
class IoProviderDriverRecord;
}

inline constexpr std::string_view kRos2ControlLifecycleProviderManifestId =
    "manyforge.provider.ros2_control";

/// Closed lifecycle plans of the ros2_control product (design R6, §7.5): the
/// trajectory controller is loaded inactive by its spawner and CCLC owns its
/// activation. Primitives `ros2_control.controller.{prepare,activate,
/// deactivate,recover}` reach the runtime handle; postconditions are proven
/// only by the controller manager's own activity facts.
[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
retained_ros2_control_lifecycle_plans();

/// Construct and verify the catalog anchored by the exact ROS provider
/// manifest revision.
[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_retained_ros2_control_lifecycle_catalog(
    const core::ProviderManifestDTO &manifest);

/// The Real gripper plans, for the gripper domain of a Real component (its
/// vendor family's gripper provider, `VendorAdapterFamily`): the shared plans
/// with Recover targeting `disabled`, the Real arm's Recover target (nothing
/// to clear on a healthy gripper).
[[nodiscard]] std::vector<core::LifecycleIntentPlanDTO>
retained_ros2_control_gripper_lifecycle_plans();

/// The catalog of one physical ROS provider (R7 package 4, V-A): a registered
/// vendor family's Real arm provider has the family's own plans, its gripper
/// provider the Real gripper plans above (`find_vendor_lifecycle_provider`);
/// every other physical provider uses the shared ros2_control catalog above.
/// Each is anchored by the exact manifest.
[[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
make_retained_physical_ros2_control_lifecycle_catalog(
    const core::ProviderManifestDTO &manifest);

/// Existing immutable host catalog/brand-port collection. The historical
/// source/CMake path stays fixed while actual generic driver records join the
/// same collection as the physical ROS controller providers, each with
/// `make_retained_physical_ros2_control_lifecycle_catalog`. Each physical
/// port answers admission from its own catalog and runs its transitions at
/// `poll_period_ns` (V-B2: the CCLC observation poll period).
class RetainedHostLifecycleProduct final {
public:
  [[nodiscard]] static core::Result<
      std::unique_ptr<RetainedHostLifecycleProduct>>
  Create(std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
         const deployment::IVerifiedRetainedProviderManifestSource
             &provider_manifest_source,
         const std::vector<core::RuntimeParticipantBinding> &physical_bindings,
         const std::vector<std::shared_ptr<RetainedRos2ControlRuntimeHandle>>
             &runtime_handles,
         core::IClock &clock, core::Duration observation_freshness_ns,
         core::Duration poll_period_ns,
         const std::vector<std::shared_ptr<host_detail::IoProviderDriverRecord>>
             &provider_drivers = {});

  [[nodiscard]] RetainedExternallyManagedDomainSnapshotSource
  lifecycle_snapshot_source() const;

  [[nodiscard]] const core::BuildOwnedLifecyclePlanCatalogSource &
  catalog_source() const noexcept;

  [[nodiscard]] const std::vector<core::BrandLifecyclePortRegistrationDTO> &
  brand_ports() const noexcept;

private:
  RetainedHostLifecycleProduct(
      core::BuildOwnedLifecyclePlanCatalogSource catalog_source,
      std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
      RetainedExternallyManagedDomainSnapshotSource snapshot_source);

  RetainedExternallyManagedDomainSnapshotSource snapshot_source_;
  core::BuildOwnedLifecyclePlanCatalogSource catalog_source_;
  std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports_;
};

} // namespace manyforge::roboplan_backend
