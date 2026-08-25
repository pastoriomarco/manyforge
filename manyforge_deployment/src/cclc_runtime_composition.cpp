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
             maximum_resolved_stop_plan_bytes;
}

bool CCLCRuntimeCompositionConfig::trusted_roots_valid() const noexcept {
  return locally_valid() && trusted_existing_directory(state_root) &&
         trusted_existing_directory(run_root) &&
         disjoint_existing_directories(state_root, run_root);
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
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler,
    std::function<core::Result<
        std::optional<core::ActiveControlConfigurationDTO>>(
        const core::ControlDomainId &)>
        kernel_current_configuration_source) {
  if (!config.trusted_roots_valid() ||
      retained_bundle == nullptr ||
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
            kernel_disable_handoff_port, kernel_cell_admission_port,
            kernel_route_fence_port,
            kernel_publication_owner, clock, scheduler));
    auto started = result->coordinator_.Start();
    if (!started.ok()) {
      return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
          started.error());
    }
    auto http_server = CCLCOpenRequestUnixServer::CreateAndStart(
        result->config_.run_root, result->open_request_port_,
        core::RequestTransportDTO::kHttp);
    if (!http_server.ok()) {
      result->coordinator_.CloseAdmission();
      return core::Result<std::unique_ptr<CCLCRuntimeComposition>>::Err(
          http_server.error());
    }
    result->http_open_request_server_ = std::move(http_server.value());
    auto ros_server = CCLCOpenRequestUnixServer::CreateAndStart(
        result->config_.run_root, result->open_request_port_,
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
    std::function<core::Result<
        std::optional<core::ActiveControlConfigurationDTO>>(
        const core::ControlDomainId &)>
        kernel_current_configuration_source,
    core::ICCLCKernelDisableHandoffPort &kernel_disable_handoff_port,
    core::IKernelCellAdmissionPort &kernel_cell_admission_port,
    core::ICCLCKernelRouteConfigurationFencePort &kernel_route_fence_port,
    IRouteConfigurationKernelPublicationOwner &kernel_publication_owner,
    core::IClock &clock, core::IScheduler &scheduler)
    : config_(std::move(config)), retained_bundle_(std::move(retained_bundle)),
      journal_(config_.state_root / std::string(core::kCCLCJournalFilename),
               config_.cell_id, config_.exact_result_history_capacity,
               config_.terminal_result_bytes_capacity),
      port_registry_(std::move(brand_ports), std::move(controller_ports)),
      plan_repository_(config_.run_root,
                       config_.maximum_resolved_stop_plan_count,
                       config_.maximum_resolved_stop_plan_bytes,
                       config_.maximum_single_resolved_stop_plan_bytes),
      lifecycle_snapshot_source_(*retained_bundle_, active_publication_reader,
                                 expected_active_publication, journal_,
                                 lifecycle_catalog_source,
                                 std::move(lifecycle_provider_occurrences)),
      route_snapshot_source_(*retained_bundle_, active_publication_reader,
                             expected_active_publication, journal_,
                             artifact_source, std::move(controller_sources),
                             std::move(kernel_current_configuration_source)),
      route_publication_port_(*retained_bundle_, artifact_source,
                              plan_repository_, kernel_publication_owner,
                              std::move(source_projections),
                              [&clock] { return clock.now(); }),
      coordinator_(config_.coordinator, journal_, domain_reservations_,
                   lifecycle_snapshot_source_, lifecycle_catalog_source,
                   port_registry_, kernel_disable_handoff_port, clock,
                   scheduler, &kernel_cell_admission_port,
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

core::Result<core::ComponentLifecycleCurrentSnapshotDTO>
CCLCRuntimeComposition::ReadCurrentComponentSnapshot(
    const core::ControllableComponentId &controllable_component_id) const {
  return lifecycle_snapshot_source_.ReadCurrentComponentSnapshot(
      controllable_component_id);
}

} // namespace manyforge::deployment
