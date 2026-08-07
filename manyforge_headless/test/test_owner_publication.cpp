#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "manyforge_headless/owner_publication.hpp"

namespace manyforge::headless {
namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kSceneId =
    "sha256:abababababababababababababababababababababababababababababababab";

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-owner-publication-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("owner-publication test mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

deployment::CanonicalEmptyRetainedBundleAdmission LoadAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

core::SceneRevision SceneRevision() {
  return {
      core::SceneId(std::string(kSceneId)),
      1U,
      core::ContentDigest(std::string(kDigestOne)),
  };
}

core::ProjectionRevisionDTO ProjectionRevision(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  return {
      admission.retained_bundle_id(),
      admission.graph_revision(),
      SceneRevision(),
  };
}

core::ActiveBundlePublicationReceiptDTO Publication(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  const auto projection = ProjectionRevision(admission);
  return {
      core::ActiveBundlePublicationRequestDTO{
          core::BootstrapPublicationTransactionDTO{
              core::BootstrapId("bootstrap-1"),
              core::BootstrapGeneration(1U),
          },
          projection,
          projection.retained_bundle_id,
          projection.graph_revision,
          std::nullopt,
          core::ContentDigest(std::string(kDigestOne)),
          core::ContentDigest(std::string(kDigestTwo)),
      },
      core::ActiveBundlePublicationGeneration(1U),
      core::ContentDigest(std::string(kDigestTwo)),
  };
}

component_manager::ManagerActiveRuntimeConvergenceResult
Runtime(const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  component_manager::RuntimeObservationReducer reducer([] { return 42U; }, {});
  return {
      component_manager::ManagerActiveRuntimePlanDTO{
          Publication(admission),
          std::nullopt,
          {},
      },
      reducer.Evaluate({}),
      component_manager::ManagerActiveRuntimeConvergenceState::kConverged,
  };
}

component_manager::ManagerApplyIntentDTO ApplyIntent(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  return {
      core::PublicRequestContextDTO{
          core::RequestId("request-1"),
          core::RequestOwnerEpoch("manager-client-epoch-1"),
          "owner-publication-test",
          std::nullopt,
      },
      core::ApplyId("apply-1"),
      core::ApplyGeneration(1U),
      admission.cell_id(),
      ProjectionRevision(admission),
      admission.retained_bundle_id(),
      admission.graph_revision(),
  };
}

core::LogicalSceneSnapshotRoot SceneSnapshot(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "free_space";
  policy.revision = {
      1U,
      std::string(core::kCanonicalEmptyCollisionPolicyDigest),
  };
  return core::LogicalSceneSnapshotRoot(
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          core::LogicalSceneSnapshotDTO{
              SceneRevision(),
              admission.cell_id(),
              1U,
              1U,
              1U,
              std::move(policy),
              {},
              {},
          }));
}

core::LogicalSceneSnapshotRoot SceneSnapshotWithSupport(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  auto policy = SceneSnapshot(admission).shared()->configured_collision_policy;
  const auto fixed_placement = core::CellFixedSceneObjectPlacementDTO{
      admission.cell_id(),
      core::Pose3DTO{
          core::Vector3DTO{0.0, 0.0, 0.0},
          core::QuaternionDTO{0.0, 0.0, 0.0, 1.0},
      },
  };
  return core::LogicalSceneSnapshotRoot(
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          core::LogicalSceneSnapshotDTO{
              SceneRevision(),
              admission.cell_id(),
              2U,
              2U,
              1U,
              std::move(policy),
              {
                  core::SceneObjectSnapshotDTO{
                      core::SceneResourceId("part"),
                      {core::SphereShapeDTO{0.01}},
                      fixed_placement,
                      std::nullopt,
                      {core::SupportContactDTO{.support_object_id = "table"}},
                  },
                  core::SceneObjectSnapshotDTO{
                      core::SceneResourceId("table"),
                      {core::BoxShapeDTO{core::Vector3DTO{1.0, 1.0, 0.1}}},
                      fixed_placement,
                      std::nullopt,
                      {},
                  },
              },
              {},
          }));
}

TEST(ManagerSourcePublication,
     ProjectsVerifiedActiveGraphAndExplicitKernelUnavailability) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());

  const auto payload = BuildManagerSourcePayload(
      admission.retained_bundle(), Runtime(admission), journal,
      ManagerApplyMutationAvailability::kKernelOwnerUnavailable);
  const auto bytes = content_identity::canonical_json(payload);
  EXPECT_NE(bytes.find("\"api_version\":\"manyforge.manager_source.v1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"cell_graph\":"), std::string::npos);
  EXPECT_NE(bytes.find("\"manager_ready\":true"), std::string::npos);
  EXPECT_NE(bytes.find("\"mutation_available\":false"), std::string::npos);
  EXPECT_NE(
      bytes.find("\"unavailable_reason_code\":\"kernel_owner_unavailable\""),
      std::string::npos);
  EXPECT_NE(bytes.find("\"records\":[]"), std::string::npos);
}

TEST(ManagerSourcePublication,
     ProjectsOneDurableIncompleteApplyWithoutInventingKernelState) {
  const auto admission = LoadAdmission();
  TemporaryDirectory directory;
  component_manager::SqliteManagerApplyJournal journal(
      directory.path() / "apply.sqlite3", admission.cell_id());
  const auto intent = journal.EnsureIntent(ApplyIntent(admission)).record;
  static_cast<void>(journal.EnsureAffectedScope(
      intent.intent_digest,
      component_manager::ManagerApplyAffectedScopeDTO{
          intent.intent.apply_id,
          intent.intent.apply_generation,
          {},
          core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      }));

  const auto bytes = content_identity::canonical_json(BuildManagerSourcePayload(
      admission.retained_bundle(), Runtime(admission), journal,
      ManagerApplyMutationAvailability::kKernelOwnerUnavailable));
  EXPECT_NE(bytes.find("\"apply_id\":\"apply-1\""), std::string::npos);
  EXPECT_NE(bytes.find("\"phase\":\"scope_persisted\""), std::string::npos);
  EXPECT_NE(bytes.find("\"state\":\"scope_persisted\""), std::string::npos);
  EXPECT_NE(bytes.find("\"incomplete\":true"), std::string::npos);
  EXPECT_EQ(bytes.find("\"configuration_fence_id\":\""), std::string::npos);
}

TEST(LogicalSceneSourcePublication,
     DistinguishesUnavailableBootstrapFromAnActiveImmutableSnapshot) {
  const auto admission = LoadAdmission();
  const auto unavailable = BuildLogicalSceneSourcePayload(
      manyforge::scene::CanonicalEmptySceneAccessStatus{
          manyforge::scene::CanonicalEmptySceneBootstrapPhase::
              kInitialCommandFenced,
          false,
          false,
          std::nullopt,
      },
      std::nullopt);
  const auto unavailable_bytes = content_identity::canonical_json(unavailable);
  EXPECT_NE(unavailable_bytes.find("\"phase\":\"initial_command_fenced\""),
            std::string::npos);
  EXPECT_NE(unavailable_bytes.find("\"snapshot\":null"), std::string::npos);

  const auto projection = ProjectionRevision(admission);
  const auto snapshot = SceneSnapshot(admission);
  const auto active = BuildLogicalSceneSourcePayload(
      manyforge::scene::CanonicalEmptySceneAccessStatus{
          manyforge::scene::CanonicalEmptySceneBootstrapPhase::kFinalized,
          true,
          true,
          projection,
      },
      snapshot);
  const auto active_bytes = content_identity::canonical_json(active);
  EXPECT_NE(active_bytes.find("\"phase\":\"finalized\""), std::string::npos);
  EXPECT_NE(active_bytes.find("\"scene_writes_open\":true"), std::string::npos);
  EXPECT_NE(active_bytes.find("\"policy_id\":\"free_space\""),
            std::string::npos);
  EXPECT_NE(active_bytes.find("\"objects\":[]"), std::string::npos);
}

TEST(LogicalSceneSourcePublication, RejectsCrossOwnerRevisionMismatch) {
  const auto admission = LoadAdmission();
  auto projection = ProjectionRevision(admission);
  projection.scene_revision.revision_id = 2U;
  EXPECT_THROW(
      BuildLogicalSceneSourcePayload(
          manyforge::scene::CanonicalEmptySceneAccessStatus{
              manyforge::scene::CanonicalEmptySceneBootstrapPhase::kFinalized,
              true,
              true,
              projection,
          },
          SceneSnapshot(admission)),
      std::invalid_argument);
}

TEST(LogicalSceneSourcePublication,
     PublishesPortableSupportDeclarationWithoutNativeBodyNames) {
  const auto admission = LoadAdmission();
  const auto projection = ProjectionRevision(admission);
  const auto bytes =
      content_identity::canonical_json(BuildLogicalSceneSourcePayload(
          manyforge::scene::CanonicalEmptySceneAccessStatus{
              manyforge::scene::CanonicalEmptySceneBootstrapPhase::kFinalized,
              true,
              true,
              projection,
          },
          SceneSnapshotWithSupport(admission)));

  EXPECT_NE(bytes.find("\"support_object_id\":\"table\""), std::string::npos);
  EXPECT_NE(bytes.find("\"mode\":\"contact_release\""), std::string::npos);
  EXPECT_EQ(bytes.find("scene/part/shape/0"), std::string::npos);
}

TEST(SceneRuntimeSourcePublication,
     WrapsTheSeparateInitializationAndPoseLifetimeOwner) {
  std::int64_t now = 10;
  SceneRuntimeStateOwner owner([&] { return now; }, [] { return "token-1"; });
  owner.EstablishOpenRuntime(
      SceneRuntimeIdentity{"bundle-1", "graph-1", 1U, 0U, 1U}, 1U, "bootstrap");
  const auto bytes = content_identity::canonical_json(
      BuildSceneRuntimeSourcePayload("cell-a", owner.Snapshot()));
  EXPECT_NE(bytes.find("\"api_version\":\"manyforge.scene_runtime_source.v1\""),
            std::string::npos);
  EXPECT_NE(bytes.find("\"cell_id\":\"cell-a\""), std::string::npos);
  EXPECT_NE(bytes.find("\"state\":\"open\""), std::string::npos);
  EXPECT_THROW(BuildSceneRuntimeSourcePayload("", owner.Snapshot()),
               std::invalid_argument);
}

} // namespace
} // namespace manyforge::headless
