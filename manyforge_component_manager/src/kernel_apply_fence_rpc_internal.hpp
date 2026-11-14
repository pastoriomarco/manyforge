#pragma once

#include <cstddef>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <optional>
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
    manyforge::core::ConfigurationFenceStatusQueryDTO,
    manyforge::core::ConfigurationFenceReleaseDTO>;

struct KernelApplyFenceRpcFailure final {
  manyforge::core::ErrorDTO error;
};

using KernelApplyFenceRpcResponse = std::variant<
    manyforge::core::ConfigurationFenceAdmissionSnapshotDTO,
    manyforge::core::ConfigurationFenceAcceptedDTO,
    manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO,
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

/// The record encoding of one request, as the fence ledger stores it.
[[nodiscard]] std::string
EncodeKernelApplyFenceRpcRequest(const KernelApplyFenceRpcRequest &request);
[[nodiscard]] KernelApplyFenceRpcRequest
DecodeKernelApplyFenceRpcRequest(std::string_view bytes);

/// One request on the Manager-child channel: the request record, then the
/// Manager's current publication, carried by every request except Release.
struct KernelApplyFenceRpcPacket final {
  KernelApplyFenceRpcRequest request;
  std::optional<manyforge::core::ActiveBundlePublicationReceiptDTO>
      active_publication;

  friend bool operator==(const KernelApplyFenceRpcPacket &,
                         const KernelApplyFenceRpcPacket &) = default;
};

[[nodiscard]] std::string
EncodeKernelApplyFenceRpcPacket(const KernelApplyFenceRpcPacket &packet);
[[nodiscard]] KernelApplyFenceRpcPacket
DecodeKernelApplyFenceRpcPacket(std::string_view bytes);

[[nodiscard]] std::string
EncodeKernelApplyFenceRpcResponse(const KernelApplyFenceRpcResponse &response);
[[nodiscard]] KernelApplyFenceRpcResponse
DecodeKernelApplyFenceRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail
