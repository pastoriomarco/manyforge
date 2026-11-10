#pragma once

#include "execution_event_stream.hpp"
#include "execution_host_commands.hpp"
#include "execution_host_posix.hpp"
#include "ros_host_composition.hpp"
#include "manager_liveness_rpc_internal.hpp"
#include <sys/eventfd.h>
#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string_view>

namespace manyforge::roboplan_backend::host_detail {

class ExecutionCommandSurface;

struct ManagedExecutionSurfaceControl final {
  std::filesystem::path run_root;
  ExecutionCompositionAccess *composition_access{};
  component_manager::detail::ManagerLivenessCommandAdmission
      *manager_liveness_admission{};
  core::RequestOwnerEpoch owner_epoch;
  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool ready{};
  std::exception_ptr failure;
  // The running surface, guarded by `state_mutex`; null before it is ready
  // and once it has stopped.
  ExecutionCommandSurface *surface{};

  ManagedExecutionSurfaceControl(
      std::filesystem::path selected_run_root,
      ExecutionCompositionAccess *selected_composition_access,
      component_manager::detail::ManagerLivenessCommandAdmission
          *selected_manager_liveness_admission,
      core::RequestOwnerEpoch selected_owner_epoch);
};

void RunManagedExecutionCommandSurface(ManagedExecutionSurfaceControl &control,
                                       std::stop_token stop_token) noexcept;

/// Preempts the running surface's advisory planning; a no-op while no
/// surface runs.
void PreemptManagedExecutionSurfacePlanning(
    ManagedExecutionSurfaceControl &control) noexcept;


class ExecutionCommandSurface final {
  // The Manager channel's component-lifecycle claim preempts planning as
  // the ordinary socket's does.
  friend void PreemptManagedExecutionSurfacePlanning(
      ManagedExecutionSurfaceControl &control) noexcept;

public:
  ExecutionCommandSurface(
      std::filesystem::path run_root,
      ExecutionCompositionAccess &composition_access,
      component_manager::detail::ManagerLivenessCommandAdmission
          &manager_liveness_admission,
      core::RequestOwnerEpoch owner_epoch);

  int Run(const std::stop_token stop_token = {});

private:
  /// `response_capacity` is the client's AtomicResponseCapacity; it bounds an
  /// inline trace, never a disk snapshot.
  [[nodiscard]] CanonicalValue
  ReadVolatileMotionTrace(const Command &command,
                          std::size_t response_capacity) const;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  [[nodiscard]] CanonicalValue
  Ros2ControlTransportDiagnosticValue(Composition &composition) const;
#endif

  [[nodiscard]] CanonicalValue
  DirectControlTargetValue(Composition &composition) const;

  struct ResolvedObservationOwner final {
    Composition *composition;
    std::shared_ptr<RetainedObservedStateChannel> channel;
    core::RuntimeParticipantBinding physical;
    ResolvedTargetObservation resolved;
  };

  [[nodiscard]] std::optional<ResolvedObservationOwner>
  ResolveObservationOwner(const core::ProgramBindingId &program_binding_id,
                          const core::ProgramTargetReferenceDTO &target,
                          const core::CapabilityId &capability_id,
                          const bool motion, Composition &composition,
                          std::map<std::string, std::string> *failures = nullptr) const;

  [[nodiscard]] Composition *
  FindBindingComposition(const core::RuntimeParticipantBindingDTO &binding,
                         Composition &composition) const;

  [[nodiscard]] core::Result<core::GripperCommandStatusDTO>
  ReadCurrentGripperStatus(
      Composition &component,
      const core::RuntimeParticipantBinding &observation_binding) const;

  [[nodiscard]] CanonicalValue
  FrameTransformResponse(const TargetObservationQuery &query,
                         Composition &composition) const;

  [[nodiscard]] CanonicalValue IoSignalQueryResponse(
      const TargetObservationQuery& query, Composition& composition) const;
  [[nodiscard]] core::Result<core::IoSignalObservationDTO> ReadBoundIoSignal(
      const TargetObservationQuery& query, Composition& composition) const;
  [[nodiscard]] CanonicalValue IoSignalResponse(
      const TargetObservationQuery& query,
      const core::IoSignalObservationDTO& observation, std::string_view command) const;
  [[nodiscard]] CanonicalValue MockIoInputResponse(
      const MockIoInputSetRequest& request, Composition& composition) const;

