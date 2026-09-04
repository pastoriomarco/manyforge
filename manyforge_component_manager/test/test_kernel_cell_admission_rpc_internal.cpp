#include <gtest/gtest.h>

#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>

namespace {
namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;

core::ManagerLivenessObservationDTO Observation() {
  core::ManagerLivenessStatusDTO status{
      core::CellId("cell"),
      core::RuntimeUnitId("program-runner.cell"),
      core::RuntimeEpoch("runner-epoch"),
      core::RuntimeUnitId("component-manager.cell"),
      core::RuntimeEpoch("manager-epoch"),
      core::RequestOwnerEpoch("manager-owner"),
      core::RuntimePolicyId("manyforge.program-runner.manager-presence.v1"),
      core::RuntimePolicyRevision("1"),
      7U,
      3U,
      0,
      core::ManagerLivenessStateDTO::kCurrent,
      false,
      std::nullopt};
  return {status, core::RetainedBundleId("bundle"),
          core::ContentDigest("sha256:" + std::string(64U, 'a')),
          core::ComputeManagerLivenessStatusDigest(status)};
}

class RecordingKernel final : public core::IKernelCellAdmissionPort {
 public:
  core::AuthoringAdmissionDTO GetAuthoringAdmission() const override {
    return {false, core::AdmissionGeneration(1U)};
  }
  core::Result<core::AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO&) override {
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        GetAuthoringAdmission());
  }
  core::Result<void> Publish(
      const core::ManagerLivenessObservationDTO& observation) override {
    published = observation;
    if (failure.has_value()) {
      return core::Result<void>::Err(*failure);
    }
    return core::Result<void>::Ok();
  }
  core::Result<core::CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO&) override {
    return core::Result<core::CellAdmissionClaimDTO>::Err(
        core::ErrorDTO{"unused", "unused", {}});
  }
  core::Result<core::AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO&) override {
    return core::Result<core::AuthoringAdmissionDTO>::Err(
        core::ErrorDTO{"unused", "unused", {}});
  }
  core::Result<core::AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const core::CellAdmissionReconcileDTO&) override {
    return core::Result<core::AuthoringAdmissionDTO>::Err(
        core::ErrorDTO{"unused", "unused", {}});
  }
  core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO&) const override {
    return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Err(
        core::ErrorDTO{"unused", "unused", {}});
  }
  core::Result<void> ValidateProgramResetSceneAuthorization(
      const core::ProgramResetSceneAuthorizationDTO& authorization)
      const override {
    reset_scene_authorization = authorization;
    if (failure.has_value()) {
      return core::Result<void>::Err(*failure);
    }
    return core::Result<void>::Ok();
  }

  std::optional<core::ManagerLivenessObservationDTO> published;
  mutable std::optional<core::ProgramResetSceneAuthorizationDTO>
      reset_scene_authorization;
  std::optional<core::ErrorDTO> failure;
};

TEST(KernelCellAdmissionRpcInternal,
     LivenessObservationRoundTripsExactlyAndDispatchesTypedPublish) {
  const component_manager::detail::KernelCellAdmissionRpcPacket request{
      "cell_admission_publish_liveness", Observation()};
  const auto decoded =
      component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
          component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
              request));
  ASSERT_EQ(decoded.command, request.command);
  ASSERT_EQ(std::get<core::ManagerLivenessObservationDTO>(decoded.request),
            Observation());

  RecordingKernel kernel;
  const auto response =
      component_manager::detail::DispatchKernelCellAdmissionRpc(kernel, decoded);
  ASSERT_TRUE(kernel.published.has_value());
  EXPECT_EQ(*kernel.published, Observation());
  EXPECT_TRUE(std::holds_alternative<std::monostate>(response));
  const auto response_packet =
      component_manager::detail::DecodeKernelCellAdmissionRpcResponse(
          component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
              decoded.command, response));
  EXPECT_EQ(response_packet.command, decoded.command);
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>(response_packet.response));
}

TEST(KernelCellAdmissionRpcInternal,
     LivenessObservationRequestRejectsDigestDriftAndWrongDto) {
  const component_manager::detail::KernelCellAdmissionRpcPacket request{
      "cell_admission_publish_liveness", Observation()};
  auto malformed =
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(request);
  malformed.back() = malformed.back() == '0' ? '1' : '0';
  EXPECT_THROW(
      component_manager::detail::DecodeKernelCellAdmissionRpcRequest(malformed),
      std::invalid_argument);
  EXPECT_THROW(
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
          {"cell_admission_publish_liveness", std::monostate{}}),
      std::invalid_argument);
}

TEST(KernelCellAdmissionRpcInternal, LivenessPublicationFailureIsPreserved) {
  RecordingKernel kernel;
  kernel.failure =
      core::ErrorDTO{"stale_liveness", "Runner observation is stale", {}};
  const auto response = component_manager::detail::DispatchKernelCellAdmissionRpc(
      kernel, {"cell_admission_publish_liveness", Observation()});
  ASSERT_TRUE(std::holds_alternative<core::ErrorDTO>(response));
  EXPECT_EQ(std::get<core::ErrorDTO>(response), *kernel.failure);
}

TEST(KernelCellAdmissionRpcInternal,
     ProgramResetSceneAuthorizationRoundTripsAndDispatches) {
  const core::ProgramResetSceneAuthorizationDTO authorization{
      core::RequestOwnerEpoch("program-runner.epoch.2"),
      core::ProgramRunId("program-cycle.2"),
      core::CellAdmissionClaimGeneration(9U),
      core::ContentDigest("sha256:" + std::string(64U, 'b'))};
  const component_manager::detail::KernelCellAdmissionRpcPacket request{
      "cell_admission_validate_program_reset_scene", authorization};
  const auto decoded =
      component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
          component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
              request));
  ASSERT_EQ(decoded.command, request.command);
  EXPECT_EQ(std::get<core::ProgramResetSceneAuthorizationDTO>(decoded.request),
            authorization);

  RecordingKernel kernel;
  const auto response =
      component_manager::detail::DispatchKernelCellAdmissionRpc(kernel,
                                                                 decoded);
  EXPECT_EQ(kernel.reset_scene_authorization, authorization);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(response));
  const auto response_packet =
      component_manager::detail::DecodeKernelCellAdmissionRpcResponse(
          component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
              decoded.command, response));
  EXPECT_TRUE(
      std::holds_alternative<std::monostate>(response_packet.response));
}

}  // namespace
