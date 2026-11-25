#include "../../manyforge_headless/src/scene_admission_reconciliation_internal.hpp"
#include <gtest/gtest.h>
#include <manyforge_scene/scene_candidate_preview.hpp>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <initializer_list>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/kernel_cell_admission_owner.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/logical_scene_projection.hpp>
#include <manyforge_core/ports/ioperation_journal.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include "lite6_retained_consumer_fixture.hpp"
#include "logical_scene_codec_internal.hpp"
#include "logical_scene_owner_internal.hpp"
#include "manyforge_scene/logical_scene_owner.hpp"
#include "manyforge_scene/logical_scene_value_codec.hpp"
#include "manyforge_scene/scene_transaction_codec.hpp"

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
using core::ProjectionExecutionHostBindingDTO;
using core::ProjectionRevisionDTO;
using core::Result;

class SceneAdmissionClock final : public core::IClock {
public:
  core::TimePoint now() const override { return 42; }
};

class SceneAdmissionJournal final : public core::IOperationJournal {
public:
  Result<void> BeginStart(core::ApplicationStartId, std::int64_t,
                          std::size_t) override {
    return Result<void>::Ok();
  }
  // Claim records only; the cell-admission owner posts them through
  // `Post` on the calling thread.
  core::JournalBatchResult WriteBatch(std::span<const core::JournalRecord> records,
                                      std::uint64_t) override {
    for (const auto &record : records) {
      if (const auto *acquired =
              std::get_if<core::JournalCellClaimAcquired>(&record.payload)) {
        const auto sequence = ++sequence_;
        if (watched_acquire_intent_.has_value() &&
            acquired->claim.intent == *watched_acquire_intent_) {
          observed_acquired_claim_ = acquired->claim;
          observed_acquire_sequence_ = sequence;
        }
      } else if (const auto *released =
                     std::get_if<core::JournalCellClaimReleased>(
                         &record.payload)) {
        const auto sequence = ++sequence_;
        if (watched_release_claim_.has_value() &&
            released->claim == *watched_release_claim_) {
          observed_released_claim_ = released->claim;
          observed_release_ = released->release;
          observed_release_sequence_ = sequence;
        }
      }
    }
    return {};
  }
  void Post(const std::span<core::JournalRecord> records) {
    static_cast<void>(WriteBatch(records, 0U));
  }

  // Retain only the two targeted callbacks needed for claim-order assertions.
  void ObserveClaimReleaseThenAcquire(
      const core::CellAdmissionClaimDTO &released_claim,
      const core::CellAdmissionClaimIntentDTO &acquired_intent) {
    watched_release_claim_ = released_claim;
    watched_acquire_intent_ = acquired_intent;
    observed_released_claim_.reset();
    observed_release_.reset();
    observed_acquired_claim_.reset();
    observed_release_sequence_.reset();
    observed_acquire_sequence_.reset();
  }

  [[nodiscard]] const std::optional<core::CellAdmissionClaimDTO> &
  observed_released_claim() const noexcept {
    return observed_released_claim_;
  }

  [[nodiscard]] const std::optional<core::CellAdmissionClaimReleaseDTO> &
  observed_release() const noexcept {
    return observed_release_;
  }

  [[nodiscard]] const std::optional<core::CellAdmissionClaimDTO> &
  observed_acquired_claim() const noexcept {
    return observed_acquired_claim_;
  }

  [[nodiscard]] const std::optional<std::uint64_t> &
  observed_release_sequence() const noexcept {
    return observed_release_sequence_;
  }

  [[nodiscard]] const std::optional<std::uint64_t> &
  observed_acquire_sequence() const noexcept {
    return observed_acquire_sequence_;
  }

private:
  std::uint64_t sequence_{};
  std::optional<core::CellAdmissionClaimDTO> watched_release_claim_;
  std::optional<core::CellAdmissionClaimIntentDTO> watched_acquire_intent_;
  std::optional<core::CellAdmissionClaimDTO> observed_released_claim_;
  std::optional<core::CellAdmissionClaimReleaseDTO> observed_release_;
  std::optional<core::CellAdmissionClaimDTO> observed_acquired_claim_;
  std::optional<std::uint64_t> observed_release_sequence_;
  std::optional<std::uint64_t> observed_acquire_sequence_;
};

// The scene owner's view of the Kernel's cell-admission owner; the next
// release can be made unavailable (the request never reaches the owner, so
// its claim stays held).
class ReleaseUnavailableAdmission final : public core::IKernelCellAdmissionPort {
public:
  explicit ReleaseUnavailableAdmission(core::IKernelCellAdmissionPort &owner)
      : owner_(owner) {}

  void FailNextRelease() { fail_next_release_ = true; }

  [[nodiscard]] core::AuthoringAdmissionDTO
  GetAuthoringAdmission() const override {
    return owner_.GetAuthoringAdmission();
  }
  [[nodiscard]] Result<core::AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &snapshot,
      std::shared_ptr<core::IIoSignalService> *actual_retired_service =
          nullptr) override {
    return owner_.UpdateCellAdmissionReadiness(snapshot,
                                               actual_retired_service);
  }
  [[nodiscard]] Result<core::CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    return owner_.AcquireCellAdmissionClaim(intent);
  }
  [[nodiscard]] Result<core::AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO &release) override {
    if (fail_next_release_) {
      fail_next_release_ = false;
      return Result<core::AuthoringAdmissionDTO>::Err(
          {"kernel_owner_unavailable", "injected release unavailable", {}});
    }
    return owner_.ReleaseCellAdmissionClaim(release);
  }
  [[nodiscard]] Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &query) const override {
    return owner_.GetCellAdmissionRecoveryReceipt(query);
  }
  [[nodiscard]] Result<void> ValidateProgramResetSceneAuthorization(
      const core::ProgramResetSceneAuthorizationDTO &authorization)
      const override {
    return owner_.ValidateProgramResetSceneAuthorization(authorization);
  }

private:
  core::IKernelCellAdmissionPort &owner_;
  bool fail_next_release_{};
};

constexpr std::string_view kExpectedSceneId =
    "sha256:8a7c7a8f2599db9875c56ead466924113cc28bc9f9c8f5ca18e2505a6c41af06";
constexpr std::string_view kExpectedEmptySceneContentHash =
    "sha256:b87f99e893eee703b197945f7d2b50bee8586ba49fccba8e27bca580b234dc52";
constexpr std::int64_t kSustainedNoOpCommitP95BudgetNs = 50'000'000;

static_assert(
    !std::is_constructible_v<LogicalSceneOwner,
                             ProjectionBootstrapPrerequisiteQueryDTO,
                             const IActiveBundlePublicationReader &>);
static_assert(!std::is_constructible_v<
              LogicalSceneOwner, std::filesystem::path,
              const deployment::CanonicalEmptyRetainedBundleAdmission &,
              const IActiveBundlePublicationReader &>);
static_assert(std::is_constructible_v<
              LogicalSceneOwner,
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

// One distinct request key per test request name (FNV-1a of the name):
// equal names give equal keys, distinct names distinct keys.
core::RequestKey SceneRequestKey(const std::string_view name) {
  std::uint64_t sequence = 0xcbf29ce484222325ULL;
  for (const char character : name) {
    sequence = (sequence ^ static_cast<unsigned char>(character)) *
               0x100000001b3ULL;
  }
  return core::RequestKey::Make(
      core::RequestKeyClass::kInternal, 0U,
      static_cast<std::uint64_t>(core::RequestKeyMinter::kManager), sequence);
}

// The largest counter the canonical JSON carries (2^53 - 1).
constexpr std::uint64_t kMaximumSafeCounter = 9'007'199'254'740'991ULL;

class TestCellAdmissionPort final : public core::IKernelCellAdmissionPort {
public:
  enum class LostReply { kNone, kAcquire, kRelease };
  void LoseNextReply(LostReply reply) { lost_reply_ = reply; }
  void RejectNextClaim(ErrorDTO error) { next_rejection_ = std::move(error); }
  void RunBeforeNextAcquire(std::function<void()> action) {
    before_next_acquire_ = std::move(action);
  }
  // Opt-in: an intent already settled here is admitted again as new, as by a
  // Kernel that no longer holds that settlement.
  void AdmitSettledIntentsAgain() {
    std::lock_guard lock(mutex_);
    admit_settled_intents_again_ = true;
  }

  void RefuseApplyClaimLookupAfter(std::size_t successful_reads) {
    std::lock_guard lock(mutex_);
    remaining_apply_claim_reads_ = successful_reads;
  }

  void RejectProgramResetSceneAuthorization(bool reject) {
    std::lock_guard lock(mutex_);
    reject_program_reset_scene_authorization_ = reject;
  }

  // Calls received from the scene owner (OD-376 counts them per Commit).
  [[nodiscard]] std::size_t acquire_calls() const {
    std::lock_guard lock(mutex_);
    return acquire_calls_;
  }
  [[nodiscard]] std::size_t release_calls() const {
    std::lock_guard lock(mutex_);
    return release_calls_;
  }
  [[nodiscard]] std::size_t recovery_receipt_calls() const {
    std::lock_guard lock(mutex_);
    return recovery_receipt_calls_;
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
        claim.intent.owner_transaction_id,
        claim.intent.request_key,
        claim.acquired_admission_generation,
        claim.claim_generation,
        {core::CellAdmissionSettlementKindDTO::kProvenNoEffect, Digest('0')},
    };
    held_index_.reset();
  }

  [[nodiscard]] core::Result<core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &,
      std::shared_ptr<core::IIoSignalService>* actual_retired_service = nullptr) override {
    if (actual_retired_service) actual_retired_service->reset();
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }

  [[nodiscard]] core::Result<core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    if (auto action = std::exchange(before_next_acquire_, nullptr)) {
      action();
    }
    std::lock_guard lock(mutex_);
    ++acquire_calls_;
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
      if (entries_[index].claim.intent != intent ||
          (admit_settled_intents_again_ &&
           entries_[index].release.has_value())) {
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
    ++release_calls_;
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
    // As the Kernel: the release names its claim by the claim's identity;
    // its request_key is the causing request's (contract 1.15.2).
    if (release.owner_role != claim.intent.owner_role ||
        release.owner_transaction_id != claim.intent.owner_transaction_id ||
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

  [[nodiscard]] core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &query) const override {
    std::lock_guard lock(mutex_);
    ++recovery_receipt_calls_;
    if (query.intent.claim_kind == core::CellAdmissionClaimKindDTO::kManagerApply &&
        remaining_apply_claim_reads_.has_value()) {
      if (*remaining_apply_claim_reads_ == 0U) {
        return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Err(
            {"kernel_owner_unavailable", "test Apply claim lookup unavailable", {}});
      }
      --*remaining_apply_claim_reads_;
    }
    core::CellAdmissionRecoveryReceiptDTO receipt{
        core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
        core::AdmissionGeneration(1U),
        std::nullopt,
        std::nullopt,
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
  bool admit_settled_intents_again_{};
  LostReply lost_reply_{LostReply::kNone};
  std::optional<ErrorDTO> next_rejection_;
  std::function<void()> before_next_acquire_;
  mutable std::optional<std::size_t> remaining_apply_claim_reads_;
  std::size_t acquire_calls_{};
  std::size_t release_calls_{};
  mutable std::size_t recovery_receipt_calls_{};
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

void Require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

ProjectionBootstrapAcceptedDTO FinalizeCanonicalBootstrap(
    LogicalSceneOwner &owner,
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
  EXPECT_TRUE(snapshot.configured_collision_policy.static_body_ids.empty());
  EXPECT_TRUE(
      snapshot.configured_collision_policy.allowed_touch_link_pairs.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.contact_rules.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.require_full_diagnostics);
  EXPECT_TRUE(snapshot.objects.empty());
  EXPECT_TRUE(snapshot.carried_object_attachments.empty());
  EXPECT_EQ(
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot),
      snapshot.revision.content_hash);
}

TEST(CanonicalEmptySceneOwner, SealedAdmissionMintsGoldenAndHoldsGatesClosed) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  LogicalSceneOwner owner(admission, publication_reader);

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
  EXPECT_EQ(integrity.acceptance_row_count, 1);
  EXPECT_EQ(integrity.rebuild_row_count, 0);
  EXPECT_EQ(integrity.finalization_row_count, 0);
  EXPECT_EQ(integrity.fixed_query, owner.fixed_query());
}

TEST(CanonicalEmptySceneOwner,
     BeginAcceptsOnlyExactAdmittedBundleAndOneBootstrapLineage) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  LogicalSceneOwner owner(admission, publication_reader);

  auto cross_spliced = Intent(admission);
  cross_spliced.canonical_empty_retained_bundle.deployment_revision =
      core::DeploymentRevision("forged-deployment");
  EXPECT_THROW(owner.BeginProjectionBootstrap(cross_spliced),
               LogicalSceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);

  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  const auto replay = owner.BeginProjectionBootstrap(Intent(admission));
  EXPECT_EQ(replay.canonical_empty_target, accepted.canonical_empty_target);
  EXPECT_EQ(replay.initialized_logical_scene.shared()->revision,
            accepted.initialized_logical_scene.shared()->revision);
  EXPECT_THROW(
      owner.BeginProjectionBootstrap(Intent(admission, "bootstrap-beta")),
      LogicalSceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 1);
}

TEST(CanonicalEmptySceneOwner, ZeroNativeRebuildIsIdempotentAndStatusComplete) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  LogicalSceneOwner owner(admission, publication_reader);
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  const auto rebuilt =
      owner.RebuildProjectionBootstrap(RebuildRequest(accepted));
  ASSERT_TRUE(rebuilt.presence_valid());
  ASSERT_TRUE(rebuilt.bootstrap_evidence.has_value());
  EXPECT_TRUE(rebuilt.bootstrap_evidence->rebuild_results.empty());
  EXPECT_TRUE(rebuilt.bootstrap_evidence->candidate_aggregate_health
                  .native_projection_health.empty());
  EXPECT_EQ(rebuilt.bootstrap_evidence->candidate_aggregate_health.state,
            core::ProjectionHealthState::kHealthy);
  const auto replay =
      owner.RebuildProjectionBootstrap(RebuildRequest(accepted));
  EXPECT_EQ(replay.canonical_empty_target, rebuilt.canonical_empty_target);

  const auto status = owner.ProjectionBootstrapStatus(
      {accepted.bootstrap_id, accepted.bootstrap_generation});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kReadyToPublish);
  ASSERT_TRUE(status.bootstrap_evidence.has_value());
  EXPECT_TRUE(status.bootstrap_evidence->rebuild_results.empty());
  EXPECT_TRUE(status.native_rebuild_observations.empty());
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
  const auto integrity = owner.VerifyIntegrity();
  EXPECT_EQ(integrity.acceptance_row_count, 1);
  EXPECT_EQ(integrity.rebuild_row_count, 1);
  EXPECT_EQ(integrity.finalization_row_count, 0);
}

TEST(CanonicalEmptySceneOwner, NonzeroNativeBootstrapIsExplicitlyUnavailable) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  LogicalSceneOwner owner(admission, publication_reader);
  auto intent = Intent(admission);
  intent.execution_host_bindings.push_back(ProjectionExecutionHostBindingDTO{
      core::CapabilityProviderBindingId("binding"),
      core::CapabilityProviderBindingRevision("binding-revision"),
      core::RuntimeUnitId("runtime"),
      core::RuntimeEpoch("epoch"),
  });
  EXPECT_THROW(owner.BeginProjectionBootstrap(intent),
               LogicalSceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);
}

TEST(CanonicalEmptySceneOwner,
     FinalizeRejectsStaleAndMismatchedHeadThenOpensGates) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  LogicalSceneOwner owner(admission, publication_reader);
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  static_cast<void>(owner.RebuildProjectionBootstrap(RebuildRequest(accepted)));
  const auto receipt = PublicationReceipt(accepted);
  const auto finalize = FinalizeRequest(accepted, receipt);

  const auto stale = owner.FinalizeProjectionBootstrap(finalize);
  ASSERT_TRUE(stale.presence_valid());
  EXPECT_EQ(stale.state, ProjectionBootstrapFinalizeResultState::kStale);
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

  auto mismatched = receipt;
  mismatched.durable_public_pointer_content_digest = Digest('d');
  publication_reader.SetCurrent(mismatched);
  const auto mismatch = owner.FinalizeProjectionBootstrap(finalize);
  ASSERT_TRUE(mismatch.presence_valid());
  EXPECT_EQ(mismatch.state, ProjectionBootstrapFinalizeResultState::kStale);
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

  publication_reader.SetCurrent(receipt);
  const auto finalized = owner.FinalizeProjectionBootstrap(finalize);
  ASSERT_TRUE(finalized.presence_valid());
  EXPECT_EQ(finalized.state,
            ProjectionBootstrapFinalizeResultState::kFinalized);
  EXPECT_EQ(finalized.publication_receipt, receipt);
  const auto access = owner.ReadAccessStatus();
  EXPECT_TRUE(access.scene_writes_open);
  EXPECT_TRUE(access.projection_leases_open);
  ASSERT_TRUE(access.active_projection.has_value());
  EXPECT_EQ(*access.active_projection, accepted.canonical_empty_target);
  const auto snapshot = owner.ReadActiveSnapshot();
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->shared()->identity(),
            accepted.initialized_logical_scene.shared()->identity());
  EXPECT_TRUE(snapshot->canonical_empty_for(
      accepted.initialized_logical_scene.shared()->identity()));
  EXPECT_EQ(snapshot->shared()->revision,
            receipt.request.candidate_projection_revision.scene_revision);
  EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 1);

  const auto observed =
      owner.ObserveProjectionBootstrapPrerequisites(owner.fixed_query());
  EXPECT_TRUE(observed.presence_valid());
  EXPECT_EQ(observed.state,
            ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection);

  // The recorded finalization answers status and replay without another
  // publication read.
  publication_reader.Clear();
  const auto calls_before_replay = publication_reader.calls();
  const auto status = owner.ProjectionBootstrapStatus(
      {BootstrapId("bootstrap-alpha"), BootstrapGeneration(1U)});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(status.publication_receipt, receipt);
  ASSERT_TRUE(status.canonical_empty_target.has_value());
  const auto replay = owner.FinalizeProjectionBootstrap({
      BootstrapId("bootstrap-alpha"),
      BootstrapGeneration(1U),
      *status.canonical_empty_target,
      receipt,
  });
  EXPECT_EQ(replay.state, ProjectionBootstrapFinalizeResultState::kFinalized);
  EXPECT_EQ(publication_reader.calls(), calls_before_replay);
}

TEST(CanonicalEmptySceneOwner,
     PublicationReadUncertaintyNeverOpensWritesOrLeases) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  LogicalSceneOwner owner(admission, publication_reader);
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

TEST(CanonicalEmptySceneOwner, RejectsOverBoundBootstrapIdentifier) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  LogicalSceneOwner owner(admission, publication_reader);
  EXPECT_THROW(
      owner.BeginProjectionBootstrap(Intent(
          admission, std::string(kMaximumSceneStoreIdentifierBytes + 1U, 'x'))),
      std::invalid_argument);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);
}

core::SceneTransactionRequestDTO
AddBoxRequest(const ProjectionRevisionDTO &expected, const CellId &cell_id,
              std::string request_id = "scene-request-add-box") {
  core::Pose3DTO pose;
  pose.position_m = {0.25, -0.5, 0.75};
  return {
      core::PublicRequestContextDTO{
          SceneRequestKey(request_id),
          "scene-owner-test",
      },
      {},
      expected,
      core::AddObjectRequestDTO{
          core::SceneResourceId("scene-box"),
          core::BoxShapeDTO{{0.1, 0.2, 0.3}},
          pose,
          core::CellSceneFrameRefDTO{cell_id},
      },
      core::AdmissionGeneration(1U),
  };
}

// Bytes of the mesh resource URI of AddLargeMeshRequest: three such objects
// keep a Scene below kMaximumLogicalSceneSnapshotBytes, a fourth exceeds it,
// and each request stays far below any request bound.
constexpr std::size_t kLargeMeshUriBytes = 100U * 1024U;

// An authored add of a captured mesh whose resource URI is kLargeMeshUriBytes
// long, so its object dominates the Scene's encoding.
core::SceneTransactionRequestDTO
AddLargeMeshRequest(const ProjectionRevisionDTO &expected,
                    const CellId &cell_id, const std::string &name) {
  auto request =
      AddBoxRequest(expected, cell_id, "scene-request-large-mesh-" + name);
  auto &operation = std::get<core::AddObjectRequestDTO>(request.operation);
  operation.scene_resource_id = core::SceneResourceId("large-mesh-" + name);
  std::string uri = "package://manyforge_test/" + name + "/";
  uri.resize(kLargeMeshUriBytes - 4U, 'm');
  uri += ".stl";
  operation.shape =
      core::MeshShapeDTO{std::move(uri), {1.0, 1.0, 1.0}, Digest('e').value()};
  return request;
}

core::PublicRequestContextDTO
SceneRequestContext(std::string request_id,
                    const ProjectionRevisionDTO &expected);

TEST(LogicalSceneTransactionCodec, AttachGraspLostBoundIsOptionalAndRoundTrips) {
  const ProjectionRevisionDTO expected{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{core::SceneId(Digest('d').value()), 1U, Digest('a')},
  };
  auto request = AddBoxRequest(expected, CellId("cell-a"));
  request.operation = core::AttachCarriedObjectRequestDTO{
      core::CarriedObjectAttachmentId("attachment.1"),
      core::SceneResourceId("scene-box"),
      core::ControllableComponentId("component.1"),
      core::CommandTargetId("arm"),
      core::FrameRoleId("tool_frame"),
      {},
      {}};
  const auto plain = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_EQ(plain.find("grasp_lost_bound"), std::string::npos);
  EXPECT_FALSE(std::get<core::AttachCarriedObjectRequestDTO>(
                   DecodeCanonicalSceneTransactionRequest(plain).operation)
                   .grasp_lost_bound.has_value());
  std::get<core::AttachCarriedObjectRequestDTO>(request.operation)
      .grasp_lost_bound = core::CarriedObjectGraspLostBoundDTO{0.03, 0.26};
  const auto bounded = logical_scene_codec_internal::EncodeRequest(request);
  const auto decoded = DecodeCanonicalSceneTransactionRequest(bounded);
  EXPECT_EQ(std::get<core::AttachCarriedObjectRequestDTO>(decoded.operation)
                .grasp_lost_bound,
            (core::CarriedObjectGraspLostBoundDTO{0.03, 0.26}));
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded), bounded);
  // The defaults are absent on the wire, also when given explicitly.
  EXPECT_EQ(bounded.find("on_grasp_lost"), std::string::npos);
  auto &attach = std::get<core::AttachCarriedObjectRequestDTO>(request.operation);
  attach.on_grasp_lost = core::GraspLostResponseDTO{};
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(request), bounded);
  // A non-default response carries all four keys and round-trips.
  attach.on_grasp_lost =
      core::GraspLostResponseDTO{false, false, "part_lost", true};
  const auto keyed = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_NE(
      keyed.find(
          R"("on_grasp_lost":{"detach":true,"raise_alarm":false,"set_key":"part_lost","stop_motion":false})"),
      std::string::npos);
  const auto keyed_decoded = DecodeCanonicalSceneTransactionRequest(keyed);
  EXPECT_EQ(std::get<core::AttachCarriedObjectRequestDTO>(keyed_decoded.operation)
                .on_grasp_lost,
            attach.on_grasp_lost);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(keyed_decoded), keyed);
  // Without a key: `set_key` is null.
  attach.on_grasp_lost = core::GraspLostResponseDTO{false, true, std::nullopt, false};
  const auto unkeyed = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_NE(unkeyed.find(R"("set_key":null)"), std::string::npos);
  EXPECT_EQ(std::get<core::AttachCarriedObjectRequestDTO>(
                DecodeCanonicalSceneTransactionRequest(unkeyed).operation)
                .on_grasp_lost,
            attach.on_grasp_lost);
  const auto refused = [&](std::string from, const std::string &to) {
    auto text = keyed;
    text.replace(text.find(from), from.size(), to);
    EXPECT_THROW(DecodeCanonicalSceneTransactionRequest(text),
                 std::invalid_argument)
        << text;
  };
  // An alarm without the Stop.
  refused(R"("raise_alarm":false)", R"("raise_alarm":true)");
  // A present member equal to the defaults.
  refused(
      R"({"detach":true,"raise_alarm":false,"set_key":"part_lost","stop_motion":false})",
      R"({"detach":true,"raise_alarm":true,"set_key":null,"stop_motion":true})");
  // A missing key, an empty key name, a wrong type.
  refused(R"("detach":true,)", "");
  refused(R"("set_key":"part_lost")", R"("set_key":"")");
  refused(R"("detach":true)", R"("detach":"yes")");
  // OD-372: a mounted component's carrier frame is named only when present.
  EXPECT_EQ(keyed.find("carrier_component_id"), std::string::npos);
  attach.carrier_component_id = core::ControllableComponentId("tube_holder");
  const auto mounted = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_NE(mounted.find(R"("carrier_component_id":"tube_holder")"), std::string::npos);
  EXPECT_EQ(std::get<core::AttachCarriedObjectRequestDTO>(
                DecodeCanonicalSceneTransactionRequest(mounted).operation)
                .carrier_component_id,
            attach.carrier_component_id);
}

TEST(LogicalSceneTransactionCodec, ScenePolicyCarriesNoCollisionRules) {
  const ProjectionRevisionDTO expected{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{core::SceneId(Digest('d').value()), 1U, Digest('a')},
  };
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U, std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  core::SceneTransactionRequestDTO request{
      core::PublicRequestContextDTO{SceneRequestKey("scene-request-policy"),
                                    "scene-owner-test"},
      {},
      expected,
      core::SetCollisionPolicyRequestDTO{policy, policy.revision},
      core::AdmissionGeneration(1U),
  };
  const auto encoded = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_EQ(encoded.find("contact_rules"), std::string::npos);
  EXPECT_EQ(encoded.find("support_contacts"), std::string::npos);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(
                DecodeCanonicalSceneTransactionRequest(encoded)),
            encoded);

  // Collision rules belong to one move; the Scene's policy cannot hold them.
  std::get<core::SetCollisionPolicyRequestDTO>(request.operation)
      .configured_policy.contact_rules = {core::CollisionPairContactRule{
      {"scene/part/shape/0", "scene/table/shape/0"},
      "lift",
      core::CollisionReleaseRule{}}};
  EXPECT_THROW(logical_scene_codec_internal::EncodeRequest(request),
               std::invalid_argument);
}

TEST(LogicalSceneTransactionCodec, ObjectMassIsAbsentAtZeroAndPositiveWhenPresent) {
  const ProjectionRevisionDTO expected{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{core::SceneId(Digest('d').value()), 1U, Digest('a')},
  };
  auto request = AddBoxRequest(expected, CellId("cell-a"));
  const auto plain = logical_scene_codec_internal::EncodeRequest(request);
  // A zero mass is absent, so massless requests keep their bytes.
  EXPECT_EQ(plain.find("mass_kg"), std::string::npos);
  EXPECT_DOUBLE_EQ(std::get<core::AddObjectRequestDTO>(
                       DecodeCanonicalSceneTransactionRequest(plain).operation)
                       .mass_kg,
                   0.0);
  std::get<core::AddObjectRequestDTO>(request.operation).mass_kg = 2.5;
  const auto heavy = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_NE(heavy.find(R"("mass_kg":2.5)"), std::string::npos);
  const auto decoded = DecodeCanonicalSceneTransactionRequest(heavy);
  EXPECT_DOUBLE_EQ(
      std::get<core::AddObjectRequestDTO>(decoded.operation).mass_kg, 2.5);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded), heavy);

  const auto &add = std::get<core::AddObjectRequestDTO>(request.operation);
  request.operation = core::UpsertObjectRequestDTO{
      add.scene_resource_id, add.shape, add.pose, add.frame_ref,
      add.mass_kg};
  const auto upsert = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_DOUBLE_EQ(std::get<core::UpsertObjectRequestDTO>(
                       DecodeCanonicalSceneTransactionRequest(upsert).operation)
                       .mass_kg,
                   2.5);

  // A present mass must be positive finite numeric data.
  for (const std::string invalid : {"0", "-1", R"("2.5")", "null"}) {
    auto text = heavy;
    const std::string from = R"("mass_kg":2.5)";
    text.replace(text.find(from), from.size(), R"("mass_kg":)" + invalid);
    EXPECT_THROW(DecodeCanonicalSceneTransactionRequest(text),
                 std::invalid_argument)
        << text;
  }
}

TEST(LogicalSceneTransactionCodec,
     MeshContentDigestIsOptionalInRequestsAndAbsentFromTheDigestInput) {
  const ProjectionRevisionDTO expected{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{core::SceneId(Digest('d').value()), 1U, Digest('a')},
  };
  auto request = AddBoxRequest(expected, CellId("cell-a"));
  std::get<core::AddObjectRequestDTO>(request.operation).shape =
      core::MeshShapeDTO{"meshes/part.stl", {1.0, 1.0, 1.0}, std::string{}};
  // Absent digest: the Manager fills it before the owner sees the request.
  const auto without = logical_scene_codec_internal::EncodeRequest(request);
  EXPECT_EQ(without.find("mesh_content_digest"), std::string::npos);
  const auto decoded_without = DecodeCanonicalSceneTransactionRequest(without);
  EXPECT_TRUE(std::get<core::MeshShapeDTO>(
                  std::get<core::AddObjectRequestDTO>(decoded_without.operation)
                      .shape)
                  .content_digest.empty());
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded_without),
            without);

  auto filled = request;
  std::get<core::MeshShapeDTO>(
      std::get<core::AddObjectRequestDTO>(filled.operation).shape)
      .content_digest = Digest('e').value();
  const auto with = logical_scene_codec_internal::EncodeRequest(filled);
  EXPECT_NE(with.find("\"mesh_content_digest\":\"" + Digest('e').value()),
            std::string::npos);
  const auto decoded_with = DecodeCanonicalSceneTransactionRequest(with);
  EXPECT_EQ(std::get<core::MeshShapeDTO>(
                std::get<core::AddObjectRequestDTO>(decoded_with.operation)
                    .shape)
                .content_digest,
            Digest('e').value());
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded_with), with);
  // The digest input is request intent only: filled and unfilled match.
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequestDigestInput(filled),
            logical_scene_codec_internal::EncodeRequestDigestInput(request));
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequestDigestInput(filled)
                .find("mesh_content_digest"),
            std::string::npos);

  // A present digest must have the exact spelling.
  auto invalid = with;
  const auto at = invalid.find(Digest('e').value());
  invalid.replace(at, Digest('e').value().size(), "sha256:E");
  EXPECT_THROW(DecodeCanonicalSceneTransactionRequest(invalid),
               std::invalid_argument);
}

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
  request.attribution = {
      core::RequestTransportDTO::kHttp,
      core::RequestAssuranceDTO::kExternallyAuthenticated,
      std::string("operator.7"),
  };
  const auto encoded = logical_scene_codec_internal::EncodeRequest(request);
  const auto decoded = DecodeCanonicalSceneTransactionRequest(encoded);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded), encoded);
  EXPECT_EQ(decoded.request_context.request_key,
            request.request_context.request_key);
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
  // The authorization carries the runner's current cycle status (OD-139).
  const core::ProgramCycleStatusDTO reset_status{
      4U, core::ProgramCycleStateDTO::kResetting, core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U), Digest('e'),
      core::AcceptedProgramResetDTO{SceneRequestKey("reset.0"), Digest('a'), true, false,
                                    std::nullopt}};
  reset_request.program_reset_scene_authorization =
      core::ProgramResetSceneAuthorizationDTO{
          core::ProgramRunNumber(2U),
          core::CellAdmissionClaimGeneration(9U), Digest('e'), std::nullopt, reset_status};
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

  reset_request.program_reset_scene_authorization = core::ProgramResetSceneAuthorizationDTO{
      core::ProgramRunNumber(0U),
      core::CellAdmissionClaimGeneration(0U), core::ContentDigest(""),
      core::ProgramResetClaimSceneAuthorizationDTO{
          3U, SceneRequestKey("reset.1"), Digest('e'),
          core::CellAdmissionClaimGeneration(9U)},
      core::ProgramCycleStatusDTO{
          3U, core::ProgramCycleStateDTO::kResetting, std::nullopt, std::nullopt, std::nullopt,
          core::AcceptedProgramResetDTO{SceneRequestKey("reset.1"), Digest('e'), true, false,
                                        core::CellAdmissionClaimGeneration(9U)}}};
  const auto reset_claim_encoded = logical_scene_codec_internal::EncodeRequest(reset_request);
  const auto reset_claim_decoded = DecodeCanonicalSceneTransactionRequest(reset_claim_encoded);
  EXPECT_EQ(reset_claim_decoded.program_reset_scene_authorization,
            reset_request.program_reset_scene_authorization);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(reset_claim_decoded), reset_claim_encoded);
  EXPECT_NE(reset_claim_encoded, reset_encoded);

  auto node_request = request;
  node_request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      "attach",
      core::NodeAttemptNumber(kMaximumSafeCounter),
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
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-program-add");
  request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      "add",
      core::NodeAttemptNumber(1U),
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
  conflicting.program_node_evidence->node_attempt_id = core::NodeAttemptNumber(2U);
  EXPECT_THROW(owner.ReadCommitResult(conflicting),
               LogicalSceneOwnerConflictError);
  auto missing = request;
  missing.request_context.request_key = SceneRequestKey("scene-program-missing");
  EXPECT_FALSE(owner.ReadCommitResult(missing).has_value());
}

TEST(CanonicalEmptySceneOwner,
     StaleSceneRefusalPreservesAdmissionForTheFreshRequest) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  const auto first =
      owner.Commit(AddBoxRequest(active, admission.cell_id(), "advance"));
  ASSERT_TRUE(first.success);
  // Only another Deployment or Scene is refused; any revision is accepted.
  auto foreign = active;
  foreign.graph_revision = core::GraphRevision(Digest('d').value());
  auto request = AddBoxRequest(foreign, admission.cell_id(), "stale");
  std::atomic<bool> reconciled{true};
  const auto rejected = headless::detail::CommitSceneWithAdmissionObservation(
      owner, request, reconciled);
  ASSERT_FALSE(rejected.success);
  EXPECT_EQ(rejected.error->code, "stale_revision");
  EXPECT_TRUE(reconciled.load());
  EXPECT_FALSE(owner.ReadCommitResult(request));
  request.expected_projection_revision = owner.CurrentProjectionRevision();
  request.request_context.request_key = SceneRequestKey("fresh");
  auto &operation = std::get<core::AddObjectRequestDTO>(request.operation);
  operation.scene_resource_id = core::SceneResourceId("new-box");
  const auto fresh = headless::detail::CommitSceneWithAdmissionObservation(
      owner, request, reconciled);
  ASSERT_TRUE(fresh.success) << fresh.error->message;
  EXPECT_TRUE(reconciled.load());
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
  EXPECT_EQ(cell_admission.Claims().size(), 2U);
}

TEST(CanonicalEmptySceneOwner,
     ProgramClaimsReconcileAfterLostAcquireOrReleaseReply) {
  for (const auto loss : {TestCellAdmissionPort::LostReply::kAcquire,
                          TestCellAdmissionPort::LostReply::kRelease}) {
    const auto admission = LoadAdmission();
    ControlledPublicationReader publication_reader;
    TestCellAdmissionPort cell_admission;
    LogicalSceneOwner owner(admission, publication_reader, cell_admission);
    const auto active =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    auto request =
        AddBoxRequest(active, admission.cell_id(), "scene-program-add");
    request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
        core::ProgramRunNumber(2U),
        core::CellAdmissionClaimGeneration(9U),
        "add",
        core::NodeAttemptNumber(1U),
        "scene.transaction"};
    cell_admission.LoseNextReply(loss);
    std::atomic<bool> reconciled_before{true};
    if (loss == TestCellAdmissionPort::LostReply::kAcquire) {
      EXPECT_FALSE(headless::detail::CommitSceneWithAdmissionObservation(
                       owner, request, reconciled_before)
                       .success);
    } else {
      EXPECT_THROW(headless::detail::CommitSceneWithAdmissionObservation(
                       owner, request, reconciled_before),
                   std::runtime_error);
    }
    EXPECT_FALSE(reconciled_before.load());
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
     InvalidProgramEvidenceNeverEntersSceneState) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-program-add");
  request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      "add",
      core::NodeAttemptNumber(0U),
      "scene.transaction"};
  for (const auto attempt : {core::NodeAttemptNumber(0U),
                             core::NodeAttemptNumber(kMaximumSafeCounter + 1U)}) {
    request.program_node_evidence->node_attempt_id = attempt;
    const auto rejected = owner.Commit(request);
    ASSERT_FALSE(rejected.success);
    ASSERT_TRUE(rejected.error.has_value());
    EXPECT_EQ(rejected.error->code, "invalid_request");
  }
  request.program_node_evidence->node_attempt_id = core::NodeAttemptNumber(1U);
  request.program_node_evidence->parent_program_cycle_claim_generation =
      core::CellAdmissionClaimGeneration(0U);
  EXPECT_FALSE(owner.Commit(request).success);
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 0);
  EXPECT_EQ(owner.VerifyIntegrity().request_outcome_count, 0);
  EXPECT_TRUE(cell_admission.Claims().empty());
  EXPECT_TRUE(owner.ReconcileAuthoredSceneAdmissionClaims().ok());
  EXPECT_TRUE(owner.GetSceneObjects().empty());
}

TEST(CanonicalEmptySceneOwner,
     StopBeforeProgramAdmissionIsExactAndHasNoEffect) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-program-add");
  request.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      "add",
      core::NodeAttemptNumber(1U),
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
      add.scene_resource_id, add.shape, add.pose, add.frame_ref};
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
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  auto active = FinalizeCanonicalBootstrap(owner, publication_reader, admission)
                    .canonical_empty_target;

  auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-request-upsert-create");
  const auto add = std::get<core::AddObjectRequestDTO>(request.operation);
  request.operation = core::UpsertObjectRequestDTO{
      add.scene_resource_id, add.shape, add.pose, add.frame_ref};
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
          core::SceneResourceId("scene-box")}};
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

// OD-302: each publication read is numbered under the owner's lock, so a
// higher number holds a state at least as new; access and snapshot are of one
// state.
TEST(CanonicalEmptySceneOwner, PublicationReadsAreNumberedInStateOrder) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto before = owner.ReadPublication();
  EXPECT_FALSE(before.snapshot.has_value());
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  const auto finalized = owner.ReadPublication();
  EXPECT_GT(finalized.read_sequence, before.read_sequence);
  ASSERT_TRUE(finalized.snapshot.has_value());
  ASSERT_TRUE(finalized.access.active_projection.has_value());
  EXPECT_EQ(finalized.access.active_projection->scene_revision,
            finalized.snapshot->shared()->revision);

  const auto added = owner.Commit(
      AddBoxRequest(active, admission.cell_id(), "scene-request-read-order"));
  ASSERT_TRUE(added.success);
  ASSERT_TRUE(added.committed_projection_revision.has_value());
  const auto committed = owner.ReadPublication();
  EXPECT_GT(committed.read_sequence, finalized.read_sequence);
  EXPECT_EQ(committed.access.active_projection,
            added.committed_projection_revision);
  EXPECT_EQ(committed.snapshot->shared()->revision,
            added.committed_projection_revision->scene_revision);
  // An unchanged state still takes a new number: one number is one read.
  EXPECT_EQ(owner.ReadPublication().read_sequence,
            committed.read_sequence + 1U);
}

TEST(CanonicalEmptySceneOwner,
     ProgramResetRestorationUsesExactAuthorizationWithoutOpeningAuthoring) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  auto request = AddBoxRequest(active, admission.cell_id(),
                               "scene-request-program-reset");
  request.expected_admission_generation.reset();
  request.program_reset_scene_authorization =
      core::ProgramResetSceneAuthorizationDTO{
          core::ProgramRunNumber(2U),
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
     ZeroNativeSceneCommitPublishesAndReplays) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  ProjectionRevisionDTO committed_projection;
  std::optional<core::SceneTransactionRequestDTO> request;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
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
}

TEST(CanonicalEmptySceneOwner,
     RequestKeyReplaysItsRecordedOutcomeAndRefusesAnotherIntent) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  std::optional<core::SceneTransactionRequestDTO> request;
  std::optional<core::SceneCommitResultDTO> committed;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  request = AddBoxRequest(active, admission.cell_id(), "scene-keyed-add");
  EXPECT_FALSE(owner.ReadCommitResult(*request).has_value());
  committed = owner.Commit(*request);
  ASSERT_TRUE(committed->success);
  EXPECT_EQ(committed->request_key, request->request_context.request_key);
  EXPECT_EQ(committed->replay_status,
            core::RequestReplayStatusDTO::kAcceptedNew);

  // Same key, same digest: the recorded outcome, no second effect.
  const auto replay = owner.Commit(*request);
  ASSERT_TRUE(replay.success);
  EXPECT_EQ(replay.request_key, request->request_context.request_key);
  EXPECT_EQ(replay.replay_status,
            core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.transaction_sequence, committed->transaction_sequence);
  EXPECT_EQ(replay.committed_projection_revision,
            committed->committed_projection_revision);
  const auto looked_up = owner.ReadCommitResult(*request);
  ASSERT_TRUE(looked_up.has_value());
  EXPECT_EQ(looked_up->replay_status,
            core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(looked_up->transaction_sequence,
            committed->transaction_sequence);

  // Same key, different digest: the existing conflict code.
  auto conflicting = *request;
  std::get<core::AddObjectRequestDTO>(conflicting.operation)
      .pose.position_m.x_m = 9.0;
  const auto rejected = owner.Commit(conflicting);
  EXPECT_FALSE(rejected.success);
  EXPECT_EQ(rejected.request_key, request->request_context.request_key);
  EXPECT_EQ(rejected.replay_status,
            core::RequestReplayStatusDTO::kRequestIdConflict);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code, "request_id_conflict");
  EXPECT_THROW(static_cast<void>(owner.ReadCommitResult(conflicting)),
               LogicalSceneOwnerConflictError);

  // Another key is unknown.
  auto other = *request;
  other.request_context.request_key = SceneRequestKey("scene-keyed-other");
  EXPECT_FALSE(owner.ReadCommitResult(other).has_value());
  const auto integrity = owner.VerifyIntegrity();
  EXPECT_EQ(integrity.transaction_intent_count, 1);
  EXPECT_EQ(integrity.request_outcome_count, 1);
}

TEST(CanonicalEmptySceneOwner, CommitsTheClosedPortableCollisionShapeSet) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  auto projection = accepted.canonical_empty_target;
  const std::vector<std::pair<std::string, core::CollisionShapeDTO>> shapes{
      {"box", core::BoxShapeDTO{{0.1, 0.2, 0.3}}},
      {"sphere", core::SphereShapeDTO{0.05}},
      {"cylinder", core::CylinderShapeDTO{0.04, 0.2}},
      {"mesh", core::MeshShapeDTO{"package://manyforge_test/fixture.stl",
                                  {1.0, 1.0, 1.0}, Digest('e').value()}},
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
  ASSERT_TRUE(std::holds_alternative<core::MeshShapeDTO>(
      snapshot->objects[2].collision_shapes.front()));
  EXPECT_EQ(std::get<core::MeshShapeDTO>(
                snapshot->objects[2].collision_shapes.front())
                .content_digest,
            Digest('e').value());
  EXPECT_TRUE(std::holds_alternative<core::SphereShapeDTO>(
      snapshot->objects[3].collision_shapes.front()));
  EXPECT_EQ(snapshot->transaction_sequence, 4U);
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 4);

  // The persisted shape requires the captured digest.
  const auto snapshot_bytes = EncodeLogicalSceneSnapshot(*snapshot);
  const std::string digest_member =
      "\"mesh_content_digest\":\"" + Digest('e').value() + "\",";
  const auto member_at = snapshot_bytes.find(digest_member);
  ASSERT_NE(member_at, std::string::npos);
  auto without_digest = snapshot_bytes;
  without_digest.erase(member_at, digest_member.size());
  EXPECT_THROW(static_cast<void>(DecodeLogicalSceneSnapshot(without_digest)),
               std::invalid_argument);

  // A mesh the Manager did not fill is a typed refusal.
  auto unfilled = AddBoxRequest(projection, admission.cell_id(),
                                "scene-request-shape-unfilled-mesh");
  auto &unfilled_operation =
      std::get<core::AddObjectRequestDTO>(unfilled.operation);
  unfilled_operation.scene_resource_id = core::SceneResourceId("scene-unfilled");
  unfilled_operation.shape = core::MeshShapeDTO{
      "package://manyforge_test/fixture.stl", {1.0, 1.0, 1.0}, std::string{}};
  const auto refused = owner.Commit(unfilled);
  ASSERT_FALSE(refused.success);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "scene_mesh_unavailable");
  EXPECT_EQ(refused.error->details.at("resource_uri"),
            "package://manyforge_test/fixture.stl");
}

core::PublicRequestContextDTO
SceneRequestContext(std::string request_id, const ProjectionRevisionDTO &) {
  return {
      SceneRequestKey(request_id),
      "scene-owner-test",
  };
}

class FakePlannerSceneProjection final : public core::IPlannerSceneProjection {
public:
  enum class Mode { kCommit, kRollbackProven, kUncertain, kMalformed };

  explicit FakePlannerSceneProjection(std::string identity)
      : FakePlannerSceneProjection(ProjectionExecutionHostBindingDTO{
            core::CapabilityProviderBindingId("provider-" + identity),
            core::CapabilityProviderBindingRevision("binding-revision-" +
                                                    identity),
            core::RuntimeUnitId("runtime-" + identity),
            core::RuntimeEpoch("runtime-epoch-" + identity),
        }) {}

  explicit FakePlannerSceneProjection(
      ProjectionExecutionHostBindingDTO binding)
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
    rebuild_requests_.clear();
    throw_after_next_rebuild_effect_ = false;
    reject_next_rebuild_without_effect_ = false;
    rebuild_entered_ = nullptr;
    rebuild_release_ = nullptr;
    apply_entered_ = nullptr;
    apply_release_ = nullptr;
    blocked_transaction_.reset();
    next_preflight_rejection_.reset();
    during_next_preflight_ = nullptr;
    preflight_calls_ = 0U;
    health_for_revision_calls_ = 0U;
    health_calls_outside_preflight_ = 0U;
    policy_calls_outside_preflight_ = 0U;
    replay_checks_.clear();
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
  [[nodiscard]] const std::vector<core::NativeProjectionRebuildDTO> &
  rebuild_requests() const noexcept {
    return rebuild_requests_;
  }
  [[nodiscard]] const ProjectionExecutionHostBindingDTO &binding() const
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

  void RejectNextRebuildWithoutEffect() noexcept {
    reject_next_rebuild_without_effect_ = true;
  }

  void AcceptNextRebuildAs(
      ProjectionExecutionHostBindingDTO resulting_binding) {
    accepted_rebuild_binding_ = std::move(resulting_binding);
  }

  void BlockNextRebuild(std::barrier<> &entered, std::barrier<> &release) {
    rebuild_entered_ = &entered;
    rebuild_release_ = &release;
  }

  void BlockNextApply(std::barrier<> &entered, std::barrier<> &release) {
    apply_entered_ = &entered;
    apply_release_ = &release;
  }

  // The transaction of the apply held by BlockNextApply, readable once its
  // `entered` barrier was passed.
  [[nodiscard]] const std::optional<core::SceneTransactionDTO> &
  blocked_transaction() const noexcept {
    return blocked_transaction_;
  }

  // Opt-in (OD-376): the next preflight call answers this typed refusal.
  void RejectNextPreflight(core::ErrorDTO error) {
    next_preflight_rejection_ = std::move(error);
  }

  // Opt-in: runs once inside the next preflight call, on the committing
  // thread while the owner holds no lock, after this projection formed its
  // answer and before the owner receives it (a change racing the answer).
  void RunDuringNextPreflight(std::function<void()> action) {
    during_next_preflight_ = std::move(action);
  }

  // OD-376: the Scene commit preflight calls, and the health and policy calls
  // made outside one.
  [[nodiscard]] std::size_t preflight_calls() const noexcept {
    return preflight_calls_;
  }
  [[nodiscard]] std::size_t health_for_revision_calls() const noexcept {
    return health_for_revision_calls_;
  }
  [[nodiscard]] std::size_t health_calls_outside_preflight() const noexcept {
    return health_calls_outside_preflight_;
  }
  [[nodiscard]] std::size_t policy_calls_outside_preflight() const noexcept {
    return policy_calls_outside_preflight_;
  }

  // What a projection replaying the owner's delta found (OD-376): the owner
  // validates and digests the candidate once; this check replays the delta
  // onto the owner's predecessor and compares it with the owner's digest.
  struct ReplayCheck final {
    bool applied{};
    bool content_hash_matches{};
    bool locally_valid{};
    std::optional<core::LogicalSceneSnapshotDTO> result;
    std::string error;
  };

  // Opt-in: each ApplyTransaction reads `owner`'s snapshot (the predecessor,
  // readable because the owner lock is not held across the native apply).
  void ReplayAgainst(const LogicalSceneOwner *owner) noexcept {
    replay_owner_ = owner;
  }

  [[nodiscard]] const std::vector<ReplayCheck> &replay_checks() const noexcept {
    return replay_checks_;
  }

  core::Result<core::SceneCommitPreflightDTO> CommitPreflight(
      const ProjectionRevisionDTO &selected_revision,
      const core::SceneTransactionDTO *transaction,
      const core::LogicalSceneSnapshotDTO *candidate) const override {
    ++preflight_calls_;
    if (next_preflight_rejection_.has_value()) {
      return core::Result<core::SceneCommitPreflightDTO>::Err(
          *std::exchange(next_preflight_rejection_, std::nullopt));
    }
    class InPreflight final {
    public:
      explicit InPreflight(bool &flag) : flag_(flag) { flag_ = true; }
      ~InPreflight() { flag_ = false; }
      InPreflight(const InPreflight &) = delete;
      InPreflight &operator=(const InPreflight &) = delete;

    private:
      bool &flag_;
    };
    auto answer = [&] {
      const InPreflight scope(in_preflight_);
      // What the port's default does.
      return core::IPlannerSceneProjection::CommitPreflight(
          selected_revision, transaction, candidate);
    }();
    if (auto action = std::exchange(during_next_preflight_, nullptr)) {
      action();
    }
    return answer;
  }

  core::ProjectionCommitResultDTO
  ApplyTransaction(const core::SceneTransactionDTO &transaction) override {
    if (apply_entered_ != nullptr && apply_release_ != nullptr) {
      auto *entered = std::exchange(apply_entered_, nullptr);
      auto *release = std::exchange(apply_release_, nullptr);
      blocked_transaction_ = transaction;
      entered->arrive_and_wait();
      release->arrive_and_wait();
    }
    if (replay_owner_ != nullptr) {
      const auto predecessor = replay_owner_->LogicalSnapshot();
      auto replayed =
          core::ApplyResolvedSceneTransaction(*predecessor, transaction);
      ReplayCheck check;
      check.applied = replayed.ok();
      if (replayed.ok()) {
        try {
          check.content_hash_matches =
              core::logical_scene_identity::ComputeLogicalSceneContentHash(
                  replayed.value()) ==
              transaction.proposed_projection_revision.scene_revision
                  .content_hash;
        } catch (const std::exception &error) {
          check.error = error.what();
        }
        check.locally_valid = replayed.value().locally_valid();
        check.result = replayed.value();
      } else {
        check.error = replayed.error().message;
      }
      replay_checks_.push_back(std::move(check));
    }
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
        rejection_message_,
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
                selector.frame_component_id.value_or(
                    selector.controllable_component_id),
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
    if (!in_preflight_) {
      ++policy_calls_outside_preflight_;
    }
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

  void ObserveRevisionQueries() {
    observe_revision_queries_ = true;
    last_health_revision_.reset();
  }

  [[nodiscard]] const std::optional<ProjectionRevisionDTO> &last_health_revision() const {
    return last_health_revision_;
  }

  core::ProjectionHealthDTO ProjectionHealthForRevision(
      const ProjectionRevisionDTO &revision) const override {
    // This fake has one exact model. Like native RevisionHandle, selecting its
    // retained product returns actual health, including a healthy predecessor
    // Scene revision during an ordinary in-place maintenance successor.
    ++health_for_revision_calls_;
    if (!in_preflight_) {
      ++health_calls_outside_preflight_;
    }
    if (!revision.valid() ||
        (health_.active_projection_revision.has_value() &&
         (health_.active_projection_revision->retained_bundle_id != revision.retained_bundle_id ||
          health_.active_projection_revision->graph_revision != revision.graph_revision))) {
      throw std::invalid_argument("test health revision names an absent retained model");
    }
    if (observe_revision_queries_) {
      last_health_revision_ = revision;
    }
    return health_;
  }

  core::ProjectionHealthDTO ProjectionHealth() const override {
    if (!in_preflight_) {
      ++health_calls_outside_preflight_;
    }
    return health_;
  }

  core::ProjectionCommitResultDTO
  RebuildProjection(const core::NativeProjectionRebuildDTO &request) override {
    if (!request.valid() ||
        request.expected_execution_host_binding != binding_) {
      throw std::invalid_argument(
          "test native projection rebuild request is foreign");
    }
    ++rebuild_calls_;
    rebuild_requests_.push_back(request);
    if (std::exchange(reject_next_rebuild_without_effect_, false)) {
      return {
          std::nullopt,
          request.target_projection_revision,
          core::ProjectionCommitResultStateDTO::kRejectedRollbackProven,
          health_.active_projection_revision,
          health_.native_projection_generation,
          true,
          health_,
          ErrorDTO{"projection_rejected_rollback_proven",
                   "injected rebuild refusal without effects", {}},
      };
    }
    if (rebuild_entered_ != nullptr && rebuild_release_ != nullptr) {
      auto *entered = std::exchange(rebuild_entered_, nullptr);
      auto *release = std::exchange(rebuild_release_, nullptr);
      entered->arrive_and_wait();
      release->arrive_and_wait();
    }
    if (accepted_rebuild_binding_.has_value()) {
      auto accepted_health = health_;
      accepted_health.execution_host_binding = *accepted_rebuild_binding_;
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
  ProjectionExecutionHostBindingDTO binding_;
  Mode mode_{Mode::kCommit};
  std::string rejection_message_{"injected native projection result"};
  core::ProjectionHealthDTO health_;
  bool throw_after_next_rebuild_effect_{};
  bool reject_next_rebuild_without_effect_{};
  std::barrier<> *rebuild_entered_{nullptr};
  std::barrier<> *rebuild_release_{nullptr};
  std::barrier<> *apply_entered_{nullptr};
  std::barrier<> *apply_release_{nullptr};
  std::optional<core::SceneTransactionDTO> blocked_transaction_;
  mutable std::optional<core::ErrorDTO> next_preflight_rejection_;
  mutable std::function<void()> during_next_preflight_;
  mutable bool in_preflight_{false};
  mutable std::size_t preflight_calls_{};
  mutable std::size_t health_for_revision_calls_{};
  mutable std::size_t health_calls_outside_preflight_{};
  mutable std::size_t policy_calls_outside_preflight_{};
  const LogicalSceneOwner *replay_owner_{nullptr};
  std::vector<ReplayCheck> replay_checks_;
  std::optional<ProjectionExecutionHostBindingDTO>
      accepted_rebuild_binding_;
  std::size_t calls_{};
  std::size_t rebuild_calls_{};
  std::vector<core::NativeProjectionRebuildDTO> rebuild_requests_;
  std::vector<core::SceneTransactionDTO> transactions_;
  core::Pose3DTO frame_pose_in_cell_{};
  std::vector<core::ComponentAttachmentRevision> attachment_revisions_;
  std::optional<core::ErrorDTO> frame_resolution_error_;
  std::optional<core::ModelInstanceId> resolved_model_instance_override_;
  std::optional<core::FrameRoleId> resolved_frame_role_override_;
  mutable std::size_t frame_resolution_calls_{};
  mutable std::size_t policy_projection_calls_{};
  std::set<core::ProjectionLeaseId> retained_leases_;
  bool observe_revision_queries_{false};
  mutable std::optional<ProjectionRevisionDTO> last_health_revision_;
};

TEST(CanonicalEmptySceneOwner, DefaultProjectionHealthSelectorRefusesConflictingTruth) {
  FakePlannerSceneProjection projection("default-health-selector");
  const ProjectionRevisionDTO selected{
      core::RetainedBundleId(Digest('b').value()), GraphRevision(Digest('c').value()),
      core::SceneRevision{core::SceneId(Digest('d').value()), 1U, Digest('a')}};
  const auto default_health = [&](const ProjectionRevisionDTO &revision) {
    return projection.core::IPlannerSceneProjection::ProjectionHealthForRevision(revision);
  };
  ASSERT_TRUE(selected.valid());
  const auto pending = projection.ProjectionHealth();
  const auto observed_pending = default_health(selected);
  EXPECT_EQ(observed_pending.state, pending.state);
  EXPECT_EQ(observed_pending.active_projection_revision, pending.active_projection_revision);
  EXPECT_EQ(observed_pending.source_sequence, pending.source_sequence);
  auto invalid = selected;
  invalid.retained_bundle_id = core::RetainedBundleId("");
  EXPECT_THROW(default_health(invalid), std::invalid_argument);
  projection.Reset(selected, FakePlannerSceneProjection::Mode::kCommit);
  const auto exact = projection.ProjectionHealth();
  const auto observed_exact = default_health(selected);
  EXPECT_EQ(observed_exact.execution_host_binding, exact.execution_host_binding);
  EXPECT_EQ(observed_exact.source_sequence, exact.source_sequence);
  EXPECT_EQ(observed_exact.state, exact.state);
  EXPECT_EQ(observed_exact.active_projection_revision, exact.active_projection_revision);
  EXPECT_EQ(observed_exact.native_projection_generation, exact.native_projection_generation);
  EXPECT_EQ(observed_exact.projection_content_digest, exact.projection_content_digest);
  EXPECT_EQ(observed_exact.error, exact.error);
  auto foreign = selected;
  foreign.retained_bundle_id = core::RetainedBundleId(Digest('e').value());
  EXPECT_THROW(default_health(foreign), std::invalid_argument);
  // Any revision of the same Scene is the same projection identity.
  auto newer_revision = selected;
  ++newer_revision.scene_revision.revision_id;
  EXPECT_NO_THROW(default_health(newer_revision));
  auto other_scene = selected;
  other_scene.scene_revision.scene_id = core::SceneId(Digest('e').value());
  EXPECT_THROW(default_health(other_scene), std::invalid_argument);
  EXPECT_EQ(projection.ProjectionHealth().active_projection_revision, exact.active_projection_revision);
  EXPECT_EQ(projection.ProjectionHealth().source_sequence, exact.source_sequence);
  EXPECT_EQ(projection.rebuild_calls(), 0U);
  EXPECT_EQ(projection.calls(), 0U);
}

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

  core::ProjectionHealthDTO ProjectionHealthForRevision(
      const ProjectionRevisionDTO &revision) const override {
    return projection_.ProjectionHealthForRevision(revision);
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

  core::Result<core::SceneCommitPreflightDTO> CommitPreflight(
      const ProjectionRevisionDTO &selected_revision,
      const core::SceneTransactionDTO *transaction,
      const core::LogicalSceneSnapshotDTO *candidate) const override {
    return projection_.CommitPreflight(selected_revision, transaction,
                                       candidate);
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
      const ProjectionExecutionHostBindingDTO &binding) const override {
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

[[nodiscard]] std::vector<ProjectionExecutionHostBindingDTO>
ExecutionHostBindings(const core::RetainedBundleDTO &bundle,
                      const std::string_view epoch_prefix) {
  std::set<core::RuntimeUnitId> declared_runtime_units;
  for (const auto &runtime_unit : bundle.cell_graph.runtime_units) {
    declared_runtime_units.insert(runtime_unit.runtime_unit_id);
  }

  std::vector<ProjectionExecutionHostBindingDTO> result;
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
    const std::vector<ProjectionExecutionHostBindingDTO> &bindings) {
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
    LogicalSceneOwner &owner,
    ControlledPublicationReader &publication_reader,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::RetainedBundle &candidate,
    const std::vector<ProjectionExecutionHostBindingDTO> &native_bindings,
    const std::uint64_t apply_number = 100U) {
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const ApplyId operation_id(apply_number);
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

class TestAdoptionFence final : public core::IManagerKernelApplyFencePort {
public:
  void Configure(const core::ConfigurationFenceStatusQueryDTO &query,
                 const ActiveBundlePublicationReceiptDTO &predecessor) {
    const auto &projection = predecessor.request.candidate_projection_revision;
    status_ = core::ConfigurationFenceStatusDTO{
        query,
        {{core::RuntimeUnitId("execution-host"), core::RuntimeEpoch("host.1"),
          Digest('4'), predecessor.publication_generation},
         query.owner_transaction, query.configuration_fence_id,
         query.configuration_fence_generation, query.expected_scope, 1U,
         projection.graph_revision, projection, predecessor.publication_generation,
         predecessor.durable_public_pointer_content_digest, {}, 0U, 0U, Digest('5')},
        core::ConfigurationFenceState::kHeld, true, std::nullopt, "", Digest('6'), std::nullopt};
    Require(status_->matches(query), "test adoption fence must be well formed");
    Require(query.configuration_fence_generation.value() > 0U,
            "test adoption fence must have an actual accepted generation");
    original_attempt_ = core::ApplyFenceAcquireAttemptQueryDTO{
        {{SceneRequestKey("parent-fence-request"), "original-Manager-test"},
         query.owner_transaction, query.expected_scope,
         core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
         predecessor.request.candidate_retained_bundle_id,
         predecessor.request.candidate_graph_revision, 0U,
         core::ConfigurationFenceGeneration(query.configuration_fence_generation.value() - 1U),
         {}, "preserve the actual Scene"}, Digest('7')};
    presence_ = core::ApplyFenceAcquireAttemptReconcileResultDTO{
        status_->reconciliation_evidence.execution_host_epoch, *original_attempt_,
        core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact,
        core::ConfigurationFenceAcceptedDTO{query.configuration_fence_id,
            query.configuration_fence_generation, query.expected_scope, 0,
            core::RequestReplayStatusDTO::kAcceptedNew},
        true, {}, Digest('8'), std::nullopt};
    Require(presence_->matches(*original_attempt_), "test held presence must name its configured owner");
  }

  [[nodiscard]] const core::ApplyFenceAcquireAttemptQueryDTO &OriginalAttemptQuery() const {
    Require(original_attempt_.has_value(), "test original attempt is unavailable");
    return *original_attempt_;
  }
  [[nodiscard]] const core::ApplyFenceAcquireAttemptReconcileResultDTO &Presence() const {
    Require(presence_.has_value(), "test held presence is unavailable");
    return *presence_;
  }
  void SetPresence(core::ApplyFenceAcquireAttemptReconcileResultDTO presence) {
    presence_ = std::move(presence);
  }
  void RefuseFullStatusQueries() { full_status_queries_allowed_ = false; }

  void RefuseRemoteQueryAfter(std::size_t successful_queries) {
    remaining_remote_queries_ = successful_queries;
  }

  core::ConfigurationFenceStatusDTO QueryStatus(
      const core::ConfigurationFenceStatusQueryDTO &) const override {
    Require(full_status_queries_allowed_, "full status requires installed candidate configurations");
    RequireOriginalRead();
    return *status_;
  }

  core::ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const core::ConfigurationFenceAdmissionQueryDTO &) const override {
    throw std::logic_error("Scene adoption must not observe/acquire a new fence");
  }
  core::ConfigurationFenceAcceptedDTO Acquire(
      const core::ApplyConfigurationFenceIntentDTO &) override {
    throw std::logic_error("Scene adoption must not acquire a fence");
  }
  core::ApplyFenceAcquireAttemptReconcileResultDTO ReconcileAcquireAttempt(
      const core::ApplyFenceAcquireAttemptQueryDTO &) const override {
    RequireOriginalRead();
    return *presence_;
  }
  core::ConfigurationFenceReleasedDTO Release(
      const core::ConfigurationFenceReleaseDTO &) override {
    throw std::logic_error("Scene adoption must not release a fence");
  }

private:
  void RequireOriginalRead() const {
    Require(status_.has_value() && presence_.has_value(), "test adoption fence has not been configured");
    if (remaining_remote_queries_.has_value()) {
      if (*remaining_remote_queries_ == 0U) {
        throw std::runtime_error("original Manager fence response unavailable");
      }
      --*remaining_remote_queries_;
    }
  }
  std::optional<core::ConfigurationFenceStatusDTO> status_;
  std::optional<core::ApplyFenceAcquireAttemptQueryDTO> original_attempt_;
  std::optional<core::ApplyFenceAcquireAttemptReconcileResultDTO> presence_;
  bool full_status_queries_allowed_{true};
  mutable std::optional<std::size_t> remaining_remote_queries_;
};

deployment::CanonicalEmptyRetainedBundleAdmission PrepareAdoptionAdmission(
    const std::filesystem::path &package_root) {
  CopyCanonicalEmptyPackage(package_root);
  return LoadAdmission(package_root);
}

deployment::test::DurableLite6RetainedConsumerFixture AdoptionProduct(
    const std::filesystem::path &package_root, const CellId &cell_id,
    const char revision_digit) {
  // The digit selects a distinct sealed product through the arm's initial
  // joint position; the lifecycle offers are the same for both.
  return deployment::test::materialize_lite6_retained_consumer_fixture_from(
      package_root,
      deployment::test::make_lite6_controllable_retention_fixture(
          false, false, 1U, cell_id.value(), false, true,
          "resource", true, Digest(revision_digit).value(), {},
          false, false, 0.1 * static_cast<double>(revision_digit - '0'),
          deployment::test::kLite6ArmTargetId, std::nullopt, std::nullopt, {},
          true, {}, "manyforge.provider.ros2_control", false, true));
}

/// The same request for another Scene. Only identity is bound by Apply, so a
/// different revision of the same Scene is accepted, but a foreign Scene is not.
template <typename Request>
void NameForeignScene(Request &request) {
  auto &scene = request.expected_snapshot_identity.revision.scene_id;
  scene = core::SceneId(Digest(scene.value() == Digest('e').value() ? 'f' : 'e').value());
  request.expected_active_projection_revision.scene_revision =
      request.expected_snapshot_identity.revision;
}

/// Runtime-owned (provider) pose evidence; it needs no admission claim.
core::SceneObjectProviderEvidenceDTO StreamedPoseEvidence() {
  return {core::ProviderInstanceId("stream-provider"),
          core::ProviderManifestId("stream-manifest"),
          core::ProviderManifestRevision(Digest('7').value()),
          "stream-epoch"};
}

struct SceneAdoptionFixture final {
  TemporaryDirectory directory{"scene-adoption"};
  std::filesystem::path package_root{directory.path() / "packages"};
  deployment::CanonicalEmptyRetainedBundleAdmission admission{PrepareAdoptionAdmission(package_root)};
  deployment::test::DurableLite6RetainedConsumerFixture predecessor{
      AdoptionProduct(package_root, admission.cell_id(), '1')};
  deployment::test::DurableLite6RetainedConsumerFixture candidate{
      AdoptionProduct(package_root, admission.cell_id(), '2')};
  std::vector<ProjectionExecutionHostBindingDTO> predecessor_bindings{
      ExecutionHostBindings(predecessor.bundle->value(), "predecessor")};
  std::vector<ProjectionExecutionHostBindingDTO> candidate_bindings{
      ExecutionHostBindings(candidate.bundle->value(), "candidate")};
  OwnedFakePlannerSceneProjections predecessor_projections{MakeFakeProjections(predecessor_bindings)};
  OwnedFakePlannerSceneProjections candidate_projections{MakeFakeProjections(candidate_bindings)};
  FakePlannerSceneProjectionResolver resolver{ProjectionClients(predecessor_projections)};
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source{package_root};
  TestCellAdmissionPort cell_admission;
  TestAdoptionFence fence;
  std::unique_ptr<LogicalSceneOwner> owner;

  // Without `claim_dependency` the owner has no cell-admission port.
  explicit SceneAdoptionFixture(const bool claim_dependency = true)
      : owner(claim_dependency
                  ? std::make_unique<LogicalSceneOwner>(
                        admission, publication_reader, bundle_source, resolver,
                        cell_admission)
                  : std::make_unique<LogicalSceneOwner>(
                        admission, publication_reader, bundle_source,
                        resolver)) {
    const auto current = FinalizeNativeCandidate(
        *owner, publication_reader, admission, *predecessor.bundle, predecessor_bindings);
    ResetProjections(predecessor_projections, current, FakePlannerSceneProjection::Mode::kCommit);
    Require(predecessor.bundle->value().retained_bundle_id != candidate.bundle->value().retained_bundle_id,
            "adoption fixture must change the sealed product");
  }

  QualifiedSceneConfigurationAdoptionRequest CaptureConfigurationRequest(
      bool authoring = false, bool same_product = false) {
    const core::ApplyFenceOwnerDTO manager{ApplyId(201U), ApplyGeneration(1U)};
    // No lifecycle plan or child authority is constructed.
    core::CellAdmissionClaimIntentDTO claim_intent{
        core::CellAdmissionClaimKindDTO::kManagerApply, core::ApplicationRole::kManager,
        manager.apply_id, SceneRequestKey("configuration-request"),
        core::AdmissionGeneration(1U), {}, std::nullopt, std::nullopt};
    if (authoring) {
      claim_intent.claim_kind = core::CellAdmissionClaimKindDTO::kAuthoringMutation;
      claim_intent.owner_role = core::ApplicationRole::kAuthoringOwner;
      claim_intent.owner_transaction_id.reset();
      claim_intent.request_key = SceneRequestKey("composer-request");
    }
    Require(core::ValidateCellAdmissionClaimIntent(claim_intent).ok(), "test configuration claim must be genuine");
    const auto claim = cell_admission.AcquireCellAdmissionClaim(claim_intent);
    Require(claim.ok(), "test configuration claim must actually be held");
    const auto published = publication_reader.ReadActiveBundlePublication(ActiveBundlePublicationGeneration(2U));
    Require(published.ok(), "test configuration predecessor must actually be current");
    const core::ConfigurationFenceStatusQueryDTO query{
        manager, core::ConfigurationFenceId("configuration-fence"), core::ConfigurationFenceGeneration(1U),
        {}};
    fence.Configure(query, published.value());
    return {{manager, claim.value(), query}, published.value(),
            owner->CurrentProjectionRevision(), owner->LogicalSnapshot()->identity(),
            same_product ? predecessor.bundle->value() : candidate.bundle->value(),
            same_product ? predecessor_bindings : candidate_bindings};
  }

  ActiveBundlePublicationReceiptDTO Publication(
      const QualifiedSceneConfigurationAdoptionRequest &request) const {
    const auto &manager = request.authority.owner;
    const ProjectionRevisionDTO target{request.candidate_retained_bundle.retained_bundle_id,
        request.candidate_retained_bundle.cell_graph.graph_revision, request.expected_snapshot_identity.revision};
    return {{NamedApplyPublicationTransactionDTO{manager.apply_id, manager.apply_generation},
             target, target.retained_bundle_id, target.graph_revision,
             request.expected_predecessor_publication.publication_generation, Digest('7')},
            ActiveBundlePublicationGeneration(3U), Digest('8')};
  }

  void PrepareCandidateClients() {
    auto clients = ProjectionClients(predecessor_projections);
    const auto candidates = ProjectionClients(candidate_projections);
    clients.insert(clients.end(), candidates.begin(), candidates.end());
    resolver.ReplaceWith(std::move(clients));
  }

  void QualifyCandidate(const QualifiedSceneConfigurationAdoptionRequest &request) {
    const ProjectionRevisionDTO target{request.candidate_retained_bundle.retained_bundle_id,
                                       request.candidate_retained_bundle.cell_graph.graph_revision,
                                       request.expected_snapshot_identity.revision};
    ResetProjections(candidate_projections, target, FakePlannerSceneProjection::Mode::kCommit);
    // Changed predecessor engines may retire; adoption must not query them.
    resolver.ReplaceWith(ProjectionClients(candidate_projections));
  }

  core::ProjectionMaintenanceInitializationStatusDTO Begin(
      const QualifiedSceneConfigurationAdoptionRequest &request) {
    return owner->BeginOriginalManagerConfigurationAdoption(request, fence);
  }

  core::ProjectionMaintenanceInitializationStatusDTO Complete(
      const QualifiedSceneConfigurationAdoptionRequest &request,
      const ActiveBundlePublicationReceiptDTO &publication) {
    return owner->CompleteOriginalManagerConfigurationAdoption(request, publication, fence);
  }

  void ExpectNoCandidateEffects(std::size_t expected_rebuilds = 0U,
                                bool same_product = false) const {
    const auto &projections = same_product ? predecessor_projections : candidate_projections;
    for (const auto &projection : projections) {
      EXPECT_EQ(projection->calls(), 0U);
      EXPECT_EQ(projection->rebuild_calls(), expected_rebuilds);
      EXPECT_EQ(projection->retained_lease_count(), 0U);
      EXPECT_EQ(projection->policy_projection_calls(), 0U);
    }
  }
};

// OD-372: the arm holds an object on a frame of a tool mounted on it; the
// attachment keeps the arm as holder and the tool's frame as carrier.
TEST(CanonicalEmptySceneOwner, AttachOnAMountedToolFrameKeepsTheArmAsHolder) {
  SceneAdoptionFixture fixture;
  auto active = fixture.owner->CurrentProjectionRevision();
  const auto added = fixture.owner->Commit(AddBoxRequest(active, fixture.admission.cell_id(), "mounted-add"));
  ASSERT_TRUE(added.success);
  ASSERT_TRUE(added.committed_projection_revision.has_value());
  active = *added.committed_projection_revision;
  for (auto &projection : fixture.predecessor_projections)
    projection->SetFrameResolution(core::Pose3DTO{}, {});
  core::AttachCarriedObjectRequestDTO operation{
      core::CarriedObjectAttachmentId("attachment.1"), core::SceneResourceId("scene-box"),
      core::ControllableComponentId("component.1"), core::CommandTargetId("arm"),
      core::FrameRoleId("holder_rod"), {core::CollisionBodyRoleId("holder_rod")}, {}};
  operation.carrier_component_id = core::ControllableComponentId("tube_holder");
  core::SceneTransactionRequestDTO attach{
      SceneRequestContext("mounted-attach", active), {}, active, operation, std::nullopt};
  attach.expected_admission_generation = core::AdmissionGeneration(1U);
  attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U), core::CellAdmissionClaimGeneration(9U),
      "attach", core::NodeAttemptNumber(1U), "scene.carried_attachment"};
  const auto attached = fixture.owner->Commit(attach);
  ASSERT_TRUE(attached.success) << (attached.error.has_value() ? attached.error->message : "missing error");
  const auto snapshot = fixture.owner->LogicalSnapshot();
  ASSERT_EQ(snapshot->carried_object_attachments.size(), 1U);
  const auto &attachment = snapshot->carried_object_attachments.front();
  EXPECT_EQ(attachment.controllable_component_id, core::ControllableComponentId("component.1"));
  EXPECT_EQ(attachment.command_target_id, core::CommandTargetId("arm"));
  const auto *carrier = std::get_if<core::ComponentSceneFrameRefDTO>(&attachment.carrier_frame_ref);
  ASSERT_NE(carrier, nullptr);
  EXPECT_EQ(carrier->controllable_component_id, core::ControllableComponentId("tube_holder"));
  EXPECT_EQ(carrier->frame_role_id, core::FrameRoleId("holder_rod"));
  EXPECT_EQ(attachment.touch_body_roles,
            (std::vector<core::CollisionBodyRoleId>{core::CollisionBodyRoleId("holder_rod")}));
}

void VerifyAdoptionPreservesObjectsAttachmentsAndLogicalIdentity(
    bool stage_candidate = false) {
  SceneAdoptionFixture fixture;
  auto active = fixture.owner->CurrentProjectionRevision();
  const auto added = fixture.owner->Commit(AddBoxRequest(active, fixture.admission.cell_id(), "adoption-add"));
  ASSERT_TRUE(added.success);
  ASSERT_TRUE(added.committed_projection_revision.has_value());
  active = *added.committed_projection_revision;
  const core::ComponentAttachmentRevision structural_revision("component-attachment-revision-1");
  for (auto &projection : fixture.predecessor_projections) {
    projection->SetFrameResolution(core::Pose3DTO{}, {structural_revision});
    projection->SetResolvedFrameRoleOverride(core::FrameRoleId("asset_tool_frame"));
  }
  core::SceneTransactionRequestDTO attach{
      SceneRequestContext("adoption-attach", active), {}, active,
      core::AttachCarriedObjectRequestDTO{
          core::CarriedObjectAttachmentId("attachment.1"), core::SceneResourceId("scene-box"),
          core::ControllableComponentId("component.1"), core::CommandTargetId("arm"),
          core::FrameRoleId("tool_frame"), {core::CollisionBodyRoleId("finger")}, {structural_revision}},
      std::nullopt};
  attach.expected_admission_generation = core::AdmissionGeneration(1U);
  attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U), core::CellAdmissionClaimGeneration(9U),
      "attach", core::NodeAttemptNumber(1U), "scene.carried_attachment"};
  const auto attached = fixture.owner->Commit(attach);
  ASSERT_TRUE(attached.success) << (attached.error.has_value() ? attached.error->message : "missing error");
  ASSERT_TRUE(attached.committed_projection_revision.has_value());
  active = *attached.committed_projection_revision;
  const auto snapshot = fixture.owner->LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), 1U);
  ASSERT_EQ(snapshot->carried_object_attachments.size(), 1U);
  const auto snapshot_bytes = EncodeLogicalSceneSnapshot(*snapshot);
  const auto integrity_before = fixture.owner->VerifyIntegrity();
  const auto request = fixture.CaptureConfigurationRequest();
  if (stage_candidate) {
    for (auto &projection : fixture.predecessor_projections) {
      projection->ObserveRevisionQueries();
    }
    for (auto &projection : fixture.candidate_projections) {
      projection->ObserveRevisionQueries();
    }
    fixture.PrepareCandidateClients();
    ASSERT_GE(request.execution_host_bindings.size(), 2U);
    const auto staged = fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, fixture.fence.OriginalAttemptQuery());
    ASSERT_EQ(staged.size(), request.execution_host_bindings.size());
    const ProjectionRevisionDTO target{
        request.candidate_retained_bundle.retained_bundle_id,
        request.candidate_retained_bundle.cell_graph.graph_revision, snapshot->revision};
    for (std::size_t index = 0U; index < staged.size(); ++index) {
      EXPECT_EQ(staged[index].execution_host_binding, request.execution_host_bindings[index]);
      EXPECT_TRUE(staged[index].healthy_for_target(target));
      EXPECT_EQ(fixture.candidate_projections[index]->last_health_revision(), std::optional{target});
      const auto &rebuilds = fixture.candidate_projections[index]->rebuild_requests();
      ASSERT_EQ(rebuilds.size(), 1U);
      EXPECT_EQ(EncodeLogicalSceneSnapshot(*rebuilds.front().logical_scene), snapshot_bytes);
      EXPECT_FALSE(rebuilds.front().maintenance_successor.has_value());
      EXPECT_FALSE(rebuilds.front().maintenance_predecessor.has_value());
    }
    EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
        {request.authority.owner.apply_id,
         request.authority.owner.apply_generation}).has_value());
    EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), active);
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot_bytes);
    const auto public_health = fixture.owner->ActiveProjectionHealth();
    ASSERT_EQ(public_health.native_projection_health.size(), fixture.predecessor_bindings.size());
    EXPECT_TRUE(public_health.healthy_for_target(active));
    for (const auto &projection : fixture.predecessor_projections) {
      EXPECT_EQ(projection->last_health_revision(), std::optional{active});
    }
    for (const auto &row : public_health.native_projection_health) {
      EXPECT_TRUE(row.healthy_for_target(active));
      EXPECT_TRUE(std::find(request.execution_host_bindings.begin(),
                            request.execution_host_bindings.end(), row.execution_host_binding) ==
                  request.execution_host_bindings.end());
    }
    // Even a locally valid private-candidate preview cannot select staged
    // projections through the ordinary current Scene API.
    auto private_preview_request = attach;
    private_preview_request.expected_projection_revision = target;
    private_preview_request.request_context.request_key = SceneRequestKey("private-candidate-preview");
    const core::SceneFrameResolutionReceiptDTO candidate_frame{
        core::ResolvedComponentModelFrameDTO{
            core::ControllableComponentId("component.1"), core::ModelInstanceId("model.1"),
            core::FrameRoleId("asset_tool_frame"), "compiled_asset_tool_frame",
            core::ModelRevision("model-revision-1")},
        core::Pose3DTO{}, {structural_revision},
        core::ProjectionLeaseBindingDTO{target, core::ProjectionLeaseId("private-preview-frame")},
        Digest('8'), Digest('9')};
    ASSERT_TRUE(candidate_frame.valid());
    const auto preview = fixture.owner->PreviewResolvedSceneCandidate(
        {private_preview_request, candidate_frame});
    ASSERT_FALSE(preview.ok());
    EXPECT_EQ(preview.error().code, "stale_revision");
    EXPECT_EQ(preview.error().details.at("reason"), "scene_candidate_projection_stale");
    const auto head = fixture.publication_reader.ReadActiveBundlePublication(
        request.expected_predecessor_publication.publication_generation);
    ASSERT_TRUE(head.ok());
    EXPECT_EQ(head.value(), request.expected_predecessor_publication);
    const auto observed = fixture.owner->ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, fixture.fence, fixture.fence.OriginalAttemptQuery());
    ASSERT_EQ(observed.size(), staged.size());
    for (const auto &row : observed) {
      EXPECT_TRUE(row.healthy_for_target(target));
    }
  } else {
    fixture.QualifyCandidate(request);
  }
  const auto ready = fixture.Begin(request);
  ASSERT_TRUE(ready.ready_for_publication());
  EXPECT_EQ(ready.target_projection_revision.scene_revision, snapshot->revision);
  EXPECT_EQ(ready.authoritative_snapshot_identity, snapshot->identity());
  EXPECT_FALSE(fixture.owner->ReadAccessStatus().scene_writes_open);
  const auto during = fixture.owner->ReadActiveSnapshot();
  ASSERT_TRUE(during.has_value());
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*during->shared()), snapshot_bytes);
  EXPECT_EQ(fixture.Begin(request), ready);
  fixture.ExpectNoCandidateEffects(stage_candidate ? 1U : 0U);
  const auto publication = fixture.Publication(request);
  fixture.publication_reader.SetCurrent(publication);
  const auto completed = fixture.Complete(request, publication);
  EXPECT_EQ(completed.state, core::ProjectionMaintenanceInitializationState::kCompleted);
  EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), ready.target_projection_revision);
  if (stage_candidate) {
    EXPECT_TRUE(fixture.owner->ActiveProjectionHealth().healthy_for_target(ready.target_projection_revision));
    for (const auto &projection : fixture.candidate_projections) {
      EXPECT_EQ(projection->last_health_revision(), std::optional{ready.target_projection_revision});
    }
  }
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot_bytes);
  const auto integrity_after = fixture.owner->VerifyIntegrity();
  EXPECT_EQ(integrity_after.transaction_intent_count, integrity_before.transaction_intent_count);
  EXPECT_EQ(integrity_after.owner_event_count, integrity_before.owner_event_count);
  EXPECT_EQ(integrity_after.projection_result_count, integrity_before.projection_result_count);
  fixture.ExpectNoCandidateEffects(stage_candidate ? 1U : 0U);
  // Ordinary completed replay remains evidence-only, including same-Scene rows.
  EXPECT_EQ(fixture.owner->CompleteMaintenanceInitialization(
                {request.authority.owner.apply_id, request.authority.owner.apply_generation,
                 ready.target_projection_revision, ready.authoritative_snapshot_identity, publication}), completed);
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot_bytes);
  EXPECT_EQ(fixture.owner->ReadMaintenanceInitialization(
                {request.authority.owner.apply_id, request.authority.owner.apply_generation}),
            std::optional{completed});
}

