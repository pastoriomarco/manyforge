#pragma once

#include <algorithm>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace manyforge::core::test {

class FakeCellAdmissionPort : public IKernelCellAdmissionPort {
 private:
  struct Entry final {
    CellAdmissionClaimDTO claim;
    std::optional<CellAdmissionClaimReleaseDTO> release;
  };

  static bool ReleaseMatchesClaim(const CellAdmissionClaimReleaseDTO& release,
                                  const CellAdmissionClaimDTO& claim) {
    return release.owner_role == claim.intent.owner_role &&
           release.owner_epoch == claim.intent.owner_epoch &&
           release.owner_transaction_id == claim.intent.owner_transaction_id &&
           release.request_id == claim.intent.request_id &&
           release.request_payload_digest == claim.intent.request_payload_digest &&
           release.acquired_admission_generation == claim.acquired_admission_generation &&
           release.claim_generation == claim.claim_generation;
  }

  void RetainCurrentOwnerLocked(const CellAdmissionClaimIntentDTO& intent) {
    const auto current =
        std::find_if(current_owners_.begin(), current_owners_.end(),
                     [&](const auto& owner) { return owner.owner_role == intent.owner_role; });
    if (current == current_owners_.end()) {
      current_owners_.push_back({intent.owner_role, intent.owner_epoch});
      std::sort(current_owners_.begin(), current_owners_.end(),
                [](const auto& lhs, const auto& rhs) { return lhs.owner_role < rhs.owner_role; });
    } else {
      current->owner_epoch = intent.owner_epoch;
    }
  }

 public:
  void RejectNextClaim(ErrorDTO error) {
    std::lock_guard lock(mutex_);
    next_claim_error_ = std::move(error);
  }

  void LoseNextReleaseReply() {
    std::lock_guard lock(mutex_);
    lose_next_release_reply_ = true;
  }

  void LoseNextReconcileReply() {
    std::lock_guard lock(mutex_);
    lose_next_reconcile_reply_ = true;
  }

  void RejectNextReconcileInventory() {
    std::lock_guard lock(mutex_);
    reject_next_reconcile_inventory_ = true;
  }

