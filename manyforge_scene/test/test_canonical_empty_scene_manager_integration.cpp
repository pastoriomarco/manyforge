#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_fence_release_coordinator.hpp>
#include <manyforge_component_manager/manager_scene_activation_coordinator.hpp>
#include <manyforge_component_manager/manager_scene_stage_coordinator.hpp>
#include <manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "manyforge_scene/canonical_empty_scene_owner.hpp"

namespace manyforge::scene {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-scene-manager-integration-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = mkdtemp(buffer_.data());
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

constexpr std::string_view kKernelLedgerDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kKernelEvidenceDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

class ExactReleaseFencePort final : public core::IManagerKernelApplyFencePort {
 public:
  explicit ExactReleaseFencePort(component_manager::SqliteManagerApplyJournal& journal)
      : journal_(journal) {}

  [[nodiscard]] core::ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const core::ConfigurationFenceAdmissionQueryDTO&) const override {
    throw std::runtime_error(
        "admission observation is outside the completion fixture");
  }

  [[nodiscard]] core::ConfigurationFenceAcceptedDTO Acquire(
      const core::ApplyConfigurationFenceIntentDTO&) override {
    throw std::runtime_error("Acquire is outside the completion fixture");
  }

  [[nodiscard]] core::ApplyFenceAcquireAttemptReconcileResultDTO ReconcileAcquireAttempt(
      const core::ApplyFenceAcquireAttemptQueryDTO&) const override {
    throw std::runtime_error("Acquire reconciliation is outside the completion fixture");
  }

  [[nodiscard]] core::ApplyFenceReconcileResultDTO Rehydrate(
      const core::ApplyFenceRehydrateDTO&) override {
    throw std::runtime_error("rehydration is outside the completion fixture");
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO QueryStatus(
      const core::ConfigurationFenceStatusQueryDTO& query) const override {
    ++status_calls;
    const auto finalized = journal_.ReadSceneFinalized(query.owner_transaction.apply_id,
                                                       query.owner_transaction.apply_generation);
    const auto publication = journal_.ReadPublicationReceipt(
        query.owner_transaction.apply_id, query.owner_transaction.apply_generation);
    if (!finalized.has_value() || !publication.has_value()) {
      throw std::runtime_error("completion fixture lacks finalized evidence");
    }
    const auto& candidate =
        finalized->scene_finalized.finalized_result.candidate_projection_revision;
    const auto& publication_receipt = publication->publication.publication_receipt;
    core::ConfigurationFenceReconciliationEvidenceDTO evidence{
        core::ExecutionHostEpochDTO{
            core::RuntimeUnitId("runtime.kernel"),
            core::RuntimeEpoch("kernel-runtime-production-stage"),
            core::RequestOwnerEpoch("kernel-owner-production-stage"),
            core::ContentDigest(std::string(kKernelLedgerDigest)),
            publication_receipt.publication_generation,
        },
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        0U,
        candidate.graph_revision,
        candidate,
        publication_receipt.publication_generation,
        publication_receipt.durable_public_pointer_content_digest,
        {},
        0U,
        0U,
        core::ContentDigest(std::string(kKernelEvidenceDigest)),
    };
    return {
        query,
        std::move(evidence),
        released_.has_value() ? core::ConfigurationFenceState::kReleased
                              : core::ConfigurationFenceState::kHeld,
        !released_.has_value(),
        released_,
        "",
        core::ContentDigest(std::string(kKernelLedgerDigest)),
        std::nullopt,
    };
  }

  [[nodiscard]] core::ConfigurationFenceReleasedDTO Release(
      const core::ConfigurationFenceReleaseDTO& request) override {
    ++release_calls;
    const auto& owner = request.reconciliation_evidence.owner_transaction;
    const auto intent = journal_.ReadFenceReleaseIntent(owner.apply_id, owner.apply_generation);
    saw_durable_intent_before_release =
        intent.has_value() && intent->fence_release_intent.release_request == request;
    released_ = core::ConfigurationFenceReleasedDTO{
        request,
        request.reconciliation_evidence.execution_host_epoch,
        20,
        core::RequestReplayStatusDTO::kAcceptedNew,
        core::ContentDigest(std::string(kKernelEvidenceDigest)),
    };
    return *released_;
  }

  bool saw_durable_intent_before_release{false};
  mutable int status_calls{0};
  int release_calls{0};

 private:
  component_manager::SqliteManagerApplyJournal& journal_;
  std::optional<core::ConfigurationFenceReleasedDTO> released_;
};

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
    EXPECT_EQ(accepted.accepted_target.canonical_empty_target.graph_revision,
              admission.graph_revision());
    EXPECT_FALSE(scene_owner.ReadAccessStatus().scene_writes_open);
    EXPECT_FALSE(scene_owner.ReadAccessStatus().projection_leases_open);

