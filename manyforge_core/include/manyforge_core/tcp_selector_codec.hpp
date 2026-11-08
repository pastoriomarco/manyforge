#pragma once

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/tcp_entry.hpp>

namespace manyforge::core {

/// Resolved internal entries retain the component-local physical shape.
/// Authored v3 entries carry separate direct target and frame references:
///   entry: {"target": {"component_id", "command_target_id"},
///           "frame": {"component_id", "frame_id"}}
///   selector: {"kind": "current"}
///           | {"kind": "default_of_target", "target": <target>}
///           | {"kind": "entry", "target": <target>, "frame": <frame>}
/// Decoders are closed; old endpoint and role forms do not select a target.
[[nodiscard]] content_identity::CanonicalValue EncodeTcpEntry(const TcpEntryDTO& entry);
[[nodiscard]] Result<TcpEntryDTO> DecodeTcpEntry(const content_identity::CanonicalValue& value);
[[nodiscard]] content_identity::CanonicalValue EncodeTcpEntryReference(const TcpEntryReferenceDTO& entry);
[[nodiscard]] Result<TcpEntryReferenceDTO> DecodeTcpEntryReference(
    const content_identity::CanonicalValue& value);
[[nodiscard]] content_identity::CanonicalValue EncodeTcpSelector(const TcpSelectorDTO& selector);
[[nodiscard]] Result<TcpSelectorDTO> DecodeTcpSelector(
    const content_identity::CanonicalValue& value);

}  // namespace manyforge::core
