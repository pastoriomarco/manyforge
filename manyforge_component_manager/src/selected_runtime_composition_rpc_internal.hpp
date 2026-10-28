#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

inline constexpr std::size_t kMaximumSelectedRuntimeCompositionRpcPacketBytes =
    256U * 1024U;

/// Budget per component a runtime-composition request installs (kAdd or
/// kReplace in its delta). The whole exchange waits for that many budgets,
/// at least one.
inline constexpr std::chrono::seconds kRuntimeCompositionPerComponentTimeout{10};

[[nodiscard]] inline std::chrono::milliseconds SelectedRuntimeCompositionTimeout(
    const manyforge::deployment::ApplyComponentDeltaDTO &delta) noexcept {
  using manyforge::deployment::ApplyComponentDisposition;
  const auto &rows = delta.components;
  const auto composed = std::count_if(rows.begin(), rows.end(), [](const auto &row) {
    return row.disposition == ApplyComponentDisposition::kAdd ||
           row.disposition == ApplyComponentDisposition::kReplace;
  });
  return kRuntimeCompositionPerComponentTimeout *
         std::max<std::ptrdiff_t>(composed, 1);
}

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

[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcResponse(
    const SelectedRuntimeCompositionRpcResponse &response);
[[nodiscard]] SelectedRuntimeCompositionRpcResponse
DecodeSelectedRuntimeCompositionRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail
