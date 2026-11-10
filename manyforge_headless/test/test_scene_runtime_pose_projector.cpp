#include "manyforge_headless/scene_runtime_pose_projector.hpp"

#include <manyforge_core/types/carried_object_lost.hpp>

#include <gtest/gtest.h>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using content_identity::CanonicalValue;

std::string Digest(const char value) {
  return "sha256:" + std::string(64U, value);
}

core::ProjectionRevisionDTO Projection(const std::uint64_t revision = 1U) {
  return {
      core::RetainedBundleId(Digest('a')),
      core::GraphRevision(Digest('b')),
      core::SceneRevision{
          core::SceneId(Digest('c')),
          revision,
          core::ContentDigest(content_identity::sha256_digest(
              "scene-" + std::to_string(revision))),
      },
  };
}

core::CapabilityProviderBindingDTO Provider(const core::CellId &cell_id) {
  return {
      core::CapabilityProviderBindingId("cell.logical-scene.pose"),
      core::CapabilityProviderBindingRevision(Digest('d')),
      core::CapabilityId("scene.edit.update_pose"),
      core::CapabilityDefinitionRevision(Digest('e')),
      core::CapabilityScope::kCellGlobal,
      core::CellGlobalCapabilityProviderTargetDTO{cell_id},
      core::ProviderInstanceId("cell.logical_scene"),
      core::ProviderManifestId("manyforge.provider.logical_scene"),
      core::ProviderManifestRevision(Digest('f')),
      {},
  };
}

CanonicalValue PosePayload(const double x = 0.25,
                           const double quaternion_w = 1.0,
                           const std::int64_t source_stamp_ns = 1250000000,
                           const double quaternion_z = 0.0) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.ros_pose_observation.v2")},
      {"kind", CanonicalValue("simulation_entity_pose")},
      {"pose", CanonicalValue(CanonicalValue::Object{
                   {"frame_id", CanonicalValue("universe")},
                   {"frame_kind", CanonicalValue("cell_frame")},
                   {"orientation_xyzw", CanonicalValue(CanonicalValue::Array{
                                            CanonicalValue(0.0),
                                            CanonicalValue(0.0),
                                            CanonicalValue(quaternion_z),
                                            CanonicalValue(quaternion_w),
                                        })},
                   {"position_m", CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(x),
                                      CanonicalValue(-0.5),
                                      CanonicalValue(0.75),
                                  })},
                   {"source_stamp_ns", content_identity::nonnegative_int64_value(source_stamp_ns)},
               })},
      {"source", CanonicalValue(CanonicalValue::Object{
                     {"entity", CanonicalValue("/box")},
                 })},
  });
}

PoseSourceObservation
Observation(const CanonicalValue &payload = PosePayload()) {
  return {
      {"isaac.observer", "box", "continuous_unattached"},
      "source-epoch.1",
      1U,
      2U,
      100,
      1000,
      PoseSourceQuality::kGood,
      true,
      payload,
  };
}

class FakeSceneAuthority final : public core::ISceneAuthority {
public:
  core::SceneCommitResultDTO
  Commit(const core::SceneTransactionRequestDTO &request) override {
    ++commit_count;
    last_request = request;
    if (failure.has_value()) {
      return {
          request.request_context.request_id,
          core::RequestReplayStatusDTO::kAcceptedNew,
          false,
          failure,
          std::nullopt,
          std::nullopt,
          core::SceneProjectionCommitStatusDTO::kRejectedRollbackProven,
          std::nullopt,
      };
    }
    if (const auto *detach =
            std::get_if<core::DetachCarriedObjectRequestDTO>(&request.operation)) {
      if (detach_refusals > 0U) {
        --detach_refusals;
        return {request.request_context.request_id,
                core::RequestReplayStatusDTO::kAcceptedNew, false,
                core::ErrorDTO{"scene_object_state_conflict", "refused", {}},
                std::nullopt, std::nullopt,
                core::SceneProjectionCommitStatusDTO::kRejectedRollbackProven,
                std::nullopt};
      }
      // Detach at the given cell pose.
      detached_at = detach->pose;
      state.attached = false;
      state.carried_object_attachment_id.reset();
      pose = detach->pose;
      projection = Projection(projection.scene_revision.revision_id + 1U);
      return {request.request_context.request_id,
              core::RequestReplayStatusDTO::kAcceptedNew, true, std::nullopt,
              static_cast<std::uint64_t>(commit_count), projection,
              core::SceneProjectionCommitStatusDTO::kCommitted,
              core::SceneUpdateStatusDTO::kGeometryChanged};
    }
    const auto &update =
        std::get<core::UpdateObjectPoseRequestDTO>(request.operation);
    pose = update.pose;
    state.provider_evidence = update.provider_evidence;
    projection = Projection(projection.scene_revision.revision_id + 1U);
    return {
        request.request_context.request_id,
        core::RequestReplayStatusDTO::kAcceptedNew,
        true,
        std::nullopt,
        static_cast<std::uint64_t>(commit_count),
        projection,
        core::SceneProjectionCommitStatusDTO::kCommitted,
        core::SceneUpdateStatusDTO::kGeometryChanged,
    };
  }

  core::RequestOwnerEpoch CurrentRequestOwnerEpoch() const override {
    return core::RequestOwnerEpoch(owner_epoch);
  }

  core::ProjectionRevisionDTO CurrentProjectionRevision() const override {
    return projection;
  }

  std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  LogicalSnapshot() const override {
    return snapshot;
  }

  std::vector<core::SceneObjectSnapshotDTO> GetSceneObjects() const override {
    return {};
  }

  core::SceneObjectStateDTO GetObjectState(
      const core::SceneResourceId &scene_resource_id) const override {
    auto result = state;
    result.scene_resource_id = scene_resource_id;
    return result;
  }

  core::Result<core::Pose3DTO>
  GetObjectPose(const core::SceneResourceId &) const override {
    if (!state.exists) {
      return core::Result<core::Pose3DTO>::Err(
          core::ErrorDTO{"object_not_found", "object is absent", {}});
    }
    return core::Result<core::Pose3DTO>::Ok(pose);
  }

  core::ProjectionRevisionDTO projection{Projection()};
  core::SceneObjectStateDTO state{core::SceneResourceId("box"), true, false,
                                  std::nullopt, std::nullopt};
  core::Pose3DTO pose{};
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> snapshot;
  std::optional<core::ErrorDTO> failure;
  std::optional<core::SceneTransactionRequestDTO> last_request;
  std::optional<core::Pose3DTO> detached_at;
  std::size_t commit_count{};
  std::string owner_epoch{"scene-owner.1"};
  std::size_t detach_refusals{};
};

PoseSourceObservation Newer(PoseSourceObservation observation,
                            const std::uint64_t sequence) {
  const auto budget = observation.expires_at_ns - observation.observed_at_ns;
  observation.source_sequence = sequence;
  observation.observed_at_ns += 250;
  observation.expires_at_ns = observation.observed_at_ns + budget;
  return observation;
}

core::Result<SceneRuntimePoseApplicationResult>
ApplyTo(SceneRuntimePoseProjector &projector, const FakeSceneAuthority &scene,
        const std::vector<core::CapabilityProviderBindingDTO> &providers,
        const PoseSourceObservation &observation,
        const std::optional<SceneRuntimePoseApplicationResult> &prior =
            std::nullopt) {
  return projector.Apply(providers, scene.projection.retained_bundle_id,
                         scene.projection.graph_revision, observation, prior);
}

