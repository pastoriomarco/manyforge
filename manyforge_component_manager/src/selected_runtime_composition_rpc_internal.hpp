#pragma once

#include <cstddef>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

inline constexpr std::size_t kMaximumSelectedRuntimeCompositionRpcPacketBytes =
    256U * 1024U;

struct SelectedRuntimeCompositionRpcFailure final {
  manyforge::core::ErrorDTO error;
};

using SelectedRuntimeCompositionRpcRequest =
    std::variant<ManagerSelectedRuntimeCompositionRequestDTO,
                 ManagerSelectedComponentBringUpRequestDTO,
                 ManagerSelectedComponentBringUpReadRequestDTO,
                 ManagerApplyComponentBringUpRequestDTO,
                 ManagerApplyComponentBringUpReadRequestDTO,
                 ManagerComponentBringUpInspectionRequestDTO,
                 ManagerComponentProcessStopRequestDTO,
                 ManagerComponentProcessStopReadRequestDTO,
                 ManagerComponentProcessInventoryRequestDTO>;

using SelectedRuntimeCompositionRpcResponse =
    std::variant<ManagerSelectedRuntimeCompositionResultDTO,
                 SelectedRuntimeCompositionRpcFailure,
                 ManagerSelectedComponentBringUpResultDTO,
                 ManagerApplyComponentBringUpResultDTO,
                 ManagerComponentBringUpInspectionResultDTO,
                 ManagerComponentProcessStopResultDTO,
                 ManagerComponentProcessInventoryResultDTO>;

[[nodiscard]] bool
IsSelectedRuntimeCompositionRpcPacket(std::string_view bytes) noexcept;

[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerSelectedRuntimeCompositionRequestDTO &request);
[[nodiscard]] ManagerSelectedRuntimeCompositionRequestDTO
DecodeSelectedRuntimeCompositionRpcRequest(std::string_view bytes);

[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerSelectedComponentBringUpRequestDTO &request);
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerSelectedComponentBringUpReadRequestDTO &request);
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerApplyComponentBringUpRequestDTO &request);
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerApplyComponentBringUpReadRequestDTO &request);
// Optional planning fields extend only closed Inspect packet tails; omitted
// fields preserve the existing MFNSRC4 tag11/tag12 and Q96 observer bytes.
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentBringUpInspectionRequestDTO &request);
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentProcessStopRequestDTO &request);
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentProcessStopReadRequestDTO &request);
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentProcessInventoryRequestDTO &request);
[[nodiscard]] SelectedRuntimeCompositionRpcRequest
DecodeSelectedRuntimeCompositionRpcRequestVariant(std::string_view bytes);

// An optional positive observer-retirement receipt extends only ordinary
// result tails. Omitted receipts preserve launch-facts and legacy bytes.
[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcResponse(
    const SelectedRuntimeCompositionRpcResponse &response);
[[nodiscard]] SelectedRuntimeCompositionRpcResponse
DecodeSelectedRuntimeCompositionRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail
