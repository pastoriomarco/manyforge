#include <gtest/gtest.h>

#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <memory>
#include <string>

namespace manyforge::core {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

LogicalSceneSnapshotDTO EmptyScene() {
  CollisionPolicySnapshot policy;
  policy.revision = {1U, std::string(kCanonicalEmptyCollisionPolicyDigest)};
  LogicalSceneSnapshotDTO scene{
      {logical_scene_identity::MintSceneId(
           {CellId("cell"), BootstrapId("bootstrap"), BootstrapGeneration(1U)}),
       1U, ContentDigest(std::string(kZeroDigest))},
      CellId("cell"),
      0U,
      0U,
      0U,
      std::move(policy),
      {},
      {}};
  scene.revision.content_hash = logical_scene_identity::ComputeLogicalSceneContentHash(scene);
  return scene;
}

SceneObjectSnapshotDTO DetachedObject() {
  return {SceneResourceId("part"),
          {BoxShapeDTO{{0.1, 0.1, 0.1}}},
          CellFixedSceneObjectPlacementDTO{CellId("cell"), {{0.2, 0.0, 0.1}, {0.0, 0.0, 0.0, 1.0}}},
          std::nullopt,
          {}};
}

ProjectionRevisionDTO Projection(const LogicalSceneSnapshotDTO& scene) {
  return {RetainedBundleId("bundle"), GraphRevision("graph"), scene.revision};
}

SceneTransactionDTO Transaction(const LogicalSceneSnapshotDTO& previous,
                                const LogicalSceneSnapshotDTO& candidate,
                                ResolvedSceneOperationDTO operation) {
  const bool resolution_evidence_required =
      !std::holds_alternative<ResolvedRemoveSceneObjectDTO>(operation) &&
      !std::holds_alternative<ResolvedCollisionPolicyDTO>(operation);
  return {RequestId("request-" + std::to_string(candidate.transaction_sequence)),
          {},
          candidate.transaction_sequence,
          Projection(previous),
          Projection(candidate),
          ContentDigest(std::string(kZeroDigest)),
          std::move(operation),
          resolution_evidence_required
              ? std::optional<ContentDigest>(
                    ContentDigest(std::string(kZeroDigest)))
              : std::nullopt};
}

void SealSuccessor(LogicalSceneSnapshotDTO& scene, const std::uint64_t revision_id) {
  scene.revision.revision_id = revision_id;
  scene.revision.content_hash = logical_scene_identity::ComputeLogicalSceneContentHash(scene);
  ASSERT_TRUE(scene.locally_valid());
}

TEST(LogicalSceneProjection, ReconstructsUpsertAndExactNoOp) {
  const auto empty = EmptyScene();
  auto added = empty;
  added.transaction_sequence = 1U;
  added.topology_epoch = 1U;
  added.objects.push_back(DetachedObject());
  SealSuccessor(added, 2U);

  const auto add = ApplyResolvedSceneTransaction(
      empty, Transaction(empty, added, ResolvedUpsertSceneObjectDTO{added.objects.front()}));
  ASSERT_TRUE(add.ok()) << add.error().message;
  EXPECT_EQ(add.value(), added);

  auto missing_resolution = Transaction(
      empty, added, ResolvedUpsertSceneObjectDTO{added.objects.front()});
  missing_resolution.resolution_evidence_digest = std::nullopt;
  EXPECT_FALSE(
      ApplyResolvedSceneTransaction(empty, missing_resolution).ok());

  auto no_op = added;
  no_op.transaction_sequence = 2U;
  const auto repeated = ApplyResolvedSceneTransaction(
      added, Transaction(added, no_op, ResolvedUpsertSceneObjectDTO{added.objects.front()}));
  ASSERT_TRUE(repeated.ok()) << repeated.error().message;
  EXPECT_EQ(repeated.value(), no_op);
  EXPECT_EQ(repeated.value().revision, added.revision);
  EXPECT_EQ(repeated.value().topology_epoch, added.topology_epoch);
}

TEST(LogicalSceneProjection,
     AcceptsAForwardSequenceGapAfterAClosedFailedWrite) {
  const auto empty = EmptyScene();
  auto added = empty;
  // Sequence 1 was assigned to an accepted write whose native effect failed
  // and was recovered back to this exact predecessor.
  added.transaction_sequence = 2U;
  added.topology_epoch = 1U;
  added.objects.push_back(DetachedObject());
  SealSuccessor(added, 2U);

  const auto advanced = ApplyResolvedSceneTransaction(
      empty,
      Transaction(empty, added,
                  ResolvedUpsertSceneObjectDTO{added.objects.front()}));
  ASSERT_TRUE(advanced.ok()) << advanced.error().message;
  EXPECT_EQ(advanced.value(), added);

  auto old_sequence = added;
  old_sequence.transaction_sequence = 1U;
  const auto rejected = ApplyResolvedSceneTransaction(
      added,
      Transaction(added, old_sequence,
                  ResolvedUpsertSceneObjectDTO{added.objects.front()}));
  EXPECT_FALSE(rejected.ok());
}

TEST(LogicalSceneProjection, DistinguishesFreshnessFromTopology) {
  auto current = EmptyScene();
  current.objects.push_back(DetachedObject());
  current.transaction_sequence = 1U;
  current.topology_epoch = 1U;
  SealSuccessor(current, 2U);

  auto freshness = current;
  freshness.transaction_sequence = 2U;
  freshness.objects.front().provider_evidence =
      SceneObjectProviderEvidenceDTO{ProviderInstanceId("camera"),
                                     ProviderManifestId("camera-manifest"),
                                     ProviderManifestRevision(std::string(kZeroDigest)),
                                     "camera-epoch"};
  SealSuccessor(freshness, 3U);
  const auto refreshed = ApplyResolvedSceneTransaction(
      current,
      Transaction(current, freshness, ResolvedUpsertSceneObjectDTO{freshness.objects.front()}));
  ASSERT_TRUE(refreshed.ok()) << refreshed.error().message;
  EXPECT_EQ(refreshed.value(), freshness);
  EXPECT_EQ(refreshed.value().topology_epoch, current.topology_epoch);

  auto moved = freshness;
  moved.transaction_sequence = 3U;
  std::get<CellFixedSceneObjectPlacementDTO>(moved.objects.front().placement)
      .pose_in_cell.position_m.x_m = 0.3;
  moved.topology_epoch = 2U;
  SealSuccessor(moved, 4U);
  const auto projected_move = ApplyResolvedSceneTransaction(
      freshness,
      Transaction(freshness, moved, ResolvedUpsertSceneObjectDTO{moved.objects.front()}));
  ASSERT_TRUE(projected_move.ok()) << projected_move.error().message;
  EXPECT_EQ(projected_move.value(), moved);
}

TEST(LogicalSceneProjection, ReconstructsAttachDetachAndRejectsWrongDigest) {
  auto detached = EmptyScene();
  detached.objects.push_back(DetachedObject());
  detached.transaction_sequence = 1U;
  detached.topology_epoch = 1U;
  SealSuccessor(detached, 2U);

  CarriedObjectAttachmentDTO attachment{
      CarriedObjectAttachmentId("part-attachment"),
      CarriedObjectAttachmentRevision(std::string(kZeroDigest)),
      SceneResourceId("part"),
      ControllableComponentId("robot"),
      CommandTargetId("arm"),
      ComponentSceneFrameRefDTO{ControllableComponentId("robot"), ModelInstanceId("robot-model"),
                                FrameRoleId("tcp")},
      {{0.0, 0.0, 0.02}, {0.0, 0.0, 0.0, 1.0}},
      {CollisionBodyRoleId("gripper")}};
  attachment.carried_object_attachment_revision =
      logical_scene_identity::ComputeCarriedObjectAttachmentRevision(attachment);
  auto carried_object = detached.objects.front();
  carried_object.placement = CarriedSceneObjectPlacementDTO{
      attachment.carried_object_attachment_id, attachment.carried_object_attachment_revision};
  auto attached = detached;
  attached.transaction_sequence = 2U;
  attached.topology_epoch = 2U;
  attached.policy_epoch = 1U;
  attached.objects.front() = carried_object;
  attached.carried_object_attachments.push_back(attachment);
  SealSuccessor(attached, 3U);

  const auto projected_attach = ApplyResolvedSceneTransaction(
      detached,
      Transaction(detached, attached, ResolvedAttachCarriedObjectDTO{carried_object, attachment}));
  ASSERT_TRUE(projected_attach.ok()) << projected_attach.error().message;
  EXPECT_EQ(projected_attach.value(), attached);

  auto final_object = DetachedObject();
  std::get<CellFixedSceneObjectPlacementDTO>(final_object.placement).pose_in_cell.position_m.z_m =
      0.0;
  auto final_scene = attached;
  final_scene.transaction_sequence = 3U;
  final_scene.topology_epoch = 3U;
  final_scene.policy_epoch = 2U;
  final_scene.objects.front() = final_object;
  final_scene.carried_object_attachments.clear();
  SealSuccessor(final_scene, 4U);
  const auto projected_detach = ApplyResolvedSceneTransaction(
      attached, Transaction(attached, final_scene,
                            ResolvedDetachCarriedObjectDTO{
                                final_object, attachment.carried_object_attachment_id,
                                attachment.carried_object_attachment_revision}));
  ASSERT_TRUE(projected_detach.ok()) << projected_detach.error().message;
  EXPECT_EQ(projected_detach.value(), final_scene);

  auto corrupt = Transaction(
      attached, final_scene,
      ResolvedDetachCarriedObjectDTO{final_object, attachment.carried_object_attachment_id,
                                     attachment.carried_object_attachment_revision});
  corrupt.proposed_projection_revision.scene_revision.content_hash =
      ContentDigest(std::string(kZeroDigest));
  const auto rejected = ApplyResolvedSceneTransaction(attached, corrupt);
  EXPECT_FALSE(rejected.ok());
}

TEST(LogicalSceneProjection, RejectsInexactProjectionReadyPolicy) {
  const auto current = EmptyScene();
  auto candidate = current;
  candidate.transaction_sequence = 1U;
  candidate.policy_epoch = 1U;
  candidate.configured_collision_policy.policy_id = "reduced-diagnostics";
  candidate.configured_collision_policy.require_full_diagnostics = false;
  candidate.configured_collision_policy.revision.revision_id = 2U;
  candidate.configured_collision_policy.revision.content_hash =
      logical_scene_identity::ComputeCollisionPolicyContentDigest(
          candidate.configured_collision_policy)
          .value();
  SealSuccessor(candidate, 2U);

  auto inexact_effective = candidate.configured_collision_policy;
  inexact_effective.revision.content_hash = std::string(kZeroDigest);
  const auto rejected = ApplyResolvedSceneTransaction(
      current, Transaction(current, candidate,
                           ResolvedCollisionPolicyDTO{candidate.configured_collision_policy,
                                                      std::move(inexact_effective)}));
  EXPECT_FALSE(rejected.ok());
}

}  // namespace
}  // namespace manyforge::core
