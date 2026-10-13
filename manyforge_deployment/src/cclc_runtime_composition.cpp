#include <manyforge_deployment/cclc_runtime_composition.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

namespace manyforge::deployment {
namespace {

[[nodiscard]] core::ErrorDTO composition_error(std::string reason,
                                               std::string message) {
  return {"cclc_runtime_composition_invalid",
          std::move(message),
          {{"reason", std::move(reason)}}};
}

[[nodiscard]] bool trusted_existing_directory(
    const std::filesystem::path &path) noexcept {
  if (!path.is_absolute() || path.lexically_normal() != path) {
    return false;
  }
  std::error_code error;
  if (std::filesystem::canonical(path, error) != path || error) {
    return false;
  }
  struct stat before {};
  if (::lstat(path.c_str(), &before) != 0 || !S_ISDIR(before.st_mode) ||
      S_ISLNK(before.st_mode) || before.st_uid != ::geteuid() ||
      (before.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (before.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) !=
          (S_IRUSR | S_IWUSR | S_IXUSR)) {
    return false;
  }
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  struct stat opened {};
  struct stat after {};
  const bool valid =
      ::fstat(descriptor, &opened) == 0 &&
      ::lstat(path.c_str(), &after) == 0 &&
      opened.st_dev == before.st_dev && opened.st_ino == before.st_ino &&
      after.st_dev == before.st_dev && after.st_ino == before.st_ino &&
      S_ISDIR(opened.st_mode) && S_ISDIR(after.st_mode) &&
      opened.st_uid == ::geteuid() && after.st_uid == ::geteuid() &&
      (opened.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
      (after.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
      (opened.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) ==
          (S_IRUSR | S_IWUSR | S_IXUSR) &&
      (after.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) ==
          (S_IRUSR | S_IWUSR | S_IXUSR);
  static_cast<void>(::close(descriptor));
  return valid;
}

[[nodiscard]] bool disjoint_existing_directories(
    const std::filesystem::path &state_root,
    const std::filesystem::path &run_root) noexcept {
  std::error_code error;
  const auto canonical_state = std::filesystem::canonical(state_root, error);
  if (error) {
    return false;
  }
  const auto canonical_run = std::filesystem::canonical(run_root, error);
  if (error) {
    return false;
  }
  const auto mismatch = std::mismatch(
      canonical_state.begin(), canonical_state.end(), canonical_run.begin(),
      canonical_run.end());
  return mismatch.first != canonical_state.end() &&
         mismatch.second != canonical_run.end();
}

} // namespace

class CCLCRuntimeComposition::ProductContext final {
public:
  ProductContext(
      std::shared_ptr<const RetainedBundle> bundle,
      const core::IActiveBundlePublicationReader &publication_reader,
      core::ActiveBundlePublicationReceiptDTO publication,
      core::SqliteCCLCJournal &journal,
      const IVerifiedRetainedProductArtifactSource &artifact_source,
      const core::ICCLCLifecyclePlanCatalogSource &catalog_source,
      std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers,
      std::vector<ControllerLifecycleSourceOccurrenceDTO> controllers,
      std::vector<core::BrandLifecyclePortRegistrationDTO> brand_ports,
      std::vector<core::ControllerLifecyclePortRegistrationDTO>
          controller_ports,
      std::vector<core::KinematicResourceSourceProjectionDTO>
          source_projections,
      std::function<
          core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
              const core::ControlDomainId &)>
          current_configuration_source,
      std::unique_ptr<core::FilesystemResolvedStopBasePlanRepository>
          &repository,
      const CCLCRuntimeCompositionConfig &config,
      core::ICCLCKernelRouteConfigurationFencePort &route_fence,
      IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
      core::IClock &clock, core::ICCLCComponentCommandReadinessPort *readiness,
      core::FilesystemResolvedStopBasePlanRepository *existing_repository =
          nullptr,
      std::shared_ptr<const void> installed_lifetime = {},
      std::shared_ptr<const core::ConfiguredRobotMeasuredStateInventory> measured_inventory = {})
      : installed_product_lifetime(std::move(installed_lifetime)),
        retained_bundle(std::move(bundle)),
        active_publication_reader(publication_reader),
        expected_publication(std::move(publication)),
        port_registry(std::move(brand_ports), std::move(controller_ports)),
        plan_repository(existing_repository != nullptr
                            ? *existing_repository
                            : InitializePlanRepository(repository, config)),
        lifecycle_snapshot_source(*retained_bundle, publication_reader,
                                  expected_publication, journal, catalog_source,
                                  std::move(providers)),
        route_snapshot_source(*retained_bundle, publication_reader,
                              expected_publication, journal, artifact_source,
                              std::move(controllers),
                              std::move(current_configuration_source)),
        route_publication_port(*retained_bundle, artifact_source,
                               plan_repository, kernel_publication_owner,
                               std::move(source_projections),
                               [&clock] { return clock.now(); }),
        services{lifecycle_snapshot_source,
                 catalog_source,
                 port_registry,
                 core::ComponentLifecycleCoordinatorRouteServices{
                     &route_snapshot_source, &port_registry, &route_fence,
                     &route_publication_port},
                 readiness,
                 ProductIdentity(*retained_bundle, expected_publication),
                 &route_snapshot_source,
                 std::move(measured_inventory)} {}

  // First member: all borrowed installed product sources/ports are destroyed
  // before this single owning lifetime pin is released.
  const std::shared_ptr<const void> installed_product_lifetime;
  const std::shared_ptr<const RetainedBundle> retained_bundle;
  const core::IActiveBundlePublicationReader &active_publication_reader;
  const core::ActiveBundlePublicationReceiptDTO expected_publication;
  core::FixedCCLCPortRegistry port_registry;
  core::FilesystemResolvedStopBasePlanRepository &plan_repository;
  VerifiedComponentLifecycleCurrentSnapshotSource lifecycle_snapshot_source;
  VerifiedRouteSelectionCurrentSnapshotSource route_snapshot_source;
  VerifiedRouteConfigurationPublicationPort route_publication_port;
  const core::ComponentLifecycleProductContext services;

private:
  static core::ComponentLifecycleProductIdentity
  ProductIdentity(const RetainedBundle &bundle,
                  const core::ActiveBundlePublicationReceiptDTO &publication) {
    core::ComponentLifecycleProductIdentity identity{publication, {}};
    for (const auto &component :
         bundle.value().cell_graph.controllable_components)
      identity.configured_components.push_back(
          component.controllable_component_id);
    return identity;
  }

  static core::FilesystemResolvedStopBasePlanRepository &
  InitializePlanRepository(
      std::unique_ptr<core::FilesystemResolvedStopBasePlanRepository>
          &repository,
      const CCLCRuntimeCompositionConfig &config) {
    // Preserve construction order: exact registry validation precedes any
    // repository filesystem work; the stable facade still owns the repository.
    repository =
        std::make_unique<core::FilesystemResolvedStopBasePlanRepository>(
            config.run_root, config.maximum_resolved_stop_plan_count,
            config.maximum_resolved_stop_plan_bytes,
            config.maximum_single_resolved_stop_plan_bytes);
    return *repository;
  }
};

bool CCLCRuntimeCompositionConfig::locally_valid() const noexcept {
  return !state_root.empty() && !run_root.empty() &&
         state_root != run_root &&
         !cell_id.empty() && coordinator.locally_valid() &&
         exact_result_history_capacity > 0U &&
         exact_result_history_capacity <=
             core::kMaximumCCLCExactResultHistory &&
         exact_result_history_capacity <=
             coordinator.request_membership_capacity &&
         terminal_result_bytes_capacity > 0U &&
         terminal_result_bytes_capacity <=
             core::kMaximumCCLCTerminalResultBytes &&
         maximum_resolved_stop_plan_count > 0U &&
         maximum_resolved_stop_plan_bytes > 0U &&
         maximum_single_resolved_stop_plan_bytes > 0U &&
         maximum_single_resolved_stop_plan_bytes <=
             core::kMaximumResolvedStopBasePlanEncodedBytes &&
         maximum_single_resolved_stop_plan_bytes <=
             maximum_resolved_stop_plan_bytes &&
         lifecycle_target_driver.locally_valid();
}

bool CCLCRuntimeCompositionConfig::trusted_roots_valid() const noexcept {
  return locally_valid() && trusted_existing_directory(state_root) &&
         trusted_existing_directory(run_root) &&
         disjoint_existing_directories(state_root, run_root);
}

core::Result<std::unique_ptr<CCLCRuntimeComposition>>
CCLCRuntimeComposition::Create(
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
    std::vector<core::ControllerLifecyclePortRegistrationDTO> controller_ports,
    std::vector<core::KinematicResourceSourceProjectionDTO> source_projections,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler,
    std::function<
        core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
            const core::ControlDomainId &)>
        kernel_current_configuration_source,
    core::ICCLCComponentCommandReadinessPort *component_command_readiness_port) {
  if (!config.trusted_roots_valid() || retained_bundle == nullptr ||
      !expected_active_publication.valid() ||
      expected_active_publication.request.candidate_retained_bundle_id !=
          retained_bundle->retained_bundle_id() ||
      expected_active_publication.request.candidate_graph_revision !=
          retained_bundle->value().cell_graph.graph_revision ||
      retained_bundle->value().cell_graph.cell_id != config.cell_id) {
    return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
        composition_error("construction_closure_invalid",
                          "CCLC composition inputs do not name one exact "
                          "active retained cell"));
  }

