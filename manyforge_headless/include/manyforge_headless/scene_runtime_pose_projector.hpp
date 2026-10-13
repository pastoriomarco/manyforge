#pragma once

#include "manyforge_headless/scene_runtime_state_owner.hpp"

#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace manyforge::headless {

/// Exact existing provider origin shared by projection and freshness admission.
[[nodiscard]] std::string
SceneRuntimePoseProviderSourceEpoch(const PoseSourceObservation &observation);

enum class SceneRuntimePoseApplicationStatus {
  kNotRequested,
  kDeferredBeforePlanning,
  kBuffered,
  kCommitted,
  kAlreadyApplied,
  kSkippedAttached,
  /// A strictly newer same-origin observation confirmed the unchanged
  /// committed pose: runtime freshness was refreshed through
  /// ISceneAuthority::ConfirmProviderFreshness without a scene transaction,
  /// so the carried projection revision is the unchanged current one.
  kFreshnessConfirmed,
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
/// When a strictly newer observation from the same provider origin and
/// freshness budget repeats the exact committed pose, it submits a
/// freshness-only confirmation instead and falls back to the transaction only
/// when the scene authority does not support confirmations.
class SceneRuntimePoseProjector final {
public:
  SceneRuntimePoseProjector(manyforge::core::ISceneAuthority &scene_authority,
                            manyforge::core::CellId cell_id);

  /// `prior_application` is the caller's retained result for this exact
  /// observation, if any. An already accepted freshness confirmation of the
  /// exact observation is reported again without re-confirming it, so a replay
  /// is idempotent and never refreshes freshness age.
  [[nodiscard]] manyforge::core::Result<SceneRuntimePoseApplicationResult>
  Apply(std::span<const manyforge::core::CapabilityProviderBindingDTO>
            provider_bindings,
        const manyforge::core::RetainedBundleId &active_retained_bundle_id,
        const manyforge::core::GraphRevision &active_graph_revision,
        const PoseSourceObservation &observation,
        const std::optional<SceneRuntimePoseApplicationResult>
            &prior_application = std::nullopt);

private:
  manyforge::core::ISceneAuthority &scene_authority_;
  manyforge::core::CellId cell_id_;
};

} // namespace manyforge::headless
