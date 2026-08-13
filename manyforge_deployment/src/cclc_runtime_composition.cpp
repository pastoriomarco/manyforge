#include <manyforge_deployment/cclc_runtime_composition.hpp>

#include <exception>
#include <limits>
#include <string>
#include <utility>

namespace manyforge::deployment {
namespace {

[[nodiscard]] core::ErrorDTO composition_error(std::string reason,
                                               std::string message) {
  return {"cclc_runtime_composition_invalid",
          std::move(message),
          {{"reason", std::move(reason)}}};
}

} // namespace

bool CCLCRuntimeCompositionConfig::locally_valid() const noexcept {
  return !state_root.empty() && !resolved_stop_plan_root.empty() &&
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
             maximum_resolved_stop_plan_bytes;
}

core::Result<std::unique_ptr<CCLCRuntimeComposition>>
CCLCRuntimeComposition::CreateAndStart(
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
    core::ICCLCKernelDisableHandoffPort &kernel_disable_handoff_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler) {
  if (!config.locally_valid() || retained_bundle == nullptr ||
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
            kernel_disable_handoff_port, kernel_route_fence_port,
            kernel_publication_owner, clock, scheduler));
    auto started = result->coordinator_.Start();
    if (!started.ok()) {
      return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
          started.error());
    }
    auto http_server = CCLCOpenRequestUnixServer::CreateAndStart(
        result->config_.state_root, result->open_request_port_,
        core::RequestTransportDTO::kHttp);
    if (!http_server.ok()) {
      result->coordinator_.CloseAdmission();
      return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
          http_server.error());
    }
    result->http_open_request_server_ = std::move(http_server.value());
    auto ros_server = CCLCOpenRequestUnixServer::CreateAndStart(
        result->config_.state_root, result->open_request_port_,
        core::RequestTransportDTO::kRos);
    if (!ros_server.ok()) {
      result->coordinator_.CloseAdmission();
      return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
          ros_server.error());
    }
    result->ros_open_request_server_ = std::move(ros_server.value());
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
    core::ICCLCKernelDisableHandoffPort &kernel_disable_handoff_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler)
    : config_(std::move(config)), retained_bundle_(std::move(retained_bundle)),
      journal_(config_.state_root / std::string(core::kCCLCJournalFilename),
               config_.cell_id, config_.exact_result_history_capacity,
               config_.terminal_result_bytes_capacity),
      port_registry_(std::move(brand_ports), std::move(controller_ports)),
      plan_repository_(config_.resolved_stop_plan_root,
                       config_.maximum_resolved_stop_plan_count,
                       config_.maximum_resolved_stop_plan_bytes,
                       config_.maximum_single_resolved_stop_plan_bytes),
      lifecycle_snapshot_source_(*retained_bundle_, active_publication_reader,
                                 expected_active_publication, journal_,
                                 lifecycle_catalog_source,
                                 std::move(lifecycle_provider_occurrences)),
      route_snapshot_source_(*retained_bundle_, active_publication_reader,
                             expected_active_publication, journal_,
                             artifact_source, std::move(controller_sources)),
      route_publication_port_(*retained_bundle_, artifact_source,
                              plan_repository_, kernel_publication_owner,
                              std::move(source_projections),
                              [&clock] { return clock.now(); }),
      coordinator_(config_.coordinator, journal_, domain_reservations_,
                   lifecycle_snapshot_source_, lifecycle_catalog_source,
                   port_registry_, kernel_disable_handoff_port, clock,
                   scheduler,
                   core::ComponentLifecycleCoordinatorRouteServices{
                       &route_snapshot_source_, &port_registry_,
                       &kernel_route_fence_port, &route_publication_port_}),
      open_request_port_(coordinator_, lifecycle_snapshot_source_,
                         route_snapshot_source_) {}

CCLCRuntimeComposition::~CCLCRuntimeComposition() = default;

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

const std::filesystem::path &
CCLCRuntimeComposition::open_request_socket_path() const noexcept {
  return http_open_request_server_->socket_path();
}

const std::filesystem::path &
CCLCRuntimeComposition::ros_open_request_socket_path() const noexcept {
  return ros_open_request_server_->socket_path();
}

} // namespace manyforge::deployment
