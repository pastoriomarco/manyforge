#pragma once

#include <manyforge_core/ports/icclc_journal.hpp>
#include <string>
#include <string_view>

namespace manyforge::core::detail {

inline constexpr std::size_t kMaximumComponentLifecycleJournalRecordBytes = 64U * 1024U * 1024U;

[[nodiscard]] std::string EncodeProgramNodeAdmissionEvidence(
    const ProgramNodeAdmissionEvidenceDTO& evidence);
[[nodiscard]] ProgramNodeAdmissionEvidenceDTO DecodeProgramNodeAdmissionEvidence(
    std::string_view bytes);

[[nodiscard]] std::string EncodeCCLCAdmissionRejection(const ErrorDTO& error);
[[nodiscard]] ErrorDTO DecodeCCLCAdmissionRejection(std::string_view bytes);

[[nodiscard]] std::string EncodeComponentLifecycleStatus(const ComponentLifecycleStatusDTO& status);

[[nodiscard]] ComponentLifecycleStatusDTO DecodeComponentLifecycleStatus(std::string_view bytes);

[[nodiscard]] std::string ComponentLifecycleStatusReleasePreconditionDigest(
    const ComponentLifecycleStatusDTO& status);

/// Digest of the exact semantic request. Transport attribution is omitted in
/// the same way as ComponentLifecycleIntentDTO::replay_equal().
[[nodiscard]] std::string ComponentLifecycleSemanticIntentDigest(
    const ComponentLifecycleIntentDTO& intent);

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
