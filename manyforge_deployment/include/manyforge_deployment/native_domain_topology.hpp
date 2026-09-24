#pragma once

#include <string_view>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/error_code.hpp>

namespace manyforge::deployment {

inline constexpr std::string_view kNativeControlProviderManifestId =
    "manyforge.provider.native_control";

/// The generic graph contract allows several components to share one control
/// domain. The in-process native provider does not yet compose or qualify
/// that layout: its lifecycle product, candidate publication and runtime
/// assume one controllable component per native domain.
///
/// Refuse an unsupported native layout before any predecessor teardown,
/// explaining the provider limitation. Non-native domains are not narrowed.
[[nodiscard]] core::Result<void>
VerifySupportedNativeDomainTopology(const core::CellGraphDTO &graph);

} // namespace manyforge::deployment