    const auto ready = coordinator.AdvanceOrRecover(admission);
    EXPECT_EQ(ready.state,
              core::ProjectionBootstrapStatusState::kReadyToPublish);
    EXPECT_FALSE(scene_owner.ReadAccessStatus().scene_writes_open);
    EXPECT_EQ(stores.StartupInventory().active_bundle_publication.receipt_count,
              0);

    const auto finalized = coordinator.AdvanceOrRecover(admission);
    EXPECT_TRUE(finalized.presence_valid());
    EXPECT_EQ(finalized.state,
              core::ProjectionBootstrapStatusState::kFinalized);
    EXPECT_TRUE(scene_owner.ReadAccessStatus().scene_writes_open);
    EXPECT_TRUE(scene_owner.ReadAccessStatus().projection_leases_open);
    EXPECT_EQ(stores.StartupInventory().active_bundle_publication.receipt_count,
              1);
    if (!finalized.publication_receipt.has_value()) {
      throw std::runtime_error(
          "finalized Manager status has no publication receipt");
    }
    const auto current_head =
        stores.active_bundle_publication_store().ReadActiveBundlePublication(
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
  EXPECT_EQ(accepted_replay.accepted_target.canonical_empty_target,
            finalized_target);
  const auto finalized_replay = recovered.AdvanceOrRecover(admission);
  EXPECT_TRUE(finalized_replay.presence_valid());
  EXPECT_EQ(finalized_replay.state,
            core::ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(finalized_replay.canonical_empty_target, finalized_target);
  EXPECT_TRUE(reopened_scene_owner.ReadAccessStatus().scene_writes_open);
  EXPECT_TRUE(reopened_scene_owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(reopened_stores.StartupInventory()
                .active_bundle_publication.receipt_count,
            1);
  const auto scene_inventory = reopened_scene_owner.VerifyIntegrity();
  EXPECT_EQ(scene_inventory.acceptance_row_count, 1);
  EXPECT_EQ(scene_inventory.rebuild_row_count, 1);
  EXPECT_EQ(scene_inventory.finalization_row_count, 1);
}

TEST(CanonicalEmptySceneManagerIntegration,
     DurableManagerStageAndActivationComposeWithProductionOwnerAndRecover) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  {
    component_manager::ExternallyProvisionedManagerStores stores(
        directory.manager_path(), admission);
    CanonicalEmptySceneOwner scene_owner(
        directory.scene_path(), admission,
        stores.active_bundle_publication_store(), bundle_source);
    component_manager::CanonicalEmptyBootstrapCoordinator bootstrap(
        stores, scene_owner);
    const auto accepted = bootstrap.BeginOrRecover(admission);
    static_cast<void>(bootstrap.AdvanceOrRecover(admission));
    const auto finalized = bootstrap.AdvanceOrRecover(admission);
    ASSERT_EQ(finalized.state,
              core::ProjectionBootstrapStatusState::kFinalized);
    ASSERT_TRUE(finalized.publication_receipt.has_value());

    const core::ApplyId apply_id("apply-production-stage");
    const core::ApplyGeneration apply_generation(1U);
    component_manager::ManagerApplyIntentDTO intent{
        core::PublicRequestContextDTO{
            core::RequestId("request-production-stage"),
            core::RequestOwnerEpoch("manager-owner-production-stage"),
            "scene-manager-integration",
            std::nullopt,
        },
        apply_id,
        apply_generation,
        admission.cell_id(),
        accepted.accepted_target.canonical_empty_target,
        admission.retained_bundle_id(),
        admission.graph_revision(),
    };
    auto &journal = stores.apply_journal();
    const auto durable_intent = journal.EnsureIntent(intent);
    const core::ConfigurationFenceScopeDTO empty_scope{};
    const component_manager::ManagerApplyAffectedScopeDTO affected_scope{
        apply_id,
        apply_generation,
        empty_scope,
        core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
    };
    const auto durable_scope = journal.EnsureAffectedScope(
        durable_intent.record.intent_digest, affected_scope);
    const core::ApplyConfigurationFenceIntentDTO fence_intent{
        intent.request_context,
        core::ApplyFenceOwnerDTO{apply_id, apply_generation},
        empty_scope,
        core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
        intent.expected_active_projection_revision.retained_bundle_id,
        intent.expected_active_projection_revision.graph_revision,
        0U,
        core::ConfigurationFenceGeneration(0U),
        {},
        "empty runtime-only named Apply",
    };
    const auto fence_attempt = journal.EnsureFenceAttempt(
        durable_intent.record.intent_digest,
        durable_scope.record.affected_scope_digest, fence_intent);
    const auto fence_acceptance = journal.EnsureFenceAcceptance(
        apply_id, apply_generation, fence_attempt.record.fence_attempt_digest,
        core::ConfigurationFenceAcceptedDTO{
            core::ConfigurationFenceId("empty-fence-production-stage"),
            core::ConfigurationFenceGeneration(1U),
            empty_scope,
            1,
            core::RequestReplayStatusDTO::kAcceptedNew,
        });
    const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
        durable_intent.record.intent_digest,
        component_manager::ManagerApplyRuntimeConvergenceIntentDTO{
            apply_id,
            apply_generation,
            admission.retained_bundle_id(),
            admission.graph_revision(),
            fence_acceptance.record.acceptance_digest,
            finalized.publication_receipt->publication_generation,
            finalized.publication_receipt
                ->durable_public_pointer_content_digest,
            {},
        });
    static_cast<void>(journal.EnsureRuntimeStagedReady(
        component_manager::ManagerApplyRuntimeStagedReadyDTO{
            apply_id,
            apply_generation,
            1U,
            runtime_intent.record.runtime_convergence_intent_digest,
            finalized.publication_receipt->publication_generation,
            finalized.publication_receipt
                ->durable_public_pointer_content_digest,
            10U,
            {},
        }));

    component_manager::ManagerSceneStageCoordinator coordinator(
        journal, bundle_source, stores.active_bundle_publication_store(),
        scene_owner);
    const auto staged = coordinator.StageOrRecover(apply_id, apply_generation);
    EXPECT_EQ(staged.path,
              component_manager::ManagerSceneStagePath::kStagedFromNewCall);
    EXPECT_TRUE(staged.durable_scene_stage_intent.inserted);
    EXPECT_TRUE(staged.durable_scene_staged.inserted);
    EXPECT_TRUE(staged.durable_scene_stage_intent.record.scene_stage_intent
                    .native_transition_rows.empty());
    const auto owner_status =
        scene_owner.ProjectionApplyStatus({apply_id, apply_generation});
    EXPECT_EQ(owner_status.state, core::ProjectionApplyStatusState::kStaged);
    EXPECT_FALSE(scene_owner.ReadAccessStatus().scene_writes_open);
    const auto inventory = stores.StartupInventory();
    EXPECT_EQ(inventory.apply_journal.scene_stage_intent_count, 1);
    EXPECT_EQ(inventory.apply_journal.scene_staged_count, 1);
    component_manager::ManagerSceneActivationCoordinator activation(
        journal, scene_owner);
    const auto activated =
        activation.ActivateOrRecover(apply_id, apply_generation);
    EXPECT_EQ(
        activated.path,
        component_manager::ManagerSceneActivationPath::kActivatedFromNewCall);
    EXPECT_TRUE(activated.durable_selection.inserted);
    EXPECT_TRUE(activated.durable_scene_activated.inserted);
    EXPECT_EQ(
        scene_owner.ProjectionApplyStatus({apply_id, apply_generation}).state,
        core::ProjectionApplyStatusState::kActivated);
    EXPECT_FALSE(scene_owner.ReadAccessStatus().scene_writes_open);
    const auto activated_inventory = stores.StartupInventory();
    EXPECT_EQ(activated_inventory.apply_journal.activation_selection_count, 1);
    EXPECT_EQ(activated_inventory.apply_journal.scene_activated_count, 1);
    component_manager::ManagerSelectedBundleTransitionCoordinator selection(
        journal, bundle_source, stores.active_bundle_publication_store(), scene_owner);
    const auto published = selection.AdvanceOrRecover(apply_id, apply_generation);
    EXPECT_EQ(published.path,
              component_manager::ManagerSelectedBundleTransitionPath::kPublishedFromNewCall);
    EXPECT_FALSE(published.durable_finalized.has_value());
    EXPECT_FALSE(scene_owner.ReadAccessStatus().scene_writes_open);
    const auto selected = selection.AdvanceOrRecover(apply_id, apply_generation);
    EXPECT_EQ(selected.path,
              component_manager::ManagerSelectedBundleTransitionPath::kFinalizedFromNewCall);
    ASSERT_TRUE(selected.durable_finalized.has_value());
    EXPECT_TRUE(scene_owner.ReadAccessStatus().scene_writes_open);
    EXPECT_TRUE(scene_owner.ReadAccessStatus().projection_leases_open);
    const auto finalized_inventory = stores.StartupInventory();
    EXPECT_EQ(finalized_inventory.apply_journal.private_materialization_count, 1);
    EXPECT_EQ(finalized_inventory.apply_journal.retired_command_surfaces_count, 1);
    EXPECT_EQ(finalized_inventory.apply_journal.publication_receipt_count, 1);
    EXPECT_EQ(finalized_inventory.apply_journal.scene_finalized_count, 1);
    EXPECT_EQ(finalized_inventory.active_bundle_publication.receipt_count, 2);
    ExactReleaseFencePort kernel_fence(journal);
    component_manager::ManagerApplyFenceReleaseCoordinator completion(
        journal, kernel_fence, core::RuntimeEpoch("manager-runtime-production-stage"));
    const auto completed = completion.ReleaseOrRecover(apply_id, apply_generation);
    EXPECT_EQ(completed.path,
              component_manager::ManagerApplyFenceReleasePath::kReleasedFromNewCall);
    EXPECT_TRUE(completed.durable_release_intent.inserted);
    EXPECT_TRUE(completed.durable_release.inserted);
    EXPECT_TRUE(completed.durable_terminal_outcome.inserted);
    EXPECT_TRUE(kernel_fence.saw_durable_intent_before_release);
    EXPECT_EQ(kernel_fence.status_calls, 1);
    EXPECT_EQ(kernel_fence.release_calls, 1);
    const auto completed_inventory = stores.StartupInventory();
    EXPECT_EQ(completed_inventory.apply_journal.fence_release_intent_count, 1);
    EXPECT_EQ(completed_inventory.apply_journal.fence_released_count, 1);
    EXPECT_EQ(completed_inventory.apply_journal.terminal_outcome_count, 1);
    scene_owner.Close();
  }

  component_manager::ExternallyProvisionedManagerStores reopened_stores(
      directory.manager_path(), admission);
  CanonicalEmptySceneOwner reopened_scene_owner(
      directory.scene_path(), admission,
      reopened_stores.active_bundle_publication_store(), bundle_source);
  component_manager::ManagerSceneStageCoordinator recovered(
      reopened_stores.apply_journal(), bundle_source,
      reopened_stores.active_bundle_publication_store(), reopened_scene_owner);
  const auto replay = recovered.StageOrRecover(
      core::ApplyId("apply-production-stage"), core::ApplyGeneration(1U));
  EXPECT_EQ(replay.path,
            component_manager::ManagerSceneStagePath::kDurableStagedReplay);
  EXPECT_FALSE(replay.durable_scene_stage_intent.inserted);
  EXPECT_FALSE(replay.durable_scene_staged.inserted);
  component_manager::ManagerSceneActivationCoordinator recovered_activation(
      reopened_stores.apply_journal(), reopened_scene_owner);
  const auto activation_replay = recovered_activation.ActivateOrRecover(
      core::ApplyId("apply-production-stage"), core::ApplyGeneration(1U));
  EXPECT_EQ(
      activation_replay.path,
      component_manager::ManagerSceneActivationPath::kDurableActivatedReplay);
  EXPECT_FALSE(activation_replay.durable_selection.inserted);
  EXPECT_FALSE(activation_replay.durable_scene_activated.inserted);
  component_manager::ManagerSelectedBundleTransitionCoordinator recovered_selection(
      reopened_stores.apply_journal(), bundle_source,
      reopened_stores.active_bundle_publication_store(), reopened_scene_owner);
  const auto selected_replay = recovered_selection.AdvanceOrRecover(
      core::ApplyId("apply-production-stage"), core::ApplyGeneration(1U));
  EXPECT_EQ(selected_replay.path,
            component_manager::ManagerSelectedBundleTransitionPath::kDurableFinalizedReplay);
  ASSERT_TRUE(selected_replay.durable_finalized.has_value());
  EXPECT_FALSE(selected_replay.durable_finalized->inserted);
  EXPECT_EQ(reopened_scene_owner
                .ProjectionApplyStatus({core::ApplyId("apply-production-stage"),
                                        core::ApplyGeneration(1U)})
                .state,
            core::ProjectionApplyStatusState::kFinalized);
  EXPECT_TRUE(reopened_scene_owner.ReadAccessStatus().scene_writes_open);
  EXPECT_TRUE(reopened_scene_owner.ReadAccessStatus().projection_leases_open);
  ExactReleaseFencePort recovered_kernel(reopened_stores.apply_journal());
  component_manager::ManagerApplyFenceReleaseCoordinator recovered_completion(
      reopened_stores.apply_journal(), recovered_kernel,
      core::RuntimeEpoch("manager-runtime-production-stage-restarted"));
  const auto completed_replay = recovered_completion.ReleaseOrRecover(
      core::ApplyId("apply-production-stage"), core::ApplyGeneration(1U));
  EXPECT_EQ(completed_replay.path,
            component_manager::ManagerApplyFenceReleasePath::kDurableTerminalReplay);
  EXPECT_FALSE(completed_replay.durable_terminal_outcome.inserted);
  EXPECT_EQ(recovered_kernel.status_calls, 0);
  EXPECT_EQ(recovered_kernel.release_calls, 0);
}

}  // namespace
}  // namespace manyforge::scene
