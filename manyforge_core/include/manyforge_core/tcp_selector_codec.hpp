#pragma once

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/tcp_entry.hpp>

namespace manyforge::core {

/// Active TCP (M7): the one wire and journal shape of a TCP entry and a TCP
/// selector, shared by the host's intent decoder, the operation journal, the
/// Set TCP payload and the `tcp_entry` blackboard value.
///
///   entry:    {"component_id": C, "target_id": T, "frame_role_id": F}
///   selector: {"kind": "current"}
///           | {"kind": "default_of_role", "role": R}
///           | {"kind": "entry", "entry": <entry>}
///
/// Decoding is closed (no unknown or missing member). The pre-M7 endpoint
/// `{"target_role_id", "frame_role_id"}` is refused with reason
/// `tcp_endpoint_legacy_form`; any other malformed value with
/// `tcp_selector_invalid`. Both use code `invalid_request`.
[[nodiscard]] content_identity::CanonicalValue EncodeTcpEntry(const TcpEntryDTO& entry);
[[nodiscard]] Result<TcpEntryDTO> DecodeTcpEntry(const content_identity::CanonicalValue& value);
[[nodiscard]] content_identity::CanonicalValue EncodeTcpSelector(const TcpSelectorDTO& selector);
[[nodiscard]] Result<TcpSelectorDTO> DecodeTcpSelector(
    const content_identity::CanonicalValue& value);

}  // namespace manyforge::core
