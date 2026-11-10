#pragma once

#include "manyforge_headless/scene_runtime_state_owner.hpp"

#include <manyforge_core/ports/iscene_authority.hpp>
#include <manyforge_core/types/cell_graph.hpp>
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
#include <variant>
#include <vector>

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
/// source-local link of the carrier frame's model instance, plus that carrier
/// frame's source link name for the runtime pose row.
struct SceneRuntimeReferenceLinkResolution final {
  manyforge::core::Pose3DTO carrier_in_reference;
  std::string carrier_link_name;

  friend bool operator==(const SceneRuntimeReferenceLinkResolution &,
                         const SceneRuntimeReferenceLinkResolution &) = default;
};

/// Errors are typed refusals: unknown links and any non-fixed joint on the
/// path never resolve.
using SceneRuntimeReferenceLinkResolver =
    std::function<manyforge::core::Result<SceneRuntimeReferenceLinkResolution>(
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

/// One actual attachment holder, including its ManyForge target and validated
/// model gripper base link.
struct SceneRuntimePoseHolder final {
  manyforge::core::ControllableComponentId controllable_component_id;
  manyforge::core::CommandTargetId command_target_id;
  std::string gripper_link;
  friend bool operator==(const SceneRuntimePoseHolder &,
                         const SceneRuntimePoseHolder &) = default;
};

/// Rigid body used as the relative observation reference.
struct SceneRuntimeMeasuredBody final {
  std::string entity;
  std::string link;
  friend bool operator==(const SceneRuntimeMeasuredBody &,
                         const SceneRuntimeMeasuredBody &) = default;
};

/// Read-only offset facts exposed beside the matching live pose-source row.
/// `measured_against` is either a measured body or the literal "cell frame".
struct SceneRuntimePoseMeasurement final {
  std::optional<SceneRuntimePoseHolder> holder;
  std::variant<std::string, SceneRuntimeMeasuredBody> measured_against;
  std::optional<std::string> source_header_frame_id;
  manyforge::core::ComponentSceneFrameRefDTO frame;
  manyforge::core::Pose3DTO current_offset;
  std::optional<manyforge::core::Pose3DTO> baseline_offset;
  std::optional<SceneRuntimePoseDistance> drift;

  friend bool operator==(const SceneRuntimePoseMeasurement &,
                         const SceneRuntimePoseMeasurement &) = default;
};

[[nodiscard]] SceneRuntimePoseDistance
SceneRuntimePoseDistanceBetween(const manyforge::core::Pose3DTO &from,
                                const manyforge::core::Pose3DTO &to) noexcept;

/// A carried object whose measured offset left its grasp-lost bound.
struct SceneRuntimeCarriedObjectLoss final {
  manyforge::core::SceneResourceId scene_resource_id;
  manyforge::core::CarriedObjectAttachmentId carried_object_attachment_id;
  manyforge::core::ControllableComponentId controllable_component_id;
  /// The gripper target holding it: the `carried_object_lost.<target>`
  /// system signal of `controllable_component_id`.
  manyforge::core::CommandTargetId command_target_id;
  manyforge::core::GraphRevision graph_revision;
  /// Unique per observation; the Stop request id.
  std::string request_id;
  /// The loss `event_id` and the attachment's stored
  /// `on_grasp_lost.set_key`, carried into the host's loss row.
  std::string loss_event_id{};
  std::optional<std::string> set_key{};
};

/// The components a carried-object-lost Stop names: the holder and
/// every component it is mounted on (the cell kinematic index's
/// `ancestors_of`), in id order as the Stop component selector requires.
[[nodiscard]] std::vector<manyforge::core::ControllableComponentId>
SceneRuntimeCarrierStopComponents(
    const manyforge::core::CellGraphDTO &graph,
    const manyforge::core::ControllableComponentId &holder);

/// Requests the normal operational Stop of the carrier's motion with reason
/// `carried_object_lost`; returns the refusal, if any.
using SceneRuntimeCarrierStopRequester =
    std::function<std::optional<manyforge::core::ErrorDTO>(
        const SceneRuntimeCarriedObjectLoss &)>;

/// The host's answer when the attachment's target has no
/// `carried_object_lost` system signal (not a gripper target of the committed
/// composition): the publication is not applicable, not a failure.
inline constexpr std::string_view kCarriedObjectLostSignalNotApplicable =
    "io_signal_unbound";

/// Publishes the level `true` of the gripper's `carried_object_lost.<target>`
/// system signal into the execution host's signal ingress; returns the
/// refusal, if any.
using SceneRuntimeCarriedObjectLostSignalPublisher =
    std::function<std::optional<manyforge::core::ErrorDTO>(
        const SceneRuntimeCarriedObjectLoss &)>;

struct SceneRuntimePoseApplicationResult final {
  SceneRuntimePoseApplicationStatus status;
  std::optional<manyforge::core::RequestId> scene_request_id;
  std::optional<manyforge::core::ProjectionRevisionDTO> projection_revision;
  std::optional<manyforge::core::SceneUpdateStatusDTO> update_status;
  std::optional<SceneRuntimePoseMeasurement> pose_measurement;

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
/// of that attachment (its baseline). Per the attachment's `on_grasp_lost`:
/// the gripper's `carried_object_lost.<target>` signal is published `true`
/// (always); `stop_motion` requests the carrier's motion Stop; `detach`
/// detaches the object at the observed cell pose. The update is not applied
/// and the result is the typed error `carried_object_lost` (message: the error
/// row's full text; details carry the four flags; `raise_alarm` is the
/// Headless alarm's selector). The baseline is forgotten only after a
/// successful detach, so a refused detach is detected and retried on the next
/// sample; without `detach` the attachment is marked lost and fires
/// once.
class SceneRuntimePoseProjector final {
public:
  SceneRuntimePoseProjector(
      manyforge::core::ISceneAuthority &scene_authority,
      manyforge::core::CellId cell_id,
      SceneRuntimeReferenceLinkResolver reference_resolver = {},
      SceneRuntimeCarrierStopRequester carrier_stop_requester = {},
      SceneRuntimeCarriedObjectLostSignalPublisher signal_publisher = {});

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
                    const manyforge::core::Pose3DTO &observed_in_cell,
                    std::optional<SceneRuntimePoseMeasurement>
                        pose_measurement = std::nullopt);

  struct GraspMeasurement final {
    manyforge::core::Pose3DTO baseline_offset;
    SceneRuntimePoseDistance drift;
    bool lost{};
  };

  /// Read and update the existing first-measured baseline. The first sample
  /// establishes a zero-drift baseline and is never lost.
  [[nodiscard]] std::optional<GraspMeasurement>
  MeasureGrasp(const manyforge::core::CarriedObjectAttachmentDTO &attachment,
               const manyforge::core::Pose3DTO &measured_in_carrier);
  void ForgetGraspBaseline(const std::string &scene_resource_id);

  struct GraspBaseline final {
    manyforge::core::CarriedObjectAttachmentId carried_object_attachment_id;
    manyforge::core::Pose3DTO measured_in_carrier;
    /// Lost but kept attached (`detach` false): never fires again.
    bool lost{};
  };
  void MarkGraspLost(const std::string &scene_resource_id);

  manyforge::core::ISceneAuthority &scene_authority_;
  manyforge::core::CellId cell_id_;
  SceneRuntimeReferenceLinkResolver reference_resolver_;
  SceneRuntimeCarrierStopRequester carrier_stop_requester_;
  SceneRuntimeCarriedObjectLostSignalPublisher signal_publisher_;
  /// In memory only: scene resource id -> first measured carried offset of
  /// its current attachment. Dropped when seen unattached or detached after a
  /// loss; a restart re-baselines on the next sample.
  std::mutex grasp_baselines_mutex_;
  std::map<std::string, GraspBaseline> grasp_baselines_;
};

} // namespace manyforge::headless
