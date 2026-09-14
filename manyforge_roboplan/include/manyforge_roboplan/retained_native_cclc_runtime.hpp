#pragma once

#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_deployment/cclc_runtime_composition.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <functional>
#include <memory>
#include <vector>

namespace manyforge::roboplan_backend {

/// Product-owned policy for the retained native CCLC join.
struct RetainedNativeCCLCRuntimeConfig final {
  deployment::CCLCRuntimeCompositionConfig composition;
  core::Duration controller_observation_freshness_ns;

  [[nodiscard]] bool locally_valid() const noexcept;
};

/// Construct the CCLC owner and verified observation sources without starting
/// workers or request admission. The caller explicitly invokes Start.
///
/// Brand registrations remain product-specific: their exact provider
/// occurrences and adapters are injected by the selected product (for
/// example, the Lite6 xArm composition). Controller registrations and
/// kinematic source projections are derived exclusively from the sealed
/// retained bundle and the current physical participant bindings. The active
/// publication reader, artifact/catalog sources, Kernel ports, native runtime,
/// clock, and scheduler must outlive the returned composition.
[[nodiscard]] core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>
CreateRetainedNativeCCLCRuntime(
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    core::ICCLCKernelDisableHandoffPort &kernel_disable_handoff_port,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    deployment::IRouteConfigurationKernelPublicationOwner
        &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler,
    std::function<
        core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
            const core::ControlDomainId &)>
        kernel_current_configuration_source = {},
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source = {});

/// Convenience overload for the production retained-native owner. Only the
/// physical representatives are consumed; Program aliases cannot widen CCLC
/// domain or resource scope. Startup adopts and reconciles routes through the
/// same owner even with a retained Stop fault. Private recovery can publish
/// verified current truth but cannot change controllers; Kernel keeps ordinary
/// admission closed until the fault is explicitly cleared.
[[nodiscard]] core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>
CreateRetainedNativeCCLCRuntime(
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    RetainedNativeRuntime &runtime,
    core::ICCLCKernelDisableHandoffPort &kernel_disable_handoff_port,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source = {});

} // namespace manyforge::roboplan_backend
