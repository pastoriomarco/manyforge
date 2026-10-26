#include "manyforge_headless/scene_runtime_pose_projector.hpp"

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
                           const std::int64_t source_stamp_ns = 1250000000) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.ros_pose_observation.v2")},
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
    if (compaction_refusals > 0U) {
      --compaction_refusals;
      const auto previous = owner_epoch;
      owner_epoch += ".next";
      std::map<std::string, std::string> details{
          {"expected_request_owner_epoch", owner_epoch}};
      if (compaction_proof) {
        details.emplace("reason", "scene_history_compaction_no_effect");
        details.emplace("previous_request_owner_epoch", previous);
      }
      return {request.request_context.request_id,
              core::RequestReplayStatusDTO::kStaleRequestOwnerEpoch, false,
              core::ErrorDTO{"stale_request_owner_epoch", "epoch rotated", details},
              std::nullopt, std::nullopt,
              core::SceneProjectionCommitStatusDTO::kNotAttempted, std::nullopt};
    }
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

  core::Result<core::ProjectionRevisionDTO> ConfirmProviderFreshness(
      const core::ProviderFreshnessConfirmationDTO &confirmation) override {
    ++confirmation_count;
    last_confirmation = confirmation;
    if (confirmation_error.has_value()) {
      return core::Result<core::ProjectionRevisionDTO>::Err(
          *confirmation_error);
    }
    return core::Result<core::ProjectionRevisionDTO>::Ok(projection);
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
  std::size_t commit_count{};
  std::string owner_epoch{"scene-owner.1"};
  std::size_t compaction_refusals{};
  bool compaction_proof{true};
  std::optional<core::ErrorDTO> confirmation_error;
  std::optional<core::ProviderFreshnessConfirmationDTO> last_confirmation;
  std::size_t confirmation_count{};
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
     RetriesOnlyOneProvenCompactionWithANewRequestIdentity) {
  for (const auto refusals : {1U, 2U}) {
    FakeSceneAuthority scene;
    scene.compaction_refusals = refusals;
    const core::CellId cell_id("cell.1");
    SceneRuntimePoseProjector projector(scene, cell_id);
    const auto result = ApplyTo(projector, scene, {Provider(cell_id)}, Observation());
    EXPECT_EQ(result.ok(), refusals == 1U);
    EXPECT_EQ(scene.commit_count, 2U);
    ASSERT_TRUE(scene.last_request.has_value());
    EXPECT_TRUE(scene.last_request->request_context.request_id.value().ends_with(
        ".after-compaction"));
    EXPECT_EQ(scene.last_request->request_context.request_owner_epoch.value(),
              "scene-owner.1.next");
  }
}

TEST(SceneRuntimePoseProjector, DoesNotRetryUnprovenOldEpochRefusal) {
  FakeSceneAuthority scene;
  scene.compaction_refusals = 1U;
  scene.compaction_proof = false;
  const core::CellId cell_id("cell.1");
  SceneRuntimePoseProjector projector(scene, cell_id);
  const auto result = ApplyTo(projector, scene, {Provider(cell_id)}, Observation());
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(scene.commit_count, 1U);
}

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

TEST(SceneRuntimePoseProjector, LongServicePoseEvidenceRemainsExactAndIdempotent) {
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
  EXPECT_EQ(update.provider_evidence->received_at_ns, observation.observed_at_ns);
  EXPECT_EQ(update.provider_evidence->source_freshness_budget_ns, 900);
  EXPECT_EQ(update.provider_evidence->source_timestamp_ns, years31 - 1);
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

TEST(SceneRuntimePoseProjector,
     SafePointSnapshotsHaveNoContinuousRuntimeExpiry) {
  for (const auto *policy : {"before_planning", "between_nodes"}) {
    const core::CellId cell_id("cell.1");
    FakeSceneAuthority scene;
    SceneRuntimePoseProjector projector(scene, cell_id);
    const std::vector providers{Provider(cell_id)};
    const auto result = projector.Apply(
        providers, scene.projection.retained_bundle_id,
        scene.projection.graph_revision, Observation(), std::nullopt, policy);
    ASSERT_TRUE(result.ok()) << result.error().message;
    const auto &update = std::get<core::UpdateObjectPoseRequestDTO>(
        scene.last_request->operation);
    ASSERT_TRUE(update.provider_evidence);
    EXPECT_FALSE(update.provider_evidence->source_timestamp_ns);
    EXPECT_FALSE(update.provider_evidence->source_freshness_budget_ns);
  }
}

TEST(SceneRuntimePoseProjector,
     FreshSafePointUnchangedPoseDoesNotCommitAcrossSamePolicyClass) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  const auto apply = [&](const PoseSourceObservation &observation,
                         const std::string_view policy) {
    return projector.Apply(providers, scene.projection.retained_bundle_id,
                           scene.projection.graph_revision, observation,
                           std::nullopt, policy);
  };

  const auto continuous = apply(Observation(), "continuous");
  ASSERT_TRUE(continuous.ok()) << continuous.error().message;
  ASSERT_EQ(scene.commit_count, 1U);
  ASSERT_TRUE(scene.state.provider_evidence.has_value());
  ASSERT_TRUE(scene.state.provider_evidence->source_freshness_budget_ns);

  // Changing from a freshness-tracked continuous policy to a safe-point
  // snapshot still commits, so the retained evidence loses continuous expiry.
  const auto safe_point = apply(Newer(Observation(), 3U), "between_nodes");
  ASSERT_TRUE(safe_point.ok()) << safe_point.error().message;
  EXPECT_EQ(safe_point.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  ASSERT_EQ(scene.commit_count, 2U);
  ASSERT_TRUE(scene.state.provider_evidence.has_value());
  EXPECT_FALSE(scene.state.provider_evidence->source_timestamp_ns);
  EXPECT_FALSE(scene.state.provider_evidence->source_freshness_budget_ns);
  const auto safe_point_projection = scene.projection;

  // A newer fresh observation at a safe point with the same provider origin
  // and exact detached pose is already represented by Scene truth.
  const auto unchanged = apply(Newer(Observation(), 4U), "before_planning");
  ASSERT_TRUE(unchanged.ok()) << unchanged.error().message;
  EXPECT_EQ(unchanged.value().status,
            SceneRuntimePoseApplicationStatus::kAlreadyApplied);
  EXPECT_EQ(unchanged.value().projection_revision, safe_point_projection);
  EXPECT_EQ(scene.projection, safe_point_projection);
  EXPECT_EQ(scene.commit_count, 2U);
  ASSERT_TRUE(scene.state.provider_evidence.has_value());
  EXPECT_EQ(scene.state.provider_evidence->provider_source_sequence, 3U);

  const auto moved =
      apply(Newer(Observation(PosePayload(0.5)), 5U), "between_nodes");
  ASSERT_TRUE(moved.ok()) << moved.error().message;
  EXPECT_EQ(moved.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  ASSERT_EQ(scene.commit_count, 3U);
  EXPECT_DOUBLE_EQ(scene.pose.position_m.x_m, 0.5);

  auto new_epoch = Newer(Observation(PosePayload(0.5)), 6U);
  new_epoch.source_epoch = "source-epoch.2";
  const auto changed_origin = apply(new_epoch, "between_nodes");
  ASSERT_TRUE(changed_origin.ok()) << changed_origin.error().message;
  EXPECT_EQ(changed_origin.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 4U);
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

TEST(SceneRuntimePoseProjector,
     ConfirmsFreshnessOfUnchangedPoseWithoutCommitting) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  const auto initial = ApplyTo(projector, scene, providers, Observation());
  ASSERT_TRUE(initial.ok()) << initial.error().message;
  const auto committed_projection = scene.projection;

  // The source timestamp is diagnostic only and never blocks confirmation.
  const auto newer =
      Newer(Observation(PosePayload(0.25, 1.0, 1500000000)), 3U);
  const auto confirmed = ApplyTo(projector, scene, providers, newer);

  ASSERT_TRUE(confirmed.ok()) << confirmed.error().message;
  ASSERT_TRUE(confirmed.value().valid());
  EXPECT_EQ(confirmed.value().status,
            SceneRuntimePoseApplicationStatus::kFreshnessConfirmed);
  EXPECT_EQ(ToString(confirmed.value().status), "freshness_confirmed");
  EXPECT_EQ(confirmed.value().projection_revision, committed_projection);
  EXPECT_FALSE(confirmed.value().update_status.has_value());
  EXPECT_EQ(scene.commit_count, 1U);
  EXPECT_EQ(scene.projection, committed_projection);
  ASSERT_EQ(scene.confirmation_count, 1U);
  ASSERT_TRUE(scene.last_confirmation.has_value());
  EXPECT_TRUE(scene.last_confirmation->valid());
  EXPECT_EQ(scene.last_confirmation->expected_projection_revision,
            committed_projection);
  EXPECT_EQ(scene.last_confirmation->scene_resource_id.value(), "box");
  EXPECT_EQ(scene.last_confirmation->pose_in_cell, scene.pose);
  const auto &evidence = scene.last_confirmation->evidence;
  EXPECT_EQ(evidence.provider_source_sequence, 3U);
  EXPECT_EQ(evidence.received_at_ns, newer.observed_at_ns);
  EXPECT_EQ(evidence.source_timestamp_ns, 1500000000);
  EXPECT_EQ(evidence.source_freshness_budget_ns, 900);
  ASSERT_TRUE(scene.state.provider_evidence.has_value());
  const auto &committed_evidence = *scene.state.provider_evidence;
  EXPECT_EQ(committed_evidence.provider_source_sequence, 2U);
  EXPECT_EQ(evidence.provider_instance_id,
            committed_evidence.provider_instance_id);
  EXPECT_EQ(evidence.provider_manifest_id,
            committed_evidence.provider_manifest_id);
  EXPECT_EQ(evidence.provider_manifest_revision,
            committed_evidence.provider_manifest_revision);
  EXPECT_EQ(evidence.provider_source_epoch,
            committed_evidence.provider_source_epoch);
  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      confirmed.value().status,
      committed_evidence.provider_source_sequence,
      newer.source_sequence));

  // A replay of the accepted confirmation neither re-confirms nor commits.
  const auto replay = ApplyTo(projector, scene, providers, newer,
                              confirmed.value());
  ASSERT_TRUE(replay.ok()) << replay.error().message;
  EXPECT_EQ(replay.value().status,
            SceneRuntimePoseApplicationStatus::kFreshnessConfirmed);
  EXPECT_EQ(replay.value().projection_revision, committed_projection);
  EXPECT_EQ(scene.confirmation_count, 1U);
  EXPECT_EQ(scene.commit_count, 1U);

  // The committed evidence is untouched, so an equal-sequence replay of the
  // committed observation remains already applied.
  const auto already_applied =
      ApplyTo(projector, scene, providers, Observation());
  ASSERT_TRUE(already_applied.ok()) << already_applied.error().message;
  EXPECT_EQ(already_applied.value().status,
            SceneRuntimePoseApplicationStatus::kAlreadyApplied);
  EXPECT_EQ(scene.confirmation_count, 1U);
  EXPECT_EQ(scene.commit_count, 1U);
}

TEST(SceneRuntimePoseProjector,
     PlanningEvidenceSequenceRuleIsStatusSensitiveAndFailClosed) {
  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kCommitted, 7U, 7U));
  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kAlreadyApplied, 7U, 7U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kCommitted, 6U, 7U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kAlreadyApplied, 7U, 8U));

  EXPECT_TRUE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kFreshnessConfirmed, 7U, 8U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kFreshnessConfirmed, 8U, 8U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kFreshnessConfirmed, 9U, 8U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kNotRequested, 7U, 7U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning, 7U, 7U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kBuffered, 7U, 7U));
  EXPECT_FALSE(SceneRuntimePoseApplicationRetainsPlanningEvidence(
      SceneRuntimePoseApplicationStatus::kSkippedAttached, 7U, 7U));
}

TEST(SceneRuntimePoseProjector, CommitsChangedPoseBudgetOrOriginInstead) {
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

  auto rebudgeted = Newer(Observation(PosePayload(0.5)), 4U);
  rebudgeted.expires_at_ns += 100;
  const auto budget = ApplyTo(projector, scene, providers, rebudgeted);
  ASSERT_TRUE(budget.ok()) << budget.error().message;
  EXPECT_EQ(budget.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 3U);

  auto other_epoch = Newer(Observation(PosePayload(0.5)), 5U);
  other_epoch.source_epoch = "source-epoch.2";
  const auto origin = ApplyTo(projector, scene, providers, other_epoch);
  ASSERT_TRUE(origin.ok()) << origin.error().message;
  EXPECT_EQ(origin.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 4U);
  EXPECT_EQ(scene.confirmation_count, 0U);
}

TEST(SceneRuntimePoseProjector, FallsBackToCommitOnlyWhenUnsupported) {
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  SceneRuntimePoseProjector projector(scene, cell_id);
  const std::vector providers{Provider(cell_id)};
  ASSERT_TRUE(ApplyTo(projector, scene, providers, Observation()).ok());

  scene.confirmation_error = core::ErrorDTO{
      "provider_freshness_unsupported", "confirmation is unsupported", {}};
  const auto fallback =
      ApplyTo(projector, scene, providers, Newer(Observation(), 3U));
  ASSERT_TRUE(fallback.ok()) << fallback.error().message;
  EXPECT_EQ(fallback.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.confirmation_count, 1U);
  EXPECT_EQ(scene.commit_count, 2U);

  scene.confirmation_error = core::ErrorDTO{
      "provider_freshness_projection_failed",
      "a required native projection did not apply the freshness confirmation",
      {{"reason", "provider_freshness_projection_failed"},
       {"failed_projection_ordinals", "1"},
       {"applied_projection_ordinals", "0"},
       {"uncertain_projection_ordinals", "1"}}};
  const auto failed =
      ApplyTo(projector, scene, providers, Newer(Observation(), 4U));
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code,
            "scene_runtime_pose_freshness_confirmation_failed");
  EXPECT_EQ(failed.error().details.at("scene_error_code"),
            "provider_freshness_projection_failed");
  // Partial-propagation details survive the projector boundary.
  EXPECT_EQ(failed.error().details.at("failed_projection_ordinals"), "1");
  EXPECT_EQ(failed.error().details.at("applied_projection_ordinals"), "0");
  EXPECT_EQ(failed.error().details.at("uncertain_projection_ordinals"), "1");
  EXPECT_EQ(scene.confirmation_count, 2U);
  EXPECT_EQ(scene.commit_count, 2U);
}