  try {
    auto result =
        std::unique_ptr<CCLCRuntimeComposition>(new CCLCRuntimeComposition(
            std::move(config), std::move(retained_bundle),
            active_publication_reader, std::move(expected_active_publication),
            artifact_source, lifecycle_catalog_source,
            std::move(lifecycle_provider_occurrences),
            std::move(controller_sources), std::move(brand_ports),
            std::move(controller_ports), std::move(source_projections),
            std::move(kernel_current_configuration_source),
            kernel_cell_admission_port,
            kernel_route_fence_port, kernel_publication_owner, clock,
            scheduler, component_command_readiness_port));
    return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Ok(
        std::move(result));
  } catch (const std::exception &exception) {
    return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
        composition_error("construction_failed", exception.what()));
  } catch (...) {
    return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
        composition_error("construction_failed",
                          "CCLC composition construction failed"));
  }
}

core::Result<void> CCLCRuntimeComposition::Start() {
  try {
    auto started = coordinator_.Start();
    if (!started.ok())
      return started;
    auto http_server = CCLCOpenRequestUnixServer::CreateAndStart(
        config_.run_root, open_request_port_, core::RequestTransportDTO::kHttp);
    if (!http_server.ok()) {
      coordinator_.CloseAdmission();
      return core::Result<void>::Err(http_server.error());
    }
    auto ros_server = CCLCOpenRequestUnixServer::CreateAndStart(
        config_.run_root, open_request_port_, core::RequestTransportDTO::kRos);
    if (!ros_server.ok()) {
      coordinator_.CloseAdmission();
      return core::Result<void>::Err(ros_server.error());
    }
    http_open_request_server_ = std::move(http_server.value());
    ros_open_request_server_ = std::move(ros_server.value());
    // Target following starts only after CCLC admission and transports are
    // open; it submits through the same coordinator as every caller.
    auto following = target_driver_.Start();
    if (!following.ok()) {
      coordinator_.CloseAdmission();
      return following;
    }
    return core::Result<void>::Ok();
  } catch (const std::exception &exception) {
    coordinator_.CloseAdmission();
    return core::Result<void>::Err(
        composition_error("startup_failed", exception.what()));
  } catch (...) {
    coordinator_.CloseAdmission();
    return core::Result<void>::Err(
        composition_error("startup_failed", "CCLC composition startup failed"));
  }
}

CCLCRuntimeComposition::CCLCRuntimeComposition(
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
    std::vector<core::ControllerLifecyclePortRegistrationDTO> controller_ports,
    std::vector<core::KinematicResourceSourceProjectionDTO> source_projections,
    std::function<
        core::Result<std::optional<core::ActiveControlConfigurationDTO>>(
            const core::ControlDomainId &)>
        kernel_current_configuration_source,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler,
    core::ICCLCComponentCommandReadinessPort *component_command_readiness_port)
    : config_(std::move(config)),
      journal_(config_.state_root / std::string(core::kCCLCJournalFilename),
               config_.cell_id, config_.exact_result_history_capacity,
               config_.terminal_result_bytes_capacity,
               config_.on_incidents_pruned, config_.preparation_failure_source),
      kernel_route_fence_port_(kernel_route_fence_port), clock_(clock),
      initial_product_context_(std::make_shared<ProductContext>(
          std::move(retained_bundle), active_publication_reader,
          std::move(expected_active_publication), journal_, artifact_source,
          lifecycle_catalog_source, std::move(lifecycle_provider_occurrences),
          std::move(controller_sources), std::move(brand_ports),
          std::move(controller_ports), std::move(source_projections),
          std::move(kernel_current_configuration_source), plan_repository_,
          config_, kernel_route_fence_port, kernel_publication_owner, clock,
          component_command_readiness_port)),
      coordinator_(
          config_.coordinator, journal_, domain_reservations_,
          std::shared_ptr<const core::ComponentLifecycleProductContext>(
              initial_product_context_, &initial_product_context_->services),
          kernel_cell_admission_port, clock, scheduler),
      target_driver_ports_(
          coordinator_, kernel_cell_admission_port,
          [this] { return journal_.LoadComponentsWithUnsettledLifecycle(); }),
      target_driver_(config_.lifecycle_target_driver, journal_,
                     target_driver_ports_, clock, scheduler),
      open_request_port_(coordinator_, &target_driver_) {
  initial_product_context_.reset();
}

