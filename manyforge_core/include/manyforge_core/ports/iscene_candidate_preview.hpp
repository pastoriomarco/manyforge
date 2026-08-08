#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/scene_transaction.hpp>

namespace manyforge::core {

/// Focused owner-to-owner surface for non-durable scene successors used by
/// planning candidate search. It is deliberately separate from the public
/// ISceneAuthority edit API: callers cannot turn a preview into committed
/// logical truth or bypass the ordinary durable Commit() protocol.
class ISceneCandidatePreview {
 public:
  virtual ~ISceneCandidatePreview() = default;

  [[nodiscard]] virtual Result<SceneCandidatePreviewDTO>
  PreviewResolvedSceneCandidate(
      const ResolvedSceneCandidatePreviewRequestDTO& request) const = 0;
};

}  // namespace manyforge::core