TEST(SceneRuntimePoseProjector,
     CommitsProviderIdentityEvidenceAndRecognizesReplay) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  const auto observation = Observation();

  const auto committed =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, observation);

  ASSERT_TRUE(committed.ok()) << committed.error().message;
  ASSERT_TRUE(committed.value().valid());
  EXPECT_EQ(committed.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 1U);
  ASSERT_TRUE(scene.last_request.has_value());
  EXPECT_EQ(scene.last_request->request_context.source_label,
            "scene_runtime_pose_source");
  EXPECT_EQ(scene.last_request->request_context.correlation_id,
            scene.last_request->request_context.request_id.value());
  const auto &update =
      std::get<core::UpdateObjectPoseRequestDTO>(scene.last_request->operation);
  EXPECT_EQ(update.scene_resource_id.value(), "box");
  EXPECT_EQ(update.expected_attached, false);
  const auto *frame =
      std::get_if<core::CellSceneFrameRefDTO>(&update.frame_ref);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->cell_id, cell_id);
  EXPECT_DOUBLE_EQ(update.pose.position_m.x_m, 0.25);
  EXPECT_DOUBLE_EQ(update.pose.position_m.y_m, -0.5);
  EXPECT_DOUBLE_EQ(update.pose.position_m.z_m, 0.75);
  ASSERT_TRUE(update.provider_evidence.has_value());
  EXPECT_EQ(update.provider_evidence->provider_instance_id.value(),
            "cell.logical_scene");
  EXPECT_EQ(update.provider_evidence->provider_manifest_id.value(),
            "manyforge.provider.logical_scene");
  EXPECT_TRUE(content_identity::is_sha256_digest(
      update.provider_evidence->provider_source_epoch));

  // A replay of the committed observation is an unchanged same-origin sample.
  const auto replay =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, observation);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().status,
            SceneRuntimePoseApplicationStatus::kUnchanged);
  EXPECT_EQ(scene.commit_count, 1U);
}

TEST(SceneRuntimePoseProjector, LongServicePoseEvidenceRemainsIdempotent) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  constexpr std::int64_t years31 = 31LL * 366 * 86400 * 1000000000;
  auto observation = Observation(PosePayload(0.25, 1.0, years31 - 1));
  observation.observed_at_ns = years31;
  observation.expires_at_ns = observation.observed_at_ns + 900;
  const auto applied = projector.Apply(providers, scene.projection.retained_bundle_id,
                                        scene.projection.graph_revision, observation);
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  ASSERT_TRUE(scene.last_request.has_value());
  const auto &update = std::get<core::UpdateObjectPoseRequestDTO>(scene.last_request->operation);
  ASSERT_TRUE(update.provider_evidence.has_value());
  const auto replay = projector.Apply(providers, scene.projection.retained_bundle_id,
                                       scene.projection.graph_revision, observation);
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(scene.commit_count, 1U);
}

TEST(SceneRuntimePoseProjector, SkipsAttachedObjectsWithoutChangingScene) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  scene.state.attached = true;
  scene.state.carried_object_attachment_id =
      core::CarriedObjectAttachmentId("attachment.1");
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};

  const auto result =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, Observation());

  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().status,
            SceneRuntimePoseApplicationStatus::kSkippedAttached);
  EXPECT_EQ(scene.commit_count, 0U);
}

TEST(SceneRuntimePoseProjector, UnchangedSameOriginSampleDoesNotCommit) {
  for (const auto *policy : {"continuous_unattached", "continuous",
                             "before_planning", "between_nodes"}) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    SceneRuntimePoseProjector projector(scene, cell_id);
    const std::vector providers{Provider(cell_id)};
    const auto apply = [&](const PoseSourceObservation &observation) {
      return projector.Apply(providers, scene.projection.retained_bundle_id,
                             scene.projection.graph_revision, observation,
                             std::nullopt, policy);
    };
    ASSERT_TRUE(apply(Observation()).ok());
    ASSERT_EQ(scene.commit_count, 1U);
    const auto committed_projection = scene.projection;

    // A newer same-origin sample of the bit-identical pose; its source
    // timestamp differs and is irrelevant.
    const auto newer =
        Newer(Observation(PosePayload(0.25, 1.0, 1500000000)), 3U);
    const auto unchanged = apply(newer);
    ASSERT_TRUE(unchanged.ok()) << unchanged.error().message;
    ASSERT_TRUE(unchanged.value().valid());
    EXPECT_EQ(unchanged.value().status,
              SceneRuntimePoseApplicationStatus::kUnchanged);
    EXPECT_EQ(ToString(unchanged.value().status), "unchanged");
    EXPECT_EQ(unchanged.value().projection_revision, committed_projection);
    EXPECT_FALSE(unchanged.value().update_status.has_value());
    EXPECT_EQ(scene.commit_count, 1U);
    EXPECT_EQ(scene.projection, committed_projection);
    EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
        unchanged.value().status));
  }
}

TEST(SceneRuntimePoseProjector, ContinuousUpdatesAttachedPoseThroughAuthority) {
  for (const auto *policy : {"continuous", "between_nodes"}) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    scene.state.attached = true;
    SceneRuntimePoseProjector projector(scene, cell_id);
    const std::vector providers{Provider(cell_id)};
    const auto result = projector.Apply(
        providers, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, Observation(), std::nullopt, policy);
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().status,
              SceneRuntimePoseApplicationStatus::kCommitted);
    ASSERT_EQ(scene.commit_count, 1U);
    const auto &update = std::get<core::UpdateObjectPoseRequestDTO>(
        scene.last_request->operation);
    EXPECT_FALSE(update.expected_attached.has_value());
    EXPECT_DOUBLE_EQ(update.pose.position_m.x_m, 0.25);
    const auto replay = projector.Apply(
        providers, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, Observation(), result.value(), policy);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    EXPECT_EQ(replay.value().status,
              SceneRuntimePoseApplicationStatus::kAlreadyApplied);
    EXPECT_EQ(scene.commit_count, 1U);
    // A cell-frame update of an attached object has no unchanged shortcut.
    const auto newer = projector.Apply(
        providers, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, Newer(Observation(), 3U),
        std::nullopt, policy);
    ASSERT_TRUE(newer.ok()) << newer.error().message;
    EXPECT_EQ(newer.value().status,
              SceneRuntimePoseApplicationStatus::kCommitted);
    EXPECT_EQ(scene.commit_count, 2U);
  }
}

TEST(SceneRuntimePoseProjector,
     RejectsMissingObjectsMalformedPayloadAndProviderAmbiguity) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};

  scene.state.exists = false;
  const auto missing =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, Observation());
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, "scene_runtime_pose_object_missing");

  scene.state.exists = true;
  const auto malformed = projector.Apply(
      providers, scene.projection.retained_bundle_id,
      scene.projection.graph_revision, Observation(PosePayload(0.25, 0.5)));
  ASSERT_FALSE(malformed.ok());
  EXPECT_EQ(malformed.error().code, "scene_runtime_pose_payload_invalid");

  const std::vector<core::CapabilityProviderBindingDTO> absent;
  const auto no_provider =
      projector.Apply(absent, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, Observation());
  ASSERT_FALSE(no_provider.ok());
  EXPECT_EQ(no_provider.error().code, "scene_runtime_pose_provider_invalid");

  const std::vector duplicate{Provider(cell_id), Provider(cell_id)};
  const auto ambiguous =
      projector.Apply(duplicate, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, Observation());
  ASSERT_FALSE(ambiguous.ok());
  EXPECT_EQ(ambiguous.error().code, "scene_runtime_pose_provider_invalid");
}

TEST(SceneRuntimePoseProjector, FailsClosedOnSceneOwnerRejection) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  const auto observation = Observation();
  const auto initial =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, observation);
  ASSERT_TRUE(initial.ok());

  scene.state.provider_evidence.reset();
  scene.failure = core::ErrorDTO{
      "native_projection_failed", "native projection rejected", {}};
  auto next = observation;
  next.source_sequence = 4U;
  const auto failed =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, next);
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "scene_runtime_pose_commit_failed");
  EXPECT_EQ(failed.error().details.at("scene_error_code"),
            "native_projection_failed");
}

TEST(SceneRuntimePoseProjector, PlanningEvidenceRuleIsStatusSensitive) {
  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kCommitted));
  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kAlreadyApplied));
  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kUnchanged));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kNotRequested));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kBuffered));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kSkippedAttached));
}

TEST(SceneRuntimePoseProjector, CommitsChangedPoseOrOtherOrigin) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  ASSERT_TRUE(ApplyTo(projector, scene, providers, Observation()).ok());

  const auto moved = ApplyTo(projector, scene, providers,
                             Newer(Observation(PosePayload(0.5)), 3U));
  ASSERT_TRUE(moved.ok()) << moved.error().message;
  EXPECT_EQ(moved.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 2U);
  EXPECT_DOUBLE_EQ(scene.pose.position_m.x_m, 0.5);

  // No deadband: the smallest representable change commits.
  const auto nudged =
      ApplyTo(projector, scene, providers,
              Newer(Observation(PosePayload(std::nextafter(0.5, 1.0))), 4U));
  ASSERT_TRUE(nudged.ok()) << nudged.error().message;
  EXPECT_EQ(nudged.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 3U);

  // A different provider origin with the identical pose commits.
  auto other_epoch =
      Newer(Observation(PosePayload(std::nextafter(0.5, 1.0))), 5U);
  other_epoch.source_epoch = "source-epoch.2";
  const auto origin = ApplyTo(projector, scene, providers, other_epoch);
  ASSERT_TRUE(origin.ok()) << origin.error().message;
  EXPECT_EQ(origin.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 4U);
}

CanonicalValue WithMember(const CanonicalValue &payload, const std::string &name,
                          CanonicalValue value) {
  auto members = payload.as_object();
  const auto found =
      std::find_if(members.begin(), members.end(),
                   [&](const auto &member) { return member.first == name; });
  if (found != members.end()) {
    found->second = std::move(value);
  } else {
    members.emplace_back(name, std::move(value));
  }
  return CanonicalValue(std::move(members));
}

CanonicalValue ReferencedPayload(const bool coherent = true,
                                 const bool relative = true,
                                 const double reference_x = 0.02,
                                 const double reference_yaw = 0.0) {
  return WithMember(
      WithMember(PosePayload(), "kind",
                 CanonicalValue("simulation_entity_pose_with_references.v1")),
      "references",
      CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"controllable_component_id", CanonicalValue("robot")},
          {"command_target_id", CanonicalValue("arm")},
          {"gripper_link", CanonicalValue("gripper_base")},
          {"coherent", CanonicalValue(coherent)},
          {"entity", CanonicalValue("/gripper")},
          {"link", CanonicalValue("gripper_base")},
          {"source_header_frame_id", CanonicalValue("gripper_base")},
          {"pose_in_reference",
           relative
               ? CanonicalValue(CanonicalValue::Object{
                     {"orientation_xyzw",
                      CanonicalValue(CanonicalValue::Array{
                          CanonicalValue(0.0), CanonicalValue(0.0),
                          CanonicalValue(std::sin(reference_yaw / 2.0)),
                          CanonicalValue(std::cos(reference_yaw / 2.0))})},
                     {"position_m", CanonicalValue(CanonicalValue::Array{
                                        CanonicalValue(reference_x),
                                        CanonicalValue(0.0),
                                        CanonicalValue(0.25)})},
                 })
               : CanonicalValue(nullptr)},
      })}));
}

const core::ComponentSceneFrameRefDTO kCarrier{
    core::ControllableComponentId("robot"), core::ModelInstanceId("robot.model"),
    core::FrameRoleId("gripper_tcp")};

void AttachInSnapshot(FakeSceneAuthority &scene,
                      const core::Pose3DTO &pose_in_carrier = {},
                      const std::optional<core::CarriedObjectGraspLostCheckDTO>
                          &grasp_lost_check = std::nullopt) {
  scene.state.attached = true;
  scene.state.carried_object_attachment_id =
      core::CarriedObjectAttachmentId("attachment.1");
  scene.snapshot = std::make_shared<core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          scene.projection.scene_revision,
          core::CellId("cell.1"),
          1U,
          1U,
          1U,
          {},
          {},
          {core::CarriedObjectAttachmentDTO{
              core::CarriedObjectAttachmentId("attachment.1"),
              core::CarriedObjectAttachmentRevision(Digest('9')),
              core::SceneResourceId("box"), kCarrier.controllable_component_id,
              core::CommandTargetId("arm"), kCarrier, pose_in_carrier, {},
              grasp_lost_check}},
      });
}

SceneRuntimeReferenceLinkResolver TcpAboveReference(std::size_t &calls) {
  return [&calls](const core::RetainedBundleId &,
                  const core::ComponentSceneFrameRefDTO &carrier,
                  const std::string &reference_link) {
    ++calls;
    EXPECT_EQ(carrier, kCarrier);
    EXPECT_EQ(reference_link, "gripper_base");
    return core::Result<SceneRuntimeReferenceLinkResolution>::Ok(
        SceneRuntimeReferenceLinkResolution{
            core::Pose3DTO{{0.0, 0.0, 0.2}, {0.0, 0.0, 0.0, 1.0}},
            "gripper_tcp_link"});
  };
}

TEST(SceneRuntimePoseProjector,
     RelativePolicySetsAttachedOffsetFromReferenceInCarrierFrame) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  AttachInSnapshot(scene);
  std::size_t calls = 0U;
  SceneRuntimePoseProjector projector(scene, cell_id, TcpAboveReference(calls));
  const auto result = projector.Apply(
      std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
      scene.projection.graph_revision, Observation(ReferencedPayload()),
      std::nullopt, "continuous_relative_to_attached");
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(calls, 1U);
  const auto &update =
      std::get<core::UpdateObjectPoseRequestDTO>(scene.last_request->operation);
  EXPECT_EQ(update.frame_ref, core::SceneTransactionFrameRefDTO{kCarrier});
  EXPECT_EQ(update.expected_attached, true);
  EXPECT_NEAR(update.pose.position_m.x_m, 0.02, 1e-12);
  EXPECT_NEAR(update.pose.position_m.y_m, 0.0, 1e-12);
  EXPECT_NEAR(update.pose.position_m.z_m, 0.05, 1e-12);
  EXPECT_TRUE(update.provider_evidence.has_value());
  ASSERT_TRUE(result.value().pose_measurement.has_value());
  const auto &measurement = *result.value().pose_measurement;
  ASSERT_TRUE(measurement.holder.has_value());
  EXPECT_EQ(measurement.holder->controllable_component_id.value(), "robot");
  EXPECT_EQ(measurement.holder->command_target_id.value(), "arm");
  EXPECT_EQ(measurement.holder->gripper_link, "gripper_base");
  EXPECT_EQ(std::get<SceneRuntimeMeasuredBody>(measurement.measured_against),
            (SceneRuntimeMeasuredBody{"/gripper", "gripper_base"}));
  EXPECT_EQ(measurement.source_header_frame_id, "gripper_base");
  EXPECT_FALSE(measurement.baseline_offset.has_value());
  EXPECT_FALSE(measurement.drift.has_value());
}

TEST(SceneRuntimePoseProjector, RelativePolicySelectsExactHolderTarget) {
  const core::CellId cell_id("cell.1");
  const auto matching = ReferencedPayload();
  const auto other = ReferencedPayload(true, true, 0.9);
  const auto other_target = WithMember(
      other.find_object_member("references")->as_array().front(),
      "command_target_id", CanonicalValue("other_gripper"));
  const auto matching_target =
      matching.find_object_member("references")->as_array().front();
  const auto apply = [&](const CanonicalValue &payload) {
    FakeSceneAuthority scene;
    AttachInSnapshot(scene);
    std::size_t calls = 0U;
    SceneRuntimePoseProjector projector(scene, cell_id, TcpAboveReference(calls));
    const auto result = projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, Observation(payload), std::nullopt,
        "continuous_relative_to_attached");
    return std::pair{result, scene.last_request};
  };

  const auto selected = apply(WithMember(
      matching, "references",
      CanonicalValue(CanonicalValue::Array{other_target, matching_target})));
  ASSERT_TRUE(selected.first.ok()) << selected.first.error().message;
  ASSERT_TRUE(selected.second.has_value());
  EXPECT_NEAR(std::get<core::UpdateObjectPoseRequestDTO>(selected.second->operation)
                  .pose.position_m.x_m,
              0.02, 1e-12);

  const auto missing = apply(WithMember(
      matching, "references", CanonicalValue(CanonicalValue::Array{other_target})));
  ASSERT_FALSE(missing.first.ok());
  EXPECT_EQ(missing.first.error().code, "scene_runtime_pose_reference_missing");
  const auto duplicate = apply(WithMember(
      matching, "references",
      CanonicalValue(CanonicalValue::Array{matching_target, matching_target})));
  ASSERT_FALSE(duplicate.first.ok());
  EXPECT_EQ(duplicate.first.error().code, "scene_runtime_pose_payload_invalid");
}

TEST(SceneRuntimePoseProjector,
     RelativePolicyUnchangedCarrierPoseDoesNotCommit) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  AttachInSnapshot(scene);
  std::size_t calls = 0U;
  SceneRuntimePoseProjector projector(scene, cell_id, TcpAboveReference(calls));
  const auto apply = [&](const std::uint64_t sequence) {
    return projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision,
        Newer(Observation(ReferencedPayload()), sequence), std::nullopt,
        "continuous_relative_to_attached");
  };
  ASSERT_TRUE(apply(2U).ok());
  ASSERT_EQ(scene.commit_count, 1U);
  const auto committed_pose =
      std::get<core::UpdateObjectPoseRequestDTO>(scene.last_request->operation)
          .pose;

  // The attachment now holds exactly the committed carrier-relative pose.
  AttachInSnapshot(scene, committed_pose);
  const auto unchanged = apply(3U);
  ASSERT_TRUE(unchanged.ok()) << unchanged.error().message;
  EXPECT_EQ(unchanged.value().status,
            SceneRuntimePoseApplicationStatus::kUnchanged);
  EXPECT_EQ(unchanged.value().projection_revision, scene.projection);
  EXPECT_EQ(scene.commit_count, 1U);

  // Any other carrier-relative pose in the scene commits.
  AttachInSnapshot(scene, core::Pose3DTO{});
  const auto changed = apply(4U);
  ASSERT_TRUE(changed.ok()) << changed.error().message;
  EXPECT_EQ(changed.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 2U);
}

TEST(SceneRuntimePoseProjector,
     RelativePolicyRefusesAttachedUpdatesWithoutOneInstantReference) {
  struct Case final {
    CanonicalValue payload;
    std::string code;
  };
  const std::vector<Case> cases{
      {PosePayload(), "scene_runtime_pose_reference_missing"},
      {ReferencedPayload(false), "scene_runtime_pose_reference_not_coherent"},
      {ReferencedPayload(true, false),
       "scene_runtime_pose_reference_unavailable"},
  };
  for (const auto &entry : cases) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    AttachInSnapshot(scene);
    std::size_t calls = 0U;
    SceneRuntimePoseProjector projector(scene, cell_id,
                                        TcpAboveReference(calls));
    const auto result = projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, Observation(entry.payload),
        std::nullopt, "continuous_relative_to_attached");
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, entry.code);
    EXPECT_EQ(scene.commit_count, 0U);
    EXPECT_EQ(calls, 0U);
  }

  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  AttachInSnapshot(scene);
  SceneRuntimePoseProjector projector(
      scene, cell_id,
      [](const core::RetainedBundleId &, const core::ComponentSceneFrameRefDTO &,
         const std::string &) {
        return core::Result<SceneRuntimeReferenceLinkResolution>::Err(core::ErrorDTO{
            "scene_runtime_pose_reference_not_fixed", "finger joint", {}});
      });
  const auto unresolved = projector.Apply(
      std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
      scene.projection.graph_revision, Observation(ReferencedPayload()),
      std::nullopt, "continuous_relative_to_attached");
  ASSERT_FALSE(unresolved.ok());
  EXPECT_EQ(unresolved.error().code, "scene_runtime_pose_reference_not_fixed");
  EXPECT_EQ(scene.commit_count, 0U);
}

TEST(SceneRuntimePoseProjector, RelativePolicyIsUnattachedWorldUpdateWhileDetached) {
  for (const auto &payload : {PosePayload(), ReferencedPayload(false)}) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    std::size_t calls = 0U;
    SceneRuntimePoseProjector projector(scene, cell_id,
                                        TcpAboveReference(calls));
    const auto result = projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, Observation(payload), std::nullopt,
        "continuous_relative_to_attached");
    ASSERT_TRUE(result.ok()) << result.error().message;
    const auto &update = std::get<core::UpdateObjectPoseRequestDTO>(
        scene.last_request->operation);
    EXPECT_EQ(update.frame_ref,
              core::SceneTransactionFrameRefDTO{core::CellSceneFrameRefDTO{cell_id}});
    EXPECT_EQ(update.expected_attached, false);
    EXPECT_DOUBLE_EQ(update.pose.position_m.x_m, 0.25);
    EXPECT_EQ(calls, 0U);
  }
}

TEST(SceneRuntimePoseProjector, ExistingPoliciesRejectReferencedPayloadShapes) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const auto result =
      ApplyTo(projector, scene, {Provider(cell_id)},
              Observation(WithMember(PosePayload(), "reference",
                                     CanonicalValue(nullptr))));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "scene_runtime_pose_payload_invalid");
  // The referenced kind still carries the world pose for other policies.
  const auto world =
      ApplyTo(projector, scene, {Provider(cell_id)},
              Observation(ReferencedPayload()));
  ASSERT_TRUE(world.ok()) << world.error().message;
  EXPECT_DOUBLE_EQ(std::get<core::UpdateObjectPoseRequestDTO>(
                       scene.last_request->operation)
                       .pose.position_m.x_m,
                   0.25);
}

deployment::UrdfDocumentIr GripperModel() {
  deployment::UrdfDocumentIr urdf;
  for (const auto *name :
       {"base", "tool0", "gripper_base", "grasp", "finger"}) {
    urdf.links.push_back(deployment::UrdfLinkIr{name, std::nullopt, {}, {}});
  }
  const auto joint = [&](const char *name, const deployment::UrdfJointType type,
                         const char *parent, const char *child,
                         const deployment::ModelSourceVector3 xyz,
                         const deployment::ModelSourceVector3 rpy) {
    deployment::UrdfJointIr row{};
    row.name = name;
    row.type = type;
    row.parent_link_name = parent;
    row.child_link_name = child;
    row.origin.xyz_m = xyz;
    row.origin.rpy_rad = rpy;
    urdf.joints.push_back(std::move(row));
  };
  constexpr double kHalfTurn = 1.5707963267948966;
  joint("wrist", deployment::UrdfJointType::kRevolute, "base", "tool0",
        {0.0, 0.0, 1.0}, {0.0, 0.0, 0.0});
  joint("mount", deployment::UrdfJointType::kFixed, "tool0", "gripper_base",
        {0.1, 0.0, 0.0}, {0.0, 0.0, kHalfTurn});
  joint("grasp", deployment::UrdfJointType::kFixed, "gripper_base", "grasp",
        {0.0, 0.0, 0.2}, {0.0, 0.0, 0.0});
  joint("finger", deployment::UrdfJointType::kRevolute, "gripper_base",
        "finger", {0.0, 0.03, 0.05}, {0.0, 0.0, 0.0});
  return urdf;
}

