#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <initializer_list>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "lite6_retained_consumer_fixture.hpp"
#include "logical_scene_codec_internal.hpp"
#include "manyforge_scene/canonical_empty_scene_owner.hpp"
#include "manyforge_scene/logical_scene_value_codec.hpp"
#include "manyforge_scene/scene_transaction_codec.hpp"
#include "sqlite_scene_lifecycle_internal.hpp"

namespace manyforge::scene {
namespace {

using namespace std::chrono_literals;

using core::ActiveBundlePublicationGeneration;
using core::ActiveBundlePublicationReceiptDTO;
using core::ActiveBundlePublicationRequestDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::BootstrapGeneration;
using core::BootstrapId;
using core::BootstrapPublicationTransactionDTO;
using core::CellId;
using core::ContentDigest;
using core::ErrorDTO;
using core::GraphRevision;
using core::IActiveBundlePublicationReader;
using core::NamedApplyPublicationTransactionDTO;
using core::ProjectionBootstrapAcceptedDTO;
using core::ProjectionBootstrapFinalizeDTO;
using core::ProjectionBootstrapFinalizeResultState;
using core::ProjectionBootstrapIntentDTO;
using core::ProjectionBootstrapPrerequisiteQueryDTO;
using core::ProjectionBootstrapPrerequisiteState;
using core::ProjectionBootstrapRebuildRequestDTO;
using core::ProjectionBootstrapStatusQueryDTO;
using core::ProjectionBootstrapStatusState;
using core::ProjectionNativeRuntimeBindingDTO;
using core::ProjectionRevisionDTO;
using core::RequestOwnerEpoch;
using core::Result;

constexpr std::string_view kExpectedSceneId =
    "sha256:b6edf7957c79f97cabefc2c61056f0bb09e00d1cac1ff3080ed540b650346dad";
constexpr std::string_view kExpectedEmptySceneContentHash =
    "sha256:bb42a1086f1fc8a45d1bcee2edc2bfce5ee2494029fdeeb5b2066717420cdfe5";
constexpr std::int64_t kSustainedNoOpCommitP95BudgetNs = 50'000'000;

static_assert(
    !std::is_constructible_v<CanonicalEmptySceneOwner, std::filesystem::path,
                             ProjectionBootstrapPrerequisiteQueryDTO,
                             const IActiveBundlePublicationReader &>);
static_assert(std::is_constructible_v<
              CanonicalEmptySceneOwner, std::filesystem::path,
              const deployment::CanonicalEmptyRetainedBundleAdmission &,
              const IActiveBundlePublicationReader &>);

class TemporaryDirectory final {
public:
  explicit TemporaryDirectory(std::string label = "scene-owner") {
    auto pattern = (std::filesystem::temp_directory_path() /
                    ("manyforge-" + std::move(label) + "-XXXXXX"))
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
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

void ExpectNoSqliteSidecars(const std::filesystem::path &path) {
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-journal"));
}

deployment::CanonicalEmptyRetainedBundleAdmission LoadAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

deployment::CanonicalEmptyRetainedBundleAdmission
LoadAdmission(const std::filesystem::path &package_root) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      package_root);
  return source.load_verified();
}

void CopyCanonicalEmptyPackage(const std::filesystem::path &package_root) {
  std::filesystem::copy(MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
                        package_root,
                        std::filesystem::copy_options::recursive);
}

ContentDigest Digest(const char digit) {
  return ContentDigest("sha256:" + std::string(64U, digit));
}

class TestCellAdmissionPort final : public core::IKernelCellAdmissionPort {
public:
  enum class LostReply { kNone, kAcquire, kRelease };
  void LoseNextReply(LostReply reply) { lost_reply_ = reply; }
  void RejectNextClaim(ErrorDTO error) { next_rejection_ = std::move(error); }

  void RejectProgramResetSceneAuthorization(bool reject) {
    std::lock_guard lock(mutex_);
    reject_program_reset_scene_authorization_ = reject;
  }

  [[nodiscard]] std::vector<core::CellAdmissionClaimDTO> Claims() const {
    std::lock_guard lock(mutex_);
    std::vector<core::CellAdmissionClaimDTO> claims;
    for (const auto &entry : entries_) {
      claims.push_back(entry.claim);
    }
    return claims;
  }

  [[nodiscard]] core::AuthoringAdmissionDTO
  GetAuthoringAdmission() const override {
    std::lock_guard lock(mutex_);
    return {!held_index_.has_value(), core::AdmissionGeneration(1U)};
  }

  void ProveHeldClaimHadNoEffect() {
    std::lock_guard lock(mutex_);
    if (!held_index_.has_value()) {
      throw std::logic_error("scene test admission has no held claim");
    }
    auto &entry = entries_[*held_index_];
    const auto &claim = entry.claim;
    entry.release = core::CellAdmissionClaimReleaseDTO{
        claim.intent.owner_role,
        claim.intent.owner_epoch,
        claim.intent.owner_transaction_id,
        claim.intent.request_id,
        claim.intent.request_payload_digest,
        claim.acquired_admission_generation,
        claim.claim_generation,
        {core::CellAdmissionSettlementKindDTO::kProvenNoEffect,
         claim.intent.request_payload_digest},
    };
    held_index_.reset();
  }

  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }

  [[nodiscard]] core::Result<core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    std::lock_guard lock(mutex_);
    if (next_rejection_.has_value()) {
      return core::Result<core::CellAdmissionClaimDTO>::Err(
          *std::exchange(next_rejection_, std::nullopt));
    }
    if (intent.expected_admission_generation !=
        core::AdmissionGeneration(1U)) {
      return core::Result<core::CellAdmissionClaimDTO>::Err(
          ErrorDTO{"test_admission_generation_mismatch",
                   "scene test expected admission generation 1", {}});
    }
    for (std::size_t index = 0U; index < entries_.size(); ++index) {
      if (entries_[index].claim.intent != intent) {
        continue;
      }
      if (!entries_[index].release.has_value() && held_index_ == index) {
        return core::Result<core::CellAdmissionClaimDTO>::Ok(
            entries_[index].claim);
      }
      return core::Result<core::CellAdmissionClaimDTO>::Err(
          ErrorDTO{"test_admission_already_settled",
                   "scene test admission intent is already settled", {}});
    }
    if (held_index_.has_value()) {
      return core::Result<core::CellAdmissionClaimDTO>::Err(
          ErrorDTO{"test_admission_busy",
                   "scene test admission already holds another claim", {}});
    }
    entries_.push_back({core::CellAdmissionClaimDTO{
                            intent,
                            core::CellAdmissionClaimGeneration(
                                next_claim_generation_++),
                            core::AdmissionGeneration(1U),
                            1},
                        std::nullopt});
    held_index_ = entries_.size() - 1U;
    if (lost_reply_ == LostReply::kAcquire) {
      lost_reply_ = LostReply::kNone;
      return core::Result<core::CellAdmissionClaimDTO>::Err(
          {"kernel_owner_unavailable", "acquisition reply lost", {}});
    }
    return core::Result<core::CellAdmissionClaimDTO>::Ok(
        entries_.back().claim);
  }

  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO &release) override {
    std::lock_guard lock(mutex_);
    if (lost_reply_ == LostReply::kRelease) {
      lost_reply_ = LostReply::kNone;
      return core::Result<core::AuthoringAdmissionDTO>::Err(
          {"kernel_owner_unavailable", "release unavailable", {}});
    }
    if (!held_index_.has_value()) {
      return core::Result<core::AuthoringAdmissionDTO>::Err(
          ErrorDTO{"test_admission_not_held",
                   "scene test admission has no held claim", {}});
    }
    auto &entry = entries_[*held_index_];
    const auto &claim = entry.claim;
    if (release.owner_role != claim.intent.owner_role ||
        release.owner_epoch != claim.intent.owner_epoch ||
        release.owner_transaction_id != claim.intent.owner_transaction_id ||
        release.request_id != claim.intent.request_id ||
        release.request_payload_digest !=
            claim.intent.request_payload_digest ||
        release.acquired_admission_generation !=
            claim.acquired_admission_generation ||
        release.claim_generation != claim.claim_generation) {
      return core::Result<core::AuthoringAdmissionDTO>::Err(
          ErrorDTO{"test_admission_release_mismatch",
                   "scene test admission release does not match its claim",
                   {}});
    }
    entry.release = release;
    held_index_.reset();
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        {true, core::AdmissionGeneration(1U)});
  }

  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  ReconcileCellAdmissionClaims(
      const core::CellAdmissionReconcileDTO &) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }

  [[nodiscard]] core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &query) const override {
    std::lock_guard lock(mutex_);
    core::CellAdmissionRecoveryReceiptDTO receipt{
        core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
        RequestOwnerEpoch("kernel-owner.scene-test"),
        core::AdmissionGeneration(1U),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        {},
        {},
    };
    for (auto iterator = entries_.rbegin(); iterator != entries_.rend();
         ++iterator) {
      if (iterator->claim.intent != query.intent) {
        continue;
      }
      if ((query.expected_claim.has_value() &&
           query.expected_claim != iterator->claim) ||
          (query.expected_release.has_value() &&
           query.expected_release != iterator->release)) {
        receipt.status =
            core::CellAdmissionRecoveryReceiptStatusDTO::kConflicting;
        return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
            std::move(receipt));
      }
      receipt.matched_claim = iterator->claim;
      receipt.matched_release = iterator->release;
      receipt.status =
          iterator->release.has_value()
              ? core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact
              : core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
      break;
    }
    if (held_index_.has_value()) {
      receipt.held_claims.push_back(entries_[*held_index_].claim);
    }
    return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
        std::move(receipt));
  }

  [[nodiscard]] core::Result<void> ValidateProgramResetSceneAuthorization(
      const core::ProgramResetSceneAuthorizationDTO &authorization)
      const override {
    std::lock_guard lock(mutex_);
    if (reject_program_reset_scene_authorization_ ||
        !core::ValidateProgramResetSceneAuthorization(authorization).ok()) {
      return core::Result<void>::Err(
          ErrorDTO{"authoring_not_ready",
                   "Program Reset scene authorization is unavailable", {}});
    }
    return core::Result<void>::Ok();
  }

private:
  struct Entry final {
    core::CellAdmissionClaimDTO claim;
    std::optional<core::CellAdmissionClaimReleaseDTO> release;
  };

  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::optional<std::size_t> held_index_;
  std::uint64_t next_claim_generation_{1U};
  bool reject_program_reset_scene_authorization_{};
  LostReply lost_reply_{LostReply::kNone};
  std::optional<ErrorDTO> next_rejection_;
};

class ControlledPublicationReader final
    : public IActiveBundlePublicationReader {
public:
  void SetCurrent(ActiveBundlePublicationReceiptDTO receipt) {
    std::lock_guard lock(mutex_);
    current_ = std::move(receipt);
    returned_error_.reset();
    throw_on_read_ = false;
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    current_.reset();
    returned_error_.reset();
    throw_on_read_ = false;
  }

  void ThrowOnRead() {
    std::lock_guard lock(mutex_);
    returned_error_.reset();
    throw_on_read_ = true;
  }

  void ReturnError(ErrorDTO error) {
    std::lock_guard lock(mutex_);
    returned_error_ = std::move(error);
    throw_on_read_ = false;
  }

  [[nodiscard]] std::size_t calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }

  Result<ActiveBundlePublicationReceiptDTO> ReadActiveBundlePublication(
      const ActiveBundlePublicationGeneration generation) const override {
    std::lock_guard lock(mutex_);
    ++calls_;
    if (throw_on_read_) {
      throw std::runtime_error("publication transport unavailable");
    }
    if (returned_error_.has_value()) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(*returned_error_);
    }
    if (!current_.has_value() ||
        current_->publication_generation != generation) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "publication_generation_not_current",
          "requested generation is not the current head",
          {},
      });
    }
    return Result<ActiveBundlePublicationReceiptDTO>::Ok(*current_);
  }

private:
  mutable std::mutex mutex_;
  mutable std::size_t calls_{};
  bool throw_on_read_{};
  std::optional<ErrorDTO> returned_error_;
  std::optional<ActiveBundlePublicationReceiptDTO> current_;
};

struct SceneLifecycleObservation final {
  SceneLifecycleObservation(
      const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
          attempt,
      const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
          acquired)
      : expected_attempt(attempt), expected_acquired(acquired),
        release_signal(release_after_acquired.get_future().share()) {}

  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
      expected_attempt;
  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
      expected_acquired;
  std::promise<void> attempted;
  std::promise<void> acquired;
  std::promise<void> release_after_acquired;
  std::shared_future<void> release_signal;
  std::atomic<bool> attempt_notified{false};
  std::atomic<bool> acquired_notified{false};
};

void ObserveSceneLifecycleEvent(
    const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
        event,
    void *const context) noexcept {
  auto &observation = *static_cast<SceneLifecycleObservation *>(context);
  try {
    if (event == observation.expected_attempt &&
        !observation.attempt_notified.exchange(true)) {
      observation.attempted.set_value();
      return;
    }
    if (event == observation.expected_acquired &&
        !observation.acquired_notified.exchange(true)) {
      observation.acquired.set_value();
      observation.release_signal.wait();
    }
  } catch (...) {
  }
}

class ScopedSceneLifecycleObserver final {
public:
  explicit ScopedSceneLifecycleObserver(
      SceneLifecycleObservation &observation) {
    sqlite_lifecycle_internal::
        SetCanonicalEmptySceneStoreLifecycleEventObserverForTesting(
            &ObserveSceneLifecycleEvent, &observation);
  }

  ~ScopedSceneLifecycleObserver() {
    sqlite_lifecycle_internal::
        ClearCanonicalEmptySceneStoreLifecycleEventObserverForTesting();
  }

  ScopedSceneLifecycleObserver(const ScopedSceneLifecycleObserver &) = delete;
  ScopedSceneLifecycleObserver &
  operator=(const ScopedSceneLifecycleObserver &) = delete;
};

template <typename Action>
void ExpectActionAcquiresSceneLifecycleGate(
    const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
        expected_attempt,
    const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
        expected_acquired,
    Action action) {
  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleLock held(
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleMutex());
  SceneLifecycleObservation observation(expected_attempt, expected_acquired);
  auto attempted = observation.attempted.get_future();
  auto acquired = observation.acquired.get_future();
  const ScopedSceneLifecycleObserver observer(observation);
  auto completion = std::async(
      std::launch::async, [action = std::move(action)]() mutable { action(); });

  const auto attempt_status = attempted.wait_for(2s);
  if (attempt_status != std::future_status::ready) {
    held.unlock();
    observation.release_after_acquired.set_value();
    ADD_FAILURE()
        << "production action did not attempt the scene lifecycle gate";
    EXPECT_NO_THROW(completion.get());
    return;
  }

  held.unlock();
  const auto acquired_status = acquired.wait_for(2s);
  if (acquired_status != std::future_status::ready) {
    observation.release_after_acquired.set_value();
    ADD_FAILURE()
        << "production action did not acquire the scene lifecycle gate";
    EXPECT_NO_THROW(completion.get());
    return;
  }

  auto &mutex =
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleMutex();
  const bool acquired_by_test = mutex.try_lock();
  if (acquired_by_test) {
    mutex.unlock();
  }
  EXPECT_FALSE(acquired_by_test)
      << "production action reported acquisition without holding the gate";

  observation.release_after_acquired.set_value();
  EXPECT_NO_THROW(completion.get());
}

ProjectionBootstrapIntentDTO
Intent(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       std::string bootstrap_id = "bootstrap-alpha") {
  return {
      BootstrapId(std::move(bootstrap_id)),
      BootstrapGeneration(1U),
      admission.cell_id(),
      admission.retained_bundle().value(),
      {},
  };
}

ProjectionBootstrapRebuildRequestDTO
RebuildRequest(const ProjectionBootstrapAcceptedDTO &accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.canonical_empty_target,
  };
}

ActiveBundlePublicationReceiptDTO
PublicationReceipt(const ProjectionBootstrapAcceptedDTO &accepted) {
  ActiveBundlePublicationRequestDTO request{
      BootstrapPublicationTransactionDTO{
          accepted.bootstrap_id,
          accepted.bootstrap_generation,
      },
      accepted.canonical_empty_target,
      accepted.canonical_empty_target.retained_bundle_id,
      accepted.canonical_empty_target.graph_revision,
      std::nullopt,
      Digest('b'),
  };
  return {
      std::move(request),
      ActiveBundlePublicationGeneration(1U),
      Digest('c'),
  };
}

ProjectionBootstrapFinalizeDTO
FinalizeRequest(const ProjectionBootstrapAcceptedDTO &accepted,
                ActiveBundlePublicationReceiptDTO receipt) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.canonical_empty_target,
      std::move(receipt),
  };
}

void ExecuteSql(const std::filesystem::path &database_path,
                const std::string &sql) {
  sqlite3 *database = nullptr;
  const auto open =
      sqlite3_open_v2(database_path.c_str(), &database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (open != SQLITE_OK) {
    const std::string detail =
        database == nullptr ? sqlite3_errstr(open) : sqlite3_errmsg(database);
    if (database != nullptr) {
      sqlite3_close(database);
    }
    throw std::runtime_error("cannot open test database: " + detail);
  }
  char *error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  const std::string detail =
      error == nullptr ? sqlite3_errmsg(database) : error;
  sqlite3_free(error);
  sqlite3_close(database);
  if (result != SQLITE_OK) {
    throw std::runtime_error("test SQL failed: " + detail);
  }
}

void Require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

ProjectionBootstrapAcceptedDTO FinalizeCanonicalBootstrap(
    CanonicalEmptySceneOwner &owner,
    ControlledPublicationReader &publication_reader,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  static_cast<void>(owner.RebuildProjectionBootstrap(RebuildRequest(accepted)));
  const auto receipt = PublicationReceipt(accepted);
  publication_reader.SetCurrent(receipt);
  const auto finalized =
      owner.FinalizeProjectionBootstrap(FinalizeRequest(accepted, receipt));
  Require(finalized.state == ProjectionBootstrapFinalizeResultState::kFinalized,
          "canonical bootstrap did not finalize");
  return accepted;
}

void ExpectExactCanonicalEmptySnapshot(
    const ProjectionBootstrapAcceptedDTO &accepted,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  ASSERT_TRUE(accepted.valid());
  const auto &snapshot = *accepted.initialized_logical_scene.shared();
  EXPECT_EQ(snapshot.cell_id, admission.cell_id());
  EXPECT_EQ(snapshot.revision, accepted.canonical_empty_target.scene_revision);
  EXPECT_EQ(snapshot.revision.revision_id, 1U);
  EXPECT_EQ(snapshot.transaction_sequence, 0U);
  EXPECT_EQ(snapshot.topology_epoch, 0U);
  EXPECT_EQ(snapshot.policy_epoch, 0U);
  EXPECT_EQ(snapshot.configured_collision_policy.policy_id, "free_space");
  EXPECT_EQ(snapshot.configured_collision_policy.revision.revision_id, 1U);
  EXPECT_EQ(snapshot.configured_collision_policy.revision.content_hash,
            core::kCanonicalEmptyCollisionPolicyDigest);
  EXPECT_TRUE(
      snapshot.configured_collision_policy.allowed_static_static_pairs.empty());
  EXPECT_TRUE(
      snapshot.configured_collision_policy.allowed_touch_link_pairs.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.support_contacts.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.contact_grasps.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.require_full_diagnostics);
  EXPECT_TRUE(snapshot.objects.empty());
  EXPECT_TRUE(snapshot.carried_object_attachments.empty());
  EXPECT_EQ(
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot),
      snapshot.revision.content_hash);
}

