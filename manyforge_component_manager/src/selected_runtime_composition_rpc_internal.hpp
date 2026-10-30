#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/component_lifecycle_budget.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

inline constexpr std::size_t kMaximumSelectedRuntimeCompositionRpcPacketBytes =
    256U * 1024U;

/// A runtime-composition request installs its kAdd and kReplace components
/// inside one exchange, which waits for the sum of their lifecycle time
/// budgets (`budgets_ns`, by component id: the authored budget, else the
/// default, see `component_composition_budgets_ns`). An exchange that
/// installs nothing (finalize, retain-only) waits one default budget.
[[nodiscard]] inline std::chrono::milliseconds SelectedRuntimeCompositionTimeout(
    const manyforge::deployment::ApplyComponentDeltaDTO &delta,
    const std::map<std::string, std::int64_t, std::less<>> &budgets_ns) {
  using manyforge::deployment::ApplyComponentDisposition;
  std::chrono::nanoseconds total{0};
  bool installs = false;
  for (const auto &row : delta.components) {
    if (row.disposition != ApplyComponentDisposition::kAdd &&
        row.disposition != ApplyComponentDisposition::kReplace)
      continue;
    installs = true;
    const auto budget = budgets_ns.find(row.controllable_component_id.value());
    total += std::chrono::nanoseconds(budget == budgets_ns.end()
                                          ? manyforge::core::kDefaultLifecycleBudgetNs
                                          : budget->second);
  }
  return std::chrono::ceil<std::chrono::milliseconds>(
      installs ? total : std::chrono::nanoseconds(manyforge::core::kDefaultLifecycleBudgetNs));
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
