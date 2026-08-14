#include "manyforge_headless/scene_runtime_pose_projector.hpp"

#include <gtest/gtest.h>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;

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
                           const double quaternion_w = 1.0) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.ros_pose_observation.v1")},
      {"kind", CanonicalValue("simulation_entity_pose")},
      {"pose", CanonicalValue(CanonicalValue::Object{
                   {"frame_id", CanonicalValue("universe")},
                   {"frame_kind", CanonicalValue("cell_frame")},
                   {"orientation_xyzw", CanonicalValue(CanonicalValue::Array{
                                            CanonicalValue(0.0),
                                            CanonicalValue(0.0),
                                            CanonicalValue(0.0),
                                            CanonicalValue(quaternion_w),
                                        })},
                   {"position_m", CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(x),
                                      CanonicalValue(-0.5),
                                      CanonicalValue(0.75),
                                  })},
                   {"source_stamp_s", CanonicalValue(1.25)},
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
    return core::RequestOwnerEpoch("scene-owner.1");
  }

  core::ProjectionRevisionDTO CurrentProjectionRevision() const override {
    return projection;
  }

  std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  LogicalSnapshot() const override {
    return nullptr;
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
  std::optional<core::ErrorDTO> failure;
  std::optional<core::SceneTransactionRequestDTO> last_request;
  std::size_t commit_count{};
};

TEST(SceneRuntimePoseProjector,
     CommitsExactProviderEvidenceAndRecognizesDurableReplay) {
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
  EXPECT_EQ(update.provider_evidence->provider_source_sequence, 2U);
  EXPECT_EQ(update.provider_evidence->received_at_ns, 100);
  EXPECT_EQ(update.provider_evidence->source_timestamp_ns, 1250000000);
  EXPECT_EQ(update.provider_evidence->source_freshness_budget_ns, 900);
  EXPECT_TRUE(content_identity::is_sha256_digest(
      update.provider_evidence->provider_source_epoch));

  const auto already_applied =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, observation);
  ASSERT_TRUE(already_applied.ok()) << already_applied.error().message;
  EXPECT_EQ(already_applied.value().status,
            SceneRuntimePoseApplicationStatus::kAlreadyApplied);
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

TEST(SceneRuntimePoseProjector,
     FailsClosedOnStaleEvidenceAndSceneOwnerRejection) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  const auto observation = Observation();
  const auto initial =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, observation);
  ASSERT_TRUE(initial.ok());

  scene.state.provider_evidence->provider_source_sequence = 3U;
  const auto stale =
      projector.Apply(providers, scene.projection.retained_bundle_id,
                      scene.projection.graph_revision, observation);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "scene_runtime_pose_observation_stale");

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

} // namespace
} // namespace manyforge::headless
