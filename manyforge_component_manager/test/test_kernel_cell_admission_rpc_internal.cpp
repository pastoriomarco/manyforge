#include <gtest/gtest.h>

#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/types/request_key.hpp>

#include <cstdint>

namespace {
namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;

core::ManagerLivenessObservationDTO Observation() {
  core::ManagerLivenessStatusDTO status{
      core::CellId("cell"),
      core::RuntimeUnitId("program-runner.cell"),
      core::RuntimeEpoch("runner-epoch"),
      core::RuntimeUnitId("component-manager.cell"),
      core::RuntimePolicyId("manyforge.program-runner.manager-presence.v1"),
      core::RuntimePolicyRevision("1"),
      7U,
      3U,
      core::ManagerLivenessStateDTO::kCurrent,
      false,
      std::nullopt};
  return {status, core::RetainedBundleId("bundle"),
          core::ContentDigest("sha256:" + std::string(64U, 'a'))};
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
  core::Result<core::CellAdmissionClaimDTO> AcquireProgramResetClaim(
      const core::CellAdmissionClaimIntentDTO& intent,
      const core::ProgramCycleStatusDTO& status) override {
    reset_acquired = std::pair{intent, status};
    return core::Result<core::CellAdmissionClaimDTO>::Err(
        core::ErrorDTO{"seen", "seen", {}});
  }
  core::Result<core::CellAdmissionClaimDTO> AcquireManagerApplyClaim(
      const core::CellAdmissionClaimIntentDTO& intent,
      const core::RetainedBundleId& candidate) override {
    manager_apply_acquired = std::pair{intent, candidate};
    return core::Result<core::CellAdmissionClaimDTO>::Err(
        core::ErrorDTO{"seen", "seen", {}});
  }
  core::Result<core::AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO&) override {
    return core::Result<core::AuthoringAdmissionDTO>::Err(
        core::ErrorDTO{"unused", "unused", {}});
  }
  core::Result<core::AuthoringAdmissionDTO> ReleaseManagerApplyClaim(
      const core::CellAdmissionClaimReleaseDTO& release,
      const core::ManagerApplyReleaseOutcomeDTO& outcome) override {
    manager_apply_released = std::pair{release, outcome};
    return core::Result<core::AuthoringAdmissionDTO>::Err(
        core::ErrorDTO{"seen", "seen", {}});
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
  std::optional<std::pair<core::CellAdmissionClaimIntentDTO, core::ProgramCycleStatusDTO>>
      reset_acquired;
  std::optional<std::pair<core::CellAdmissionClaimIntentDTO, core::RetainedBundleId>>
      manager_apply_acquired;
  std::optional<std::pair<core::CellAdmissionClaimReleaseDTO, core::ManagerApplyReleaseOutcomeDTO>>
      manager_apply_released;
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
     LivenessObservationRequestRejectsMalformedDigestAndWrongDto) {
  const component_manager::detail::KernelCellAdmissionRpcPacket request{
      "cell_admission_publish_liveness", Observation()};
  auto malformed =
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(request);
  // The packet ends with the required-runtime-set digest; a non-hex last
  // character (same length) is not a sha256 digest and is refused.
  ASSERT_EQ(malformed.back(), 'a');
  malformed.back() = 'G';
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
  const auto digest = core::ContentDigest("sha256:" + std::string(64U, 'b'));
  // MFCA3 carries the runner's current cycle status last (OD-139).
  const core::ProgramResetSceneAuthorizationDTO authorization{
      core::ProgramRunNumber(2U),
      core::CellAdmissionClaimGeneration(9U),
      digest, std::nullopt,
      core::ProgramCycleStatusDTO{
          4U, core::ProgramCycleStateDTO::kResetting, core::ProgramRunNumber(2U),
          core::CellAdmissionClaimGeneration(9U), digest,
          core::AcceptedProgramResetDTO{
              core::RequestKey::Make(
                  core::RequestKeyClass::kInternal, 0U,
                  static_cast<std::uint64_t>(core::RequestKeyMinter::kManager), 2U),
              digest, true, false, std::nullopt}}};
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
      core::RequestKey::Make(
          core::RequestKeyClass::kInternal, 0U,
          static_cast<std::uint64_t>(core::RequestKeyMinter::kManager), 1U),
      digest, true, true, core::CellAdmissionClaimGeneration(9U)};
  core::CellAdmissionReadinessSnapshotDTO readiness;
  readiness.program_cycle_status_current = true;
  readiness.program_cycle_status = core::ProgramCycleStatusDTO{
      3U, core::ProgramCycleStateDTO::kResetting,
      std::nullopt, std::nullopt, std::nullopt, accepted};
  const component_manager::detail::KernelCellAdmissionRpcPacket request{
      "cell_admission_update_readiness", readiness};
  const auto decoded = component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(request));
  EXPECT_EQ(std::get<core::CellAdmissionReadinessSnapshotDTO>(decoded.request), readiness);

  const core::ProgramResetSceneAuthorizationDTO authorization{
      core::ProgramRunNumber(0U),
      core::CellAdmissionClaimGeneration(0U), core::ContentDigest(""),
      core::ProgramResetClaimSceneAuthorizationDTO{
          3U, accepted.request_key, accepted.request_payload_digest, *accepted.reset_claim_generation},
      *readiness.program_cycle_status};
  const auto auth_decoded = component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
      component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
          {"cell_admission_validate_program_reset_scene", authorization}));
  EXPECT_EQ(std::get<core::ProgramResetSceneAuthorizationDTO>(auth_decoded.request), authorization);
}

