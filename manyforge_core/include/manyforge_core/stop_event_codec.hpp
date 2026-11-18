#pragma once

// Stop state model (design 2026-11-13, §7; contract §5.11): the payload
// `manyforge.stop_event.v1` of the two journal rows of one Stop event,
// `stop_event_accepted` and `stop_event_completed`.
//
// The journal writer calls these on its own ordinary thread with a copy of the
// event record that the Kernel's projection worker took under the Kernel lock;
// nothing here runs under a Kernel lock. There is no decoder: no owner reads
// these rows back (recovery is gone, every start is clean); History validates
// the rows it ingests itself.

#include <cstdint>
#include <manyforge_core/stop_state.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::core {

/// Canonical JSON (RFC 8785) of the `stop_event_accepted` row: the request,
/// its attribution, the hold change and the covered participants (contract
/// §5.11 "Accepted"). Throws `content_identity::CanonicalValueError` on text
/// that is not NFC UTF-8 and `std::invalid_argument` on a record that is not
/// an accepted event; the journal turns either into its typed failure.
[[nodiscard]] std::string EncodeStopEventAccepted(const StopEventRecord& record);

/// Canonical JSON of the `stop_event_completed` row: result, completion time
/// and the outcome of every covered participant (contract §5.11 "Completed",
/// with the containing stage as index and stage kind, coordinator N4).
[[nodiscard]] std::string EncodeStopEventCompleted(const StopEventRecord& record);

/// Text of an encoded error code (`StopCoveredEntry::error_code`,
/// `StopEventRecord::fence_degraded_reason`): absent for 0; the catalogued
/// code for 1 + its position in the sorted catalogue; `uncatalogued` for
/// `kStopErrorCodeUncatalogued`.
[[nodiscard]] std::optional<std::string_view> StopEventErrorCodeText(std::uint16_t code) noexcept;

}  // namespace manyforge::core