// A grasp-lost Stop names the holder and every component it is
// mounted on, in the Stop selector's id order. On lite6 + lite_gripper a loss
// at the gripper also stops the arm (before: the gripper alone, so an arm
// move was not stopped: kAlreadyTerminalNoEffect).
TEST(SceneRuntimePoseProjector, CarrierStopScopesTheHolderAndEverythingItIsMountedOn) {
  // The graph DTOs hold strong ids without default constructors.
  const auto component = [](const char *id) {
    return core::ControllableComponentDTO{core::ControllableComponentId(id),
                                          core::ComponentTypeId("type"),
                                          core::ComponentTypeRevision("1"),
                                          {},
                                          {},
                                          std::nullopt,
                                          std::nullopt};
  };
  const auto mount = [](const char *id, const char *parent, const char *child) {
    return core::ComponentAttachmentEdgeDTO{
        core::ComponentAttachmentId(id),
        core::ComponentAttachmentRevision("1"),
        {core::ControllableComponentId(parent), core::AttachmentInterfaceId("flange")},
        {core::ControllableComponentId(child), core::AttachmentInterfaceId("mount")},
        {},
        false};
  };
  core::CellGraphDTO graph{"manyforge.cell_graph.v1",
                           core::GraphRevision("graph-1"),
                           core::CellId("cell"),
                           {},
                           {},
                           {component("lite6_robot"), component("lite_gripper"),
                            component("ur5_robot")},
                           {},
                           {},
                           {mount("lite6_flange", "lite6_robot", "lite_gripper")}};
  using Ids = std::vector<core::ControllableComponentId>;
  EXPECT_EQ(SceneRuntimeCarrierStopComponents(graph, core::ControllableComponentId("lite_gripper")),
            (Ids{core::ControllableComponentId("lite6_robot"),
                 core::ControllableComponentId("lite_gripper")}));
  // A holder mounted on nothing (a fixed gripper, or the arm itself) stops alone.
  EXPECT_EQ(SceneRuntimeCarrierStopComponents(graph, core::ControllableComponentId("ur5_robot")),
            (Ids{core::ControllableComponentId("ur5_robot")}));
  EXPECT_EQ(SceneRuntimeCarrierStopComponents(graph, core::ControllableComponentId("lite6_robot")),
            (Ids{core::ControllableComponentId("lite6_robot")}));
  // A chained mount stops the whole chain.
  graph.controllable_components.push_back(component("a_nozzle"));
  graph.component_attachments.push_back(mount("gripper_tip", "lite_gripper", "a_nozzle"));
  EXPECT_EQ(SceneRuntimeCarrierStopComponents(graph, core::ControllableComponentId("a_nozzle")),
            (Ids{core::ControllableComponentId("a_nozzle"),
                 core::ControllableComponentId("lite6_robot"),
                 core::ControllableComponentId("lite_gripper")}));
}

TEST(SceneRuntimePoseProjector, FixedLinkTransformFollowsOnlyFixedJoints) {
  const auto urdf = GripperModel();
  const auto direct = SceneRuntimeFixedLinkTransform(urdf, "gripper_base", "grasp");
  ASSERT_TRUE(direct.ok()) << direct.error().message;
  EXPECT_NEAR(direct.value().position_m.z_m, 0.2, 1e-12);
  const auto inverse = SceneRuntimeFixedLinkTransform(urdf, "grasp", "gripper_base");
  ASSERT_TRUE(inverse.ok()) << inverse.error().message;
  EXPECT_NEAR(inverse.value().position_m.z_m, -0.2, 1e-12);
  // tool0 -> grasp composes the mount yaw: grasp sits 0.2 m along tool0 z.
  const auto composed = SceneRuntimeFixedLinkTransform(urdf, "tool0", "grasp");
  ASSERT_TRUE(composed.ok()) << composed.error().message;
  EXPECT_NEAR(composed.value().position_m.x_m, 0.1, 1e-12);
  EXPECT_NEAR(composed.value().position_m.y_m, 0.0, 1e-12);
  EXPECT_NEAR(composed.value().position_m.z_m, 0.2, 1e-12);
  EXPECT_NEAR(composed.value().orientation.z, std::sqrt(0.5), 1e-12);
  EXPECT_NEAR(composed.value().orientation.w, std::sqrt(0.5), 1e-12);
  const auto same = SceneRuntimeFixedLinkTransform(urdf, "grasp", "grasp");
  ASSERT_TRUE(same.ok());
  EXPECT_EQ(same.value(), core::Pose3DTO{});

  for (const auto &[from, to] :
       {std::pair{"base", "grasp"}, std::pair{"finger", "grasp"}}) {
    const auto moving = SceneRuntimeFixedLinkTransform(urdf, from, to);
    ASSERT_FALSE(moving.ok());
    EXPECT_EQ(moving.error().code, "scene_runtime_pose_reference_not_fixed");
  }
  const auto unknown = SceneRuntimeFixedLinkTransform(urdf, "camera", "grasp");
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, "scene_runtime_pose_reference_link_unknown");
}

core::CarriedObjectGraspLostCheckDTO GraspCheck() { return {{0.03, 0.26}}; }

core::Pose3DTO YawPose(const double x, const double yaw) {
  return {{x, 0.0, 0.05}, {0.0, 0.0, std::sin(yaw / 2.0), std::cos(yaw / 2.0)}};
}

// Records each Stop request with the scene's commit count at that moment.
struct StopRecorder final {
  std::vector<SceneRuntimeCarriedObjectLoss> requests;
  std::vector<std::size_t> commits_before;
  SceneRuntimeCarrierStopRequester Requester(const FakeSceneAuthority &scene) {
    return [this, &scene](const SceneRuntimeCarriedObjectLoss &loss) {
      requests.push_back(loss);
      commits_before.push_back(scene.commit_count);
      return std::optional<core::ErrorDTO>{};
    };
  }
};

void ExpectLostStoppedThenDetached(
    const core::Result<SceneRuntimePoseApplicationResult> &result,
    const FakeSceneAuthority &scene, const StopRecorder &stops,
    const core::Pose3DTO &observed_in_cell,
    const std::size_t prior_commits = 0U) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "carried_object_lost");
  EXPECT_EQ(result.error().details.at("stop"), "requested");
  EXPECT_EQ(result.error().details.at("detach"), "true");
  EXPECT_EQ(result.error().details.at("detach_result"), "committed");
  EXPECT_NE(result.error().message.find("box moved "), std::string::npos);
  EXPECT_NE(result.error().message.find("it was detached and the motion stopped"),
            std::string::npos);
  // Stop first, before any scene change; then exactly one commit: the detach
  // at the observed cell pose. The update itself is not applied.
  ASSERT_EQ(stops.requests.size(), 1U);
  EXPECT_EQ(stops.commits_before.front(), prior_commits);
  EXPECT_EQ(stops.requests.front().controllable_component_id,
            kCarrier.controllable_component_id);
  EXPECT_EQ(stops.requests.front().carried_object_attachment_id,
            core::CarriedObjectAttachmentId("attachment.1"));
  EXPECT_EQ(scene.commit_count, prior_commits + 1U);
  ASSERT_TRUE(scene.detached_at.has_value());
  EXPECT_EQ(*scene.detached_at, observed_in_cell);
  const auto &detach = std::get<core::DetachCarriedObjectRequestDTO>(
      scene.last_request->operation);
  EXPECT_EQ(detach.frame_ref,
            core::SceneTransactionFrameRefDTO{core::CellSceneFrameRefDTO{
                core::CellId("cell.1")}});
}

