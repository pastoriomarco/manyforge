#include "manyforge_scene/canonical_empty_scene_owner.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>

namespace manyforge::scene {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-scene-manager-integration-XXXXXX")
            .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto* created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
    std::filesystem::create_directory(manager_path());
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] std::filesystem::path manager_path() const {
    return path_ / "manager";
  }

  [[nodiscard]] std::filesystem::path scene_path() const {
    return path_ / "logical-scene.sqlite3";
  }

 private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

deployment::CanonicalEmptyRetainedBundleAdmission LoadAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

TEST(CanonicalEmptySceneManagerIntegration,
     RealCoordinatorPublishesFinalizesAndRecoversAfterRestart) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  core::ProjectionRevisionDTO finalized_target = [&] {
    component_manager::ExternallyProvisionedManagerStores stores(
        directory.manager_path(), admission);
    CanonicalEmptySceneOwner scene_owner(
        directory.scene_path(), admission,
        stores.active_bundle_publication_store());
    component_manager::CanonicalEmptyBootstrapCoordinator coordinator(
        stores, scene_owner);

    const auto accepted = coordinator.BeginOrRecover(admission);
    EXPECT_EQ(
        accepted.accepted_target.canonical_empty_target.retained_bundle_id,
        admission.retained_bundle_id());
    EXPECT_EQ(
        accepted.accepted_target.canonical_empty_target.graph_revision,
        admission.graph_revision());
    EXPECT_FALSE(scene_owner.ReadAccessStatus().scene_writes_open);
    EXPECT_FALSE(scene_owner.ReadAccessStatus().projection_leases_open);

    const auto ready = coordinator.AdvanceOrRecover(admission);
    EXPECT_EQ(
        ready.state,
        core::ProjectionBootstrapStatusState::kReadyToPublish);
    EXPECT_FALSE(scene_owner.ReadAccessStatus().scene_writes_open);
    EXPECT_EQ(
        stores.StartupInventory().active_bundle_publication.receipt_count,
        0);

    const auto finalized = coordinator.AdvanceOrRecover(admission);
    EXPECT_TRUE(finalized.presence_valid());
    EXPECT_EQ(
        finalized.state,
        core::ProjectionBootstrapStatusState::kFinalized);
    EXPECT_TRUE(scene_owner.ReadAccessStatus().scene_writes_open);
    EXPECT_TRUE(scene_owner.ReadAccessStatus().projection_leases_open);
    EXPECT_EQ(
        stores.StartupInventory().active_bundle_publication.receipt_count,
        1);
    if (!finalized.publication_receipt.has_value()) {
      throw std::runtime_error(
          "finalized Manager status has no publication receipt");
    }
    const auto current_head =
        stores.active_bundle_publication_store()
            .ReadActiveBundlePublication(
                finalized.publication_receipt->publication_generation);
    if (!current_head.ok()) {
      throw std::runtime_error(
          "generation-one receipt is no longer the current head");
    }
    EXPECT_EQ(current_head.value(), *finalized.publication_receipt);
    EXPECT_EQ(scene_owner.VerifyIntegrity().finalization_row_count, 1);
    return accepted.accepted_target.canonical_empty_target;
  }();

  component_manager::ExternallyProvisionedManagerStores reopened_stores(
      directory.manager_path(), admission);
  CanonicalEmptySceneOwner reopened_scene_owner(
      directory.scene_path(), admission,
      reopened_stores.active_bundle_publication_store());
  component_manager::CanonicalEmptyBootstrapCoordinator recovered(
      reopened_stores, reopened_scene_owner);
  const auto accepted_replay = recovered.BeginOrRecover(admission);
  EXPECT_EQ(
      accepted_replay.accepted_target.canonical_empty_target,
      finalized_target);
  const auto finalized_replay = recovered.AdvanceOrRecover(admission);
  EXPECT_TRUE(finalized_replay.presence_valid());
  EXPECT_EQ(
      finalized_replay.state,
      core::ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(finalized_replay.canonical_empty_target, finalized_target);
  EXPECT_TRUE(reopened_scene_owner.ReadAccessStatus().scene_writes_open);
  EXPECT_TRUE(reopened_scene_owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(
      reopened_stores.StartupInventory()
          .active_bundle_publication.receipt_count,
      1);
  const auto scene_inventory = reopened_scene_owner.VerifyIntegrity();
  EXPECT_EQ(scene_inventory.acceptance_row_count, 1);
  EXPECT_EQ(scene_inventory.rebuild_row_count, 1);
  EXPECT_EQ(scene_inventory.finalization_row_count, 1);
}

}  // namespace
}  // namespace manyforge::scene
