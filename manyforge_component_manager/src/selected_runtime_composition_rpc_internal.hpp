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

struct SelectedRuntimeCompositionRpcFailure final {
  manyforge::core::ErrorDTO error;
};

using SelectedRuntimeCompositionRpcRequest =
    std::variant<HostApplyRequest,
                 ManagerComponentProcessInventoryRequestDTO>;

using SelectedRuntimeCompositionRpcResponse =
    std::variant<HostApplyResult,
                 SelectedRuntimeCompositionRpcFailure,
                 ManagerComponentProcessInventoryResultDTO>;

[[nodiscard]] bool
IsSelectedRuntimeCompositionRpcPacket(std::string_view bytes) noexcept;

[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const HostApplyRequest &request);

[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentProcessInventoryRequestDTO &request);
[[nodiscard]] SelectedRuntimeCompositionRpcRequest
DecodeSelectedRuntimeCompositionRpcRequestVariant(std::string_view bytes);

[[nodiscard]] std::string EncodeSelectedRuntimeCompositionRpcResponse(
    const SelectedRuntimeCompositionRpcResponse &response);
[[nodiscard]] SelectedRuntimeCompositionRpcResponse
DecodeSelectedRuntimeCompositionRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail
