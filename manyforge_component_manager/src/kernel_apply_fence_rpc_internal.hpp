#pragma once

#include <cstddef>
#include <manyforge_core/types/configuration_fence.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

inline constexpr std::size_t kMaximumKernelApplyFenceRpcPacketBytes =
    256U * 1024U;

using KernelApplyFenceRpcRequest = std::variant<
    manyforge::core::ConfigurationFenceAdmissionQueryDTO,
    manyforge::core::ApplyConfigurationFenceIntentDTO,
    manyforge::core::ApplyFenceAcquireAttemptQueryDTO,
    manyforge::core::ApplyFenceRehydrateDTO,
    manyforge::core::ConfigurationFenceStatusQueryDTO,
    manyforge::core::ConfigurationFenceReleaseDTO>;

struct KernelApplyFenceRpcFailure final {
  manyforge::core::ErrorDTO error;
};

using KernelApplyFenceRpcResponse = std::variant<
    manyforge::core::ConfigurationFenceAdmissionSnapshotDTO,
    manyforge::core::ConfigurationFenceAcceptedDTO,
    manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO,
    manyforge::core::ApplyFenceReconcileResultDTO,
    manyforge::core::ConfigurationFenceStatusDTO,
    manyforge::core::ConfigurationFenceReleasedDTO, KernelApplyFenceRpcFailure>;

[[nodiscard]] bool IsKernelApplyFenceRpcPacket(std::string_view bytes) noexcept;

/// Closed standalone record of one configuration-fence scope (controller and
/// provider domains), shared by the Apply journal and the runtime-composition
/// RPC. It restores the value only; it grants no fence ownership.
[[nodiscard]] std::string EncodeConfigurationFenceScopeRecord(
    const manyforge::core::ConfigurationFenceScopeDTO &scope);
[[nodiscard]] manyforge::core::ConfigurationFenceScopeDTO
DecodeConfigurationFenceScopeRecord(std::string_view bytes);

[[nodiscard]] std::string
EncodeKernelApplyFenceRpcRequest(const KernelApplyFenceRpcRequest &request);
[[nodiscard]] KernelApplyFenceRpcRequest
DecodeKernelApplyFenceRpcRequest(std::string_view bytes);

[[nodiscard]] std::string
EncodeKernelApplyFenceRpcResponse(const KernelApplyFenceRpcResponse &response);
[[nodiscard]] KernelApplyFenceRpcResponse
DecodeKernelApplyFenceRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail
