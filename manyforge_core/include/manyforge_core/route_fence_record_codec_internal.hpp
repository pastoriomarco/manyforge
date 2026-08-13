#pragma once

#include <manyforge_core/route_selection.hpp>
#include <string>
#include <string_view>

namespace manyforge::core::detail {

/// Closed canonical records shared by the CCLC journal and the managed
/// Kernel route-fence ledger. These functions validate at both boundaries.
[[nodiscard]] std::string EncodeRouteFenceAdmissionSnapshotRecord(
    const RouteConfigurationFenceAdmissionSnapshotDTO& value);
[[nodiscard]] RouteConfigurationFenceAdmissionSnapshotDTO DecodeRouteFenceAdmissionSnapshotRecord(
    std::string_view bytes);

[[nodiscard]] std::string EncodeRouteFenceIntentRecord(
    const RouteConfigurationFenceIntentDTO& value);
[[nodiscard]] RouteConfigurationFenceIntentDTO DecodeRouteFenceIntentRecord(std::string_view bytes);

[[nodiscard]] std::string EncodeRouteFenceAcceptedRecord(
    const RouteConfigurationFenceAcceptedDTO& value);
[[nodiscard]] RouteConfigurationFenceAcceptedDTO DecodeRouteFenceAcceptedRecord(
    std::string_view bytes);

[[nodiscard]] std::string EncodeRouteFenceEvidenceRecord(
    const RouteConfigurationFenceReconciliationEvidenceDTO& value);

[[nodiscard]] std::string EncodeRouteFenceStatusRecord(
    const RouteConfigurationFenceStatusDTO& value);

[[nodiscard]] std::string EncodeRouteFenceReleaseRequestRecord(
    const RouteConfigurationFenceReleaseRequestDTO& value);
[[nodiscard]] RouteConfigurationFenceReleaseRequestDTO DecodeRouteFenceReleaseRequestRecord(
    std::string_view bytes);

[[nodiscard]] std::string EncodeRouteFenceReleasedRecord(
    const RouteConfigurationFenceReleasedDTO& value);
[[nodiscard]] RouteConfigurationFenceReleasedDTO DecodeRouteFenceReleasedRecord(
    std::string_view bytes);

}  // namespace manyforge::core::detail