// OD-139: a Program Reset claim acquisition carries the runner's status beside
// its intent; dispatch hands both to the Kernel. The Manager-child binary
// channel never carries it.
TEST(KernelCellAdmissionRpcInternal, ProgramResetAcquireDispatchesTheCarriedStatus) {
  const auto digest = core::ContentDigest("sha256:" + std::string(64U, 'c'));
  const auto key = core::RequestKey::Make(
      core::RequestKeyClass::kInternal, 0U,
      static_cast<std::uint64_t>(core::RequestKeyMinter::kManager), 3U);
  const core::CellAdmissionClaimIntentDTO intent{
      core::CellAdmissionClaimKindDTO::kProgramReset, core::ApplicationRole::kProgramRunner,
      std::nullopt, key.WithPart(2U), core::AdmissionGeneration(1U), {}, std::nullopt,
      std::nullopt};
  const core::ProgramCycleStatusDTO status{
      5U, core::ProgramCycleStateDTO::kResetting, std::nullopt, std::nullopt, std::nullopt,
      core::AcceptedProgramResetDTO{key, digest, true, false, std::nullopt}};
  const component_manager::detail::KernelCellAdmissionRpcPacket packet{
      "cell_admission_acquire",
      component_manager::detail::ProgramResetClaimAcquireRequest{intent, status}};
  RecordingKernel kernel;
  const auto response = component_manager::detail::DispatchKernelCellAdmissionRpc(kernel, packet);
  ASSERT_TRUE(kernel.reset_acquired.has_value());
  EXPECT_EQ(kernel.reset_acquired->first, intent);
  EXPECT_EQ(kernel.reset_acquired->second, status);
  EXPECT_EQ(std::get<core::ErrorDTO>(response).code, "seen");
  EXPECT_THROW(component_manager::detail::EncodeKernelCellAdmissionRpcRequest(packet),
               std::invalid_argument);
}

core::RequestKey ClaimKey(const std::uint64_t sequence) {
  return core::RequestKey::Make(
      core::RequestKeyClass::kInternal, 0U,
      static_cast<std::uint64_t>(core::RequestKeyMinter::kManager), sequence);
}

core::CellAdmissionClaimIntentDTO ClaimIntent(
    const core::CellAdmissionClaimKindDTO kind, const core::ApplicationRole role,
    std::optional<core::CellAdmissionOwnerTransactionId> id) {
  core::CellAdmissionClaimIntentDTO intent{
      kind, role, std::move(id), ClaimKey(11U), core::AdmissionGeneration(4U),
      core::CellAdmissionScopeDTO{{core::ResourceId("robot.arm")}, {}},
      std::nullopt, std::nullopt};
  if (kind == core::CellAdmissionClaimKindDTO::kProgramCycle)
    intent.program_cycle_evidence = core::ProgramCycleAdmissionEvidenceDTO{
        core::ProgramRevision("revision-1"), core::ProgramRevision("revision-1")};
  return intent;
}