core::Result<
    std::shared_ptr<const CCLCRuntimeComposition::PreparedProductContext>>
CCLCRuntimeComposition::PrepareProductContext(CCLCProductContextInputs inputs) {
  using PreparedResult =
      core::Result<std::shared_ptr<const PreparedProductContext>>;
  // Preparation owns no journal/store/owner and grants no activation. Missing
  // installed occurrences remain missing; constructor closure checks refuse
  // them instead of synthesizing catalogs or readiness.
  if (!inputs.retained_bundle || !inputs.installed_product_lifetime ||
      !inputs.expected_publication.valid() ||
      inputs.expected_publication.request.candidate_retained_bundle_id !=
          inputs.retained_bundle->retained_bundle_id() ||
      inputs.expected_publication.request.candidate_graph_revision !=
          inputs.retained_bundle->value().cell_graph.graph_revision ||
      inputs.retained_bundle->value().cell_graph.cell_id != config_.cell_id) {
    return PreparedResult::Err(composition_error(
        "prepared_product_closure_invalid",
        "prepared CCLC inputs do not name one owned retained cell"));
  }
  auto acquired = coordinator_.AcquireProductPin();
  if (!acquired.ok())
    return PreparedResult::Err(acquired.error());
  const auto previous = acquired.value().context_;
  const auto owner = coordinator_.request_owner_epoch();
  if (!previous->identity ||
      !inputs.expected_publication.request
           .expected_previous_publication_generation ||
      *inputs.expected_publication.request
              .expected_previous_publication_generation !=
          previous->identity->publication.publication_generation ||
      inputs.expected_publication.publication_generation.value() <=
          previous->identity->publication.publication_generation.value()) {
    return PreparedResult::Err(composition_error(
        "prepared_product_predecessor_mismatch",
        "prepared publication does not extend the current product"));
  }
  try {
    const auto current =
        inputs.active_publication_reader.ReadActiveBundlePublication(
            inputs.expected_publication.publication_generation);
    if (!current.ok())
      return PreparedResult::Err(current.error());
    if (current.value() != inputs.expected_publication)
      return PreparedResult::Err(composition_error(
          "prepared_product_publication_mismatch",
          "prepared product is not the exact current publication"));
    auto product = std::make_shared<ProductContext>(
        std::move(inputs.retained_bundle), inputs.active_publication_reader,
        std::move(inputs.expected_publication), journal_,
        inputs.artifact_source, inputs.lifecycle_catalog_source,
        std::move(inputs.lifecycle_provider_occurrences),
        std::move(inputs.controller_sources), std::move(inputs.brand_ports),
        std::move(inputs.controller_ports),
        std::move(inputs.source_projections),
        std::move(inputs.current_configuration_source), plan_repository_,
        config_, kernel_route_fence_port_, inputs.kernel_publication_owner,
        clock_, inputs.readiness_port, plan_repository_.get(),
        std::move(inputs.installed_product_lifetime),
        std::move(inputs.measured_state_inventory));
    // Recheck after construction/source qualification, outside owner locks.
    const auto after =
        product->active_publication_reader.ReadActiveBundlePublication(
            product->expected_publication.publication_generation);
    if (!after.ok())
      return PreparedResult::Err(after.error());
    if (after.value() != product->expected_publication ||
        coordinator_.request_owner_epoch() != owner)
      return PreparedResult::Err(
          composition_error("prepared_product_changed",
                            "publication or owner changed during preparation"));
    return PreparedResult::Ok(std::shared_ptr<const PreparedProductContext>(
        new PreparedProductContext(this, owner, previous, std::move(product))));
  } catch (const std::exception &) {
    return PreparedResult::Err(composition_error(
        "prepared_product_source_failed",
        "a prepared CCLC product source raised an exception"));
  }
}

core::Result<core::ComponentLifecycleProductExchangeObservation>
CCLCRuntimeComposition::TryExchangeProductContext(
    const PreparedProductContext &prepared,
    const core::RequestOwnerEpoch &expected_owner,
    const core::ActiveBundlePublicationReceiptDTO &expected_previous) {
  using ExchangeResult =
      core::Result<core::ComponentLifecycleProductExchangeObservation>;
  if (prepared.origin_ != this || !prepared.previous_ || !prepared.product_)
    return ExchangeResult::Err(
        composition_error("prepared_product_origin_foreign",
                          "preparation belongs to a different CCLC facade"));
  const auto successor =
      std::shared_ptr<const core::ComponentLifecycleProductContext>(
          prepared.product_, &prepared.product_->services);
  if (prepared.owner_ != expected_owner || !prepared.previous_->identity ||
      prepared.previous_->identity->publication != expected_previous) {
    // Withdraw only this prepared request. Never rebind a preparation from a
    // prior owner epoch to a newer caller-selected epoch.
    if (!prepared.previous_->identity)
      return ExchangeResult::Err(
          composition_error("prepared_product_predecessor_foreign",
                            "prepared predecessor identity is absent"));
    if (!coordinator_.WithdrawProductExchange(prepared.previous_, successor,
                                              prepared.owner_))
      return ExchangeResult::Ok(
          {core::ComponentLifecycleProductExchangePhase::kPending,
           prepared.owner_, prepared.previous_->identity->publication,
           prepared.previous_->identity->publication,
           "product_owner_exclusion_busy"});
    return ExchangeResult::Err(composition_error(
        "prepared_product_predecessor_foreign",
        "prepared owner or predecessor differs from the original capture"));
  }
  try {
    const auto before =
        prepared.product_->active_publication_reader
            .ReadActiveBundlePublication(
                prepared.product_->expected_publication.publication_generation);
    if (!before.ok() ||
        before.value() != prepared.product_->expected_publication) {
      coordinator_.CloseProductExchangeAdmission();
      return before.ok()
                 ? ExchangeResult::Err(composition_error(
                       "prepared_product_publication_changed",
                       "candidate is no longer the exact publication head"))
                 : ExchangeResult::Err(before.error());
    }
    auto exchanged = coordinator_.TryExchangeProductContext(
        prepared.previous_, successor, expected_owner, expected_previous);
    if (!exchanged.ok())
      return exchanged;
    const auto after =
        prepared.product_->active_publication_reader
            .ReadActiveBundlePublication(
                prepared.product_->expected_publication.publication_generation);
    if (!after.ok() ||
        after.value() != prepared.product_->expected_publication ||
        coordinator_.request_owner_epoch() != expected_owner) {
      // If the actual slot already changed, preserve that truth and stop
      // admission. Never fabricate rollback/adoption or release a parent.
      coordinator_.CloseProductExchangeAdmission();
      return after.ok() ? ExchangeResult::Err(composition_error(
                              "product_exchange_publication_unknown",
                              "publication or owner changed during exchange"))
                        : ExchangeResult::Err(after.error());
    }
    return exchanged;
  } catch (...) {
    coordinator_.CloseProductExchangeAdmission();
    return ExchangeResult::Err(composition_error(
        "product_exchange_source_unknown",
        "product exchange publication read raised an exception"));
  }
}

CCLCRuntimeComposition::~CCLCRuntimeComposition() {
  StopRequestFollowingAndTransports();
}

void CCLCRuntimeComposition::StopRequestFollowingAndTransports() noexcept {
  // Stop following before any transport or owner it submits through goes.
  target_driver_.Stop();
  ros_open_request_server_.reset();
  http_open_request_server_.reset();
}

core::Result<core::NativeApplyPlanDTO> CCLCRuntimeComposition::PlanNativeApply(
    const core::NativeApplyPlanRequestDTO &request) {
  auto pin = coordinator_.AcquireProductPin();
  if (!pin.ok())
    return core::Result<core::NativeApplyPlanDTO>::Err(pin.error());
  return journal_.PlanNativeApply(request);
}