TEST(CanonicalEmptySceneOwner, SealedAdmissionMintsGoldenAndHoldsGatesClosed) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);

  EXPECT_EQ(owner.fixed_query().cell_id, admission.cell_id());
  EXPECT_EQ(owner.fixed_query().canonical_empty_retained_bundle_id,
            admission.retained_bundle_id());
  EXPECT_EQ(owner.fixed_query().canonical_empty_graph_revision,
            admission.graph_revision());
  const auto observed =
      owner.ObserveProjectionBootstrapPrerequisites(owner.fixed_query());
  EXPECT_TRUE(observed.ready_for(owner.fixed_query()));

  auto foreign_query = owner.fixed_query();
  foreign_query.cell_id = CellId("foreign-cell");
  const auto foreign =
      owner.ObserveProjectionBootstrapPrerequisites(foreign_query);
  EXPECT_TRUE(foreign.presence_valid());
  EXPECT_EQ(foreign.state, ProjectionBootstrapPrerequisiteState::kUnavailable);

  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  ExpectExactCanonicalEmptySnapshot(accepted, admission);
  EXPECT_EQ(accepted.canonical_empty_target.retained_bundle_id,
            admission.retained_bundle_id());
  EXPECT_EQ(accepted.canonical_empty_target.graph_revision,
            admission.graph_revision());
  EXPECT_EQ(accepted.canonical_empty_target.scene_revision.scene_id.value(),
            kExpectedSceneId);
  EXPECT_EQ(accepted.canonical_empty_target.scene_revision.content_hash.value(),
            kExpectedEmptySceneContentHash);

  const auto access = owner.ReadAccessStatus();
  EXPECT_TRUE(access.valid());
  EXPECT_EQ(access.phase, CanonicalEmptySceneBootstrapPhase::kAccepted);
  EXPECT_FALSE(access.scene_writes_open);
  EXPECT_FALSE(access.projection_leases_open);
  EXPECT_FALSE(access.active_projection.has_value());
  EXPECT_FALSE(owner.ReadActiveSnapshot().has_value());
  const auto integrity = owner.VerifyIntegrity();
  EXPECT_EQ(integrity.schema_id, kLogicalSceneStoreSchemaId);
  EXPECT_EQ(integrity.schema_version, kLogicalSceneStoreSchemaVersion);
  EXPECT_EQ(integrity.acceptance_row_count, 1);
  EXPECT_EQ(integrity.rebuild_row_count, 0);
  EXPECT_EQ(integrity.finalization_row_count, 0);
  EXPECT_EQ(integrity.fixed_query, owner.fixed_query());
}

TEST(CanonicalEmptySceneOwner,
     BeginAcceptsOnlyExactAdmittedBundleAndOneBootstrapLineage) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);

  auto cross_spliced = Intent(admission);
  cross_spliced.canonical_empty_retained_bundle.deployment_artifact_id =
      core::DeploymentArtifactId("forged-deployment");
  EXPECT_THROW(owner.BeginProjectionBootstrap(cross_spliced),
               CanonicalEmptySceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);

  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  const auto replay = owner.BeginProjectionBootstrap(Intent(admission));
  EXPECT_EQ(replay.canonical_empty_target, accepted.canonical_empty_target);
  EXPECT_EQ(replay.initialized_logical_scene.shared()->revision,
            accepted.initialized_logical_scene.shared()->revision);
  EXPECT_THROW(
      owner.BeginProjectionBootstrap(Intent(admission, "bootstrap-beta")),
      CanonicalEmptySceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 1);
}

TEST(CanonicalEmptySceneOwner,
     RestartReconstructsCompleteSnapshotAndExactReplay) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  ProjectionRevisionDTO target = [&] {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
    ExpectExactCanonicalEmptySnapshot(accepted, admission);
    owner.Close();
    return accepted.canonical_empty_target;
  }();

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  const auto status =
      reopened.ProjectionBootstrapStatus(ProjectionBootstrapStatusQueryDTO{
          BootstrapId("bootstrap-alpha"), BootstrapGeneration(1U)});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kAccepted);
  EXPECT_EQ(status.canonical_empty_target, target);
  const auto replay = reopened.BeginProjectionBootstrap(Intent(admission));
  EXPECT_EQ(replay.canonical_empty_target, target);
  ExpectExactCanonicalEmptySnapshot(replay, admission);

  const auto absent =
      reopened.ProjectionBootstrapStatus(ProjectionBootstrapStatusQueryDTO{
          BootstrapId("bootstrap-foreign"), BootstrapGeneration(9U)});
  EXPECT_TRUE(absent.presence_valid());
  EXPECT_EQ(absent.state, ProjectionBootstrapStatusState::kAbsent);
}

TEST(CanonicalEmptySceneOwner,
     ZeroNativeRebuildIsDurableIdempotentAndStatusComplete) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  ProjectionBootstrapAcceptedDTO accepted = [&] {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    auto value = owner.BeginProjectionBootstrap(Intent(admission));
    const auto rebuilt =
        owner.RebuildProjectionBootstrap(RebuildRequest(value));
    Require(rebuilt.presence_valid(), "Rebuild result is invalid");
    Require(rebuilt.bootstrap_evidence.has_value(),
            "Rebuild result has no evidence");
    EXPECT_TRUE(rebuilt.bootstrap_evidence->rebuild_results.empty());
    EXPECT_TRUE(rebuilt.bootstrap_evidence->candidate_aggregate_health
                    .native_projection_health.empty());
    EXPECT_EQ(rebuilt.bootstrap_evidence->candidate_aggregate_health.state,
              core::ProjectionHealthState::kHealthy);
    const auto replay = owner.RebuildProjectionBootstrap(RebuildRequest(value));
    EXPECT_EQ(replay.canonical_empty_target, rebuilt.canonical_empty_target);
    owner.Close();
    return value;
  }();

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  const auto status = reopened.ProjectionBootstrapStatus(
      {accepted.bootstrap_id, accepted.bootstrap_generation});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kReadyToPublish);
  ASSERT_TRUE(status.bootstrap_evidence.has_value());
  EXPECT_TRUE(status.bootstrap_evidence->rebuild_results.empty());
  EXPECT_TRUE(status.native_rebuild_observations.empty());
  EXPECT_FALSE(reopened.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(reopened.ReadAccessStatus().projection_leases_open);
  const auto integrity = reopened.VerifyIntegrity();
  EXPECT_EQ(integrity.acceptance_row_count, 1);
  EXPECT_EQ(integrity.rebuild_row_count, 1);
  EXPECT_EQ(integrity.finalization_row_count, 0);
}

TEST(CanonicalEmptySceneOwner, NonzeroNativeBootstrapIsExplicitlyUnavailable) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  auto intent = Intent(admission);
  intent.native_runtime_bindings.push_back(ProjectionNativeRuntimeBindingDTO{
      core::CapabilityProviderBindingId("binding"),
      core::CapabilityProviderBindingRevision("binding-revision"),
      core::RuntimeUnitId("runtime"),
      core::RuntimeEpoch("epoch"),
  });
  EXPECT_THROW(owner.BeginProjectionBootstrap(intent),
               CanonicalEmptySceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);
}

TEST(CanonicalEmptySceneOwner,
     FinalizeRejectsStaleAndMismatchedHeadThenDurablyOpensGates) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  ActiveBundlePublicationReceiptDTO exact_receipt = [&] {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
    static_cast<void>(
        owner.RebuildProjectionBootstrap(RebuildRequest(accepted)));
    auto receipt = PublicationReceipt(accepted);
    const auto finalize = FinalizeRequest(accepted, receipt);

    const auto stale = owner.FinalizeProjectionBootstrap(finalize);
    Require(stale.presence_valid(), "stale Finalize result is invalid");
    EXPECT_EQ(stale.state, ProjectionBootstrapFinalizeResultState::kStale);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
    EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

    auto mismatched = receipt;
    mismatched.durable_public_pointer_content_digest = Digest('d');
    publication_reader.SetCurrent(mismatched);
    const auto mismatch = owner.FinalizeProjectionBootstrap(finalize);
    Require(mismatch.presence_valid(), "mismatched Finalize result is invalid");
    EXPECT_EQ(mismatch.state, ProjectionBootstrapFinalizeResultState::kStale);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

    publication_reader.SetCurrent(receipt);
    const auto finalized = owner.FinalizeProjectionBootstrap(finalize);
    Require(finalized.presence_valid(),
            "successful Finalize result is invalid");
    EXPECT_EQ(finalized.state,
              ProjectionBootstrapFinalizeResultState::kFinalized);
    EXPECT_EQ(finalized.publication_receipt, receipt);
    const auto access = owner.ReadAccessStatus();
    EXPECT_TRUE(access.scene_writes_open);
    EXPECT_TRUE(access.projection_leases_open);
    Require(access.active_projection.has_value(),
            "finalized access has no active projection");
    EXPECT_EQ(*access.active_projection, accepted.canonical_empty_target);
    const auto snapshot = owner.ReadActiveSnapshot();
    Require(snapshot.has_value(), "finalized owner has no active snapshot");
    EXPECT_EQ(snapshot->shared()->identity(),
              accepted.initialized_logical_scene.shared()->identity());
    EXPECT_TRUE(snapshot->canonical_empty_for(
        accepted.initialized_logical_scene.shared()->identity()));
    EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 1);

    const auto observed =
        owner.ObserveProjectionBootstrapPrerequisites(owner.fixed_query());
    EXPECT_TRUE(observed.presence_valid());
    EXPECT_EQ(
        observed.state,
        ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection);
    owner.Close();
    return receipt;
  }();

  publication_reader.Clear();
  const auto calls_before_reopen = publication_reader.calls();
  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  const auto status = reopened.ProjectionBootstrapStatus(
      {BootstrapId("bootstrap-alpha"), BootstrapGeneration(1U)});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(status.publication_receipt, exact_receipt);
  EXPECT_TRUE(reopened.ReadAccessStatus().scene_writes_open);
  EXPECT_TRUE(reopened.ReadAccessStatus().projection_leases_open);
  const auto reopened_snapshot = reopened.ReadActiveSnapshot();
  ASSERT_TRUE(reopened_snapshot.has_value());
  EXPECT_EQ(reopened_snapshot->shared()->revision,
            exact_receipt.request.candidate_projection_revision.scene_revision);
  ASSERT_TRUE(status.canonical_empty_target.has_value());
  const auto replay = reopened.FinalizeProjectionBootstrap({
      BootstrapId("bootstrap-alpha"),
      BootstrapGeneration(1U),
      *status.canonical_empty_target,
      exact_receipt,
  });
  EXPECT_EQ(replay.state, ProjectionBootstrapFinalizeResultState::kFinalized);
  EXPECT_EQ(publication_reader.calls(), calls_before_reopen);
}

TEST(CanonicalEmptySceneOwner,
     PublicationReadUncertaintyNeverOpensWritesOrLeases) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  static_cast<void>(owner.RebuildProjectionBootstrap(RebuildRequest(accepted)));
  const auto finalize = FinalizeRequest(accepted, PublicationReceipt(accepted));
  publication_reader.ReturnError(ErrorDTO{
      "internal_error",
      "durable active-bundle publication read failed",
      {},
  });
  const auto returned_error_result =
      owner.FinalizeProjectionBootstrap(finalize);
  EXPECT_TRUE(returned_error_result.presence_valid());
  EXPECT_EQ(returned_error_result.state,
            ProjectionBootstrapFinalizeResultState::kFailedProjectionUncertain);
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

  publication_reader.ThrowOnRead();
  const auto thrown_result = owner.FinalizeProjectionBootstrap(finalize);
  EXPECT_TRUE(thrown_result.presence_valid());
  EXPECT_EQ(thrown_result.state,
            ProjectionBootstrapFinalizeResultState::kFailedProjectionUncertain);
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);
}

TEST(CanonicalEmptySceneOwnerLifecycleGate,
     ContendedStartupDeadlineReturnsWithoutOwnership) {
  constexpr auto kTimeout = 50ms;
  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleLock held(
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleMutex());
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  auto waiter = std::async(std::launch::async, [start] {
    start.get();
    const auto started = std::chrono::steady_clock::now();
    auto attempted = sqlite_lifecycle_internal::
        TryAcquireCanonicalEmptySceneStoreLifecycleLock(kTimeout);
    return std::pair{attempted.owns_lock(),
                     std::chrono::steady_clock::now() - started};
  });
  start_promise.set_value();

  const auto [owns_lock, elapsed] = waiter.get();
  held.unlock();
  EXPECT_FALSE(owns_lock);
  EXPECT_GE(elapsed, kTimeout);
}

TEST(CanonicalEmptySceneOwner, ConcurrentFirstOpenConvergesOneExactEmptyStore) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  const auto database = directory.path() / "logical-scene.sqlite3";
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  std::barrier open_together(4);
  std::vector<std::future<CanonicalEmptySceneStoreIntegrityReport>> openings;
  openings.reserve(4U);
  try {
    for (std::size_t index = 0U; index < 4U; ++index) {
      openings.push_back(std::async(std::launch::async, [&, index, start] {
        start.get();
        open_together.arrive_and_wait();
        CanonicalEmptySceneOwner owner(database, admission, publication_reader);
        const auto report = owner.VerifyIntegrity();
        if (index % 2U == 0U) {
          owner.Close();
        }
        return report;
      }));
    }
  } catch (...) {
    const auto launch_error = std::current_exception();
    start_promise.set_exception(launch_error);
    for (auto &opening : openings) {
      try {
        static_cast<void>(opening.get());
      } catch (...) {
      }
    }
    std::rethrow_exception(launch_error);
  }
  start_promise.set_value();

  std::string schema_digest;
  for (auto &opening : openings) {
    const auto report = opening.get();
    EXPECT_EQ(report.acceptance_row_count, 0);
    EXPECT_EQ(report.rebuild_row_count, 0);
    EXPECT_EQ(report.finalization_row_count, 0);
    if (schema_digest.empty()) {
      schema_digest = report.schema_artifact_digest;
    } else {
      EXPECT_EQ(report.schema_artifact_digest, schema_digest);
    }
  }
  ExpectNoSqliteSidecars(database);
  CanonicalEmptySceneOwner reopened(database, admission, publication_reader);
  EXPECT_EQ(reopened.VerifyIntegrity().acceptance_row_count, 0);
}

TEST(CanonicalEmptySceneOwner, ConcurrentExactBeginCommitsOneAcceptance) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner first(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  CanonicalEmptySceneOwner second(directory.path() / "logical-scene.sqlite3",
                                  admission, publication_reader);
  std::barrier start(3);
  const auto call = [&](CanonicalEmptySceneOwner &owner) {
    start.arrive_and_wait();
    return owner.BeginProjectionBootstrap(Intent(admission))
        .canonical_empty_target;
  };
  auto first_result = std::async(std::launch::async, call, std::ref(first));
  auto second_result = std::async(std::launch::async, call, std::ref(second));
  start.arrive_and_wait();
  EXPECT_EQ(first_result.get(), second_result.get());
  EXPECT_EQ(first.VerifyIntegrity().acceptance_row_count, 1);
  EXPECT_EQ(second.VerifyIntegrity().acceptance_row_count, 1);
}

TEST(CanonicalEmptySceneOwner, ConcurrentConflictingBeginHasOneWinner) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner first(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  CanonicalEmptySceneOwner second(directory.path() / "logical-scene.sqlite3",
                                  admission, publication_reader);
  std::barrier start(3);
  const auto call = [&admission, &start](CanonicalEmptySceneOwner &owner,
                                         std::string bootstrap_id) {
    start.arrive_and_wait();
    try {
      static_cast<void>(owner.BeginProjectionBootstrap(
          Intent(admission, std::move(bootstrap_id))));
      return true;
    } catch (const CanonicalEmptySceneOwnerConflictError &) {
      return false;
    }
  };
  auto first_result =
      std::async(std::launch::async, call, std::ref(first), "bootstrap-alpha");
  auto second_result =
      std::async(std::launch::async, call, std::ref(second), "bootstrap-beta");
  start.arrive_and_wait();
  const auto first_won = first_result.get();
  const auto second_won = second_result.get();
  EXPECT_NE(first_won, second_won);
  EXPECT_EQ(first.VerifyIntegrity().acceptance_row_count, 1);
}

TEST(CanonicalEmptySceneOwner,
     StartupRejectsSemanticRowCorruptionWithIntactSchema) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    static_cast<void>(owner.BeginProjectionBootstrap(Intent(admission)));
    owner.Close();
  }
  ExecuteSql(
      database_path,
      "DROP TRIGGER canonical_empty_scene_acceptance_no_update;"
      "UPDATE canonical_empty_scene_acceptance "
      "SET scene_content_hash='sha256:"
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee';"
      "CREATE TRIGGER IF NOT EXISTS "
      "canonical_empty_scene_acceptance_no_update "
      "BEFORE UPDATE ON canonical_empty_scene_acceptance BEGIN "
      "SELECT RAISE(ABORT, 'canonical_empty_scene_acceptance is immutable');"
      "END;");
  EXPECT_THROW(
      CanonicalEmptySceneOwner(database_path, admission, publication_reader),
      CanonicalEmptySceneOwnerIntegrityError);
}

TEST(CanonicalEmptySceneOwner, StartupRejectsTheProvisionalV3PayloadContract) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    owner.Close();
  }
  ExecuteSql(database_path, "PRAGMA user_version = 3;");
  EXPECT_THROW(
      CanonicalEmptySceneOwner(database_path, admission, publication_reader),
      CanonicalEmptySceneOwnerIntegrityError);
}

TEST(CanonicalEmptySceneOwner, StartupRejectsOversizedDatabaseImage) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    owner.Close();
  }
  std::filesystem::resize_file(
      database_path,
      static_cast<std::uintmax_t>(kMaximumSceneStoreDatabasePageImageBytes) +
          1U);
  EXPECT_THROW(
      CanonicalEmptySceneOwner(database_path, admission, publication_reader),
      CanonicalEmptySceneOwnerError);
}

TEST(CanonicalEmptySceneOwner, StartupRejectsOversizedWalResidue) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto wal_path = std::filesystem::path(database_path.string() + "-wal");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    owner.Close();
  }
  {
    std::ofstream wal(wal_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(wal.good());
  }
  std::filesystem::resize_file(
      wal_path, static_cast<std::uintmax_t>(kMaximumSceneStoreWalBytes) + 1U);
  EXPECT_THROW(
      CanonicalEmptySceneOwner(database_path, admission, publication_reader),
      CanonicalEmptySceneOwnerError);
}

TEST(CanonicalEmptySceneOwner, RestartsExactEmptyInitializationResidue) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  sqlite3 *empty_database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(database_path.c_str(), &empty_database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr),
            SQLITE_OK);
  ASSERT_NE(empty_database, nullptr);
  ASSERT_EQ(sqlite3_close(empty_database), SQLITE_OK);

  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(database_path, admission, publication_reader);
  const auto inventory = owner.VerifyIntegrity();
  EXPECT_EQ(inventory.acceptance_row_count, 0);
  EXPECT_EQ(inventory.rebuild_row_count, 0);
  EXPECT_EQ(inventory.finalization_row_count, 0);
  EXPECT_EQ(inventory.access.phase,
            CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced);
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  ExpectExactCanonicalEmptySnapshot(accepted, admission);
}

TEST(CanonicalEmptySceneOwner, RejectsOverBoundBootstrapIdentifier) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  EXPECT_THROW(
      owner.BeginProjectionBootstrap(Intent(
          admission, std::string(kMaximumSceneStoreIdentifierBytes + 1U, 'x'))),
      std::invalid_argument);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);
}

core::SceneTransactionRequestDTO
AddBoxRequest(const ProjectionRevisionDTO &expected, const CellId &cell_id,
              std::string request_id = "scene-request-add-box",
              std::vector<core::SupportContactDTO> support_contacts = {}) {
  core::Pose3DTO pose;
  pose.position_m = {0.25, -0.5, 0.75};
  return {
      core::PublicRequestContextDTO{
          core::RequestId(std::move(request_id)),
          core::RequestOwnerEpoch(expected.scene_revision.scene_id.value()),
          "scene-owner-test",
          std::nullopt,
      },
      {},
      expected,
      core::AddObjectRequestDTO{
          core::SceneResourceId("scene-box"),
          core::BoxShapeDTO{{0.1, 0.2, 0.3}},
          pose,
          core::CellSceneFrameRefDTO{cell_id},
          std::move(support_contacts),
      },
      core::AdmissionGeneration(1U),
  };
}

core::PublicRequestContextDTO
SceneRequestContext(std::string request_id,
                    const ProjectionRevisionDTO &expected);

TEST(LogicalSceneTransactionCodec,
     CanonicalInternalRequestRoundTripsAndRejectsWireDrift) {
  const ProjectionRevisionDTO expected{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{
          core::SceneId(Digest('d').value()),
          1U,
          Digest('a'),
      },
  };
  auto request = AddBoxRequest(expected, CellId("cell-a"));
  std::get<core::AddObjectRequestDTO>(request.operation).support_contacts = {
      core::SupportContactDTO{.support_object_id = "support-table"},
  };
  request.attribution = {
      core::RequestTransportDTO::kHttp,
      core::RequestAssuranceDTO::kExternallyAuthenticated,
      std::string("operator.7"),
  };
  const auto encoded = logical_scene_codec_internal::EncodeRequest(request);
  const auto decoded = DecodeCanonicalSceneTransactionRequest(encoded);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded), encoded);
  EXPECT_EQ(decoded.request_context.request_id,
            request.request_context.request_id);
  EXPECT_EQ(decoded.attribution.principal_id, std::string("operator.7"));
  auto differently_attributed = request;
  differently_attributed.attribution = {
      core::RequestTransportDTO::kRos,
      core::RequestAssuranceDTO::kUnverified,
      std::nullopt,
  };
  EXPECT_NE(logical_scene_codec_internal::EncodeRequest(differently_attributed),
            encoded);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequestDigestInput(
                differently_attributed),
            logical_scene_codec_internal::EncodeRequestDigestInput(request));
  ASSERT_TRUE(
      std::holds_alternative<core::AddObjectRequestDTO>(decoded.operation));
  EXPECT_EQ(
      std::get<core::AddObjectRequestDTO>(decoded.operation).scene_resource_id,
      core::SceneResourceId("scene-box"));
  ASSERT_EQ(std::get<core::AddObjectRequestDTO>(decoded.operation)
                .support_contacts.size(),
            1U);
  EXPECT_EQ(std::get<core::AddObjectRequestDTO>(decoded.operation)
                .support_contacts.front()
                .support_object_id,
            "support-table");
  EXPECT_THROW(DecodeCanonicalSceneTransactionRequest(" " + encoded),
               std::invalid_argument);
  auto unknown_field = encoded;
  ASSERT_EQ(unknown_field.back(), '}');
  unknown_field.pop_back();
  unknown_field += ",\"unknown\":true}";
  EXPECT_THROW(DecodeCanonicalSceneTransactionRequest(unknown_field),
               std::invalid_argument);

  auto reset_request = request;
  reset_request.expected_admission_generation.reset();
  reset_request.program_reset_scene_authorization =
      core::ProgramResetSceneAuthorizationDTO{
          RequestOwnerEpoch("program-runner.epoch.2"),
          core::ProgramRunId("program-cycle.2"),
          core::CellAdmissionClaimGeneration(9U), Digest('e')};
  const auto reset_encoded =
      logical_scene_codec_internal::EncodeRequest(reset_request);
  const auto reset_decoded =
      DecodeCanonicalSceneTransactionRequest(reset_encoded);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(reset_decoded),
            reset_encoded);
  EXPECT_EQ(reset_decoded.program_reset_scene_authorization,
            reset_request.program_reset_scene_authorization);
  EXPECT_NE(logical_scene_codec_internal::EncodeRequestDigestInput(
                reset_request),
            logical_scene_codec_internal::EncodeRequestDigestInput(request));

  auto node_request = request;
  node_request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner.2"),
      core::ProgramRunId("program.2"),
      core::CellAdmissionClaimGeneration(9U),
      "attach",
      std::string(512U, 'a'),
      "scene.transaction"};
  const auto node_bytes =
      logical_scene_codec_internal::EncodeRequest(node_request);
  const auto node_decoded = DecodeCanonicalSceneTransactionRequest(node_bytes);
  EXPECT_EQ(node_decoded.program_node_evidence,
            node_request.program_node_evidence);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(node_decoded),
            node_bytes);
  EXPECT_NE(
      logical_scene_codec_internal::EncodeRequestDigestInput(node_request),
      logical_scene_codec_internal::EncodeRequestDigestInput(request));
}

TEST(CanonicalEmptySceneOwner,
     ProgramTransactionResultLookupIsExactAndReadOnly) {
  TemporaryDirectory directory("scene-program-result");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-program-add");
  request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner.2"),
      core::ProgramRunId("program.2"),
      core::CellAdmissionClaimGeneration(9U),
      "add",
      "add:attempt.1",
      "scene.transaction"};
  EXPECT_FALSE(owner.ReadCommitResult(request).has_value());
  EXPECT_TRUE(cell_admission.Claims().empty());
  const auto result = owner.Commit(request);
  ASSERT_TRUE(result.success)
      << (result.error ? result.error->message : "missing error");
  const auto claims = cell_admission.Claims();
  ASSERT_EQ(claims.size(), 1U);
  EXPECT_EQ(claims.front().intent.program_node_evidence,
            request.program_node_evidence);
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
  const auto before = owner.VerifyIntegrity();
  const auto retained = owner.ReadCommitResult(request);
  ASSERT_TRUE(retained.has_value());
  EXPECT_TRUE(retained->success);
  EXPECT_EQ(retained->replay_status,
            core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(retained->committed_projection_revision,
            result.committed_projection_revision);
  EXPECT_EQ(cell_admission.Claims().size(), 1U);
  EXPECT_EQ(owner.VerifyIntegrity().request_outcome_count,
            before.request_outcome_count);
  auto conflicting = request;
  conflicting.program_node_evidence->node_attempt_id = "add:attempt.2";
  EXPECT_THROW(owner.ReadCommitResult(conflicting),
               CanonicalEmptySceneOwnerConflictError);
  auto missing = request;
  missing.request_context.request_id = core::RequestId("scene-program-missing");
  EXPECT_FALSE(owner.ReadCommitResult(missing).has_value());
}

TEST(CanonicalEmptySceneOwner,
     ProgramClaimsReconcileAfterLostAcquireOrReleaseReply) {
  for (const auto loss : {TestCellAdmissionPort::LostReply::kAcquire,
                          TestCellAdmissionPort::LostReply::kRelease}) {
    TemporaryDirectory directory("scene-program-claim-reply");
    const auto admission = LoadAdmission();
    ControlledPublicationReader publication_reader;
    TestCellAdmissionPort cell_admission;
    CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                   admission, publication_reader,
                                   cell_admission);
    const auto active =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    auto request =
        AddBoxRequest(active, admission.cell_id(), "scene-program-add");
    request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
        RequestOwnerEpoch("runner.2"),
        core::ProgramRunId("program.2"),
        core::CellAdmissionClaimGeneration(9U),
        "add",
        "add:attempt.1",
        "scene.transaction"};
    cell_admission.LoseNextReply(loss);
    if (loss == TestCellAdmissionPort::LostReply::kAcquire) {
      EXPECT_FALSE(owner.Commit(request).success);
    } else {
      EXPECT_THROW(owner.Commit(request), std::runtime_error);
    }
    EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
    const auto result = owner.ReadCommitResult(request);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->success,
              loss == TestCellAdmissionPort::LostReply::kRelease);
    EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
    const auto reconciled = owner.ReconcileAuthoredSceneAdmissionClaims();
    ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
    EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
    EXPECT_EQ(cell_admission.Claims().size(), 1U);
    EXPECT_EQ(owner.GetSceneObjects().size(), result->success ? 1U : 0U);
  }
}

TEST(CanonicalEmptySceneOwner,
     InvalidProgramEvidenceNeverEntersDurableSceneState) {
  TemporaryDirectory directory("scene-program-evidence");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  const auto path = directory.path() / "logical-scene.sqlite3";
  {
    CanonicalEmptySceneOwner owner(path, admission, publication_reader,
                                   cell_admission);
    const auto active =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    auto request =
        AddBoxRequest(active, admission.cell_id(), "scene-program-add");
    request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
        RequestOwnerEpoch("runner.2"),
        core::ProgramRunId("program.2"),
        core::CellAdmissionClaimGeneration(9U),
        "add",
        "",
        "scene.transaction"};
    for (const auto &attempt : {std::string{}, std::string(513U, 'a')}) {
      request.program_node_evidence->node_attempt_id = attempt;
      const auto rejected = owner.Commit(request);
      ASSERT_FALSE(rejected.success);
      ASSERT_TRUE(rejected.error.has_value());
      EXPECT_EQ(rejected.error->code, "invalid_request");
    }
    request.program_node_evidence->node_attempt_id = "add:attempt.1";
    request.program_node_evidence->parent_program_cycle_claim_generation =
        core::CellAdmissionClaimGeneration(0U);
    EXPECT_FALSE(owner.Commit(request).success);
    EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 0);
    EXPECT_EQ(owner.VerifyIntegrity().request_outcome_count, 0);
    EXPECT_TRUE(cell_admission.Claims().empty());
  }
  CanonicalEmptySceneOwner reopened(path, admission, publication_reader,
                                    cell_admission);
  EXPECT_TRUE(reopened.ReconcileAuthoredSceneAdmissionClaims().ok());
  EXPECT_TRUE(reopened.GetSceneObjects().empty());
}

TEST(CanonicalEmptySceneOwner,
     StopBeforeProgramAdmissionIsExactAndHasNoEffect) {
  TemporaryDirectory directory("scene-program-held");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-program-add");
  request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner.2"),
      core::ProgramRunId("program.2"),
      core::CellAdmissionClaimGeneration(9U),
      "add",
      "add:attempt.1",
      "scene.transaction"};
  const ErrorDTO held{"invalid_state_transition",
                      "not started; held by Stop",
                      {{"reason", "program_node_stop_fence_active"},
                       {"selection_revision", "7"}}};
  cell_admission.RejectNextClaim(held);
  const auto rejected = owner.Commit(request);
  ASSERT_FALSE(rejected.success);
  EXPECT_EQ(rejected.error, held);
  EXPECT_EQ(rejected.projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_TRUE(owner.GetSceneObjects().empty());
  ASSERT_TRUE(owner.ReadCommitResult(request).has_value());
  EXPECT_EQ(owner.ReadCommitResult(request)->error, held);
  EXPECT_TRUE(cell_admission.Claims().empty());
}

TEST(LogicalSceneTransactionCodec,
     UpsertAndCurrentPoseDetachRequestsRoundTripCanonically) {
  const ProjectionRevisionDTO expected{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{core::SceneId(Digest('d').value()), 1U, Digest('a')},
  };
  auto upsert =
      AddBoxRequest(expected, CellId("cell-a"), "scene-request-upsert-box");
  const auto add = std::get<core::AddObjectRequestDTO>(upsert.operation);
  upsert.operation = core::UpsertObjectRequestDTO{
      add.scene_resource_id, add.shape, add.pose, add.frame_ref,
      add.support_contacts};
  std::get<core::UpsertObjectRequestDTO>(upsert.operation).frame_ref =
      core::SceneResourceSceneFrameRefDTO{
          core::SceneResourceId("reference-object")};
  const auto upsert_bytes = logical_scene_codec_internal::EncodeRequest(upsert);
  const auto decoded_upsert =
      DecodeCanonicalSceneTransactionRequest(upsert_bytes);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded_upsert),
            upsert_bytes);
  EXPECT_TRUE(std::holds_alternative<core::UpsertObjectRequestDTO>(
      decoded_upsert.operation));
  const auto &decoded_frame =
      std::get<core::UpsertObjectRequestDTO>(decoded_upsert.operation)
          .frame_ref;
  ASSERT_TRUE(
      std::holds_alternative<core::SceneResourceSceneFrameRefDTO>(
          decoded_frame));
  EXPECT_EQ(
      std::get<core::SceneResourceSceneFrameRefDTO>(decoded_frame)
          .scene_resource_id,
      core::SceneResourceId("reference-object"));

  core::SceneTransactionRequestDTO detach{
      SceneRequestContext("scene-request-detach-current", expected),
      {},
      expected,
      core::DetachCarriedObjectAtCurrentPoseRequestDTO{
          core::CarriedObjectAttachmentId("attachment.1"),
          core::CarriedObjectAttachmentRevision(Digest('e').value()),
          core::SceneResourceId("scene-box"),
          core::ControllableComponentId("component.1"),
          core::CommandTargetId("arm")},
      std::nullopt,
  };
  const auto detach_bytes = logical_scene_codec_internal::EncodeRequest(detach);
  const auto decoded_detach =
      DecodeCanonicalSceneTransactionRequest(detach_bytes);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded_detach),
            detach_bytes);
  EXPECT_TRUE(
      std::holds_alternative<core::DetachCarriedObjectAtCurrentPoseRequestDTO>(
          decoded_detach.operation));
}

TEST(CanonicalEmptySceneOwner, UpsertCreatesReplacesAndRecognizesAnExactNoOp) {
  TemporaryDirectory directory("scene-upsert");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, cell_admission);
  auto active = FinalizeCanonicalBootstrap(owner, publication_reader, admission)
                    .canonical_empty_target;

  auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-request-upsert-create");
  const auto add = std::get<core::AddObjectRequestDTO>(request.operation);
  request.operation = core::UpsertObjectRequestDTO{
      add.scene_resource_id, add.shape, add.pose, add.frame_ref,
      add.support_contacts};
  const auto created = owner.Commit(request);
  ASSERT_TRUE(created.success);
  ASSERT_TRUE(created.committed_projection_revision.has_value());
  active = *created.committed_projection_revision;

  auto replace = request;
  replace.request_context =
      SceneRequestContext("scene-request-upsert-replace", active);
  replace.expected_projection_revision = active;
  auto &replacement = std::get<core::UpsertObjectRequestDTO>(replace.operation);
  replacement.shape = core::SphereShapeDTO{0.4};
  replacement.pose.position_m.x_m = 1.25;
  const auto replaced = owner.Commit(replace);
  ASSERT_TRUE(replaced.success);
  ASSERT_TRUE(replaced.committed_projection_revision.has_value());
  ASSERT_TRUE(replaced.update_status.has_value());
  EXPECT_EQ(*replaced.update_status,
            core::SceneUpdateStatusDTO::kGeometryChanged);
  active = *replaced.committed_projection_revision;
  const auto objects = owner.GetSceneObjects();
  ASSERT_EQ(objects.size(), 1U);
  ASSERT_TRUE(std::holds_alternative<core::SphereShapeDTO>(
      objects.front().collision_shapes.front()));
  const auto *fixed = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
      &objects.front().placement);
  ASSERT_NE(fixed, nullptr);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.x_m, 1.25);

  auto no_op = replace;
  no_op.request_context =
      SceneRequestContext("scene-request-upsert-no-op", active);
  no_op.expected_projection_revision = active;
  const auto unchanged = owner.Commit(no_op);
  ASSERT_TRUE(unchanged.success);
  ASSERT_TRUE(unchanged.update_status.has_value());
  EXPECT_EQ(*unchanged.update_status, core::SceneUpdateStatusDTO::kNoOp);
  EXPECT_EQ(unchanged.committed_projection_revision, active);

  auto relative =
      AddBoxRequest(active, admission.cell_id(), "scene-request-upsert-relative");
  const auto relative_add =
      std::get<core::AddObjectRequestDTO>(relative.operation);
  auto relative_pose = relative_add.pose;
  relative_pose.position_m = {0.5, 1.0, -0.25};
  relative.operation = core::UpsertObjectRequestDTO{
      core::SceneResourceId("scene-relative"),
      relative_add.shape,
      relative_pose,
      core::SceneResourceSceneFrameRefDTO{
          core::SceneResourceId("scene-box")},
      relative_add.support_contacts};
  const auto relative_result = owner.Commit(relative);
  ASSERT_TRUE(relative_result.success)
      << (relative_result.error.has_value() ? relative_result.error->message
                                            : "missing error");
  const auto with_relative = owner.GetSceneObjects();
  const auto relative_object = std::find_if(
      with_relative.begin(), with_relative.end(), [](const auto &object) {
        return object.scene_resource_id ==
               core::SceneResourceId("scene-relative");
      });
  ASSERT_NE(relative_object, with_relative.end());
  const auto *relative_fixed =
      std::get_if<core::CellFixedSceneObjectPlacementDTO>(
          &relative_object->placement);
  ASSERT_NE(relative_fixed, nullptr);
  EXPECT_DOUBLE_EQ(relative_fixed->pose_in_cell.position_m.x_m, 1.75);
  EXPECT_DOUBLE_EQ(relative_fixed->pose_in_cell.position_m.y_m, 0.5);
  EXPECT_DOUBLE_EQ(relative_fixed->pose_in_cell.position_m.z_m, 0.5);
}

