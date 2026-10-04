#pragma once

#include <manyforge_core/ports/icclc_component_command_readiness_port.hpp>

#include <filesystem>
#include <manyforge_core/cclc_open_request.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/filesystem_resolved_stop_base_plan_repository.hpp>
#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_deployment/cclc_open_request_server.hpp>
#include <manyforge_deployment/component_lifecycle_current_snapshot_source.hpp>
#include <manyforge_deployment/route_configuration_publication.hpp>
#include <manyforge_deployment/route_selection_current_snapshot_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <functional>
#include <memory>
#include <vector>

namespace manyforge::deployment {

/// Stable construction policy for one production CCLC owner epoch.
///
/// The durable state and disposable run roots are trusted, pre-existing
/// deployment-owned directories. Product-specific adapter instances and
/// build-owned lifecycle catalogs are injected explicitly; this composition
/// performs no discovery, default selection, or adapter construction.
struct CCLCRuntimeCompositionConfig final {
  std::filesystem::path state_root;
  std::filesystem::path run_root;
  core::CellId cell_id;
  core::ComponentLifecycleCoordinatorConfig coordinator;
  std::size_t exact_result_history_capacity{
      core::kDefaultCCLCExactResultHistory};
  std::size_t terminal_result_bytes_capacity{
      core::kDefaultCCLCTerminalResultBytes};
  std::size_t maximum_resolved_stop_plan_count{
      core::kDefaultResolvedStopBasePlanRepositoryCount};
  std::uint64_t maximum_resolved_stop_plan_bytes{
      core::kDefaultResolvedStopBasePlanRepositoryBytes};
  std::uint64_t maximum_single_resolved_stop_plan_bytes{
      core::kMaximumResolvedStopBasePlanEncodedBytes};

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool trusted_roots_valid() const noexcept;
};

/// Complete L0.4 production lifetime join for one exact retained bundle.
///
/// The object owns the CCLC journal, shared reservation table, immutable port
/// registry, resolved-plan repository, verified current sources, route
/// publication adapter, and sole ComponentLifecycleCoordinator. The supplied
/// active-publication reader, artifact source, lifecycle catalog source,
/// Kernel ports, clock, scheduler, and Kernel publication owner must outlive
/// this object. Destruction stops/drains the coordinator before releasing any
/// owned dependency.
class CCLCRuntimeComposition final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<CCLCRuntimeComposition>>
  Create(CCLCRuntimeCompositionConfig config,
         std::shared_ptr<const RetainedBundle> retained_bundle,
         const core::IActiveBundlePublicationReader &active_publication_reader,
         core::ActiveBundlePublicationReceiptDTO expected_active_publication,
         const IVerifiedRetainedProductArtifactSource &artifact_source,
         const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
         std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>
             lifecycle_provider_occurrences,
         std::vector<ControllerLifecycleSourceOccurrenceDTO> controller_sources,
         std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
         std::vector<core::ControllerLifecyclePortRegistrationDTO>
             controller_ports,
         std::vector<core::KinematicResourceSourceProjectionDTO>
             source_projections,
         core::IKernelCellAdmissionPort &kernel_cell_admission_port,
         core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
         IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
         core::IClock &clock, core::IScheduler &scheduler,
         std::function<
             core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
                 const core::ControlDomainId &)>
             kernel_current_configuration_source = {},
         core::ICCLCComponentCommandReadinessPort
             *component_command_readiness_port = nullptr);

  /// Starts effectful retained-transaction recovery and request transports.
  /// Construction alone permits owner-local observation, without admitting
  /// requests or resuming journaled effects. Call only with startup authority;
  /// a failed Start is terminal for this composition and requires destruction.
  [[nodiscard]] core::Result<void> Start();

  ~CCLCRuntimeComposition();

  CCLCRuntimeComposition(const CCLCRuntimeComposition &) = delete;
  CCLCRuntimeComposition &operator=(const CCLCRuntimeComposition &) = delete;

  [[nodiscard]] core::ComponentLifecycleCoordinator &coordinator() noexcept;
  [[nodiscard]] const core::ComponentLifecycleCoordinator &
  coordinator() const noexcept;
  [[nodiscard]] core::ICCLCOpenRequestPort &open_request_port() noexcept;
  [[nodiscard]] const std::filesystem::path &open_request_socket_path() const;
  [[nodiscard]] const std::filesystem::path &
  ros_open_request_socket_path() const;
  [[nodiscard]] core::Result<core::ComponentLifecycleCurrentSnapshotDTO>
  ReadCurrentComponentSnapshot(
      const core::ControllableComponentId &controllable_component_id) const;

private:
  CCLCRuntimeComposition(
      CCLCRuntimeCompositionConfig config,
      std::shared_ptr<const RetainedBundle> retained_bundle,
      const core::IActiveBundlePublicationReader &active_publication_reader,
      core::ActiveBundlePublicationReceiptDTO expected_active_publication,
      const IVerifiedRetainedProductArtifactSource &artifact_source,
      const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source,
      std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>
          lifecycle_provider_occurrences,
      std::vector<ControllerLifecycleSourceOccurrenceDTO> controller_sources,
      std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
      std::vector<core::ControllerLifecyclePortRegistrationDTO>
          controller_ports,
      std::vector<core::KinematicResourceSourceProjectionDTO>
          source_projections,
      std::function<
          core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
              const core::ControlDomainId &)>
          kernel_current_configuration_source,
      core::IKernelCellAdmissionPort &kernel_cell_admission_port,
      core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
      IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
      core::IClock &clock, core::IScheduler &scheduler,
      core::ICCLCComponentCommandReadinessPort *component_command_readiness_port);

  CCLCRuntimeCompositionConfig config_;
  std::shared_ptr<const RetainedBundle> retained_bundle_;
  core::SqliteCCLCJournal journal_;
  core::CCLCDomainReservationTable domain_reservations_;
  core::FixedCCLCPortRegistry port_registry_;
  core::FilesystemResolvedStopBasePlanRepository plan_repository_;
  VerifiedComponentLifecycleCurrentSnapshotSource lifecycle_snapshot_source_;
  VerifiedRouteSelectionCurrentSnapshotSource route_snapshot_source_;
  VerifiedRouteConfigurationPublicationPort route_publication_port_;
  core::ComponentLifecycleCoordinator coordinator_;
  core::ComponentLifecycleCoordinatorOpenRequestPort open_request_port_;
  std::unique_ptr<CCLCOpenRequestUnixServer> http_open_request_server_;
  std::unique_ptr<CCLCOpenRequestUnixServer> ros_open_request_server_;
};

} // namespace manyforge::deployment
