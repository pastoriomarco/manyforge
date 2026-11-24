#include <manyforge_core/logical_scene_identity.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core::logical_scene_identity {
namespace {

constexpr char kSceneIdGolden[] =
    "sha256:1702618655fff8c8b8f86181440bc94fb28d0a7853113bb5d162ebbe1d71be17";
constexpr char kEmptySceneGolden[] =
    "sha256:5f895143fe92b8b0b1a857fe27a8c59f96c9ae0d46c305546dc58d237d206eeb";
constexpr char kAttachmentGolden[] =
    "sha256:752cc330ef8133e5797b4c2cc2ea1ef450197978f99f5ddfef40eb20c331f679";
constexpr char kDigestA[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kDigestB[] =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr char kMeshDigest[] =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

CollisionPolicySnapshot EmptyPolicy() {
  CollisionPolicySnapshot policy;
  policy.revision = {1U, std::string(kCanonicalEmptyCollisionPolicyDigest)};
  return policy;
}

CollisionPolicySnapshot RichPolicy() {
  CollisionPolicySnapshot policy;
  policy.policy_id = "rich-policy";
  policy.static_body_ids = {"scene/fixture-a/shape/0", "scene/fixture-b/shape/0"};
  policy.allowed_touch_link_pairs = {
      {"component/arm-a/model/arm-model/body/finger-a/collision/0",
       "scene/object-a/shape/0"}};
  CollisionReleaseRule release;
  release.release_evidence_mode =
      ContactReleaseEvidenceMode::kSignedPenetration;
  release.enforce_max_start_penetration = true;
  release.max_start_penetration_m = 0.001;
  release.release_tolerance_m = 0.00001;
  release.min_release_progress_m = 0.00002;
  release.require_separation_by_end = true;
  release.require_strictly_decreasing_penetration = true;
  release.allow_recontact = false;
  policy.contact_rules = {
      CollisionPairContactRule{
          {"component/arm-a/model/arm-model/body/finger-a/collision/0",
           "scene/object-b/shape/0"},
          "grasp-a",
          CollisionContactRule{0.0005, 0.8, std::nullopt}},
      CollisionPairContactRule{
          {"scene/object-a/shape/0", "scene/support/shape/0"},
          "lift-a",
          release},
  };
  policy.require_full_diagnostics = true;
  return policy;
}

void SealPolicy(CollisionPolicySnapshot& policy,
                std::uint64_t revision_id = 1U) {
  policy.revision = {
      revision_id,
      ComputeCollisionPolicyContentDigest(policy).value(),
  };
}

LogicalSceneSnapshotDTO EmptySnapshot() {
  return LogicalSceneSnapshotDTO{
      SceneRevision{SceneId(kSceneIdGolden), 1U,
                    ContentDigest(kEmptySceneGolden)},
      CellId("cell-alpha"),
      0U,
      0U,
      0U,
      EmptyPolicy(),
      {},
      {},
  };
}

LogicalSceneSnapshotDTO RichSnapshot() {
  auto snapshot = EmptySnapshot();
  snapshot.revision.content_hash = ContentDigest(kDigestA);
  snapshot.transaction_sequence = 4U;
  snapshot.topology_epoch = 3U;
  snapshot.policy_epoch = 2U;

  // A policy retained in the logical scene carries no collision rules.
  auto policy = RichPolicy();
  policy.contact_rules.clear();
  SealPolicy(policy, 7U);
  snapshot.configured_collision_policy = policy;

  const CarriedObjectAttachmentId attachment_id("attachment-a");
  const CarriedObjectAttachmentRevision attachment_revision(kDigestB);
  snapshot.objects = {
      SceneObjectSnapshotDTO{
          SceneResourceId("object-a"),
          {SphereShapeDTO{0.04}, BoxShapeDTO{Vector3DTO{0.1, 0.2, 0.3}}},
          CellFixedSceneObjectPlacementDTO{
              CellId("cell-alpha"),
              Pose3DTO{Vector3DTO{0.1, 0.2, 0.3},
                       QuaternionDTO{0.0, 0.0, 0.0, 1.0}},
          },
          SceneObjectProviderEvidenceDTO{
              ProviderInstanceId("provider-a"),
              ProviderManifestId("manifest-a"),
              ProviderManifestRevision(kDigestA),
              "epoch-a",
          },
      },
      SceneObjectSnapshotDTO{
          SceneResourceId("object-b"),
          {MeshShapeDTO{"asset://tool.stl", Vector3DTO{1.0, 1.0, 1.0},
                        kMeshDigest}},
          CarriedSceneObjectPlacementDTO{attachment_id, attachment_revision},
          std::nullopt,
      },
  };
  snapshot.carried_object_attachments = {
      CarriedObjectAttachmentDTO{
          attachment_id,
          attachment_revision,
          SceneResourceId("object-b"),
          ControllableComponentId("arm-a"),
          CommandTargetId("tool-target"),
          ComponentSceneFrameRefDTO{
              ControllableComponentId("arm-a"),
              ModelInstanceId("arm-model"),
              FrameRoleId("tool-frame"),
          },
          Pose3DTO{Vector3DTO{0.0, 0.0, 0.1},
                   QuaternionDTO{0.0, 0.0, 0.0, 1.0}},
          {CollisionBodyRoleId("finger-left"),
           CollisionBodyRoleId("finger-right")},
      },
  };
  const auto sealed_attachment_revision =
      ComputeCarriedObjectAttachmentRevision(
          snapshot.carried_object_attachments.front());
  snapshot.carried_object_attachments.front()
      .carried_object_attachment_revision = sealed_attachment_revision;
  std::get<CarriedSceneObjectPlacementDTO>(snapshot.objects[1].placement)
      .carried_object_attachment_revision = sealed_attachment_revision;
  return snapshot;
}

TEST(LogicalSceneIdentity, MintsPublishedSceneLineageGolden) {
  const LogicalSceneBootstrapIdentityInputDTO input{
      CellId("cell-alpha"), BootstrapId("bootstrap-alpha"),
      BootstrapGeneration(1U)};
  EXPECT_EQ(MintSceneId(input).value(), kSceneIdGolden);

  auto changed = input;
  changed.cell_id = CellId("cell-beta");
  EXPECT_NE(MintSceneId(changed), MintSceneId(input));
  changed = input;
  changed.bootstrap_id = BootstrapId("bootstrap-beta");
  EXPECT_NE(MintSceneId(changed), MintSceneId(input));
  changed = input;
  changed.bootstrap_generation = BootstrapGeneration(2U);
  EXPECT_NE(MintSceneId(changed), MintSceneId(input));
}

TEST(LogicalSceneIdentity, RejectsInvalidOrUnsafeBootstrapIdentity) {
  EXPECT_THROW(
      MintSceneId(LogicalSceneBootstrapIdentityInputDTO{
          CellId(""), BootstrapId("bootstrap-alpha"), BootstrapGeneration(1U)}),
      LogicalSceneIdentityError);
  EXPECT_THROW(MintSceneId(LogicalSceneBootstrapIdentityInputDTO{
                   CellId("cell-alpha"), BootstrapId("bootstrap-alpha"),
                   BootstrapGeneration(9007199254740992ULL)}),
               LogicalSceneIdentityError);
}

TEST(LogicalSceneIdentity, ComputesPublishedCanonicalEmptyPolicyGolden) {
  auto policy = EmptyPolicy();
  policy.revision = {999U, std::string(kDigestA)};
  EXPECT_EQ(ComputeCollisionPolicyContentDigest(policy).value(),
            kCanonicalEmptyCollisionPolicyDigest);
}

TEST(LogicalSceneIdentity, PolicyDigestOfManyStaticBodiesIsDeterministic) {
  // 200 cell-fixed objects: the digest covers 200 static bodies, not their
  // 19900 pairs, and two equal policies hash alike.
  auto policy = EmptyPolicy();
  for (int index = 0; index < 200; ++index) {
    policy.static_body_ids.push_back("scene/fixed-" +
                                     std::to_string(1000 + index).substr(1U) +
                                     "/shape/0");
  }
  const auto digest = ComputeCollisionPolicyContentDigest(policy);
  const auto copy = policy;
  EXPECT_EQ(ComputeCollisionPolicyContentDigest(copy), digest);
  EXPECT_NE(digest.value(), kCanonicalEmptyCollisionPolicyDigest);
  auto fewer = policy;
  fewer.static_body_ids.erase(fewer.static_body_ids.begin() + 57);
  EXPECT_NE(ComputeCollisionPolicyContentDigest(fewer), digest);
}

TEST(LogicalSceneIdentity, PolicyDigestCoversEverySemanticSection) {
  const auto baseline = RichPolicy();
  const auto digest = ComputeCollisionPolicyContentDigest(baseline);
  const auto expect_changed = [&](auto mutate) {
    auto changed = baseline;
    mutate(changed);
    EXPECT_NE(ComputeCollisionPolicyContentDigest(changed), digest);
  };

  expect_changed([](auto& value) { value.policy_id = "other-policy"; });
  expect_changed([](auto& value) {
    value.static_body_ids[1] = "scene/fixture-c/shape/0";
  });
  expect_changed([](auto& value) { value.static_body_ids.pop_back(); });
  expect_changed([](auto& value) {
    value.allowed_touch_link_pairs[0].body_b = "scene/object-b/shape/0";
  });
  expect_changed([](auto& value) {
    std::get<CollisionReleaseRule>(value.contact_rules[1].value)
        .max_start_penetration_m = 0.002;
  });
  expect_changed([](auto& value) {
    std::get<CollisionContactRule>(value.contact_rules[0].value)
        .end_from_fraction = 0.9;
  });
  expect_changed([](auto& value) {
    std::get<CollisionContactRule>(value.contact_rules[0].value)
        .max_penetration_m.reset();
  });
  expect_changed([](auto& value) {
    std::get<CollisionContactRule>(value.contact_rules[0].value)
        .end_from_fraction = 0.0;
  });
  expect_changed([](auto& value) { value.contact_rules[0].rule_id = "grasp-b"; });
  expect_changed([](auto& value) {
    value.contact_rules[0].value = CollisionReleaseRule{};
  });
  expect_changed([](auto& value) { value.require_full_diagnostics = false; });

  auto revision_only = baseline;
  revision_only.revision = {123U, std::string(kDigestB)};
  EXPECT_EQ(ComputeCollisionPolicyContentDigest(revision_only), digest);
}

TEST(LogicalSceneIdentity, RejectsNoncanonicalPolicyInsteadOfNormalizingIt) {
  auto policy = RichPolicy();
  policy.static_body_ids.push_back(policy.static_body_ids.front());
  EXPECT_THROW(ComputeCollisionPolicyContentDigest(policy),
               LogicalSceneIdentityError);
  policy = RichPolicy();
  std::swap(policy.static_body_ids[0], policy.static_body_ids[1]);
  EXPECT_THROW(ComputeCollisionPolicyContentDigest(policy),
               LogicalSceneIdentityError);

  // Binary-contact evidence keeps the signed-only release fields neutral.
  policy = RichPolicy();
  std::get<CollisionReleaseRule>(policy.contact_rules[1].value)
      .release_evidence_mode = ContactReleaseEvidenceMode::kBinaryContact;
  EXPECT_THROW(ComputeCollisionPolicyContentDigest(policy),
               LogicalSceneIdentityError);

  policy = RichPolicy();
  std::swap(policy.contact_rules[0], policy.contact_rules[1]);
  EXPECT_THROW(ComputeCollisionPolicyContentDigest(policy),
               LogicalSceneIdentityError);

  policy = RichPolicy();
  policy.contact_rules[0].rule_id.clear();
  EXPECT_THROW(ComputeCollisionPolicyContentDigest(policy),
               LogicalSceneIdentityError);

  policy = RichPolicy();
  std::get<CollisionContactRule>(policy.contact_rules[0].value)
      .end_from_fraction = 1.5;
  EXPECT_THROW(ComputeCollisionPolicyContentDigest(policy),
               LogicalSceneIdentityError);
}

TEST(LogicalSceneIdentity, AttachmentRevisionCoversContentButNotItself) {
  const auto snapshot = RichSnapshot();
  const auto& attachment = snapshot.carried_object_attachments.front();
  const auto revision = ComputeCarriedObjectAttachmentRevision(attachment);
  EXPECT_EQ(revision, attachment.carried_object_attachment_revision);
  EXPECT_EQ(revision.value(), kAttachmentGolden);

  auto changed = attachment;
  changed.carried_object_attachment_revision =
      CarriedObjectAttachmentRevision(kDigestA);
  EXPECT_EQ(ComputeCarriedObjectAttachmentRevision(changed), revision);
  changed = attachment;
  changed.command_target_id = CommandTargetId("other-target");
  EXPECT_NE(ComputeCarriedObjectAttachmentRevision(changed), revision);
  changed = attachment;
  changed.pose_in_carrier_frame.position_m.z_m += 0.01;
  EXPECT_NE(ComputeCarriedObjectAttachmentRevision(changed), revision);
}

TEST(LogicalSceneIdentity, ComputesCanonicalEmptyLogicalSceneGolden) {
  EXPECT_EQ(ComputeLogicalSceneContentHash(EmptySnapshot()).value(),
            kEmptySceneGolden);
}

TEST(LogicalSceneIdentity, SceneHashCoversPortableContent) {
  const auto baseline = RichSnapshot();
  const auto digest = ComputeLogicalSceneContentHash(baseline);
  const auto expect_changed = [&](auto mutate) {
    auto changed = baseline;
    mutate(changed);
    EXPECT_NE(ComputeLogicalSceneContentHash(changed), digest);
  };

  expect_changed([](auto& value) {
    value.cell_id = CellId("cell-beta");
    std::get<CellFixedSceneObjectPlacementDTO>(value.objects[0].placement)
        .cell_id = CellId("cell-beta");
  });
  expect_changed([](auto& value) {
    std::get<CellFixedSceneObjectPlacementDTO>(value.objects[0].placement)
        .pose_in_cell.position_m.x_m += 0.01;
  });
  expect_changed([](auto& value) {
    value.objects[0].provider_evidence->provider_source_epoch = "epoch-b";
  });
  expect_changed([](auto& value) {
    value.carried_object_attachments[0].command_target_id =
        CommandTargetId("other-target");
    const auto revision = ComputeCarriedObjectAttachmentRevision(
        value.carried_object_attachments[0]);
    value.carried_object_attachments[0].carried_object_attachment_revision =
        revision;
    std::get<CarriedSceneObjectPlacementDTO>(value.objects[1].placement)
        .carried_object_attachment_revision = revision;
  });
  expect_changed([](auto& value) {
    value.configured_collision_policy.revision.revision_id += 1U;
  });
  expect_changed([](auto& value) {
    std::get<MeshShapeDTO>(value.objects[1].collision_shapes.front())
        .content_digest = kDigestB;
  });
}

TEST(LogicalSceneIdentity, MeshShapeRequiresExactContentDigest) {
  for (const std::string &invalid :
       {std::string{}, std::string("sha256:CCCC"),
        std::string(kMeshDigest).substr(1U)}) {
    auto snapshot = RichSnapshot();
    std::get<MeshShapeDTO>(snapshot.objects[1].collision_shapes.front())
        .content_digest = invalid;
    EXPECT_FALSE(snapshot.objects[1].locally_valid());
    EXPECT_THROW(ComputeLogicalSceneContentHash(snapshot),
                 LogicalSceneIdentityError);
  }
}

TEST(LogicalSceneIdentity,
     SceneObjectCollisionIdentitySeparatesSamplesFromCollisionSemantics) {
  const auto snapshot = RichSnapshot();
  const auto& object = snapshot.objects.front();
  const auto identity =
      ComputeSceneObjectCollisionIdentity(object, snapshot.cell_id);

  auto changed = object;
  auto& fixed =
      std::get<CellFixedSceneObjectPlacementDTO>(changed.placement);
  fixed.pose_in_cell.position_m.x_m += 0.5;
  EXPECT_EQ(ComputeSceneObjectCollisionIdentity(changed, snapshot.cell_id),
            identity);

  changed = object;
  std::get<SphereShapeDTO>(changed.collision_shapes.front()).radius_m += 0.01;
  EXPECT_NE(ComputeSceneObjectCollisionIdentity(changed, snapshot.cell_id),
            identity);

  changed = object;
  changed.provider_evidence->provider_source_epoch = "epoch-b";
  EXPECT_NE(ComputeSceneObjectCollisionIdentity(changed, snapshot.cell_id),
            identity);
}

TEST(LogicalSceneIdentity, SceneHashExcludesLineageAndOwnerCounters) {
  const auto baseline = RichSnapshot();
  const auto digest = ComputeLogicalSceneContentHash(baseline);
  auto changed = baseline;
  changed.revision = SceneRevision{SceneId(kDigestB),
                                   std::numeric_limits<std::uint64_t>::max(),
                                   ContentDigest("not-even-a-digest")};
  changed.transaction_sequence = std::numeric_limits<std::uint64_t>::max();
  changed.topology_epoch = std::numeric_limits<std::uint64_t>::max();
  changed.policy_epoch = std::numeric_limits<std::uint64_t>::max();
  EXPECT_EQ(ComputeLogicalSceneContentHash(changed), digest);
}

TEST(LogicalSceneIdentity, ShapeSetOrderDoesNotAffectIdentity) {
  const auto baseline = RichSnapshot();
  auto reordered = baseline;
  std::reverse(reordered.objects[0].collision_shapes.begin(),
               reordered.objects[0].collision_shapes.end());
  EXPECT_EQ(ComputeLogicalSceneContentHash(reordered),
            ComputeLogicalSceneContentHash(baseline));

  reordered.objects[0].collision_shapes.push_back(
      reordered.objects[0].collision_shapes.front());
  EXPECT_THROW(ComputeLogicalSceneContentHash(reordered),
               LogicalSceneIdentityError);
}

TEST(LogicalSceneIdentity, RejectsStalePolicyAndAttachmentInconsistency) {
  auto snapshot = RichSnapshot();
  snapshot.configured_collision_policy.policy_id = "stale-policy";
  EXPECT_THROW(ComputeLogicalSceneContentHash(snapshot),
               LogicalSceneIdentityError);

  snapshot = RichSnapshot();
  snapshot.carried_object_attachments[0].scene_resource_id =
      SceneResourceId("object-a");
  EXPECT_THROW(ComputeLogicalSceneContentHash(snapshot),
               LogicalSceneIdentityError);
}

TEST(LogicalSceneIdentity, ObjectMassIsAbsentAtZeroAndCoveredWhenPositive) {
  const auto baseline = RichSnapshot();
  const auto digest = ComputeLogicalSceneContentHash(baseline);
  const auto collision =
      ComputeSceneObjectCollisionIdentity(baseline.objects[1], baseline.cell_id);

  // A zero mass is absent from identity: existing digests keep their value.
  auto massless = baseline;
  massless.objects[1].mass_kg = 0.0;
  EXPECT_EQ(ComputeLogicalSceneContentHash(massless), digest);

  auto heavy = baseline;
  heavy.objects[1].mass_kg = 1.5;
  EXPECT_TRUE(heavy.objects[1].locally_valid());
  EXPECT_NE(ComputeLogicalSceneContentHash(heavy), digest);
  // Mass is payload, not collision geometry.
  EXPECT_EQ(
      ComputeSceneObjectCollisionIdentity(heavy.objects[1], heavy.cell_id),
      collision);

  for (const double invalid :
       {-1.0, std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity()}) {
    auto refused = baseline;
    refused.objects[1].mass_kg = invalid;
    EXPECT_FALSE(refused.objects[1].locally_valid());
    EXPECT_THROW(ComputeLogicalSceneContentHash(refused),
                 LogicalSceneIdentityError);
  }
}

TEST(LogicalSceneIdentity, RejectsNegativeZero) {
  auto snapshot = RichSnapshot();
  std::get<CellFixedSceneObjectPlacementDTO>(snapshot.objects[0].placement)
      .pose_in_cell.position_m.x_m = -0.0;
  EXPECT_THROW(ComputeLogicalSceneContentHash(snapshot),
               LogicalSceneIdentityError);
}

}  // namespace
}  // namespace manyforge::core::logical_scene_identity