  void RetainClaim(CellAdmissionClaimDTO claim) {
    std::lock_guard lock(mutex_);
    const auto existing = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
      return entry.claim.claim_generation == claim.claim_generation;
    });
    if (existing == entries_.end()) {
      entries_.push_back({claim, std::nullopt});
    } else {
      existing->claim = claim;
      existing->release.reset();
    }
    next_claim_generation_ = CellAdmissionClaimGeneration(
        std::max(next_claim_generation_.value(), claim.claim_generation.value() + 1U));
    RetainCurrentOwnerLocked(claim.intent);
  }

  AuthoringAdmissionDTO GetAuthoringAdmission() const override {
    return {true, AdmissionGeneration(1U)};
  }
  Result<AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const CellAdmissionReadinessSnapshotDTO&,
      std::shared_ptr<IIoSignalService>* actual_retired_service = nullptr) override {
    if (actual_retired_service) actual_retired_service->reset();
    return Result<AuthoringAdmissionDTO>::Ok(GetAuthoringAdmission());
  }
  Result<CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const CellAdmissionClaimIntentDTO& intent) override {
    std::lock_guard lock(mutex_);
    if (next_claim_error_) {
      auto error = std::move(*next_claim_error_);
      next_claim_error_.reset();
      return Result<CellAdmissionClaimDTO>::Err(std::move(error));
    }
    const auto replay = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
      return entry.claim.intent.owner_epoch == intent.owner_epoch &&
             entry.claim.intent.request_id == intent.request_id;
    });
    if (replay != entries_.end()) {
      if (replay->claim.intent != intent || replay->release.has_value()) {
        return Result<CellAdmissionClaimDTO>::Err(
            ErrorDTO{"request_id_conflict", "cell-admission request reuse conflict", {}});
      }
      return Result<CellAdmissionClaimDTO>::Ok(replay->claim);
    }
    CellAdmissionClaimDTO claim{intent, next_claim_generation_,
                                intent.expected_admission_generation, 1};
    next_claim_generation_ = CellAdmissionClaimGeneration(next_claim_generation_.value() + 1U);
    entries_.push_back({claim, std::nullopt});
    RetainCurrentOwnerLocked(intent);
    return Result<CellAdmissionClaimDTO>::Ok(std::move(claim));
  }
  Result<AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const CellAdmissionClaimReleaseDTO& release) override {
    std::lock_guard lock(mutex_);
    const auto held = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
      return !entry.release.has_value() && ReleaseMatchesClaim(release, entry.claim);
    });
    if (held == entries_.end()) {
      return Result<AuthoringAdmissionDTO>::Err(
          ErrorDTO{"stale_generation", "cell-admission release is not exact", {}});
    }
    held->release = release;
    if (std::exchange(lose_next_release_reply_, false)) {
      throw std::runtime_error("release reply lost after Kernel recorded settlement");
    }
    return Result<AuthoringAdmissionDTO>::Ok(GetAuthoringAdmission());
  }
  Result<AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const CellAdmissionReconcileDTO& reconcile) override {
    std::lock_guard lock(mutex_);
    if (std::exchange(reject_next_reconcile_inventory_, false)) {
      return Result<AuthoringAdmissionDTO>::Err({"authoring_not_ready",
                                                 "another owner changed a claim",
                                                 {{"reason", "reconcile_inventory_not_exact"}}});
    }
    std::vector<CellAdmissionClaimDTO> held_claims;
    for (const auto& entry : entries_) {
      if (!entry.release.has_value()) {
        held_claims.push_back(entry.claim);
      }
    }
    std::sort(held_claims.begin(), held_claims.end(), [](const auto& lhs, const auto& rhs) {
      return lhs.claim_generation < rhs.claim_generation;
    });
    if (held_claims != reconcile.retained_claims) {
      return Result<AuthoringAdmissionDTO>::Err(ErrorDTO{
          "reconcile_inventory_not_exact", "cell-admission reconcile inventory is not exact", {}});
    }
    current_owners_ = reconcile.current_owner_epochs;
    if (std::exchange(lose_next_reconcile_reply_, false)) {
      throw std::runtime_error("reply lost after exact reconciliation");
    }
    return Result<AuthoringAdmissionDTO>::Ok(GetAuthoringAdmission());
  }
  Result<CellAdmissionRecoveryReceiptDTO> GetCellAdmissionRecoveryReceipt(
      const CellAdmissionRecoveryReceiptQueryDTO& query) const override {
    std::lock_guard lock(mutex_);
    CellAdmissionRecoveryReceiptDTO receipt{CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
                                            RequestOwnerEpoch("kernel.1"),
                                            AdmissionGeneration(1U),
                                            std::nullopt,
                                            std::nullopt,
                                            std::nullopt,
                                            {},
                                            current_owners_};
    for (const auto& entry : entries_) {
      if (!entry.release.has_value()) {
        receipt.held_claims.push_back(entry.claim);
      }
    }
    std::sort(receipt.held_claims.begin(), receipt.held_claims.end(),
              [](const auto& lhs, const auto& rhs) {
                return lhs.claim_generation < rhs.claim_generation;
              });
    const auto matched = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
      return entry.claim.intent.owner_epoch == query.intent.owner_epoch &&
             entry.claim.intent.request_id == query.intent.request_id;
    });
    if (matched == entries_.end()) {
      return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
    }
    if (matched->claim.intent != query.intent ||
        (query.expected_claim.has_value() && query.expected_claim.value() != matched->claim) ||
        (query.expected_release.has_value() && matched->release.has_value() &&
         query.expected_release.value() != matched->release.value())) {
      receipt.status = CellAdmissionRecoveryReceiptStatusDTO::kConflicting;
      return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
    }
    receipt.matched_claim = matched->claim;
    receipt.matched_release = matched->release;
    receipt.status = matched->release.has_value()
                         ? CellAdmissionRecoveryReceiptStatusDTO::kReleasedExact
                         : CellAdmissionRecoveryReceiptStatusDTO::kHeldExact;
    return Result<CellAdmissionRecoveryReceiptDTO>::Ok(std::move(receipt));
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
  std::vector<CellAdmissionCurrentOwnerEpochDTO> current_owners_;
  CellAdmissionClaimGeneration next_claim_generation_{1U};
  std::optional<ErrorDTO> next_claim_error_;
  bool lose_next_release_reply_{};
  bool lose_next_reconcile_reply_{};
  bool reject_next_reconcile_inventory_{};
};

}  // namespace manyforge::core::test
