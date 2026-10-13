#pragma once

#include <manyforge_core/ports/icclc_component_command_readiness_port.hpp>

#include <filesystem>
#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
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

using CCLCReadinessSourceCapture = std::function<
    core::Result<core::ComponentLifecycleReadinessSourceView>(
        const core::ComponentLifecycleCurrentSnapshotDTO &)>;

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
  /// CCLC-owned requested-target follower, started with the composition.
  core::CCLCLifecycleTargetDriverConfig lifecycle_target_driver{};
  // Optional informational logging only; invoked after journal commit/unlock.
  std::function<void(const core::ComponentIncidentPruneNotice &)>
      on_incidents_pruned{};
  /// Read-only exact early-preparation terminal classification; no authority.
  core::NativeApplyParentPreparationFailureSource preparation_failure_source{};

  [[nodiscard]] bool locally_valid() const noexcept;
  [[nodiscard]] bool trusted_roots_valid() const noexcept;
};

/// Complete product inputs for preparation on an already existing cell owner.
/// The single strong lifetime pin encloses every borrowed product dependency,
/// including artifacts/catalog/readiness and the full installed projection set.
/// Missing live occurrences are truthful inputs, not absent configured members.
struct CCLCProductContextInputs final {
  std::shared_ptr<const RetainedBundle> retained_bundle;
  const core::IActiveBundlePublicationReader &active_publication_reader;
  core::ActiveBundlePublicationReceiptDTO expected_publication;
  const IVerifiedRetainedProductArtifactSource &artifact_source;
  const core::ICCLCLifecyclePlanCatalogSource &lifecycle_catalog_source;
  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO>
      lifecycle_provider_occurrences;
  std::vector<ControllerLifecycleSourceOccurrenceDTO> controller_sources;
  std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports;
  std::vector<core::ControllerLifecyclePortRegistrationDTO> controller_ports;
  std::vector<core::KinematicResourceSourceProjectionDTO> source_projections;
  std::function<
      core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
          const core::ControlDomainId &)>
      current_configuration_source;
  core::ICCLCComponentCommandReadinessPort *readiness_port;
  IRouteConfigurationKernelPublicationOwner &kernel_publication_owner;
  std::shared_ptr<const void> installed_product_lifetime;
  std::shared_ptr<const core::ConfiguredRobotMeasuredStateInventory> measured_state_inventory{};
  CCLCReadinessSourceCapture capture_readiness_sources{};
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
  class ProductContext;

public:
  /// Origin-bound, immutable preparation; only this facade can construct it.
  /// It conveys lifetime and exact identity, never activation/release
  /// authority.
  class PreparedProductContext final {
  public:
    PreparedProductContext(const PreparedProductContext &) = delete;
    PreparedProductContext &operator=(const PreparedProductContext &) = delete;

  private:
    friend class CCLCRuntimeComposition;
    PreparedProductContext(
        const CCLCRuntimeComposition *origin, core::RequestOwnerEpoch owner,
        std::shared_ptr<const core::ComponentLifecycleProductContext> previous,
        std::shared_ptr<const ProductContext> product)
        : origin_(origin), owner_(std::move(owner)),
          previous_(std::move(previous)), product_(std::move(product)) {}
    const CCLCRuntimeComposition *const origin_;
    const core::RequestOwnerEpoch owner_;
    const std::shared_ptr<const core::ComponentLifecycleProductContext>
        previous_;
    const std::shared_ptr<const ProductContext> product_;
  };

  /// Existing actual-journal operations. Writes hold one product operation
  /// pin through commit; the original host still proves parent/fence/installed
  /// facts. The read remains available during pending product exchange.
  [[nodiscard]] core::Result<core::NativeApplyPlanDTO>
  PlanNativeApply(const core::NativeApplyPlanRequestDTO &request);
  [[nodiscard]] core::Result<std::optional<core::NativeApplyPlanDTO>>
  LoadNativeApplyPlan(const core::NamedApplyPublicationTransactionDTO &apply);
  [[nodiscard]] core::Result<void>
  AdoptNativeApplyFacts(const core::NativeApplyFactAdoptionDTO &adoption);

  /// Preparation never constructs a journal, owner or transport.
  /// The origin owner must outlive the prepared value and all its use.
  [[nodiscard]] core::Result<std::shared_ptr<const PreparedProductContext>>
  PrepareProductContext(CCLCProductContextInputs inputs);
  [[nodiscard]] core::Result<core::ComponentLifecycleProductExchangeObservation>
  TryExchangeProductContext(
      const PreparedProductContext &prepared,
      const core::RequestOwnerEpoch &expected_owner,
      const core::ActiveBundlePublicationReceiptDTO &expected_previous);
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
             *component_command_readiness_port = nullptr,
         std::shared_ptr<const core::ConfiguredRobotMeasuredStateInventory>
             measured_state_inventory = {},
         CCLCReadinessSourceCapture capture_readiness_sources = {});

  /// Starts effectful retained-transaction recovery and request transports.
  /// Construction alone permits owner-local observation, without admitting
  /// requests or resuming journaled effects. Call only with startup authority;
  /// a failed Start is terminal for this composition and requires destruction.
  [[nodiscard]] core::Result<void> Start();

  /// Join the follower and request servers while retaining the owner and dependencies.
  /// Call outside their threads; the host must separately drain managed callers and work.
  void StopRequestFollowingAndTransports() noexcept;

  /// Store checked observer exit evidence through this facade's existing journal.
  /// This grants no component lifecycle or containment authority.
  [[nodiscard]] core::Result<core::CellObserverIncidentRecordDTO>
  RecordCellObserverIncident(const core::CellObserverIncidentRecordDTO &record);

  /// Observe the first open record in the existing single cell observer slot.
  [[nodiscard]] core::Result<std::optional<core::CellObserverIncidentRecordDTO>>
  LoadOpenCellObserverIncident();
  /// The actual existing row association for explicit bring-up admission CAS.
  [[nodiscard]] core::Result<std::optional<core::CellObserverLossAssociationDTO>>
  LoadOpenCellObserverLossAssociation();

  [[nodiscard]] core::Result<std::optional<core::CellObserverBringUpRecordDTO>>
  LoadLatestCellObserverBringUp();
  [[nodiscard]] core::Result<core::CellObserverBringUpAdmissionDTO>
  BeginLatestCellObserverBringUp(const core::CellObserverBringUpRecordDTO& admitted,
      const std::optional<core::CellObserverBringUpRecordDTO>& expected_latest);
  [[nodiscard]] core::Result<core::CellObserverBringUpRecordDTO>
  UpdateLatestCellObserverBringUp(const core::CellObserverBringUpRecordDTO& expected,
      const core::CellObserverBringUpRecordDTO& updated);
  [[nodiscard]] core::Result<core::CellObserverBringUpRecordDTO>
  CloseCellObserverIncidentByOperatorBringUp(const core::CellObserverBringUpRecordDTO& expected_started,
      const core::CellObserverBringUpOpenDTO& first_open);

  /// Observe this genuine owner's Q97 component generation selection. The
  /// journal verifies its current owner and later acceptance repeats the CAS.
  [[nodiscard]] core::Result<core::ApplyComponentGenerationSelectionDTO>
  ReadApplyComponentGenerationSelection(
      const core::ControllableComponentId &component);

  /// Observe the existing journal's exact settled owner; grants no handoff authority.
  [[nodiscard]] core::Result<void>
  ReadQuiescentOwner(const core::RequestOwnerEpoch &expected);

  ~CCLCRuntimeComposition();

  CCLCRuntimeComposition(const CCLCRuntimeComposition &) = delete;
  CCLCRuntimeComposition &operator=(const CCLCRuntimeComposition &) = delete;

  [[nodiscard]] core::ComponentLifecycleCoordinator &coordinator() noexcept;
  [[nodiscard]] const core::ComponentLifecycleCoordinator &
  coordinator() const noexcept;
  [[nodiscard]] core::ICCLCOpenRequestPort &open_request_port() noexcept;
  /// Event-driven wakeups (observation publication, configuration change)
  /// are delivered through `Wake`; the driver never runs on the caller.
  [[nodiscard]] core::CCLCLifecycleTargetDriver &lifecycle_target_driver() noexcept;
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
      core::ICCLCComponentCommandReadinessPort *component_command_readiness_port,
      std::shared_ptr<const core::ConfiguredRobotMeasuredStateInventory>
          measured_state_inventory,
      CCLCReadinessSourceCapture capture_readiness_sources);

  CCLCRuntimeCompositionConfig config_;
  core::SqliteCCLCJournal journal_;
  core::CCLCDomainReservationTable domain_reservations_;
  std::unique_ptr<core::FilesystemResolvedStopBasePlanRepository>
      plan_repository_;
  // Stable cell-wide construction ports; product-specific referents belong
  // to the one immutable context and its owning installed-product pin.
  core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port_;
  core::IClock &clock_;
  // Constructor-only root. After coordinator aliasing ownership is installed,
  // this is reset; the coordinator has the sole current-context slot.
  std::shared_ptr<const ProductContext> initial_product_context_;
  core::ComponentLifecycleCoordinator coordinator_;
  core::CoordinatorLifecycleTargetDriverPorts target_driver_ports_;
  // Declared after the coordinator: destroyed (stopped) before it.
  core::CCLCLifecycleTargetDriver target_driver_;
  core::ComponentLifecycleCoordinatorOpenRequestPort open_request_port_;
  std::unique_ptr<CCLCOpenRequestUnixServer> http_open_request_server_;
  std::unique_ptr<CCLCOpenRequestUnixServer> ros_open_request_server_;
};

} // namespace manyforge::deployment
