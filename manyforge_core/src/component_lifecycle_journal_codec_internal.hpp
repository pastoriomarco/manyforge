#pragma once

#include <manyforge_core/ports/icclc_journal.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {
struct SelectedComponentBringUpOfferDTO;
struct NativeApplyComponentBringUpDTO;
}

namespace manyforge::core::detail {

inline constexpr std::size_t kMaximumComponentLifecycleJournalRecordBytes = 64U * 1024U * 1024U;

[[nodiscard]] std::string EncodeProgramNodeAdmissionEvidence(
    const ProgramNodeAdmissionEvidenceDTO& evidence);
[[nodiscard]] ProgramNodeAdmissionEvidenceDTO DecodeProgramNodeAdmissionEvidence(
    std::string_view bytes);

[[nodiscard]] std::string EncodeCCLCRouteRecoveryConfiguration(
    const CellAdmissionConfigurationScopeDTO& configuration);
[[nodiscard]] CellAdmissionConfigurationScopeDTO DecodeCCLCRouteRecoveryConfiguration(
    std::string_view bytes);

struct SelectedCCLCClaimIntentRecordDTO final {
  CellAdmissionClaimIntentDTO full_claim_intent;
  ContentDigest original_request_payload_digest;

  friend bool operator==(const SelectedCCLCClaimIntentRecordDTO&,
                         const SelectedCCLCClaimIntentRecordDTO&) = default;
};

/// Distinct private selected shape in the existing recovery-configuration blob.
/// A decoded claim remains observation-only; it never authorizes redispatch.
[[nodiscard]] std::string EncodeCCLCSelectedBringUpClaimIntent(
    const SelectedCCLCClaimIntentRecordDTO& record);
[[nodiscard]] SelectedCCLCClaimIntentRecordDTO DecodeCCLCSelectedBringUpClaimIntent(
    std::string_view bytes);
[[nodiscard]] bool IsCCLCSelectedBringUpClaimIntentEncoding(std::string_view bytes) noexcept;

/// Exact original intent retained in the existing membership result slot.
struct CCLCTargetRetirementRecordDTO final {
  CCLCLifecycleTargetRetirementIntentDTO intent;
  std::vector<ComponentLifecycleTargetDTO> retired_targets;
  friend bool operator==(const CCLCTargetRetirementRecordDTO&,
                         const CCLCTargetRetirementRecordDTO&) = default;
};
[[nodiscard]] std::string EncodeCCLCTargetRetirementRecord(const CCLCTargetRetirementRecordDTO&);
[[nodiscard]] CCLCTargetRetirementRecordDTO DecodeCCLCTargetRetirementRecord(std::string_view);
[[nodiscard]] std::string CCLCTargetRetirementSemanticIntentDigest(
    const CCLCLifecycleTargetRetirementIntentDTO&);

[[nodiscard]] std::string EncodeCCLCAdmissionRejection(const ErrorDTO& error);
[[nodiscard]] ErrorDTO DecodeCCLCAdmissionRejection(std::string_view bytes);

[[nodiscard]] std::string EncodeComponentLifecycleStatus(const ComponentLifecycleStatusDTO& status);

[[nodiscard]] std::string EncodeNativeApplyComponentBringUp(const NativeApplyComponentBringUpDTO&);
[[nodiscard]] NativeApplyComponentBringUpDTO DecodeNativeApplyComponentBringUp(std::string_view);

[[nodiscard]] ComponentLifecycleStatusDTO DecodeComponentLifecycleStatus(std::string_view bytes);

[[nodiscard]] std::string ComponentLifecycleStatusReleasePreconditionDigest(
    const ComponentLifecycleStatusDTO& status);

/// Digest of the exact semantic request. Transport attribution is omitted in
/// the same way as ComponentLifecycleIntentDTO::replay_equal().
[[nodiscard]] std::string ComponentLifecycleSemanticIntentDigest(
    const ComponentLifecycleIntentDTO& intent);
/// Private selected effect request identity, using this same bounded codec.
[[nodiscard]] std::string SelectedComponentBringUpSemanticOfferDigest(
    const SelectedComponentBringUpOfferDTO& offer);

[[nodiscard]] std::string EncodeLifecyclePrimitiveIntent(
    const BrandLifecyclePrimitiveIntentDTO& intent);
[[nodiscard]] BrandLifecyclePrimitiveIntentDTO DecodeLifecyclePrimitiveIntent(
    std::string_view bytes);
[[nodiscard]] std::string EncodeLifecyclePrimitiveResult(
    const BrandLifecyclePrimitiveResultDTO& result);
[[nodiscard]] BrandLifecyclePrimitiveResultDTO DecodeLifecyclePrimitiveResult(
    std::string_view bytes);

[[nodiscard]] std::string EncodeCCLCEffectAttempt(const CCLCEffectAttemptDTO& attempt);
[[nodiscard]] CCLCEffectAttemptDTO DecodeCCLCEffectAttempt(std::string_view bytes);

[[nodiscard]] std::string EncodeRouteSelectionStatus(const RouteSelectionStatusDTO& status);
[[nodiscard]] RouteSelectionStatusDTO DecodeRouteSelectionStatus(std::string_view bytes);
[[nodiscard]] std::string EncodeActiveControlConfiguration(
    const ActiveControlConfigurationDTO& configuration);
[[nodiscard]] ActiveControlConfigurationDTO DecodeActiveControlConfiguration(
    std::string_view bytes);
[[nodiscard]] std::string RouteSelectionStatusReleasePreconditionDigest(
    const RouteSelectionStatusDTO& status);
[[nodiscard]] std::string RouteSelectionSemanticIntentDigest(const RouteSelectionIntentDTO& intent);
[[nodiscard]] std::string EncodeCCLCRouteEffectAttempt(const CCLCRouteEffectAttemptDTO& attempt);
[[nodiscard]] CCLCRouteEffectAttemptDTO DecodeCCLCRouteEffectAttempt(std::string_view bytes);

}  // namespace manyforge::core::detail
