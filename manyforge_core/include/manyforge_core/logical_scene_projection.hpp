#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::core {

/// Apply one scene-owner-resolved delta to its exact predecessor snapshot.
///
/// This function is deliberately narrower than logical-scene authority. It
/// accepts no public request, resolves no frame, mints no identity, and
/// performs no native effect. The owner validated and digested the successor
/// once; this applies the owner's resolved delta and trusts its proposed
/// revision and digest. It refuses only a delta that does not apply: a wrong
/// predecessor, a non-advancing sequence, a changed bundle, graph or Scene, a
/// missing object or attachment, a duplicate attachment, or inconsistent
/// revision counters. It never validates or digests the whole snapshot.
[[nodiscard]] Result<LogicalSceneSnapshotDTO> ApplyResolvedSceneTransaction(
    const LogicalSceneSnapshotDTO& current, const SceneTransactionDTO& transaction);

}  // namespace manyforge::core
