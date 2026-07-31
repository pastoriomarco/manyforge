#include "manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>

#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_deployment/canonical_empty_admission.hpp>

namespace manyforge::component_manager {
namespace {

using core::ActiveBundlePublicationReceiptDTO;
using core::ActiveBundlePublicationRequestDTO;
using core::BootstrapGeneration;
using core::BootstrapId;
using core::BootstrapProjectionSetSourceDTO;
using core::ContentDigest;
using core::ErrorDTO;
using core::ProjectionAggregateHealthDTO;
using core::ProjectionBootstrapAcceptedDTO;
using core::ProjectionBootstrapEvidenceDTO;
using core::ProjectionBootstrapFinalizeDTO;
using core::ProjectionBootstrapFinalizeResultDTO;
using core::ProjectionBootstrapFinalizeResultState;
using core::ProjectionBootstrapIntentDTO;
using core::ProjectionBootstrapPrerequisiteQueryDTO;
using core::ProjectionBootstrapPrerequisiteStatusDTO;
using core::ProjectionBootstrapRebuildRequestDTO;
using core::ProjectionBootstrapRebuildResultDTO;
using core::ProjectionBootstrapRebuildResultState;
using core::ProjectionBootstrapStatusDTO;
using core::ProjectionBootstrapStatusQueryDTO;
using core::ProjectionBootstrapStatusState;
using core::ProjectionHealthState;
using core::ProjectionRevisionDTO;
using core::SceneId;
using core::SceneRevision;

constexpr std::string_view kSceneIdDigest =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

template <typename T>
concept HasPublicAdmissionMatcher = requires(
    const T& stores,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  stores.matches_admission(admission);
};

static_assert(
    !HasPublicAdmissionMatcher<ExternallyProvisionedManagerStores>);

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-bootstrap-advance-test-XXXXXX")
            .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto* created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

deployment::CanonicalEmptyRetainedBundleAdmission LoadAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

ProjectionRevisionDTO Target(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  return {
      admission.retained_bundle_id(),
      admission.graph_revision(),
      SceneRevision{
          SceneId(std::string(kSceneIdDigest)),
          1U,
          ContentDigest(
              "sha256:"
              "111111111111111111111111111111111111111111111111111111111111"
              "1111"),
      },
  };
}

struct SeededBootstrap final {
  ManagerBootstrapIntentDTO intent;
  ManagerBootstrapAcceptedTargetDTO accepted_target;
};

SeededBootstrap SeedAccepted(
    ExternallyProvisionedManagerStores& stores,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  SeededBootstrap seeded{
      {
          BootstrapId("bootstrap-0123456789abcdef0123456789abcdef"),
          BootstrapGeneration(1U),
          admission.cell_id(),
          admission.retained_bundle_id(),
          admission.graph_revision(),
          {},
      },
      {
          BootstrapId("bootstrap-0123456789abcdef0123456789abcdef"),
          BootstrapGeneration(1U),
          Target(admission),
      },
  };
  EXPECT_TRUE(stores.bootstrap_journal().EnsureIntent(seeded.intent).inserted);
  EXPECT_TRUE(
      stores.bootstrap_journal()
          .RecordAcceptedTarget(seeded.accepted_target)
          .inserted);
  return seeded;
}

ProjectionAggregateHealthDTO ZeroNativeHealth(
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  return {
      BootstrapProjectionSetSourceDTO{
          intent.bootstrap_id,
          intent.bootstrap_generation,
          target.canonical_empty_target,
      },
      ProjectionHealthState::kHealthy,
      {},
      std::nullopt,
  };
}

ProjectionBootstrapEvidenceDTO ZeroNativeEvidence(
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  return {
      intent.bootstrap_id,
      intent.bootstrap_generation,
      target.canonical_empty_target,
      {},
      ZeroNativeHealth(intent, target),
  };
}

void AppendExpectedField(
    std::string& record,
    const std::string_view name,
    const std::string_view value) {
  record.append(std::to_string(name.size()));
  record.push_back(':');
  record.append(name);
  record.push_back('=');
  record.append(std::to_string(value.size()));
  record.push_back(':');
  record.append(value);
  record.push_back('\n');
}

ContentDigest ExpectedZeroCaseDigest(
    const std::string_view domain,
    const std::string_view count_name,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  std::string record;
  AppendExpectedField(record, "domain", domain);
  AppendExpectedField(
      record, "admission_digest", admission.admission_digest().value());
  AppendExpectedField(record, "cell_id", admission.cell_id().value());
  AppendExpectedField(
      record, "retained_bundle_id", admission.retained_bundle_id().value());
  AppendExpectedField(
      record, "graph_revision", admission.graph_revision().value());
  AppendExpectedField(
      record, "bootstrap_id", intent.bootstrap_id.value());
  AppendExpectedField(
      record,
      "bootstrap_generation",
      std::to_string(intent.bootstrap_generation.value()));
  AppendExpectedField(
      record,
      "target_retained_bundle_id",
      target.canonical_empty_target.retained_bundle_id.value());
  AppendExpectedField(
      record,
      "target_graph_revision",
      target.canonical_empty_target.graph_revision.value());
  AppendExpectedField(
      record,
      "target_scene_id",
      target.canonical_empty_target.scene_revision.scene_id.value());
  AppendExpectedField(
      record,
      "target_scene_revision",
      std::to_string(
          target.canonical_empty_target.scene_revision.revision_id));
  AppendExpectedField(
      record,
      "target_scene_content_digest",
      target.canonical_empty_target.scene_revision.content_hash.value());
  AppendExpectedField(record, count_name, "0");

  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned int output_length = 0U;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    throw std::runtime_error("cannot allocate test SHA-256 context");
  }
  const auto release = [&]() { EVP_MD_CTX_free(context); };
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, record.data(), record.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &output_length) != 1) {
    release();
    throw std::runtime_error("test SHA-256 computation failed");
  }
  release();
  constexpr std::array<char, 16> kHex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string digest("sha256:");
  digest.reserve(7U + (output_length * 2U));
  for (const auto byte :
       std::span<const unsigned char>(output.data(), output_length)) {
    digest.push_back(kHex[byte >> 4U]);
    digest.push_back(kHex[byte & 0x0fU]);
  }
  return ContentDigest(std::move(digest));
}

