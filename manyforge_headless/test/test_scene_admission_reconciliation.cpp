#include "../src/scene_admission_reconciliation_internal.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>

namespace manyforge::headless {
namespace {

namespace core = manyforge::core;

const std::string kDigest = "sha256:" + std::string(64U, 'a');

core::SceneTransactionRequestDTO Request() {
  return {{core::RequestId("scene-request"),
           core::RequestOwnerEpoch("scene-owner"), "test", std::nullopt},
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
      core::RequestId("scene-request"),
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

} // namespace
} // namespace manyforge::headless