core::Result<std::optional<core::NativeApplyPlanDTO>>
CCLCRuntimeComposition::LoadNativeApplyPlan(
    const core::NamedApplyPublicationTransactionDTO &apply) {
  return journal_.LoadNativeApplyPlan(apply);
}

core::Result<void> CCLCRuntimeComposition::AdoptNativeApplyFacts(
    const core::NativeApplyFactAdoptionDTO &adoption) {
  auto pin = coordinator_.AcquireProductPin();
  if (!pin.ok())
    return core::Result<void>::Err(pin.error());
  return journal_.AdoptNativeApplyFacts(adoption);
}

core::Result<core::CellObserverIncidentRecordDTO>
CCLCRuntimeComposition::RecordCellObserverIncident(
    const core::CellObserverIncidentRecordDTO &record) {
  return journal_.RecordCellObserverIncident(record);
}

core::Result<std::optional<core::CellObserverIncidentRecordDTO>>
CCLCRuntimeComposition::LoadOpenCellObserverIncident() {
  return journal_.LoadOpenCellObserverIncident();
}

core::Result<core::ApplyComponentGenerationSelectionDTO>
CCLCRuntimeComposition::ReadApplyComponentGenerationSelection(
    const core::ControllableComponentId &component) {
  std::optional<core::ApplyComponentQualifiedPredecessorDTO> predecessor;
  auto pin = coordinator_.AcquireProductPin();
  if (!pin.ok())
    return core::Result<core::ApplyComponentGenerationSelectionDTO>::Err(
        pin.error());
  const auto &context = pin.value().context();
  if (!context.identity)
    return core::Result<core::ApplyComponentGenerationSelectionDTO>::Err(
        {"cclc_product_identity_unavailable",
         "current configured membership is unavailable",
         {}});
  const auto &components = context.identity->configured_components;
  const auto count =
      std::count(components.begin(), components.end(), component);
  if (count > 1) {
    return core::Result<core::ApplyComponentGenerationSelectionDTO>::Err(
        {"component_lifecycle_snapshot_invalid",
         "the retained predecessor component is ambiguous", {}});
  }
  if (count == 1) {
    const auto current =
        context.current_snapshot_source.ReadCurrentComponentSnapshot(component);
    if (!current.ok())
      return core::Result<core::ApplyComponentGenerationSelectionDTO>::Err(current.error());
    predecessor = core::ApplyComponentQualifiedPredecessorDTO{
        current.value().component_lifecycle_generation, current.value().snapshot_digest};
  }
  return journal_.ReadApplyComponentGenerationSelection(
      coordinator_.request_owner_epoch(), component, predecessor);
}

core::Result<void> CCLCRuntimeComposition::ReadQuiescentOwner(
    const core::RequestOwnerEpoch &expected) {
  return journal_.ReadQuiescentOwner(expected);
}

core::ComponentLifecycleCoordinator &
CCLCRuntimeComposition::coordinator() noexcept {
  return coordinator_;
}

const core::ComponentLifecycleCoordinator &
CCLCRuntimeComposition::coordinator() const noexcept {
  return coordinator_;
}

core::ICCLCOpenRequestPort &
CCLCRuntimeComposition::open_request_port() noexcept {
  return open_request_port_;
}

core::CCLCLifecycleTargetDriver &
CCLCRuntimeComposition::lifecycle_target_driver() noexcept {
  return target_driver_;
}

const std::filesystem::path &
CCLCRuntimeComposition::open_request_socket_path() const {
  if (!http_open_request_server_)
    throw std::logic_error("CCLC request transport is not started");
  return http_open_request_server_->socket_path();
}

const std::filesystem::path &
CCLCRuntimeComposition::ros_open_request_socket_path() const {
  if (!ros_open_request_server_)
    throw std::logic_error("CCLC ROS request transport is not started");
  return ros_open_request_server_->socket_path();
}

core::Result<core::ComponentLifecycleCurrentSnapshotDTO>
CCLCRuntimeComposition::ReadCurrentComponentSnapshot(
    const core::ControllableComponentId &controllable_component_id) const {
  auto pin = coordinator_.AcquireProductPin();
  if (!pin.ok())
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
        pin.error());
  return pin.value()
      .context()
      .current_snapshot_source.ReadCurrentComponentSnapshot(
          controllable_component_id);
}

} // namespace manyforge::deployment