  [[nodiscard]] CanonicalValue MockRunPauseResponse(
      const core::ProgramRunMockSimulationPauseRequestDTO& request,
      core::ResourceAddressedExecutionKernel& owner) const;
  [[nodiscard]] CanonicalValue MockNodeSimulationResponse(
      const core::ProgramNodeMockSimulationScopeDTO& requested_scope,
      const std::vector<core::ProgramMockSimulationSignalDTO>* requested_signals,
      ExecutionProduct& product, core::ResourceAddressedExecutionKernel& owner) const;

  [[nodiscard]] CanonicalValue
  IoWatchResponse(const Command &command,
                  core::ResourceAddressedExecutionKernel &owner) const;
  [[nodiscard]] CanonicalValue ProgramBlackboardResponse(
      const core::IoProgramBlackboardPublicationDTO &publication,
      core::ResourceAddressedExecutionKernel &owner) const;

  [[nodiscard]] CanonicalValue
  QueryTargetResponse(const TargetObservationQuery &query,
                      Composition &composition) const;

  [[nodiscard]] CanonicalValue
  QueryOperationResponse(const ProgramOperationQuery &query,
                         Composition &composition) const;

  [[nodiscard]] CanonicalValue DirectControlObservedValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO &observed_state) const;

  [[nodiscard]] CanonicalValue DirectControlStatusValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO &observed_state) const;

  [[nodiscard]] CanonicalValue
  DirectControlGripperTargetValue(Composition &composition) const;

  [[nodiscard]] CanonicalValue DirectControlGripperObservedValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO *observed_state) const;

  [[nodiscard]] CanonicalValue DirectControlGripperStatusValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO *observed_state) const;

  struct CurrentControlState final {
    std::optional<core::ObservedStateSnapshotDTO> motion;
    std::optional<core::ObservedStateSnapshotDTO> gripper;
  };

  [[nodiscard]] CurrentControlState
  ReadCurrentControlState(Composition &composition) const;

  [[nodiscard]] CanonicalValue::Object
  IdentityFields(ExecutionProduct &composition,
                 const core::ProjectionRevisionDTO &projection) const;

  [[nodiscard]] CanonicalValue
  IdentityValue(ExecutionProduct &composition,
                const core::ProjectionRevisionDTO &projection) const;

  [[nodiscard]] std::optional<CanonicalValue>
  ExecuteProgramCommand(const Command &command, ExecutionProduct &product,
                        core::ResourceAddressedExecutionKernel *owner);

  [[nodiscard]] CanonicalValue
  ExecuteLogicalProduct(const Command &command, ExecutionProduct &product);

  [[nodiscard]] CanonicalValue
  StatusResponse(Composition &composition,
                 const core::ProjectionRevisionDTO &projection) const;

  [[nodiscard]] CanonicalValue
  IdentityResponse(ExecutionProduct &composition,
                   const core::ProjectionRevisionDTO &projection) const;

  [[nodiscard]] CanonicalValue ResetRobotPositionCapabilityResponse(
      Composition &composition, const core::ProjectionRevisionDTO &projection) const;

  [[nodiscard]] CanonicalValue ResetRobotPositionResponse(
      Composition &composition, const ProgramResetPositionRequest &request,
      std::stop_token stop_token) const;

  [[nodiscard]] CanonicalValue
  TargetsResponse(Composition &composition,
                  const core::ProjectionRevisionDTO &projection) const;

  [[nodiscard]] CanonicalValue
  ControlStateResponse(Composition &composition,
                       const core::ProjectionRevisionDTO &projection) const;

  struct ObservationProjection final {
    CanonicalValue response;
    std::shared_ptr<IRetainedObservedStateChannel> channel;
    std::string source_epoch;
    std::uint64_t source_sequence{};
    std::uint64_t change_generation{};
  };

  [[nodiscard]] ObservationProjection
  BuildObservationProjection(Composition &composition) const;

  [[nodiscard]] CanonicalValue
  ObservationResponse(Composition &composition) const;

  [[nodiscard]] CanonicalValue
  ObservationResponseAfter(const Command &command, const int client_fd,
                           const std::stop_token stop_token) const;

  [[nodiscard]] CanonicalValue
  ExecuteStop(const Command &command,
              core::ResourceAddressedExecutionKernel &addressed_kernel);

  [[nodiscard]] CanonicalValue
  ReadStop(const Command &command,
           core::ResourceAddressedExecutionKernel &addressed_kernel) const;

  /// `read_component_model` (control socket):
  /// `component_states`, `cell_latch`, `signal_samples`, `rule_holds` and
  /// `required_signal_ids` (the inputs the Applied Program reads).
  [[nodiscard]] CanonicalValue ReadComponentModelResponse(
      core::ResourceAddressedExecutionKernel &addressed_kernel) const;
  /// Starts one component action (`component_action`).
  [[nodiscard]] CanonicalValue ComponentActionResponse(const Command &command) const;
  [[nodiscard]] CanonicalValue ComponentRecoveryPreflightResponse(const Command &command) const;
  /// `set_io_output`: the Kernel admits the write
  /// (`cell_latched`, `rule_holds`), then the component's adapter writes it.
  /// A refusal is an error reply; the write's outcome is `result`.
  [[nodiscard]] CanonicalValue SetIoOutputResponse(
      const Command &command, core::ResourceAddressedExecutionKernel &addressed_kernel) const;
  /// Who reads the component model fields.
  enum class ComponentModelRead {
    /// `read_component_model` (control socket): every field, samples with
    /// their age; never waits for an Apply holding the composition.
    kControl,
    /// The `events` reply and the event stream's `state`: the signal samples
    /// without their age, so only a value, source or freshness change makes a
    /// new state.
    kState,
  };
  [[nodiscard]] CanonicalValue::Object ComponentModelFields(
      core::ResourceAddressedExecutionKernel &addressed_kernel, ComponentModelRead read) const;

  [[nodiscard]] CanonicalValue EventsResponse(const Command &command) const;

  /// Active TCP: each robot's `{component_id, entry | null,
  /// owner_epoch, registration_generation, sequence}`, read under its
  /// owner's lock; a reader keeps the greatest triple per robot from
  /// results, reads and `active_tcp_changed` rows.
  [[nodiscard]] CanonicalValue ActiveTcpBaselineValue() const;
  [[nodiscard]] static CanonicalValue ActiveTcpBaselineValue(
      Composition &composition, const core::RequestOwnerEpoch &owner_epoch);
  /// `read_active_tcp` / `reset_active_tcp` (every robot; a reset always
  /// completes) replies.
  [[nodiscard]] CanonicalValue ActiveTcpResponse(Composition *composition,
                                                bool reset) const;
  /// Robot control's `active_tcp: {entry, display} | null`.
  [[nodiscard]] CanonicalValue ActiveTcpStatusValue(Composition &composition) const;

  /// Event stream reads: the `events` reply facts without
  /// page and progress, and one page of the same journal read.
  [[nodiscard]] std::pair<std::string, CanonicalValue> EventStreamState() const;
  [[nodiscard]] CanonicalValue EventStreamPage(const std::string &owner_epoch,
                                               std::uint64_t after) const;
  /// The executing direct-control operations' progress rows, as in the
  /// `events` reply's `operation_progress`.
  [[nodiscard]] CanonicalValue EventStreamProgress() const;
  [[nodiscard]] CanonicalValue
  OperationProgressValue(core::ResourceAddressedExecutionKernel &owner,
                         const core::RequestOwnerEpoch &owner_epoch) const;

  [[nodiscard]] CanonicalValue
  Execute(const Command &command, Composition &composition,
          const core::ProjectionRevisionDTO &projection,
          const std::stop_token planning_stop_token);

  [[nodiscard]] std::optional<core::ErrorDTO> LastReconcileError() const;

  [[nodiscard]] core::Result<void>
  ReconcileDirectControlOperations(Composition &composition) const;

  void RunReconciliationLane(std::stop_token stop_token) noexcept;

  /// Socket lanes only wait for clients; they need no periodic wakeup.
  /// Every reason to leave a lane (its stop request, a failed lane, a process
  /// stop signal) makes one of these descriptors readable.
  [[nodiscard]] int WaitForSocket(const int socket, short &revents) const;

  /// Ends every socket-lane wait. The wake stays readable: it is raised only
  /// when this surface is stopping.
  void WakeLanes() noexcept;

  [[nodiscard]] bool LanesWoken() const noexcept;

  void RunControlLane(std::stop_token stop_token) noexcept;

  void RunObservationLane(std::stop_token stop_token) noexcept;

  void HandleOneClient(const CommandSocket &socket, const CommandLane lane,
                       const std::stop_token stop_token = {}) noexcept;

  void SendResponse(
      const int client, const CanonicalValue &response,
      const std::optional<std::string> &cell_admission_command) const;

  /// Lane and ordering contract of the execution sockets.
  ///
  /// Every ordinary and control connection carries one request and its
  /// response, so a client that needs A before B already waits for A's
  /// response; requests on different connections or sockets are ordered by
  /// nothing but the owner fences below, never by arrival.
  ///
  /// - Control socket: Stop, Program Stop, Program-selection Stop, full and
  ///   malformed Stop, plus the in-memory reads Stop progression needs
  ///   (Stop status, component states and the cell latch) and the
  ///   operator-alarm attention heartbeat.
  ///   It reaches only the stable Kernel, without the composition lock, so
  ///   neither planning nor an Apply delays it. Each request is a bounded
  ///   copy under the Kernel's state lock.
  /// - Ordinary socket: operation controls (Reset,
  ///   acknowledgement), events, reads, Program selection and capture, cell
  ///   admission and recovery. It never waits for planning, submission
  ///   ordering or a Manager round trip; those go to deferred lanes.
  /// - Submission-ordered lane: requests whose Kernel entry takes submission
  ///   ordering (IsSubmissionOrdered). One worker, FIFO in accept order, one
  ///   in flight, as the single ordinary lane executed them before.
  /// - Scene-read lane: projections that first read the live Scene revision
  ///   from Manager (ReadsManagerScene). One worker, FIFO.
  ///
  /// Cross-lane fences: Reset, recovery and Stop-fence release name the
  /// exact operation revision, or Stop id and generation they act on, and their clients issue them only after reading that state
  /// on the control socket. A stale name is refused (the Program owner also
  /// refuses a cycle Reset while its Stop is not terminal); a Kernel Reset of
  /// a participant still stopping hard-stops it first, so no ordering of the
  /// two sockets lets Reset restart motion or release a newer Stop. Alarm
  /// attention is fenced by owner epoch, incarnation and monotonic generation
  /// and evaluated at admission only by age, so admission on another lane
  /// observes either the previous or the new publication, both valid. Event
  /// pages are journal-sequence ordered.
  [[nodiscard]] core::ResourceAddressedExecutionKernel &StableKernel() const;

  [[nodiscard]] CanonicalValue
  OwnerNotReadyResponse(const Command &command) const;

  struct PendingDeferredCommand final {
    FileDescriptor client;
    Command command;
    std::stop_source cancellation;
  };

  struct DeferredLane final {
    explicit DeferredLane(const std::string_view busy) : busy_code(busy) {}
    const std::string_view busy_code;
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<PendingDeferredCommand> queue;
    std::optional<std::stop_source> in_flight;
  };

  [[nodiscard]] bool EnqueueDeferred(DeferredLane &lane, FileDescriptor &client,
                                     Command command);

  /// Cancels every advisory planning query already accepted, executing or
  /// queued; a queued one then reaches the Kernel already cancelled. Only
  /// planning consults the token: a queued submit or read is unaffected.
  /// Called before a Stop or a component-lifecycle cell-admission claim is
  /// dispatched and after a Reset is accepted; none waits for the cancelled
  /// planning to return. (The Kernel's own
  /// Reset of a participant additionally preempts that participant's preview
  /// before taking its lifecycle lock.)
  void PreemptPlanning() noexcept;

  [[nodiscard]] CanonicalValue
  DeferredResponse(const Command &command, const std::stop_token cancellation,
                   const std::stop_token lane_stop_token);

  void RunDeferredLane(DeferredLane &lane,
                       const std::stop_token stop_token) noexcept;

  core::RequestOwnerEpoch owner_epoch_;
  core::ProgramRunId direct_control_program_run_id_;
  core::ProgramRunId direct_control_gripper_program_run_id_;
  ExecutionCompositionAccess &composition_access_;
  component_manager::detail::ManagerLivenessCommandAdmission
      &manager_liveness_admission_;
  HostLock host_lock_;
  CommandSocket command_socket_;
  CommandSocket control_socket_;
  CommandSocket observation_socket_;
  // Each command lane is serviced by exactly one thread.
  std::array<bool, 3> accept_failure_logged_{};
  FileDescriptor lane_wake_{::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)};
  std::atomic<bool> control_socket_failed_{false};
  std::atomic<bool> reconciliation_lane_failed_{false};
  mutable std::mutex reconcile_error_mutex_;
  std::optional<core::ErrorDTO> last_reconcile_error_;
  std::exception_ptr reconciliation_lane_failure_;
  DeferredLane submission_ordered_lane_{"execution_submission_lane_busy"};
  DeferredLane scene_read_lane_{"execution_scene_read_lane_busy"};
  // Event subscriptions: own socket, acceptor and one writer per subscriber.
  CommandSocket event_stream_socket_;
  ExecutionEventStream event_stream_{ExecutionEventStreamSource{
      [this] { return EventStreamState(); },
      [this](const std::string &owner_epoch, const std::uint64_t after) {
        return EventStreamPage(owner_epoch, after);
      },
      [this] { return EventStreamProgress(); }}};
};
} // namespace manyforge::roboplan_backend::host_detail
