#pragma once

#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_deployment/cclc_runtime_composition.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <functional>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace manyforge::roboplan_backend {

/// A verified empty cell has no physical route publication surface. This
/// dependency refuses every physical-domain operation and fabricates no
/// participant or configuration readback. Recreate it with the real route
/// owner only after the original CCLC owner and callers have fully settled.
[[nodiscard]] core::Result<
    std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>>
MakeVerifiedEmptyCellRoutePublicationOwner(
    const deployment::RetainedBundle &retained_bundle);

/// Plain construction dependencies, owned by the existing product context.
/// The callback is consumed once after existing route recovery; it performs
/// no provider sampling, source staging, lifecycle action or publication.
struct RetainedNativeCCLCReadinessDependencies final {
  std::shared_ptr<const core::ConfiguredRobotMeasuredStateInventory> inventory;
  deployment::CCLCReadinessSourceCapture capture;
};
using RetainedNativeCCLCReadinessConstruction =
    std::function<RetainedNativeCCLCReadinessDependencies()>;

/// Product-owned policy for the retained native CCLC join.
struct RetainedNativeCCLCRuntimeConfig final {
  deployment::CCLCRuntimeCompositionConfig composition;
  core::Duration controller_observation_freshness_ns;
  /// Immutable complete geometry and pure current-source capture belong to
  /// this same product context, including successor preparation.
  std::shared_ptr<const core::ConfiguredRobotMeasuredStateInventory>
      measured_state_inventory{};
  deployment::CCLCReadinessSourceCapture capture_readiness_sources{};
  /// Empty keeps the direct inputs unchanged. A nonempty pure construction
  /// callback is consumed synchronously once and never retained by reads.
  RetainedNativeCCLCReadinessConstruction prepare_readiness_dependencies{};

  [[nodiscard]] bool locally_valid() const noexcept;
};

/// Derive the canonical state-authority projection shared by lifecycle-managed
/// and fixed direct native routes. Every resource is tied to the exact retained
/// control bindings that may publish its observations.
[[nodiscard]] core::Result<
    std::vector<core::KinematicResourceSourceProjectionDTO>>
DeriveRetainedNativeStateSourceProjections(
    std::span<const core::RuntimeParticipantBinding> physical_bindings);

/// Derive the same native Apply request before installation or outgoing-owner
/// retirement. Compiled generations are planned floors; retained readbacks are
/// exact current Kernel facts. This performs no I/O, records no plan, and
/// proves neither actual controller survival nor installed candidate readiness.
/// Ordinary Apply may supply the exact observed retained/removed owner sets;
/// these choose transient lifecycle effects, never alter the authored delta.
/// Both overrides must be present together and borrowed through this call.
[[nodiscard]] core::Result<core::NativeApplyPlanRequestDTO>
DeriveRetainedNativeApplyPlanRequest(
    const core::NamedApplyPublicationTransactionDTO &apply,
    const deployment::RetainedBundle &candidate,
    const deployment::RetainedBundle *predecessor,
    const deployment::ApplyComponentDeltaDTO &delta,
    std::span<const core::RuntimeParticipantBinding>
        candidate_physical_bindings,
    std::span<const core::RuntimeRouteConfigurationReadbackDTO>
        retained_route_readbacks,
    core::ComponentLifecycleStateDTO changed_component_state =
        core::ComponentLifecycleStateDTO::kActive,
    std::optional<std::span<const core::ControllableComponentId>>
        actual_retained_components = std::nullopt,
    std::optional<std::span<const core::ControllableComponentId>>
        actual_removed_components = std::nullopt);

/// Parent-Apply candidate initialization delegated to CCLC. The caller must
/// hold and verify the exact Manager fence around this call and pass the plan
/// CCLC recorded for this Apply before any route is published. Each route uses
/// the planned generation and component expectation; a component that keeps
/// an explicit Disable stays command-closed. This uses the same retained
/// native controller/Stop-readiness path as post-publication CCLC; it never
/// opens the parent fence or starts motion.
[[nodiscard]] core::Result<void> InitializeRetainedNativeCandidateControlRoutes(
    const deployment::RetainedBundle &bundle,
    core::ResourceAddressedExecutionKernel &kernel,
    deployment::IRouteConfigurationKernelPublicationOwner &route_owner,
    core::ICCLCComponentCommandReadinessPort &readiness_owner,
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

/// Prepare a product on the existing CCLC owner using the same verified native
/// runtime closure as CreateRetainedNativeCCLCRuntime. This neither creates nor
/// starts a CCLC owner, installs routes, exchanges contexts, or activates a
/// component. The publication must already be the exact current candidate.
/// The single installed-product pin must own every borrowed candidate source,
/// readiness port, candidate route publication owner, callback dependency and
/// installed projection/handle set.
/// The existing composition and its clock must outlive the prepared value.
[[nodiscard]] core::Result<std::shared_ptr<
    const deployment::CCLCRuntimeComposition::PreparedProductContext>>
PrepareRetainedNativeCCLCProductContext(
    deployment::CCLCRuntimeComposition &composition,
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    core::IClock &clock,
    deployment::IRouteConfigurationKernelPublicationOwner
        &kernel_publication_owner,
    std::shared_ptr<const void> installed_product_lifetime,
    std::function<
        core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
            const core::ControlDomainId &)>
        kernel_current_configuration_source = {},
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source = {},
    core::ICCLCComponentCommandReadinessPort *component_command_readiness_port = nullptr);

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
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    RetainedExternallyManagedDomainSnapshotSource
        externally_managed_controller_snapshot_source = {});

/// Restart variant for a stable native cell with any mix of component owner
/// kinds. It adopts durable predecessor routes under route-recovery exclusion,
/// reconciles command-free orphans, reconstructs component eligibility from
/// committed CCLC facts, and starts CCLC. The returned callback advances any
/// old-runtime-epoch route recovery; the host retains it until composition
/// detachment and never clears recovery admission before it succeeds.
[[nodiscard]] core::Result<std::unique_ptr<deployment::CCLCRuntimeComposition>>
CreateRecoveredRetainedNativeCellCCLCRuntime(
    RetainedNativeCCLCRuntimeConfig config,
    std::shared_ptr<const deployment::RetainedBundle> retained_bundle,
    const core::IActiveBundlePublicationReader &active_publication_reader,
    core::ActiveBundlePublicationReceiptDTO expected_active_publication,
    const deployment::IVerifiedRetainedProductArtifactSource &artifact_source,
    const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
    std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<core::RuntimeParticipantBinding> physical_bindings,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    deployment::IRouteConfigurationKernelPublicationOwner &route_owner,
    core::ICCLCComponentCommandReadinessPort &readiness_owner,
    core::ResourceAddressedExecutionKernel &kernel,
    core::IClock &clock, core::IScheduler &scheduler,
    std::function<core::Result<core::RuntimeRouteConfigurationReadbackDTO>(
        const core::ActiveControlConfigurationDTO &)> adopt_predecessor,
    std::function<core::Result<void>()> reconcile_command_free_orphans,
    std::function<core::Result<void>()> &reconcile_after_start);

} // namespace manyforge::roboplan_backend
