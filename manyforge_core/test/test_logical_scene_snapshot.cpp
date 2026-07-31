#include <manyforge_core/types/logical_scene_snapshot.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

// Fixed contract vectors exercise spelling and value predicates only. This
// DTO test deliberately does not claim to qualify the external
// JSON-canonicalization/SHA implementation or recompute either preimage.
constexpr char kSceneId[] =
    "sha256:1702618655fff8c8b8f86181440bc94fb28d0a7853113bb5d162ebbe1d71be17";
constexpr char kOtherSceneId[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kCanonicalEmptySceneDigest[] =
    "sha256:8f39833b5d7eb081ce9c0026868a9f8f381eb3c0a3c30f69353f2de0c865abe3";
constexpr char kNonemptySceneDigest[] =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";

LogicalSceneSnapshotIdentityDTO Identity(
    std::string cell = "cell-alpha",
    std::string scene = kSceneId,
    std::string content_hash = kCanonicalEmptySceneDigest) {
  return {
      SceneRevision{
          SceneId(std::move(scene)),
          1U,
          ContentDigest(std::move(content_hash)),
      },
      CellId(std::move(cell)),
  };
}

CollisionPolicySnapshot ConfiguredPolicy() {
  CollisionPolicySnapshot policy;
  policy.policy_id = "free_space";
  policy.revision = {1U, std::string(kCanonicalEmptyCollisionPolicyDigest)};
  return policy;
}

LogicalSceneSnapshotDTO CanonicalEmptySnapshot(
    LogicalSceneSnapshotIdentityDTO identity = Identity()) {
  return {
      std::move(identity.revision),
      std::move(identity.cell_id),
      0U,
      0U,
      0U,
      ConfiguredPolicy(),
      {},
      {},
  };
}

CollisionShapeDTO Box() {
  return BoxShapeDTO{Vector3DTO{0.1, 0.2, 0.3}};
}

TEST(LogicalSceneSnapshot, DigestSpellingIsExactLowercaseSha256) {
  EXPECT_TRUE(IsExactLowercaseSha256Digest(kSceneId));
  EXPECT_FALSE(IsExactLowercaseSha256Digest(""));
  EXPECT_FALSE(IsExactLowercaseSha256Digest(
      "SHA256:1702618655fff8c8b8f86181440bc94fb28d0a7853113bb5d162ebbe1d71be17"));
  EXPECT_FALSE(IsExactLowercaseSha256Digest(
      "sha256:1702618655fff8c8b8f86181440bc94fb28d0a7853113bb5d162ebbe1d71be1G"));
  EXPECT_FALSE(IsExactLowercaseSha256Digest("sha256:1234"));
}

TEST(LogicalSceneSnapshot, BootstrapIdentityInputIsValueOnlyAndComplete) {
  LogicalSceneBootstrapIdentityInputDTO input{
      CellId("cell-alpha"),
      BootstrapId("bootstrap-alpha"),
      BootstrapGeneration(1U),
  };
  EXPECT_TRUE(input.valid());
  input.bootstrap_generation = BootstrapGeneration(0U);
  EXPECT_FALSE(input.valid());
}

TEST(LogicalSceneSnapshot, CanonicalEmptyRecognizesOwnerMintedValue) {
  const auto expected = Identity();
  const auto snapshot = CanonicalEmptySnapshot(expected);

  EXPECT_TRUE(snapshot.locally_valid());
  EXPECT_TRUE(snapshot.has_exact_identity(expected));
  EXPECT_TRUE(snapshot.canonical_empty_for(expected));
  EXPECT_TRUE(snapshot.objects.empty());
  EXPECT_TRUE(snapshot.carried_object_attachments.empty());
  EXPECT_EQ(snapshot.transaction_sequence, 0U);
  EXPECT_EQ(snapshot.topology_epoch, 0U);
  EXPECT_EQ(snapshot.policy_epoch, 0U);

  EXPECT_FALSE(snapshot.canonical_empty_for(
      Identity("cell-beta", kOtherSceneId, kCanonicalEmptySceneDigest)));
  EXPECT_FALSE(snapshot.canonical_empty_for(
      LogicalSceneSnapshotIdentityDTO{
          SceneRevision{
              SceneId(kSceneId),
              2U,
              ContentDigest(kCanonicalEmptySceneDigest),
          },
          CellId("cell-alpha"),
      }));
}

TEST(LogicalSceneSnapshot, CanonicalEmptyRequiresExactCountersAndPolicy) {
  auto snapshot = CanonicalEmptySnapshot();
  snapshot.transaction_sequence = 1U;
  EXPECT_FALSE(snapshot.canonical_empty_for(Identity()));
  snapshot.transaction_sequence = 0U;
  snapshot.topology_epoch = 1U;
  EXPECT_FALSE(snapshot.canonical_empty_for(Identity()));
  snapshot.topology_epoch = 0U;
  snapshot.policy_epoch = 1U;
  EXPECT_FALSE(snapshot.canonical_empty_for(Identity()));
  snapshot.policy_epoch = 0U;

  snapshot.configured_collision_policy.require_full_diagnostics = false;
  EXPECT_FALSE(snapshot.canonical_empty_for(Identity()));
  snapshot.configured_collision_policy.require_full_diagnostics = true;
  snapshot.configured_collision_policy.revision.content_hash =
      "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  EXPECT_TRUE(snapshot.locally_valid());
  EXPECT_FALSE(snapshot.canonical_empty_for(Identity()));
}

TEST(LogicalSceneSnapshot, RevisionRejectsNoncanonicalDigestSpellings) {
  auto revision = Identity().revision;
  EXPECT_TRUE(revision.valid());
  revision.scene_id = SceneId("scene-alpha");
  EXPECT_FALSE(revision.valid());
  revision.scene_id = SceneId(kSceneId);
  revision.content_hash = ContentDigest(
      "sha256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
  EXPECT_FALSE(revision.valid());
}

TEST(LogicalSceneSnapshot, CellFixedRowsMustUseSnapshotCellAndCanonicalOrder) {
  auto snapshot = CanonicalEmptySnapshot(
      Identity("cell-alpha", kSceneId, kNonemptySceneDigest));
  snapshot.transaction_sequence = 3U;
  snapshot.topology_epoch = 2U;
  snapshot.policy_epoch = 1U;
  snapshot.objects = {
      SceneObjectSnapshotDTO{
          SceneResourceId("object-a"),
          {Box()},
          CellFixedSceneObjectPlacementDTO{
              CellId("cell-alpha"),
              Pose3DTO{},
          },
          std::nullopt,
      },
      SceneObjectSnapshotDTO{
          SceneResourceId("object-b"),
          {Box()},
          CellFixedSceneObjectPlacementDTO{
              CellId("cell-alpha"),
              Pose3DTO{},
          },
          std::nullopt,
      },
  };
  EXPECT_TRUE(snapshot.locally_valid());
  EXPECT_FALSE(snapshot.canonical_empty_for(Identity()));

  std::swap(snapshot.objects[0], snapshot.objects[1]);
  EXPECT_FALSE(snapshot.locally_valid());
  std::swap(snapshot.objects[0], snapshot.objects[1]);
  std::get<CellFixedSceneObjectPlacementDTO>(snapshot.objects[1].placement)
      .cell_id = CellId("cell-beta");
  EXPECT_FALSE(snapshot.locally_valid());
}

TEST(LogicalSceneSnapshot, CarriedObjectAndAttachmentAreOneToOne) {
  auto snapshot = CanonicalEmptySnapshot(
      Identity("cell-alpha", kSceneId, kNonemptySceneDigest));
  snapshot.transaction_sequence = 4U;
  snapshot.topology_epoch = 3U;
  snapshot.policy_epoch = 2U;
  const CarriedObjectAttachmentId attachment_id("attachment-a");
  const CarriedObjectAttachmentRevision attachment_revision(
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  snapshot.objects = {
      SceneObjectSnapshotDTO{
          SceneResourceId("object-a"),
          {Box()},
          CarriedSceneObjectPlacementDTO{
              attachment_id,
              attachment_revision,
          },
          std::nullopt,
      },
  };
  snapshot.carried_object_attachments = {
      CarriedObjectAttachmentDTO{
          attachment_id,
          attachment_revision,
          SceneResourceId("object-a"),
          ControllableComponentId("arm-a"),
          CommandTargetId("tool-target"),
          ComponentSceneFrameRefDTO{
              ControllableComponentId("arm-a"),
              ModelInstanceId("arm-model"),
              FrameRoleId("tool-frame"),
          },
          Pose3DTO{},
          {CollisionBodyRoleId("finger-left"),
           CollisionBodyRoleId("finger-right")},
      },
  };
  EXPECT_TRUE(snapshot.locally_valid());

  snapshot.carried_object_attachments[0].scene_resource_id =
      SceneResourceId("object-b");
  EXPECT_FALSE(snapshot.locally_valid());
}

}  // namespace
}  // namespace manyforge::core
