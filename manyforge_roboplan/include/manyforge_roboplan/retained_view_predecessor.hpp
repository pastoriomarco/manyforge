#pragma once

#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::roboplan_backend {

/// Whether a retained native view (same bundle, every component retained)
/// may replace the active composition's publication view.
///
/// The active composition carries the projection revision it was published
/// at. A run's attach, detach and reset advance the live Scene revision after
/// that without changing the Deployment, and the Scene owner alone proves the
/// exact Scene predecessor. The view therefore follows its predecessor when
/// the active composition, the request predecessor and the retained view all
/// name the same active Deployment.
[[nodiscard]] inline bool RetainedViewFollowsPredecessor(
    const core::RetainedBundleId &active_bundle_id,
    const core::ProjectionRevisionDTO &active_projection_revision,
    const core::RetainedBundleId &retained_view_bundle_id,
    const core::ProjectionRevisionDTO &request_predecessor) noexcept {
  return active_bundle_id == request_predecessor.retained_bundle_id &&
         core::SameActiveDeployment(active_projection_revision,
                                    request_predecessor) &&
         retained_view_bundle_id == active_bundle_id;
}

} // namespace manyforge::roboplan_backend
