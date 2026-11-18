#pragma once

#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

/// One packet bound for every endpoint of the inherited Manager-child channel.
inline constexpr std::size_t kMaximumManagedRuntimeRpcPacketBytes =
    manyforge::core::kMaximumCellAdmissionPrivateBinaryBytes;

/// `cell_admission_acquire` of a Program Reset claim: the Program runner's
/// current cycle status travels beside the intent, never inside it, so intent
/// equality and replay are unchanged (OD-139). Only the execution command
/// socket carries it; the Manager-child binary channel refuses it.
struct ProgramResetClaimAcquireRequest final {
  manyforge::core::CellAdmissionClaimIntentDTO intent;
  manyforge::core::ProgramCycleStatusDTO program_cycle_status;

  friend bool operator==(const ProgramResetClaimAcquireRequest &,
                         const ProgramResetClaimAcquireRequest &) = default;
};

/// `cell_admission_acquire` of a Manager-Apply claim: the candidate bundle the
/// Apply installs travels beside the intent. Manager-child channel only.
struct ManagerApplyClaimAcquireRequest final {
  manyforge::core::CellAdmissionClaimIntentDTO intent;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;

  friend bool operator==(const ManagerApplyClaimAcquireRequest &,
                         const ManagerApplyClaimAcquireRequest &) = default;
};

/// `cell_admission_release` of the claim a Manager Apply settles: the Apply's
/// outcome travels beside the release. Manager-child channel only.
struct ManagerApplyClaimReleaseRequest final {
  manyforge::core::CellAdmissionClaimReleaseDTO release;
  manyforge::core::ManagerApplyReleaseOutcomeDTO outcome;

  friend bool operator==(const ManagerApplyClaimReleaseRequest &,
                         const ManagerApplyClaimReleaseRequest &) = default;
};

/// Private, same-host forwarding protocol for the native execution child.
/// The command strings are deliberately part of the wire format: unlike an
/// enum, they make packet captures and malformed-command diagnostics
/// unambiguous.
using KernelCellAdmissionRpcRequest =
    std::variant<std::monostate,
                 manyforge::core::CellAdmissionReadinessSnapshotDTO,
                 manyforge::core::CellAdmissionClaimIntentDTO,
                 ProgramResetClaimAcquireRequest,
                 ManagerApplyClaimAcquireRequest,
                 manyforge::core::CellAdmissionClaimReleaseDTO,
                 ManagerApplyClaimReleaseRequest,
                 manyforge::core::CellAdmissionRecoveryReceiptQueryDTO,
                 manyforge::core::ProgramResetSceneAuthorizationDTO,
                 manyforge::core::ManagerLivenessObservationDTO,
                 manyforge::core::OperatorAlarmAttentionSnapshotDTO>;

/// `cell_admission_publish_operator_alarm_attention` is accepted only on the
/// execution command socket (Headless to Kernel). It is never forwarded over
/// the Manager-child binary channel, whose codec refuses it.
inline constexpr std::string_view kPublishOperatorAlarmAttentionCommand =
    "cell_admission_publish_operator_alarm_attention";

struct KernelCellAdmissionRpcPacket final {
  std::string command;
  KernelCellAdmissionRpcRequest request;
};

using KernelCellAdmissionRpcResponse =
    std::variant<std::monostate,
                 manyforge::core::AuthoringAdmissionDTO,
                 manyforge::core::CellAdmissionClaimDTO,
                 manyforge::core::CellAdmissionRecoveryReceiptDTO,
                 manyforge::core::ErrorDTO,
                 manyforge::core::OperatorAlarmAttentionAcceptedDTO>;

struct KernelCellAdmissionRpcResponsePacket final {
  std::string command;
  KernelCellAdmissionRpcResponse response;
};

[[nodiscard]] KernelCellAdmissionRpcResponse DispatchKernelCellAdmissionRpc(
    manyforge::core::IKernelCellAdmissionPort &kernel,
    const KernelCellAdmissionRpcPacket &request,
    std::shared_ptr<manyforge::core::IIoSignalService>* actual_retired_service = nullptr);

[[nodiscard]] bool
IsKernelCellAdmissionRpcPacket(std::string_view bytes) noexcept;
[[nodiscard]] std::string EncodeKernelCellAdmissionRpcRequest(
    const KernelCellAdmissionRpcPacket &request);
[[nodiscard]] KernelCellAdmissionRpcPacket
DecodeKernelCellAdmissionRpcRequest(std::string_view bytes);
[[nodiscard]] std::string EncodeKernelCellAdmissionRpcResponse(
    std::string_view command, const KernelCellAdmissionRpcResponse &response);
[[nodiscard]] KernelCellAdmissionRpcResponsePacket
DecodeKernelCellAdmissionRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail
