#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_component_manager/manager_liveness_transport.hpp>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/application_identity.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

namespace manyforge::deployment {
class IVerifiedRetainedBundleSource;
class IVerifiedRetainedProductArtifactSource;
} // namespace manyforge::deployment

namespace manyforge::component_manager {

class RetainedExecutionHostUnitAdapterError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Error code of the typed busy answer: a call met the execution host while it
/// is being launched, while its projection bindings are being prepared, while
/// its component processes are being read or while a projection is being
/// rebuilt (B13, OD-122 (16); contract 1.3; OD-270). Details: `phase`
/// (`launching`, `preparing`, `inspecting` or `rebuilding`) and
/// `runtime_unit_id`. The caller retries by its own action.
inline constexpr std::string_view kRuntimeUnitLaunchInProgressCode =
    "runtime_unit_launch_in_progress";

/// Error code of an exchange with the execution host (none has a time limit,
/// OD-270, OD-290 (a)) that ended because the execution host exited. HostExit() is set when it is answered; the Manager
/// then ends the application. Details: `exchange`; `effect: none` when the
/// exit was seen before the request was sent.
inline constexpr std::string_view kExecutionHostExitedCode =
    "execution_host_exited";

/// Error code of an exchange with the execution host that ended because the
/// application is stopping (AbandonExchanges, OD-270). Its outcome is
/// unknown; one that was refused before it was sent carries `effect: none`.
/// Details: `exchange`.
inline constexpr std::string_view kApplicationStoppingCode =
    "application_stopping";

/// Error code of the answer to a call that meets an outstanding exchange on
/// the same channel (OD-290 (a)): the execution host has not answered an
/// earlier request yet, so this one was refused before it was sent
/// (`effect: none`). Nothing is halted and the host does not count as
/// faulted. Details: `exchange` (the outstanding exchange kind),
/// `waited_ms` and `effect`.
inline constexpr std::string_view kExecutionHostLateCode =
    "execution_host_late";

/// The oldest exchange whose reply the execution host has not sent yet, as
/// the Manager's observation tick reads it (OD-290 (a)).
struct ExecutionHostReplyOutstandingDTO final {
  /// The exchange's number; a new value is a new exchange.
  std::uint64_t episode{};
  std::string exchange;
  /// Since the request was sent.
  std::chrono::milliseconds waited{};
};

/// How the execution host child ended, as the Manager observed it.
struct ExecutionHostExitDTO final {
  /// True when `value` is the terminating signal, false when it is the exit
  /// status.
  bool signaled{};
  int value{};
  /// The host child's PID, and whether the kernel reported a core dump
  /// (WCOREDUMP) for its terminating signal.
  pid_t pid{};
  bool core_dumped{};
};

/// Manager-owned local adapter for the retained native execution product.
///
/// The adapter resolves only the contract-fixed installed target beneath one
/// trusted target directory, launches it as a direct child, and communicates
/// over one inherited AF_UNIX packet channel. There is no process discovery,
/// adoption, public socket, executable/argument selection, or caller
/// authorization. A child is staged-ready only after its direct status report
/// matches the verified retained product and reports a closed command surface.
///
/// `activation_root`, when non-empty, is also every child's scene-content
/// root: a duplicate of its descriptor is inherited as FD 9, from whose
/// raw-artifact store scene meshes are read by content digest. Without an
/// activation root no FD 9 is passed and scene meshes fail closed.
///
/// `site_settings_document`, when supplied, is the Manager-validated generic
/// machine site document (`manyforge.site.v2`). It is captured once into a
/// private run-root snapshot that is never rewritten and stays with the run's
/// evidence; children receive only that snapshot path.
class RetainedExecutionHostUnitAdapter final
    : public IRuntimeUnitProcessPort,
      public IRuntimeUnitProgressPort,
      public manyforge::core::IManagerKernelApplyFencePort,
      public manyforge::core::IKernelCellAdmissionPort,
      public manyforge::core::IPlannerSceneProjectionResolver,
      public IManagerSelectedRuntimeCompositionPort,
      public IManagerLivenessTransportPort {
public:
  RetainedExecutionHostUnitAdapter(
      manyforge::core::ApplicationStartId application_start_id,
      std::filesystem::path trusted_installed_target_root,
      std::filesystem::path trusted_package_root,
      std::filesystem::path manager_run_root,
      std::filesystem::path execution_state_root,
      std::filesystem::path execution_run_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system, std::string launch_environment_contract,
      std::filesystem::path activation_root = {},
      std::optional<std::string> site_settings_document = std::nullopt,
      std::function<manyforge::core::ActiveBundlePublicationReceiptDTO()>
          active_publication = {});
  RetainedExecutionHostUnitAdapter(
      manyforge::core::ApplicationStartId application_start_id,
      std::filesystem::path trusted_installed_target_root,
      std::filesystem::path trusted_package_root,
      std::filesystem::path manager_run_root,
      std::filesystem::path execution_state_root,
      std::filesystem::path execution_run_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system,
      std::vector<std::string> launch_environment_contracts,
      std::filesystem::path activation_root = {},
      std::optional<std::string> site_settings_document = std::nullopt,
      // The Manager's current publication; every fence request but Release
      // carries it to the host.
      std::function<manyforge::core::ActiveBundlePublicationReceiptDTO()>
          active_publication = {});
  ~RetainedExecutionHostUnitAdapter() override;

  RetainedExecutionHostUnitAdapter(const RetainedExecutionHostUnitAdapter &) =
      delete;
  RetainedExecutionHostUnitAdapter &
  operator=(const RetainedExecutionHostUnitAdapter &) = delete;
  RetainedExecutionHostUnitAdapter(RetainedExecutionHostUnitAdapter &&) =
      delete;
  RetainedExecutionHostUnitAdapter &
  operator=(RetainedExecutionHostUnitAdapter &&) = delete;

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const manyforge::core::RuntimeEpoch &runtime_epoch,
                  std::uint64_t answering_after) override;
  [[nodiscard]] std::uint64_t ProgressRequestMark() override;
  [[nodiscard]] manyforge::core::ConfigurationFenceAdmissionSnapshotDTO
  ObserveAdmission(const manyforge::core::ConfigurationFenceAdmissionQueryDTO
                       &query) const override;
  [[nodiscard]] manyforge::core::ConfigurationFenceAcceptedDTO Acquire(
      const manyforge::core::ApplyConfigurationFenceIntentDTO &intent) override;
  [[nodiscard]]
  manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::ConfigurationFenceStatusDTO
  QueryStatus(const manyforge::core::ConfigurationFenceStatusQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::ConfigurationFenceReleasedDTO Release(
      const manyforge::core::ConfigurationFenceReleaseDTO &request) override;
  [[nodiscard]] manyforge::core::AuthoringAdmissionDTO
  GetAuthoringAdmission() const override;
  [[nodiscard]] manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const manyforge::core::CellAdmissionReadinessSnapshotDTO &snapshot,
      std::shared_ptr<manyforge::core::IIoSignalService>* actual_retired_service = nullptr)
      override;
  [[nodiscard]] manyforge::core::Result<void> Publish(
      const manyforge::core::ManagerLivenessObservationDTO &observation)
      override;
  [[nodiscard]]
  manyforge::core::Result<manyforge::core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const manyforge::core::CellAdmissionClaimIntentDTO &intent) override;
  [[nodiscard]]
  manyforge::core::Result<manyforge::core::CellAdmissionClaimDTO>
  AcquireManagerApplyClaim(
      const manyforge::core::CellAdmissionClaimIntentDTO &intent,
      const manyforge::core::RetainedBundleId &candidate_retained_bundle_id) override;
  [[nodiscard]] manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
  ReleaseCellAdmissionClaim(
      const manyforge::core::CellAdmissionClaimReleaseDTO &release) override;
  [[nodiscard]] manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
  ReleaseManagerApplyClaim(
      const manyforge::core::CellAdmissionClaimReleaseDTO &release,
      const manyforge::core::ManagerApplyReleaseOutcomeDTO &outcome) override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const manyforge::core::CellAdmissionRecoveryReceiptQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::Result<void>
  ValidateProgramResetSceneAuthorization(
      const manyforge::core::ProgramResetSceneAuthorizationDTO &authorization)
      const override;
  [[nodiscard]] manyforge::core::Result<
      std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
  ResolveProjection(const manyforge::core::ProjectionExecutionHostBindingDTO
                        &execution_host_binding) const override;
  [[nodiscard]] manyforge::core::Result<
      ManagerComponentProcessInventoryResultDTO>
  InspectInstalledComponentProcesses(
      const ManagerComponentProcessInventoryRequestDTO &request) override;
  [[nodiscard]] manyforge::core::Result<HostApplyResult>
  ApplyOnHost(const manyforge::core::RuntimeUnitId &runtime_unit_id,
              const HostApplyRequest &request) override;
  [[nodiscard]] manyforge::core::ManagerLivenessObservationDTO
  RenewManagerLiveness(
      const ManagerLivenessTransportRequestDTO& request) override;
  [[nodiscard]]
  std::vector<manyforge::core::ManagerLivenessObservationDTO>
  RenewManagerLivenessBatch(
      const std::vector<ManagerLivenessTransportRequestDTO>& requests)
      override;

  /// The execution host's exit, once any observation or call has seen it.
  /// Lock-free: read by the Manager's observation tick, which ends the
  /// application on it (OD-122 (1)).
  [[nodiscard]] std::optional<ExecutionHostExitDTO> HostExit() const noexcept;

  /// The oldest management exchange whose reply has not arrived yet
  /// (OD-290 (a)); none when every reply is in. A failed management channel
  /// with the host alive keeps its exchange outstanding until the host is
  /// stopped. Information only: the Manager's observation tick logs an
  /// exchange still outstanding at its next tick; nothing is halted or
  /// faulted. Settles replies that arrived with non-blocking reads under the
  /// adapter's leaf locks; never waits on the host. A failed observation is
  /// logged once per episode, with no lock held. Called from one thread (the
  /// Manager's main thread).
  [[nodiscard]] std::optional<ExecutionHostReplyOutstandingDTO>
  HostReplyOutstanding() const noexcept;

  /// OD-187 (1): `force_requested` answers whether a second stop request
  /// came during the Manager's stop sequence. While the host's stop waits
  /// for its exit and it answers true, the host is ended with SIGKILL. Set
  /// once by the Manager before any stop; read lock-free; memory reads only.
  void SetShutdownForce(bool (*force_requested)() noexcept) noexcept;

  /// OD-270: the application is stopping. Every exchange that waits for the
  /// host (none has a time limit, OD-290 (a)) now ends with the typed
  /// `application_stopping` answer, and later ones are refused before they
  /// are sent; a waiting exchange stays the channel's pending one, so its
  /// late reply is still recognised and settled. Called once by the Manager
  /// first in its stop sequence; one eventfd write, lock-free.
  void AbandonExchanges() noexcept;

  /// The eventfd AbandonExchanges writes, open for the adapter's lifetime.
  /// The Manager's stop signal handler writes it directly (one write(2),
  /// async-signal-safe), so a stop request ends a host wait on any thread,
  /// the waiting one included.
  [[nodiscard]] int AbandonSignalFd() const noexcept;

  /// True while the adapter holds an execution host child whose exit it has
  /// not observed, a host being stopped included (OD-308). Memory reads
  /// under the adapter's lock only.
  [[nodiscard]] bool HasHostChild() const;

  /// The PID of the host child ended by SIGKILL for a second stop request,
  /// once that happened; the Manager logs the kill. Lock-free.
  [[nodiscard]] std::optional<pid_t> HostForceKilledPid() const noexcept;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace manyforge::component_manager
