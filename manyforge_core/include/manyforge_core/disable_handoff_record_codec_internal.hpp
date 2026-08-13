#pragma once

#include <manyforge_core/types/component_lifecycle.hpp>
#include <string>
#include <string_view>

namespace manyforge::core::detail {

/// Minimal durable prepared-fence truth.  A persisted Disable request does
/// not contain (and must not reconstruct) the earlier component-snapshot
/// digest, so the fence ledger stores the independently re-resolved actual
/// disposition and complete scope directly.
struct DisableHandoffFenceScopeRecordDTO final {
  DisableHandoffScopeResolutionDTO scope_resolution{
      DisableHandoffScopeResolutionDTO::kPromotedToFull};
  ConfigurationFenceScopeDTO scope;

  friend bool operator==(const DisableHandoffFenceScopeRecordDTO&,
                         const DisableHandoffFenceScopeRecordDTO&) = default;
};

/// Closed canonical records shared by the CCLC journal and the managed
/// Kernel Disable-handoff ledger.  Every encode/decode validates the complete
/// DTO at its boundary; these are not public transport payloads.
[[nodiscard]] std::string EncodeDisableHandoffAdmissionSnapshotRecord(
    const DisableHandoffAdmissionSnapshotDTO& value);
[[nodiscard]] DisableHandoffAdmissionSnapshotDTO DecodeDisableHandoffAdmissionSnapshotRecord(
    std::string_view bytes);

[[nodiscard]] std::string EncodeDisableHandoffFenceScopeRecord(
    const DisableHandoffFenceScopeRecordDTO& value);
[[nodiscard]] DisableHandoffFenceScopeRecordDTO DecodeDisableHandoffFenceScopeRecord(
    std::string_view bytes);

[[nodiscard]] std::string EncodeDisableHandoffRequestRecord(const DisableHandoffRequestDTO& value);
[[nodiscard]] DisableHandoffRequestDTO DecodeDisableHandoffRequestRecord(std::string_view bytes);

[[nodiscard]] std::string EncodeDisableHandoffAcceptedRecord(
    const DisableHandoffAcceptedDTO& value);
[[nodiscard]] DisableHandoffAcceptedDTO DecodeDisableHandoffAcceptedRecord(std::string_view bytes);

[[nodiscard]] std::string EncodeDisableHandoffStatusRecord(const DisableHandoffStatusDTO& value);
[[nodiscard]] DisableHandoffStatusDTO DecodeDisableHandoffStatusRecord(std::string_view bytes);

[[nodiscard]] std::string EncodeDisableFenceReleaseRequestRecord(
    const DisableFenceReleaseRequestDTO& value);
[[nodiscard]] DisableFenceReleaseRequestDTO DecodeDisableFenceReleaseRequestRecord(
    std::string_view bytes);

[[nodiscard]] std::string EncodeDisableFenceReleasedRecord(const DisableFenceReleasedDTO& value);
[[nodiscard]] DisableFenceReleasedDTO DecodeDisableFenceReleasedRecord(std::string_view bytes);

}  // namespace manyforge::core::detail
