#pragma once

#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_deployment/cclc_runtime_composition.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <functional>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace manyforge::roboplan_backend {

/// Product-owned policy for the retained native CCLC join.
struct RetainedNativeCCLCRuntimeConfig final {
  deployment::CCLCRuntimeCompositionConfig composition;
  core::Duration controller_observation_freshness_ns;

  [[nodiscard]] bool locally_valid() const noexcept;
};

/// Derive the canonical state-authority projection shared by lifecycle-managed
/// and fixed direct native routes. Every resource is tied to the exact retained
/// control bindings that may publish its observations.
[[nodiscard]] core::Result<
    std::vector<core::KinematicResourceSourceProjectionDTO>>
DeriveRetainedNativeStateSourceProjections(
    std::span<const core::RuntimeParticipantBinding> physical_bindings);

/// Parent-Apply candidate initialization delegated to CCLC. The caller must
/// hold and verify the exact Manager fence around this call and pass the plan
/// CCLC recorded for this Apply before any route is published. Each route uses
/// the planned generation and component expectation; a component that keeps
/// an explicit Disable stays command-closed. This uses the same retained
/// native controller/Stop-readiness path as post-publication CCLC; it never
/// opens the parent fence or starts motion.
[[nodiscard]] core::Result<void> InitializeRetainedNativeCandidateControlRoutes(
    const deployment::RetainedBundle &bundle, RetainedNativeRuntime &runtime,
    core::ActiveBundlePublicationGeneration candidate_publication_generation,
    const core::NativeApplyPlanDTO &plan,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    std::span<const core::RuntimeParticipantBinding> physical_bindings,
    const std::filesystem::path &execution_run_root,
    const core::ConfigurationFenceId &parent_apply_fence_id);

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
        externally_managed_controller_snapshot_source = {},
    core::ICCLCComponentCommandReadinessPort *component_command_readiness_port =
        nullptr);

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
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    RetainedNativeRuntime &runtime,
    core::ICCLCKernelDisableHandoffPort &kernel_disable_handoff_port,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source = {});

} // namespace manyforge::roboplan_backend
