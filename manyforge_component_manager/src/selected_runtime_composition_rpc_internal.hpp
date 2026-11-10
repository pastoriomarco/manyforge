#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

inline constexpr std::size_t kMaximumSelectedRuntimeCompositionRpcPacketBytes =
    256U * 1024U;

/// The default time budget for installing one component inside a composition
/// exchange; a Deployment `lifecycle_command_timeout_s` replaces it per
/// component (`component_composition_budgets_ns`).
inline constexpr std::int64_t kDefaultComponentInstallBudgetNs = 10'000'000'000LL;

/// A host Apply stops its removed components and installs its added and
/// replaced components (every component not retained) inside one exchange,
/// which waits for the sum of their budgets (`budgets_ns`, by component id:
/// the authored budget, else the default, see
/// `component_composition_budgets_ns`; a removed component is absent from the
/// candidate's budgets and takes the default). Each of them may also retire a
/// launch first (a removed or replaced component; an added one isn't told
/// apart here): a hung one takes the host's stop grace before its group
/// SIGKILL and reap, so each adds `core::kControllerLaunchStopGrace` (a
/// lone hung arm outlasted a bound equal to that grace, and the timeout shut
/// the execution host down). An Apply that installs and removes nothing
/// waits one default budget.
[[nodiscard]] inline std::chrono::milliseconds HostApplyTimeout(
    const HostApplyRequest &request,
    const std::map<std::string, std::int64_t, std::less<>> &budgets_ns) {
  std::chrono::nanoseconds total{0};
  bool works = false;
  const auto add = [&](const auto &component) {
    works = true;
    const auto budget = budgets_ns.find(component.value());
    total += std::chrono::nanoseconds(budget == budgets_ns.end()
                                          ? kDefaultComponentInstallBudgetNs
                                          : budget->second) +
             manyforge::core::kControllerLaunchStopGrace;
  };
  for (const auto &component : request.components)
    if (!std::binary_search(request.retained_components.begin(),
                            request.retained_components.end(), component))
      add(component);
  for (const auto &component : request.removed_components) add(component);
  return std::chrono::ceil<std::chrono::milliseconds>(
      works ? total : std::chrono::nanoseconds(kDefaultComponentInstallBudgetNs));
}

struct SelectedRuntimeCompositionRpcFailure final {
  manyforge::core::ErrorDTO error;
};

using SelectedRuntimeCompositionRpcRequest =
    std::variant<HostApplyRequest,
                 ManagerComponentProcessStopRequestDTO,
                 ManagerComponentProcessStopReadRequestDTO,
                 ManagerComponentProcessInventoryRequestDTO>;

using SelectedRuntimeCompositionRpcResponse =
    std::variant<HostApplyResult,
                 SelectedRuntimeCompositionRpcFailure,
                 ManagerComponentProcessStopResultDTO,
                 ManagerComponentProcessInventoryResultDTO>;

[[nodiscard]] bool
IsSelectedRuntimeCompositionRpcPacket(std::string_view bytes) noexcept;

[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const HostApplyRequest &request);

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
