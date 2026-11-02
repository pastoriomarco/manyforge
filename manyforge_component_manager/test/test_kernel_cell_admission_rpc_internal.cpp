#include <gtest/gtest.h>

#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>

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
      const core::CellAdmissionReadinessSnapshotDTO &readiness,
      std::shared_ptr<core::IIoSignalService>* actual_retired_service = nullptr) override {
    if (actual_retired_service) actual_retired_service->reset();
    updated_readiness = readiness;
    if (actual_retired_service) *actual_retired_service = retired_service;
    if (failure) return core::Result<core::AuthoringAdmissionDTO>::Err(*failure);
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
  std::optional<core::CellAdmissionReadinessSnapshotDTO> updated_readiness;
  mutable std::optional<core::ProgramResetSceneAuthorizationDTO>
      reset_scene_authorization;
  std::optional<core::ErrorDTO> failure;
  std::shared_ptr<core::IIoSignalService> retired_service;
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

TEST(KernelCellAdmissionRpcInternal, SelectedIdleResetStatusAndAuthorizationRoundTrip) {
  const auto digest = core::ContentDigest("sha256:" + std::string(64U, 'b'));
  const core::AcceptedProgramResetDTO accepted{
      core::RequestOwnerEpoch("operator.1"), core::RequestId("reset.1"), digest,
      true, true, core::CellAdmissionClaimGeneration(9U)};
  core::CellAdmissionReadinessSnapshotDTO readiness;
  readiness.program_cycle_status_current = true;
  readiness.program_cycle_status = core::ProgramCycleStatusDTO{
      core::RequestOwnerEpoch("runner.1"), 3U, core::ProgramCycleStateDTO::kResetting,
      std::nullopt, std::nullopt, std::nullopt, accepted};
  const component_manager::detail::KernelCellAdmissionRpcPacket request{
      "cell_admission_update_readiness", readiness};
  const auto decoded = component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(request));
  EXPECT_EQ(std::get<core::CellAdmissionReadinessSnapshotDTO>(decoded.request), readiness);

  const core::ProgramResetSceneAuthorizationDTO authorization{
      core::RequestOwnerEpoch("runner.1"), core::ProgramRunId(""),
      core::CellAdmissionClaimGeneration(0U), core::ContentDigest(""),
      core::ProgramResetClaimSceneAuthorizationDTO{
          3U, accepted.request_owner_epoch, accepted.request_id,
          accepted.request_payload_digest, *accepted.reset_claim_generation}};
  const auto auth_decoded = component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
          {"cell_admission_validate_program_reset_scene", authorization}));
  EXPECT_EQ(std::get<core::ProgramResetSceneAuthorizationDTO>(auth_decoded.request), authorization);
}

core::CellAdmissionReadinessSnapshotDTO ReadinessWithRunnerLoss() {
  core::CellAdmissionReadinessSnapshotDTO readiness;
  readiness.program_runner_loss = core::ProgramRunnerContactLossDTO{
      core::RequestOwnerEpoch("manager.owner"),
      core::RuntimeEpoch("manager.runtime"),
      core::ProgramCycleStatusDTO{
          core::RequestOwnerEpoch("runner.owner"), 7U,
          core::ProgramCycleStateDTO::kRunning, core::ProgramRunId("run.1"),
          core::CellAdmissionClaimGeneration(3U), std::nullopt, std::nullopt},
      "program_runner_liveness_probe", "Program Runner contact was lost."};
  return readiness;
}

TEST(KernelCellAdmissionRpcInternal,
     ReadinessWithoutRunnerLossPreservesLegacyPacketBytes) {
  const core::CellAdmissionReadinessSnapshotDTO readiness;
  const auto encoded =
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
          {"cell_admission_update_readiness", readiness});
  std::string hex;
  for (const unsigned char byte : encoded) {
    constexpr char digits[] = "0123456789abcdef";
    hex.push_back(digits[byte >> 4U]);
    hex.push_back(digits[byte & 15U]);
  }
  EXPECT_EQ(
      hex,
      "4d46434132000000000000001f63656c6c5f61646d697373696f6e5f7570646174655f"
      "72656164696e6573730000000000000000000000000000000000000000000000000000"
      "00000000000000000000000000000000000000");
  const auto decoded =
      component_manager::detail::DecodeKernelCellAdmissionRpcRequest(encoded);
  EXPECT_EQ(std::get<core::CellAdmissionReadinessSnapshotDTO>(decoded.request),
            readiness);
}

TEST(KernelCellAdmissionRpcInternal,
     RunnerContactLossReadinessRoundTripsAndDispatches) {
  for (const auto *predicate :
       {"program_runner_liveness_probe", "program_runner_liveness_renewal",
        "program_runner_reconciliation_renewal"}) {
    auto readiness = ReadinessWithRunnerLoss();
    readiness.program_runner_loss->failure_predicate = predicate;
    const auto encoded =
        component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
            {"cell_admission_update_readiness", readiness});
    const auto decoded =
        component_manager::detail::DecodeKernelCellAdmissionRpcRequest(encoded);
    EXPECT_EQ(
        std::get<core::CellAdmissionReadinessSnapshotDTO>(decoded.request),
        readiness);
    EXPECT_EQ(
        component_manager::detail::EncodeKernelCellAdmissionRpcRequest(decoded),
        encoded);
    RecordingKernel kernel;
    const auto response =
        component_manager::detail::DispatchKernelCellAdmissionRpc(kernel,
                                                                  decoded);
    EXPECT_EQ(kernel.updated_readiness, readiness);
    EXPECT_TRUE(std::holds_alternative<core::AuthoringAdmissionDTO>(response));
  }
}

TEST(KernelCellAdmissionRpcInternal,
     RunnerContactLossReadinessRejectsInvalidDtoAndExtension) {
  for (const int malformed : {0, 1, 2, 3, 4, 5, 6, 7}) {
    auto readiness = ReadinessWithRunnerLoss();
    auto &loss = *readiness.program_runner_loss;
    if (malformed == 0)
      loss.manager_owner_epoch = core::RequestOwnerEpoch("");
    if (malformed == 1)
      loss.manager_runtime_epoch = core::RuntimeEpoch(std::string(129U, 'x'));
    if (malformed == 2)
      loss.failure_predicate = "unknown_contact_failure";
    if (malformed == 3)
      loss.message = std::string(1025U, 'x');
    if (malformed == 4)
      loss.established_cycle.program_run_id.reset();
    if (malformed == 5)
      loss.established_cycle.cycle_claim_generation.reset();
    if (malformed == 6)
      loss.message = "e\xCC\x81";
    if (malformed == 7)
      loss.established_cycle.state =
          core::ProgramCycleStateDTO::kTerminalAwaitingReset;
    EXPECT_THROW(component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
                     {"cell_admission_update_readiness", readiness}),
                 std::invalid_argument);
  }
  const auto readiness = ReadinessWithRunnerLoss();
  const auto encoded =
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
          {"cell_admission_update_readiness", readiness});
  auto malformed_predicate = encoded;
  const auto predicate_offset =
      malformed_predicate.find("program_runner_liveness_probe");
  ASSERT_NE(predicate_offset, std::string::npos);
  malformed_predicate[predicate_offset] = 'X';
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                   malformed_predicate),
               std::invalid_argument);
  const auto base =
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
          {"cell_admission_update_readiness",
           core::CellAdmissionReadinessSnapshotDTO{}});
  EXPECT_EQ(encoded.substr(0U, base.size()), base);
  ASSERT_GT(encoded.size(), base.size());
  EXPECT_EQ(encoded[base.size()], '\1');
  for (const char marker : {'\0', '\2'}) {
    auto malformed_marker = encoded;
    malformed_marker[base.size()] = marker;
    EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                     malformed_marker),
                 std::invalid_argument);
  }
  for (std::size_t size = base.size() + 1U; size < encoded.size(); ++size) {
    EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                     encoded.substr(0U, size)),
                 std::invalid_argument)
        << size;
  }
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                   encoded + "x"),
               std::invalid_argument);
  auto malformed_bool = encoded;
  const auto predicate = malformed_bool.find("program_runner_liveness_probe");
  ASSERT_GT(predicate, sizeof(std::uint64_t));
  malformed_bool[predicate - sizeof(std::uint64_t) - 1U] = '\2';
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                   malformed_bool),
               std::invalid_argument);
}

TEST(KernelCellAdmissionRpcInternal,
     AffectedServicePinSurvivesUpdateErrorAndIsNeverPartOfTheWire) {
  core::FakeClock clock;
  auto created = core::IoSignalSubsystem::Create(
      {core::RequestOwnerEpoch("host"), 1U, {}},
      {1U, 1U, 4U, 4U, 256U, 256U}, clock);
  ASSERT_TRUE(created.ok());
  std::shared_ptr<core::IIoSignalService> service(std::move(created.value()));
  RecordingKernel kernel;
  kernel.retired_service = service;
  const component_manager::detail::KernelCellAdmissionRpcPacket request{
      "cell_admission_update_readiness", ReadinessWithRunnerLoss()};
  const auto wire =
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(request);
  for (const bool fails_after_mutation : {false, true}) {
    if (fails_after_mutation)
      kernel.failure = core::ErrorDTO{"journal_failed", "retained failure", {}};
    std::shared_ptr<core::IIoSignalService> affected;
    const auto response = component_manager::detail::DispatchKernelCellAdmissionRpc(
        kernel, request, &affected);
    EXPECT_EQ(affected, service);
    EXPECT_EQ(std::holds_alternative<core::ErrorDTO>(response), fails_after_mutation);
    EXPECT_EQ(component_manager::detail::EncodeKernelCellAdmissionRpcRequest(request), wire);
  }
  // A later unchanged heartbeat must not reuse an earlier notification.
  kernel.retired_service.reset();
  kernel.failure.reset();
  auto affected = service;
  const component_manager::detail::KernelCellAdmissionRpcPacket heartbeat{
      "cell_admission_update_readiness", core::CellAdmissionReadinessSnapshotDTO{}};
  const auto response = component_manager::detail::DispatchKernelCellAdmissionRpc(
      kernel, heartbeat, &affected);
  EXPECT_FALSE(affected);
  EXPECT_TRUE(std::holds_alternative<core::AuthoringAdmissionDTO>(response));
}

} // namespace