TEST(CanonicalEmptySceneOwner,
     ProgramResetRestorationUsesExactAuthorizationWithoutOpeningAuthoring) {
  TemporaryDirectory directory("scene-program-reset");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader,
                                 cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  auto request = AddBoxRequest(active, admission.cell_id(),
                               "scene-request-program-reset");
  request.expected_admission_generation.reset();
  request.program_reset_scene_authorization =
      core::ProgramResetSceneAuthorizationDTO{
          RequestOwnerEpoch("program-runner.epoch.2"),
          core::ProgramRunId("program-cycle.2"),
          core::CellAdmissionClaimGeneration(9U), Digest('e')};

  cell_admission.RejectProgramResetSceneAuthorization(true);
  const auto rejected = owner.Commit(request);
  ASSERT_FALSE(rejected.success);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code, "authoring_not_ready");
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);

  cell_admission.RejectProgramResetSceneAuthorization(false);
  const auto committed = owner.Commit(request);
  ASSERT_TRUE(committed.success)
      << (committed.error.has_value() ? committed.error->message
                                      : "missing error");
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
}

TEST(CanonicalEmptySceneOwner,
     ZeroNativeSceneCommitPublishesReplaysAndRestarts) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  ProjectionRevisionDTO committed_projection;
  std::optional<core::SceneTransactionRequestDTO> request;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader, cell_admission);
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    request =
        AddBoxRequest(accepted.canonical_empty_target, admission.cell_id());

    const auto committed = owner.Commit(*request);
    ASSERT_TRUE(committed.presence_valid());
    ASSERT_TRUE(committed.success);
    ASSERT_TRUE(committed.transaction_sequence.has_value());
    ASSERT_TRUE(committed.committed_projection_revision.has_value());
    ASSERT_TRUE(committed.update_status.has_value());
    EXPECT_EQ(*committed.transaction_sequence, 1U);
    EXPECT_EQ(*committed.update_status,
              core::SceneUpdateStatusDTO::kGeometryChanged);
    EXPECT_EQ(committed.projection_status,
              core::SceneProjectionCommitStatusDTO::kCommitted);
    EXPECT_EQ(
        committed.committed_projection_revision->scene_revision.revision_id,
        2U);
    committed_projection = *committed.committed_projection_revision;
    EXPECT_EQ(owner.CurrentProjectionRevision(), committed_projection);

    const auto objects = owner.GetSceneObjects();
    ASSERT_EQ(objects.size(), 1U);
    EXPECT_EQ(objects.front().scene_resource_id,
              core::SceneResourceId("scene-box"));
    const auto state = owner.GetObjectState(core::SceneResourceId("scene-box"));
    EXPECT_TRUE(state.exists);
    EXPECT_FALSE(state.attached);
    const auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box"));
    ASSERT_TRUE(pose.ok());
    EXPECT_DOUBLE_EQ(pose.value().position_m.x_m, 0.25);

    auto relayed_request = *request;
    relayed_request.attribution = {
        core::RequestTransportDTO::kHttp,
        core::RequestAssuranceDTO::kExternallyAuthenticated,
        std::string("operator.7"),
    };
    const auto replay = owner.Commit(relayed_request);
    ASSERT_TRUE(replay.success);
    EXPECT_EQ(replay.replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(replay.transaction_sequence, committed.transaction_sequence);
    EXPECT_EQ(replay.committed_projection_revision,
              committed.committed_projection_revision);

    auto conflict = *request;
    std::get<core::AddObjectRequestDTO>(conflict.operation)
        .pose.position_m.x_m = 9.0;
    const auto rejected = owner.Commit(conflict);
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.replay_status,
              core::RequestReplayStatusDTO::kRequestIdConflict);
    ASSERT_TRUE(rejected.error.has_value());
    EXPECT_EQ(rejected.error->code, "request_id_conflict");

    const auto integrity = owner.VerifyIntegrity();
    EXPECT_EQ(integrity.transaction_intent_count, 1);
    EXPECT_EQ(integrity.request_outcome_count, 1);
    EXPECT_EQ(integrity.projection_attempt_count, 0);
    EXPECT_EQ(integrity.projection_result_count, 0);
    EXPECT_EQ(integrity.owner_event_count, 1);
    EXPECT_FALSE(integrity.projection_desynchronized);
    owner.Close();
  }

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  EXPECT_EQ(reopened.CurrentProjectionRevision(), committed_projection);
  const auto snapshot = reopened.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), 1U);
  EXPECT_EQ(snapshot->transaction_sequence, 1U);
  ASSERT_TRUE(request.has_value());
  const auto replay = reopened.Commit(*request);
  ASSERT_TRUE(replay.success);
  EXPECT_EQ(replay.replay_status, core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.committed_projection_revision, committed_projection);
}

TEST(CanonicalEmptySceneOwner,
     LiveStateCacheReconstructsAfterAnotherOwnerConnectionCommits) {
  TemporaryDirectory directory("scene-external-owner-write");
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner first(database_path, admission, publication_reader,
                                 cell_admission);
  const auto initial =
      FinalizeCanonicalBootstrap(first, publication_reader, admission)
          .canonical_empty_target;
  CanonicalEmptySceneOwner second(database_path, admission,
                                  publication_reader, cell_admission);

  // Warm both process-local views before either connection advances the scene.
  EXPECT_EQ(first.CurrentProjectionRevision(), initial);
  EXPECT_EQ(second.CurrentProjectionRevision(), initial);

  const auto first_result = first.Commit(AddBoxRequest(
      initial, admission.cell_id(), "scene-request-first-owner"));
  ASSERT_TRUE(first_result.success);
  ASSERT_TRUE(first_result.committed_projection_revision.has_value());

  // The other connection must observe the committed revision rather than use
  // its stale cached predecessor. Its stale request still fails before effect.
  auto stale_request = AddBoxRequest(
      initial, admission.cell_id(), "scene-request-stale-second-owner");
  std::get<core::AddObjectRequestDTO>(stale_request.operation)
      .scene_resource_id = core::SceneResourceId("scene-box-stale");
  const auto stale_result = second.Commit(stale_request);
  ASSERT_FALSE(stale_result.success);
  ASSERT_TRUE(stale_result.error.has_value());
  EXPECT_EQ(stale_result.error->code, "stale_revision");
  EXPECT_EQ(second.CurrentProjectionRevision(),
            *first_result.committed_projection_revision);

  auto second_request = AddBoxRequest(
      *first_result.committed_projection_revision, admission.cell_id(),
      "scene-request-second-owner");
  std::get<core::AddObjectRequestDTO>(second_request.operation)
      .scene_resource_id = core::SceneResourceId("scene-box-second");
  const auto second_result = second.Commit(second_request);
  ASSERT_TRUE(second_result.success);
  ASSERT_TRUE(second_result.committed_projection_revision.has_value());

  EXPECT_EQ(first.CurrentProjectionRevision(),
            *second_result.committed_projection_revision);
  EXPECT_EQ(first.LogicalSnapshot()->objects.size(), 2U);
  EXPECT_EQ(first.VerifyIntegrity().transaction_intent_count, 2);
}

TEST(CanonicalEmptySceneOwner,
     PortableSupportDeclarationsNormalizePersistAndProtectReferences) {
  TemporaryDirectory directory("scene-support-declarations");
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  ProjectionRevisionDTO projection;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader, cell_admission);
    projection =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;

    for (const auto &support_id : {"support-a", "support-b"}) {
      auto request =
          AddBoxRequest(projection, admission.cell_id(),
                        std::string("scene-request-add-") + support_id);
      std::get<core::AddObjectRequestDTO>(request.operation).scene_resource_id =
          core::SceneResourceId(support_id);
      const auto result = owner.Commit(request);
      ASSERT_TRUE(result.success);
      projection = *result.committed_projection_revision;
    }

    core::SupportContactDTO bounded;
    bounded.support_object_id = "support-b";
    bounded.mode = core::CollisionContactPolicyMode::kBoundedPenetration;
    bounded.max_contact_penetration_m = 0.002;
    core::SupportContactDTO binary;
    binary.support_object_id = "support-a";
    binary.release_evidence_mode =
        core::ContactReleaseEvidenceMode::kBinaryContact;
    auto add_supported =
        AddBoxRequest(projection, admission.cell_id(),
                      "scene-request-add-supported", {bounded, binary});
    const auto supported = owner.Commit(add_supported);
    ASSERT_TRUE(supported.success);
    projection = *supported.committed_projection_revision;

    const auto snapshot = owner.LogicalSnapshot();
    const auto object =
        std::lower_bound(snapshot->objects.begin(), snapshot->objects.end(),
                         core::SceneResourceId("scene-box"),
                         [](const core::SceneObjectSnapshotDTO &candidate,
                            const core::SceneResourceId &id) {
                           return candidate.scene_resource_id < id;
                         });
    ASSERT_NE(object, snapshot->objects.end());
    ASSERT_EQ(object->support_contacts.size(), 2U);
    EXPECT_EQ(object->support_contacts[0].support_object_id, "support-a");
    EXPECT_EQ(object->support_contacts[1].support_object_id, "support-b");
    EXPECT_FALSE(object->support_contacts[0].enforce_max_start_penetration);
    EXPECT_DOUBLE_EQ(object->support_contacts[0].release_tolerance_m, 0.0);
    EXPECT_FALSE(
        object->support_contacts[0].require_strictly_decreasing_penetration);
    EXPECT_FALSE(object->support_contacts[1].require_separation_by_end);
    EXPECT_DOUBLE_EQ(object->support_contacts[1].max_contact_penetration_m,
                     0.002);

    core::SceneTransactionRequestDTO remove_support{
        SceneRequestContext("scene-request-remove-support", projection),
        {},
        projection,
        core::RemoveObjectRequestDTO{core::SceneResourceId("support-a")},
        core::AdmissionGeneration(1U),
    };
    const auto removal = owner.Commit(remove_support);
    ASSERT_FALSE(removal.success);
    ASSERT_TRUE(removal.error.has_value());
    EXPECT_EQ(removal.error->code, "scene_object_state_conflict");
    EXPECT_EQ(removal.error->details.at("reason"),
              "support_contact_reference_exists");

    auto missing = AddBoxRequest(
        projection, admission.cell_id(), "scene-request-missing-support",
        {core::SupportContactDTO{.support_object_id = "missing-support"}});
    std::get<core::AddObjectRequestDTO>(missing.operation).scene_resource_id =
        core::SceneResourceId("missing-dependent");
    const auto missing_result = owner.Commit(missing);
    ASSERT_FALSE(missing_result.success);
    ASSERT_TRUE(missing_result.error.has_value());
    EXPECT_EQ(missing_result.error->code, "missing_reference");

    auto self = AddBoxRequest(
        projection, admission.cell_id(), "scene-request-self-support",
        {core::SupportContactDTO{.support_object_id = "self-supported"}});
    std::get<core::AddObjectRequestDTO>(self.operation).scene_resource_id =
        core::SceneResourceId("self-supported");
    const auto self_result = owner.Commit(self);
    ASSERT_FALSE(self_result.success);
    ASSERT_TRUE(self_result.error.has_value());
    EXPECT_EQ(self_result.error->code, "invalid_contact_policy");
    EXPECT_EQ(self_result.error->details.at("reason"), "self_support_contact");

    auto duplicate = AddBoxRequest(
        projection, admission.cell_id(), "scene-request-duplicate-support",
        {core::SupportContactDTO{.support_object_id = "support-a"},
         core::SupportContactDTO{.support_object_id = "support-a"}});
    std::get<core::AddObjectRequestDTO>(duplicate.operation).scene_resource_id =
        core::SceneResourceId("duplicate-dependent");
    const auto duplicate_result = owner.Commit(duplicate);
    ASSERT_FALSE(duplicate_result.success);
    ASSERT_TRUE(duplicate_result.error.has_value());
    EXPECT_EQ(duplicate_result.error->details.at("reason"),
              "duplicate_support_contact_pair");
    EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 3);
    owner.Close();
  }

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  EXPECT_EQ(reopened.CurrentProjectionRevision(), projection);
  const auto restored = reopened.LogicalSnapshot();
  const auto object = std::find_if(
      restored->objects.begin(), restored->objects.end(), [](const auto &row) {
        return row.scene_resource_id == core::SceneResourceId("scene-box");
      });
  ASSERT_NE(object, restored->objects.end());
  ASSERT_EQ(object->support_contacts.size(), 2U);
  EXPECT_EQ(object->support_contacts.front().support_object_id, "support-a");
}

TEST(CanonicalEmptySceneOwner, CommitsTheClosedPortableCollisionShapeSet) {
  TemporaryDirectory directory("scene-shape-set");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  auto projection = accepted.canonical_empty_target;
  const std::vector<std::pair<std::string, core::CollisionShapeDTO>> shapes{
      {"box", core::BoxShapeDTO{{0.1, 0.2, 0.3}}},
      {"sphere", core::SphereShapeDTO{0.05}},
      {"cylinder", core::CylinderShapeDTO{0.04, 0.2}},
      {"mesh", core::MeshShapeDTO{"package://manyforge_test/fixture.stl",
                                  {1.0, 1.0, 1.0}}},
  };
  for (const auto &[name, shape] : shapes) {
    auto request = AddBoxRequest(projection, admission.cell_id(),
                                 "scene-request-shape-" + name);
    auto &operation = std::get<core::AddObjectRequestDTO>(request.operation);
    operation.scene_resource_id = core::SceneResourceId("scene-" + name);
    operation.shape = shape;
    const auto result = owner.Commit(request);
    ASSERT_TRUE(result.success)
        << (result.error.has_value() ? result.error->message : "missing error");
    projection = *result.committed_projection_revision;
  }

  const auto snapshot = owner.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), shapes.size());
  EXPECT_TRUE(std::holds_alternative<core::BoxShapeDTO>(
      snapshot->objects[0].collision_shapes.front()));
  EXPECT_TRUE(std::holds_alternative<core::CylinderShapeDTO>(
      snapshot->objects[1].collision_shapes.front()));
  EXPECT_TRUE(std::holds_alternative<core::MeshShapeDTO>(
      snapshot->objects[2].collision_shapes.front()));
  EXPECT_TRUE(std::holds_alternative<core::SphereShapeDTO>(
      snapshot->objects[3].collision_shapes.front()));
  EXPECT_EQ(snapshot->transaction_sequence, 4U);
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 4);
}

core::PublicRequestContextDTO
SceneRequestContext(std::string request_id,
                    const ProjectionRevisionDTO &expected) {
  return {
      core::RequestId(std::move(request_id)),
      core::RequestOwnerEpoch(expected.scene_revision.scene_id.value()),
      "scene-owner-test",
      std::nullopt,
  };
}

class FakePlannerSceneProjection final : public core::IPlannerSceneProjection {
public:
  enum class Mode { kCommit, kRollbackProven, kUncertain, kMalformed };

  explicit FakePlannerSceneProjection(std::string identity)
      : FakePlannerSceneProjection(ProjectionNativeRuntimeBindingDTO{
            core::CapabilityProviderBindingId("provider-" + identity),
            core::CapabilityProviderBindingRevision("binding-revision-" +
                                                    identity),
            core::RuntimeUnitId("runtime-" + identity),
            core::RuntimeEpoch("runtime-epoch-" + identity),
        }) {}

  explicit FakePlannerSceneProjection(
      ProjectionNativeRuntimeBindingDTO binding)
      : binding_(std::move(binding)),
        health_{binding_,
                1U,
                core::ProjectionHealthState::kUninitialized,
                std::nullopt,
                1U,
                Digest('8'),
                std::nullopt} {
    if (!binding_.valid()) {
      throw std::invalid_argument("test native projection binding is invalid");
    }
  }

  void Reset(const ProjectionRevisionDTO &target, const Mode mode) {
    mode_ = mode;
    calls_ = 0U;
    rebuild_calls_ = 0U;
    frame_resolution_calls_ = 0U;
    policy_projection_calls_ = 0U;
    throw_after_next_rebuild_effect_ = false;
    rebuild_entered_ = nullptr;
    rebuild_release_ = nullptr;
    accepted_rebuild_binding_.reset();
    frame_resolution_error_.reset();
    resolved_model_instance_override_.reset();
    resolved_frame_role_override_.reset();
    retained_leases_.clear();
    transactions_.clear();
    health_ = core::ProjectionHealthDTO{
        binding_,    1U,           core::ProjectionHealthState::kHealthy,
        target,      1U,
        Digest('8'), std::nullopt,
    };
  }

  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }
  [[nodiscard]] std::size_t rebuild_calls() const noexcept {
    return rebuild_calls_;
  }
  [[nodiscard]] const ProjectionNativeRuntimeBindingDTO &binding() const
      noexcept {
    return binding_;
  }
  [[nodiscard]] const std::vector<core::SceneTransactionDTO> &
  transactions() const noexcept {
    return transactions_;
  }

  void SetFrameResolution(core::Pose3DTO pose,
                          std::vector<core::ComponentAttachmentRevision>
                              attachment_revisions = {}) {
    frame_pose_in_cell_ = std::move(pose);
    attachment_revisions_ = std::move(attachment_revisions);
  }

  void RejectFrameResolution(core::ErrorDTO error) {
    frame_resolution_error_ = std::move(error);
  }

  void SetResolvedModelInstanceOverride(
      std::optional<core::ModelInstanceId> model_instance_id) {
    resolved_model_instance_override_ = std::move(model_instance_id);
  }

  void SetResolvedFrameRoleOverride(
      std::optional<core::FrameRoleId> frame_role_id) {
    resolved_frame_role_override_ = std::move(frame_role_id);
  }

  [[nodiscard]] std::size_t frame_resolution_calls() const noexcept {
    return frame_resolution_calls_;
  }

  [[nodiscard]] std::size_t policy_projection_calls() const noexcept {
    return policy_projection_calls_;
  }

  [[nodiscard]] std::size_t retained_lease_count() const noexcept {
    return retained_leases_.size();
  }

  void SetMode(const Mode mode) noexcept { mode_ = mode; }

  void ThrowAfterNextRebuildEffect() noexcept {
    throw_after_next_rebuild_effect_ = true;
  }

  void AcceptNextRebuildAs(
      ProjectionNativeRuntimeBindingDTO resulting_binding) {
    accepted_rebuild_binding_ = std::move(resulting_binding);
  }

  void BlockNextRebuild(std::barrier<> &entered, std::barrier<> &release) {
    rebuild_entered_ = &entered;
    rebuild_release_ = &release;
  }

  core::ProjectionCommitResultDTO
  ApplyTransaction(const core::SceneTransactionDTO &transaction) override {
    ++calls_;
    transactions_.push_back(transaction);
    ++health_.source_sequence;
    health_.native_projection_generation =
        *health_.native_projection_generation + 1U;
    if (mode_ == Mode::kCommit) {
      health_.state = core::ProjectionHealthState::kHealthy;
      health_.active_projection_revision =
          transaction.proposed_projection_revision;
      health_.error = std::nullopt;
      return {
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          core::ProjectionCommitResultStateDTO::kCommitted,
          transaction.proposed_projection_revision,
          health_.native_projection_generation,
          false,
          health_,
          std::nullopt,
      };
    }
    const ErrorDTO rejected{
        mode_ == Mode::kRollbackProven ? "projection_rejected_rollback_proven"
                                       : "projection_state_uncertain",
        "injected native projection result",
        {},
    };
    if (mode_ == Mode::kRollbackProven) {
      health_.state = core::ProjectionHealthState::kHealthy;
      health_.active_projection_revision =
          transaction.prior_projection_revision;
      health_.error = std::nullopt;
      return {
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          core::ProjectionCommitResultStateDTO::kRejectedRollbackProven,
          transaction.prior_projection_revision,
          health_.native_projection_generation,
          true,
          health_,
          rejected,
      };
    }
    if (mode_ == Mode::kMalformed) {
      health_.state = core::ProjectionHealthState::kHealthy;
      health_.active_projection_revision =
          transaction.prior_projection_revision;
      health_.error = std::nullopt;
      return {
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          core::ProjectionCommitResultStateDTO::kCommitted,
          transaction.prior_projection_revision,
          health_.native_projection_generation,
          false,
          health_,
          std::nullopt,
      };
    }
    health_.state = core::ProjectionHealthState::kDesynchronized;
    health_.active_projection_revision = transaction.prior_projection_revision;
    health_.error = rejected;
    return {
        transaction.transaction_sequence,
        transaction.proposed_projection_revision,
        core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain,
        std::nullopt,
        health_.native_projection_generation,
        false,
        health_,
        rejected,
    };
  }

  core::Result<core::CollisionWorldLeaseInventory>
  RetainProjection(const ProjectionRevisionDTO &,
                   core::ProjectionLeaseId) override {
    return core::Result<core::CollisionWorldLeaseInventory>::Err(
        ErrorDTO{"unsupported_capability",
                 "test projection does not provide leases",
                 {}});
  }

  core::Result<core::ProjectionLeaseBindingDTO>
  RetainProjectionBinding(const ProjectionRevisionDTO &revision,
                          core::ProjectionLeaseId lease_id) override {
    if (!health_.healthy_for_target(revision) || lease_id.empty() ||
        !retained_leases_.insert(lease_id).second) {
      return core::Result<core::ProjectionLeaseBindingDTO>::Err(
          ErrorDTO{"live_scene_snapshot_unavailable",
                   "test projection cannot retain the requested revision",
                   {{"reason", "test_projection_lease_rejected"}}});
    }
    return core::Result<core::ProjectionLeaseBindingDTO>::Ok(
        core::ProjectionLeaseBindingDTO{revision, std::move(lease_id)});
  }

  void ReleaseProjection(const core::ProjectionLeaseId &lease_id) override {
    retained_leases_.erase(lease_id);
  }

  core::Result<core::SceneFrameResolutionReceiptDTO>
  ResolveSceneFrame(
      const core::SceneFrameResolutionQueryDTO &query) const override {
    ++frame_resolution_calls_;
    if (!query.valid() ||
        !retained_leases_.contains(
            query.projection_binding.projection_lease_id)) {
      return core::Result<core::SceneFrameResolutionReceiptDTO>::Err(
          ErrorDTO{"live_scene_snapshot_unavailable",
                   "test frame query has no retained projection",
                   {{"reason", "test_projection_lease_missing"}}});
    }
    if (frame_resolution_error_.has_value()) {
      return core::Result<core::SceneFrameResolutionReceiptDTO>::Err(
          *frame_resolution_error_);
    }
    const auto frame = std::visit(
        [&](const auto &selector) -> core::ResolvedFrameDTO {
          using Selector = std::remove_cvref_t<decltype(selector)>;
          if constexpr (std::is_same_v<Selector,
                                       core::TargetCarrierSceneFrameRefDTO>) {
            return core::ResolvedComponentModelFrameDTO{
                selector.controllable_component_id,
                resolved_model_instance_override_.value_or(
                    core::ModelInstanceId("model.1")),
                resolved_frame_role_override_.value_or(selector.frame_role_id),
                "compiled_" +
                    resolved_frame_role_override_
                        .value_or(selector.frame_role_id)
                        .value(),
                core::ModelRevision("model-revision-1")};
          } else {
            return std::visit(
                [&](const auto &reference) -> core::ResolvedFrameDTO {
                  using Reference =
                      std::remove_cvref_t<decltype(reference)>;
                  if constexpr (std::is_same_v<
                                    Reference,
                                    core::CellSceneFrameRefDTO>) {
                    return core::ResolvedCellFrameDTO{
                        reference.cell_id, "compiled_cell"};
                  } else if constexpr (std::is_same_v<
                                           Reference,
                                           core::ComponentSceneFrameRefDTO>) {
                    return core::ResolvedComponentModelFrameDTO{
                        reference.controllable_component_id,
                        resolved_model_instance_override_.value_or(
                            reference.model_instance_id),
                        reference.frame_role_id,
                        "compiled_" + reference.frame_role_id.value(),
                        core::ModelRevision("model-revision-1")};
                  } else {
                    return core::ResolvedPassiveModelFrameDTO{
                        reference.model_instance_id,
                        reference.frame_role_id,
                        "compiled_" + reference.frame_role_id.value(),
                        core::ModelRevision("model-revision-1")};
                  }
                },
                selector);
          }
        },
        query.selector);
    return core::Result<core::SceneFrameResolutionReceiptDTO>::Ok(
        core::SceneFrameResolutionReceiptDTO{
            frame, frame_pose_in_cell_, attachment_revisions_,
            query.projection_binding, std::nullopt, Digest('9')});
  }

  core::Result<core::CollisionPolicySnapshot>
  ProjectEffectiveCollisionPolicy(
      const core::LogicalSceneSnapshotDTO &snapshot,
      const std::uint64_t effective_policy_revision_id) const override {
    ++policy_projection_calls_;
    if (!snapshot.locally_valid() ||
        effective_policy_revision_id != snapshot.revision.revision_id) {
      return core::Result<core::CollisionPolicySnapshot>::Err(
          ErrorDTO{"invalid_request",
                   "test collision-policy query is malformed", {}});
    }
    auto effective = snapshot.configured_collision_policy;
    effective.revision.revision_id = effective_policy_revision_id;
    effective.revision.content_hash =
        core::logical_scene_identity::
            ComputeCollisionPolicyContentDigest(effective)
                .value();
    return core::Result<core::CollisionPolicySnapshot>::Ok(
        std::move(effective));
  }

  core::ProjectionHealthDTO ProjectionHealth() const override {
    return health_;
  }

  core::ProjectionCommitResultDTO
  RebuildProjection(const core::NativeProjectionRebuildDTO &request) override {
    if (!request.valid() ||
        request.expected_native_runtime_binding != binding_) {
      throw std::invalid_argument(
          "test native projection rebuild request is foreign");
    }
    ++rebuild_calls_;
    if (rebuild_entered_ != nullptr && rebuild_release_ != nullptr) {
      auto *entered = std::exchange(rebuild_entered_, nullptr);
      auto *release = std::exchange(rebuild_release_, nullptr);
      entered->arrive_and_wait();
      release->arrive_and_wait();
    }
    if (accepted_rebuild_binding_.has_value()) {
      auto accepted_health = health_;
      accepted_health.native_runtime_binding = *accepted_rebuild_binding_;
      accepted_health.state = core::ProjectionHealthState::kRebuilding;
      accepted_health.active_projection_revision = std::nullopt;
      accepted_health.error = std::nullopt;
      ++accepted_health.source_sequence;
      accepted_rebuild_binding_.reset();
      core::ProjectionCommitResultDTO accepted{
          std::nullopt,
          request.target_projection_revision,
          core::ProjectionCommitResultStateDTO::kAccepted,
          std::nullopt,
          accepted_health.native_projection_generation,
          false,
          std::move(accepted_health),
          std::nullopt,
      };
      if (!accepted.presence_valid()) {
        throw std::logic_error("test Accepted rebuild result is invalid");
      }
      return accepted;
    }
    ++health_.source_sequence;
    health_.state = core::ProjectionHealthState::kHealthy;
    health_.active_projection_revision = request.target_projection_revision;
    health_.native_projection_generation =
        *health_.native_projection_generation + 1U;
    health_.projection_content_digest = Digest('6');
    health_.error = std::nullopt;
    if (throw_after_next_rebuild_effect_) {
      throw_after_next_rebuild_effect_ = false;
      throw std::runtime_error(
          "injected lost native projection rebuild response");
    }
    return {
        std::nullopt,
        request.target_projection_revision,
        core::ProjectionCommitResultStateDTO::kCommitted,
        request.target_projection_revision,
        health_.native_projection_generation,
        false,
        health_,
        std::nullopt,
    };
  }

  core::NativeProjectionBootstrapRebuildResultDTO RebuildBootstrapProjection(
      const core::NativeProjectionBootstrapRebuildDTO &) override {
    throw std::logic_error("test projection bootstrap is not configured");
  }

  core::NativeProjectionBootstrapStatusDTO BootstrapProjectionStatus(
      const core::NativeProjectionBootstrapStatusQueryDTO &) const override {
    throw std::logic_error("test projection bootstrap is not configured");
  }


private:
  ProjectionNativeRuntimeBindingDTO binding_;
  Mode mode_{Mode::kCommit};
  core::ProjectionHealthDTO health_;
  bool throw_after_next_rebuild_effect_{};
  std::barrier<> *rebuild_entered_{nullptr};
  std::barrier<> *rebuild_release_{nullptr};
  std::optional<ProjectionNativeRuntimeBindingDTO>
      accepted_rebuild_binding_;
  std::size_t calls_{};
  std::size_t rebuild_calls_{};
  std::vector<core::SceneTransactionDTO> transactions_;
  core::Pose3DTO frame_pose_in_cell_{};
  std::vector<core::ComponentAttachmentRevision> attachment_revisions_;
  std::optional<core::ErrorDTO> frame_resolution_error_;
  std::optional<core::ModelInstanceId> resolved_model_instance_override_;
  std::optional<core::FrameRoleId> resolved_frame_role_override_;
  mutable std::size_t frame_resolution_calls_{};
  mutable std::size_t policy_projection_calls_{};
  std::set<core::ProjectionLeaseId> retained_leases_;
};

/// Deliberately return a distinct forwarding client from the resolver so the
/// owner exercises only the transport-neutral port, never a participant
/// pointer supplied as an alternative source of runtime truth.
class ForwardingPlannerSceneProjectionClient final
    : public core::IPlannerSceneProjection {
public:
  explicit ForwardingPlannerSceneProjectionClient(
      FakePlannerSceneProjection &projection)
      : projection_(projection) {}

  core::ProjectionCommitResultDTO
  ApplyTransaction(const core::SceneTransactionDTO &request) override {
    return projection_.ApplyTransaction(request);
  }

  core::Result<core::CollisionWorldLeaseInventory>
  RetainProjection(const ProjectionRevisionDTO &revision,
                   core::ProjectionLeaseId lease_id) override {
    return projection_.RetainProjection(revision, std::move(lease_id));
  }

  core::Result<core::ProjectionLeaseBindingDTO>
  RetainProjectionBinding(const ProjectionRevisionDTO &revision,
                          core::ProjectionLeaseId lease_id) override {
    return projection_.RetainProjectionBinding(revision,
                                               std::move(lease_id));
  }

  void ReleaseProjection(const core::ProjectionLeaseId &lease_id) override {
    projection_.ReleaseProjection(lease_id);
  }

  core::ProjectionHealthDTO ProjectionHealth() const override {
    return projection_.ProjectionHealth();
  }

  core::ProjectionCommitResultDTO RebuildProjection(
      const core::NativeProjectionRebuildDTO &request) override {
    return projection_.RebuildProjection(request);
  }

  core::NativeProjectionBootstrapRebuildResultDTO RebuildBootstrapProjection(
      const core::NativeProjectionBootstrapRebuildDTO &request) override {
    return projection_.RebuildBootstrapProjection(request);
  }

  core::NativeProjectionBootstrapStatusDTO BootstrapProjectionStatus(
      const core::NativeProjectionBootstrapStatusQueryDTO &query) const
      override {
    return projection_.BootstrapProjectionStatus(query);
  }

  core::Result<core::SceneFrameResolutionReceiptDTO>
  ResolveSceneFrame(
      const core::SceneFrameResolutionQueryDTO &query) const override {
    return projection_.ResolveSceneFrame(query);
  }

  core::Result<core::CollisionPolicySnapshot>
  ProjectEffectiveCollisionPolicy(
      const core::LogicalSceneSnapshotDTO &snapshot,
      const std::uint64_t effective_policy_revision_id) const override {
    return projection_.ProjectEffectiveCollisionPolicy(snapshot,
                                                       effective_policy_revision_id);
  }

private:
  FakePlannerSceneProjection &projection_;
};

class FakePlannerSceneProjectionResolver final
    : public core::IPlannerSceneProjectionResolver {
public:
  explicit FakePlannerSceneProjectionResolver(
      FakePlannerSceneProjection &projection)
      : projections_{&projection} {}

  FakePlannerSceneProjectionResolver(
      std::initializer_list<FakePlannerSceneProjection *> projections)
      : projections_(projections) {}

  explicit FakePlannerSceneProjectionResolver(
      std::vector<FakePlannerSceneProjection *> projections)
      : projections_(std::move(projections)) {}

  void ReplaceWith(FakePlannerSceneProjection &projection) {
    projections_ = {&projection};
  }

  void ReplaceWith(std::vector<FakePlannerSceneProjection *> projections) {
    projections_ = std::move(projections);
  }

  core::Result<std::shared_ptr<core::IPlannerSceneProjection>>
  ResolveProjection(
      const ProjectionNativeRuntimeBindingDTO &binding) const override {
    const auto found = std::find_if(
        projections_.begin(), projections_.end(),
        [&binding](const FakePlannerSceneProjection *projection) {
          return projection != nullptr && projection->binding() == binding;
        });
    if (found == projections_.end()) {
      return core::Result<
          std::shared_ptr<core::IPlannerSceneProjection>>::Err(
          ErrorDTO{"missing_reference",
                   "test resolver has no exact native projection", {}});
    }
    return core::Result<std::shared_ptr<core::IPlannerSceneProjection>>::Ok(
        std::make_shared<ForwardingPlannerSceneProjectionClient>(**found));
  }

private:
  std::vector<FakePlannerSceneProjection *> projections_;
};

[[nodiscard]] std::vector<ProjectionNativeRuntimeBindingDTO>
NativeRuntimeBindings(const core::RetainedBundleDTO &bundle,
                      const std::string_view epoch_prefix) {
  std::set<core::RuntimeUnitId> declared_runtime_units;
  for (const auto &runtime_unit : bundle.cell_graph.runtime_units) {
    declared_runtime_units.insert(runtime_unit.runtime_unit_id);
  }

  std::vector<ProjectionNativeRuntimeBindingDTO> result;
  for (const auto &binding : bundle.capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(
            binding.capability_id,
            binding.capability_definition_revision)) {
      continue;
    }
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : binding.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      if (runtime_unit == nullptr || runtime_unit->empty()) {
        throw std::logic_error(
            "native test fixture has a malformed runtime endpoint");
      }
      runtime_sources.insert(*runtime_unit);
    }
    if (runtime_sources.size() != 1U ||
        !declared_runtime_units.contains(*runtime_sources.begin())) {
      throw std::logic_error(
          "native test fixture does not resolve one declared runtime unit");
    }
    result.push_back({
        binding.capability_provider_binding_id,
        binding.capability_provider_binding_revision,
        *runtime_sources.begin(),
        core::RuntimeEpoch(std::string(epoch_prefix) + "-" +
                           std::to_string(result.size() + 1U)),
    });
  }
  std::sort(result.begin(), result.end(),
            core::CanonicalProjectionNativeBindingLess);
  if (result.empty() || std::adjacent_find(result.begin(), result.end()) !=
                            result.end()) {
    throw std::logic_error(
        "native test fixture has no exact canonical projection set");
  }
  return result;
}

using OwnedFakePlannerSceneProjections =
    std::vector<std::unique_ptr<FakePlannerSceneProjection>>;

[[nodiscard]] OwnedFakePlannerSceneProjections MakeFakeProjections(
    const std::vector<ProjectionNativeRuntimeBindingDTO> &bindings) {
  OwnedFakePlannerSceneProjections projections;
  projections.reserve(bindings.size());
  for (const auto &binding : bindings) {
    projections.push_back(
        std::make_unique<FakePlannerSceneProjection>(binding));
  }
  return projections;
}

[[nodiscard]] std::vector<FakePlannerSceneProjection *> ProjectionClients(
    const OwnedFakePlannerSceneProjections &projections) {
  std::vector<FakePlannerSceneProjection *> clients;
  clients.reserve(projections.size());
  for (const auto &projection : projections) {
    clients.push_back(projection.get());
  }
  return clients;
}

void ResetProjections(
    OwnedFakePlannerSceneProjections &projections,
    const ProjectionRevisionDTO &revision,
    const FakePlannerSceneProjection::Mode mode) {
  for (auto &projection : projections) {
    projection->Reset(revision, mode);
  }
}