TEST(SceneRuntimePoseProjector, DefaultAuthorityFallsBackToCommit) {
  struct DefaultConfirmationAuthority final : core::ISceneAuthority {
    explicit DefaultConfirmationAuthority(FakeSceneAuthority &delegate)
        : delegate(delegate) {}
    core::SceneCommitResultDTO
    Commit(const core::SceneTransactionRequestDTO &request) override {
      return delegate.Commit(request);
    }
    core::RequestOwnerEpoch CurrentRequestOwnerEpoch() const override {
      return delegate.CurrentRequestOwnerEpoch();
    }
    core::ProjectionRevisionDTO CurrentProjectionRevision() const override {
      return delegate.CurrentProjectionRevision();
    }
    std::shared_ptr<const core::LogicalSceneSnapshotDTO>
    LogicalSnapshot() const override {
      return nullptr;
    }
    std::vector<core::SceneObjectSnapshotDTO> GetSceneObjects() const override {
      return {};
    }
    core::SceneObjectStateDTO
    GetObjectState(const core::SceneResourceId &id) const override {
      return delegate.GetObjectState(id);
    }
    core::Result<core::Pose3DTO>
    GetObjectPose(const core::SceneResourceId &id) const override {
      return delegate.GetObjectPose(id);
    }
    FakeSceneAuthority &delegate;
  };
  const core::CellId cell_id("cell.1");
  FakeSceneAuthority scene;
  DefaultConfirmationAuthority authority(scene);
  SceneRuntimePoseProjector projector(authority, cell_id);
  const std::vector providers{Provider(cell_id)};
  ASSERT_TRUE(ApplyTo(projector, scene, providers, Observation()).ok());
  const auto fallback =
      ApplyTo(projector, scene, providers, Newer(Observation(), 3U));
  ASSERT_TRUE(fallback.ok()) << fallback.error().message;
  EXPECT_EQ(fallback.value().status,
            SceneRuntimePoseApplicationStatus::kCommitted);
  EXPECT_EQ(scene.commit_count, 2U);
  EXPECT_EQ(scene.confirmation_count, 0U);
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
                                 const bool relative = true) {
  return WithMember(
      WithMember(PosePayload(), "kind",
                 CanonicalValue("simulation_entity_pose_with_reference.v1")),
      "reference",
      CanonicalValue(CanonicalValue::Object{
          {"coherent", CanonicalValue(coherent)},
          {"entity", CanonicalValue("/gripper")},
          {"link", CanonicalValue("gripper_base")},
          {"pose_in_reference",
           relative
               ? CanonicalValue(CanonicalValue::Object{
                     {"orientation_xyzw",
                      CanonicalValue(CanonicalValue::Array{
                          CanonicalValue(0.0), CanonicalValue(0.0),
                          CanonicalValue(0.0), CanonicalValue(1.0)})},
                     {"position_m", CanonicalValue(CanonicalValue::Array{
                                        CanonicalValue(0.02),
                                        CanonicalValue(0.0),
                                        CanonicalValue(0.25)})},
                 })
               : CanonicalValue(nullptr)},
      }));
}

const core::ComponentSceneFrameRefDTO kCarrier{
    core::ControllableComponentId("robot"), core::ModelInstanceId("robot.model"),
    core::FrameRoleId("gripper_tcp")};

void AttachInSnapshot(FakeSceneAuthority &scene) {
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
              core::CommandTargetId("arm"), kCarrier, core::Pose3DTO{}, {}}},
      });
}

SceneRuntimeReferenceLinkResolver TcpAboveReference(std::size_t &calls) {
  return [&calls](const core::RetainedBundleId &,
                  const core::ComponentSceneFrameRefDTO &carrier,
                  const std::string &reference_link) {
    ++calls;
    EXPECT_EQ(carrier, kCarrier);
    EXPECT_EQ(reference_link, "gripper_base");
    return core::Result<core::Pose3DTO>::Ok(
        core::Pose3DTO{{0.0, 0.0, 0.2}, {0.0, 0.0, 0.0, 1.0}});
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
        return core::Result<core::Pose3DTO>::Err(core::ErrorDTO{
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

} // namespace
} // namespace manyforge::headless
