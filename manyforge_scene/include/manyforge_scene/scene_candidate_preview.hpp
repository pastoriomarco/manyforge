#pragma once

#include <functional>
#include <memory>
#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::scene {

using CandidatePolicyProjector = std::function<core::Result<core::CollisionPolicySnapshot>(
    const core::LogicalSceneSnapshotDTO &)>;

/// Pure speculative attach/detach reduction. It has no live-Scene equality,
/// claim, replay or commit effect; actual Commit retains those checks.
[[nodiscard]] core::Result<core::SceneCandidatePreviewDTO>
BuildResolvedSceneCandidatePreview(
    const core::ResolvedSceneCandidatePreviewRequestDTO &request,
    const core::ProjectionRevisionDTO &predecessor,
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> snapshot,
    std::uint64_t transaction_sequence,
    const CandidatePolicyProjector &project_policy);

}  // namespace manyforge::scene
