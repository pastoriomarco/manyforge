#pragma once

#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <optional>
#include <string>

namespace manyforge::roboplan_backend::host_detail {

struct ProgramResetPositionCapability final {
  bool available{};
  std::optional<std::string> reason;
};

/// Deployment identity pinned before the first selected Scene effect. The
/// Scene revision deliberately advances independently when Scene reload runs.
struct ProgramResetPositionRequest final {
  core::ProgramResetPositionAuthorizationDTO authorization;
  core::RequestOwnerEpoch owner_epoch;
  core::ActiveBundlePublicationGeneration active_publication_generation;
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
};

}  // namespace manyforge::roboplan_backend::host_detail