core::CellAdmissionClaimReleaseDTO ClaimRelease(
    const core::ApplicationRole role,
    std::optional<core::CellAdmissionOwnerTransactionId> id) {
  return {role, std::move(id), ClaimKey(11U), core::AdmissionGeneration(4U),
          core::CellAdmissionClaimGeneration(6U),
          {core::CellAdmissionSettlementKindDTO::kOwnerCommit,
           core::ContentDigest("sha256:" + std::string(64U, 'd'))}};
}

// Intents with and without an owner transaction id decode to the same typed
// DTO and re-encode to the same bytes.
TEST(KernelCellAdmissionRpcInternal, ClaimIntentRoundTripsWithAndWithoutTransactionId) {
  for (const auto &intent :
       {ClaimIntent(core::CellAdmissionClaimKindDTO::kManagerApply,
                    core::ApplicationRole::kManager, core::ApplyId(3U)),
        ClaimIntent(core::CellAdmissionClaimKindDTO::kProgramCycle,
                    core::ApplicationRole::kProgramRunner, core::ProgramRunNumber(4U)),
        ClaimIntent(core::CellAdmissionClaimKindDTO::kManualOperation,
                    core::ApplicationRole::kManualOperationOwner, core::OperationNumber(5U)),
        ClaimIntent(core::CellAdmissionClaimKindDTO::kAuthoringMutation,
                    core::ApplicationRole::kAuthoringOwner, std::nullopt)}) {
    const component_manager::detail::KernelCellAdmissionRpcPacket packet{
        "cell_admission_acquire", intent};
    const auto encoded =
        component_manager::detail::EncodeKernelCellAdmissionRpcRequest(packet);
    const auto decoded =
        component_manager::detail::DecodeKernelCellAdmissionRpcRequest(encoded);
    ASSERT_EQ(decoded.command, packet.command);
    const auto &round_trip = std::get<core::CellAdmissionClaimIntentDTO>(decoded.request);
    EXPECT_EQ(round_trip, intent);
    EXPECT_EQ(round_trip.owner_transaction_id.has_value(),
              intent.owner_transaction_id.has_value());
    if (intent.owner_transaction_id) {
      EXPECT_EQ(round_trip.owner_transaction_id->index(),
                intent.owner_transaction_id->index());
    }
    EXPECT_EQ(component_manager::detail::EncodeKernelCellAdmissionRpcRequest(decoded),
              encoded);
  }
}

// Releases with and without an owner transaction id round-trip exactly.
TEST(KernelCellAdmissionRpcInternal, ClaimReleaseRoundTripsWithAndWithoutTransactionId) {
  for (const auto &release :
       {ClaimRelease(core::ApplicationRole::kManager, core::ApplyId(3U)),
        ClaimRelease(core::ApplicationRole::kProgramRunner, core::ProgramRunNumber(4U)),
        ClaimRelease(core::ApplicationRole::kManualOperationOwner, core::OperationNumber(5U)),
        ClaimRelease(core::ApplicationRole::kAuthoringOwner, std::nullopt)}) {
    const component_manager::detail::KernelCellAdmissionRpcPacket packet{
        "cell_admission_release", release};
    const auto encoded =
        component_manager::detail::EncodeKernelCellAdmissionRpcRequest(packet);
    const auto decoded =
        component_manager::detail::DecodeKernelCellAdmissionRpcRequest(encoded);
    ASSERT_EQ(decoded.command, packet.command);
    EXPECT_EQ(std::get<core::CellAdmissionClaimReleaseDTO>(decoded.request), release);
    EXPECT_EQ(component_manager::detail::EncodeKernelCellAdmissionRpcRequest(decoded),
              encoded);
  }
}

// A plain acquire and a plain release end with a zero presence byte.
TEST(KernelCellAdmissionRpcInternal, PlainAcquireAndReleaseEndWithAbsentManagerApplyFacts) {
  const auto acquire = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {"cell_admission_acquire",
       ClaimIntent(core::CellAdmissionClaimKindDTO::kManagerApply,
                   core::ApplicationRole::kManager, core::ApplyId(3U))});
  const auto release = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {"cell_admission_release", ClaimRelease(core::ApplicationRole::kManager, core::ApplyId(3U))});
  EXPECT_EQ(acquire.back(), '\0');
  EXPECT_EQ(release.back(), '\0');
  EXPECT_TRUE(std::holds_alternative<core::CellAdmissionClaimIntentDTO>(
      component_manager::detail::DecodeKernelCellAdmissionRpcRequest(acquire).request));
  EXPECT_TRUE(std::holds_alternative<core::CellAdmissionClaimReleaseDTO>(
      component_manager::detail::DecodeKernelCellAdmissionRpcRequest(release).request));
}

// A Manager-Apply acquire carries its candidate bundle and dispatches to the
// Manager-Apply port method.
TEST(KernelCellAdmissionRpcInternal, ManagerApplyClaimAcquireRoundTripsAndDispatches) {
  const component_manager::detail::ManagerApplyClaimAcquireRequest request{
      ClaimIntent(core::CellAdmissionClaimKindDTO::kManagerApply,
                  core::ApplicationRole::kManager, core::ApplyId(3U)),
      core::RetainedBundleId("candidate-bundle")};
  const component_manager::detail::KernelCellAdmissionRpcPacket packet{
      "cell_admission_acquire", request};
  const auto encoded = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(packet);
  const auto decoded = component_manager::detail::DecodeKernelCellAdmissionRpcRequest(encoded);
  ASSERT_EQ(decoded.command, packet.command);
  EXPECT_EQ(std::get<component_manager::detail::ManagerApplyClaimAcquireRequest>(decoded.request),
            request);
  EXPECT_EQ(component_manager::detail::EncodeKernelCellAdmissionRpcRequest(decoded), encoded);

  RecordingKernel kernel;
  static_cast<void>(component_manager::detail::DispatchKernelCellAdmissionRpc(kernel, decoded));
  ASSERT_TRUE(kernel.manager_apply_acquired.has_value());
  EXPECT_EQ(kernel.manager_apply_acquired->first, request.intent);
  EXPECT_EQ(kernel.manager_apply_acquired->second, request.candidate_retained_bundle_id);
}

// A Manager-Apply release carries the Apply's outcome and dispatches to the
// Manager-Apply port method.
TEST(KernelCellAdmissionRpcInternal, ManagerApplyClaimReleaseRoundTripsAndDispatches) {
  for (const bool succeeded : {false, true}) {
    const component_manager::detail::ManagerApplyClaimReleaseRequest request{
        ClaimRelease(core::ApplicationRole::kManager, core::ApplyId(3U)),
        core::ManagerApplyReleaseOutcomeDTO{core::ApplyId(3U), core::ApplyGeneration(2U),
                                            succeeded}};
    const component_manager::detail::KernelCellAdmissionRpcPacket packet{
        "cell_admission_release", request};
    const auto encoded = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(packet);
    const auto decoded = component_manager::detail::DecodeKernelCellAdmissionRpcRequest(encoded);
    ASSERT_EQ(decoded.command, packet.command);
    EXPECT_EQ(std::get<component_manager::detail::ManagerApplyClaimReleaseRequest>(decoded.request),
              request);
    EXPECT_EQ(component_manager::detail::EncodeKernelCellAdmissionRpcRequest(decoded), encoded);

    RecordingKernel kernel;
    static_cast<void>(component_manager::detail::DispatchKernelCellAdmissionRpc(kernel, decoded));
    ASSERT_TRUE(kernel.manager_apply_released.has_value());
    EXPECT_EQ(kernel.manager_apply_released->first, request.release);
    EXPECT_EQ(kernel.manager_apply_released->second, request.outcome);
  }
}

// Only a Manager-Apply intent may carry a candidate bundle.
TEST(KernelCellAdmissionRpcInternal, CandidateBundleOnOtherClaimKindIsRefused) {
  const auto encoded = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {"cell_admission_acquire",
       component_manager::detail::ManagerApplyClaimAcquireRequest{
           ClaimIntent(core::CellAdmissionClaimKindDTO::kAuthoringMutation,
                       core::ApplicationRole::kAuthoringOwner, std::nullopt),
           core::RetainedBundleId("candidate-bundle")}});
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(encoded),
               std::invalid_argument);
}

// The layout without the trailing presence byte is refused.
TEST(KernelCellAdmissionRpcInternal, AcquireAndReleaseWithoutPresenceByteAreRefused) {
  auto acquire = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {"cell_admission_acquire",
       ClaimIntent(core::CellAdmissionClaimKindDTO::kManagerApply,
                   core::ApplicationRole::kManager, core::ApplyId(3U))});
  auto release = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {"cell_admission_release", ClaimRelease(core::ApplicationRole::kManager, core::ApplyId(3U))});
  acquire.pop_back();
  release.pop_back();
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(acquire),
               std::invalid_argument);
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(release),
               std::invalid_argument);
}

// A transaction id on a role that has no transaction number is refused on
// decode, for an intent and for a release.
TEST(KernelCellAdmissionRpcInternal, TransactionIdOnRoleWithoutNumberIsRefused) {
  // Layout: magic, command string (u64 length + bytes), then the DTO; an
  // intent starts with kind and role (u64 each), a release with role.
  const auto id_offset = [](const std::string &command, const std::size_t leading_u64s) {
    return std::string_view("MFCA3").size() + sizeof(std::uint64_t) + command.size() +
           leading_u64s * sizeof(std::uint64_t);
  };
  // Splices the present id (presence byte + u64) of `with_id` into `without_id`.
  const auto splice = [](const std::string &without_id, const std::string &with_id,
                         const std::size_t offset) {
    EXPECT_EQ(without_id.at(offset), '\0');
    EXPECT_EQ(with_id.at(offset), '\1');
    EXPECT_EQ(with_id.size(), without_id.size() + sizeof(std::uint64_t));
    return without_id.substr(0U, offset) +
           with_id.substr(offset, 1U + sizeof(std::uint64_t)) +
           without_id.substr(offset + 1U);
  };

  const std::string acquire = "cell_admission_acquire";
  const auto authoring = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {acquire, ClaimIntent(core::CellAdmissionClaimKindDTO::kAuthoringMutation,
                            core::ApplicationRole::kAuthoringOwner, std::nullopt)});
  const auto manager = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {acquire, ClaimIntent(core::CellAdmissionClaimKindDTO::kManagerApply,
                            core::ApplicationRole::kManager, core::ApplyId(3U))});
  const auto intent_offset = id_offset(acquire, 2U);
  ASSERT_LT(intent_offset, authoring.size());
  EXPECT_NO_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(authoring));
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                   splice(authoring, manager, intent_offset)),
               std::invalid_argument);

  const std::string release_command = "cell_admission_release";
  const auto authoring_release = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {release_command, ClaimRelease(core::ApplicationRole::kAuthoringOwner, std::nullopt)});
  const auto manager_release = component_manager::detail::EncodeKernelCellAdmissionRpcRequest(
      {release_command, ClaimRelease(core::ApplicationRole::kManager, core::ApplyId(3U))});
  const auto release_offset = id_offset(release_command, 1U);
  ASSERT_LT(release_offset, authoring_release.size());
  EXPECT_NO_THROW(
      component_manager::detail::DecodeKernelCellAdmissionRpcRequest(authoring_release));
  EXPECT_THROW(component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                   splice(authoring_release, manager_release, release_offset)),
               std::invalid_argument);
}

core::CellAdmissionReadinessSnapshotDTO ReadinessWithRunnerLoss() {
  core::CellAdmissionReadinessSnapshotDTO readiness;
  readiness.program_runner_loss = core::ProgramRunnerContactLossDTO{
      core::ProgramCycleStatusDTO{
          7U, core::ProgramCycleStateDTO::kRunning, core::ProgramRunNumber(1U),
          core::CellAdmissionClaimGeneration(3U), std::nullopt, std::nullopt},
      "program_runner_liveness_probe", "Program Runner contact was lost.",
      "program_runner_contact_lost"};
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
      "4d46434133000000000000001f63656c6c5f61646d697373696f6e5f7570646174655f"
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
        "program_runner_reconciliation_renewal", "program_runner_fault"}) {
    auto readiness = ReadinessWithRunnerLoss();
    readiness.program_runner_loss->failure_predicate = predicate;
    if (std::string_view(predicate) == "program_runner_fault")
      readiness.program_runner_loss->fault_code = "program_tick_worker_failed";
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
  for (const int malformed : {2, 3, 4, 5, 6, 7, 8}) {
    auto readiness = ReadinessWithRunnerLoss();
    auto &loss = *readiness.program_runner_loss;
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
    if (malformed == 8)
      loss.fault_code = "program_tick_worker_failed";
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
      {1U, {}},
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