TEST(CanonicalEmptySceneOwner,
     ConfigurationAdoptionPreservesObjectsAttachmentsAndLogicalIdentity) {
  VerifyAdoptionPreservesObjectsAttachmentsAndLogicalIdentity();
}

// Apply binds identity and structure, never streamed state: pose-only commits
// after the request and after staging do not fail adoption, which adopts the
// latest Scene. The request's revision stays evidence of its moment.
TEST(CanonicalEmptySceneOwner,
     PoseOnlyCommitsBetweenApplyRequestAndAdoptionDoNotFailApply) {
  SceneAdoptionFixture fixture;
  ASSERT_TRUE(fixture.owner
                  ->Commit(AddBoxRequest(fixture.owner->CurrentProjectionRevision(),
                                         fixture.admission.cell_id(), "stream-add"))
                  .success);
  std::uint64_t sequence = 0U;
  const auto stream_pose = [&](const double x_m) {
    auto pose = fixture.owner->GetObjectPose(core::SceneResourceId("scene-box"));
    ASSERT_TRUE(pose.ok());
    pose.value().position_m.x_m = x_m;
    ++sequence;
    const core::SceneTransactionRequestDTO update{
        core::PublicRequestContextDTO{
            SceneRequestKey("stream-pose-" + std::to_string(sequence)), "scene-owner-test"},
        {},
        fixture.owner->CurrentProjectionRevision(),
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId("scene-box"), pose.value(),
            core::CellSceneFrameRefDTO{fixture.admission.cell_id()},
            StreamedPoseEvidence(), false},
        std::nullopt};
    const auto committed = fixture.owner->Commit(update);
    ASSERT_TRUE(committed.success)
        << (committed.error.has_value() ? committed.error->message : "missing error");
  };

  const auto request = fixture.CaptureConfigurationRequest();
  ASSERT_NO_FATAL_FAILURE(stream_pose(0.30));  // after the Apply request
  const auto at_stage = fixture.owner->CurrentProjectionRevision();
  ASSERT_NE(at_stage.scene_revision, request.expected_snapshot_identity.revision);
  fixture.PrepareCandidateClients();
  const auto staged = fixture.owner->StageOriginalManagerConfigurationAdoption(
      request, fixture.fence, fixture.fence.OriginalAttemptQuery());
  const ProjectionRevisionDTO staged_target{
      request.candidate_retained_bundle.retained_bundle_id,
      request.candidate_retained_bundle.cell_graph.graph_revision, at_stage.scene_revision};
  ASSERT_EQ(staged.size(), request.execution_host_bindings.size());
  for (const auto &row : staged) {
    EXPECT_TRUE(row.healthy_for_target(staged_target));
  }

  ASSERT_NO_FATAL_FAILURE(stream_pose(0.35));  // after staging, before adoption
  const auto latest = fixture.owner->LogicalSnapshot();
  const auto latest_bytes = EncodeLogicalSceneSnapshot(*latest);
  const auto ready = fixture.Begin(request);
  ASSERT_TRUE(ready.ready_for_publication());
  EXPECT_EQ(ready.target_projection_revision.scene_revision, latest->revision);
  EXPECT_EQ(ready.authoritative_snapshot_identity, latest->identity());
  EXPECT_FALSE(fixture.owner->ReadAccessStatus().scene_writes_open);
  // Candidates staged at the earlier revision moved to the adopted Scene.
  for (const auto &projection : fixture.candidate_projections) {
    ASSERT_EQ(projection->rebuild_calls(), 2U);
    const auto &successor = projection->rebuild_requests().back();
    EXPECT_EQ(successor.target_projection_revision, ready.target_projection_revision);
    EXPECT_EQ(successor.maintenance_predecessor, std::optional{staged_target});
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*successor.logical_scene), latest_bytes);
  }

  // Publication carries the request's revision as evidence only.
  const auto publication = fixture.Publication(request);
  fixture.publication_reader.SetCurrent(publication);
  const auto completed = fixture.Complete(request, publication);
  EXPECT_EQ(completed.state, core::ProjectionMaintenanceInitializationState::kCompleted);
  EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), ready.target_projection_revision);
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), latest_bytes);
  ASSERT_NO_FATAL_FAILURE(stream_pose(0.40));  // streaming continues on the new Deployment
}

TEST(CanonicalEmptySceneOwner,
     QualifiedAdoptionRefusesStaleInputsAndUnheldAuthorityBeforeCapture) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  auto altered = request;
  altered.authority.held_cell_claim.claim_generation = core::CellAdmissionClaimGeneration(99U);
  EXPECT_THROW(fixture.Begin(altered), LogicalSceneOwnerConflictError);
  altered = request;
  altered.authority.held_fence_query.configuration_fence_id = core::ConfigurationFenceId("other-fence");
  EXPECT_THROW(fixture.Begin(altered), LogicalSceneOwnerConflictError);
  altered = request;
  altered.expected_predecessor_publication.durable_public_pointer_content_digest = Digest('a');
  EXPECT_THROW(fixture.Begin(altered), LogicalSceneOwnerConflictError);
  altered = request;
  NameForeignScene(altered);
  EXPECT_THROW(fixture.Begin(altered), LogicalSceneOwnerConflictError);
  altered = request;
  altered.candidate_retained_bundle.cell_graph.graph_revision = core::GraphRevision("forged-graph");
  EXPECT_THROW(fixture.Begin(altered), LogicalSceneOwnerConflictError);
  fixture.cell_admission.ProveHeldClaimHadNoEffect();
  EXPECT_THROW(fixture.Begin(request), LogicalSceneOwnerConflictError);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id, request.authority.owner.apply_generation}).has_value());
  EXPECT_TRUE(fixture.owner->ReadAccessStatus().scene_writes_open);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     QualifiedAdoptionRequiresExactCompleteAlreadyHealthyBindings) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  auto altered = request;
  altered.execution_host_bindings.pop_back();
  EXPECT_THROW(fixture.Begin(altered), LogicalSceneOwnerConflictError);
  ASSERT_GE(request.execution_host_bindings.size(), 2U);
  altered = request;
  altered.execution_host_bindings.back() = altered.execution_host_bindings.front();
  altered.execution_host_bindings.back().runtime_epoch = core::RuntimeEpoch("zz-duplicate-epoch");
  std::sort(altered.execution_host_bindings.begin(), altered.execution_host_bindings.end(), core::CanonicalProjectionNativeBindingLess);
  EXPECT_THROW(fixture.Begin(altered), LogicalSceneOwnerConflictError);
  fixture.candidate_projections.front()->Reset(request.expected_active_projection_revision, FakePlannerSceneProjection::Mode::kCommit);
  EXPECT_THROW(fixture.Begin(request), LogicalSceneOwnerConflictError);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id, request.authority.owner.apply_generation}).has_value());
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     QualifiedAdoptionRechecksHeldClaimAfterCandidateHealth) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  fixture.cell_admission.RefuseApplyClaimLookupAfter(1U);
  EXPECT_THROW(fixture.Begin(request), LogicalSceneOwnerConflictError);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id, request.authority.owner.apply_generation}).has_value());
  EXPECT_TRUE(fixture.owner->ReadAccessStatus().scene_writes_open);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     PendingPrivateAdoptionCannotCompleteThroughOrdinaryPortOrChangedRequest) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  const auto ready = fixture.Begin(request);
  const auto publication = fixture.Publication(request);
  fixture.publication_reader.SetCurrent(publication);
  const core::ProjectionMaintenanceCompletionDTO ordinary{
      request.authority.owner.apply_id, request.authority.owner.apply_generation,
      ready.target_projection_revision, ready.authoritative_snapshot_identity, publication};
  EXPECT_THROW(fixture.owner->CompleteMaintenanceInitialization(ordinary), LogicalSceneOwnerConflictError);
  auto changed = request;
  changed.expected_predecessor_publication.durable_public_pointer_content_digest = Digest('b');
  fixture.publication_reader.SetCurrent(changed.expected_predecessor_publication);
  EXPECT_THROW(fixture.Begin(changed), LogicalSceneOwnerConflictError);
  fixture.publication_reader.SetCurrent(publication);
  EXPECT_THROW(fixture.Complete(changed, publication), LogicalSceneOwnerConflictError);
  fixture.cell_admission.RefuseApplyClaimLookupAfter(0U);
  EXPECT_THROW(fixture.Complete(request, publication), LogicalSceneOwnerConflictError);
  EXPECT_EQ(fixture.owner->ReadMaintenanceInitialization(
                {ordinary.apply_id, ordinary.apply_generation}), std::optional{ready});
  EXPECT_FALSE(fixture.owner->ReadAccessStatus().scene_writes_open);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     QualifiedAdoptionCompletionRechecksPublicationHealthAndClaim) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  const auto ready = fixture.Begin(request);
  const auto publication = fixture.Publication(request);
  // A supplied receipt is not evidence that publication actually happened.
  EXPECT_THROW(fixture.Complete(request, publication), LogicalSceneOwnerConflictError);
  fixture.publication_reader.SetCurrent(publication);
  fixture.candidate_projections.front()->Reset(request.expected_active_projection_revision, FakePlannerSceneProjection::Mode::kCommit);
  EXPECT_THROW(fixture.Complete(request, publication), LogicalSceneOwnerConflictError);
  fixture.QualifyCandidate(request);
  fixture.cell_admission.RefuseApplyClaimLookupAfter(1U);
  EXPECT_THROW(fixture.Complete(request, publication), LogicalSceneOwnerConflictError);
  EXPECT_EQ(fixture.owner->ReadMaintenanceInitialization(
                {request.authority.owner.apply_id, request.authority.owner.apply_generation}),
            std::optional{ready});
  EXPECT_FALSE(fixture.owner->ReadAccessStatus().scene_writes_open);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     MissingClaimDependencyCannotAdmitQualifiedAdoption) {
  SceneAdoptionFixture fixture(false);
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  EXPECT_THROW(fixture.Begin(request), LogicalSceneOwnerConflictError);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id, request.authority.owner.apply_generation}).has_value());
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     ConfigurationAdoptionPreservesPopulatedSceneWithDirectOrStoredAuthoringClaim) {
  for (const bool same_product : {false, true}) {
    SCOPED_TRACE(same_product);
  for (const bool authoring : {false, true}) {
    SCOPED_TRACE(authoring);
    SceneAdoptionFixture fixture;
  auto active = fixture.owner->CurrentProjectionRevision();
  const auto added = fixture.owner->Commit(AddBoxRequest(active, fixture.admission.cell_id(), "adoption-add"));
  ASSERT_TRUE(added.success);
  ASSERT_TRUE(added.committed_projection_revision.has_value());
  active = *added.committed_projection_revision;
  const core::ComponentAttachmentRevision structural_revision("component-attachment-revision-1");
  for (auto &projection : fixture.predecessor_projections) {
    projection->SetFrameResolution(core::Pose3DTO{}, {structural_revision});
    projection->SetResolvedFrameRoleOverride(core::FrameRoleId("asset_tool_frame"));
  }
  core::SceneTransactionRequestDTO attach{
      SceneRequestContext("adoption-attach", active), {}, active,
      core::AttachCarriedObjectRequestDTO{
          core::CarriedObjectAttachmentId("attachment.1"), core::SceneResourceId("scene-box"),
          core::ControllableComponentId("component.1"), core::CommandTargetId("arm"),
          core::FrameRoleId("tool_frame"), {core::CollisionBodyRoleId("finger")}, {structural_revision}},
      std::nullopt};
  attach.expected_admission_generation = core::AdmissionGeneration(1U);
  attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U), core::CellAdmissionClaimGeneration(9U),
      "attach", core::NodeAttemptNumber(1U), "scene.carried_attachment"};
  const auto attached = fixture.owner->Commit(attach);
  ASSERT_TRUE(attached.success) << (attached.error.has_value() ? attached.error->message : "missing error");
  ASSERT_TRUE(attached.committed_projection_revision.has_value());
  active = *attached.committed_projection_revision;
  const auto snapshot = fixture.owner->LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), 1U);
  ASSERT_EQ(snapshot->carried_object_attachments.size(), 1U);
  const auto snapshot_bytes = EncodeLogicalSceneSnapshot(*snapshot);
    const auto request = fixture.CaptureConfigurationRequest(authoring, same_product);
    ASSERT_EQ(request.authority.held_cell_claim.intent.claim_kind,
              authoring ? core::CellAdmissionClaimKindDTO::kAuthoringMutation
                        : core::CellAdmissionClaimKindDTO::kManagerApply);
    // A Manager Apply claim carries the Apply id; an authoring claim carries none.
    if (authoring) {
      EXPECT_FALSE(request.authority.held_cell_claim.intent.owner_transaction_id.has_value());
    } else {
      EXPECT_EQ(request.authority.held_cell_claim.intent.owner_transaction_id,
                core::CellAdmissionOwnerTransactionId(request.authority.owner.apply_id));
    }
    if (same_product) {
      EXPECT_EQ(request.candidate_retained_bundle.retained_bundle_id,
                fixture.predecessor.bundle->value().retained_bundle_id);
      EXPECT_EQ(request.candidate_retained_bundle.cell_graph.graph_revision,
                fixture.predecessor.bundle->value().cell_graph.graph_revision);
      EXPECT_EQ(request.execution_host_bindings, fixture.predecessor_bindings);
    } else {
      fixture.PrepareCandidateClients();
    }
    const auto &projections = same_product ? fixture.predecessor_projections : fixture.candidate_projections;
    std::vector<std::size_t> calls_before;
    std::vector<std::size_t> rebuilds_before;
    std::vector<std::size_t> policy_calls_before;
    for (const auto &projection : projections) {
      calls_before.push_back(projection->calls());
      rebuilds_before.push_back(projection->rebuild_calls());
      policy_calls_before.push_back(projection->policy_projection_calls());
    }
    const auto &attempt = fixture.fence.OriginalAttemptQuery();
    const auto staged = fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, attempt);
    const auto observed = fixture.owner->ReadOriginalManagerConfigurationAdoptionProjectionHealth(
        request, fixture.fence, attempt);
    ASSERT_FALSE(staged.empty());
    ASSERT_EQ(staged.size(), request.execution_host_bindings.size());
    ASSERT_EQ(observed.size(), staged.size());
    const ProjectionRevisionDTO target{request.candidate_retained_bundle.retained_bundle_id,
        request.candidate_retained_bundle.cell_graph.graph_revision, request.expected_snapshot_identity.revision};
    for (const auto &row : staged) {
      EXPECT_TRUE(row.healthy_for_target(target));
    }
    for (const auto &row : observed) {
      EXPECT_TRUE(row.healthy_for_target(target));
    }
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot_bytes);
    EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), active);
    EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
        {request.authority.owner.apply_id, request.authority.owner.apply_generation}));
    const auto ready = fixture.owner->BeginOriginalManagerConfigurationAdoption(request, fixture.fence);
    ASSERT_TRUE(ready.ready_for_publication());
    if (same_product) {
      EXPECT_EQ(ready.target_projection_revision, request.expected_active_projection_revision);
    }
    EXPECT_EQ(ready.authoritative_snapshot_identity, snapshot->identity());
    const auto captured = fixture.owner->ReadActiveSnapshot();
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*captured->shared()), snapshot_bytes);
    const auto publication = fixture.Publication(request);
    ASSERT_TRUE(publication.request.expected_previous_publication_generation.has_value());
    EXPECT_EQ(*publication.request.expected_previous_publication_generation, ActiveBundlePublicationGeneration(2U));
    EXPECT_EQ(publication.publication_generation, ActiveBundlePublicationGeneration(3U));
    fixture.publication_reader.SetCurrent(publication);
    const auto completed = fixture.owner->CompleteOriginalManagerConfigurationAdoption(request, publication, fixture.fence);
    EXPECT_EQ(completed.state, core::ProjectionMaintenanceInitializationState::kCompleted);
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot_bytes);
    EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), ready.target_projection_revision);
    for (std::size_t index = 0U; index < projections.size(); ++index) {
      EXPECT_EQ(projections[index]->calls(), calls_before[index]);
      EXPECT_EQ(projections[index]->rebuild_calls(), rebuilds_before[index] + (same_product ? 0U : 1U));
      EXPECT_EQ(projections[index]->policy_projection_calls(), policy_calls_before[index]);
      EXPECT_EQ(projections[index]->retained_lease_count(), 0U);
    }
    if (same_product) {
      fixture.ExpectNoCandidateEffects();  // The unrelated changed-product clients were never selected.
    } else {
      fixture.ExpectNoCandidateEffects(1U);
    }
    const auto held = fixture.cell_admission.GetCellAdmissionRecoveryReceipt(
        {request.authority.held_cell_claim.intent, request.authority.held_cell_claim, std::nullopt});
    ASSERT_TRUE(held.ok());
    EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  }
  }
}

