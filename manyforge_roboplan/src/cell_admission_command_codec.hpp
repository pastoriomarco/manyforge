#pragma once

#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/cell_admission.hpp>

#include <optional>
#include <string_view>

namespace manyforge::roboplan_backend::detail {

inline constexpr std::string_view kCellAdmissionCommandApiVersion =
    "manyforge.execution_command.v4";
inline constexpr std::string_view kCellAdmissionResponseApiVersion =
    "manyforge.execution_command_response.v5";

/// Strict decoder of a claim as the response JSON spells it.
[[nodiscard]] manyforge::core::CellAdmissionClaimDTO
DecodeCellAdmissionClaim(std::string_view bytes);

[[nodiscard]] manyforge::content_identity::CanonicalValue ProgramRunnerLossValue(
    const manyforge::core::ProgramRunnerContactLossDTO &loss,
    manyforge::core::ApplicationStartId application_start_id);

/// Returns nullopt only when a well-formed command names another command.
/// Recognized cell-admission commands are decoded as closed, canonical DTOs.
[[nodiscard]] std::optional<
    manyforge::component_manager::detail::KernelCellAdmissionRpcPacket>
TryDecodeCellAdmissionCommand(std::string_view bytes);

[[nodiscard]] manyforge::content_identity::CanonicalValue
CellAdmissionResponseValue(
    std::string_view command,
    const manyforge::component_manager::detail::
        KernelCellAdmissionRpcResponse &response,
    std::string_view producer = "kernel");

} // namespace manyforge::roboplan_backend::detail