class RecoverableSceneOwner final
    : public core::IManagerSceneProjectionBootstrapPort {
 public:
  RecoverableSceneOwner(
      ManagerBootstrapIntentDTO intent,
      ManagerBootstrapAcceptedTargetDTO target,
      const core::IActiveBundlePublicationReader& publication_reader)
      : intent_(std::move(intent)),
        target_(std::move(target)),
        evidence_(ZeroNativeEvidence(intent_, target_)),
        publication_reader_(publication_reader) {}

  void LoseNextRebuildResponse() {
    std::lock_guard lock(mutex_);
    lose_next_rebuild_response_ = true;
  }

  void LoseNextFinalizeResponse() {
    std::lock_guard lock(mutex_);
    lose_next_finalize_response_ = true;
  }

  void FailNextFinalizeBeforeEffect() {
    std::lock_guard lock(mutex_);
    fail_next_finalize_before_effect_ = true;
  }

  void SetRebuilding() {
    std::lock_guard lock(mutex_);
    state_ = ProjectionBootstrapStatusState::kRebuilding;
  }

  void SetAccepted() {
    std::lock_guard lock(mutex_);
    state_ = ProjectionBootstrapStatusState::kAccepted;
  }

  void SetUncertain() {
    std::lock_guard lock(mutex_);
    state_ = ProjectionBootstrapStatusState::kFailedProjectionUncertain;
  }

  void SetStatusTargetMismatch() {
    std::lock_guard lock(mutex_);
    status_target_mismatch_ = true;
  }

  void SetNextStatusHook(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    next_status_hook_ = std::move(hook);
  }

  void SetNextStatusReturnHook(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    next_status_return_hook_ = std::move(hook);
  }

  [[nodiscard]] std::size_t rebuild_calls() const {
    std::lock_guard lock(mutex_);
    return rebuild_calls_;
  }

  [[nodiscard]] std::size_t rebuild_effects() const {
    std::lock_guard lock(mutex_);
    return rebuild_effects_;
  }

  [[nodiscard]] std::size_t finalize_calls() const {
    std::lock_guard lock(mutex_);
    return finalize_calls_;
  }

  [[nodiscard]] std::size_t finalize_effects() const {
    std::lock_guard lock(mutex_);
    return finalize_effects_;
  }

  ProjectionBootstrapPrerequisiteStatusDTO
  ObserveProjectionBootstrapPrerequisites(
      const ProjectionBootstrapPrerequisiteQueryDTO&) const override {
    throw std::logic_error("Advance must not observe Begin prerequisites");
  }

  ProjectionBootstrapAcceptedDTO BeginProjectionBootstrap(
      const ProjectionBootstrapIntentDTO&) override {
    throw std::logic_error("Advance must not call Begin");
  }

  ProjectionBootstrapRebuildResultDTO RebuildProjectionBootstrap(
      const ProjectionBootstrapRebuildRequestDTO& request) override {
    std::lock_guard lock(mutex_);
    ++rebuild_calls_;
    if (!request.valid() ||
        request.bootstrap_id != intent_.bootstrap_id ||
        request.bootstrap_generation != intent_.bootstrap_generation ||
        request.canonical_empty_target !=
            target_.canonical_empty_target ||
        state_ != ProjectionBootstrapStatusState::kAccepted) {
      throw std::runtime_error("unexpected Rebuild request");
    }
    state_ = ProjectionBootstrapStatusState::kReadyToPublish;
    ++rebuild_effects_;
    if (lose_next_rebuild_response_) {
      lose_next_rebuild_response_ = false;
      throw std::runtime_error("lost Rebuild response");
    }
    return {
        intent_.bootstrap_id,
        intent_.bootstrap_generation,
        target_.canonical_empty_target,
        ProjectionBootstrapRebuildResultState::kReadyToPublish,
        evidence_,
        std::nullopt,
    };
  }

  ProjectionBootstrapStatusDTO ProjectionBootstrapStatus(
      const ProjectionBootstrapStatusQueryDTO& query) const override {
    std::function<void()> hook;
    {
      std::lock_guard lock(mutex_);
      hook = std::move(next_status_hook_);
    }
    if (hook) {
      hook();
    }
    ProjectionBootstrapStatusDTO result = [&] {
      std::lock_guard lock(mutex_);
      if (!query.valid() ||
          query.bootstrap_id != intent_.bootstrap_id ||
          query.bootstrap_generation != intent_.bootstrap_generation) {
        throw std::runtime_error("unexpected status query");
      }
      auto target = target_.canonical_empty_target;
      if (status_target_mismatch_) {
        target.scene_revision.revision_id += 1U;
      }
      const auto error =
          state_ ==
                  ProjectionBootstrapStatusState::
                      kFailedProjectionUncertain
              ? std::optional<ErrorDTO>(
                    ErrorDTO{
                        "projection_uncertain",
                        "test projection uncertainty",
                        {}})
              : std::nullopt;
      return ProjectionBootstrapStatusDTO{
          intent_.bootstrap_id,
          intent_.bootstrap_generation,
          state_,
          target,
          {},
          state_ ==
                      ProjectionBootstrapStatusState::kReadyToPublish ||
                  state_ == ProjectionBootstrapStatusState::kPublished ||
                  state_ == ProjectionBootstrapStatusState::kFinalized
              ? std::optional<ProjectionBootstrapEvidenceDTO>(evidence_)
              : std::nullopt,
          state_ == ProjectionBootstrapStatusState::kPublished ||
                  state_ == ProjectionBootstrapStatusState::kFinalized
              ? publication_receipt_
              : std::nullopt,
          state_ == ProjectionBootstrapStatusState::kFinalized
              ? std::optional<ProjectionAggregateHealthDTO>(
                    ZeroNativeHealth(intent_, target_))
              : std::nullopt,
          error,
      };
    }();
    std::function<void()> return_hook;
    {
      std::lock_guard lock(mutex_);
      return_hook = std::move(next_status_return_hook_);
    }
    if (return_hook) {
      return_hook();
    }
    return result;
  }

  ProjectionBootstrapFinalizeResultDTO FinalizeProjectionBootstrap(
      const ProjectionBootstrapFinalizeDTO& request) override {
    std::lock_guard lock(mutex_);
    ++finalize_calls_;
    if (fail_next_finalize_before_effect_) {
      fail_next_finalize_before_effect_ = false;
      throw std::runtime_error("Finalize failed before effect");
    }
    if (!request.valid() ||
        request.bootstrap_id != intent_.bootstrap_id ||
        request.bootstrap_generation != intent_.bootstrap_generation ||
        request.canonical_empty_target !=
            target_.canonical_empty_target ||
        (state_ != ProjectionBootstrapStatusState::kReadyToPublish &&
         state_ != ProjectionBootstrapStatusState::kPublished &&
         state_ != ProjectionBootstrapStatusState::kFinalized)) {
      throw std::runtime_error("unexpected Finalize request");
    }
    const auto durable =
        publication_reader_.ReadActiveBundlePublication(
            request.publication_receipt.publication_generation);
    if (!durable.ok() ||
        durable.value() != request.publication_receipt) {
      throw std::runtime_error("Finalize cannot verify durable publication");
    }
    if (state_ != ProjectionBootstrapStatusState::kFinalized) {
      state_ = ProjectionBootstrapStatusState::kFinalized;
      publication_receipt_ = request.publication_receipt;
      ++finalize_effects_;
    } else if (!publication_receipt_.has_value() ||
               *publication_receipt_ != request.publication_receipt) {
      throw std::runtime_error("conflicting Finalize replay");
    }
    if (lose_next_finalize_response_) {
      lose_next_finalize_response_ = false;
      throw std::runtime_error("lost Finalize response");
    }
    return {
        intent_.bootstrap_id,
        intent_.bootstrap_generation,
        target_.canonical_empty_target,
        request.publication_receipt,
        ProjectionBootstrapFinalizeResultState::kFinalized,
        evidence_,
        ZeroNativeHealth(intent_, target_),
        std::nullopt,
    };
  }

 private:
  ManagerBootstrapIntentDTO intent_;
  ManagerBootstrapAcceptedTargetDTO target_;
  ProjectionBootstrapEvidenceDTO evidence_;
  const core::IActiveBundlePublicationReader& publication_reader_;
  mutable std::mutex mutex_;
  ProjectionBootstrapStatusState state_{
      ProjectionBootstrapStatusState::kAccepted};
  std::optional<ActiveBundlePublicationReceiptDTO> publication_receipt_;
  bool lose_next_rebuild_response_{};
  bool lose_next_finalize_response_{};
  bool fail_next_finalize_before_effect_{};
  bool status_target_mismatch_{};
  mutable std::function<void()> next_status_hook_;
  mutable std::function<void()> next_status_return_hook_;
  std::size_t rebuild_calls_{};
  std::size_t rebuild_effects_{};
  std::size_t finalize_calls_{};
  std::size_t finalize_effects_{};
};