TEST(CanonicalEmptySceneOwner,
     RepeatedEmptyConfigurationAdoptionReplaysBeforeOrdinarySceneEdits) {
  for (const bool no_op_between_applies : {false, true}) {
    SCOPED_TRACE(no_op_between_applies);
    SceneAdoptionFixture fixture;
    const auto added = fixture.owner->Commit(AddBoxRequest(
        fixture.owner->CurrentProjectionRevision(), fixture.admission.cell_id(),
        "replay-add"));
    ASSERT_TRUE(added.success);
    std::uint64_t publication_generation = 2U;
    const auto adopt_empty = [&](const std::uint64_t apply_number) {
      const core::ApplyFenceOwnerDTO manager{ApplyId(apply_number), ApplyGeneration(1U)};
      const auto id = std::to_string(apply_number);
      const auto claim = fixture.cell_admission.AcquireCellAdmissionClaim(
          {core::CellAdmissionClaimKindDTO::kManagerApply,
           core::ApplicationRole::kManager,
           manager.apply_id, SceneRequestKey(id + ".request"),
           core::AdmissionGeneration(1U), {}, std::nullopt,
           std::nullopt});
      ASSERT_TRUE(claim.ok());
      const auto previous = fixture.publication_reader.ReadActiveBundlePublication(
          ActiveBundlePublicationGeneration(publication_generation));
      ASSERT_TRUE(previous.ok());
      const core::ConfigurationFenceStatusQueryDTO query{
          manager, core::ConfigurationFenceId(id + ".fence"),
          core::ConfigurationFenceGeneration(1U), {}};
      fixture.fence.Configure(query, previous.value());
      const QualifiedSceneConfigurationAdoptionRequest request{
          {manager, claim.value(), query}, previous.value(),
          fixture.owner->CurrentProjectionRevision(),
          fixture.owner->LogicalSnapshot()->identity(),
          fixture.admission.retained_bundle().value(), {}};
      static_cast<void>(fixture.owner->StageOriginalManagerConfigurationAdoption(
          request, fixture.fence, fixture.fence.OriginalAttemptQuery()));
      const auto ready = fixture.owner->BeginOriginalManagerConfigurationAdoption(
          request, fixture.fence);
      ASSERT_TRUE(ready.ready_for_publication());
      const ActiveBundlePublicationReceiptDTO publication{
          {NamedApplyPublicationTransactionDTO{manager.apply_id,
                                              manager.apply_generation},
           ready.target_projection_revision,
           ready.target_projection_revision.retained_bundle_id,
           ready.target_projection_revision.graph_revision,
           previous.value().publication_generation, Digest('7')},
          ActiveBundlePublicationGeneration(++publication_generation),
          Digest('8')};
      fixture.publication_reader.SetCurrent(publication);
      const auto completed =
          fixture.owner->CompleteOriginalManagerConfigurationAdoption(
              request, publication, fixture.fence);
      ASSERT_EQ(completed.state,
                core::ProjectionMaintenanceInitializationState::kCompleted);
      const auto &held = claim.value();
      ASSERT_TRUE(fixture.cell_admission.ReleaseCellAdmissionClaim(
          {held.intent.owner_role,
           held.intent.owner_transaction_id, held.intent.request_key,
           held.acquired_admission_generation,
           held.claim_generation,
           {core::CellAdmissionSettlementKindDTO::kOwnerCommit, Digest('b')}}).ok());
    };
    ASSERT_NO_FATAL_FAILURE(adopt_empty(301U));
    const auto empty_projection = fixture.owner->CurrentProjectionRevision();
    if (no_op_between_applies) {
      const auto pose = fixture.owner->GetObjectPose(core::SceneResourceId("scene-box"));
      ASSERT_TRUE(pose.ok());
      const core::SceneTransactionRequestDTO no_op{
          SceneRequestContext("replay-no-op", empty_projection), {},
          empty_projection,
          core::UpdateObjectPoseRequestDTO{
              core::SceneResourceId("scene-box"), pose.value(),
              core::CellSceneFrameRefDTO{fixture.admission.cell_id()},
              std::nullopt, false},
          core::AdmissionGeneration(1U)};
      const auto unchanged = fixture.owner->Commit(no_op);
      ASSERT_TRUE(unchanged.success);
      EXPECT_EQ(unchanged.update_status, core::SceneUpdateStatusDTO::kNoOp);
      EXPECT_EQ(unchanged.committed_projection_revision, empty_projection);
    }
    ASSERT_NO_FATAL_FAILURE(adopt_empty(302U));
    EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), empty_projection);
    const core::SceneTransactionRequestDTO remove{
        SceneRequestContext("replay-remove", empty_projection), {},
        empty_projection,
        core::RemoveObjectRequestDTO{core::SceneResourceId("scene-box")},
        core::AdmissionGeneration(1U)};
    const auto removed = fixture.owner->Commit(remove);
    ASSERT_TRUE(removed.success);
    ASSERT_TRUE(removed.committed_projection_revision.has_value());
    const auto final_projection = *removed.committed_projection_revision;
    EXPECT_TRUE(fixture.owner->LogicalSnapshot()->objects.empty());
    EXPECT_EQ(fixture.owner->VerifyIntegrity().transaction_intent_count,
              no_op_between_applies ? 3 : 2);
    EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), final_projection);
    EXPECT_TRUE(fixture.owner->LogicalSnapshot()->objects.empty());
    EXPECT_EQ(fixture.owner->LogicalSnapshot()->transaction_sequence,
              no_op_between_applies ? 3U : 2U);
    EXPECT_TRUE(fixture.owner->ReadAccessStatus().scene_writes_open);
    fixture.ExpectNoCandidateEffects();
  }
}

// Close (canonical-empty Apply) and a Load of the same product both
// preserve the Scene revision, so the composite returns to the exact prior
// head (A -> B -> A). Replay must still order both completions before the
// next committed edit instead of stranding them behind it.
TEST(CanonicalEmptySceneOwner,
     ConfigurationAdoptionChainBackToThePriorHeadReplaysBeforeTheNextEdit) {
  SceneAdoptionFixture fixture;
  const auto added = fixture.owner->Commit(AddBoxRequest(
      fixture.owner->CurrentProjectionRevision(), fixture.admission.cell_id(),
      "aba-add"));
  ASSERT_TRUE(added.success);
  const auto product_head = fixture.owner->CurrentProjectionRevision();
  std::uint64_t publication_generation = 2U;
  const auto adopt = [&](const std::uint64_t apply_number,
                         const core::RetainedBundleDTO &bundle,
                         const std::vector<ProjectionExecutionHostBindingDTO> &bindings) {
    const core::ApplyFenceOwnerDTO manager{ApplyId(apply_number), ApplyGeneration(1U)};
    const auto id = std::to_string(apply_number);
    const auto claim = fixture.cell_admission.AcquireCellAdmissionClaim(
        {core::CellAdmissionClaimKindDTO::kManagerApply,
         core::ApplicationRole::kManager,
         manager.apply_id, SceneRequestKey(id + ".request"),
         core::AdmissionGeneration(1U), {}, std::nullopt,
         std::nullopt});
    ASSERT_TRUE(claim.ok());
    const auto previous = fixture.publication_reader.ReadActiveBundlePublication(
        ActiveBundlePublicationGeneration(publication_generation));
    ASSERT_TRUE(previous.ok());
    const core::ConfigurationFenceStatusQueryDTO query{
        manager, core::ConfigurationFenceId(id + ".fence"),
        core::ConfigurationFenceGeneration(1U), {}};
    fixture.fence.Configure(query, previous.value());
    const QualifiedSceneConfigurationAdoptionRequest request{
        {manager, claim.value(), query}, previous.value(),
        fixture.owner->CurrentProjectionRevision(),
        fixture.owner->LogicalSnapshot()->identity(), bundle, bindings};
    static_cast<void>(fixture.owner->StageOriginalManagerConfigurationAdoption(
        request, fixture.fence, fixture.fence.OriginalAttemptQuery()));
    const auto ready = fixture.owner->BeginOriginalManagerConfigurationAdoption(
        request, fixture.fence);
    ASSERT_TRUE(ready.ready_for_publication());
    const ActiveBundlePublicationReceiptDTO publication{
        {NamedApplyPublicationTransactionDTO{manager.apply_id,
                                            manager.apply_generation},
         ready.target_projection_revision,
         ready.target_projection_revision.retained_bundle_id,
         ready.target_projection_revision.graph_revision,
         previous.value().publication_generation, Digest('7')},
        ActiveBundlePublicationGeneration(++publication_generation),
        Digest('8')};
    fixture.publication_reader.SetCurrent(publication);
    ASSERT_EQ(fixture.owner
                  ->CompleteOriginalManagerConfigurationAdoption(
                      request, publication, fixture.fence)
                  .state,
              core::ProjectionMaintenanceInitializationState::kCompleted);
    const auto &held = claim.value();
    ASSERT_TRUE(fixture.cell_admission.ReleaseCellAdmissionClaim(
        {held.intent.owner_role,
         held.intent.owner_transaction_id, held.intent.request_key,
         held.acquired_admission_generation,
         held.claim_generation,
         {core::CellAdmissionSettlementKindDTO::kOwnerCommit, Digest('b')}}).ok());
  };
  // Close: canonical-empty Apply. Load: the same product again.
  ASSERT_NO_FATAL_FAILURE(
      adopt(311U, fixture.admission.retained_bundle().value(), {}));
  EXPECT_NE(fixture.owner->CurrentProjectionRevision(), product_head);
  ASSERT_NO_FATAL_FAILURE(adopt(312U, fixture.predecessor.bundle->value(),
                                fixture.predecessor_bindings));
  ASSERT_EQ(fixture.owner->CurrentProjectionRevision(), product_head);

  // The first edit after the Load (live: the gripper attach).
  const core::SceneTransactionRequestDTO remove{
      SceneRequestContext("aba-remove", product_head), {}, product_head,
      core::RemoveObjectRequestDTO{core::SceneResourceId("scene-box")},
      core::AdmissionGeneration(1U)};
  const auto removed = fixture.owner->Commit(remove);
  ASSERT_TRUE(removed.success);
  ASSERT_TRUE(removed.committed_projection_revision.has_value());
  const auto final_projection = *removed.committed_projection_revision;
  // A full replay of the history (periodic reconciliation).
  EXPECT_NO_THROW(static_cast<void>(fixture.owner->VerifyIntegrity()));
  EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), final_projection);
  EXPECT_TRUE(fixture.owner->LogicalSnapshot()->objects.empty());
  EXPECT_EQ(fixture.owner->LogicalSnapshot()->transaction_sequence, 2U);
  EXPECT_TRUE(fixture.owner->ReadAccessStatus().scene_writes_open);
}

TEST(CanonicalEmptySceneOwner, ConfigurationAdoptionRefusesForeignClaimIntentFenceAndScene) {
  for (const bool same_product : {false, true}) {
    SCOPED_TRACE(same_product);
  for (unsigned scenario = 0U; scenario < 8U; ++scenario) {
    SCOPED_TRACE(scenario);
    SceneAdoptionFixture fixture;
    auto request = fixture.CaptureConfigurationRequest(false, same_product);
    if (!same_product) {
      fixture.PrepareCandidateClients();
    }
    const auto bytes = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
    switch (scenario) {
    case 0U: request.authority.held_cell_claim.intent.owner_transaction_id = ApplyId(999U); break;
    case 1U: request.authority.held_cell_claim.claim_generation = core::CellAdmissionClaimGeneration(99U); break;
    case 2U: request.authority.held_fence_query.configuration_fence_id = core::ConfigurationFenceId("foreign-fence"); break;
    case 3U: request.expected_predecessor_publication.durable_public_pointer_content_digest = Digest('a'); break;
    case 4U: NameForeignScene(request); break;
    case 5U: fixture.cell_admission.ProveHeldClaimHadNoEffect(); break;
    case 6U: request.candidate_retained_bundle.cell_graph.graph_revision = GraphRevision("foreign-candidate"); break;
    case 7U: request.execution_host_bindings.pop_back(); break;
    }
    const auto stage = [&] {
      return fixture.owner->StageOriginalManagerConfigurationAdoption(
          request, fixture.fence, fixture.fence.OriginalAttemptQuery());
    };
    const auto begin = [&] { return fixture.owner->BeginOriginalManagerConfigurationAdoption(request, fixture.fence); };
    if (scenario < 1U) {
      EXPECT_THROW(stage(), std::invalid_argument);
      EXPECT_THROW(begin(), std::invalid_argument);
    } else {
      EXPECT_THROW(stage(), LogicalSceneOwnerConflictError);
      EXPECT_THROW(begin(), LogicalSceneOwnerConflictError);
    }
    fixture.ExpectNoCandidateEffects(0U, same_product);
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), bytes);
    EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
        {request.authority.owner.apply_id, request.authority.owner.apply_generation}));
  }
  }
}

TEST(CanonicalEmptySceneOwner, ConfigurationAdoptionUncertaintyRetainsTruth) {
  for (const bool same_product : {false, true}) {
    SCOPED_TRACE(same_product);
  for (const bool completion_uncertainty : {false, true}) {
    SCOPED_TRACE(completion_uncertainty);
    SceneAdoptionFixture fixture;
    const auto request = fixture.CaptureConfigurationRequest(true, same_product);
    if (!same_product) {
      fixture.PrepareCandidateClients();
    }
    const auto &attempt = fixture.fence.OriginalAttemptQuery();
    ASSERT_FALSE(fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, attempt).empty());
    const auto publication = fixture.Publication(request);
    if (completion_uncertainty) {
      ASSERT_TRUE(fixture.owner->BeginOriginalManagerConfigurationAdoption(request, fixture.fence).ready_for_publication());
      fixture.publication_reader.SetCurrent(publication);
      fixture.fence.RefuseRemoteQueryAfter(2U);
      EXPECT_THROW(fixture.owner->CompleteOriginalManagerConfigurationAdoption(request, publication, fixture.fence), std::runtime_error);
    } else {
      fixture.fence.RefuseRemoteQueryAfter(2U);
      EXPECT_THROW(fixture.owner->BeginOriginalManagerConfigurationAdoption(request, fixture.fence), std::runtime_error);
    }
    const core::ProjectionMaintenanceInitializationQueryDTO query{
        request.authority.owner.apply_id, request.authority.owner.apply_generation};
    const auto actual = fixture.owner->ReadMaintenanceInitialization(query);
    ASSERT_TRUE(actual.has_value());
    EXPECT_EQ(actual->state, completion_uncertainty ? core::ProjectionMaintenanceInitializationState::kCompleted
                                                  : core::ProjectionMaintenanceInitializationState::kInitialized);
    fixture.ExpectNoCandidateEffects(same_product ? 0U : 1U, same_product);
  }
  }
}


TEST(CanonicalEmptySceneOwner, EqualConfigurationAdoptionRequiresExactCurrentProjectionHealth) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest(false, true);
  const auto bytes = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
  ASSERT_FALSE(fixture.owner->StageOriginalManagerConfigurationAdoption(
      request, fixture.fence, fixture.fence.OriginalAttemptQuery()).empty());
  for (const auto &projection : fixture.predecessor_projections) {
    EXPECT_EQ(projection->rebuild_calls(), 0U);
  }
  // A projection on another Scene is not healthy for this one.
  auto stale = request.expected_active_projection_revision;
  stale.scene_revision.scene_id = core::SceneId(
      Digest(stale.scene_revision.scene_id.value() == Digest('e').value() ? 'f' : 'e').value());
  fixture.predecessor_projections.front()->Reset(stale, FakePlannerSceneProjection::Mode::kCommit);
  std::vector<std::size_t> rebuilds_before;
  for (const auto &projection : fixture.predecessor_projections) {
    rebuilds_before.push_back(projection->rebuild_calls());
  }
  EXPECT_THROW(fixture.owner->BeginOriginalManagerConfigurationAdoption(request, fixture.fence),
               LogicalSceneOwnerConflictError);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id, request.authority.owner.apply_generation}));
  const auto stored = fixture.owner->ReadActiveSnapshot();
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*stored->shared()), bytes);
  for (std::size_t index = 0U; index < fixture.predecessor_projections.size(); ++index) {
    EXPECT_EQ(fixture.predecessor_projections[index]->rebuild_calls(), rebuilds_before[index]);
    EXPECT_EQ(fixture.predecessor_projections[index]->calls(), 0U);
    EXPECT_EQ(fixture.predecessor_projections[index]->retained_lease_count(), 0U);
  }
  fixture.ExpectNoCandidateEffects();
  const auto held = fixture.cell_admission.GetCellAdmissionRecoveryReceipt(
      {request.authority.held_cell_claim.intent, request.authority.held_cell_claim, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerStagingUsesHeldPresenceBeforeInstalledConfigurationStatus) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  const auto snapshot = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
  fixture.PrepareCandidateClients();
  fixture.fence.RefuseFullStatusQueries();
  const auto &attempt = fixture.fence.OriginalAttemptQuery();
  ASSERT_TRUE(fixture.fence.Presence().matches(attempt));
  ASSERT_NE(attempt.persisted_intent.owner_request_context.request_key,
            request.authority.held_cell_claim.intent.request_key);
  const auto staged = fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, attempt);
  const auto observed = fixture.owner->ReadOriginalManagerConfigurationAdoptionProjectionHealth(
      request, fixture.fence, attempt);
  const ProjectionRevisionDTO target{request.candidate_retained_bundle.retained_bundle_id,
      request.candidate_retained_bundle.cell_graph.graph_revision,
      request.expected_snapshot_identity.revision};
  ASSERT_EQ(staged.size(), request.execution_host_bindings.size());
  ASSERT_EQ(observed.size(), staged.size());
  for (const auto &row : observed) {
    EXPECT_TRUE(row.healthy_for_target(target));
  }
  fixture.ExpectNoCandidateEffects(1U);
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot);
  EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), request.expected_active_projection_revision);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id,
       request.authority.owner.apply_generation}).has_value());
  // Begin still requires its full installed status; held presence cannot replace it.
  EXPECT_THROW(fixture.Begin(request), std::runtime_error);
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerStagingRefusesForeignMissingReleasedAndStaleHeldPresence) {
  for (unsigned scenario = 0U; scenario < 10U; ++scenario) {
    SCOPED_TRACE(scenario);
    SceneAdoptionFixture fixture;
    auto request = fixture.CaptureConfigurationRequest();
    const auto snapshot = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
    fixture.PrepareCandidateClients();
    auto attempt = fixture.fence.OriginalAttemptQuery();
    auto presence = fixture.fence.Presence();
    switch (scenario) {
    case 0U:
      attempt.persisted_attempt_digest = Digest('a');
      break;
    case 1U:
      attempt.persisted_intent.reason = "foreign complete intent";
      break;
    case 2U:
      attempt.persisted_intent.owner_transaction.apply_id = ApplyId(299U);
      break;
    case 3U:
      attempt.persisted_intent.expected_graph_revision = core::GraphRevision("foreign-graph");
      break;
    case 4U:
      presence.admission_fenced = false;
      break;
    case 5U:
      presence.state = core::ApplyFenceAcquireAttemptReconcileState::kNotSeen;
      presence.accepted_fence.reset();
      presence.admission_fenced = false;
      break;
    case 6U:
      // The actual owner reports a released request as conflicting, never held.
      presence.state = core::ApplyFenceAcquireAttemptReconcileState::kConflicting;
      presence.accepted_fence.reset();
      presence.required_compensation_or_operator_direction = "released fence remains excluded";
      presence.error = ErrorDTO{"configuration_fence_request_conflict", "request is already released", {}};
      break;
    case 7U:
      presence.execution_host_epoch.publication_generation = ActiveBundlePublicationGeneration(99U);
      break;
    case 8U:
      presence.accepted_fence->configuration_fence_id = core::ConfigurationFenceId("foreign-fence");
      break;
    case 9U:
      request.expected_predecessor_publication.durable_public_pointer_content_digest = Digest('a');
      break;
    }
    fixture.fence.SetPresence(presence);
    EXPECT_THROW(fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, attempt),
                 LogicalSceneOwnerConflictError);
    EXPECT_THROW(fixture.owner->ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, fixture.fence, attempt),
                 LogicalSceneOwnerConflictError);
    fixture.ExpectNoCandidateEffects();
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot);
    EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
        {request.authority.owner.apply_id,
         request.authority.owner.apply_generation}).has_value());
  }
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerStagingPreservesPopulatedSceneAndKeepsPublicProjectionOnPredecessor) {
  VerifyAdoptionPreservesObjectsAttachmentsAndLogicalIdentity(true);
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerStagingPartialFailureKeepsOldSceneAndProjectionTruth) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  const auto snapshot = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
  const auto old_revision = fixture.owner->CurrentProjectionRevision();
  const auto integrity = fixture.owner->VerifyIntegrity();
  for (auto &projection : fixture.predecessor_projections) {
    projection->ObserveRevisionQueries();
  }
  for (auto &projection : fixture.candidate_projections) {
    projection->ObserveRevisionQueries();
  }
  fixture.PrepareCandidateClients();
  ASSERT_GE(fixture.candidate_projections.size(), 2U);
  fixture.candidate_projections.back()->RejectNextRebuildWithoutEffect();
  EXPECT_THROW(fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, fixture.fence.OriginalAttemptQuery()),
               LogicalSceneOwnerConflictError);
  // A prior binding really committed; no cleanup/rollback/retry is fabricated.
  const ProjectionRevisionDTO target{
      request.candidate_retained_bundle.retained_bundle_id,
      request.candidate_retained_bundle.cell_graph.graph_revision,
      request.expected_snapshot_identity.revision};
  EXPECT_TRUE(fixture.candidate_projections.front()->ProjectionHealth().healthy_for_target(target));
  EXPECT_FALSE(fixture.candidate_projections.back()->ProjectionHealth().healthy_for_target(target));
  for (const auto &projection : fixture.candidate_projections) {
    EXPECT_EQ(projection->rebuild_calls(), 1U);
    EXPECT_EQ(projection->calls(), 0U);
  }
  EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), old_revision);
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot);
  const auto public_health = fixture.owner->ActiveProjectionHealth();
  EXPECT_TRUE(public_health.healthy_for_target(old_revision));
  for (const auto &projection : fixture.predecessor_projections) {
    EXPECT_EQ(projection->last_health_revision(), std::optional{old_revision});
  }
  for (const auto &projection : fixture.candidate_projections) {
    EXPECT_EQ(projection->last_health_revision(), std::optional{target});
  }
  for (const auto &row : public_health.native_projection_health) {
    EXPECT_TRUE(row.healthy_for_target(old_revision));
  }
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id,
       request.authority.owner.apply_generation}).has_value());
  const auto after = fixture.owner->VerifyIntegrity();
  EXPECT_EQ(after.transaction_intent_count, integrity.transaction_intent_count);
  EXPECT_EQ(after.owner_event_count, integrity.owner_event_count);
  EXPECT_EQ(after.projection_result_count, integrity.projection_result_count);
  const auto held = fixture.cell_admission.GetCellAdmissionRecoveryReceipt(
      {request.authority.held_cell_claim.intent, request.authority.held_cell_claim, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerStagingObservesAcceptedWorkWithoutRepeatingSceneEffects) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  for (auto &projection : fixture.predecessor_projections) {
    projection->ObserveRevisionQueries();
  }
  for (auto &projection : fixture.candidate_projections) {
    projection->ObserveRevisionQueries();
  }
  fixture.PrepareCandidateClients();
  fixture.candidate_projections.front()->AcceptNextRebuildAs(request.execution_host_bindings.front());
  const auto staged = fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, fixture.fence.OriginalAttemptQuery());
  ASSERT_EQ(staged.size(), request.execution_host_bindings.size());
  EXPECT_EQ(staged.front().state, core::ProjectionHealthState::kRebuilding);
  EXPECT_THROW(fixture.Begin(request), LogicalSceneOwnerConflictError);
  const auto pending = fixture.owner->ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, fixture.fence, fixture.fence.OriginalAttemptQuery());
  ASSERT_EQ(pending.size(), staged.size());
  const ProjectionRevisionDTO target{
      request.candidate_retained_bundle.retained_bundle_id,
      request.candidate_retained_bundle.cell_graph.graph_revision,
      request.expected_snapshot_identity.revision};
  EXPECT_FALSE(pending.front().healthy_for_target(target));
  for (const auto &projection : fixture.candidate_projections) {
    EXPECT_EQ(projection->last_health_revision(), std::optional{target});
  }
  const auto active = fixture.owner->CurrentProjectionRevision();
  EXPECT_TRUE(fixture.owner->ActiveProjectionHealth().healthy_for_target(active));
  for (const auto &projection : fixture.predecessor_projections) {
    EXPECT_EQ(projection->last_health_revision(), std::optional{active});
  }
  for (const auto &projection : fixture.candidate_projections) {
    EXPECT_EQ(projection->rebuild_calls(), 1U);
  }
  // Complete the fake native producer's captured job, without calling Stage.
  const auto job = fixture.candidate_projections.front()->rebuild_requests().front();
  const auto completed = fixture.candidate_projections.front()->RebuildProjection(job);
  ASSERT_EQ(completed.result, core::ProjectionCommitResultStateDTO::kCommitted);
  const auto observed = fixture.owner->ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, fixture.fence, fixture.fence.OriginalAttemptQuery());
  ASSERT_EQ(observed.size(), staged.size());
  for (const auto &row : observed) {
    EXPECT_TRUE(row.healthy_for_target(job.target_projection_revision));
  }
  EXPECT_EQ(fixture.candidate_projections.front()->rebuild_calls(), 2U);
  const auto ready = fixture.Begin(request);
  EXPECT_TRUE(ready.ready_for_publication());
  EXPECT_EQ(fixture.candidate_projections.front()->rebuild_calls(), 2U);
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerStagingRefusesForeignProofBeforeNativeEffectsAndPreservesLostResponseTruth) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.PrepareCandidateClients();
  fixture.fence.RefuseRemoteQueryAfter(1U);
  EXPECT_THROW(fixture.owner->StageOriginalManagerConfigurationAdoption(request, fixture.fence, fixture.fence.OriginalAttemptQuery()), std::runtime_error);
  for (const auto &projection : fixture.candidate_projections) {
    EXPECT_EQ(projection->rebuild_calls(), 1U);
  }
  EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), request.expected_active_projection_revision);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id,
       request.authority.owner.apply_generation}).has_value());
  const auto held = fixture.cell_admission.GetCellAdmissionRecoveryReceipt(
      {request.authority.held_cell_claim.intent, request.authority.held_cell_claim, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerAdoptionRefusesForeignIdentityHeadSnapshotAndHealth) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  auto foreign = request;
  foreign.authority.held_cell_claim.claim_generation = core::CellAdmissionClaimGeneration(99U);
  EXPECT_THROW(fixture.Begin(foreign), LogicalSceneOwnerConflictError);
  foreign = request;
  foreign.authority.owner.apply_id = ApplyId(299U);
  foreign.authority.held_cell_claim.intent.owner_transaction_id = ApplyId(299U);
  foreign.authority.held_fence_query.owner_transaction = foreign.authority.owner;
  EXPECT_THROW(fixture.Begin(foreign), LogicalSceneOwnerConflictError);
  foreign = request;
  foreign.expected_predecessor_publication.durable_public_pointer_content_digest = Digest('9');
  EXPECT_THROW(fixture.Begin(foreign), LogicalSceneOwnerConflictError);
  foreign = request;
  NameForeignScene(foreign);
  EXPECT_THROW(fixture.Begin(foreign), LogicalSceneOwnerConflictError);
  foreign = request;
  foreign.execution_host_bindings.pop_back();
  EXPECT_THROW(fixture.Begin(foreign), LogicalSceneOwnerConflictError);
  fixture.candidate_projections.front()->Reset(
      request.expected_active_projection_revision, FakePlannerSceneProjection::Mode::kCommit);
  EXPECT_THROW(fixture.Begin(request), LogicalSceneOwnerConflictError);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id,
       request.authority.owner.apply_generation}).has_value());
  EXPECT_TRUE(fixture.owner->ReadAccessStatus().scene_writes_open);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerUnavailableResponseBeforeCaptureRecordsNothing) {
  SceneAdoptionFixture fixture;
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  fixture.fence.RefuseRemoteQueryAfter(1U);
  EXPECT_THROW(fixture.Begin(request), std::runtime_error);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization(
      {request.authority.owner.apply_id,
       request.authority.owner.apply_generation}).has_value());
  EXPECT_TRUE(fixture.owner->ReadAccessStatus().scene_writes_open);
  const auto held = fixture.cell_admission.GetCellAdmissionRecoveryReceipt(
      {request.authority.held_cell_claim.intent, request.authority.held_cell_claim, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerLostCaptureResponsePreservesTruth) {
  SceneAdoptionFixture fixture;
  const auto snapshot = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  fixture.fence.RefuseRemoteQueryAfter(2U);
  EXPECT_THROW(fixture.Begin(request), std::runtime_error);
  const auto query = core::ProjectionMaintenanceInitializationQueryDTO{
      request.authority.owner.apply_id,
      request.authority.owner.apply_generation};
  const auto captured = fixture.owner->ReadMaintenanceInitialization(query);
  ASSERT_TRUE(captured.has_value());
  EXPECT_EQ(captured->state, core::ProjectionMaintenanceInitializationState::kInitialized);
  EXPECT_FALSE(fixture.owner->ReadAccessStatus().scene_writes_open);
  const auto captured_snapshot = fixture.owner->ReadActiveSnapshot();
  ASSERT_TRUE(captured_snapshot.has_value());
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*captured_snapshot->shared()), snapshot);
  const auto held = fixture.cell_admission.GetCellAdmissionRecoveryReceipt(
      {request.authority.held_cell_claim.intent, request.authority.held_cell_claim, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     OriginalManagerLostCompletionResponseKeepsCompletionAndParentHeld) {
  SceneAdoptionFixture fixture;
  const auto snapshot = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
  const auto request = fixture.CaptureConfigurationRequest();
  fixture.QualifyCandidate(request);
  const auto ready = fixture.Begin(request);
  const auto publication = fixture.Publication(request);
  fixture.publication_reader.SetCurrent(publication);
  fixture.fence.RefuseRemoteQueryAfter(2U);
  EXPECT_THROW(fixture.Complete(request, publication), std::runtime_error);
  const auto query = core::ProjectionMaintenanceInitializationQueryDTO{
      request.authority.owner.apply_id,
      request.authority.owner.apply_generation};
  const auto completed = fixture.owner->ReadMaintenanceInitialization(query);
  ASSERT_TRUE(completed.has_value());
  EXPECT_EQ(completed->state, core::ProjectionMaintenanceInitializationState::kCompleted);
  EXPECT_EQ(completed->target_projection_revision, ready.target_projection_revision);
  EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot);
  const auto held = fixture.cell_admission.GetCellAdmissionRecoveryReceipt(
      {request.authority.held_cell_claim.intent, request.authority.held_cell_claim, std::nullopt});
  ASSERT_TRUE(held.ok());
  EXPECT_EQ(held.value().status, core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     QualifiedAdoptionCannotRepairAnUncertainSceneTransaction) {
  SceneAdoptionFixture fixture;
  const auto active = fixture.owner->CurrentProjectionRevision();
  fixture.predecessor_projections.front()->SetMode(FakePlannerSceneProjection::Mode::kUncertain);
  const auto failed = fixture.owner->Commit(AddBoxRequest(active, fixture.admission.cell_id(), "adoption-uncertain"));
  ASSERT_FALSE(failed.success);
  const auto integrity_before = fixture.owner->VerifyIntegrity();
  ASSERT_TRUE(integrity_before.projection_desynchronized);
  // Capture only real pre-effect values; public projection-lease access is
  // correctly closed by the failed transaction and cannot be used as truth.
  // A Kernel reset settlement does not repair the Scene owner's uncertainty.
  const auto claims = fixture.cell_admission.Claims();
  ASSERT_FALSE(claims.empty());
  const auto &held = claims.back();
  ASSERT_TRUE(fixture.cell_admission.ReleaseCellAdmissionClaim(
      {held.intent.owner_role, held.intent.owner_transaction_id,
       held.intent.request_key, held.acquired_admission_generation,
       held.claim_generation, {core::CellAdmissionSettlementKindDTO::kExplicitReset, Digest('e')}}).ok());
  const core::ApplyFenceOwnerDTO parent{ApplyId(200U), ApplyGeneration(1U)};
  const auto claim = fixture.cell_admission.AcquireCellAdmissionClaim(
      {core::CellAdmissionClaimKindDTO::kManagerApply, core::ApplicationRole::kManager,
       parent.apply_id, SceneRequestKey("parent-request"),
       core::AdmissionGeneration(1U), {}, std::nullopt, std::nullopt});
  ASSERT_TRUE(claim.ok());
  const auto published = fixture.publication_reader.ReadActiveBundlePublication(ActiveBundlePublicationGeneration(2U));
  ASSERT_TRUE(published.ok());
  const core::ConfigurationFenceStatusQueryDTO query{
      parent, core::ConfigurationFenceId("apply-fence"), core::ConfigurationFenceGeneration(1U),
      {}};
  fixture.fence.Configure(query, published.value());
  const auto snapshot = fixture.owner->ReadActiveSnapshot();
  ASSERT_TRUE(snapshot.has_value());
  const QualifiedSceneConfigurationAdoptionRequest request{
      {parent, claim.value(), query}, published.value(), active,
      snapshot->shared()->identity(), fixture.candidate.bundle->value(), fixture.candidate_bindings};
  fixture.QualifyCandidate(request);
  EXPECT_THROW(fixture.Begin(request), LogicalSceneOwnerConflictError);
  const auto integrity_after = fixture.owner->VerifyIntegrity();
  EXPECT_TRUE(integrity_after.projection_desynchronized);
  EXPECT_EQ(integrity_after.transaction_intent_count, integrity_before.transaction_intent_count);
  EXPECT_EQ(integrity_after.owner_event_count, integrity_before.owner_event_count);
  EXPECT_FALSE(fixture.owner->ReadMaintenanceInitialization({parent.apply_id, parent.apply_generation}).has_value());
  fixture.ExpectNoCandidateEffects();
}

TEST(CanonicalEmptySceneOwner,
     InitialSceneIsInstalledOnceBeforePublicationAndSurvivesReadback) {
  TemporaryDirectory directory("scene-initial-baseline");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(candidate.bundle->value(),
                                               "runtime-initial-baseline");
  auto projections = MakeFakeProjections(bindings);
  FakePlannerSceneProjectionResolver resolver(ProjectionClients(projections));
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
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
      ApplyId(400U), ApplyGeneration(1U),
      predecessor, candidate.bundle->value(), bindings,
      {core::AddObjectRequestDTO{
           core::SceneResourceId("frame_object"),
           core::SphereShapeDTO{0.05}, pose,
           core::ComponentSceneFrameRefDTO{
               core::ControllableComponentId("candidate-component"),
               core::ModelInstanceId("candidate-model"),
               core::FrameRoleId("candidate-frame")}},
       core::AddObjectRequestDTO{
           core::SceneResourceId("mesh"),
           core::MeshShapeDTO{"asset://fixture/mesh", {1.0, 1.0, 1.0},
                              Digest('e').value()},
           pose, core::CellSceneFrameRefDTO{admission.cell_id()}},
       core::AddObjectRequestDTO{
          core::SceneResourceId("wall"), core::BoxShapeDTO{{0.1, 0.2, 0.3}},
          pose, core::CellSceneFrameRefDTO{admission.cell_id()}}}};
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
  // Its empty initial Scene must still replace the prior Program's objects,
  // while refreshing the projection through the same retained native owner.
  std::vector<FakePlannerSceneProjection *> retained_projection_owners;
  retained_projection_owners.reserve(projections.size());
  for (const auto &projection : projections) {
    retained_projection_owners.push_back(projection.get());
  }
  const auto next_bindings = bindings;
  const core::ProjectionMaintenanceInitializationRequestDTO empty_program{
      ApplyId(401U), ApplyGeneration(1U),
      completed.target_projection_revision, candidate.bundle->value(),
      next_bindings, {}};
  const auto empty_provisional =
      owner.BeginMaintenanceInitialization(empty_program);
  ASSERT_EQ(empty_provisional.state,
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
  ASSERT_EQ(projections.size(), retained_projection_owners.size());
  for (std::size_t index = 0U; index < projections.size(); ++index) {
    EXPECT_EQ(projections[index].get(), retained_projection_owners[index]);
    EXPECT_EQ(projections[index]->rebuild_calls(), 4U);
    EXPECT_EQ(projections[index]->ProjectionHealth().active_projection_revision,
              empty_installed.target_projection_revision);
    const auto &rebuilds = projections[index]->rebuild_requests();
    ASSERT_EQ(rebuilds.size(), 4U);
    EXPECT_EQ(rebuilds[2].maintenance_successor,
              std::make_pair(empty_program.apply_id,
                             empty_program.apply_generation));
    EXPECT_EQ(rebuilds[2].maintenance_predecessor,
              completed.target_projection_revision);
    EXPECT_EQ(rebuilds[3].maintenance_successor,
              std::make_pair(empty_program.apply_id,
                             empty_program.apply_generation));
    EXPECT_EQ(rebuilds[3].maintenance_predecessor,
              empty_provisional.target_projection_revision);
  }
  const auto empty_snapshot = owner.ReadActiveSnapshot();
  ASSERT_TRUE(empty_snapshot.has_value());
  EXPECT_TRUE(empty_snapshot->shared()->objects.empty());
  EXPECT_EQ(empty_snapshot->shared()->revision,
            empty_installed.target_projection_revision.scene_revision);
}

// The maintenance-initialization history is bounded by eviction: once it is
// full, each new Apply evicts the oldest initialization no reader still needs
// (here, after folding the replayed completions and the resident Scene
// transactions into the history checkpoint), so no Apply is ever refused.
TEST(CanonicalEmptySceneOwner,
     MaintenanceInitializationHistoryEvictsOldestSettledAtItsBound) {
  TemporaryDirectory directory("scene-maintenance-eviction");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(candidate.bundle->value(),
                                              "runtime-maintenance-eviction");
  auto projections = MakeFakeProjections(bindings);
  FakePlannerSceneProjectionResolver resolver(ProjectionClients(projections));
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  TestCellAdmissionPort cell_admission;
  const auto apply_id = [](const std::size_t index) {
    return ApplyId(500U + static_cast<std::uint64_t>(index));
  };
  const std::size_t total = kMaximumMaintenanceInitializations + 8U;
  const auto last_completed = apply_id(total - 1U);
  const auto unsettled = apply_id(total);
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          resolver, cell_admission);
  auto current =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  for (std::size_t index = 0U; index < total; ++index) {
    if (index % 40U == 20U) {
      // Resident Scene transactions interleave with the Applies; the fold
      // at the bound compacts them, and later commits still succeed.
      const auto authored = owner.Commit(AddBoxRequest(
          current, admission.cell_id(),
          "scene-request-eviction-" + std::to_string(index)));
      ASSERT_TRUE(authored.success)
          << index << " "
          << (authored.error.has_value() ? authored.error->message : "");
      current = *authored.committed_projection_revision;
    }
    const core::ProjectionMaintenanceInitializationRequestDTO request{
        apply_id(index), ApplyGeneration(1U), current,
        candidate.bundle->value(), bindings, {}};
    const auto initialized = owner.BeginMaintenanceInitialization(request);
    ASSERT_EQ(initialized.state,
              core::ProjectionMaintenanceInitializationState::kInitialized)
        << index;
    const auto installed = owner.InstallInitialScene(request);
    ASSERT_TRUE(installed.ready_for_publication()) << index;
    const core::ActiveBundlePublicationReceiptDTO receipt{
        core::ActiveBundlePublicationRequestDTO{
            core::NamedApplyPublicationTransactionDTO{
                request.apply_id, request.apply_generation},
            installed.target_projection_revision,
            installed.target_projection_revision.retained_bundle_id,
            installed.target_projection_revision.graph_revision,
            ActiveBundlePublicationGeneration(index + 1U), Digest('d')},
        ActiveBundlePublicationGeneration(index + 2U), Digest('f')};
    publication_reader.SetCurrent(receipt);
    const auto completed = owner.CompleteMaintenanceInitialization({
        request.apply_id, request.apply_generation,
        installed.target_projection_revision,
        installed.authoritative_snapshot_identity, receipt});
    ASSERT_EQ(completed.state,
              core::ProjectionMaintenanceInitializationState::kCompleted)
        << index;
    current = completed.target_projection_revision;
  }
  EXPECT_FALSE(owner.ReadMaintenanceInitialization({apply_id(0U),
                                                    ApplyGeneration(1U)})
                   .has_value());
  EXPECT_EQ(owner.ReadMaintenanceInitialization({last_completed,
                                                 ApplyGeneration(1U)})
                .value()
                .state,
            core::ProjectionMaintenanceInitializationState::kCompleted);

  // An unsettled (initialized, uncompleted) Apply is admitted at the bound
  // and survives as the latest initialization.
  const core::ProjectionMaintenanceInitializationRequestDTO pending{
      unsettled, ApplyGeneration(1U), current, candidate.bundle->value(),
      bindings, {}};
  ASSERT_EQ(owner.BeginMaintenanceInitialization(pending).state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  EXPECT_EQ(owner.ReadMaintenanceInitialization({unsettled,
                                                 ApplyGeneration(1U)})
                .value()
                .state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  // The history holds its bound: the newest initializations, the unsettled
  // one included.
  std::size_t resident = 0U;
  for (std::size_t index = 0U; index <= total; ++index) {
    const bool present = owner
                             .ReadMaintenanceInitialization(
                                 {apply_id(index), ApplyGeneration(1U)})
                             .has_value();
    EXPECT_EQ(present, index + kMaximumMaintenanceInitializations > total)
        << index;
    resident += present ? 1U : 0U;
  }
  EXPECT_EQ(resident, kMaximumMaintenanceInitializations);
  EXPECT_NO_THROW(static_cast<void>(owner.VerifyIntegrity()));
}

TEST(CanonicalEmptySceneOwner,
     UnresolvableCandidateFrameCannotPublishInitialScene) {
  TemporaryDirectory directory("scene-initial-frame-rejection");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(candidate.bundle->value(),
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
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  core::Pose3DTO pose;
  const core::ProjectionMaintenanceInitializationRequestDTO invalid_geometry{
      ApplyId(402U), ApplyGeneration(1U),
      accepted.canonical_empty_target, candidate.bundle->value(), bindings,
      {core::AddObjectRequestDTO{
          core::SceneResourceId("fixture"), core::SphereShapeDTO{-0.1}, pose,
          core::CellSceneFrameRefDTO{admission.cell_id()}}}};
  EXPECT_THROW(owner.BeginMaintenanceInitialization(invalid_geometry),
               std::invalid_argument);
  EXPECT_EQ(projections.front()->rebuild_calls(), 0U);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId(403U), ApplyGeneration(1U),
      accepted.canonical_empty_target, candidate.bundle->value(), bindings,
      {core::AddObjectRequestDTO{
          core::SceneResourceId("fixture"),
          core::BoxShapeDTO{{0.1, 0.2, 0.3}}, pose,
          core::ComponentSceneFrameRefDTO{
              core::ControllableComponentId("missing-component"),
              core::ModelInstanceId("missing-model"),
              core::FrameRoleId("missing-frame")}}}};
  ASSERT_EQ(owner.BeginMaintenanceInitialization(request).state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  const auto failed = owner.InstallInitialScene(request);
  EXPECT_EQ(failed.state,
            core::ProjectionMaintenanceInitializationState::kFailed);
  EXPECT_FALSE(failed.ready_for_publication());
  // A failed initialization no longer holds the gates.
  EXPECT_TRUE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_EQ(owner.ReadAccessStatus().phase,
            CanonicalEmptySceneBootstrapPhase::kFinalized);
  EXPECT_EQ(owner.InstallInitialScene(request), failed);
}

TEST(CanonicalEmptySceneOwner,
     InterruptedInitialSceneEffectEndsClosedWithoutReplaying) {
  TemporaryDirectory directory("scene-initial-effect-interrupted");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(candidate.bundle->value(),
                                              "runtime-initial-interrupted");
  auto projections = MakeFakeProjections(bindings);
  FakePlannerSceneProjectionResolver resolver(ProjectionClients(projections));
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId(404U),
      ApplyGeneration(1U),
      accepted.canonical_empty_target,
      candidate.bundle->value(),
      bindings,
      {core::AddObjectRequestDTO{
          core::SceneResourceId("wall"), core::BoxShapeDTO{{0.1, 0.2, 0.3}},
          core::Pose3DTO{}, core::CellSceneFrameRefDTO{admission.cell_id()}}}};
  ASSERT_EQ(owner.BeginMaintenanceInitialization(request).state,
            core::ProjectionMaintenanceInitializationState::kInitialized);
  projections.front()->ThrowAfterNextRebuildEffect();
  const auto interrupted = owner.InstallInitialScene(request);
  EXPECT_EQ(interrupted.state,
            core::ProjectionMaintenanceInitializationState::kInterrupted);
  EXPECT_TRUE(interrupted.effects_unknown);
  EXPECT_FALSE(interrupted.ready_for_publication());
  const auto rebuild_calls = projections.front()->rebuild_calls();
  EXPECT_EQ(owner.InstallInitialScene(request), interrupted);
  EXPECT_EQ(owner.ReadMaintenanceInitialization(
                {request.apply_id, request.apply_generation}),
            std::optional{interrupted});
  // An ended initialization no longer holds the gates; the Scene stays at its
  // predecessor.
  EXPECT_TRUE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_TRUE(owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(owner.CurrentProjectionRevision(),
            request.expected_active_projection_revision);
  EXPECT_EQ(projections.front()->rebuild_calls(), rebuild_calls);
}

TEST(CanonicalEmptySceneOwner,
     MaintenanceDuplicateObservesOneFreshInitialization) {
  TemporaryDirectory directory("scene-maintenance-duplicate");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(
      candidate.bundle->value(), "runtime-epoch-maintenance-duplicate");
  auto projections = MakeFakeProjections(bindings);
  auto clients = ProjectionClients(projections);
  FakePlannerSceneProjectionResolver resolver(clients);
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId(405U), ApplyGeneration(1U),
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
     MaintenanceReplayRetainsPreviouslyCommittedProjectionEffects) {
  TemporaryDirectory directory("scene-maintenance-partial-replay");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(
      candidate.bundle->value(), "runtime-epoch-partial-replay");
  auto projections = MakeFakeProjections(bindings);
  ASSERT_GE(projections.size(), 2U);
  auto clients = ProjectionClients(projections);
  FakePlannerSceneProjectionResolver resolver(clients);
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          resolver, cell_admission);
  const auto predecessor = FinalizeNativeCandidate(
      owner, publication_reader, admission, *candidate.bundle, bindings);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId(406U), ApplyGeneration(1U),
      predecessor, candidate.bundle->value(), bindings};
  // One participant commits; the other reports accepted, then refuses on
  // the next observation. Classification must retain the earlier effect.
  projections.back()->AcceptNextRebuildAs(bindings.back());
  const auto pending = owner.BeginMaintenanceInitialization(request);
  ASSERT_EQ(pending.state,
            core::ProjectionMaintenanceInitializationState::kInitializing);
  const auto first_rebuilds = projections.front()->rebuild_calls();
  projections.back()->RejectNextRebuildWithoutEffect();
  const auto result = owner.BeginMaintenanceInitialization(request);
  EXPECT_EQ(result.state,
            core::ProjectionMaintenanceInitializationState::kInterrupted);
  EXPECT_TRUE(result.effects_unknown);
  EXPECT_EQ(projections.front()->rebuild_calls(), first_rebuilds);
  EXPECT_EQ(owner.ReadMaintenanceInitialization(
                {request.apply_id, request.apply_generation}),
            std::optional{result});
  EXPECT_EQ(owner.BeginMaintenanceInitialization(request), result);
}

TEST(CanonicalEmptySceneOwner,
     MaintenanceStatusRemainsReadableWhileNativeInitializationIsBlocked) {
  TemporaryDirectory directory("scene-maintenance-responsive-status");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(
      candidate.bundle->value(), "runtime-epoch-maintenance-responsive");
  auto projections = MakeFakeProjections(bindings);
  auto clients = ProjectionClients(projections);
  FakePlannerSceneProjectionResolver resolver(clients);
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          resolver, cell_admission);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const core::ProjectionMaintenanceInitializationRequestDTO request{
      ApplyId(407U), ApplyGeneration(1U),
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
     ResolvesPortableFramesAndCarriesThenDetachesAnObjectBeforeEffects) {
  TemporaryDirectory directory("scene-frame-carried-object");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto native_bindings = ExecutionHostBindings(
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
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          projection_resolver, cell_admission);
  auto active = FinalizeNativeCandidate(
      owner, publication_reader, admission, *candidate.bundle,
      native_bindings, 101U);
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
  // The object mass rides along the add, attach and both detaches.
  std::get<core::AddObjectRequestDTO>(add.operation).mass_kg = 1.5;
  const auto added = owner.Commit(add);
  ASSERT_TRUE(added.success)
      << (added.error.has_value() ? added.error->message : "missing error");
  ASSERT_TRUE(added.committed_projection_revision.has_value());
  active = *added.committed_projection_revision;
  auto snapshot = owner.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), 1U);
  EXPECT_DOUBLE_EQ(snapshot->objects.front().mass_kg, 1.5);
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
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      "attach",
      core::NodeAttemptNumber(1U),
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
  // The owner preview names the older projection of this Scene and resolves
  // against the current one, where the attachment already exists.
  const auto old_projection_preview =
      owner.PreviewResolvedSceneCandidate(preview_request);
  ASSERT_FALSE(old_projection_preview.ok());
  EXPECT_EQ(old_projection_preview.error().code, "scene_object_state_conflict");
  EXPECT_EQ(old_projection_preview.error().details.at("reason"),
            "carried_attachment_already_exists");
  // Planning can finish against its already captured input after live Scene
  // truth advances. This pure preview neither commits nor repeats the attach.
  const auto planned_after_live_change = BuildResolvedSceneCandidatePreview(
      preview_request, active, snapshot, snapshot->transaction_sequence + 1U,
      [&](const core::LogicalSceneSnapshotDTO &candidate) {
        return projection.ProjectEffectiveCollisionPolicy(candidate, candidate.revision.revision_id);
      });
  ASSERT_TRUE(planned_after_live_change.ok()) << planned_after_live_change.error().message;
  EXPECT_EQ(planned_after_live_change.value().logical_scene->revision,
            preview.value().logical_scene->revision);
  EXPECT_EQ(owner.CurrentProjectionRevision(), *attached.committed_projection_revision);
  auto foreign_predecessor = active;
  foreign_predecessor.scene_revision.content_hash = Digest('0');
  EXPECT_FALSE(BuildResolvedSceneCandidatePreview(
      preview_request, foreign_predecessor, snapshot, snapshot->transaction_sequence + 1U,
      [&](const core::LogicalSceneSnapshotDTO &candidate) {
        return projection.ProjectEffectiveCollisionPolicy(candidate, candidate.revision.revision_id);
      }).ok());
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
  EXPECT_DOUBLE_EQ(snapshot->objects.front().mass_kg, 1.5);
  {
    const auto bytes = EncodeLogicalSceneSnapshot(*snapshot);
    EXPECT_NE(bytes.find(R"("mass_kg":1.5)"), std::string::npos);
    EXPECT_EQ(DecodeLogicalSceneSnapshot(bytes), *snapshot);
  }
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
  EXPECT_DOUBLE_EQ(snapshot->objects.front().mass_kg, 1.5);
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
  EXPECT_DOUBLE_EQ(snapshot->objects.front().mass_kg, 1.5);
  // The relative object was added without a mass.
  EXPECT_DOUBLE_EQ(snapshot->objects.back().mass_kg, 0.0);
  fixed = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
      &snapshot->objects.front().placement);
  ASSERT_NE(fixed, nullptr);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.x_m, 2.5);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.y_m, 2.0);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.z_m, 3.0);

  // An old carrier structural-attachment revision is not a precondition:
  // the carrier resolves against the current attachments (last arrival wins).
  auto old_revision_attach = attach;
  old_revision_attach.request_context =
      SceneRequestContext("scene-request-old-revision-attach", active);
  old_revision_attach.expected_projection_revision = active;
  auto &old_revision_operation = std::get<core::AttachCarriedObjectRequestDTO>(
      old_revision_attach.operation);
  old_revision_operation.expected_component_attachment_revisions = {
      core::ComponentAttachmentRevision("old-revision")};
  const auto old_revision = owner.Commit(old_revision_attach);
  ASSERT_TRUE(old_revision.success)
      << (old_revision.error.has_value() ? old_revision.error->message
                                         : "missing error");
  ASSERT_TRUE(old_revision.committed_projection_revision.has_value());
  active = *old_revision.committed_projection_revision;
  EXPECT_EQ(owner.LogicalSnapshot()->carried_object_attachments.size(), 1U);

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
    EXPECT_EQ(projections[index]->calls(), 7U);
    // One runtime unit hosts every projection: only its first answers.
    EXPECT_EQ(projections[index]->frame_resolution_calls(),
              index == 0U ? 10U : 0U);
    EXPECT_EQ(projections[index]->policy_projection_calls(), index == 0U ? 9U : 8U);
    EXPECT_EQ(projections[index]->retained_lease_count(), 0U);
  }
  const auto integrity = owner.VerifyIntegrity();
  EXPECT_EQ(integrity.transaction_intent_count, 7);
  EXPECT_EQ(integrity.request_outcome_count, 7);
  EXPECT_EQ(integrity.projection_attempt_count,
            static_cast<std::int64_t>(7U * projections.size()));
}

TEST(CanonicalEmptySceneOwner,
     LiveFrameIsResolvedOnceByItsOwningRuntimeUnit) {
  TemporaryDirectory directory("scene-frame-owner-only");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto native_bindings = ExecutionHostBindings(
      candidate.bundle->value(), "runtime-epoch-frame-owner-only");

  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  std::vector<std::unique_ptr<FakePlannerSceneProjection>> projections;
  std::vector<FakePlannerSceneProjection *> projection_clients;
  for (const auto &binding : native_bindings) {
    projections.push_back(
        std::make_unique<FakePlannerSceneProjection>(binding));
    projection_clients.push_back(projections.back().get());
  }
  // Real-lane shape: several native projections hosted by one runtime unit.
  ASSERT_GE(projections.size(), 2U);
  for (const auto &projection : projections) {
    ASSERT_EQ(projection->binding().runtime_unit_id,
              projections.front()->binding().runtime_unit_id);
  }
  FakePlannerSceneProjectionResolver projection_resolver(projection_clients);
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          projection_resolver, cell_admission);
  auto active = FinalizeNativeCandidate(
      owner, publication_reader, admission, *candidate.bundle,
      native_bindings, 102U);
  for (auto &participant : projections) {
    participant->Reset(active, FakePlannerSceneProjection::Mode::kCommit);
  }

  // Each projection would read a different measured sample of an arm at rest.
  for (std::size_t index = 0U; index < projections.size(); ++index) {
    projections[index]->SetFrameResolution(
        core::Pose3DTO{{1.0 + 1.0e-5 * static_cast<double>(index), 2.0, 3.0},
                       {0.0, 0.0, 0.0, 1.0}});
  }
  auto add = AddBoxRequest(active, admission.cell_id(),
                           "scene-request-owner-only-frame");
  std::get<core::AddObjectRequestDTO>(add.operation).frame_ref =
      core::ComponentSceneFrameRefDTO{
          core::ControllableComponentId("component.1"),
          core::ModelInstanceId("model.1"),
          core::FrameRoleId("tool_frame")};
  const auto added = owner.Commit(add);
  ASSERT_TRUE(added.success)
      << (added.error.has_value() ? added.error->message : "missing error");
  const auto snapshot = owner.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), 1U);
  const auto *fixed = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
      &snapshot->objects.front().placement);
  ASSERT_NE(fixed, nullptr);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.x_m, 1.25);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.y_m, 1.5);
  EXPECT_DOUBLE_EQ(fixed->pose_in_cell.position_m.z_m, 3.75);
  // Only the owning unit's view answered, exactly once.
  EXPECT_EQ(projections.front()->frame_resolution_calls(), 1U);
  for (std::size_t index = 1U; index < projections.size(); ++index) {
    EXPECT_EQ(projections[index]->frame_resolution_calls(), 0U);
  }
  for (const auto &participant : projections) {
    EXPECT_EQ(participant->retained_lease_count(), 0U);
  }
}

TEST(CanonicalEmptySceneOwner,
     FrameOwnershipAcrossRuntimeUnitsIsTypedNeverCompared) {
  using scene::logical_scene_owner_detail::RequiredSceneProjection;
  using scene::logical_scene_owner_detail::ResolveFrameAcrossProjections;
  using scene::logical_scene_owner_detail::SceneRequestRejectionError;
  const ProjectionRevisionDTO active{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{core::SceneId(Digest('d').value()), 1U, Digest('a')},
  };
  const auto make = [&](const std::string &provider, const std::string &unit) {
    auto projection = std::make_shared<FakePlannerSceneProjection>(
        ProjectionExecutionHostBindingDTO{
            core::CapabilityProviderBindingId("provider-" + provider),
            core::CapabilityProviderBindingRevision("binding-revision"),
            core::RuntimeUnitId(unit), core::RuntimeEpoch("epoch-" + unit)});
    projection->Reset(active, FakePlannerSceneProjection::Mode::kCommit);
    return projection;
  };
  // Two projections share the robot's unit; a gripper unit hosts a third.
  const auto robot_a = make("a", "runtime.robot");
  const auto robot_b = make("b", "runtime.robot");
  const auto gripper = make("c", "runtime.gripper");
  robot_a->SetFrameResolution(
      core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}});
  robot_b->SetFrameResolution(
      core::Pose3DTO{{1.00004, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}});
  const auto health = [&](const auto &projection) {
    return projection->ProjectionHealthForRevision(active);
  };
  const std::vector<RequiredSceneProjection> projections{
      {robot_a, health(robot_a), robot_a->binding()},
      {robot_b, health(robot_b), robot_b->binding()},
      {gripper, health(gripper), gripper->binding()}};
  const core::SceneFrameResolutionSelectorDTO tcp{
      core::TargetCarrierSceneFrameRefDTO{
          core::ControllableComponentId("component.1"),
          core::CommandTargetId("arm"), core::FrameRoleId("tcp")}};
  const auto reject_missing = [](const auto &projection) {
    projection->RejectFrameResolution(core::ErrorDTO{
        "missing_reference", "frame is not owned here",
        {{"reason", "kinematic_state_source_unavailable"}}});
  };

  // A non-owner declines; the owner answers once from one view.
  reject_missing(gripper);
  const auto owned =
      ResolveFrameAcrossProjections(tcp, active, projections, "owner");
  ASSERT_TRUE(owned.has_value());
  EXPECT_DOUBLE_EQ(owned->frame_pose_in_cell.position_m.x_m, 1.0);
  EXPECT_EQ(robot_a->frame_resolution_calls(), 1U);
  EXPECT_EQ(robot_b->frame_resolution_calls(), 0U);
  EXPECT_EQ(gripper->frame_resolution_calls(), 1U);

  // Two runtime units claiming one robot frame is a typed configuration
  // error, whatever their poses.
  gripper->Reset(active, FakePlannerSceneProjection::Mode::kCommit);
  gripper->SetFrameResolution(
      core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}});
  try {
    static_cast<void>(
        ResolveFrameAcrossProjections(tcp, active, projections, "dual"));
    FAIL() << "dual robot-frame ownership resolved";
  } catch (const SceneRequestRejectionError &error) {
    EXPECT_EQ(error.code(), "missing_reference");
    EXPECT_EQ(error.details().at("reason"), "scene_frame_owner_ambiguous");
  }

  // Cell and passive fixture frames are Scene-owned static geometry: every
  // unit that answers must agree exactly, and the agreed frame is served.
  const core::SceneFrameResolutionSelectorDTO fixture{
      std::in_place_type<core::SceneFrameRefDTO>,
      core::SceneFrameRefDTO{core::PassiveModelSceneFrameRefDTO{
          core::ModelInstanceId("fixture.1"), core::FrameRoleId("origin")}}};
  gripper->SetFrameResolution(
      core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}});
  const auto static_frame =
      ResolveFrameAcrossProjections(fixture, active, projections, "fixture");
  ASSERT_TRUE(static_frame.has_value());
  EXPECT_DOUBLE_EQ(static_frame->frame_pose_in_cell.position_m.x_m, 1.0);

  // Two units that disagree on a static fixture frame are refused.
  gripper->SetFrameResolution(
      core::Pose3DTO{{1.0, 2.0, 3.5}, {0.0, 0.0, 0.0, 1.0}});
  try {
    static_cast<void>(ResolveFrameAcrossProjections(fixture, active,
                                                    projections, "fixture-x"));
    FAIL() << "disagreeing static frame resolved";
  } catch (const SceneRequestRejectionError &error) {
    EXPECT_EQ(error.code(), "missing_reference");
    EXPECT_EQ(error.details().at("reason"),
              "scene_frame_resolution_ambiguous");
  }
  for (const auto &projection : {robot_a, robot_b, gripper}) {
    EXPECT_EQ(projection->retained_lease_count(), 0U);
  }
}

TEST(CanonicalEmptySceneOwner,
     PolicyProjectionCarriesConfiguredAndEffectiveValuesSeparately) {
  TemporaryDirectory directory("scene-policy-resolution");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto native_bindings = ExecutionHostBindings(
      candidate.bundle->value(), "runtime-epoch-policy-resolution");
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);
  auto projections = MakeFakeProjections(native_bindings);
  FakePlannerSceneProjectionResolver projection_resolver(
      ProjectionClients(projections));
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          projection_resolver, cell_admission);
  const auto active = FinalizeNativeCandidate(
      owner, publication_reader, admission, *candidate.bundle,
      native_bindings, 103U);
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

  // A later edit naming the old configured-policy revision is accepted
  // against the current policy: the last arrival wins.
  const auto old_revision =
      std::get<core::SetCollisionPolicyRequestDTO>(request.operation)
          .expected_configured_policy_revision;
  const auto current_revision =
      owner.LogicalSnapshot()->configured_collision_policy.revision;
  ASSERT_NE(old_revision, current_revision);
  auto last = owner.LogicalSnapshot()->configured_collision_policy;
  last.require_full_diagnostics = !last.require_full_diagnostics;
  core::SceneTransactionRequestDTO last_request{
      SceneRequestContext("scene-request-policy-last-arrival",
                          *result.committed_projection_revision),
      {},
      *result.committed_projection_revision,
      core::SetCollisionPolicyRequestDTO{last, old_revision},
      core::AdmissionGeneration(1U),
  };
  const auto last_result = owner.Commit(last_request);
  ASSERT_TRUE(last_result.success)
      << (last_result.error.has_value() ? last_result.error->message
                                        : "missing error");
  const auto stored = owner.LogicalSnapshot()->configured_collision_policy;
  EXPECT_EQ(stored.require_full_diagnostics, last.require_full_diagnostics);
  EXPECT_EQ(stored.revision.revision_id, current_revision.revision_id + 1U);
}

TEST(CanonicalEmptySceneOwner,
     SceneUpdatesClassifyNoOpFreshnessGeometryAndPolicyEpochs) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
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
  core::SceneTransactionRequestDTO geometry{
      SceneRequestContext("scene-request-geometry", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          moved_pose,
          core::CellSceneFrameRefDTO{admission.cell_id()},
          evidence,
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

  // A pose update naming an older revision of this Scene is admitted to its
  // own identity checks; repeating the committed pose and origin is a no-op.
  // Observation order is the scene runtime's, not the logical scene's.
  auto stale = geometry;
  stale.request_context.request_key = SceneRequestKey("scene-request-stale");
  const auto replayed = owner.Commit(stale);
  ASSERT_TRUE(replayed.success);
  EXPECT_EQ(replayed.update_status, core::SceneUpdateStatusDTO::kNoOp);
  EXPECT_EQ(replayed.committed_projection_revision, projection);
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 6);
}

TEST(CanonicalEmptySceneOwner,
     SceneOperationsTakeNoRevisionPrecondition) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  ProjectionRevisionDTO older;
  ProjectionRevisionDTO active;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto initial =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  const auto first = owner.Commit(
      AddBoxRequest(initial, admission.cell_id(), "scene-request-first"));
  ASSERT_TRUE(first.success);
  older = *first.committed_projection_revision;
  auto second_request =
      AddBoxRequest(older, admission.cell_id(), "scene-request-second");
  std::get<core::AddObjectRequestDTO>(second_request.operation)
      .scene_resource_id = core::SceneResourceId("scene-box-2");
  const auto second = owner.Commit(second_request);
  ASSERT_TRUE(second.success);
  active = *second.committed_projection_revision;
  ASSERT_GT(active.scene_revision.revision_id,
            older.scene_revision.revision_id);

  const auto request = [&](std::string request_id,
                           const ProjectionRevisionDTO &expected,
                           core::SceneOperationRequestDTO operation) {
    return core::SceneTransactionRequestDTO{
        SceneRequestContext(std::move(request_id), older),
        {},
        expected,
        std::move(operation),
        core::AdmissionGeneration(1U),
    };
  };
  const auto stale_code = [&](const core::SceneTransactionRequestDTO &value) {
    const auto result = owner.Commit(value);
    EXPECT_FALSE(result.success);
    return result.error.has_value() ? result.error->code : std::string{};
  };
  const auto upsert_from = [&](std::string request_id,
                               const ProjectionRevisionDTO &expected,
                               const core::Pose3DTO &pose) {
    auto upsert =
        AddBoxRequest(expected, admission.cell_id(), std::move(request_id));
    const auto add = std::get<core::AddObjectRequestDTO>(upsert.operation);
    upsert.operation = core::UpsertObjectRequestDTO{
        core::SceneResourceId("scene-box-2"), add.shape, pose, add.frame_ref};
    return upsert;
  };

  // Ordinary authoring takes no revision precondition either: the last
  // arrival wins.
  core::Pose3DTO authored;
  authored.position_m = {0.25, 0.25, 0.25};
  const auto authored_upsert = owner.Commit(
      upsert_from("scene-request-older-upsert", older, authored));
  ASSERT_TRUE(authored_upsert.success)
      << (authored_upsert.error.has_value() ? authored_upsert.error->message
                                            : "");
  active = *authored_upsert.committed_projection_revision;

  // Another Scene, graph or bundle is refused; the revision is not compared.
  auto other_scene = older;
  other_scene.scene_revision.scene_id = core::SceneId(Digest('e').value());
  auto other_graph = older;
  other_graph.graph_revision = core::GraphRevision(Digest('d').value());
  auto other_bundle = older;
  other_bundle.retained_bundle_id =
      core::RetainedBundleId(Digest('c').value());
  int foreign_index = 0;
  for (const auto &foreign : {other_scene, other_graph, other_bundle}) {
    EXPECT_EQ(stale_code(request(
                  "scene-request-foreign-" + std::to_string(++foreign_index),
                  foreign,
                  core::RemoveObjectRequestDTO{
                      core::SceneResourceId("scene-box")})),
              "stale_revision");
  }

  // An older revision, one the owner has not reached, and the same revision
  // with other content are all accepted.
  auto ahead = active;
  ahead.scene_revision.revision_id += 5U;
  auto same_revision_other_content = active;
  same_revision_other_content.scene_revision.content_hash = Digest('e');

  core::Pose3DTO moved;
  moved.position_m = {1.5, -0.5, 0.75};
  const auto updated = owner.Commit(request(
      "scene-request-older-update", older,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box-2"), moved,
          core::CellSceneFrameRefDTO{admission.cell_id()}, std::nullopt,
          false}));
  ASSERT_TRUE(updated.success)
      << (updated.error.has_value() ? updated.error->message : "");
  EXPECT_EQ(updated.committed_projection_revision->scene_revision.revision_id,
            active.scene_revision.revision_id + 1U);

  // A Program scene node upsert is accepted the same way.
  core::Pose3DTO declared;
  declared.position_m = {1.25, -0.25, 0.5};
  auto node_upsert =
      upsert_from("scene-request-node-upsert", ahead, declared);
  node_upsert.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      "upsert",
      core::NodeAttemptNumber(1U),
      "scene.transaction"};
  const auto node_upserted = owner.Commit(node_upsert);
  ASSERT_TRUE(node_upserted.success)
      << (node_upserted.error.has_value() ? node_upserted.error->message : "");

  const auto removed = owner.Commit(request(
      "scene-request-other-content-remove", same_revision_other_content,
      core::RemoveObjectRequestDTO{core::SceneResourceId("scene-box")}));
  ASSERT_TRUE(removed.success)
      << (removed.error.has_value() ? removed.error->message : "");
  active = *removed.committed_projection_revision;
  const auto objects = owner.GetSceneObjects();
  ASSERT_EQ(objects.size(), 1U);
  EXPECT_EQ(objects.front().scene_resource_id,
            core::SceneResourceId("scene-box-2"));
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 6);

  // Exact replay of an accepted request is unchanged.
  const auto replay = owner.Commit(request(
      "scene-request-older-update", older,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box-2"), moved,
          core::CellSceneFrameRefDTO{admission.cell_id()}, std::nullopt,
          false}));
  EXPECT_TRUE(replay.success);
  EXPECT_EQ(replay.replay_status,
            core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(owner.CurrentProjectionRevision(), active);
  const auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box-2"));
  ASSERT_TRUE(pose.ok());
  EXPECT_DOUBLE_EQ(pose.value().position_m.x_m, 1.25);
}

TEST(CanonicalEmptySceneOwner,
     ProjectionCommitRollbackAndUncertaintyUseClosedDurableOutcomes) {
  TemporaryDirectory packages("scene-native-projection-packages");
  const auto package_root = packages.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto native_bindings = ExecutionHostBindings(
      candidate.bundle->value(), "runtime-epoch-scene-projection");
  const auto two_robot_candidate = deployment::test::
      materialize_two_lite6_same_type_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto two_robot_native_bindings = ExecutionHostBindings(
      two_robot_candidate.bundle->value(),
      "runtime-epoch-scene-projection-partial");
  ASSERT_EQ(two_robot_native_bindings.size(), 2U * native_bindings.size());
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(package_root);

  {
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                            projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *candidate.bundle,
        native_bindings, 104U);
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
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                            projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *candidate.bundle,
        native_bindings, 105U);
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
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                            projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *candidate.bundle,
        native_bindings, 106U);
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
                 LogicalSceneOwnerConflictError);
  }

  {
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(two_robot_native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                            projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *two_robot_candidate.bundle,
        two_robot_native_bindings, 107U);
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
    ControlledPublicationReader publication_reader;
    auto projections = MakeFakeProjections(two_robot_native_bindings);
    FakePlannerSceneProjectionResolver projection_resolver(
        ProjectionClients(projections));
    TestCellAdmissionPort cell_admission;
    LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                            projection_resolver, cell_admission);
    const auto active = FinalizeNativeCandidate(
        owner, publication_reader, admission, *two_robot_candidate.bundle,
        two_robot_native_bindings, 108U);
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

/// Native owner with one provider-backed cell-fixed "scene-box" committed at
/// provider source sequence 1; every native projection is reset to the
/// resulting revision.
class ProviderFreshnessScene final {
public:
  ProviderFreshnessScene()
      : directory_("scene-provider-freshness"),
        package_root_(PreparePackages(directory_.path())),
        admission_(LoadAdmission(package_root_)),
        candidate_(deployment::test::
                       materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
                           package_root_, admission_.cell_id().value())),
        bindings_(ExecutionHostBindings(candidate_.bundle->value(),
                                        "runtime-epoch-provider-freshness")),
        projections_(MakeFakeProjections(bindings_)),
        resolver_(ProjectionClients(projections_)),
        bundle_source_(package_root_),
        owner_(admission_, publication_reader_, bundle_source_, resolver_,
               cell_admission_) {
    const auto active = FinalizeNativeCandidate(
        owner_, publication_reader_, admission_, *candidate_.bundle,
        bindings_, 110U);
    ResetProjections(projections_, active,
                     FakePlannerSceneProjection::Mode::kCommit);
    const auto added = owner_.Commit(AddBoxRequest(
        active, admission_.cell_id(), "scene-request-freshness-add"));
    Require(added.success, "provider freshness fixture did not add its box");
    const auto pose = owner_.GetObjectPose(core::SceneResourceId("scene-box"));
    Require(pose.ok(), "provider freshness fixture has no committed pose");
    pose_ = pose.value();
    const auto committed =
        owner_.Commit(PoseUpdate("scene-request-freshness-evidence",
                                 committed_evidence_));
    Require(committed.success &&
                committed.update_status ==
                    core::SceneUpdateStatusDTO::kFreshnessOnly,
            "provider freshness fixture did not commit its evidence");
    ResetProjections(projections_, owner_.CurrentProjectionRevision(),
                     FakePlannerSceneProjection::Mode::kCommit);
  }

  [[nodiscard]] LogicalSceneOwner &owner() noexcept { return owner_; }
  [[nodiscard]] OwnedFakePlannerSceneProjections &projections() noexcept {
    return projections_;
  }
  [[nodiscard]] const CellId &cell_id() const noexcept {
    return admission_.cell_id();
  }
  [[nodiscard]] const core::SceneObjectProviderEvidenceDTO &
  committed_evidence() const noexcept {
    return committed_evidence_;
  }

  [[nodiscard]] core::SceneTransactionRequestDTO
  PoseUpdate(std::string request_id,
             const core::SceneObjectProviderEvidenceDTO &evidence) {
    const auto current = owner_.CurrentProjectionRevision();
    return {
        core::PublicRequestContextDTO{
            SceneRequestKey(request_id),
            "scene-owner-test",
        },
        {},
        current,
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId("scene-box"),
            pose_,
            core::CellSceneFrameRefDTO{admission_.cell_id()},
            evidence,
            false,
        },
        std::nullopt,
    };
  }

private:
  static std::filesystem::path
  PreparePackages(const std::filesystem::path &root) {
    auto package_root = root / "packages";
    CopyCanonicalEmptyPackage(package_root);
    return package_root;
  }

  TemporaryDirectory directory_;
  std::filesystem::path package_root_;
  deployment::CanonicalEmptyRetainedBundleAdmission admission_;
  deployment::test::DurableLite6RetainedConsumerFixture candidate_;
  std::vector<ProjectionExecutionHostBindingDTO> bindings_;
  OwnedFakePlannerSceneProjections projections_;
  FakePlannerSceneProjectionResolver resolver_;
  ControlledPublicationReader publication_reader_;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source_;
  TestCellAdmissionPort cell_admission_;
  LogicalSceneOwner owner_;
  core::Pose3DTO pose_{};
  core::SceneObjectProviderEvidenceDTO committed_evidence_{
      core::ProviderInstanceId("provider-instance"),
      core::ProviderManifestId("provider-manifest"),
      core::ProviderManifestRevision(Digest('7').value()),
      "provider-epoch",
  };
};

TEST(CanonicalEmptySceneOwnerProviderFreshness,
     ContinuousPoseUpdatePreservesAttachmentAndUsesCurrentCarrier) {
  ProviderFreshnessScene scene;
  auto &owner = scene.owner();
  const core::ComponentAttachmentRevision structural_revision(
      "component-attachment-revision-1");
  for (auto &participant : scene.projections()) {
    participant->SetFrameResolution(
        core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}},
        {structural_revision});
  }
  auto add = AddBoxRequest(owner.CurrentProjectionRevision(), scene.cell_id(),
                           "scene-request-freshness-carried-add");
  auto &added_object = std::get<core::AddObjectRequestDTO>(add.operation);
  added_object.scene_resource_id = core::SceneResourceId("carried-box");
  added_object.frame_ref = core::ComponentSceneFrameRefDTO{
      core::ControllableComponentId("component.1"),
      core::ModelInstanceId("model.1"), core::FrameRoleId("tool_frame")};
  const auto added = owner.Commit(add);
  ASSERT_TRUE(added.success)
      << (added.error.has_value() ? added.error->message : "missing error");
  for (auto &participant : scene.projections()) {
    participant->SetResolvedFrameRoleOverride(
        core::FrameRoleId("asset_tool_frame"));
  }
  const auto active = owner.CurrentProjectionRevision();
  core::SceneTransactionRequestDTO attach{
      SceneRequestContext("scene-request-freshness-attach", active),
      {},
      active,
      core::AttachCarriedObjectRequestDTO{
          core::CarriedObjectAttachmentId("attachment.1"),
          core::SceneResourceId("carried-box"),
          core::ControllableComponentId("component.1"),
          core::CommandTargetId("arm"),
          core::FrameRoleId("tool_frame"),
          {core::CollisionBodyRoleId("finger")},
          {structural_revision},
      },
      core::AdmissionGeneration(1U),
  };
  attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      "attach",
      core::NodeAttemptNumber(1U),
      "scene.carried_attachment"};
  const auto attached = owner.Commit(attach);
  ASSERT_TRUE(attached.success)
      << (attached.error.has_value() ? attached.error->message
                                     : "missing error");
  ASSERT_TRUE(
      owner.GetObjectState(core::SceneResourceId("carried-box")).attached);

  const auto before =
      owner.LogicalSnapshot()->carried_object_attachments.front();
  const auto apply = [&](const std::string &id, const double x,
                         const bool require_detached) {
    const auto expected = owner.CurrentProjectionRevision();
    const auto &evidence = scene.committed_evidence();
    return owner.Commit(core::SceneTransactionRequestDTO{
        SceneRequestContext(id, expected),
        {},
        expected,
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId("carried-box"),
            core::Pose3DTO{{x, 2.0, 3.0}, {0, 0, 0, 1}},
            core::CellSceneFrameRefDTO{scene.cell_id()}, evidence,
            require_detached ? std::optional(false) : std::nullopt},
        std::nullopt});
  };
  const auto rejected = apply("continuous-requires-detached", 4.0, true);
  ASSERT_FALSE(rejected.success);
  EXPECT_EQ(rejected.error->details.at("reason"),
            "scene_object_attachment_precondition_failed");
  const auto updated = apply("continuous-attached", 4.0, false);
  ASSERT_TRUE(updated.success) << updated.error->message;
  const auto after =
      owner.LogicalSnapshot()->carried_object_attachments.front();
  EXPECT_EQ(after.carried_object_attachment_id,
            before.carried_object_attachment_id);
  EXPECT_EQ(after.carrier_frame_ref, before.carrier_frame_ref);
  EXPECT_EQ(after.touch_body_roles, before.touch_body_roles);
  EXPECT_NE(after.carried_object_attachment_revision,
            before.carried_object_attachment_revision);
  EXPECT_DOUBLE_EQ(after.pose_in_carrier_frame.position_m.x_m, 3.0);
  const auto world = owner.GetObjectPose(core::SceneResourceId("carried-box"));
  ASSERT_TRUE(world.ok());
  EXPECT_DOUBLE_EQ(world.value().position_m.x_m, 4.0);
  EXPECT_TRUE(
      owner.GetObjectState(core::SceneResourceId("carried-box")).attached);
  const auto current = owner.CurrentProjectionRevision();
  const auto &evidence = scene.committed_evidence();
  const auto unchanged = owner.Commit(core::SceneTransactionRequestDTO{
      SceneRequestContext("continuous-attached-freshness", current),
      {},
      current,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("carried-box"), world.value(),
          core::CellSceneFrameRefDTO{scene.cell_id()}, evidence, std::nullopt},
      std::nullopt});
  ASSERT_TRUE(unchanged.success) << unchanged.error->message;
  EXPECT_EQ(unchanged.update_status, core::SceneUpdateStatusDTO::kNoOp);
  const auto revision = owner.CurrentProjectionRevision();
  const auto authored = owner.Commit(core::SceneTransactionRequestDTO{
      SceneRequestContext("authored-attached-pose", revision),
      {},
      revision,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("carried-box"), world.value(),
          core::CellSceneFrameRefDTO{scene.cell_id()}, std::nullopt,
          std::nullopt},
      core::AdmissionGeneration(1U)});
  ASSERT_FALSE(authored.success);
  EXPECT_EQ(authored.error->details.at("reason"), "scene_object_is_attached");
  const auto replay = apply("continuous-attached", 4.0, false);
  // A changed expected projection is a changed request, never a repeated
  // effect.
  ASSERT_FALSE(replay.success);

  // A pose in the attachment's own carrier frame is the carried offset.
  const auto carrier_update = [&](const std::string &id,
                                  const core::SceneTransactionFrameRefDTO &frame) {
    const auto expected = owner.CurrentProjectionRevision();
    return owner.Commit(core::SceneTransactionRequestDTO{
        SceneRequestContext(id, expected),
        {},
        expected,
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId("carried-box"),
            core::Pose3DTO{{0.5, 0.0, 0.1}, {0, 0, 0, 1}}, frame,
            scene.committed_evidence(), true},
        std::nullopt});
  };
  const auto carrier_frame =
      core::ToSceneTransactionFrameRef(before.carrier_frame_ref);
  const auto relative = carrier_update("carrier-frame-offset", carrier_frame);
  ASSERT_TRUE(relative.success) << relative.error->message;
  const auto offset =
      owner.LogicalSnapshot()->carried_object_attachments.front();
  EXPECT_EQ(offset.carrier_frame_ref, before.carrier_frame_ref);
  EXPECT_EQ(offset.pose_in_carrier_frame,
            (core::Pose3DTO{{0.5, 0.0, 0.1}, {0, 0, 0, 1}}));
  const auto carried = owner.GetObjectPose(core::SceneResourceId("carried-box"));
  ASSERT_TRUE(carried.ok());
  EXPECT_DOUBLE_EQ(carried.value().position_m.x_m, 1.5);
  EXPECT_DOUBLE_EQ(carried.value().position_m.z_m, 3.1);
  const auto same = carrier_update("carrier-frame-unchanged", carrier_frame);
  ASSERT_TRUE(same.success) << same.error->message;
  EXPECT_EQ(same.update_status, core::SceneUpdateStatusDTO::kNoOp);
  const auto foreign = carrier_update(
      "carrier-frame-foreign",
      core::ComponentSceneFrameRefDTO{core::ControllableComponentId("component.1"),
                                      core::ModelInstanceId("model.1"),
                                      core::FrameRoleId("other_frame")});
  ASSERT_FALSE(foreign.success);
  EXPECT_EQ(foreign.error->code, "missing_reference");
  EXPECT_NO_THROW(static_cast<void>(owner.VerifyIntegrity()));
}

TEST(CanonicalEmptySceneOwnerProviderFreshness,
     AttachKeepsGraspLostBoundWithAttachTimeOffset) {
  ProviderFreshnessScene scene;
  auto &owner = scene.owner();
  const core::ComponentAttachmentRevision structural_revision(
      "component-attachment-revision-1");
  for (auto &participant : scene.projections()) {
    participant->SetFrameResolution(
        core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}},
        {structural_revision});
  }
  auto add = AddBoxRequest(owner.CurrentProjectionRevision(), scene.cell_id(),
                           "scene-request-grasp-bound-add");
  auto &added_object = std::get<core::AddObjectRequestDTO>(add.operation);
  added_object.scene_resource_id = core::SceneResourceId("carried-box");
  added_object.pose = core::Pose3DTO{{1.5, 2.0, 3.25}, {0, 0, 0, 1}};
  ASSERT_TRUE(owner.Commit(add).success);
  const auto attach_with = [&](const std::string &id,
                               const std::optional<core::CarriedObjectGraspLostBoundDTO> &bound,
                               const std::optional<core::GraspLostResponseDTO>
                                   &response = std::nullopt) {
    const auto active = owner.CurrentProjectionRevision();
    core::SceneTransactionRequestDTO attach{
        SceneRequestContext(id, active),
        {},
        active,
        core::AttachCarriedObjectRequestDTO{
            core::CarriedObjectAttachmentId("attachment.1"),
            core::SceneResourceId("carried-box"),
            core::ControllableComponentId("component.1"),
            core::CommandTargetId("arm"),
            core::FrameRoleId("tool_frame"),
            {core::CollisionBodyRoleId("finger")},
            {structural_revision},
            bound,
            response,
        },
        core::AdmissionGeneration(1U),
    };
    attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
        core::ProgramRunNumber(2U),
        core::CellAdmissionClaimGeneration(9U),
        "attach",
        core::NodeAttemptNumber(1U),
        "scene.carried_attachment"};
    return owner.Commit(attach);
  };
  const auto invalid = attach_with(
      "grasp-bound-invalid", core::CarriedObjectGraspLostBoundDTO{0.0, 0.26});
  ASSERT_FALSE(invalid.success);
  EXPECT_FALSE(
      owner.GetObjectState(core::SceneResourceId("carried-box")).attached);
  // A non-default response needs a bound.
  const core::GraspLostResponseDTO keyed{false, false, "part_lost", true};
  const auto unbounded =
      attach_with("grasp-response-unbounded", std::nullopt, keyed);
  ASSERT_FALSE(unbounded.success);
  EXPECT_FALSE(
      owner.GetObjectState(core::SceneResourceId("carried-box")).attached);
  // An alarm without the Stop is refused.
  const auto alarm_without_stop =
      attach_with("grasp-response-alarm-no-stop",
                  core::CarriedObjectGraspLostBoundDTO{0.03, 0.26},
                  core::GraspLostResponseDTO{true, false, std::nullopt, true});
  ASSERT_FALSE(alarm_without_stop.success);
  EXPECT_FALSE(
      owner.GetObjectState(core::SceneResourceId("carried-box")).attached);

  const auto attached = attach_with(
      "grasp-bound-attach", core::CarriedObjectGraspLostBoundDTO{0.03, 0.26},
      keyed);
  ASSERT_TRUE(attached.success)
      << (attached.error.has_value() ? attached.error->message
                                     : "missing error");
  const core::Pose3DTO attach_offset{{0.5, 0.0, 0.25}, {0, 0, 0, 1}};
  const auto row = owner.LogicalSnapshot()->carried_object_attachments.front();
  EXPECT_EQ(row.pose_in_carrier_frame, attach_offset);
  ASSERT_TRUE(row.grasp_lost_check.has_value());
  EXPECT_EQ(row.grasp_lost_check->bound,
            (core::CarriedObjectGraspLostBoundDTO{0.03, 0.26}));
  EXPECT_EQ(row.grasp_lost_check->on_grasp_lost, std::optional(keyed));

  // A measured update moves the offset; the bound stays.
  const auto expected = owner.CurrentProjectionRevision();
  const auto updated = owner.Commit(core::SceneTransactionRequestDTO{
      SceneRequestContext("grasp-bound-update", expected),
      {},
      expected,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("carried-box"),
          core::Pose3DTO{{0.51, 0.0, 0.25}, {0, 0, 0, 1}},
          core::ToSceneTransactionFrameRef(row.carrier_frame_ref),
          scene.committed_evidence(), true},
      std::nullopt});
  ASSERT_TRUE(updated.success) << updated.error->message;
  const auto moved = owner.LogicalSnapshot()->carried_object_attachments.front();
  EXPECT_DOUBLE_EQ(moved.pose_in_carrier_frame.position_m.x_m, 0.51);
  EXPECT_EQ(moved.grasp_lost_check, row.grasp_lost_check);
  EXPECT_NO_THROW(static_cast<void>(owner.VerifyIntegrity()));
}

TEST(CanonicalEmptySceneOwnerProviderFreshness,
     HistoryCompactionIsTransparentToCallers) {
  ProviderFreshnessScene scene;
  auto &owner = scene.owner();
  auto other = AddBoxRequest(owner.CurrentProjectionRevision(),
                             scene.cell_id(), "scene-request-compaction-add");
  std::get<core::AddObjectRequestDTO>(other.operation).scene_resource_id =
      core::SceneResourceId("other-box");
  const auto added = owner.Commit(other);
  ASSERT_TRUE(added.success);
  auto other_pose = owner.GetObjectPose(core::SceneResourceId("other-box"));
  ASSERT_TRUE(other_pose.ok());
  // OD-378: three large meshes bring the Scene near
  // kMaximumLogicalSceneSnapshotBytes; the fold at the transaction count
  // still takes it.
  for (const auto *name : {"fold-1", "fold-2", "fold-3"}) {
    const auto large = owner.Commit(AddLargeMeshRequest(
        owner.CurrentProjectionRevision(), scene.cell_id(), name));
    ASSERT_TRUE(large.success)
        << (large.error.has_value() ? large.error->message : "missing error");
  }
  const auto large_scene = owner.LogicalSnapshot();
  const auto large_scene_bytes =
      EncodeLogicalSceneSnapshot(*large_scene).size();
  ASSERT_GT(large_scene_bytes, 3U * kLargeMeshUriBytes);
  ASSERT_LT(large_scene_bytes, kMaximumLogicalSceneSnapshotBytes);

  // Compaction is transparent: every commit succeeds, the commit that
  // triggers it included.
  const auto resident = static_cast<std::size_t>(
      owner.VerifyIntegrity().transaction_intent_count);
  ASSERT_LT(resident, kMaximumLogicalSceneTransactions);
  for (std::size_t index = 0U;
       index <= kMaximumLogicalSceneTransactions - resident; ++index) {
    auto pose = other_pose.value();
    pose.position_m.x_m += 0.001 * static_cast<double>(index + 1U);
    const auto current = owner.CurrentProjectionRevision();
    const core::SceneTransactionRequestDTO move{
        core::PublicRequestContextDTO{
            SceneRequestKey("scene-request-compaction-move-" +
                            std::to_string(index)),
            "scene-owner-test"},
        {},
        current,
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId("other-box"), pose,
            core::CellSceneFrameRefDTO{scene.cell_id()},
            scene.committed_evidence(), false},
        std::nullopt,
    };
    const auto moved = owner.Commit(move);
    ASSERT_TRUE(moved.success)
        << index << " "
        << (moved.error.has_value() ? moved.error->message : "missing error");
  }
  // The last commit compacted the resident history and then committed itself.
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 1);
  EXPECT_EQ(owner.VerifyIntegrity().request_outcome_count, 1);
  // The folded Scene keeps every object.
  EXPECT_EQ(owner.LogicalSnapshot()->objects.size(),
            large_scene->objects.size());
}

