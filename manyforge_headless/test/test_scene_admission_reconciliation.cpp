#include "../src/scene_admission_reconciliation_internal.hpp"

#include <manyforge_scene/logical_scene_value_codec.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::headless {
namespace {

namespace core = manyforge::core;
namespace scene = manyforge::scene;

const std::string kDigest = "sha256:" + std::string(64U, 'a');
const core::RequestKey kRequestKey = core::RequestKey::Make(
    core::RequestKeyClass::kHttpClient, 0U, 1U, 1U);

core::SceneTransactionRequestDTO Request() {
  return {{kRequestKey, "test"},
          {},
          {core::RetainedBundleId(kDigest),
           core::GraphRevision(kDigest),
           {core::SceneId(kDigest), 1U, core::ContentDigest(kDigest)}},
          core::RemoveObjectRequestDTO{core::SceneResourceId("box")},
          core::AdmissionGeneration(1U)};
}

struct SceneOwner final {
  struct Access final {
    bool scene_writes_open{true};
    bool projection_leases_open{true};
  } access;
  core::SceneCommitResultDTO result{
      kRequestKey,
      core::RequestReplayStatusDTO::kAcceptedNew,
      false,
      core::ErrorDTO{"stale_revision", "revision changed", {}},
      std::nullopt,
      std::nullopt,
      core::SceneProjectionCommitStatusDTO::kNotAttempted,
      std::nullopt};
  bool retained{};
  bool commit_throws{};
  bool lookup_throws{};
  unsigned commits{};
  unsigned lookups{};

  core::SceneCommitResultDTO Commit(const core::SceneTransactionRequestDTO &) {
    ++commits;
    if (commit_throws)
      throw std::runtime_error("release response lost");
    return result;
  }
  std::optional<core::SceneCommitResultDTO>
  ReadCommitResult(const core::SceneTransactionRequestDTO &) {
    ++lookups;
    if (lookup_throws)
      throw std::runtime_error("journal unreadable");
    return retained ? std::optional{result} : std::nullopt;
  }
  Access ReadAccessStatus() const { return access; }
};

TEST(SceneAdmissionReconciliation,
     PreAcceptanceRefusalDoesNotInventAnUnresolvedOwner) {
  SceneOwner owner;
  std::atomic<bool> reconciled{true};
  const auto result =
      detail::CommitSceneWithAdmissionObservation(owner, Request(), reconciled);
  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.error->code, "stale_revision");
  EXPECT_TRUE(reconciled.load());
  EXPECT_EQ(owner.commits, 1U);
  EXPECT_EQ(owner.lookups, 1U);
}

TEST(SceneAdmissionReconciliation,
     NotAttemptedWithRetainedOutcomeStillRequiresSettlement) {
  SceneOwner owner;
  owner.retained = true;
  owner.result.error = core::ErrorDTO{
      "authoring_not_ready", "claim acquisition was refused", {}};
  std::atomic<bool> reconciled{true};
  EXPECT_FALSE(
      detail::CommitSceneWithAdmissionObservation(owner, Request(), reconciled)
          .success);
  EXPECT_FALSE(reconciled.load());
  EXPECT_EQ(owner.commits, 1U);
}

TEST(SceneAdmissionReconciliation,
     UnavailableReconciliationOfEarlierClaimsStaysClosedWithoutCurrentOutcome) {
  SceneOwner owner;
  owner.result.error = core::ErrorDTO{
      "kernel_owner_unavailable", "earlier claim settlement unavailable", {}};
  std::atomic<bool> reconciled{true};
  static_cast<void>(detail::CommitSceneWithAdmissionObservation(
      owner, Request(), reconciled));
  EXPECT_FALSE(reconciled.load());
  EXPECT_EQ(owner.lookups, 0U);
}

TEST(SceneAdmissionReconciliation,
     RequestIdConflictStaysTypedAndKeepsReconciliationRequired) {
  SceneOwner owner;
  owner.lookup_throws = true; // the owner refuses a lookup of another intent
  owner.result.replay_status = core::RequestReplayStatusDTO::kRequestIdConflict;
  owner.result.error = core::ErrorDTO{
      "request_id_conflict", "scene request ID was reused with different intent",
      {}};
  std::atomic<bool> reconciled{true};
  const auto result =
      detail::CommitSceneWithAdmissionObservation(owner, Request(), reconciled);
  EXPECT_EQ(result.error->code, "request_id_conflict");
  EXPECT_FALSE(reconciled.load());
  EXPECT_EQ(owner.lookups, 0U);
}