[[nodiscard]] ProjectionRevisionDTO FinalizeNativeCandidate(
    CanonicalEmptySceneOwner &owner,
    ControlledPublicationReader &publication_reader,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::RetainedBundle &candidate,
    const std::vector<ProjectionNativeRuntimeBindingDTO> &native_bindings,
    std::string apply_id = "apply-native-candidate") {
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const ApplyId operation_id(std::move(apply_id));
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      operation_id,
      ApplyGeneration(1U),
      accepted.canonical_empty_target,
      candidate.value(),
      native_bindings,
  };
  const auto initialized = owner.BeginMaintenanceInitialization(request);
  Require(initialized.state ==
              core::ProjectionMaintenanceInitializationState::kInitialized,
          "native candidate did not initialize");
  const auto installed = owner.InstallInitialScene(request);
  Require(installed.ready_for_publication(),
          "native candidate did not install its initial Scene");
  ActiveBundlePublicationReceiptDTO receipt{
      ActiveBundlePublicationRequestDTO{
          NamedApplyPublicationTransactionDTO{
              operation_id, ApplyGeneration(1U)},
          installed.target_projection_revision,
          installed.target_projection_revision.retained_bundle_id,
          installed.target_projection_revision.graph_revision,
          ActiveBundlePublicationGeneration(1U), Digest('d')},
      ActiveBundlePublicationGeneration(2U), Digest('f')};
  publication_reader.SetCurrent(receipt);
  const auto completed = owner.CompleteMaintenanceInitialization({
      operation_id, ApplyGeneration(1U),
      installed.target_projection_revision,
      installed.authoritative_snapshot_identity, receipt});
  Require(completed.state ==
              core::ProjectionMaintenanceInitializationState::kCompleted,
          "native candidate did not complete");
  return completed.target_projection_revision;
}

TEST(CanonicalEmptySceneOwner,
     InitialSceneIsInstalledOnceBeforePublicationAndSurvivesReadback) {
  TemporaryDirectory directory("scene-initial-baseline");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = NativeRuntimeBindings(candidate.bundle->value(),
                                               "runtime-initial-baseline");
  auto projections = MakeFakeProjections(bindings);
  FakePlannerSceneProjectionResolver resolver(ProjectionClients(projections));
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, bundle_source,
                                 resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const auto authored = owner.Commit(AddBoxRequest(
      accepted.canonical_empty_target, admission.cell_id(), "before-apply"));
  ASSERT_TRUE(authored.success);
  ASSERT_TRUE(authored.committed_projection_revision.has_value());
  const auto predecessor = *authored.committed_projection_revision;
  core::Pose3DTO pose;
  pose.position_m = {0.2, 0.1, 0.3};
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId("apply-initial-baseline"), ApplyGeneration(1U),
      predecessor, candidate.bundle->value(), bindings,
      {core::AddObjectRequestDTO{
           core::SceneResourceId("frame_object"),
           core::SphereShapeDTO{0.05}, pose,
           core::ComponentSceneFrameRefDTO{
               core::ControllableComponentId("candidate-component"),
               core::ModelInstanceId("candidate-model"),
               core::FrameRoleId("candidate-frame")},
           {core::SupportContactDTO{.support_object_id = "wall"}}},
       core::AddObjectRequestDTO{
           core::SceneResourceId("mesh"),
           core::MeshShapeDTO{"asset://fixture/mesh", {1.0, 1.0, 1.0}},
           pose, core::CellSceneFrameRefDTO{admission.cell_id()}, {}},
       core::AddObjectRequestDTO{
          core::SceneResourceId("wall"), core::BoxShapeDTO{{0.1, 0.2, 0.3}},
          pose, core::CellSceneFrameRefDTO{admission.cell_id()}, {}}}};
  const auto provisional = owner.BeginMaintenanceInitialization(request);
  ASSERT_EQ(provisional.state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  EXPECT_FALSE(provisional.ready_for_publication());
  EXPECT_NE(provisional.target_projection_revision.scene_revision,
            predecessor.scene_revision);
  ASSERT_EQ(owner.ReadActiveSnapshot()->shared()->objects.size(), 1U);
  EXPECT_EQ(owner.ReadActiveSnapshot()->shared()->objects.front()
                .scene_resource_id.value(),
            "scene-box");
  for (auto &projection : projections) {
    projection->SetFrameResolution(
        core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}});
  }
  const auto final = owner.InstallInitialScene(request);
  ASSERT_TRUE(final.ready_for_publication());
  EXPECT_NE(final.target_projection_revision, provisional.target_projection_revision);
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(owner.InstallInitialScene(request), final);
  for (const auto &projection : projections) {
    EXPECT_EQ(projection->rebuild_calls(), 2U);
  }
  const core::ActiveBundlePublicationReceiptDTO receipt{
      core::ActiveBundlePublicationRequestDTO{
          core::NamedApplyPublicationTransactionDTO{
              request.apply_id, request.apply_generation},
          final.target_projection_revision,
          final.target_projection_revision.retained_bundle_id,
          final.target_projection_revision.graph_revision,
          ActiveBundlePublicationGeneration(1U), Digest('d')},
      ActiveBundlePublicationGeneration(2U), Digest('f')};
  publication_reader.SetCurrent(receipt);
  const auto completed = owner.CompleteMaintenanceInitialization({
      request.apply_id, request.apply_generation,
      final.target_projection_revision, final.authoritative_snapshot_identity,
      receipt});
  ASSERT_EQ(completed.state,
            core::ProjectionMaintenanceInitializationState::kCompleted);
  const auto snapshot = owner.ReadActiveSnapshot();
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_EQ(snapshot->shared()->objects.size(), 3U);
  EXPECT_EQ(snapshot->shared()->objects.front().scene_resource_id.value(),
            "frame_object");
  ASSERT_EQ(snapshot->shared()->objects.front().support_contacts.size(), 1U);
  EXPECT_EQ(snapshot->shared()->objects.front().support_contacts.front()
                .support_object_id,
            "wall");
  const auto *placement = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
      &snapshot->shared()->objects.front().placement);
  ASSERT_NE(placement, nullptr);
  EXPECT_NEAR(placement->pose_in_cell.position_m.x_m, 1.2, 1e-9);
  EXPECT_NEAR(placement->pose_in_cell.position_m.y_m, 2.1, 1e-9);
  EXPECT_NEAR(placement->pose_in_cell.position_m.z_m, 3.3, 1e-9);
  EXPECT_EQ(snapshot->shared()->objects[1].scene_resource_id.value(), "mesh");
  ASSERT_EQ(snapshot->shared()->objects[1].collision_shapes.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<core::MeshShapeDTO>(
      snapshot->shared()->objects[1].collision_shapes.front()));
  EXPECT_EQ(snapshot->shared()->objects.back().scene_resource_id.value(),
            "wall");
  EXPECT_EQ(snapshot->shared()->revision,
            final.target_projection_revision.scene_revision);

  // Changing only the selected Program may keep the same Deployment bundle.
  // Its empty initial Scene must still replace the prior Program's objects.
  const auto next_bindings = NativeRuntimeBindings(
      candidate.bundle->value(), "runtime-empty-program");
  auto next_projections = MakeFakeProjections(next_bindings);
  resolver.ReplaceWith(ProjectionClients(next_projections));
  const core::ProjectionMaintenanceInitializationRequestDTO empty_program{
      ApplyId("apply-program-only-empty-scene"), ApplyGeneration(1U),
      completed.target_projection_revision, candidate.bundle->value(),
      next_bindings, {}};
  ASSERT_EQ(owner.BeginMaintenanceInitialization(empty_program).state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  const auto empty_installed = owner.InstallInitialScene(empty_program);
  ASSERT_TRUE(empty_installed.ready_for_publication());
  const core::ActiveBundlePublicationReceiptDTO empty_receipt{
      core::ActiveBundlePublicationRequestDTO{
          core::NamedApplyPublicationTransactionDTO{
              empty_program.apply_id, empty_program.apply_generation},
          empty_installed.target_projection_revision,
          empty_installed.target_projection_revision.retained_bundle_id,
          empty_installed.target_projection_revision.graph_revision,
          ActiveBundlePublicationGeneration(2U), Digest('f')},
      ActiveBundlePublicationGeneration(3U), Digest('g')};
  publication_reader.SetCurrent(empty_receipt);
  ASSERT_EQ(owner.CompleteMaintenanceInitialization({
                empty_program.apply_id, empty_program.apply_generation,
                empty_installed.target_projection_revision,
                empty_installed.authoritative_snapshot_identity, empty_receipt})
                .state,
            core::ProjectionMaintenanceInitializationState::kCompleted);
  const auto empty_snapshot = owner.ReadActiveSnapshot();
  ASSERT_TRUE(empty_snapshot.has_value());
  EXPECT_TRUE(empty_snapshot->shared()->objects.empty());
  EXPECT_EQ(empty_snapshot->shared()->revision,
            empty_installed.target_projection_revision.scene_revision);
}

TEST(CanonicalEmptySceneOwner,
     UnresolvableCandidateFrameCannotPublishInitialScene) {
  TemporaryDirectory directory("scene-initial-frame-rejection");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = NativeRuntimeBindings(candidate.bundle->value(),
                                               "runtime-initial-frame");
  auto projections = MakeFakeProjections(bindings);
  for (auto &projection : projections) {
    projection->RejectFrameResolution(
        ErrorDTO{"missing_reference", "candidate frame does not exist",
                 {{"reason", "scene_frame_reference_missing"}}});
  }
  FakePlannerSceneProjectionResolver resolver(ProjectionClients(projections));
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, bundle_source,
                                 resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  core::Pose3DTO pose;
  const core::ProjectionMaintenanceInitializationRequestDTO invalid_geometry{
      ApplyId("apply-invalid-geometry"), ApplyGeneration(1U),
      accepted.canonical_empty_target, candidate.bundle->value(), bindings,
      {core::AddObjectRequestDTO{
          core::SceneResourceId("fixture"), core::SphereShapeDTO{-0.1}, pose,
          core::CellSceneFrameRefDTO{admission.cell_id()}, {}}}};
  EXPECT_THROW(owner.BeginMaintenanceInitialization(invalid_geometry),
               std::invalid_argument);
  EXPECT_EQ(projections.front()->rebuild_calls(), 0U);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId("apply-invalid-frame"), ApplyGeneration(1U),
      accepted.canonical_empty_target, candidate.bundle->value(), bindings,
      {core::AddObjectRequestDTO{
          core::SceneResourceId("fixture"),
          core::BoxShapeDTO{{0.1, 0.2, 0.3}}, pose,
          core::ComponentSceneFrameRefDTO{
              core::ControllableComponentId("missing-component"),
              core::ModelInstanceId("missing-model"),
              core::FrameRoleId("missing-frame")},
          {}}}};
  ASSERT_EQ(owner.BeginMaintenanceInitialization(request).state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  const auto failed = owner.InstallInitialScene(request);
  EXPECT_EQ(failed.state,
            core::ProjectionMaintenanceInitializationState::kFailed);
  EXPECT_FALSE(failed.ready_for_publication());
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_EQ(owner.InstallInitialScene(request), failed);
}

TEST(CanonicalEmptySceneOwner,
     InterruptedInitialSceneEffectReopensClosedWithoutReplaying) {
  TemporaryDirectory directory("scene-initial-effect-interrupted");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = NativeRuntimeBindings(candidate.bundle->value(),
                                               "runtime-initial-interrupted");
  auto projections = MakeFakeProjections(bindings);
  FakePlannerSceneProjectionResolver resolver(ProjectionClients(projections));
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  TestCellAdmissionPort cell_admission;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  std::optional<core::ProjectionMaintenanceInitializationRequestDTO> request;
  std::size_t rebuild_calls = 0U;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader, bundle_source, resolver,
                                   cell_admission);
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    request.emplace(core::ProjectionMaintenanceInitializationRequestDTO{
        ApplyId("apply-initial-effect-interrupted"), ApplyGeneration(1U),
        accepted.canonical_empty_target, candidate.bundle->value(), bindings,
        {core::AddObjectRequestDTO{
            core::SceneResourceId("wall"), core::BoxShapeDTO{{0.1, 0.2, 0.3}},
            core::Pose3DTO{}, core::CellSceneFrameRefDTO{admission.cell_id()},
            {}}}});
    ASSERT_EQ(owner.BeginMaintenanceInitialization(*request).state,
              core::ProjectionMaintenanceInitializationState::kInitialized);
    projections.front()->ThrowAfterNextRebuildEffect();
    const auto interrupted = owner.InstallInitialScene(*request);
    EXPECT_EQ(interrupted.state,
              core::ProjectionMaintenanceInitializationState::kInterrupted);
    EXPECT_TRUE(interrupted.effects_unknown);
    EXPECT_FALSE(interrupted.ready_for_publication());
    EXPECT_EQ(owner.InstallInitialScene(*request), interrupted);
    rebuild_calls = projections.front()->rebuild_calls();
    owner.Close();
  }

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader, bundle_source, resolver,
                                    cell_admission);
  const auto interrupted = reopened.ReadMaintenanceInitialization(
      {request->apply_id, request->apply_generation});
  ASSERT_TRUE(interrupted.has_value());
  EXPECT_EQ(interrupted->state,
            core::ProjectionMaintenanceInitializationState::kInterrupted);
  EXPECT_TRUE(interrupted->effects_unknown);
  EXPECT_FALSE(interrupted->ready_for_publication());
  EXPECT_FALSE(reopened.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(reopened.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(reopened.InstallInitialScene(*request), *interrupted);
  EXPECT_EQ(projections.front()->rebuild_calls(), rebuild_calls);
}

TEST(CanonicalEmptySceneOwner,
     MaintenanceDuplicateObservesOneFreshInitialization) {
  TemporaryDirectory directory("scene-maintenance-duplicate");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = NativeRuntimeBindings(
      candidate.bundle->value(), "runtime-epoch-maintenance-duplicate");
  auto projections = MakeFakeProjections(bindings);
  auto clients = ProjectionClients(projections);
  FakePlannerSceneProjectionResolver resolver(clients);
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, bundle_source,
                                 resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId("maintenance-duplicate"), ApplyGeneration(1U),
      accepted.canonical_empty_target, candidate.bundle->value(), bindings};

  const auto first = owner.BeginMaintenanceInitialization(request);
  ASSERT_EQ(first.state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  for (const auto &projection : projections) {
    EXPECT_EQ(projection->rebuild_calls(), 1U);
  }

  const auto replay = owner.BeginMaintenanceInitialization(request);
  EXPECT_EQ(replay.state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  EXPECT_EQ(replay.target_projection_revision,
            first.target_projection_revision);
  EXPECT_EQ(owner.ReadMaintenanceInitialization(
                {request.apply_id, request.apply_generation}),
            std::optional{first});
  for (const auto &projection : projections) {
    EXPECT_EQ(projection->rebuild_calls(), 1U);
  }
}

TEST(CanonicalEmptySceneOwner,
     MaintenanceStatusRemainsReadableWhileNativeInitializationIsBlocked) {
  TemporaryDirectory directory("scene-maintenance-responsive-status");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = NativeRuntimeBindings(
      candidate.bundle->value(), "runtime-epoch-maintenance-responsive");
  auto projections = MakeFakeProjections(bindings);
  auto clients = ProjectionClients(projections);
  FakePlannerSceneProjectionResolver resolver(clients);
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, bundle_source,
                                 resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId("maintenance-responsive-status"), ApplyGeneration(1U),
      accepted.canonical_empty_target, candidate.bundle->value(), bindings};

  std::barrier rebuild_entered(2);
  std::barrier rebuild_release(2);
  projections.front()->BlockNextRebuild(rebuild_entered, rebuild_release);
  auto initialization = std::async(std::launch::async, [&] {
    return owner.BeginMaintenanceInitialization(request);
  });
  rebuild_entered.arrive_and_wait();

  auto status_read = std::async(std::launch::async, [&] {
    return owner.ReadMaintenanceInitialization(
        {request.apply_id, request.apply_generation});
  });
  const auto status_readiness = status_read.wait_for(1s);
  rebuild_release.arrive_and_wait();

  const auto initialized = initialization.get();
  const auto observed = status_read.get();
  EXPECT_EQ(status_readiness, std::future_status::ready);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->state,
            core::ProjectionMaintenanceInitializationState::kInitializing);
  EXPECT_EQ(initialized.state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
}

TEST(CanonicalEmptySceneOwner,
     InterruptedMaintenanceReopensClosedWithoutAutomaticRetry) {
  TemporaryDirectory directory("scene-maintenance-interrupted-reopen");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = NativeRuntimeBindings(
      candidate.bundle->value(), "runtime-epoch-maintenance-interrupted");
  auto projections = MakeFakeProjections(bindings);
  auto clients = ProjectionClients(projections);
  FakePlannerSceneProjectionResolver resolver(clients);
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  TestCellAdmissionPort cell_admission;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  std::optional<core::ProjectionMaintenanceInitializationRequestDTO>
      interrupted_request;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader, bundle_source, resolver,
                                   cell_admission);
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    interrupted_request.emplace(
        core::ProjectionMaintenanceInitializationRequestDTO{
        ApplyId("maintenance-interrupted"), ApplyGeneration(1U),
        accepted.canonical_empty_target, candidate.bundle->value(), bindings});
    ASSERT_EQ(owner.BeginMaintenanceInitialization(*interrupted_request).state,
              core::ProjectionMaintenanceInitializationState::kInitialized);
    owner.Close();
  }
  std::vector<std::size_t> rebuild_calls;
  for (const auto &projection : projections) {
    rebuild_calls.push_back(projection->rebuild_calls());
  }

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader, bundle_source, resolver,
                                    cell_admission);
  const auto interrupted = reopened.ReadMaintenanceInitialization(
      {interrupted_request->apply_id, interrupted_request->apply_generation});
  ASSERT_TRUE(interrupted.has_value());
  EXPECT_EQ(interrupted->state,
            core::ProjectionMaintenanceInitializationState::kInterrupted);
  EXPECT_TRUE(interrupted->effects_unknown);
  EXPECT_FALSE(reopened.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(reopened.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(reopened.BeginMaintenanceInitialization(*interrupted_request),
            *interrupted);
  for (std::size_t index = 0U; index < projections.size(); ++index) {
    EXPECT_EQ(projections[index]->rebuild_calls(), rebuild_calls[index]);
  }

  auto restoration_request = *interrupted_request;
  restoration_request.apply_id = ApplyId("maintenance-explicit-restoration");
  const auto restored =
      reopened.BeginMaintenanceInitialization(restoration_request);
  EXPECT_EQ(restored.state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  for (std::size_t index = 0U; index < projections.size(); ++index) {
    EXPECT_EQ(projections[index]->rebuild_calls(), rebuild_calls[index] + 1U);
  }
}

TEST(CanonicalEmptySceneOwner,
     ResolvesPortableFramesAndCarriesThenDetachesAnObjectBeforeEffects) {
  TemporaryDirectory directory("scene-frame-carried-object");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto native_bindings = NativeRuntimeBindings(
      candidate.bundle->value(), "runtime-epoch-frame-resolution");

  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  std::vector<std::unique_ptr<FakePlannerSceneProjection>> projections;
  std::vector<FakePlannerSceneProjection *> projection_clients;
  projections.reserve(native_bindings.size());
  projection_clients.reserve(native_bindings.size());
  for (const auto &binding : native_bindings) {
    projections.push_back(
        std::make_unique<FakePlannerSceneProjection>(binding));
    projection_clients.push_back(projections.back().get());
  }
  auto &projection = *projections.front();
  FakePlannerSceneProjectionResolver projection_resolver(projection_clients);
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, bundle_source,
                                 projection_resolver, cell_admission);
  auto active = FinalizeNativeCandidate(
      owner, publication_reader, admission, *candidate.bundle,
      native_bindings, "apply-frame-resolution-native");
  for (auto &participant : projections) {
    participant->Reset(active, FakePlannerSceneProjection::Mode::kCommit);
  }

  const core::ComponentAttachmentRevision structural_revision(
      "component-attachment-revision-1");
  for (auto &participant : projections) {
    participant->SetFrameResolution(
        core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}},
        {structural_revision});
  }
  auto add = AddBoxRequest(active, admission.cell_id(),
                           "scene-request-component-add");
  std::get<core::AddObjectRequestDTO>(add.operation).frame_ref =
      core::ComponentSceneFrameRefDTO{
          core::ControllableComponentId("component.1"),
          core::ModelInstanceId("model.1"),
          core::FrameRoleId("tool_frame")};
  const auto added = owner.Commit(add);
  ASSERT_TRUE(added.success)
      << (added.error.has_value() ? added.error->message : "missing error");
  ASSERT_TRUE(added.committed_projection_revision.has_value());
  active = *added.committed_projection_revision;
  auto snapshot = owner.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), 1U);
  const auto *fixed = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
      &snapshot->objects.front().placement);
  ASSERT_NE(fixed, nullptr);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.x_m, 1.25);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.y_m, 1.5);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.z_m, 3.75);
  EXPECT_EQ(projection.retained_lease_count(), 0U);

  for (auto &participant : projections) {
    participant->SetResolvedFrameRoleOverride(
        core::FrameRoleId("asset_tool_frame"));
  }

  core::SceneTransactionRequestDTO attach{
      SceneRequestContext("scene-request-attach-resolved", active),
      {},
      active,
      core::AttachCarriedObjectRequestDTO{
          core::CarriedObjectAttachmentId("attachment.1"),
          core::SceneResourceId("scene-box"),
          core::ControllableComponentId("component.1"),
          core::CommandTargetId("arm"),
          core::FrameRoleId("tool_frame"),
          {core::CollisionBodyRoleId("finger")},
          {structural_revision},
      },
      std::nullopt,
  };
  attach.expected_admission_generation = core::AdmissionGeneration(1U);
  attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner.2"),
      core::ProgramRunId("program.2"),
      core::CellAdmissionClaimGeneration(9U),
      "attach",
      "attach:attempt.1",
      "scene.carried_attachment"};
  const auto integrity_before_preview = owner.VerifyIntegrity();
  const auto projection_calls_before_preview = projection.calls();
  const auto preview_policy_calls_before =
      projection.policy_projection_calls();
  const core::SceneFrameResolutionReceiptDTO candidate_frame{
      core::ResolvedComponentModelFrameDTO{
          core::ControllableComponentId("component.1"),
          core::ModelInstanceId("model.1"),
          core::FrameRoleId("asset_tool_frame"),
          "compiled_asset_tool_frame",
          core::ModelRevision("model-revision-1")},
      core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}},
      {structural_revision},
      core::ProjectionLeaseBindingDTO{
          active, core::ProjectionLeaseId("candidate-terminal-frame")},
      Digest('8'),
      Digest('9')};
  const core::ResolvedSceneCandidatePreviewRequestDTO preview_request{
      attach, candidate_frame};
  const auto encoded_preview_request =
      EncodeResolvedSceneCandidatePreviewRequest(preview_request);
  EXPECT_EQ(EncodeResolvedSceneCandidatePreviewRequest(
                DecodeResolvedSceneCandidatePreviewRequest(
                    encoded_preview_request)),
            encoded_preview_request);
  const auto preview = owner.PreviewResolvedSceneCandidate(preview_request);
  ASSERT_TRUE(preview.ok()) << preview.error().message;
  ASSERT_TRUE(preview.value().valid());
  const auto encoded_preview = EncodeSceneCandidatePreview(preview.value());
  EXPECT_EQ(EncodeSceneCandidatePreview(
                DecodeSceneCandidatePreview(encoded_preview)),
            encoded_preview);
  ASSERT_EQ(preview.value().logical_scene->objects.size(), 1U);
  ASSERT_EQ(preview.value().logical_scene->carried_object_attachments.size(),
            1U);
  EXPECT_TRUE(std::holds_alternative<core::CarriedSceneObjectPlacementDTO>(
      preview.value().logical_scene->objects.front().placement));
  EXPECT_EQ(owner.LogicalSnapshot()->carried_object_attachments.size(), 0U);
  EXPECT_EQ(projection.calls(), projection_calls_before_preview);
  EXPECT_EQ(projection.policy_projection_calls(),
            preview_policy_calls_before + 1U);
  const auto integrity_after_preview = owner.VerifyIntegrity();
  EXPECT_EQ(integrity_after_preview.transaction_intent_count,
            integrity_before_preview.transaction_intent_count);
  EXPECT_EQ(integrity_after_preview.request_outcome_count,
            integrity_before_preview.request_outcome_count);
  EXPECT_EQ(integrity_after_preview.projection_attempt_count,
            integrity_before_preview.projection_attempt_count);

  const auto attached = owner.Commit(attach);
  ASSERT_TRUE(attached.success)
      << (attached.error.has_value() ? attached.error->message
                                     : "missing error");
  ASSERT_TRUE(attached.committed_projection_revision.has_value());
  ASSERT_FALSE(cell_admission.Claims().empty());
  EXPECT_EQ(cell_admission.Claims().back().intent.program_node_evidence,
            attach.program_node_evidence);
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
  active = *attached.committed_projection_revision;
  snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(*snapshot, *preview.value().logical_scene);
  ASSERT_FALSE(projection.transactions().empty());
  EXPECT_EQ(projection.transactions().back().resolved_operation,
            preview.value().resolved_operation);
  EXPECT_EQ(projection.transactions().back().original_request_digest,
            preview.value().request_digest);
  ASSERT_EQ(snapshot->carried_object_attachments.size(), 1U);
  EXPECT_EQ(snapshot->carried_object_attachments.front()
                .carrier_frame_ref,
            core::SceneFrameRefDTO(core::ComponentSceneFrameRefDTO{
                core::ControllableComponentId("component.1"),
                core::ModelInstanceId("model.1"),
                core::FrameRoleId("asset_tool_frame")}));

  for (auto &participant : projections) {
    participant->SetFrameResolution(
        core::Pose3DTO{{2.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}},
        {structural_revision});
  }
  const auto carried_pose =
      owner.GetObjectPose(core::SceneResourceId("scene-box"));
  ASSERT_TRUE(carried_pose.ok()) << carried_pose.error().message;
  EXPECT_DOUBLE_EQ(carried_pose.value().position_m.x_m, 2.25);
  EXPECT_DOUBLE_EQ(carried_pose.value().position_m.y_m, 1.5);
  EXPECT_DOUBLE_EQ(carried_pose.value().position_m.z_m, 3.75);
  EXPECT_EQ(projection.retained_lease_count(), 0U);

  auto relative_to_carried = AddBoxRequest(
      active, admission.cell_id(), "scene-request-relative-to-carried");
  auto &relative_operation =
      std::get<core::AddObjectRequestDTO>(relative_to_carried.operation);
  relative_operation.scene_resource_id =
      core::SceneResourceId("scene-relative-carried");
  relative_operation.pose.position_m = {0.5, 0.5, 0.5};
  relative_operation.frame_ref = core::SceneResourceSceneFrameRefDTO{
      core::SceneResourceId("scene-box")};
  const auto relative_result = owner.Commit(relative_to_carried);
  ASSERT_TRUE(relative_result.success)
      << (relative_result.error.has_value() ? relative_result.error->message
                                            : "missing error");
  ASSERT_TRUE(relative_result.committed_projection_revision.has_value());
  active = *relative_result.committed_projection_revision;
  snapshot = owner.LogicalSnapshot();
  const auto relative_object = std::find_if(
      snapshot->objects.begin(), snapshot->objects.end(), [](const auto &entry) {
        return entry.scene_resource_id ==
               core::SceneResourceId("scene-relative-carried");
      });
  ASSERT_NE(relative_object, snapshot->objects.end());
  const auto *relative_fixed =
      std::get_if<core::CellFixedSceneObjectPlacementDTO>(
          &relative_object->placement);
  ASSERT_NE(relative_fixed, nullptr);
  EXPECT_DOUBLE_EQ(relative_fixed->pose_in_cell.position_m.x_m, 2.75);
  EXPECT_DOUBLE_EQ(relative_fixed->pose_in_cell.position_m.y_m, 2.0);
  EXPECT_DOUBLE_EQ(relative_fixed->pose_in_cell.position_m.z_m, 4.25);
  EXPECT_EQ(projection.retained_lease_count(), 0U);

  const auto attachment = snapshot->carried_object_attachments.front();
  core::SceneTransactionRequestDTO detach_current{
      SceneRequestContext("scene-request-detach-current", active),
      {},
      active,
      core::DetachCarriedObjectAtCurrentPoseRequestDTO{
          attachment.carried_object_attachment_id,
          attachment.carried_object_attachment_revision,
          attachment.scene_resource_id,
          attachment.controllable_component_id,
          attachment.command_target_id},
      std::nullopt,
  };
  const auto detached_current = owner.Commit(detach_current);
  ASSERT_TRUE(detached_current.success)
      << (detached_current.error.has_value() ? detached_current.error->message
                                             : "missing error");
  ASSERT_TRUE(detached_current.committed_projection_revision.has_value());
  active = *detached_current.committed_projection_revision;
  snapshot = owner.LogicalSnapshot();
  ASSERT_TRUE(snapshot->carried_object_attachments.empty());
  fixed = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
      &snapshot->objects.front().placement);
  ASSERT_NE(fixed, nullptr);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.x_m, 2.25);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.y_m, 1.5);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.z_m, 3.75);

  auto reattach = attach;
  reattach.request_context =
      SceneRequestContext("scene-request-reattach-resolved", active);
  reattach.expected_projection_revision = active;
  const auto reattached = owner.Commit(reattach);
  ASSERT_TRUE(reattached.success)
      << (reattached.error.has_value() ? reattached.error->message
                                       : "missing error");
  ASSERT_TRUE(reattached.committed_projection_revision.has_value());
  active = *reattached.committed_projection_revision;
  snapshot = owner.LogicalSnapshot();
  ASSERT_EQ(snapshot->carried_object_attachments.size(), 1U);

  core::SceneTransactionRequestDTO detach{
      SceneRequestContext("scene-request-detach-resolved", active),
      {},
      active,
      core::DetachCarriedObjectRequestDTO{
          attachment.carried_object_attachment_id,
          attachment.carried_object_attachment_revision,
          core::Pose3DTO{{0.5, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}},
          core::PassiveModelSceneFrameRefDTO{
              core::ModelInstanceId("fixture.1"),
              core::FrameRoleId("origin")},
      },
      std::nullopt,
  };
  const auto detached = owner.Commit(detach);
  ASSERT_TRUE(detached.success)
      << (detached.error.has_value() ? detached.error->message
                                     : "missing error");
  ASSERT_TRUE(detached.committed_projection_revision.has_value());
  active = *detached.committed_projection_revision;
  snapshot = owner.LogicalSnapshot();
  ASSERT_TRUE(snapshot->carried_object_attachments.empty());
  fixed = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
      &snapshot->objects.front().placement);
  ASSERT_NE(fixed, nullptr);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.x_m, 2.5);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.y_m, 2.0);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.z_m, 3.0);

  auto stale_attach = attach;
  stale_attach.request_context =
      SceneRequestContext("scene-request-stale-attach", active);
  stale_attach.expected_projection_revision = active;
  auto &stale_operation =
      std::get<core::AttachCarriedObjectRequestDTO>(stale_attach.operation);
  stale_operation.expected_component_attachment_revisions = {
      core::ComponentAttachmentRevision("stale-revision")};
  const auto stale = owner.Commit(stale_attach);
  ASSERT_FALSE(stale.success);
  ASSERT_TRUE(stale.error.has_value());
  EXPECT_EQ(stale.error->code, "stale_revision");
  EXPECT_EQ(stale.error->details.at("reason"),
            "carrier_attachment_revision_mismatch");

  for (auto &participant : projections) {
    participant->SetResolvedModelInstanceOverride(
        core::ModelInstanceId("model.foreign"));
  }
  auto cross_instance = AddBoxRequest(
      active, admission.cell_id(), "scene-request-cross-instance-frame");
  std::get<core::AddObjectRequestDTO>(cross_instance.operation).frame_ref =
      core::ComponentSceneFrameRefDTO{
          core::ControllableComponentId("component.1"),
          core::ModelInstanceId("model.1"),
          core::FrameRoleId("tool_frame")};
  const auto crossed = owner.Commit(cross_instance);
  ASSERT_FALSE(crossed.success);
  ASSERT_TRUE(crossed.error.has_value());
  EXPECT_EQ(crossed.error->code, "missing_reference");
  EXPECT_EQ(crossed.error->details.at("reason"),
            "scene_frame_resolution_missing_or_foreign");

  for (auto &participant : projections) {
    participant->SetResolvedModelInstanceOverride(std::nullopt);
    participant->RejectFrameResolution(core::ErrorDTO{
        "missing_reference", "test retained frame is ambiguous",
        {{"reason", "scene_frame_reference_ambiguous"}}});
  }
  auto ambiguous = AddBoxRequest(
      active, admission.cell_id(), "scene-request-ambiguous-frame");
  std::get<core::AddObjectRequestDTO>(ambiguous.operation).frame_ref =
      core::PassiveModelSceneFrameRefDTO{
          core::ModelInstanceId("fixture.1"),
          core::FrameRoleId("origin")};
  const auto ambiguity = owner.Commit(ambiguous);
  ASSERT_FALSE(ambiguity.success);
  ASSERT_TRUE(ambiguity.error.has_value());
  EXPECT_EQ(ambiguity.error->code, "missing_reference");
  EXPECT_EQ(ambiguity.error->details.at("reason"),
            "scene_frame_reference_ambiguous");

  for (std::size_t index = 0U; index < projections.size(); ++index) {
    EXPECT_EQ(projections[index]->calls(), 6U);
    EXPECT_EQ(projections[index]->frame_resolution_calls(),
              index == 0U ? 10U : 9U);
    EXPECT_EQ(projections[index]->policy_projection_calls(), 7U);
    EXPECT_EQ(projections[index]->retained_lease_count(), 0U);
  }
  const auto integrity = owner.VerifyIntegrity();
  EXPECT_EQ(integrity.transaction_intent_count, 6);
  EXPECT_EQ(integrity.request_outcome_count, 6);
  EXPECT_EQ(integrity.projection_attempt_count,
            static_cast<std::int64_t>(6U * projections.size()));
}

TEST(CanonicalEmptySceneOwner,
     PolicyProjectionCarriesConfiguredAndEffectiveValuesSeparately) {
  TemporaryDirectory directory("scene-policy-resolution");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto native_bindings = NativeRuntimeBindings(
      candidate.bundle->value(), "runtime-epoch-policy-resolution");
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  auto projections = MakeFakeProjections(native_bindings);
  FakePlannerSceneProjectionResolver projection_resolver(
      ProjectionClients(projections));
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, bundle_source,
                                 projection_resolver, cell_admission);
  const auto active = FinalizeNativeCandidate(
      owner, publication_reader, admission, *candidate.bundle,
      native_bindings, "apply-policy-resolution-native");
  ResetProjections(projections, active,
                   FakePlannerSceneProjection::Mode::kCommit);

  auto configured = owner.LogicalSnapshot()->configured_collision_policy;
  configured.policy_id = "strict-diagnostics";
  core::SceneTransactionRequestDTO request{
      SceneRequestContext("scene-request-policy-resolution", active),
      {},
      active,
      core::SetCollisionPolicyRequestDTO{
          configured,
          owner.LogicalSnapshot()->configured_collision_policy.revision,
      },
      core::AdmissionGeneration(1U),
  };
  const auto result = owner.Commit(request);
  ASSERT_TRUE(result.success);
  for (const auto &projection : projections) {
    ASSERT_EQ(projection->transactions().size(), 1U);
    const auto *resolved = std::get_if<core::ResolvedCollisionPolicyDTO>(
        &projection->transactions().front().resolved_operation);
    ASSERT_NE(resolved, nullptr);
    EXPECT_EQ(resolved->configured_policy.policy_id, "strict-diagnostics");
    EXPECT_EQ(resolved->effective_policy.policy_id, "strict-diagnostics");
    EXPECT_EQ(resolved->configured_policy.revision.content_hash,
              owner.LogicalSnapshot()
                  ->configured_collision_policy.revision.content_hash);
    EXPECT_EQ(resolved->effective_policy.revision.content_hash,
              resolved->configured_policy.revision.content_hash);
  }
}