TEST(CanonicalEmptySceneOwner,
     ProgramSceneCompactionKeepsStableAdmissionAndSettlesBeforePruning) {
  TemporaryDirectory directory("scene-program-compaction");
  const auto package_root = directory.path() / "packages";
  CopyCanonicalEmptyPackage(package_root);
  const auto admission = LoadAdmission(package_root);
  const auto candidate = deployment::test::
      materialize_lite6_ros2_control_motion_retained_consumer_fixture_for_cell(
          package_root, admission.cell_id().value());
  const auto bindings = ExecutionHostBindings(
      candidate.bundle->value(), "runtime-epoch-program-compaction");
  auto projections = MakeFakeProjections(bindings);
  FakePlannerSceneProjectionResolver resolver(ProjectionClients(projections));
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);

  SceneAdmissionClock clock;
  SceneAdmissionJournal journal;
  core::ManagerLivenessRequiredSetDTO liveness{
      admission.cell_id(),
      core::RetainedBundleId(Digest('a').value()),
      Digest('b'),
      core::RuntimeUnitId("manager"),
      {}};
  liveness.required_runtime_set_digest =
      core::ComputeRequiredRuntimeSetDigest(liveness.required_receivers);
  const core::ProgramRunNumber run_id(2U);
  std::optional<core::CellAdmissionClaimGeneration> parent_generation;
  core::KernelCellAdmissionOwner cell_admission(
      512U, clock,
      [&journal](const std::span<core::JournalRecord> records) {
        journal.Post(records);
      },
      [](const auto &, const auto &) { return Result<void>::Ok(); },
      // As the execution host: the owner's exact release is the whole proof.
      [](const auto &, const auto &) { return Result<void>::Ok(); },
      liveness,
      [&](const core::ProgramNodeAdmissionEvidenceDTO &evidence) {
        if (!parent_generation.has_value() ||
            evidence.program_run_id != run_id ||
            evidence.parent_program_cycle_claim_generation !=
                *parent_generation) {
          return Result<void>::Err({"program_node_parent_not_current",
                                    "scene test node evidence is not exact",
                                    {}});
        }
        return Result<void>::Ok();
      });
  ReleaseUnavailableAdmission scene_admission(cell_admission);

  auto readiness = core::CellAdmissionReadinessSnapshotDTO{
      true,
      core::ProgramCycleStatusDTO{1U,
                                  core::ProgramCycleStateDTO::kIdleAfterReset,
                                  std::nullopt, std::nullopt, std::nullopt},
      true,
      2U,
      2U,
      true,
      true,
      0U,
      0U,
      0U};
  ASSERT_TRUE(cell_admission.UpdateCellAdmissionReadiness(readiness).ok());

  std::optional<ProjectionRevisionDTO> final_revision;
  std::optional<core::LogicalSceneSnapshotDTO> final_snapshot;
  std::optional<core::SceneTransactionRequestDTO> request_to_settle;
  std::optional<core::CellAdmissionClaimDTO> parent_claim;
  core::CellAdmissionClaimIntentDTO parent_intent{
      core::CellAdmissionClaimKindDTO::kProgramCycle,
      core::ApplicationRole::kProgramRunner,
      run_id,
      SceneRequestKey("program-cycle-claim"),
      core::AdmissionGeneration(0U),
      {},
      core::ProgramCycleAdmissionEvidenceDTO{
          core::ProgramRevision("program.rev"),
          core::ProgramRevision("program.rev")},
      std::nullopt,
      std::nullopt};

  const auto node_evidence = [&](const std::string &node_id,
                                 const std::uint64_t attempt_id,
                                 const std::string &owner_api) {
    return core::ProgramNodeAdmissionEvidenceDTO{
        run_id, *parent_generation, node_id,
        core::NodeAttemptNumber(attempt_id), owner_api};
  };
  const auto make_pose_update = [&](LogicalSceneOwner &owner,
                                    const core::RequestKey &request_key,
                                    const std::uint64_t attempt_id) {
    auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box"));
    if (!pose.ok())
      throw std::runtime_error(pose.error().message);
    pose.value().position_m.x_m += 0.001;
    core::SceneTransactionRequestDTO request{
        core::PublicRequestContextDTO{request_key, "scene-owner-test"},
        {},
        owner.CurrentProjectionRevision(),
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId("scene-box"), pose.value(),
            core::CellSceneFrameRefDTO{admission.cell_id()}, std::nullopt,
            false},
        std::nullopt};
    request.expected_admission_generation =
        cell_admission.GetAuthoringAdmission().admission_generation;
    request.program_node_evidence =
        node_evidence("pose", attempt_id, "scene.transaction");
    return request;
  };
  const auto scene_claim_intent =
      [&](const core::SceneTransactionRequestDTO &request) {
        return core::CellAdmissionClaimIntentDTO{
            core::CellAdmissionClaimKindDTO::kAuthoredScene,
            core::ApplicationRole::kAuthoredSceneOwner,
            std::nullopt,
            request.request_context.request_key,
            *request.expected_admission_generation,
            {},
            std::nullopt,
            std::nullopt,
            request.program_node_evidence};
      };
  std::optional<core::SceneTransactionRequestDTO> reused_external_request;

  LogicalSceneOwner owner(admission, publication_reader, bundle_source,
                          resolver, scene_admission);
  auto active = FinalizeNativeCandidate(owner, publication_reader, admission,
                                        *candidate.bundle, bindings,
                                        111U);
  ResetProjections(projections, active,
                   FakePlannerSceneProjection::Mode::kCommit);

  auto add = AddBoxRequest(active, admission.cell_id(),
                           "scene-program-compaction-add");
  add.expected_admission_generation =
      cell_admission.GetAuthoringAdmission().admission_generation;
  const auto added = owner.Commit(add);
  ASSERT_TRUE(added.success)
      << (added.error.has_value() ? added.error->message : "missing error");

  readiness.program_cycle_status = core::ProgramCycleStatusDTO{
      2U, core::ProgramCycleStateDTO::kStarting, run_id, std::nullopt,
      std::nullopt};
  ASSERT_TRUE(cell_admission.UpdateCellAdmissionReadiness(readiness).ok());
  parent_intent.expected_admission_generation =
      cell_admission.GetAuthoringAdmission().admission_generation;
  const auto acquired_parent =
      cell_admission.AcquireCellAdmissionClaim(parent_intent);
  ASSERT_TRUE(acquired_parent.ok()) << acquired_parent.error().message;
  parent_claim = acquired_parent.value();
  parent_generation = parent_claim->claim_generation;
  readiness.program_cycle_status =
      core::ProgramCycleStatusDTO{3U,
                                  core::ProgramCycleStateDTO::kRunning,
                                  run_id,
                                  parent_claim->claim_generation,
                                  std::nullopt};
  ASSERT_TRUE(cell_admission.UpdateCellAdmissionReadiness(readiness).ok());

  const auto initial_count = static_cast<std::size_t>(
      owner.VerifyIntegrity().transaction_intent_count);
  ASSERT_LT(initial_count, kMaximumLogicalSceneTransactions - 1U);
  for (std::size_t index = initial_count;
       index < kMaximumLogicalSceneTransactions - 1U; ++index) {
    const auto request = make_pose_update(
        owner,
        SceneRequestKey("scene-program-compaction-pose-" +
                        std::to_string(index)),
        index + 1U);
    const auto moved = owner.Commit(request);
    ASSERT_TRUE(moved.success)
        << index << " "
        << (moved.error.has_value() ? moved.error->message : "missing error");
    if (index == initial_count)
      reused_external_request = request;
  }
  ASSERT_EQ(owner.VerifyIntegrity().transaction_intent_count,
            static_cast<std::int64_t>(kMaximumLogicalSceneTransactions - 1U));

  auto unknown_release =
      make_pose_update(owner,
                       SceneRequestKey("scene-program-compaction-release-unknown"),
                       1001U);
  scene_admission.FailNextRelease();
  EXPECT_THROW(owner.Commit(unknown_release), std::runtime_error);
  ASSERT_EQ(owner.VerifyIntegrity().transaction_intent_count,
            static_cast<std::int64_t>(kMaximumLogicalSceneTransactions));
  const auto unresolved = cell_admission.GetCellAdmissionRecoveryReceipt(
      {scene_claim_intent(unknown_release), std::nullopt, std::nullopt});
  ASSERT_TRUE(unresolved.ok());
  ASSERT_EQ(unresolved.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  ASSERT_TRUE(unresolved.value().matched_claim.has_value());
  const auto unknown_claim = *unresolved.value().matched_claim;
  EXPECT_EQ(unknown_claim.intent, scene_claim_intent(unknown_release));

  auto compaction_probe =
      make_pose_update(owner, SceneRequestKey("scene-program-compaction-retry"),
                       1002U);
  const auto before_blocked_compaction = owner.VerifyIntegrity();
  scene_admission.FailNextRelease();
  const auto blocked = owner.Commit(compaction_probe);
  ASSERT_FALSE(blocked.success);
  ASSERT_TRUE(blocked.error.has_value());
  EXPECT_EQ(blocked.error->code, "kernel_owner_unavailable");
  const auto after_blocked_compaction = owner.VerifyIntegrity();
  EXPECT_EQ(after_blocked_compaction.transaction_intent_count,
            before_blocked_compaction.transaction_intent_count);
  EXPECT_EQ(after_blocked_compaction.request_outcome_count,
            before_blocked_compaction.request_outcome_count);
  const auto still_held = cell_admission.GetCellAdmissionRecoveryReceipt(
      {scene_claim_intent(unknown_release), std::nullopt, std::nullopt});
  ASSERT_TRUE(still_held.ok());
  EXPECT_EQ(still_held.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);

  const auto compaction_probe_intent = scene_claim_intent(compaction_probe);
  journal.ObserveClaimReleaseThenAcquire(unknown_claim,
                                         compaction_probe_intent);

  // Compaction is transparent: the triggering request settles the held
  // claim, prunes, and then continues. Settling
  // advanced the admission generation; the request's own claim still names
  // the older one, which is evidence only, so it commits without a refresh.
  const auto revision_before_compaction = owner.CurrentProjectionRevision();
  const auto cleanup_started = std::chrono::steady_clock::now();
  const auto compacted = owner.Commit(compaction_probe);
  const auto cleanup_elapsed =
      std::chrono::steady_clock::now() - cleanup_started;
  RecordProperty(
      "scene_compaction_settlement_us",
      std::chrono::duration_cast<std::chrono::microseconds>(cleanup_elapsed)
          .count());
  ASSERT_TRUE(compacted.success)
      << (compacted.error.has_value() ? compacted.error->message
                                      : "missing error");
  EXPECT_EQ(compacted.replay_status,
            core::RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(compacted.projection_status,
            core::SceneProjectionCommitStatusDTO::kCommitted);
  EXPECT_NE(owner.CurrentProjectionRevision(), revision_before_compaction);
  const auto after_compaction = owner.VerifyIntegrity();
  EXPECT_EQ(after_compaction.transaction_intent_count, 1);
  EXPECT_EQ(after_compaction.request_outcome_count, 1);
  ASSERT_TRUE(journal.observed_released_claim().has_value());
  EXPECT_EQ(*journal.observed_released_claim(), unknown_claim);
  ASSERT_TRUE(journal.observed_release().has_value());
  const auto &recorded_release = *journal.observed_release();
  EXPECT_EQ(recorded_release.owner_role, unknown_claim.intent.owner_role);
  EXPECT_EQ(recorded_release.owner_transaction_id,
            unknown_claim.intent.owner_transaction_id);
  EXPECT_EQ(recorded_release.request_key, unknown_claim.intent.request_key);
  EXPECT_EQ(recorded_release.acquired_admission_generation,
            unknown_claim.acquired_admission_generation);
  EXPECT_EQ(recorded_release.claim_generation,
            unknown_claim.claim_generation);
  ASSERT_TRUE(journal.observed_release_sequence().has_value());
  ASSERT_TRUE(journal.observed_acquired_claim().has_value());
  EXPECT_EQ(journal.observed_acquired_claim()->intent,
            compaction_probe_intent);
  ASSERT_TRUE(journal.observed_acquire_sequence().has_value());
  EXPECT_LT(*journal.observed_release_sequence(),
            *journal.observed_acquire_sequence());

  const auto retired_old_receipt =
      cell_admission.GetCellAdmissionRecoveryReceipt(
          {scene_claim_intent(unknown_release), std::nullopt, std::nullopt});
  ASSERT_TRUE(retired_old_receipt.ok());
  EXPECT_EQ(retired_old_receipt.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown);
  EXPECT_FALSE(retired_old_receipt.value().matched_claim.has_value());

  // The next request reads the current admission generation and commits;
  // its resend replays exactly.
  const auto next = make_pose_update(
      owner, SceneRequestKey("scene-program-compaction-next"), 1003U);
  const auto committed_next = owner.Commit(next);
  ASSERT_TRUE(committed_next.success)
      << (committed_next.error.has_value() ? committed_next.error->message
                                           : "missing error");
  const auto exact_replay = owner.Commit(next);
  ASSERT_TRUE(exact_replay.success);
  EXPECT_EQ(exact_replay.replay_status,
            core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(exact_replay.transaction_sequence,
            committed_next.transaction_sequence);

  // A request ID whose outcome was pruned is evaluated as a new request
  // against the latest scene.
  ASSERT_TRUE(reused_external_request.has_value());
  auto reused_id = make_pose_update(
      owner, reused_external_request->request_context.request_key, 1004U);
  EXPECT_EQ(reused_id.request_context.request_key,
            reused_external_request->request_context.request_key);
  const auto reused = owner.Commit(reused_id);
  ASSERT_TRUE(reused.success)
      << (reused.error.has_value() ? reused.error->message : "missing error");
  const auto reused_claim = cell_admission.GetCellAdmissionRecoveryReceipt(
      {scene_claim_intent(reused_id), std::nullopt, std::nullopt});
  ASSERT_TRUE(reused_claim.ok());
  EXPECT_EQ(reused_claim.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);

  const core::ComponentAttachmentRevision structural_revision(
      "program-compaction-attachment-revision");
  for (auto &participant : projections) {
    participant->SetFrameResolution(
        core::Pose3DTO{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0, 1.0}},
        {structural_revision});
    participant->SetResolvedFrameRoleOverride(
        core::FrameRoleId("asset_tool_frame"));
  }
  active = owner.CurrentProjectionRevision();
  core::SceneTransactionRequestDTO attach{
      core::PublicRequestContextDTO{
          SceneRequestKey("scene-program-compaction-attach"),
          "scene-owner-test"},
      {},
      active,
      core::AttachCarriedObjectRequestDTO{
          core::CarriedObjectAttachmentId("attachment.compaction"),
          core::SceneResourceId("scene-box"),
          core::ControllableComponentId("component.1"),
          core::CommandTargetId("arm"),
          core::FrameRoleId("tool_frame"),
          {core::CollisionBodyRoleId("finger")},
          {structural_revision}},
      std::nullopt};
  attach.expected_admission_generation =
      cell_admission.GetAuthoringAdmission().admission_generation;
  attach.program_node_evidence = node_evidence(
      "attach", 1005U, "scene.carried_attachment");
  const auto attached = owner.Commit(attach);
  ASSERT_TRUE(attached.success)
      << (attached.error.has_value() ? attached.error->message
                                     : "missing error");
  ASSERT_TRUE(attached.committed_projection_revision.has_value());
  const auto after_attach = owner.LogicalSnapshot();
  ASSERT_EQ(after_attach->carried_object_attachments.size(), 1U);
  const auto attachment = after_attach->carried_object_attachments.front();

  core::SceneTransactionRequestDTO detach{
      core::PublicRequestContextDTO{
          SceneRequestKey("scene-program-compaction-detach"),
          "scene-owner-test"},
      {},
      *attached.committed_projection_revision,
      core::DetachCarriedObjectAtCurrentPoseRequestDTO{
          attachment.carried_object_attachment_id,
          attachment.carried_object_attachment_revision,
          attachment.scene_resource_id, attachment.controllable_component_id,
          attachment.command_target_id},
      std::nullopt};
  detach.expected_admission_generation =
      cell_admission.GetAuthoringAdmission().admission_generation;
  detach.program_node_evidence = node_evidence(
      "detach", 1006U, "scene.carried_attachment");
  const auto detached = owner.Commit(detach);
  ASSERT_TRUE(detached.success)
      << (detached.error.has_value() ? detached.error->message
                                     : "missing error");

  auto last_release_unknown = make_pose_update(
      owner,
      SceneRequestKey("scene-program-compaction-last-release-unknown"),
      1007U);
  request_to_settle = last_release_unknown;
  scene_admission.FailNextRelease();
  EXPECT_THROW(owner.Commit(last_release_unknown), std::runtime_error);
  const auto held_until_reconciled =
      cell_admission.GetCellAdmissionRecoveryReceipt(
          {scene_claim_intent(last_release_unknown), std::nullopt,
           std::nullopt});
  ASSERT_TRUE(held_until_reconciled.ok());
  ASSERT_EQ(held_until_reconciled.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);

  final_revision = owner.CurrentProjectionRevision();
  final_snapshot = *owner.LogicalSnapshot();
  EXPECT_TRUE(final_snapshot->carried_object_attachments.empty());
  EXPECT_EQ(final_snapshot->revision, final_revision->scene_revision);

  const auto parent_receipt = cell_admission.GetCellAdmissionRecoveryReceipt(
      {parent_intent, *parent_claim, std::nullopt});
  ASSERT_TRUE(parent_receipt.ok());
  EXPECT_EQ(parent_receipt.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
  EXPECT_EQ(cell_admission.GetAuthoringAdmission().authoring_allowed, false);

  const auto reconciliation_started = std::chrono::steady_clock::now();
  const auto reconciled = owner.ReconcileAuthoredSceneAdmissionClaims();
  const auto reconciliation_elapsed =
      std::chrono::steady_clock::now() - reconciliation_started;
  RecordProperty("scene_admission_settlement_us",
                 std::chrono::duration_cast<std::chrono::microseconds>(
                     reconciliation_elapsed)
                     .count());
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  ASSERT_TRUE(final_revision.has_value());
  ASSERT_TRUE(final_snapshot.has_value());
  EXPECT_EQ(owner.CurrentProjectionRevision(), *final_revision);
  const auto head_snapshot = owner.LogicalSnapshot();
  ASSERT_NE(head_snapshot, nullptr);
  EXPECT_EQ(*head_snapshot, *final_snapshot);
  EXPECT_TRUE(head_snapshot->carried_object_attachments.empty());
  ASSERT_TRUE(request_to_settle.has_value());
  const auto settled = cell_admission.GetCellAdmissionRecoveryReceipt(
      {scene_claim_intent(*request_to_settle), std::nullopt, std::nullopt});
  ASSERT_TRUE(settled.ok());
  EXPECT_EQ(settled.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact);
  const auto parent_receipt_after =
      cell_admission.GetCellAdmissionRecoveryReceipt(
          {parent_intent, *parent_claim, std::nullopt});
  ASSERT_TRUE(parent_receipt_after.ok());
  EXPECT_EQ(parent_receipt_after.value().status,
            core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact);
}

// OD-202: a failed Apply ends its own Scene adoption. (A) failed after its
// publication, before the Scene completion; (B) failed after the completion
// committed, with the capture still set; (4b) failed after Begin, before any
// publication. Each time the Scene reads work at once, and New's own Apply
// for the empty cell runs Stage, Begin and Complete.
TEST(CanonicalEmptySceneOwner, FailedApplyEndsItsSceneAdoptionAndNewAdoptsTheEmptyCell) {
  enum class Failure { kAfterPublication, kAfterCompletionCommit, kBeforePublication };
  for (const auto failure : {Failure::kAfterPublication, Failure::kAfterCompletionCommit,
                             Failure::kBeforePublication}) {
    SCOPED_TRACE(static_cast<int>(failure));
    SceneAdoptionFixture fixture;
    const auto added = fixture.owner->Commit(AddBoxRequest(
        fixture.owner->CurrentProjectionRevision(), fixture.admission.cell_id(), "end-add"));
    ASSERT_TRUE(added.success);
    const auto snapshot_bytes = EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot());
    const auto request = fixture.CaptureConfigurationRequest();
    fixture.PrepareCandidateClients();
    static_cast<void>(fixture.owner->StageOriginalManagerConfigurationAdoption(
        request, fixture.fence, fixture.fence.OriginalAttemptQuery()));
    const auto ready =
        fixture.owner->BeginOriginalManagerConfigurationAdoption(request, fixture.fence);
    ASSERT_TRUE(ready.ready_for_publication());
    const auto &owner = request.authority.owner;
    const core::ProjectionMaintenanceInitializationQueryDTO query{owner.apply_id,
                                                                 owner.apply_generation};
    std::optional<ActiveBundlePublicationReceiptDTO> publication;
    if (failure != Failure::kBeforePublication) {
      publication = fixture.Publication(request);
      fixture.publication_reader.SetCurrent(*publication);
    }
    if (failure == Failure::kAfterCompletionCommit) {
      // The held parent is checked twice before the commit and once after.
      fixture.fence.RefuseRemoteQueryAfter(2U);
      EXPECT_THROW(fixture.owner->CompleteOriginalManagerConfigurationAdoption(
                       request, *publication, fixture.fence),
                   std::runtime_error);
      ASSERT_EQ(fixture.owner->ReadMaintenanceInitialization(query)->state,
                core::ProjectionMaintenanceInitializationState::kCompleted);
      fixture.fence.RefuseRemoteQueryAfter(100U);
    } else {
      // The open adoption closes the leases until the Apply ends.
      EXPECT_THROW(static_cast<void>(fixture.owner->CurrentProjectionRevision()),
                   LogicalSceneOwnerConflictError);
    }

    fixture.owner->EndOriginalManagerConfigurationAdoption(owner.apply_id, owner.apply_generation,
                                                           publication);
    const auto ended = fixture.owner->ReadMaintenanceInitialization(query);
    ASSERT_TRUE(ended.has_value());
    if (failure == Failure::kBeforePublication) {
      EXPECT_EQ(ended->state, core::ProjectionMaintenanceInitializationState::kInterrupted);
      EXPECT_TRUE(ended->effects_unknown);
      ASSERT_TRUE(ended->first_error.has_value());
      EXPECT_EQ(ended->first_error->code, "scene_adoption_ended_without_publication");
      EXPECT_FALSE(ended->publication_receipt.has_value());
      EXPECT_EQ(fixture.owner->CurrentProjectionRevision(),
                request.expected_active_projection_revision);
    } else {
      EXPECT_EQ(ended->state, core::ProjectionMaintenanceInitializationState::kCompleted);
      EXPECT_TRUE(core::SameProjectionIdentity(fixture.owner->CurrentProjectionRevision(),
                                         publication->request.candidate_projection_revision));
    }
    EXPECT_TRUE(fixture.owner->ReadAccessStatus().valid());
    EXPECT_TRUE(fixture.owner->ReadAccessStatus().scene_writes_open);
    EXPECT_EQ(EncodeLogicalSceneSnapshot(*fixture.owner->LogicalSnapshot()), snapshot_bytes);
    EXPECT_TRUE(fixture.owner->GetObjectPose(core::SceneResourceId("scene-box")).ok());
    // Ending again changes nothing.
    fixture.owner->EndOriginalManagerConfigurationAdoption(owner.apply_id, owner.apply_generation,
                                                           publication);
    EXPECT_EQ(fixture.owner->ReadMaintenanceInitialization(query), ended);

    // New's release frees the failed Apply's claim.
    const auto &held = request.authority.held_cell_claim;
    ASSERT_TRUE(fixture.cell_admission.ReleaseCellAdmissionClaim(
        {held.intent.owner_role, held.intent.owner_transaction_id, held.intent.request_key,
         held.acquired_admission_generation, held.claim_generation,
         {core::CellAdmissionSettlementKindDTO::kExplicitReset, Digest('e')}}).ok());

    // New's own Apply for the empty cell.
    const core::ApplyFenceOwnerDTO manager{ApplyId(202U), ApplyGeneration(1U)};
    const auto claim = fixture.cell_admission.AcquireCellAdmissionClaim(
        {core::CellAdmissionClaimKindDTO::kManagerApply, core::ApplicationRole::kManager,
         manager.apply_id, SceneRequestKey("new.request"), core::AdmissionGeneration(1U), {},
         std::nullopt, std::nullopt});
    ASSERT_TRUE(claim.ok());
    const auto head = ActiveBundlePublicationGeneration(
        failure == Failure::kBeforePublication ? 2U : 3U);
    const auto previous = fixture.publication_reader.ReadActiveBundlePublication(head);
    ASSERT_TRUE(previous.ok());
    const core::ConfigurationFenceStatusQueryDTO new_query{
        manager, core::ConfigurationFenceId("new.fence"), core::ConfigurationFenceGeneration(1U),
        {}};
    fixture.fence.Configure(new_query, previous.value());
    const QualifiedSceneConfigurationAdoptionRequest empty_cell{
        {manager, claim.value(), new_query}, previous.value(),
        fixture.owner->CurrentProjectionRevision(), fixture.owner->LogicalSnapshot()->identity(),
        fixture.admission.retained_bundle().value(), {}};
    static_cast<void>(fixture.owner->StageOriginalManagerConfigurationAdoption(
        empty_cell, fixture.fence, fixture.fence.OriginalAttemptQuery()));
    const auto new_ready =
        fixture.owner->BeginOriginalManagerConfigurationAdoption(empty_cell, fixture.fence);
    ASSERT_TRUE(new_ready.ready_for_publication());
    const ActiveBundlePublicationReceiptDTO new_publication{
        {NamedApplyPublicationTransactionDTO{manager.apply_id, manager.apply_generation},
         new_ready.target_projection_revision,
         new_ready.target_projection_revision.retained_bundle_id,
         new_ready.target_projection_revision.graph_revision, previous.value().publication_generation,
         Digest('7')},
        ActiveBundlePublicationGeneration(head.value() + 1U), Digest('8')};
    fixture.publication_reader.SetCurrent(new_publication);
    const auto new_completed = fixture.owner->CompleteOriginalManagerConfigurationAdoption(
        empty_cell, new_publication, fixture.fence);
    EXPECT_EQ(new_completed.state, core::ProjectionMaintenanceInitializationState::kCompleted);
    EXPECT_EQ(fixture.owner->CurrentProjectionRevision(), new_ready.target_projection_revision);
  }
}

// A bounded wait for an owner call that must not block (OD-376): a
// regression fails fast instead of hanging the suite.
constexpr auto kOwnerCallBound = 10s;

template <typename Value>
[[nodiscard]] bool ReadyWithinBound(const std::future<Value> &future) {
  return future.wait_for(kOwnerCallBound) == std::future_status::ready;
}

[[nodiscard]] std::string CommitError(const core::SceneCommitResultDTO &result) {
  return result.error.has_value() ? result.error->message : "missing error";
}

// OD-376: per projection, a Commit makes one preflight call (health and,
// unless the delta only moves an object, the candidate's effective policy)
// and one native apply, and nothing else; an authored change acquires and
// releases one admission claim directly, with no recovery lookup; a
// runtime-owned change claims nothing.
TEST(CanonicalEmptySceneOwner,
     CommitMakesOnePreflightOneApplyAndOneClaimPairPerProjection) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  auto &kernel = fixture.cell_admission;
  const auto &projections = fixture.predecessor_projections;
  ASSERT_FALSE(projections.empty());
  const auto cell = fixture.admission.cell_id();
  const auto acquires = kernel.acquire_calls();
  const auto releases = kernel.release_calls();
  const auto lookups = kernel.recovery_receipt_calls();
  const auto expect_per_projection = [&](const std::size_t commits,
                                         const std::size_t policies) {
    for (const auto &projection : projections) {
      EXPECT_EQ(projection->preflight_calls(), commits);
      EXPECT_EQ(projection->calls(), commits);
      EXPECT_EQ(projection->health_for_revision_calls(), commits);
      EXPECT_EQ(projection->policy_projection_calls(), policies);
      EXPECT_EQ(projection->health_calls_outside_preflight(), 0U);
      EXPECT_EQ(projection->policy_calls_outside_preflight(), 0U);
    }
  };

  const auto added = owner.Commit(AddBoxRequest(
      owner.CurrentProjectionRevision(), cell, "od376-counted-add"));
  ASSERT_TRUE(added.success) << CommitError(added);
  expect_per_projection(1U, 1U);
  EXPECT_EQ(kernel.acquire_calls(), acquires + 1U);
  EXPECT_EQ(kernel.release_calls(), releases + 1U);
  EXPECT_EQ(kernel.recovery_receipt_calls(), lookups);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);

  core::Pose3DTO pose;
  pose.position_m = {0.30, -0.5, 0.75};
  const core::SceneTransactionRequestDTO streamed{
      core::PublicRequestContextDTO{SceneRequestKey("od376-counted-pose"),
                                    "scene-owner-test"},
      {},
      owner.CurrentProjectionRevision(),
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"), pose,
          core::CellSceneFrameRefDTO{cell}, StreamedPoseEvidence(), false},
      std::nullopt};
  const auto moved = owner.Commit(streamed);
  ASSERT_TRUE(moved.success) << CommitError(moved);
  // A pose-only delta asks for health only.
  expect_per_projection(2U, 1U);
  EXPECT_EQ(kernel.acquire_calls(), acquires + 1U);
  EXPECT_EQ(kernel.release_calls(), releases + 1U);
  EXPECT_EQ(kernel.recovery_receipt_calls(), lookups);
}

// OD-376: the owner lock is not held across the native apply. While a Commit
// is inside it, readers answer at once with the settled predecessor and open
// gates, and a second Commit is refused as in progress without effect.
TEST(CanonicalEmptySceneOwner, CommitHoldsNoOwnerLockAcrossTheNativeApply) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  const auto cell = fixture.admission.cell_id();
  const auto predecessor = owner.CurrentProjectionRevision();
  const auto predecessor_snapshot = owner.LogicalSnapshot();
  const auto first = AddBoxRequest(predecessor, cell, "od376-in-flight-first");
  auto second = AddBoxRequest(predecessor, cell, "od376-in-flight-second");
  std::get<core::AddObjectRequestDTO>(second.operation).scene_resource_id =
      core::SceneResourceId("scene-second");

  struct InFlightReads final {
    std::string failure;
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> snapshot;
    std::optional<ProjectionRevisionDTO> revision;
    std::optional<CanonicalEmptySceneAccessStatus> access;
    std::optional<LogicalScenePublicationRead> publication;
    std::optional<core::LogicalSceneSnapshotRoot> active;
    std::optional<core::SceneCommitResultDTO> overlapping;
    std::optional<core::SceneCommitResultDTO> overlapping_stored;
  };
  std::barrier apply_entered(2);
  std::barrier apply_release(2);
  fixture.predecessor_projections.front()->BlockNextApply(apply_entered,
                                                          apply_release);
  auto committing =
      std::async(std::launch::async, [&] { return owner.Commit(first); });
  apply_entered.arrive_and_wait();

  // The first Commit is inside its native apply. The reads run on another
  // thread with a bounded wait, so the release below is always reached: else
  // the blocked Commit would never end.
  auto reading = std::async(std::launch::async, [&] {
    InFlightReads reads;
    try {
      reads.snapshot = owner.LogicalSnapshot();
      reads.revision = owner.CurrentProjectionRevision();
      reads.access = owner.ReadAccessStatus();
      reads.publication = owner.ReadPublication();
      reads.active = owner.ReadActiveSnapshot();
      reads.overlapping = owner.Commit(second);
      reads.overlapping_stored = owner.ReadCommitResult(second);
    } catch (const std::exception &error) {
      reads.failure = error.what();
    }
    return reads;
  });
  const bool read_in_time = ReadyWithinBound(reading);
  apply_release.arrive_and_wait();
  const auto committed = committing.get();
  const auto reads = reading.get();

  EXPECT_TRUE(read_in_time) << "an in-flight read waited for the native apply";
  EXPECT_TRUE(reads.failure.empty()) << reads.failure;
  ASSERT_NE(reads.snapshot, nullptr);
  EXPECT_EQ(*reads.snapshot, *predecessor_snapshot);
  EXPECT_EQ(reads.revision, std::optional{predecessor});
  ASSERT_TRUE(reads.access.has_value());
  EXPECT_TRUE(reads.access->scene_writes_open);
  EXPECT_TRUE(reads.access->projection_leases_open);
  EXPECT_EQ(reads.access->active_projection, std::optional{predecessor});
  ASSERT_TRUE(reads.publication.has_value());
  EXPECT_TRUE(reads.publication->access.scene_writes_open);
  ASSERT_TRUE(reads.publication->snapshot.has_value());
  EXPECT_EQ(reads.publication->snapshot->shared()->revision,
            predecessor.scene_revision);
  ASSERT_TRUE(reads.active.has_value());
  EXPECT_EQ(reads.active->shared()->revision, predecessor.scene_revision);
  ASSERT_TRUE(reads.overlapping.has_value());
  EXPECT_FALSE(reads.overlapping->success);
  ASSERT_TRUE(reads.overlapping->error.has_value());
  EXPECT_EQ(reads.overlapping->error->code, "stale_revision");
  EXPECT_EQ(reads.overlapping->error->details.at("reason"),
            "scene_commit_in_progress");
  EXPECT_EQ(reads.overlapping->projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_FALSE(reads.overlapping_stored.has_value());

  ASSERT_TRUE(committed.success) << CommitError(committed);
  ASSERT_TRUE(committed.committed_projection_revision.has_value());
  const auto successor = *committed.committed_projection_revision;
  EXPECT_NE(successor, predecessor);
  EXPECT_EQ(owner.CurrentProjectionRevision(), successor);
  const auto settled = owner.LogicalSnapshot();
  EXPECT_EQ(settled->revision, successor.scene_revision);
  EXPECT_EQ(settled->objects.size(), predecessor_snapshot->objects.size() + 1U);
  const auto access = owner.ReadAccessStatus();
  EXPECT_TRUE(access.scene_writes_open);
  EXPECT_EQ(access.active_projection, std::optional{successor});
  const auto publication = owner.ReadPublication();
  ASSERT_TRUE(publication.snapshot.has_value());
  EXPECT_EQ(publication.snapshot->shared()->revision, successor.scene_revision);
  EXPECT_GT(publication.read_sequence, reads.publication->read_sequence);
  EXPECT_FALSE(owner.ReadCommitResult(second).has_value());
  for (const auto &projection : fixture.predecessor_projections) {
    ASSERT_EQ(projection->calls(), 1U);
    EXPECT_EQ(projection->transactions().front().request_key,
              first.request_context.request_key);
  }
  EXPECT_TRUE(fixture.cell_admission.GetAuthoringAdmission().authoring_allowed);

  // The refused request took no claim and left no result: resent, it commits.
  const auto resent = owner.Commit(second);
  EXPECT_TRUE(resent.success) << CommitError(resent);
}

// OD-376: a change that reaches the owner while a request is prepared
// unlocked (here: while the last preflight answer is on its way) makes that
// request stale. It is refused before its intent: no result, no admission
// claim and no native effect.
TEST(CanonicalEmptySceneOwner,
     ConcurrentChangeDuringThePreflightIsRefusedAsStaleWithoutAClaim) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  auto &kernel = fixture.cell_admission;
  const auto cell = fixture.admission.cell_id();
  const auto predecessor = owner.CurrentProjectionRevision();
  const auto predecessor_objects = owner.LogicalSnapshot()->objects.size();
  const auto integrity_before = owner.VerifyIntegrity();
  const auto first = AddBoxRequest(predecessor, cell, "od376-concurrent-first");
  auto inner = AddBoxRequest(predecessor, cell, "od376-concurrent-inner");
  std::get<core::AddObjectRequestDTO>(inner.operation).scene_resource_id =
      core::SceneResourceId("scene-inner");
  const auto claims_before = kernel.Claims().size();
  const auto acquires_before = kernel.acquire_calls();
  const auto releases_before = kernel.release_calls();

  // The inner Commit runs inside the outer one's last preflight call, once
  // every projection answered healthy at the predecessor; it runs on another
  // thread with a bounded wait.
  std::future<core::SceneCommitResultDTO> inner_commit;
  bool inner_in_time = false;
  std::size_t preflights = 0U;
  const auto projection_count = fixture.predecessor_projections.size();
  for (const auto &projection : fixture.predecessor_projections) {
    projection->RunDuringNextPreflight([&] {
      if (++preflights != projection_count) {
        return;
      }
      inner_commit =
          std::async(std::launch::async, [&] { return owner.Commit(inner); });
      inner_in_time = ReadyWithinBound(inner_commit);
    });
  }
  const auto refused = owner.Commit(first);
  ASSERT_TRUE(inner_commit.valid());
  const auto inner_result = inner_commit.get();
  EXPECT_TRUE(inner_in_time) << "a Commit waited for another one's preflight";

  ASSERT_TRUE(inner_result.success) << CommitError(inner_result);
  ASSERT_TRUE(inner_result.committed_projection_revision.has_value());
  EXPECT_FALSE(refused.success);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "stale_revision");
  EXPECT_EQ(refused.error->details.at("reason"),
            "scene_commit_concurrent_change");
  EXPECT_EQ(refused.replay_status, core::RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(refused.projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_FALSE(refused.committed_projection_revision.has_value());
  EXPECT_FALSE(owner.ReadCommitResult(first).has_value());
  // Only the inner request was recorded.
  const auto integrity_after = owner.VerifyIntegrity();
  EXPECT_EQ(integrity_after.transaction_intent_count,
            integrity_before.transaction_intent_count + 1);
  EXPECT_EQ(integrity_after.request_outcome_count,
            integrity_before.request_outcome_count + 1);

  // Only the inner request claimed admission; the refused one never did.
  EXPECT_EQ(kernel.acquire_calls(), acquires_before + 1U);
  EXPECT_EQ(kernel.release_calls(), releases_before + 1U);
  const auto claims = kernel.Claims();
  ASSERT_EQ(claims.size(), claims_before + 1U);
  EXPECT_EQ(claims.back().intent.request_key,
            inner.request_context.request_key);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);

  // Only the inner change took effect, natively too.
  EXPECT_EQ(owner.CurrentProjectionRevision(),
            *inner_result.committed_projection_revision);
  const auto snapshot = owner.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), predecessor_objects + 1U);
  const auto has_object = [&](const std::string_view id) {
    return std::any_of(snapshot->objects.begin(), snapshot->objects.end(),
                       [&](const core::SceneObjectSnapshotDTO &object) {
                         return object.scene_resource_id ==
                                core::SceneResourceId(std::string(id));
                       });
  };
  EXPECT_TRUE(has_object("scene-inner"));
  EXPECT_FALSE(has_object("scene-box"));
  for (const auto &projection : fixture.predecessor_projections) {
    ASSERT_EQ(projection->calls(), 1U);
    EXPECT_EQ(projection->transactions().front().request_key,
              inner.request_context.request_key);
  }

  // The same change, as a new request against the current Scene, commits.
  auto renewed = first;
  renewed.request_context.request_key =
      SceneRequestKey("od376-concurrent-renewed");
  renewed.expected_projection_revision = owner.CurrentProjectionRevision();
  const auto committed = owner.Commit(renewed);
  EXPECT_TRUE(committed.success) << CommitError(committed);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
}

// OD-376: the Commit is in flight from its intent, across its admission
// claim: a Commit arriving then is refused as in progress without effect,
// and readers see the settled predecessor.
TEST(CanonicalEmptySceneOwner,
     CommitInFlightAcrossTheAdmissionClaimRefusesASecondCommit) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  auto &kernel = fixture.cell_admission;
  const auto cell = fixture.admission.cell_id();
  const auto predecessor = owner.CurrentProjectionRevision();
  const auto predecessor_snapshot = owner.LogicalSnapshot();
  const auto outer = AddBoxRequest(predecessor, cell, "od376-claim-outer");
  auto inner = AddBoxRequest(predecessor, cell, "od376-claim-inner");
  std::get<core::AddObjectRequestDTO>(inner.operation).scene_resource_id =
      core::SceneResourceId("scene-inner");
  const auto acquires_before = kernel.acquire_calls();

  struct ClaimReads final {
    std::string failure;
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> snapshot;
    std::optional<ProjectionRevisionDTO> revision;
    std::optional<CanonicalEmptySceneAccessStatus> access;
    std::optional<core::SceneCommitResultDTO> inner;
    std::optional<core::SceneCommitResultDTO> inner_stored;
  };
  std::future<ClaimReads> reading;
  bool read_in_time = false;
  kernel.RunBeforeNextAcquire([&] {
    reading = std::async(std::launch::async, [&] {
      ClaimReads reads;
      try {
        reads.snapshot = owner.LogicalSnapshot();
        reads.revision = owner.CurrentProjectionRevision();
        reads.access = owner.ReadAccessStatus();
        reads.inner = owner.Commit(inner);
        reads.inner_stored = owner.ReadCommitResult(inner);
      } catch (const std::exception &error) {
        reads.failure = error.what();
      }
      return reads;
    });
    read_in_time = ReadyWithinBound(reading);
  });
  const auto committed = owner.Commit(outer);
  ASSERT_TRUE(reading.valid());
  const auto reads = reading.get();

  EXPECT_TRUE(read_in_time) << "a read waited for the admission claim";
  EXPECT_TRUE(reads.failure.empty()) << reads.failure;
  ASSERT_NE(reads.snapshot, nullptr);
  EXPECT_EQ(*reads.snapshot, *predecessor_snapshot);
  EXPECT_EQ(reads.revision, std::optional{predecessor});
  ASSERT_TRUE(reads.access.has_value());
  EXPECT_EQ(reads.access->active_projection, std::optional{predecessor});
  ASSERT_TRUE(reads.inner.has_value());
  EXPECT_FALSE(reads.inner->success);
  ASSERT_TRUE(reads.inner->error.has_value());
  EXPECT_EQ(reads.inner->error->code, "stale_revision");
  EXPECT_EQ(reads.inner->error->details.at("reason"),
            "scene_commit_in_progress");
  EXPECT_EQ(reads.inner->projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_FALSE(reads.inner_stored.has_value());

  ASSERT_TRUE(committed.success) << CommitError(committed);
  EXPECT_EQ(kernel.acquire_calls(), acquires_before + 1U);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
  EXPECT_FALSE(owner.ReadCommitResult(inner).has_value());
  for (const auto &projection : fixture.predecessor_projections) {
    ASSERT_EQ(projection->calls(), 1U);
    EXPECT_EQ(projection->transactions().front().request_key,
              outer.request_context.request_key);
  }

  // The refused request left nothing: resent, it commits.
  const auto resent = owner.Commit(inner);
  EXPECT_TRUE(resent.success) << CommitError(resent);
}

// OD-376: the intent is recorded before the admission claim is acquired. A
// refused claim, or one whose reply is lost, is the request's stored
// outcome; a request whose intent cannot be recorded acquires no claim.
TEST(CanonicalEmptySceneOwner, IntentPrecedesTheAdmissionClaim) {
  for (const bool lost_reply : {false, true}) {
    SCOPED_TRACE(lost_reply ? "lost acquire reply" : "refused claim");
    const auto admission = LoadAdmission();
    ControlledPublicationReader publication_reader;
    TestCellAdmissionPort cell_admission;
    LogicalSceneOwner owner(admission, publication_reader, cell_admission);
    const auto active =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    const auto request =
        AddBoxRequest(active, admission.cell_id(), "od376-intent-first");
    const ErrorDTO refusal =
        lost_reply ? ErrorDTO{"kernel_owner_unavailable",
                              "acquisition reply lost",
                              {}}
                   : ErrorDTO{"invalid_state_transition",
                              "not started; held by Stop",
                              {{"reason", "program_node_stop_fence_active"}}};
    if (lost_reply) {
      cell_admission.LoseNextReply(TestCellAdmissionPort::LostReply::kAcquire);
    } else {
      cell_admission.RejectNextClaim(refusal);
    }
    const auto integrity_before = owner.VerifyIntegrity();
    const auto acquires_before = cell_admission.acquire_calls();
    const auto claims_before = cell_admission.Claims().size();
    // An acquire that never reached the Kernel answers authoring_not_ready,
    // as before OD-376; the Kernel's own refusals pass through unchanged.
    const ErrorDTO expected =
        lost_reply ? ErrorDTO{"authoring_not_ready",
                              "authored scene admission is unavailable: "
                              "acquisition reply lost",
                              {}}
                   : refusal;
    const auto refused = owner.Commit(request);
    ASSERT_FALSE(refused.success);
    EXPECT_EQ(refused.error, expected);
    EXPECT_EQ(refused.replay_status,
              core::RequestReplayStatusDTO::kAcceptedNew);
    EXPECT_EQ(refused.projection_status,
              core::SceneProjectionCommitStatusDTO::kNotAttempted);
    EXPECT_EQ(cell_admission.acquire_calls(), acquires_before + 1U);
    EXPECT_TRUE(owner.GetSceneObjects().empty());
    EXPECT_EQ(owner.CurrentProjectionRevision(), active);

    const auto stored = owner.ReadCommitResult(request);
    ASSERT_TRUE(stored.has_value());
    EXPECT_FALSE(stored->success);
    EXPECT_EQ(stored->error, expected);
    EXPECT_EQ(stored->projection_status,
              core::SceneProjectionCommitStatusDTO::kNotAttempted);
    const auto integrity_after = owner.VerifyIntegrity();
    EXPECT_EQ(integrity_after.transaction_intent_count,
              integrity_before.transaction_intent_count + 1);
    EXPECT_EQ(integrity_after.request_outcome_count,
              integrity_before.request_outcome_count + 1);
    // A refused claim was never held; a lost reply leaves the Kernel holding
    // it, so authoring stays closed until reconciliation settles it.
    EXPECT_EQ(cell_admission.Claims().size(),
              claims_before + (lost_reply ? 1U : 0U));
    EXPECT_EQ(cell_admission.GetAuthoringAdmission().authoring_allowed,
              !lost_reply);
  }
}

// OD-378: a Commit whose resulting Scene would encode above
// kMaximumLogicalSceneSnapshotBytes is refused `scene_too_large` before its
// intent and before any admission claim: nothing is recorded.
TEST(CanonicalEmptySceneOwner, SceneAboveItsSizeBoundIsRefusedBeforeItsIntent) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  auto active = FinalizeCanonicalBootstrap(owner, publication_reader, admission)
                    .canonical_empty_target;
  for (const auto *name : {"limit-1", "limit-2", "limit-3"}) {
    const auto added =
        owner.Commit(AddLargeMeshRequest(active, admission.cell_id(), name));
    ASSERT_TRUE(added.success) << CommitError(added);
    ASSERT_TRUE(added.committed_projection_revision.has_value());
    active = *added.committed_projection_revision;
  }
  const auto current_bytes =
      EncodeLogicalSceneSnapshot(*owner.LogicalSnapshot()).size();
  ASSERT_LT(current_bytes, kMaximumLogicalSceneSnapshotBytes);
  ASSERT_GT(current_bytes + kLargeMeshUriBytes,
            kMaximumLogicalSceneSnapshotBytes);
  const auto over_bound =
      AddLargeMeshRequest(active, admission.cell_id(), "limit-4");
  const auto acquires_before = cell_admission.acquire_calls();
  const auto claims_before = cell_admission.Claims();
  const auto integrity_before = owner.VerifyIntegrity();
  const auto refused = owner.Commit(over_bound);
  ASSERT_FALSE(refused.success);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "scene_too_large");
  EXPECT_EQ(refused.replay_status, core::RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(refused.projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_FALSE(refused.committed_projection_revision.has_value());
  // No claim was taken and none is held.
  EXPECT_EQ(cell_admission.acquire_calls(), acquires_before);
  EXPECT_EQ(cell_admission.Claims(), claims_before);
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
  // Nothing was recorded; the Scene is unchanged.
  const auto integrity_after = owner.VerifyIntegrity();
  EXPECT_EQ(integrity_after.transaction_intent_count,
            integrity_before.transaction_intent_count);
  EXPECT_EQ(integrity_after.request_outcome_count,
            integrity_before.request_outcome_count);
  EXPECT_FALSE(owner.ReadCommitResult(over_bound).has_value());
  EXPECT_EQ(owner.CurrentProjectionRevision(), active);
  const auto head = owner.ReadActiveSnapshot();
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(head->shared()->revision, active.scene_revision);
  EXPECT_EQ(head->shared()->objects.size(), 3U);

  // A smaller change still commits on the unchanged head.
  const auto small = owner.Commit(AddBoxRequest(
      active, admission.cell_id(), "scene-request-after-too-large"));
  ASSERT_TRUE(small.success) << CommitError(small);
  ASSERT_TRUE(small.committed_projection_revision.has_value());
  EXPECT_EQ(small.committed_projection_revision->scene_revision.revision_id,
            active.scene_revision.revision_id + 1U);
  EXPECT_EQ(owner.GetSceneObjects().size(), 4U);
}

// OD-378: an intent keeps the resolved operation and the proposed revision's
// content hash; its candidate is the delta applied to the predecessor, and a
// result of another content is an integrity failure.
TEST(CanonicalEmptySceneOwner, SceneCandidateIsDerivedFromTheIntentsDelta) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  const auto predecessor = owner.LogicalSnapshot();
  const auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-request-derive");
  const auto committed = owner.Commit(request);
  ASSERT_TRUE(committed.success) << CommitError(committed);
  ASSERT_TRUE(committed.transaction_sequence.has_value());
  ASSERT_TRUE(committed.committed_projection_revision.has_value());
  const auto successor = owner.LogicalSnapshot();
  ASSERT_EQ(successor->objects.size(), 1U);
  EXPECT_EQ(
      core::logical_scene_identity::ComputeLogicalSceneContentHash(*successor),
      committed.committed_projection_revision->scene_revision.content_hash);

  const logical_scene_owner_detail::SceneTransactionRecord record{
      *committed.transaction_sequence,
      request.request_context.request_key,
      ContentDigest(manyforge::content_identity::sha256_digest(
          logical_scene_codec_internal::EncodeRequestDigestInput(request))),
      logical_scene_codec_internal::EncodeRequest(request),
      active,
      *committed.committed_projection_revision,
      core::ResolvedUpsertSceneObjectDTO{successor->objects.front(),
                                         std::nullopt},
      core::SceneUpdateStatusDTO::kGeometryChanged,
      0U,
      0U,
      {},
  };
  EXPECT_EQ(
      logical_scene_owner_detail::DeriveSceneCandidate(*predecessor, record),
      *successor);

  auto wrong_hash = record;
  wrong_hash.proposed.scene_revision.content_hash = Digest('f');
  EXPECT_THROW(
      static_cast<void>(logical_scene_owner_detail::DeriveSceneCandidate(
          *predecessor, wrong_hash)),
      LogicalSceneOwnerIntegrityError);
}