TEST(SceneAdmissionReconciliation,
     ClosedSceneGatesRemainUnresolvedWithoutAnOutcome) {
  for (bool leases_closed : {false, true}) {
    SceneOwner owner;
    if (leases_closed)
      owner.access.projection_leases_open = false;
    else
      owner.access.scene_writes_open = false;
    std::atomic<bool> reconciled{true};
    static_cast<void>(detail::CommitSceneWithAdmissionObservation(
        owner, Request(), reconciled));
    EXPECT_FALSE(reconciled.load());
  }
}

TEST(SceneAdmissionReconciliation,
     NativeUncertaintyDoesNotUseMissingOutcomeAsProof) {
  SceneOwner owner;
  owner.result.projection_status =
      core::SceneProjectionCommitStatusDTO::kFailedProjectionUncertain;
  std::atomic<bool> reconciled{true};
  static_cast<void>(detail::CommitSceneWithAdmissionObservation(
      owner, Request(), reconciled));
  EXPECT_FALSE(reconciled.load());
  EXPECT_EQ(owner.lookups, 0U);
}

TEST(SceneAdmissionReconciliation,
     RefusalCannotClearAnExistingReconciliationRequirement) {
  SceneOwner owner;
  std::atomic<bool> reconciled{false};
  static_cast<void>(detail::CommitSceneWithAdmissionObservation(
      owner, Request(), reconciled));
  EXPECT_FALSE(reconciled.load());
}

TEST(SceneAdmissionReconciliation,
     LostReleaseAndUnreadableOutcomeRemainClosedAndPropagate) {
  for (bool lost_release : {false, true}) {
    SceneOwner owner;
    owner.commit_throws = lost_release;
    owner.lookup_throws = !lost_release;
    std::atomic<bool> reconciled{true};
    EXPECT_THROW(detail::CommitSceneWithAdmissionObservation(owner, Request(),
                                                             reconciled),
                 std::runtime_error);
    EXPECT_FALSE(reconciled.load());
    EXPECT_EQ(owner.commits, 1U);
  }
}

// A fake owner for `scene_owner_read`: one `ReadPublication` of one state;
// `leases_open` is its gate, `active` whether it has a projection and its
// snapshot. The separate reads must not be used: between them the Apply lane
// may change the Scene.
struct ReadOwner final {
  struct Access final {
    bool projection_leases_open{true};
    std::optional<core::ProjectionRevisionDTO> active_projection;
  };
  struct Snapshot final {
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> snapshot;
    const std::shared_ptr<const core::LogicalSceneSnapshotDTO> &shared() const {
      return snapshot;
    }
  };
  struct Read final {
    Access access;
    std::optional<Snapshot> snapshot;
  };

  bool leases_open{true};
  bool active{true};
  core::ProjectionRevisionDTO projection{
      core::RetainedBundleId(kDigest), core::GraphRevision(kDigest),
      {core::SceneId(kDigest), 7U, core::ContentDigest(kDigest)}};
  // The view hands on the read's pointer; its content is not looked at.
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> snapshot;
  mutable unsigned reads{};

  Read ReadPublication() const {
    ++reads;
    Read read{{leases_open && active, std::nullopt}, std::nullopt};
    if (active) {
      read.access.active_projection = projection;
      read.snapshot = Snapshot{snapshot};
    }
    return read;
  }
};

TEST(SceneOwnerRead, ClosedProjectionGateRefusesWithTheOwnersTypedCode) {
  ReadOwner owner;
  owner.leases_open = false;
  try {
    static_cast<void>(detail::ReadSceneOwnerView(owner));
    FAIL() << "a fenced scene owner read must refuse";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), "active_native_projection_initialization_pending");
  }
}

TEST(SceneOwnerRead, ProjectionAndSnapshotComeFromOneOwnerRead) {
  ReadOwner owner;
  const auto [projection, snapshot] = detail::ReadSceneOwnerView(owner);
  EXPECT_EQ(owner.reads, 1U);
  EXPECT_EQ(projection, owner.projection);
  EXPECT_EQ(snapshot, owner.snapshot);
  owner.active = false;  // No active projection: the gate is closed.
  EXPECT_THROW(static_cast<void>(detail::ReadSceneOwnerView(owner)), core::TypedError);
}

// A fixed box and two carried parts whose attachment ids sort the other way
// round from their object ids.
core::LogicalSceneSnapshotDTO SelectionFixture() {
  const auto box = [] {
    return std::vector<core::CollisionShapeDTO>{
        core::BoxShapeDTO{core::Vector3DTO{0.1, 0.2, 0.3}}};
  };
  const auto carried = [&](const std::string &object,
                           const std::string &attachment) {
    return std::pair{
        core::SceneObjectSnapshotDTO{
            core::SceneResourceId(object),
            box(),
            core::CarriedSceneObjectPlacementDTO{
                core::CarriedObjectAttachmentId(attachment),
                core::CarriedObjectAttachmentRevision(kDigest)},
            std::nullopt},
        core::CarriedObjectAttachmentDTO{
            core::CarriedObjectAttachmentId(attachment),
            core::CarriedObjectAttachmentRevision(kDigest),
            core::SceneResourceId(object),
            core::ControllableComponentId("arm"),
            core::CommandTargetId("tool"),
            core::ComponentSceneFrameRefDTO{core::ControllableComponentId("arm"),
                                            core::ModelInstanceId("arm-model"),
                                            core::FrameRoleId("tcp")},
            core::Pose3DTO{},
            {core::CollisionBodyRoleId("finger")}}};
  };
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U, kDigest};
  const auto [part_b, attachment_z] = carried("part-b", "z-attachment");
  const auto [part_c, attachment_a] = carried("part-c", "a-attachment");
  core::LogicalSceneSnapshotDTO snapshot{
      {core::SceneId(kDigest), 9U, core::ContentDigest(kDigest)},
      core::CellId("cell"),
      5U,
      2U,
      1U,
      policy,
      {core::SceneObjectSnapshotDTO{
           core::SceneResourceId("box-a"), box(),
           core::CellFixedSceneObjectPlacementDTO{core::CellId("cell"),
                                                  core::Pose3DTO{}},
           std::nullopt},
       part_b, part_c},
      {attachment_a, attachment_z}};
  return snapshot;
}

std::vector<core::SceneResourceId> Ids(std::vector<std::string> ids) {
  return {ids.begin(), ids.end()};
}

TEST(SceneOwnerRead, ASelectionKeepsTheNamedRowsWithTheirAttachments) {
  const auto snapshot = SelectionFixture();
  ASSERT_TRUE(snapshot.locally_valid());
  // Everything named: the snapshot itself.
  EXPECT_EQ(detail::SelectSceneResources(snapshot,
                                         Ids({"box-a", "part-b", "part-c"})),
            snapshot);

  // Two carried parts: both rows, their attachments in attachment-id order.
  const auto carried =
      detail::SelectSceneResources(snapshot, Ids({"part-b", "part-c"}));
  EXPECT_TRUE(carried.locally_valid());
  ASSERT_EQ(carried.objects.size(), 2U);
  EXPECT_EQ(carried.objects[0], snapshot.objects[1]);
  EXPECT_EQ(carried.objects[1], snapshot.objects[2]);
  EXPECT_EQ(carried.carried_object_attachments,
            snapshot.carried_object_attachments);

  // An unknown id names nothing; the fixed box has no attachment.
  const auto fixed =
      detail::SelectSceneResources(snapshot, Ids({"absent", "box-a"}));
  EXPECT_TRUE(fixed.locally_valid());
  ASSERT_EQ(fixed.objects.size(), 1U);
  EXPECT_EQ(fixed.objects[0], snapshot.objects[0]);
  EXPECT_TRUE(fixed.carried_object_attachments.empty());

  // Only unknown ids: no rows; every other member is the snapshot's own.
  const auto none = detail::SelectSceneResources(snapshot, Ids({"absent"}));
  EXPECT_TRUE(none.locally_valid());
  auto expected = snapshot;
  expected.objects.clear();
  expected.carried_object_attachments.clear();
  EXPECT_EQ(none, expected);

  // One carried part: its own attachment only; the selection encodes.
  const auto one = detail::SelectSceneResources(snapshot, Ids({"part-c"}));
  ASSERT_EQ(one.carried_object_attachments.size(), 1U);
  EXPECT_EQ(one.carried_object_attachments[0].scene_resource_id.value(),
            "part-c");
  EXPECT_FALSE(scene::EncodeLogicalSceneSnapshot(one).empty());
}

} // namespace
} // namespace manyforge::headless
