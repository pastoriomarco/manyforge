#include "logical_scene_owner_impl.hpp"

namespace manyforge::scene {

std::string_view
ToString(const CanonicalEmptySceneBootstrapPhase phase) noexcept {
  switch (phase) {
  case CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced:
    return "initial_command_fenced";
  case CanonicalEmptySceneBootstrapPhase::kAccepted:
    return "accepted";
  case CanonicalEmptySceneBootstrapPhase::kReadyToPublish:
    return "ready_to_publish";
  case CanonicalEmptySceneBootstrapPhase::kFinalized:
    return "finalized";
  case CanonicalEmptySceneBootstrapPhase::kMaintenanceInitializing:
    return "maintenance_initializing";
  case CanonicalEmptySceneBootstrapPhase::kMaintenanceCompleted:
    return "maintenance_completed";
  }
  return {};
}

bool CanonicalEmptySceneAccessStatus::valid() const noexcept {
  switch (phase) {
  case CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced:
  case CanonicalEmptySceneBootstrapPhase::kAccepted:
  case CanonicalEmptySceneBootstrapPhase::kReadyToPublish:
    return !scene_writes_open && !projection_leases_open &&
           !active_projection.has_value();
  case CanonicalEmptySceneBootstrapPhase::kFinalized:
    return scene_writes_open == projection_leases_open &&
           active_projection.has_value() && active_projection->valid();
  case CanonicalEmptySceneBootstrapPhase::kMaintenanceInitializing:
    return !scene_writes_open && !projection_leases_open &&
           active_projection.has_value() && active_projection->valid();
  case CanonicalEmptySceneBootstrapPhase::kMaintenanceCompleted:
    return scene_writes_open == projection_leases_open &&
           active_projection.has_value() && active_projection->valid();
  }
  return false;
}

LogicalSceneOwner::LogicalSceneOwner(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader)
    : impl_(std::make_unique<Impl>(admission, publication_reader)) {}

LogicalSceneOwner::LogicalSceneOwner(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    core::IKernelCellAdmissionPort &cell_admission)
    : impl_(std::make_unique<Impl>(admission, publication_reader,
                                   cell_admission)) {}

LogicalSceneOwner::LogicalSceneOwner(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source)
    : impl_(std::make_unique<Impl>(admission, publication_reader,
                                   retained_bundle_source)) {}

LogicalSceneOwner::LogicalSceneOwner(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const core::IPlannerSceneProjectionResolver &projection_resolver)
    : impl_(std::make_unique<Impl>(admission, publication_reader,
                                   retained_bundle_source,
                                   projection_resolver)) {}

LogicalSceneOwner::LogicalSceneOwner(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const core::IActiveBundlePublicationReader &publication_reader,
    const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
    const core::IPlannerSceneProjectionResolver &projection_resolver,
    core::IKernelCellAdmissionPort &cell_admission)
    : impl_(std::make_unique<Impl>(admission, publication_reader,
                                   retained_bundle_source, projection_resolver,
                                   cell_admission)) {}

LogicalSceneOwner::~LogicalSceneOwner() = default;

LogicalSceneOwner::LogicalSceneOwner(
    LogicalSceneOwner &&) noexcept = default;

LogicalSceneOwner &LogicalSceneOwner::operator=(
    LogicalSceneOwner &&) noexcept = default;

const ProjectionBootstrapPrerequisiteQueryDTO &
LogicalSceneOwner::fixed_query() const noexcept {
  return impl_->fixed_query_;
}

} // namespace manyforge::scene