// OD-378: a successful Commit whose final claim release fails after its
// success is recorded keeps the head at the committed revision, derived from
// the recorded delta.
TEST(CanonicalEmptySceneOwner, LostReleaseAfterASuccessKeepsTheCommittedHead) {
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  TestCellAdmissionPort cell_admission;
  LogicalSceneOwner owner(admission, publication_reader, cell_admission);
  const auto active =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  const auto request =
      AddBoxRequest(active, admission.cell_id(), "scene-request-release-lost");
  cell_admission.LoseNextReply(TestCellAdmissionPort::LostReply::kRelease);
  EXPECT_THROW(static_cast<void>(owner.Commit(request)), std::runtime_error);
  const auto recorded = owner.ReadCommitResult(request);
  ASSERT_TRUE(recorded.has_value());
  ASSERT_TRUE(recorded->success);
  ASSERT_TRUE(recorded->committed_projection_revision.has_value());
  const auto committed = *recorded->committed_projection_revision;
  EXPECT_EQ(committed.scene_revision.revision_id,
            active.scene_revision.revision_id + 1U);
  const auto head = owner.ReadActiveSnapshot();
  ASSERT_TRUE(head.has_value());
  EXPECT_EQ(head->shared()->revision, committed.scene_revision);
  ASSERT_EQ(head->shared()->objects.size(), 1U);
  EXPECT_EQ(head->shared()->objects.front().scene_resource_id,
            core::SceneResourceId("scene-box"));
  EXPECT_EQ(core::logical_scene_identity::ComputeLogicalSceneContentHash(
                *head->shared()),
            committed.scene_revision.content_hash);
  EXPECT_EQ(owner.CurrentProjectionRevision(), committed);

  // Reconciliation settles the held claim; the next Commit builds on the head.
  EXPECT_FALSE(cell_admission.GetAuthoringAdmission().authoring_allowed);
  const auto reconciled = owner.ReconcileAuthoredSceneAdmissionClaims();
  ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
  EXPECT_TRUE(cell_admission.GetAuthoringAdmission().authoring_allowed);
  auto next = AddBoxRequest(committed, admission.cell_id(),
                            "scene-request-after-release-lost");
  std::get<core::AddObjectRequestDTO>(next.operation).scene_resource_id =
      core::SceneResourceId("scene-box-2");
  const auto followed = owner.Commit(next);
  ASSERT_TRUE(followed.success) << CommitError(followed);
  ASSERT_TRUE(followed.committed_projection_revision.has_value());
  EXPECT_EQ(followed.committed_projection_revision->scene_revision.revision_id,
            committed.scene_revision.revision_id + 1U);
  EXPECT_EQ(owner.GetSceneObjects().size(), 2U);
}

// OD-376: a preflight call refused as a call (e.g. a transaction beyond the
// transport bound) reaches the caller as its own typed refusal, before any
// intent, claim or native apply.
TEST(CanonicalEmptySceneOwner, PreflightRefusalReachesTheCallerAsItsTypedCode) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  auto &kernel = fixture.cell_admission;
  const auto predecessor = owner.CurrentProjectionRevision();
  const auto request = AddBoxRequest(predecessor, fixture.admission.cell_id(),
                                     "od376-preflight-refused");
  const ErrorDTO bound_exceeded{
      "native_projection_rpc_bound_exceeded",
      "the Scene transaction exceeds the native projection transport bound",
      {{"reason", "native_projection_rpc_bound_exceeded"}}};
  fixture.predecessor_projections.front()->RejectNextPreflight(bound_exceeded);
  const auto claims_before = kernel.Claims().size();
  const auto acquires_before = kernel.acquire_calls();
  const auto integrity_before = owner.VerifyIntegrity();

  const auto refused = owner.Commit(request);
  ASSERT_FALSE(refused.success);
  EXPECT_EQ(refused.error, bound_exceeded);
  EXPECT_EQ(refused.replay_status, core::RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(refused.projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_FALSE(refused.committed_projection_revision.has_value());
  EXPECT_FALSE(owner.ReadCommitResult(request).has_value());
  EXPECT_EQ(kernel.acquire_calls(), acquires_before);
  EXPECT_EQ(kernel.Claims().size(), claims_before);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
  for (const auto &projection : fixture.predecessor_projections) {
    EXPECT_EQ(projection->calls(), 0U);
  }
  const auto integrity_after = owner.VerifyIntegrity();
  EXPECT_EQ(integrity_after.transaction_intent_count,
            integrity_before.transaction_intent_count);
  EXPECT_EQ(integrity_after.request_outcome_count,
            integrity_before.request_outcome_count);
  EXPECT_EQ(owner.CurrentProjectionRevision(), predecessor);

  // The refusal left nothing: resent, the request commits.
  const auto resent = owner.Commit(request);
  EXPECT_TRUE(resent.success) << CommitError(resent);
}

// What an Apply-lane writer did while a Commit was held in its native apply.
template <typename Written> struct WriterDuringAHeldCommit final {
  std::optional<core::SceneCommitResultDTO> committed;
  std::optional<Written> written;
  bool waited_for_the_commit{};
  std::string failure;
};

// Holds `commit` inside the native apply of `held`, then starts `writer`
// (given the held transaction) on another thread and observes that it is
// still waiting 50 ms later. The release is always reached.
template <typename Commit, typename Writer>
[[nodiscard]] auto RunWriterDuringAHeldCommit(FakePlannerSceneProjection &held,
                                              Commit commit, Writer writer) {
  using Written =
      std::invoke_result_t<Writer &, const core::SceneTransactionDTO &>;
  WriterDuringAHeldCommit<Written> observed;
  std::barrier apply_entered(2);
  std::barrier apply_release(2);
  held.BlockNextApply(apply_entered, apply_release);
  auto committing = std::async(std::launch::async, std::move(commit));
  apply_entered.arrive_and_wait();
  std::future<Written> writing;
  try {
    const auto blocked = held.blocked_transaction();
    if (!blocked.has_value()) {
      throw std::logic_error("the held native apply has no transaction");
    }
    writing = std::async(std::launch::async,
                         [&writer, transaction = *blocked] {
                           return writer(transaction);
                         });
    // An observation only: the writer is expected to wait for the Commit.
    observed.waited_for_the_commit =
        writing.wait_for(50ms) == std::future_status::timeout;
  } catch (const std::exception &error) {
    observed.failure = error.what();
  }
  apply_release.arrive_and_wait();
  observed.committed.emplace(committing.get());
  if (writing.valid()) {
    if (!ReadyWithinBound(writing)) {
      observed.failure += "the writer did not end after the Commit; ";
    }
    try {
      observed.written.emplace(writing.get());
    } catch (const std::exception &error) {
      observed.failure += error.what();
    }
  }
  return observed;
}

// OD-376 (B1): the Apply-lane writers (maintenance initialization, qualified
// adoption Stage and Begin) wait while a Commit is in flight, then behave as
// they would after that Commit: they succeed against its successor, and the
// Commit is not refused.
TEST(CanonicalEmptySceneOwner, ApplyLaneWritersWaitForAnInFlightCommit) {
  {
    SCOPED_TRACE("maintenance initialization");
    SceneAdoptionFixture fixture;
    auto &owner = *fixture.owner;
    fixture.PrepareCandidateClients();
    const auto integrity_before = owner.VerifyIntegrity();
    const auto add = AddBoxRequest(owner.CurrentProjectionRevision(),
                                   fixture.admission.cell_id(),
                                   "od376-held-before-maintenance");
    const auto observed = RunWriterDuringAHeldCommit(
        *fixture.predecessor_projections.front(),
        [&] { return owner.Commit(add); },
        [&](const core::SceneTransactionDTO &held) {
          // Expects the successor of the held Commit.
          return owner.BeginMaintenanceInitialization({
              ApplyId(300U),
              ApplyGeneration(1U),
              held.proposed_projection_revision,
              fixture.candidate.bundle->value(),
              fixture.candidate_bindings,
          });
        });
    EXPECT_TRUE(observed.failure.empty()) << observed.failure;
    EXPECT_TRUE(observed.waited_for_the_commit);
    ASSERT_TRUE(observed.committed.has_value());
    ASSERT_TRUE(observed.committed->success) << CommitError(*observed.committed);
    ASSERT_TRUE(observed.written.has_value());
    EXPECT_EQ(observed.written->state,
              core::ProjectionMaintenanceInitializationState::kInitialized);
    EXPECT_EQ(owner.ReadMaintenanceInitialization(
                  {ApplyId(300U), ApplyGeneration(1U)}),
              observed.written);
    // The Commit's write precedes the maintenance window; none follows it.
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    const auto integrity_after = owner.VerifyIntegrity();
    EXPECT_EQ(integrity_after.transaction_intent_count,
              integrity_before.transaction_intent_count + 1);
    EXPECT_EQ(integrity_after.request_outcome_count,
              integrity_before.request_outcome_count + 1);
    for (const auto &projection : fixture.predecessor_projections) {
      EXPECT_EQ(projection->calls(), 1U);
    }
  }
  {
    SCOPED_TRACE("qualified adoption");
    SceneAdoptionFixture fixture;
    auto &owner = *fixture.owner;
    const auto cell = fixture.admission.cell_id();
    const auto added = owner.Commit(AddBoxRequest(
        owner.CurrentProjectionRevision(), cell, "od376-adoption-add"));
    ASSERT_TRUE(added.success) << CommitError(added);
    // The Apply holds its claim from here: the held Commits are
    // runtime-owned pose updates.
    const auto request = fixture.CaptureConfigurationRequest();
    fixture.PrepareCandidateClients();
    const auto streamed_pose = [&](const std::string &name, const double x_m) {
      core::Pose3DTO pose;
      pose.position_m = {x_m, -0.5, 0.75};
      return core::SceneTransactionRequestDTO{
          core::PublicRequestContextDTO{SceneRequestKey(name),
                                        "scene-owner-test"},
          {},
          owner.CurrentProjectionRevision(),
          core::UpdateObjectPoseRequestDTO{
              core::SceneResourceId("scene-box"), pose,
              core::CellSceneFrameRefDTO{cell}, StreamedPoseEvidence(), false},
          std::nullopt};
    };
    const auto integrity_before = owner.VerifyIntegrity();

    const auto before_stage = streamed_pose("od376-held-before-stage", 0.30);
    const auto staged = RunWriterDuringAHeldCommit(
        *fixture.predecessor_projections.front(),
        [&] { return owner.Commit(before_stage); },
        [&](const core::SceneTransactionDTO &) {
          return owner.StageOriginalManagerConfigurationAdoption(
              request, fixture.fence, fixture.fence.OriginalAttemptQuery());
        });
    EXPECT_TRUE(staged.failure.empty()) << staged.failure;
    EXPECT_TRUE(staged.waited_for_the_commit);
    ASSERT_TRUE(staged.committed.has_value());
    ASSERT_TRUE(staged.committed->success) << CommitError(*staged.committed);
    ASSERT_TRUE(staged.committed->committed_projection_revision.has_value());
    ASSERT_TRUE(staged.written.has_value());
    ASSERT_EQ(staged.written->size(), request.execution_host_bindings.size());
    const ProjectionRevisionDTO staged_target{
        request.candidate_retained_bundle.retained_bundle_id,
        request.candidate_retained_bundle.cell_graph.graph_revision,
        staged.committed->committed_projection_revision->scene_revision};
    for (const auto &row : *staged.written) {
      EXPECT_TRUE(row.healthy_for_target(staged_target));
    }

    const auto before_begin = streamed_pose("od376-held-before-begin", 0.35);
    const auto begun = RunWriterDuringAHeldCommit(
        *fixture.predecessor_projections.front(),
        [&] { return owner.Commit(before_begin); },
        [&](const core::SceneTransactionDTO &) {
          return fixture.Begin(request);
        });
    EXPECT_TRUE(begun.failure.empty()) << begun.failure;
    EXPECT_TRUE(begun.waited_for_the_commit);
    ASSERT_TRUE(begun.committed.has_value());
    ASSERT_TRUE(begun.committed->success) << CommitError(*begun.committed);
    ASSERT_TRUE(begun.committed->committed_projection_revision.has_value());
    ASSERT_TRUE(begun.written.has_value());
    ASSERT_TRUE(begun.written->ready_for_publication());
    // Begin captured the Scene the held Commit produced.
    EXPECT_EQ(begun.written->target_projection_revision.scene_revision,
              begun.committed->committed_projection_revision->scene_revision);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    const auto integrity_after = owner.VerifyIntegrity();
    EXPECT_EQ(integrity_after.transaction_intent_count,
              integrity_before.transaction_intent_count + 2);
    EXPECT_EQ(integrity_after.request_outcome_count,
              integrity_before.request_outcome_count + 2);
  }
}

// OD-376: the owner validates and digests the candidate once, and the native
// projection applies only the owner's delta. Replaying that delta onto the
// owner's predecessor (readable during the apply: no owner lock is held)
// gives a valid snapshot with the owner's digest, equal to the one the owner
// then publishes. This is the cross-check the owner no longer runs itself.
TEST(CanonicalEmptySceneOwner, ProjectionReplaysTheOwnersValidatedDelta) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  const auto &projections = fixture.predecessor_projections;
  ASSERT_FALSE(projections.empty());
  for (const auto &projection : projections) {
    projection->ReplayAgainst(fixture.owner.get());
    projection->SetFrameResolution(core::Pose3DTO{}, {});
  }
  const auto cell = fixture.admission.cell_id();
  std::optional<core::SceneCommitResultDTO> last;
  const auto commit_and_check =
      [&](const core::SceneTransactionRequestDTO &request) {
        std::vector<std::size_t> before;
        for (const auto &projection : projections) {
          before.push_back(projection->replay_checks().size());
        }
        last = owner.Commit(request);
        ASSERT_TRUE(last->success) << CommitError(*last);
        const auto settled = owner.LogicalSnapshot();
        for (std::size_t index = 0U; index < projections.size(); ++index) {
          const auto &checks = projections[index]->replay_checks();
          ASSERT_EQ(checks.size(), before[index] + 1U);
          const auto &check = checks.back();
          EXPECT_TRUE(check.applied) << check.error;
          EXPECT_TRUE(check.content_hash_matches) << check.error;
          EXPECT_TRUE(check.locally_valid);
          ASSERT_TRUE(check.result.has_value());
          EXPECT_EQ(*check.result, *settled);
        }
      };

  ASSERT_NO_FATAL_FAILURE(commit_and_check(AddBoxRequest(
      owner.CurrentProjectionRevision(), cell, "od376-replay-add")));

  auto upsert = AddBoxRequest(owner.CurrentProjectionRevision(), cell,
                              "od376-replay-upsert");
  const auto add = std::get<core::AddObjectRequestDTO>(upsert.operation);
  upsert.operation = core::UpsertObjectRequestDTO{
      add.scene_resource_id, add.shape, add.pose, add.frame_ref};
  auto &replacement = std::get<core::UpsertObjectRequestDTO>(upsert.operation);
  replacement.shape = core::SphereShapeDTO{0.4};
  replacement.pose.position_m.x_m = 1.25;
  ASSERT_NO_FATAL_FAILURE(commit_and_check(upsert));
  EXPECT_EQ(last->update_status, core::SceneUpdateStatusDTO::kGeometryChanged);

  const auto before_no_op = owner.CurrentProjectionRevision();
  auto no_op = upsert;
  no_op.request_context =
      SceneRequestContext("od376-replay-upsert-no-op", before_no_op);
  no_op.expected_projection_revision = before_no_op;
  ASSERT_NO_FATAL_FAILURE(commit_and_check(no_op));
  EXPECT_EQ(last->update_status, core::SceneUpdateStatusDTO::kNoOp);
  EXPECT_EQ(last->committed_projection_revision, before_no_op);

  core::Pose3DTO streamed_pose;
  streamed_pose.position_m = {1.5, -0.5, 0.75};
  ASSERT_NO_FATAL_FAILURE(commit_and_check(core::SceneTransactionRequestDTO{
      core::PublicRequestContextDTO{SceneRequestKey("od376-replay-pose"),
                                    "scene-owner-test"},
      {},
      owner.CurrentProjectionRevision(),
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"), streamed_pose,
          core::CellSceneFrameRefDTO{cell}, StreamedPoseEvidence(), false},
      std::nullopt}));

  auto configured = owner.LogicalSnapshot()->configured_collision_policy;
  configured.policy_id = "strict-diagnostics";
  const auto policy_revision = owner.CurrentProjectionRevision();
  ASSERT_NO_FATAL_FAILURE(commit_and_check(core::SceneTransactionRequestDTO{
      SceneRequestContext("od376-replay-policy", policy_revision),
      {},
      policy_revision,
      core::SetCollisionPolicyRequestDTO{
          configured,
          owner.LogicalSnapshot()->configured_collision_policy.revision,
      },
      core::AdmissionGeneration(1U),
  }));
  EXPECT_EQ(owner.LogicalSnapshot()->configured_collision_policy.policy_id,
            "strict-diagnostics");

  // A carried object: attach (as AttachOnAMountedToolFrameKeepsTheArmAsHolder),
  // its provider pose (a resolved upsert of the carried object), detach at
  // the current pose, attach again and detach to a named pose.
  const auto attach_request = [&](const std::string &name) {
    core::AttachCarriedObjectRequestDTO operation{
        core::CarriedObjectAttachmentId("attachment.1"),
        core::SceneResourceId("scene-box"),
        core::ControllableComponentId("component.1"),
        core::CommandTargetId("arm"),
        core::FrameRoleId("holder_rod"),
        {core::CollisionBodyRoleId("holder_rod")},
        {}};
    operation.carrier_component_id =
        core::ControllableComponentId("tube_holder");
    const auto revision = owner.CurrentProjectionRevision();
    core::SceneTransactionRequestDTO attach{
        SceneRequestContext(name, revision), {}, revision, operation,
        core::AdmissionGeneration(1U)};
    attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
        core::ProgramRunNumber(2U), core::CellAdmissionClaimGeneration(9U),
        "attach", core::NodeAttemptNumber(1U), "scene.carried_attachment"};
    return attach;
  };
  ASSERT_NO_FATAL_FAILURE(commit_and_check(attach_request("od376-replay-attach")));
  ASSERT_EQ(owner.LogicalSnapshot()->carried_object_attachments.size(), 1U);

  core::Pose3DTO carried_pose;
  carried_pose.position_m = {0.5, 0.25, 0.1};
  ASSERT_NO_FATAL_FAILURE(commit_and_check(core::SceneTransactionRequestDTO{
      core::PublicRequestContextDTO{SceneRequestKey("od376-replay-carried-pose"),
                                    "scene-owner-test"},
      {},
      owner.CurrentProjectionRevision(),
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"), carried_pose,
          core::CellSceneFrameRefDTO{cell}, StreamedPoseEvidence(),
          std::nullopt},
      std::nullopt}));
  for (const auto &projection : projections) {
    ASSERT_FALSE(projection->transactions().empty());
    const auto *carried = std::get_if<core::ResolvedUpsertSceneObjectDTO>(
        &projection->transactions().back().resolved_operation);
    ASSERT_NE(carried, nullptr);
    EXPECT_TRUE(carried->attachment.has_value());
  }

  const auto detach_current_request = [&](const std::string &name) {
    const auto attachment =
        owner.LogicalSnapshot()->carried_object_attachments.front();
    const auto revision = owner.CurrentProjectionRevision();
    return core::SceneTransactionRequestDTO{
        SceneRequestContext(name, revision),
        {},
        revision,
        core::DetachCarriedObjectAtCurrentPoseRequestDTO{
            attachment.carried_object_attachment_id,
            attachment.carried_object_attachment_revision,
            attachment.scene_resource_id, attachment.controllable_component_id,
            attachment.command_target_id},
        std::nullopt};
  };
  ASSERT_NO_FATAL_FAILURE(
      commit_and_check(detach_current_request("od376-replay-detach-current")));
  EXPECT_TRUE(owner.LogicalSnapshot()->carried_object_attachments.empty());

  ASSERT_NO_FATAL_FAILURE(
      commit_and_check(attach_request("od376-replay-reattach")));
  {
    const auto attachment =
        owner.LogicalSnapshot()->carried_object_attachments.front();
    const auto revision = owner.CurrentProjectionRevision();
    ASSERT_NO_FATAL_FAILURE(commit_and_check(core::SceneTransactionRequestDTO{
        SceneRequestContext("od376-replay-detach", revision),
        {},
        revision,
        core::DetachCarriedObjectRequestDTO{
            attachment.carried_object_attachment_id,
            attachment.carried_object_attachment_revision,
            core::Pose3DTO{{0.5, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}},
            core::CellSceneFrameRefDTO{cell}},
        std::nullopt}));
  }
  EXPECT_TRUE(owner.LogicalSnapshot()->carried_object_attachments.empty());

  const auto remove_revision = owner.CurrentProjectionRevision();
  ASSERT_NO_FATAL_FAILURE(commit_and_check(core::SceneTransactionRequestDTO{
      SceneRequestContext("od376-replay-remove", remove_revision),
      {},
      remove_revision,
      core::RemoveObjectRequestDTO{core::SceneResourceId("scene-box")},
      core::AdmissionGeneration(1U)}));
  const auto remaining = owner.LogicalSnapshot();
  EXPECT_TRUE(std::none_of(
      remaining->objects.begin(), remaining->objects.end(),
      [](const core::SceneObjectSnapshotDTO &object) {
        return object.scene_resource_id == core::SceneResourceId("scene-box");
      }));
}

// OD-376 (S5): a pose-only delta (an existing cell-fixed object keeps its
// shapes, mass and placement kind; only its pose or provider evidence
// changes) cannot change the effective collision policy, so its preflight
// asks for health only. Every other delta projects the policy once.
TEST(CanonicalEmptySceneOwner, PoseOnlyDeltaSkipsThePolicyProjection) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  const auto &projections = fixture.predecessor_projections;
  ASSERT_FALSE(projections.empty());
  for (const auto &projection : projections) {
    projection->SetFrameResolution(core::Pose3DTO{}, {});
  }
  const auto cell = fixture.admission.cell_id();
  const auto expect_policy_projections =
      [&](const core::SceneTransactionRequestDTO &request,
          const std::size_t policies) {
        std::vector<std::pair<std::size_t, std::size_t>> before;
        for (const auto &projection : projections) {
          before.emplace_back(projection->preflight_calls(),
                              projection->policy_projection_calls());
        }
        const auto result = owner.Commit(request);
        ASSERT_TRUE(result.success) << CommitError(result);
        for (std::size_t index = 0U; index < projections.size(); ++index) {
          const auto &projection = *projections[index];
          EXPECT_EQ(projection.preflight_calls() - before[index].first, 1U);
          EXPECT_EQ(projection.policy_projection_calls() - before[index].second,
                    policies);
          EXPECT_EQ(projection.policy_calls_outside_preflight(), 0U);
        }
      };
  const auto pose_at = [](const double x_m) {
    core::Pose3DTO pose;
    pose.position_m = {x_m, -0.5, 0.75};
    return pose;
  };
  const auto pose_update =
      [&](const std::string &name, const core::Pose3DTO &pose,
          std::optional<core::SceneObjectProviderEvidenceDTO> evidence) {
        const auto authored = !evidence.has_value();
        return core::SceneTransactionRequestDTO{
            core::PublicRequestContextDTO{SceneRequestKey(name),
                                          "scene-owner-test"},
            {},
            owner.CurrentProjectionRevision(),
            core::UpdateObjectPoseRequestDTO{
                core::SceneResourceId("scene-box"), pose,
                core::CellSceneFrameRefDTO{cell}, std::move(evidence),
                std::nullopt},
            authored ? std::optional{core::AdmissionGeneration(1U)}
                     : std::nullopt};
      };
  const auto upsert = [&](const std::string &name,
                          const core::CollisionShapeDTO &shape,
                          const core::Pose3DTO &pose, const double mass_kg) {
    auto request =
        AddBoxRequest(owner.CurrentProjectionRevision(), cell, name);
    const auto add = std::get<core::AddObjectRequestDTO>(request.operation);
    request.operation = core::UpsertObjectRequestDTO{
        add.scene_resource_id, shape, pose, add.frame_ref, mass_kg};
    return request;
  };
  const core::CollisionShapeDTO box = core::BoxShapeDTO{{0.1, 0.2, 0.3}};

  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      AddBoxRequest(owner.CurrentProjectionRevision(), cell, "od376-s5-add"),
      1U));
  // Pose-only: an authored move, provider evidence only (same pose), a
  // provider move, and an upsert that keeps the shape and mass.
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      pose_update("od376-s5-authored-move", pose_at(0.30), std::nullopt), 0U));
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      pose_update("od376-s5-evidence-only", pose_at(0.30),
                  StreamedPoseEvidence()),
      0U));
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      pose_update("od376-s5-provider-move", pose_at(0.35),
                  StreamedPoseEvidence()),
      0U));
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      upsert("od376-s5-upsert-pose", box, pose_at(0.40), 0.0), 0U));
  // Not pose-only: a changed shape or mass, a policy, an attach, a detach,
  // a removal.
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      upsert("od376-s5-upsert-shape", core::SphereShapeDTO{0.2}, pose_at(0.40),
             0.0),
      1U));
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      upsert("od376-s5-upsert-mass", core::SphereShapeDTO{0.2}, pose_at(0.40),
             2.0),
      1U));
  auto configured = owner.LogicalSnapshot()->configured_collision_policy;
  configured.policy_id = "strict-diagnostics";
  const auto policy_revision = owner.CurrentProjectionRevision();
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      core::SceneTransactionRequestDTO{
          SceneRequestContext("od376-s5-policy", policy_revision),
          {},
          policy_revision,
          core::SetCollisionPolicyRequestDTO{
              configured,
              owner.LogicalSnapshot()->configured_collision_policy.revision},
          core::AdmissionGeneration(1U)},
      1U));
  core::AttachCarriedObjectRequestDTO attach_operation{
      core::CarriedObjectAttachmentId("attachment.1"),
      core::SceneResourceId("scene-box"),
      core::ControllableComponentId("component.1"),
      core::CommandTargetId("arm"),
      core::FrameRoleId("holder_rod"),
      {core::CollisionBodyRoleId("holder_rod")},
      {}};
  attach_operation.carrier_component_id =
      core::ControllableComponentId("tube_holder");
  const auto attach_revision = owner.CurrentProjectionRevision();
  core::SceneTransactionRequestDTO attach{
      SceneRequestContext("od376-s5-attach", attach_revision), {},
      attach_revision, attach_operation, core::AdmissionGeneration(1U)};
  attach.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
      core::ProgramRunNumber(2U), core::CellAdmissionClaimGeneration(9U),
      "attach", core::NodeAttemptNumber(1U), "scene.carried_attachment"};
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(attach, 1U));
  const auto attachment =
      owner.LogicalSnapshot()->carried_object_attachments.front();
  const auto detach_revision = owner.CurrentProjectionRevision();
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      core::SceneTransactionRequestDTO{
          SceneRequestContext("od376-s5-detach", detach_revision),
          {},
          detach_revision,
          core::DetachCarriedObjectAtCurrentPoseRequestDTO{
              attachment.carried_object_attachment_id,
              attachment.carried_object_attachment_revision,
              attachment.scene_resource_id,
              attachment.controllable_component_id,
              attachment.command_target_id},
          std::nullopt},
      1U));
  const auto remove_revision = owner.CurrentProjectionRevision();
  ASSERT_NO_FATAL_FAILURE(expect_policy_projections(
      core::SceneTransactionRequestDTO{
          SceneRequestContext("od376-s5-remove", remove_revision),
          {},
          remove_revision,
          core::RemoveObjectRequestDTO{core::SceneResourceId("scene-box")},
          core::AdmissionGeneration(1U)},
      1U));
}

// Documented behaviour (OD-218, OD-376): once the history is folded, a
// request's stored result is no longer read. Sent again with the same key,
// it is a new request, unless the Kernel refuses its admission as already
// settled: that refusal is the request's outcome, stored for its key, and
// nothing else changes.
TEST(CanonicalEmptySceneOwner,
     ResentRequestAfterTheFoldIsNewUnlessItsAdmissionIsSettled) {
  SceneAdoptionFixture fixture;
  auto &owner = *fixture.owner;
  auto &kernel = fixture.cell_admission;
  const auto cell = fixture.admission.cell_id();
  const auto pose_at = [](const double x_m) {
    core::Pose3DTO pose;
    pose.position_m = {x_m, -0.5, 0.75};
    return pose;
  };
  const auto upsert = [&](const std::string &name, const double x_m) {
    auto request =
        AddBoxRequest(owner.CurrentProjectionRevision(), cell, name);
    const auto add = std::get<core::AddObjectRequestDTO>(request.operation);
    request.operation = core::UpsertObjectRequestDTO{
        add.scene_resource_id, add.shape, pose_at(x_m), add.frame_ref};
    return request;
  };
  const auto expired = upsert("od376-fold-expired", 0.5);
  const auto committed_expired = owner.Commit(expired);
  ASSERT_TRUE(committed_expired.success) << CommitError(committed_expired);
  const auto readmitted = upsert("od376-fold-readmitted", 0.25);
  const auto committed_readmitted = owner.Commit(readmitted);
  ASSERT_TRUE(committed_readmitted.success)
      << CommitError(committed_readmitted);

  // Runtime-owned pose updates fill the resident history until it folds (as
  // HistoryCompactionIsTransparentToCallers).
  const auto resident = static_cast<std::size_t>(
      owner.VerifyIntegrity().transaction_intent_count);
  ASSERT_LT(resident, kMaximumLogicalSceneTransactions);
  for (std::size_t index = 0U;
       index <= kMaximumLogicalSceneTransactions - resident; ++index) {
    const auto moved = owner.Commit(core::SceneTransactionRequestDTO{
        core::PublicRequestContextDTO{
            SceneRequestKey("od376-fold-move-" + std::to_string(index)),
            "scene-owner-test"},
        {},
        owner.CurrentProjectionRevision(),
        core::UpdateObjectPoseRequestDTO{
            core::SceneResourceId("scene-box"),
            pose_at(1.0 + 0.001 * static_cast<double>(index + 1U)),
            core::CellSceneFrameRefDTO{cell}, StreamedPoseEvidence(), false},
        std::nullopt});
    ASSERT_TRUE(moved.success) << index << " " << CommitError(moved);
  }
  ASSERT_EQ(owner.VerifyIntegrity().transaction_intent_count, 1);
  EXPECT_FALSE(owner.ReadCommitResult(expired).has_value());
  EXPECT_FALSE(owner.ReadCommitResult(readmitted).has_value());

  // The Kernel refuses an admission it already settled.
  const auto revision_before = owner.CurrentProjectionRevision();
  const auto snapshot_before = owner.LogicalSnapshot();
  const auto acquires_before = kernel.acquire_calls();
  const ErrorDTO result_expired{
      "request_result_expired",
      "the request's admission is already settled",
      {{"reason", "request_result_expired"}}};
  kernel.RejectNextClaim(result_expired);
  const auto refused = owner.Commit(expired);
  ASSERT_FALSE(refused.success);
  EXPECT_EQ(refused.error, result_expired);
  EXPECT_EQ(refused.replay_status, core::RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(refused.projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_EQ(kernel.acquire_calls(), acquires_before + 1U);
  EXPECT_EQ(owner.CurrentProjectionRevision(), revision_before);
  EXPECT_EQ(*owner.LogicalSnapshot(), *snapshot_before);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
  const auto stored = owner.ReadCommitResult(expired);
  ASSERT_TRUE(stored.has_value());
  EXPECT_FALSE(stored->success);
  EXPECT_EQ(stored->error, result_expired);

  // A Kernel that admits it again: the resent request is new and commits.
  kernel.AdmitSettledIntentsAgain();
  const auto resent = owner.Commit(readmitted);
  ASSERT_TRUE(resent.success) << CommitError(resent);
  EXPECT_EQ(resent.replay_status, core::RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(kernel.acquire_calls(), acquires_before + 2U);
  ASSERT_TRUE(resent.committed_projection_revision.has_value());
  EXPECT_NE(*resent.committed_projection_revision, revision_before);
  const auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box"));
  ASSERT_TRUE(pose.ok());
  EXPECT_DOUBLE_EQ(pose.value().position_m.x_m, 0.25);
  EXPECT_TRUE(kernel.GetAuthoringAdmission().authoring_allowed);
}

} // namespace
} // namespace manyforge::scene
