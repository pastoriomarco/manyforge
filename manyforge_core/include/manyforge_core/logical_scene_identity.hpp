#pragma once

#include <stdexcept>

#include <manyforge_core/types/logical_scene_snapshot.hpp>

namespace manyforge::core::logical_scene_identity {

class LogicalSceneIdentityError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Mints the deterministic lineage identity from one accepted bootstrap input.
[[nodiscard]] SceneId MintSceneId(
    const LogicalSceneBootstrapIdentityInputDTO& input);

/// Computes the policy's semantic content digest, excluding its complete
/// CollisionPolicyRevision field.
[[nodiscard]] ContentDigest ComputeCollisionPolicyContentDigest(
    const CollisionPolicySnapshot& normalized_policy);

/// Computes the content revision of one carried attachment, excluding only
/// its own CarriedObjectAttachmentRevision field.
[[nodiscard]] CarriedObjectAttachmentRevision
ComputeCarriedObjectAttachmentRevision(
    const CarriedObjectAttachmentDTO& normalized_attachment);

/// Computes pure logical-scene content identity. SceneRevision and the
/// transaction/topology/policy counters are deliberately excluded.
[[nodiscard]] ContentDigest ComputeLogicalSceneContentHash(
    const LogicalSceneSnapshotDTO& normalized_snapshot);

}  // namespace manyforge::core::logical_scene_identity
