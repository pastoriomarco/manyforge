#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::core {

/// Reconstruct one scene-owner-normalized immutable snapshot delta.
///
/// This function is deliberately narrower than logical-scene authority. It
/// accepts no public request, resolves no frame, mints no identity, and
/// performs no native effect. Projection adapters use it to verify that the
/// complete resolved operation and proposed revision received from the sole
/// scene owner produce exactly one deterministic successor snapshot.
[[nodiscard]] Result<LogicalSceneSnapshotDTO> ApplyResolvedSceneTransaction(
    const LogicalSceneSnapshotDTO& current, const SceneTransactionDTO& transaction);

}  // namespace manyforge::core
