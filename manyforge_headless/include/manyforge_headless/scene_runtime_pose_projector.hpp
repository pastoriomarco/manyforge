#pragma once

#include "manyforge_headless/scene_runtime_state_owner.hpp"

#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_deployment/model_source.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace manyforge::headless {

/// Exact existing provider origin shared by projection and freshness admission.
[[nodiscard]] std::string
SceneRuntimePoseProviderSourceEpoch(const PoseSourceObservation &observation);

enum class SceneRuntimePoseApplicationStatus {
  kNotRequested,
  kDeferredBeforePlanning,
  kDeferredBetweenNodes,
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

/// Whether the scene's retained provider sequence proves the exact successful
/// application outcome used by the before-planning latch. A freshness-only
/// confirmation intentionally advances the observation without rewriting the
/// committed scene evidence.
[[nodiscard]] bool SceneRuntimePoseApplicationRetainsPlanningEvidence(
    SceneRuntimePoseApplicationStatus status,
    std::uint64_t retained_provider_source_sequence,
    std::uint64_t observation_source_sequence) noexcept;

/// Fixed retained-model pose of the carrier frame in `reference_link`, a
/// source-local link of the carrier frame's model instance. Errors are typed
/// refusals: unknown links and any non-fixed joint on the path never resolve.
using SceneRuntimeReferenceLinkResolver =
    std::function<manyforge::core::Result<manyforge::core::Pose3DTO>(
        const manyforge::core::RetainedBundleId &retained_bundle_id,
        const manyforge::core::ComponentSceneFrameRefDTO &carrier_frame,
        const std::string &reference_link)>;

/// Pose of `target_link` in `reference_link` when only fixed joints join them.
[[nodiscard]] manyforge::core::Result<manyforge::core::Pose3DTO>
SceneRuntimeFixedLinkTransform(const manyforge::deployment::UrdfDocumentIr &urdf,
                               std::string_view reference_link,
                               std::string_view target_link);

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
/// Under `continuous_relative_to_attached` an attached object's update is the
/// observed reference-relative pose composed onto the carrier frame through
/// `reference_resolver`; no robot pose is involved.
class SceneRuntimePoseProjector final {
public:
  SceneRuntimePoseProjector(
      manyforge::core::ISceneAuthority &scene_authority,
      manyforge::core::CellId cell_id,
      SceneRuntimeReferenceLinkResolver reference_resolver = {});

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
            &prior_application = std::nullopt,
        std::string_view apply_policy = {});

private:
  manyforge::core::ISceneAuthority &scene_authority_;
  manyforge::core::CellId cell_id_;
  SceneRuntimeReferenceLinkResolver reference_resolver_;
};

} // namespace manyforge::headless