const core::Pose3DTO kObservedCellPose{{0.25, -0.5, 0.75}, {0.0, 0.0, 0.0, 1.0}};

TEST(SceneRuntimePoseProjector, RelativeGraspLostComparesWithTheFirstMeasuredOffset) {
  // Live 2026-10-26: the stream's carried offset differed from ManyForge's
  // attach-time offset by a fixed 180° plus ~25 mm of real grasp error. That
  // constant difference is not a loss; only drift from the first measured
  // offset (0.02, 0, 0.05), yaw 0, is.
  struct Case final {
    double x;
    double yaw;
    bool lost;
  };
  for (const auto &later : {Case{0.02, 0.0, false}, Case{0.04, 0.2, false},
                            Case{0.06, 0.0, true}, Case{0.02, 0.3, true}}) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    const auto attach_offset = YawPose(-0.006, std::acos(-1.0));
    AttachInSnapshot(scene, attach_offset, GraspCheck());
    std::size_t calls = 0U;
    StopRecorder stops;
    SceneRuntimePoseProjector projector(scene, cell_id, TcpAboveReference(calls),
                                        stops.Requester(scene));
    const auto apply = [&](const std::uint64_t sequence, const double x,
                           const double yaw) {
      // The fake's snapshot follows each committed projection revision.
      AttachInSnapshot(scene, attach_offset, GraspCheck());
      return projector.Apply(
          std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
          scene.projection.graph_revision,
          Newer(Observation(ReferencedPayload(true, true, x, yaw)), sequence),
          std::nullopt, "continuous_relative_to_attached");
    };
    for (const auto sequence : {2U, 3U}) {
      const auto baseline = apply(sequence, 0.02, 0.0);
      ASSERT_TRUE(baseline.ok()) << baseline.error().message;
      EXPECT_EQ(baseline.value().status,
                SceneRuntimePoseApplicationStatus::kCommitted);
    }
    EXPECT_TRUE(stops.requests.empty());
    const auto result = apply(4U, later.x, later.yaw);
    if (!later.lost) {
      ASSERT_TRUE(result.ok()) << result.error().message;
      EXPECT_TRUE(stops.requests.empty());
      continue;
    }
    ExpectLostStoppedThenDetached(result, scene, stops, kObservedCellPose, 2U);
  }
}

TEST(SceneRuntimePoseProjector, LostSampleReappliedForPlanningRetainsEvidence) {
  // Live 2026-10-26 (r10-E): the cheese fell at the end of a motion; its loss
  // sample detached it and returned carried_object_lost, so that sample had
  // no application. The next node's before-planning latch now applies the
  // same sample again with the effective policy: the object is a world object
  // at exactly the observed pose, which is retained planning evidence.
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  const auto attach_offset = YawPose(0.02, 0.0);
  AttachInSnapshot(scene, attach_offset, GraspCheck());
  std::size_t calls = 0U;
  StopRecorder stops;
  SceneRuntimePoseProjector projector(scene, cell_id, TcpAboveReference(calls),
                                      stops.Requester(scene));
  const auto apply = [&](const PoseSourceObservation &observation) {
    return projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, observation, std::nullopt,
        "continuous_relative_to_attached");
  };
  const auto baseline =
      apply(Newer(Observation(ReferencedPayload(true, true, 0.02, 0.0)), 2U));
  ASSERT_TRUE(baseline.ok()) << baseline.error().message;
  AttachInSnapshot(scene, attach_offset, GraspCheck());
  const auto lost_sample =
      Newer(Observation(ReferencedPayload(true, true, 0.08, 0.0)), 3U);
  ExpectLostStoppedThenDetached(apply(lost_sample), scene, stops,
                                kObservedCellPose, 1U);
  const auto reapplied = apply(lost_sample);
  ASSERT_TRUE(reapplied.ok()) << reapplied.error().message;
  EXPECT_EQ(reapplied.value().status,
            SceneRuntimePoseApplicationStatus::kUnchanged);
  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      reapplied.value().status));
  EXPECT_EQ(stops.requests.size(), 1U);
  EXPECT_EQ(scene.commit_count, 2U);
}

TEST(SceneRuntimePoseProjector, FailedDetachKeepsLossBaselineOnReplay) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  const auto attach_offset = YawPose(0.02, 0.0);
  AttachInSnapshot(scene, attach_offset, GraspCheck());
  std::size_t calls = 0U;
  StopRecorder stops;
  SceneRuntimePoseProjector projector(scene, cell_id, TcpAboveReference(calls),
                                      stops.Requester(scene));
  const auto apply = [&](const PoseSourceObservation &observation) {
    return projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, observation, std::nullopt,
        "continuous_relative_to_attached");
  };
  ASSERT_TRUE(apply(Newer(Observation(ReferencedPayload(true, true, 0.02)), 2U)).ok());
  AttachInSnapshot(scene, attach_offset, GraspCheck());
  scene.failure = core::ErrorDTO{"detach_refused", "detach refused", {}};
  const auto lost_sample =
      Newer(Observation(ReferencedPayload(true, true, 0.08)), 3U);
  const auto first = apply(lost_sample);
  ASSERT_FALSE(first.ok());
  EXPECT_EQ(first.error().code, "carried_object_lost");
  EXPECT_EQ(first.error().details.at("detach"), "true");
  EXPECT_EQ(first.error().details.at("detach_result"), "detach_refused");
  EXPECT_TRUE(scene.state.attached);

  // Retrying the same out-of-bounds observation must not re-baseline it.
  const auto replay = apply(lost_sample);
  ASSERT_FALSE(replay.ok());
  EXPECT_EQ(replay.error().code, "carried_object_lost");
  EXPECT_EQ(replay.error().details.at("detach"), "true");
  EXPECT_EQ(replay.error().details.at("detach_result"), "detach_refused");
  EXPECT_TRUE(scene.state.attached);
  EXPECT_EQ(stops.requests.size(), 2U);
}

TEST(SceneRuntimePoseProjector, ContinuousGraspLostComparesWithTheFirstMeasuredOffset) {
  // The carrier sits at (0.25, -0.5, 0.75); the attachment's stored offset is
  // identity, but the first sample measures (0.05, 0, 0): that is the
  // baseline, not a loss.
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  scene.pose = core::Pose3DTO{{0.25, -0.5, 0.75}, {}};
  AttachInSnapshot(scene, {}, GraspCheck());
  StopRecorder stops;
  SceneRuntimePoseProjector projector(scene, cell_id, {}, stops.Requester(scene));
  const auto apply = [&](const std::uint64_t sequence, const double x) {
    return projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision,
        Newer(Observation(PosePayload(x)), sequence), std::nullopt, "continuous");
  };
  const auto first = apply(2U, 0.30);
  ASSERT_TRUE(first.ok()) << first.error().message;
  EXPECT_EQ(first.value().status, SceneRuntimePoseApplicationStatus::kCommitted);
  // As the scene owner does: the committed offset is the measured one.
  AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, GraspCheck());
  const auto within = apply(3U, 0.31);
  ASSERT_TRUE(within.ok()) << within.error().message;
  EXPECT_TRUE(stops.requests.empty());
  AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, GraspCheck());
  const auto lost = apply(4U, 0.40);
  ExpectLostStoppedThenDetached(
      lost, scene, stops, core::Pose3DTO{{0.40, -0.5, 0.75}, {0.0, 0.0, 0.0, 1.0}},
      2U);
  // Afterwards the object is an ordinary world object, and its baseline is
  // gone: a new attachment re-baselines on its first sample.
  const auto world = apply(5U, 0.35);
  ASSERT_TRUE(world.ok()) << world.error().message;
  EXPECT_EQ(world.value().status, SceneRuntimePoseApplicationStatus::kCommitted);
  scene.pose = core::Pose3DTO{{0.25, -0.5, 0.75}, {}};
  AttachInSnapshot(scene, {}, GraspCheck());
  const auto reattached = apply(6U, 0.40);
  ASSERT_TRUE(reattached.ok()) << reattached.error().message;
  EXPECT_EQ(stops.requests.size(), 1U);
}

TEST(SceneRuntimePoseProjector, CarriedObjectLostTextComesFromItsErrorRow) {
  EXPECT_EQ(core::kCarriedObjectLostRow.code, "scene.carried_object_lost");
  EXPECT_EQ(core::kCarriedObjectLostRow.compact_text, "Carried object lost");
  EXPECT_EQ(core::CarriedObjectLostFullText("box", 0.052, 0.0523599, 0.03, 0.26),
            "box moved 52.0 mm / 3.0° from its grasp pose (bound 30.0 mm / "
            "14.9°); it was detached and the motion stopped.");
  EXPECT_EQ(
      core::CarriedObjectLostFullText("box", 0.052, 0.0523599, 0.03, 0.26, false),
      "box moved 52.0 mm / 3.0° from its grasp pose (bound 30.0 mm / "
      "14.9°); it was detached.");
  EXPECT_EQ(core::CarriedObjectLostFullText("box", 0.052, 0.0523599, 0.03, 0.26,
                                            true, false),
            "box moved 52.0 mm / 3.0° from its grasp pose (bound 30.0 mm / "
            "14.9°); the motion stopped.");
  EXPECT_EQ(core::CarriedObjectLostFullText("box", 0.052, 0.0523599, 0.03, 0.26,
                                            false, false),
            "box moved 52.0 mm / 3.0° from its grasp pose (bound 30.0 mm / "
            "14.9°).");
}

// Records each signal publication.
struct SignalRecorder final {
  std::vector<SceneRuntimeCarriedObjectLoss> published;
  SceneRuntimeCarriedObjectLostSignalPublisher Publisher() {
    return [this](const SceneRuntimeCarriedObjectLoss &loss) {
      published.push_back(loss);
      return std::optional<core::ErrorDTO>{};
    };
  }
};

TEST(SceneRuntimePoseProjector, OnGraspLostFlagsDriveSignalStopDetachAndAlarm) {
  // Every combination the attach node allows (an alarm forces the Stop): the
  // signal is always published; Stop and detach follow their flags; the
  // alarm selector and the key travel in the details.
  const std::vector<core::GraspLostResponseDTO> responses{
      {true, true, std::nullopt, true},    {true, true, std::nullopt, false},
      {false, true, std::nullopt, true},   {false, true, "part_lost", false},
      {false, false, "part_lost", true},   {false, false, std::nullopt, false}};
  for (const auto &response : responses) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    scene.pose = core::Pose3DTO{{0.25, -0.5, 0.75}, {}};
    auto check = GraspCheck();
    if (!response.is_default())
      check.on_grasp_lost = response;
    AttachInSnapshot(scene, {}, check);
    StopRecorder stops;
    SignalRecorder signals;
    SceneRuntimePoseProjector projector(scene, cell_id, {},
                                        stops.Requester(scene),
                                        signals.Publisher());
    const auto apply = [&](const std::uint64_t sequence, const double x) {
      return projector.Apply(
          std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
          scene.projection.graph_revision,
          Newer(Observation(PosePayload(x)), sequence), std::nullopt,
          "continuous");
    };
    const auto first = apply(2U, 0.30);
    ASSERT_TRUE(first.ok()) << first.error().message;
    AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, check);
    const auto lost = apply(3U, 0.40);
    ASSERT_FALSE(lost.ok());
    const auto &details = lost.error().details;
    EXPECT_EQ(lost.error().code, "carried_object_lost");
    ASSERT_EQ(signals.published.size(), 1U);
    EXPECT_EQ(signals.published.front().command_target_id,
              core::CommandTargetId("arm"));
    EXPECT_EQ(signals.published.front().carried_object_attachment_id,
              core::CarriedObjectAttachmentId("attachment.1"));
    EXPECT_EQ(details.at("signal"), "published");
    // The loss event id and the stored key reach the host row.
    EXPECT_EQ(signals.published.front().loss_event_id, details.at("event_id"));
    EXPECT_EQ(signals.published.front().set_key, response.set_key);
    EXPECT_EQ(details.at("command_target_id"), "arm");
    EXPECT_EQ(details.at("raise_alarm"), response.raise_alarm ? "true" : "false");
    EXPECT_EQ(details.at("stop_motion"), response.stop_motion ? "true" : "false");
    EXPECT_EQ(details.at("detach"), response.detach ? "true" : "false");
    EXPECT_EQ(details.contains("set_key"), response.set_key.has_value());
    EXPECT_EQ(stops.requests.size(), response.stop_motion ? 1U : 0U);
    EXPECT_EQ(details.contains("stop"), response.stop_motion);
    EXPECT_EQ(scene.detached_at.has_value(), response.detach);
    EXPECT_EQ(details.contains("detach_result"), response.detach);
    EXPECT_EQ(lost.error().message.find("the motion stopped") != std::string::npos,
              response.stop_motion);
    EXPECT_EQ(lost.error().message.find("it was detached") != std::string::npos,
              response.detach);
    if (!response.detach) {
      // Kept attached: the loss fires once for this attachment.
      AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, check);
      const auto again = apply(4U, 0.45);
      ASSERT_TRUE(again.ok()) << again.error().message;
      EXPECT_EQ(signals.published.size(), 1U);
      EXPECT_EQ(stops.requests.size(), response.stop_motion ? 1U : 0U);
    }
  }
}

TEST(SceneRuntimePoseProjector, FailedSignalPublicationFailsClosedOncePerLoss) {
  // A loss whose `true` cannot be published (Manager transport, or
  // the host refusing it) is a component fault: alarm and Stop whatever
  // `on_grasp_lost` says, the typed error in the details; like any loss it
  // fires once (no retry), and the detach still follows its flag.
  for (const std::string code :
       {"carried_object_lost_signal_unavailable", "io_signal_service_unavailable"}) {
    for (const bool detach : {false, true}) {
      const core::CellId cell_id("cell.1");
      FakeSceneAuthority scene;
      scene.pose = core::Pose3DTO{{0.25, -0.5, 0.75}, {}};
      auto check = GraspCheck();
      check.on_grasp_lost = core::GraspLostResponseDTO{false, false, std::nullopt, detach};
      AttachInSnapshot(scene, {}, check);
      StopRecorder stops;
      std::size_t publications = 0U;
      SceneRuntimePoseProjector projector(
          scene, cell_id, {}, stops.Requester(scene),
          [&](const SceneRuntimeCarriedObjectLoss &) {
            ++publications;
            return std::optional(core::ErrorDTO{code, "refused", {}});
          });
      const auto apply = [&](const std::uint64_t sequence, const double x) {
        return projector.Apply(
            std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
            scene.projection.graph_revision,
            Newer(Observation(PosePayload(x)), sequence), std::nullopt,
            "continuous");
      };
      ASSERT_TRUE(apply(2U, 0.30).ok());
      AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, check);
      const auto lost = apply(3U, 0.40);
      ASSERT_FALSE(lost.ok());
      const auto &details = lost.error().details;
      EXPECT_EQ(details.at("signal"), code);
      EXPECT_EQ(details.at("signal_error"), code);
      EXPECT_EQ(details.at("raise_alarm"), "true");
      EXPECT_EQ(details.at("stop_motion"), "true");
      EXPECT_EQ(stops.requests.size(), 1U);
      EXPECT_EQ(scene.detached_at.has_value(), detach);
      if (!detach) {
        // Kept attached: the next sample raises nothing again.
        AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, check);
        const auto again = apply(4U, 0.45);
        ASSERT_TRUE(again.ok()) << again.error().message;
        EXPECT_EQ(publications, 1U);
        EXPECT_EQ(stops.requests.size(), 1U);
      }
    }
  }
}