TEST(CanonicalEmptyBootstrapAdvance, RebuildPublishFinalizeAndReplay) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);

  const auto ready = coordinator.AdvanceOrRecover(admission);
  ASSERT_EQ(
      ready.state, ProjectionBootstrapStatusState::kReadyToPublish);
  EXPECT_EQ(owner.rebuild_calls(), 1U);
  EXPECT_EQ(owner.rebuild_effects(), 1U);
  EXPECT_EQ(owner.finalize_calls(), 0U);
  EXPECT_EQ(
      stores.StartupInventory().active_bundle_publication.receipt_count,
      0);

  const auto finalized = coordinator.AdvanceOrRecover(admission);
  ASSERT_EQ(finalized.state, ProjectionBootstrapStatusState::kFinalized);
  ASSERT_TRUE(finalized.publication_receipt.has_value());
  const auto& request = finalized.publication_receipt->request;
  EXPECT_EQ(
      request.private_materialization_content_digest,
      ExpectedZeroCaseDigest(
          "manyforge.component-manager.canonical-empty."
          "private-materialization.v1",
          "native_materialization_count",
          admission,
          seeded.intent,
          seeded.accepted_target));
  EXPECT_EQ(
      request.obsolete_managed_command_surfaces_retired_evidence_digest,
      ExpectedZeroCaseDigest(
          "manyforge.component-manager.canonical-empty."
          "retired-managed-command-surfaces.v1",
          "retired_managed_command_surface_count",
          admission,
          seeded.intent,
          seeded.accepted_target));
  EXPECT_NE(
      request.private_materialization_content_digest,
      request.obsolete_managed_command_surfaces_retired_evidence_digest);
  EXPECT_EQ(owner.finalize_calls(), 1U);
  EXPECT_EQ(owner.finalize_effects(), 1U);

  const auto replay = coordinator.AdvanceOrRecover(admission);
  EXPECT_EQ(replay.state, finalized.state);
  EXPECT_EQ(replay.bootstrap_id, finalized.bootstrap_id);
  EXPECT_EQ(replay.bootstrap_generation, finalized.bootstrap_generation);
  EXPECT_EQ(replay.canonical_empty_target, finalized.canonical_empty_target);
  EXPECT_EQ(replay.publication_receipt, finalized.publication_receipt);
  EXPECT_EQ(owner.rebuild_calls(), 1U);
  EXPECT_EQ(owner.finalize_calls(), 1U);
  const auto inventory = stores.StartupInventory();
  EXPECT_EQ(inventory.active_bundle_publication.receipt_count, 1);
  EXPECT_EQ(
      inventory.active_bundle_publication.current_generation.value(), 1U);

  SqliteActiveBundlePublicationStore reopened(
      stores.active_bundle_publication_store().database_path(),
      admission.cell_id());
  const auto reread = reopened.ReadActiveBundlePublication(
      finalized.publication_receipt->publication_generation);
  ASSERT_TRUE(reread.ok());
  EXPECT_EQ(reread.value(), *finalized.publication_receipt);
}

TEST(CanonicalEmptyBootstrapAdvance, RecoversLostRebuildResponse) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  owner.LoseNextRebuildResponse();
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);

  const auto ready = coordinator.AdvanceOrRecover(admission);
  EXPECT_EQ(
      ready.state, ProjectionBootstrapStatusState::kReadyToPublish);
  EXPECT_EQ(owner.rebuild_calls(), 1U);
  EXPECT_EQ(owner.rebuild_effects(), 1U);
  EXPECT_EQ(owner.finalize_calls(), 0U);
}

TEST(CanonicalEmptyBootstrapAdvance, RebuildingIsObservedWithoutAnotherEffect) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  owner.SetRebuilding();
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);

  const auto rebuilding = coordinator.AdvanceOrRecover(admission);
  EXPECT_EQ(
      rebuilding.state, ProjectionBootstrapStatusState::kRebuilding);
  EXPECT_EQ(owner.rebuild_calls(), 0U);
  EXPECT_EQ(owner.finalize_calls(), 0U);
}

TEST(
    CanonicalEmptyBootstrapAdvance,
    ExactPublishedHeadSurvivesFinalizeFailureAndBeginReplay) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);
  ASSERT_EQ(
      coordinator.AdvanceOrRecover(admission).state,
      ProjectionBootstrapStatusState::kReadyToPublish);
  owner.FailNextFinalizeBeforeEffect();

  EXPECT_THROW(
      coordinator.AdvanceOrRecover(admission),
      CanonicalEmptyBootstrapCoordinatorError);
  EXPECT_EQ(
      stores.StartupInventory().active_bundle_publication.receipt_count,
      1);

  const auto accepted_replay = coordinator.BeginOrRecover(admission);
  EXPECT_EQ(
      accepted_replay.accepted_target, seeded.accepted_target);
  const auto finalized = coordinator.AdvanceOrRecover(admission);
  EXPECT_EQ(finalized.state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(owner.finalize_effects(), 1U);
}

TEST(CanonicalEmptyBootstrapAdvance, RecoversLostFinalizeResponse) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);
  ASSERT_EQ(
      coordinator.AdvanceOrRecover(admission).state,
      ProjectionBootstrapStatusState::kReadyToPublish);
  owner.LoseNextFinalizeResponse();

  const auto finalized = coordinator.AdvanceOrRecover(admission);
  EXPECT_EQ(finalized.state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(owner.finalize_calls(), 1U);
  EXPECT_EQ(owner.finalize_effects(), 1U);
}

TEST(CanonicalEmptyBootstrapAdvance, UncertainOrMismatchedStatusFailsClosed) {
  {
    TemporaryDirectory directory;
    const auto admission = LoadAdmission();
    ExternallyProvisionedManagerStores stores(directory.path(), admission);
    const auto seeded = SeedAccepted(stores, admission);
    RecoverableSceneOwner owner(
        seeded.intent,
        seeded.accepted_target,
        stores.active_bundle_publication_store());
    owner.SetUncertain();
    CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);
    EXPECT_THROW(
        coordinator.AdvanceOrRecover(admission),
        CanonicalEmptyBootstrapCoordinatorError);
    EXPECT_EQ(owner.rebuild_calls(), 0U);
    EXPECT_EQ(owner.finalize_calls(), 0U);
  }
  {
    TemporaryDirectory directory;
    const auto admission = LoadAdmission();
    ExternallyProvisionedManagerStores stores(directory.path(), admission);
    const auto seeded = SeedAccepted(stores, admission);
    RecoverableSceneOwner owner(
        seeded.intent,
        seeded.accepted_target,
        stores.active_bundle_publication_store());
    owner.SetStatusTargetMismatch();
    CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);
    EXPECT_THROW(
        coordinator.AdvanceOrRecover(admission),
        CanonicalEmptyBootstrapCoordinatorError);
    EXPECT_EQ(owner.rebuild_calls(), 0U);
    EXPECT_EQ(owner.finalize_calls(), 0U);
  }
}

TEST(CanonicalEmptyBootstrapAdvance, MismatchedPublicationFailsBeforeEffect) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  const ActiveBundlePublicationRequestDTO wrong_request{
      core::BootstrapPublicationTransactionDTO{
          seeded.intent.bootstrap_id,
          seeded.intent.bootstrap_generation,
      },
      seeded.accepted_target.canonical_empty_target,
      seeded.intent.canonical_empty_retained_bundle_id,
      seeded.intent.canonical_empty_graph_revision,
      std::nullopt,
      ContentDigest(
          "sha256:"
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
      ContentDigest(
          "sha256:"
          "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
  };
  ASSERT_TRUE(wrong_request.valid());
  ASSERT_EQ(
      stores.active_bundle_publication_store()
          .PublishActiveBundle(wrong_request)
          .publication_generation.value(),
      1U);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);

  EXPECT_THROW(
      coordinator.AdvanceOrRecover(admission),
      CanonicalEmptyBootstrapCoordinatorError);
  EXPECT_EQ(owner.rebuild_calls(), 0U);
  EXPECT_EQ(owner.finalize_calls(), 0U);
}

TEST(
    CanonicalEmptyBootstrapAdvance,
    PrematureExactHeadWithAcceptedOrRebuildingOwnerFailsWithoutMutation) {
  const auto exercise = [](const bool rebuilding) {
    TemporaryDirectory directory;
    const auto admission = LoadAdmission();
    ExternallyProvisionedManagerStores stores(directory.path(), admission);
    const auto seeded = SeedAccepted(stores, admission);
    RecoverableSceneOwner owner(
        seeded.intent,
        seeded.accepted_target,
        stores.active_bundle_publication_store());
    CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);
    ASSERT_EQ(
        coordinator.AdvanceOrRecover(admission).state,
        ProjectionBootstrapStatusState::kReadyToPublish);
    owner.FailNextFinalizeBeforeEffect();
    EXPECT_THROW(
        coordinator.AdvanceOrRecover(admission),
        CanonicalEmptyBootstrapCoordinatorError);
    ASSERT_EQ(
        stores.StartupInventory().active_bundle_publication.receipt_count,
        1);

    if (rebuilding) {
      owner.SetRebuilding();
    } else {
      owner.SetAccepted();
    }
    const auto rebuild_calls = owner.rebuild_calls();
    const auto rebuild_effects = owner.rebuild_effects();
    const auto finalize_calls = owner.finalize_calls();
    const auto finalize_effects = owner.finalize_effects();

    EXPECT_THROW(
        coordinator.AdvanceOrRecover(admission),
        CanonicalEmptyBootstrapCoordinatorError);
    EXPECT_THROW(
        coordinator.BeginOrRecover(admission),
        CanonicalEmptyBootstrapCoordinatorError);
    EXPECT_EQ(owner.rebuild_calls(), rebuild_calls);
    EXPECT_EQ(owner.rebuild_effects(), rebuild_effects);
    EXPECT_EQ(owner.finalize_calls(), finalize_calls);
    EXPECT_EQ(owner.finalize_effects(), finalize_effects);
  };

  exercise(false);
  exercise(true);
}

