#pragma once

#include "manyforge_headless/scene_runtime_state_owner.hpp"

#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace manyforge::headless {

enum class SceneRuntimePoseApplicationStatus {
  kNotRequested,
  kDeferredBeforePlanning,
  kBuffered,
  kCommitted,
  kAlreadyApplied,
  kSkippedAttached,
};

[[nodiscard]] std::string_view
ToString(SceneRuntimePoseApplicationStatus status) noexcept;

struct SceneRuntimePoseApplicationResult final {
  SceneRuntimePoseApplicationStatus status;
  std::optional<manyforge::core::RequestId> scene_request_id;
  std::optional<manyforge::core::ProjectionRevisionDTO> projection_revision;
  std::optional<manyforge::core::SceneUpdateStatusDTO> update_status;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const SceneRuntimePoseApplicationResult &,
                         const SceneRuntimePoseApplicationResult &) = default;
};

/// Manager-local adapter from one admitted pose observation to the sole
/// logical-scene authority. The adapter owns no source cache or scene truth;
/// it only validates the closed observation payload, derives exact retained
/// provider evidence, and submits one compare-and-commit pose transaction.
class SceneRuntimePoseProjector final {
public:
  SceneRuntimePoseProjector(manyforge::core::ISceneAuthority &scene_authority,
                            manyforge::core::CellId cell_id);

  [[nodiscard]] manyforge::core::Result<SceneRuntimePoseApplicationResult>
  Apply(std::span<const manyforge::core::CapabilityProviderBindingDTO>
            provider_bindings,
        const manyforge::core::RetainedBundleId &active_retained_bundle_id,
        const manyforge::core::GraphRevision &active_graph_revision,
        const PoseSourceObservation &observation);

private:
  manyforge::core::ISceneAuthority &scene_authority_;
  manyforge::core::CellId cell_id_;
};

} // namespace manyforge::headless
