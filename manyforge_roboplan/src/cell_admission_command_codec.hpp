#pragma once

#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/cell_admission.hpp>

#include <optional>
#include <string_view>

namespace manyforge::roboplan_backend::detail {

inline constexpr std::string_view kCellAdmissionCommandApiVersion =
    "manyforge.execution_command.v1";
inline constexpr std::string_view kCellAdmissionResponseApiVersion =
    "manyforge.execution_command_response.v1";

/// Strict decoders shared by descriptor-bound, read-only owner-journal
/// settlement readers. They accept only the canonical private DTO spelling.
[[nodiscard]] manyforge::core::CellAdmissionClaimDTO
DecodeCellAdmissionClaim(std::string_view bytes);

[[nodiscard]] manyforge::core::CellAdmissionClaimReleaseDTO
DecodeCellAdmissionClaimRelease(std::string_view bytes);

/// Returns nullopt only when a well-formed command names another command.
/// Recognized cell-admission commands are decoded as closed, canonical DTOs.
[[nodiscard]] std::optional<
    manyforge::component_manager::detail::KernelCellAdmissionRpcPacket>
TryDecodeCellAdmissionCommand(std::string_view bytes);

[[nodiscard]] manyforge::content_identity::CanonicalValue
CellAdmissionResponseValue(
    std::string_view command,
    const manyforge::component_manager::detail::
        KernelCellAdmissionRpcResponse &response);

} // namespace manyforge::roboplan_backend::detail