TEST(CanonicalEmptySceneOwner,
     SceneUpdatesClassifyNoOpFreshnessGeometryAndPolicyEpochs) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const auto added = owner.Commit(
      AddBoxRequest(accepted.canonical_empty_target, admission.cell_id()));
  ASSERT_TRUE(added.success);
  auto projection = *added.committed_projection_revision;
  auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box"));
  ASSERT_TRUE(pose.ok());

  const auto duplicate = owner.Commit(AddBoxRequest(
      projection, admission.cell_id(), "scene-request-duplicate"));
  ASSERT_FALSE(duplicate.success);
  ASSERT_TRUE(duplicate.error.has_value());
  EXPECT_EQ(duplicate.error->code, "object_already_exists");
  EXPECT_EQ(duplicate.error->details.at("scene_resource_id"), "scene-box");

  core::SceneTransactionRequestDTO missing_remove{
      SceneRequestContext("scene-request-missing-remove", projection),
      {},
      projection,
      core::RemoveObjectRequestDTO{core::SceneResourceId("missing-object")},
      core::AdmissionGeneration(1U),
  };
  const auto missing_result = owner.Commit(missing_remove);
  ASSERT_FALSE(missing_result.success);
  ASSERT_TRUE(missing_result.error.has_value());
  EXPECT_EQ(missing_result.error->code, "object_not_found");
  EXPECT_EQ(missing_result.error->details.at("scene_resource_id"),
            "missing-object");

  core::SceneTransactionRequestDTO no_op{
      SceneRequestContext("scene-request-no-op", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          pose.value(),
          core::CellSceneFrameRefDTO{admission.cell_id()},
          std::nullopt,
          false,
      },
      core::AdmissionGeneration(1U),
  };
  const auto unchanged = owner.Commit(no_op);
  ASSERT_TRUE(unchanged.success);
  EXPECT_EQ(unchanged.update_status, core::SceneUpdateStatusDTO::kNoOp);
  EXPECT_EQ(unchanged.committed_projection_revision, projection);
  auto snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 2U);
  EXPECT_EQ(snapshot->topology_epoch, 1U);
  EXPECT_EQ(snapshot->policy_epoch, 0U);

  core::SceneObjectProviderEvidenceDTO evidence{
      core::ProviderInstanceId("provider-instance"),
      core::ProviderManifestId("provider-manifest"),
      core::ProviderManifestRevision(Digest('7').value()),
      "provider-epoch",
      1U,
      1000,
      900,
      5000,
  };
  core::SceneTransactionRequestDTO freshness{
      SceneRequestContext("scene-request-freshness", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          pose.value(),
          core::CellSceneFrameRefDTO{admission.cell_id()},
          evidence,
          false,
      },
      std::nullopt,
  };
  const auto refreshed = owner.Commit(freshness);
  ASSERT_TRUE(refreshed.success)
      << (refreshed.error.has_value()
              ? refreshed.error->code + ": " + refreshed.error->message
              : "missing error");
  EXPECT_EQ(refreshed.update_status,
            core::SceneUpdateStatusDTO::kFreshnessOnly);
  projection = *refreshed.committed_projection_revision;
  EXPECT_EQ(projection.scene_revision.revision_id, 3U);
  snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 3U);
  EXPECT_EQ(snapshot->topology_epoch, 1U);
  EXPECT_EQ(snapshot->policy_epoch, 0U);

  auto moved_pose = pose.value();
  moved_pose.position_m.y_m += 0.125;
  auto stale_evidence = evidence;
  core::SceneTransactionRequestDTO stale_provider{
      SceneRequestContext("scene-request-stale-provider", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          moved_pose,
          core::CellSceneFrameRefDTO{admission.cell_id()},
          stale_evidence,
          false,
      },
      std::nullopt,
  };
  const auto stale_provider_result = owner.Commit(stale_provider);
  ASSERT_FALSE(stale_provider_result.success);
  ASSERT_TRUE(stale_provider_result.error.has_value());
  EXPECT_EQ(stale_provider_result.error->code, "scene_object_state_conflict");
  EXPECT_EQ(stale_provider_result.error->details.at("reason"),
            "provider_source_sequence_not_increasing");
  EXPECT_EQ(stale_provider_result.error->details.at(
                "previous_provider_source_sequence"),
            "1");
  EXPECT_EQ(stale_provider_result.error->details.at(
                "received_provider_source_sequence"),
            "1");

  auto next_evidence = evidence;
  next_evidence.provider_source_sequence = 2U;
  next_evidence.received_at_ns = 1100;
  core::SceneTransactionRequestDTO geometry{
      SceneRequestContext("scene-request-geometry", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          moved_pose,
          core::CellSceneFrameRefDTO{admission.cell_id()},
          next_evidence,
          false,
      },
      std::nullopt,
  };
  const auto moved = owner.Commit(geometry);
  ASSERT_TRUE(moved.success);
  EXPECT_EQ(moved.update_status, core::SceneUpdateStatusDTO::kGeometryChanged);
  projection = *moved.committed_projection_revision;
  EXPECT_EQ(projection.scene_revision.revision_id, 4U);
  snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 4U);
  EXPECT_EQ(snapshot->topology_epoch, 2U);
  EXPECT_EQ(snapshot->policy_epoch, 0U);

  auto policy = snapshot->configured_collision_policy;
  policy.policy_id = "strict-diagnostics";
  core::SceneTransactionRequestDTO policy_update{
      SceneRequestContext("scene-request-policy", projection),
      {},
      projection,
      core::SetCollisionPolicyRequestDTO{
          policy,
          snapshot->configured_collision_policy.revision,
      },
      core::AdmissionGeneration(1U),
  };
  const auto policy_result = owner.Commit(policy_update);
  ASSERT_TRUE(policy_result.success);
  projection = *policy_result.committed_projection_revision;
  EXPECT_EQ(projection.scene_revision.revision_id, 5U);
  snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 5U);
  EXPECT_EQ(snapshot->topology_epoch, 2U);
  EXPECT_EQ(snapshot->policy_epoch, 1U);
  EXPECT_EQ(snapshot->configured_collision_policy.policy_id,
            "strict-diagnostics");

  auto stale = geometry;
  stale.request_context.request_id = core::RequestId("scene-request-stale");
  const auto rejected = owner.Commit(stale);
  EXPECT_FALSE(rejected.success);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code, "stale_revision");
  EXPECT_EQ(rejected.projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 5);
}

TEST(CanonicalEmptySceneOwner, LongServiceProviderEvidenceSurvivesRealJournalRestart) {
  TemporaryDirectory directory;
  const auto database = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  constexpr std::int64_t years31 = 31LL * 366 * 86400 * 1000000000;
  core::SceneObjectProviderEvidenceDTO evidence{
      core::ProviderInstanceId("provider-instance"),
      core::ProviderManifestId("provider-manifest"),
      core::ProviderManifestRevision(Digest('7').value()),
      "provider-epoch", 1U, years31 + 1, years31 - 1, 5000,
  };
  {
    CanonicalEmptySceneOwner owner(database, admission, publication_reader, cell_admission);
    const auto bootstrap = FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    const auto added = owner.Commit(AddBoxRequest(bootstrap.canonical_empty_target, admission.cell_id()));
    ASSERT_TRUE(added.success);
    const auto projection = *added.committed_projection_revision;
    const auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box"));
    ASSERT_TRUE(pose.ok());
    core::SceneTransactionRequestDTO request{
        SceneRequestContext("scene-request-long-clock", projection), {}, projection,
        core::UpdateObjectPoseRequestDTO{core::SceneResourceId("scene-box"), pose.value(),
            core::CellSceneFrameRefDTO{admission.cell_id()}, evidence, false}, std::nullopt,
    };
    const auto committed = owner.Commit(request);
    ASSERT_TRUE(committed.success) << (committed.error ? committed.error->message : "missing error");
    EXPECT_EQ(owner.GetObjectState(core::SceneResourceId("scene-box")).provider_evidence, evidence);
    EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 2);
  }
  CanonicalEmptySceneOwner replacement(database, admission, publication_reader, cell_admission);
  EXPECT_EQ(replacement.GetObjectState(core::SceneResourceId("scene-box")).provider_evidence, evidence);
  EXPECT_EQ(replacement.VerifyIntegrity().transaction_intent_count, 2);
}

TEST(CanonicalEmptySceneOwner,
     ProjectionCommitRollbackAndUncertaintyUseClosedDurableOutcomes) {
  TemporaryDirectory packages("scene-native-projection-packages");
  const auto package_root = packages.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto native_bindings = NativeRuntimeBindings(
      candidate.bundle->value(), "runtime-epoch-scene-projection");
  const auto two_robot_candidate = deployment::test::
      materialize_two_lite6_same_type_native_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto two_robot_native_bindings = NativeRuntimeBindings(
      two_robot_candidate.bundle->value(),
      "runtime-epoch-scene-projection-partial");
  ASSERT_EQ(two_robot_native_bindings.size(), 2U * native_bindings.size());
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);

  {
    TemporaryDirectory directory("scene-projection-commit");
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                   admission, publication_reader, bundle_source,
                                   projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *candidate.bundle,
        native_bindings, "apply-native-commit");
    ResetProjections(projections, active,
                     FakePlannerSceneProjection::Mode::kCommit);
    const auto request = AddBoxRequest(active, admission.cell_id(),
                                       "scene-request-native-commit");
    const auto result = owner.Commit(request);
    ASSERT_TRUE(result.success);
    for (const auto &projection : projections) {
      EXPECT_EQ(projection->calls(), 1U);
    }
    EXPECT_EQ(owner.VerifyIntegrity().projection_attempt_count,
              static_cast<std::int64_t>(projections.size()));
    EXPECT_EQ(owner.VerifyIntegrity().projection_result_count,
              static_cast<std::int64_t>(projections.size()));
    const auto replay = owner.Commit(request);
    EXPECT_TRUE(replay.success);
    for (const auto &projection : projections) {
      EXPECT_EQ(projection->calls(), 1U);
    }
  }

  {
    TemporaryDirectory directory("scene-projection-rollback");
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                   admission, publication_reader, bundle_source,
                                   projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *candidate.bundle,
        native_bindings, "apply-native-rollback");
    ResetProjections(projections, active,
                     FakePlannerSceneProjection::Mode::kRollbackProven);
    const auto request = AddBoxRequest(active, admission.cell_id(),
                                       "scene-request-native-rollback");
    const auto result = owner.Commit(request);
    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.projection_status,
              core::SceneProjectionCommitStatusDTO::kRejectedRollbackProven);
    EXPECT_TRUE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_TRUE(owner.GetSceneObjects().empty());
    const auto replay = owner.Commit(request);
    EXPECT_FALSE(replay.success);
    EXPECT_EQ(replay.replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);
    for (std::size_t index = 0U; index < projections.size(); ++index) {
      EXPECT_EQ(projections[index]->calls(), index == 0U ? 1U : 0U);
    }
  }

  {
    TemporaryDirectory directory("scene-projection-uncertain");
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                   admission, publication_reader, bundle_source,
                                   projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *candidate.bundle,
        native_bindings, "apply-native-uncertain");
    ResetProjections(projections, active,
                     FakePlannerSceneProjection::Mode::kMalformed);
    const auto result = owner.Commit(
        AddBoxRequest(active, admission.cell_id(),
                      "scene-request-native-uncertain"));
    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.projection_status,
              core::SceneProjectionCommitStatusDTO::kFailedProjectionUncertain);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
    EXPECT_TRUE(owner.VerifyIntegrity().projection_desynchronized);
    EXPECT_THROW(owner.LogicalSnapshot(),
                 CanonicalEmptySceneOwnerConflictError);
  }

  {
    TemporaryDirectory directory("scene-projection-partial");
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(two_robot_native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    CanonicalEmptySceneOwner owner(
        directory.path() / "logical-scene.sqlite3", admission,
        publication_reader, bundle_source, projection_resolver,
        cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *two_robot_candidate.bundle,
        two_robot_native_bindings, "apply-native-partial");
    ResetProjections(projections, active,
                     FakePlannerSceneProjection::Mode::kRollbackProven);
    projections.front()->SetMode(FakePlannerSceneProjection::Mode::kCommit);
    const auto result = owner.Commit(
        AddBoxRequest(active, admission.cell_id(),
                      "scene-request-native-partial"));
    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.projection_status,
              core::SceneProjectionCommitStatusDTO::kFailedProjectionUncertain);
    for (std::size_t index = 0U; index < projections.size(); ++index) {
      EXPECT_EQ(projections[index]->calls(), index < 2U ? 1U : 0U);
    }
    const auto integrity = owner.VerifyIntegrity();
    EXPECT_EQ(integrity.projection_attempt_count, 2);
    EXPECT_EQ(integrity.projection_result_count, 2);
    EXPECT_TRUE(integrity.projection_desynchronized);
  }

  {
    TemporaryDirectory directory("scene-frame-multi-projection-errors");
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(two_robot_native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    CanonicalEmptySceneOwner owner(
        directory.path() / "logical-scene.sqlite3", admission,
        publication_reader, bundle_source, projection_resolver,
        cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *two_robot_candidate.bundle,
        two_robot_native_bindings, "apply-frame-multi-projection-errors");
    ResetProjections(projections, active,
                     FakePlannerSceneProjection::Mode::kCommit);

    auto request = AddBoxRequest(active, admission.cell_id(),
                                 "scene-request-stale-frame-owner");
    std::get<core::AddObjectRequestDTO>(request.operation).frame_ref =
        core::ComponentSceneFrameRefDTO{
            two_robot_candidate.bundle->value()
                .cell_graph.control_bindings.front()
                .controllable_component_id,
            core::ModelInstanceId("model.owner"),
            core::FrameRoleId("tool_frame")};
    projections.front()->RejectFrameResolution(core::ErrorDTO{
        "stale_revision", "test owner state is stale",
        {{"reason", "kinematic_state_view_stale"}}});
    const auto stale = owner.Commit(request);
    ASSERT_FALSE(stale.success);
    ASSERT_TRUE(stale.error.has_value());
    EXPECT_EQ(stale.error->code, "stale_revision");
    EXPECT_EQ(stale.error->details.at("reason"),
              "kinematic_state_view_stale");
    for (const auto &projection : projections) {
      EXPECT_EQ(projection->calls(), 0U);
      EXPECT_EQ(projection->retained_lease_count(), 0U);
    }
    EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 0);

    ResetProjections(projections, active,
                     FakePlannerSceneProjection::Mode::kCommit);
    projections.front()->RejectFrameResolution(core::ErrorDTO{
        "missing_reference", "test projection does not own this frame",
        {{"reason", "scene_frame_reference_missing"}}});
    request.request_context = SceneRequestContext(
        "scene-request-one-frame-owner", active);
    const auto resolved_by_owner = owner.Commit(request);
    ASSERT_TRUE(resolved_by_owner.success)
        << (resolved_by_owner.error.has_value()
                ? resolved_by_owner.error->message
                : "missing error");
    for (const auto &projection : projections) {
      EXPECT_EQ(projection->calls(), 1U);
      EXPECT_EQ(projection->retained_lease_count(), 0U);
    }
  }
}

TEST(CanonicalEmptySceneOwner,
     RestartSettlesZeroNativeResidueAndFencesMissingNativeResult) {
  const auto admission = LoadAdmission();

  {
    TemporaryDirectory directory("scene-zero-native-recovery");
    const auto database_path = directory.path() / "logical-scene.sqlite3";
    ControlledPublicationReader publication_reader;
    TestCellAdmissionPort cell_admission;
    std::optional<core::SceneTransactionRequestDTO> request;
    ProjectionRevisionDTO committed_projection;
    {
      CanonicalEmptySceneOwner owner(database_path, admission,
                                     publication_reader, cell_admission);
      const auto accepted =
          FinalizeCanonicalBootstrap(owner, publication_reader, admission);
      request =
          AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                        "scene-request-zero-recovery");
      const auto committed = owner.Commit(*request);
      ASSERT_TRUE(committed.success);
      committed_projection = *committed.committed_projection_revision;
      owner.Close();
    }
    ExecuteSql(database_path,
               "DROP TRIGGER logical_scene_request_outcomes_no_delete;"
               "DELETE FROM logical_scene_request_outcomes;"
               "CREATE TRIGGER IF NOT EXISTS "
               "logical_scene_request_outcomes_no_delete BEFORE DELETE ON "
               "logical_scene_request_outcomes WHEN (SELECT "
               "compaction_in_progress FROM logical_scene_history_state "
               "WHERE singleton = 1) != 1 BEGIN SELECT RAISE(ABORT, "
               "'logical_scene_request_outcomes is immutable'); END;");

    CanonicalEmptySceneOwner recovered(database_path, admission,
                                       publication_reader);
    EXPECT_EQ(recovered.CurrentProjectionRevision(), committed_projection);
    ASSERT_TRUE(request.has_value());
    const auto replay = recovered.Commit(*request);
    ASSERT_TRUE(replay.success);
    EXPECT_EQ(replay.replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(recovered.VerifyIntegrity().request_outcome_count, 1);
  }

  {
    TemporaryDirectory packages("scene-native-recovery-packages");
    const auto package_root = packages.path() / "packages";
    CopyCanonicalEmptyPackage(package_root);
    const auto native_admission = LoadAdmission(package_root);
    const auto candidate = deployment::test::
        materialize_lite6_native_motion_retained_consumer_fixture_for_cell(
            package_root, native_admission.cell_id().value());
    const auto native_bindings = NativeRuntimeBindings(
        candidate.bundle->value(), "runtime-epoch-scene-recovery");
    TemporaryDirectory directory("scene-native-recovery");
    const auto database_path = directory.path() / "logical-scene.sqlite3";
    ControlledPublicationReader publication_reader;
    deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
    auto projections = MakeFakeProjections(native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    {
      CanonicalEmptySceneOwner owner(database_path, native_admission,
                                     publication_reader, bundle_source,
                                     projection_resolver, cell_admission);
      const auto active = FinalizeNativeCandidate(
          owner, publication_reader, native_admission, *candidate.bundle,
          native_bindings, "apply-native-recovery");
      ResetProjections(projections, active,
                       FakePlannerSceneProjection::Mode::kCommit);
      const auto committed = owner.Commit(
          AddBoxRequest(active, native_admission.cell_id(),
                        "scene-request-native-recovery"));
      ASSERT_TRUE(committed.success);
      owner.Close();
    }
    ExecuteSql(database_path,
               "DROP TRIGGER logical_scene_request_outcomes_no_delete;"
               "DROP TRIGGER logical_scene_projection_results_no_delete;"
               "DELETE FROM logical_scene_request_outcomes;"
               "DELETE FROM logical_scene_projection_results;"
               "CREATE TRIGGER IF NOT EXISTS "
               "logical_scene_request_outcomes_no_delete BEFORE DELETE ON "
               "logical_scene_request_outcomes WHEN (SELECT "
               "compaction_in_progress FROM logical_scene_history_state "
               "WHERE singleton = 1) != 1 BEGIN SELECT RAISE(ABORT, "
               "'logical_scene_request_outcomes is immutable'); END;"
               "CREATE TRIGGER IF NOT EXISTS "
               "logical_scene_projection_results_no_delete BEFORE DELETE ON "
               "logical_scene_projection_results WHEN (SELECT "
               "compaction_in_progress FROM logical_scene_history_state "
               "WHERE singleton = 1) != 1 BEGIN SELECT RAISE(ABORT, "
               "'logical_scene_projection_results is immutable'); END;");

    CanonicalEmptySceneOwner recovered(database_path, native_admission,
                                       publication_reader, bundle_source,
                                       projection_resolver);
    const auto integrity = recovered.VerifyIntegrity();
    EXPECT_TRUE(integrity.projection_desynchronized);
    EXPECT_EQ(integrity.request_outcome_count, 1);
    EXPECT_EQ(integrity.projection_attempt_count,
              static_cast<std::int64_t>(projections.size()));
    EXPECT_EQ(integrity.projection_result_count, 0);
    EXPECT_FALSE(recovered.ReadAccessStatus().scene_writes_open);
    EXPECT_THROW(recovered.LogicalSnapshot(),
                 CanonicalEmptySceneOwnerConflictError);
  }
}


} // namespace
} // namespace manyforge::scene