TEST(SceneRuntimePoseProjector, TargetWithoutSignalFollowsItsFlags) {
  // A grasp check on a target without the system signal (not a gripper
  // target: the host answers `io_signal_unbound`) is not a failure: the
  // configured flags apply as before.
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  scene.pose = core::Pose3DTO{{0.25, -0.5, 0.75}, {}};
  auto check = GraspCheck();
  check.on_grasp_lost = core::GraspLostResponseDTO{false, false, std::nullopt, false};
  AttachInSnapshot(scene, {}, check);
  StopRecorder stops;
  SceneRuntimePoseProjector projector(
      scene, cell_id, {}, stops.Requester(scene),
      [](const SceneRuntimeCarriedObjectLoss &) {
        return std::optional(core::ErrorDTO{"io_signal_unbound", "no signal", {}});
      });
  const auto apply = [&](const std::uint64_t sequence, const double x) {
    return projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision,
        Newer(Observation(PosePayload(x)), sequence), std::nullopt, "continuous");
  };
  ASSERT_TRUE(apply(2U, 0.30).ok());
  AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, check);
  const auto lost = apply(3U, 0.40);
  ASSERT_FALSE(lost.ok());
  const auto &details = lost.error().details;
  EXPECT_EQ(details.at("signal"), "not_applicable");
  EXPECT_FALSE(details.contains("signal_error"));
  EXPECT_EQ(details.at("raise_alarm"), "false");
  EXPECT_EQ(details.at("stop_motion"), "false");
  EXPECT_TRUE(stops.requests.empty());
  EXPECT_FALSE(scene.detached_at.has_value());
}

TEST(SceneRuntimePoseProjector, RefusedDetachKeepsTheGraspBaseline) {
  // A refused detach leaves the attachment and its baseline, so the
  // next sample detects the loss again and retries the detach.
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  scene.pose = core::Pose3DTO{{0.25, -0.5, 0.75}, {}};
  AttachInSnapshot(scene, {}, GraspCheck());
  StopRecorder stops;
  SignalRecorder signals;
  SceneRuntimePoseProjector projector(scene, cell_id, {}, stops.Requester(scene),
                                      signals.Publisher());
  const auto apply = [&](const std::uint64_t sequence, const double x) {
    return projector.Apply(
        std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
        scene.projection.graph_revision,
        Newer(Observation(PosePayload(x)), sequence), std::nullopt, "continuous");
  };
  ASSERT_TRUE(apply(2U, 0.30).ok());
  AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, GraspCheck());
  scene.detach_refusals = 1U;
  const auto refused = apply(3U, 0.40);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("detach_result"),
            "scene_object_state_conflict");
  EXPECT_FALSE(scene.detached_at.has_value());
  // Still attached, same baseline: the next sample is lost again (not a new
  // baseline) and this time the detach commits.
  AttachInSnapshot(scene, core::Pose3DTO{{0.05, 0.0, 0.0}, {}}, GraspCheck());
  const auto retried = apply(4U, 0.41);
  ASSERT_FALSE(retried.ok());
  EXPECT_EQ(retried.error().code, "carried_object_lost");
  EXPECT_EQ(retried.error().details.at("detach_result"), "committed");
  ASSERT_TRUE(scene.detached_at.has_value());
  EXPECT_EQ(signals.published.size(), 2U);
}

TEST(SceneRuntimePoseProjector, GraspLostCheckOnlyForMeasuredOffsetPolicies) {
  // Without a check every update applies; between_nodes is not checked.
  for (const auto *policy : {"continuous", "between_nodes"}) {
    for (const bool checked : {false, true}) {
      if (checked && std::string_view(policy) == "continuous")
        continue;
      const core::CellId cell_id("cell.1");
      FakeSceneAuthority scene;
      AttachInSnapshot(scene, {},
                       checked ? std::optional(GraspCheck()) : std::nullopt);
      SceneRuntimePoseProjector projector(scene, cell_id);
      const auto result = projector.Apply(
          std::vector{Provider(cell_id)}, scene.projection.retained_bundle_id,
          scene.projection.graph_revision, Observation(PosePayload(1.8)),
          std::nullopt, policy);
      ASSERT_TRUE(result.ok()) << result.error().message;
      EXPECT_EQ(result.value().status,
                SceneRuntimePoseApplicationStatus::kCommitted);
    }
  }
}

TEST(SceneRuntimePoseProjector, DeadbandTreatsSmallSameOriginChangesAsUnchanged) {
  struct Case final {
    SceneRuntimePoseDeadband deadband;
    CanonicalValue payload;
    SceneRuntimePoseApplicationStatus status;
  };
  const auto yaw = [](const double angle) {
    return PosePayload(0.25, std::cos(angle / 2.0), 1250000000,
                       std::sin(angle / 2.0));
  };
  const std::vector<Case> cases{
      // Default 0: any change commits, as before.
      {{}, PosePayload(0.2501), SceneRuntimePoseApplicationStatus::kCommitted},
      {{}, yaw(0.0001), SceneRuntimePoseApplicationStatus::kCommitted},
      {{0.001, 0.0}, PosePayload(0.2505),
       SceneRuntimePoseApplicationStatus::kUnchanged},
      {{0.001, 0.0}, PosePayload(0.252),
       SceneRuntimePoseApplicationStatus::kCommitted},
      // A zero rotation deadband keeps the orientation exact.
      {{0.001, 0.0}, yaw(0.0001), SceneRuntimePoseApplicationStatus::kCommitted},
      {{0.0, 0.001}, yaw(0.0005), SceneRuntimePoseApplicationStatus::kUnchanged},
      {{0.0, 0.001}, yaw(0.002), SceneRuntimePoseApplicationStatus::kCommitted},
  };
  for (const auto &entry : cases) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    SceneRuntimePoseProjector projector(scene, cell_id);
    const std::vector providers{Provider(cell_id)};
    const auto apply = [&](const PoseSourceObservation &observation) {
      return projector.Apply(providers, scene.projection.retained_bundle_id,
                             scene.projection.graph_revision, observation,
                             std::nullopt, "continuous_unattached",
                             entry.deadband);
    };
    ASSERT_TRUE(apply(Observation()).ok());
    ASSERT_EQ(scene.commit_count, 1U);
    const auto result = apply(Newer(Observation(entry.payload), 3U));
    ASSERT_TRUE(result.ok()) << result.error().message;
    EXPECT_EQ(result.value().status, entry.status);
    EXPECT_EQ(scene.commit_count,
              entry.status == SceneRuntimePoseApplicationStatus::kCommitted
                  ? 2U
                  : 1U);
  }
}

} // namespace
} // namespace manyforge::headless