TEST(
    CanonicalEmptyBootstrapAdvance,
    ConcurrentFinalizeBetweenStatusEntryAndReadRecoversExactCurrentHead) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);
  ASSERT_EQ(
      coordinator.AdvanceOrRecover(admission).state,
      ProjectionBootstrapStatusState::kReadyToPublish);

  std::barrier status_entered(2);
  std::barrier status_release(2);
  owner.SetNextStatusHook([&] {
    status_entered.arrive_and_wait();
    status_release.arrive_and_wait();
  });
  auto delayed = std::async(std::launch::async, [&] {
    return coordinator.AdvanceOrRecover(admission);
  });
  status_entered.arrive_and_wait();

  std::optional<ProjectionBootstrapStatusDTO> winner;
  std::exception_ptr winner_error;
  try {
    winner = coordinator.AdvanceOrRecover(admission);
  } catch (...) {
    winner_error = std::current_exception();
  }
  status_release.arrive_and_wait();
  if (winner_error != nullptr) {
    std::rethrow_exception(winner_error);
  }
  ASSERT_TRUE(winner.has_value());
  EXPECT_EQ(
      winner->state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(
      delayed.get().state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(owner.rebuild_effects(), 1U);
  EXPECT_EQ(owner.finalize_effects(), 1U);
  EXPECT_EQ(
      stores.StartupInventory().active_bundle_publication.receipt_count,
      1);
}

TEST(
    CanonicalEmptyBootstrapAdvance,
    AcceptedStatusCapturedBeforeConcurrentPublicationIsRereadAfterHead) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);

  std::barrier accepted_captured(2);
  std::barrier accepted_release(2);
  owner.SetNextStatusReturnHook([&] {
    accepted_captured.arrive_and_wait();
    accepted_release.arrive_and_wait();
  });
  auto delayed = std::async(std::launch::async, [&] {
    return coordinator.AdvanceOrRecover(admission);
  });
  accepted_captured.arrive_and_wait();

  std::exception_ptr winner_error;
  try {
    EXPECT_EQ(
        coordinator.AdvanceOrRecover(admission).state,
        ProjectionBootstrapStatusState::kReadyToPublish);
    EXPECT_EQ(
        coordinator.AdvanceOrRecover(admission).state,
        ProjectionBootstrapStatusState::kFinalized);
  } catch (...) {
    winner_error = std::current_exception();
  }
  accepted_release.arrive_and_wait();
  if (winner_error != nullptr) {
    std::rethrow_exception(winner_error);
  }

  EXPECT_EQ(
      delayed.get().state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(owner.rebuild_effects(), 1U);
  EXPECT_EQ(owner.finalize_effects(), 1U);
  EXPECT_EQ(
      stores.StartupInventory().active_bundle_publication.receipt_count,
      1);
}

TEST(CanonicalEmptyBootstrapAdvance, ConcurrentAdvanceHasOneLogicalEffect) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto seeded = SeedAccepted(stores, admission);
  RecoverableSceneOwner owner(
      seeded.intent,
      seeded.accepted_target,
      stores.active_bundle_publication_store());
  CanonicalEmptyBootstrapCoordinator coordinator(stores, owner);
  ASSERT_EQ(
      coordinator.AdvanceOrRecover(admission).state,
      ProjectionBootstrapStatusState::kReadyToPublish);

  std::barrier start(3);
  const auto invoke = [&] {
    start.arrive_and_wait();
    return coordinator.AdvanceOrRecover(admission);
  };
  auto first = std::async(std::launch::async, invoke);
  auto second = std::async(std::launch::async, invoke);
  start.arrive_and_wait();

  EXPECT_EQ(
      first.get().state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(
      second.get().state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(owner.rebuild_effects(), 1U);
  EXPECT_EQ(owner.finalize_effects(), 1U);
  EXPECT_GE(owner.finalize_calls(), 1U);
  EXPECT_LE(owner.finalize_calls(), 2U);
  EXPECT_EQ(
      stores.StartupInventory().active_bundle_publication.receipt_count,
      1);
}

}  // namespace
}  // namespace manyforge::component_manager
