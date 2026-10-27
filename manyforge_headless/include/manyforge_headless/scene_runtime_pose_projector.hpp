#pragma once

#include "manyforge_headless/scene_runtime_state_owner.hpp"

#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_deployment/model_source.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace manyforge::headless {

/// Exact existing provider origin shared by projection and pose-source admission.
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
  /// A same-origin observation repeats the exact pose already in the scene:
  /// no transaction was submitted, so the carried projection revision is the
  /// unchanged current one.
  kUnchanged,
};

[[nodiscard]] std::string_view
ToString(SceneRuntimePoseApplicationStatus status) noexcept;

/// Whether the caller's retained application of its latest observation is a
/// success that the before-planning latch accepts as planning evidence. An
/// unchanged sample is a success without a scene commit.
[[nodiscard]] bool SceneRuntimePoseApplicationRetainsPlanningEvidence(
    SceneRuntimePoseApplicationStatus status) noexcept;

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

/// Deadband of one scene pose-source binding. A same-origin observation
/// within both bounds of the object's current pose is `kUnchanged`. A zero
/// bound means exact equality on that axis, so the default is exact.
struct SceneRuntimePoseDeadband final {
  double translation_m{};
  double rotation_rad{};
};

/// Translation and rotation between two poses.
struct SceneRuntimePoseDistance final {
  double translation_m{};
  double rotation_rad{};
};

[[nodiscard]] SceneRuntimePoseDistance
SceneRuntimePoseDistanceBetween(const manyforge::core::Pose3DTO &from,
                                const manyforge::core::Pose3DTO &to) noexcept;

/// A carried object whose measured offset left its grasp-lost bound.
struct SceneRuntimeCarriedObjectLoss final {
  manyforge::core::SceneResourceId scene_resource_id;
  manyforge::core::CarriedObjectAttachmentId carried_object_attachment_id;
  manyforge::core::ControllableComponentId controllable_component_id;
  manyforge::core::GraphRevision graph_revision;
  /// Unique per observation; the Stop request id.
  std::string request_id;
};

/// Requests the normal operational Stop of the carrier's motion with reason
/// `carried_object_lost`; returns the refusal, if any.
using SceneRuntimeCarrierStopRequester =
    std::function<std::optional<manyforge::core::ErrorDTO>(
        const SceneRuntimeCarriedObjectLoss &)>;

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
/// it only validates the closed observation payload, derives the provider
/// identity evidence, and submits one compare-and-commit pose transaction.
/// Observation order is enforced per source by the scene runtime upstream.
/// An observation from the same provider origin whose update pose is
/// bit-identical to the object's current pose in the update frame (cell, or
/// the carrier under `continuous_relative_to_attached`) submits nothing.
/// Committed evidence carries no observation sequence, time or freshness.
/// Under `continuous_relative_to_attached` an attached object's update is the
/// observed reference-relative pose composed onto the carrier frame through
/// `reference_resolver`; no robot pose is involved.
/// Under `continuous` and `continuous_relative_to_attached`, an attached
/// object whose attachment carries a grasp-lost check is lost when the
/// measured carried offset leaves the bound around the first measured offset
/// of that attachment (its baseline):
/// the update is not applied, the carrier's motion Stop is requested, the
/// object is detached at the observed cell pose, and the result is the typed
/// error `carried_object_lost` (message: the error row's full text).
class SceneRuntimePoseProjector final {
public:
  SceneRuntimePoseProjector(
      manyforge::core::ISceneAuthority &scene_authority,
      manyforge::core::CellId cell_id,
      SceneRuntimeReferenceLinkResolver reference_resolver = {},
      SceneRuntimeCarrierStopRequester carrier_stop_requester = {});

  /// `prior_application` is the caller's retained result for this exact
  /// observation, if any; an attached object's retained success is reported
  /// again instead of reapplying the old observation.
  [[nodiscard]] manyforge::core::Result<SceneRuntimePoseApplicationResult>
  Apply(std::span<const manyforge::core::CapabilityProviderBindingDTO>
            provider_bindings,
        const manyforge::core::RetainedBundleId &active_retained_bundle_id,
        const manyforge::core::GraphRevision &active_graph_revision,
        const PoseSourceObservation &observation,
        const std::optional<SceneRuntimePoseApplicationResult>
            &prior_application = std::nullopt,
        std::string_view apply_policy = {},
        SceneRuntimePoseDeadband deadband = {});

private:
  [[nodiscard]] manyforge::core::SceneCommitResultDTO
  Commit(manyforge::core::RequestId request_id,
         const manyforge::core::ProjectionRevisionDTO &projection,
         manyforge::core::SceneOperationRequestDTO operation);
  [[nodiscard]] manyforge::core::Result<SceneRuntimePoseApplicationResult>
  CarriedObjectLost(const manyforge::core::CarriedObjectAttachmentDTO &attachment,
                    const SceneRuntimePoseDistance &distance,
                    const manyforge::core::ProjectionRevisionDTO &projection,
                    const manyforge::core::RequestId &request_id,
                    const manyforge::core::Pose3DTO &observed_in_cell);

  /// Distance of a measured carried offset from its attachment's baseline
  /// when beyond the grasp-lost bound. The first measured sample of an
  /// attachment sets the baseline and is never lost.
  [[nodiscard]] std::optional<SceneRuntimePoseDistance>
  GraspDrift(const manyforge::core::CarriedObjectAttachmentDTO &attachment,
             const manyforge::core::Pose3DTO &measured_in_carrier);
  void ForgetGraspBaseline(const std::string &scene_resource_id);

  struct GraspBaseline final {
    manyforge::core::CarriedObjectAttachmentId carried_object_attachment_id;
    manyforge::core::Pose3DTO measured_in_carrier;
  };

  manyforge::core::ISceneAuthority &scene_authority_;
  manyforge::core::CellId cell_id_;
  SceneRuntimeReferenceLinkResolver reference_resolver_;
  SceneRuntimeCarrierStopRequester carrier_stop_requester_;
  /// In memory only: scene resource id -> first measured carried offset of
  /// its current attachment. Dropped when seen unattached or lost; a restart
  /// re-baselines on the next sample.
  std::mutex grasp_baselines_mutex_;
  std::map<std::string, GraspBaseline> grasp_baselines_;
};

} // namespace manyforge::headless
