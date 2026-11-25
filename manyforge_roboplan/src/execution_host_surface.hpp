#pragma once

#include "execution_event_stream.hpp"
#include "execution_host_commands.hpp"
#include "execution_host_planning_jobs.hpp"
#include "execution_host_posix.hpp"
#include "ros_host_composition.hpp"
#include "manager_liveness_rpc_internal.hpp"
#include <poll.h>
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
#include <vector>

namespace manyforge::roboplan_backend::host_detail {

class ExecutionCommandSurface;

struct ManagedExecutionSurfaceControl final {
  std::filesystem::path run_root;
  ExecutionCompositionAccess *composition_access{};
  component_manager::detail::ManagerLivenessCommandAdmission
      *manager_liveness_admission{};
  core::ApplicationStartId application_start_id;
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
      core::ApplicationStartId selected_application_start_id);
};

void RunManagedExecutionCommandSurface(ManagedExecutionSurfaceControl &control,
                                       std::stop_token stop_token) noexcept;

/// Preempts the running surface's advisory planning and planning jobs for a
/// Program Reset claim, and releases the planning jobs of the run it ends; a
/// no-op while no surface runs.
void PreemptManagedExecutionSurfacePlanning(
    ManagedExecutionSurfaceControl &control,
    const component_manager::detail::KernelCellAdmissionRpcPacket
        &packet) noexcept;


class ExecutionCommandSurface final {
  // The Manager channel's component-lifecycle claim preempts planning as
  // the ordinary socket's does.
  friend void PreemptManagedExecutionSurfacePlanning(
      ManagedExecutionSurfaceControl &control,
      const component_manager::detail::KernelCellAdmissionRpcPacket
          &packet) noexcept;

public:
  ExecutionCommandSurface(
      std::filesystem::path run_root,
      ExecutionCompositionAccess &composition_access,
      component_manager::detail::ManagerLivenessCommandAdmission
          &manager_liveness_admission,
      core::ApplicationStartId application_start_id);

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

  /// `read_component_model` (component socket):
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
    /// `read_component_model` (component socket): every field, samples with
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
  /// registration_generation, sequence}`, read under its owner's lock; a
  /// reader keeps the greatest pair per robot from results, reads and
  /// `active_tcp_changed` rows.
  [[nodiscard]] CanonicalValue ActiveTcpBaselineValue() const;
  [[nodiscard]] static CanonicalValue ActiveTcpBaselineValue(Composition &composition);
  /// `read_active_tcp` / `reset_active_tcp` (every robot; a reset always
  /// completes) replies.
  [[nodiscard]] CanonicalValue ActiveTcpResponse(Composition *composition,
                                                bool reset) const;
  /// Robot control's `active_tcp: {entry, display} | null`.
  [[nodiscard]] CanonicalValue ActiveTcpStatusValue(Composition &composition) const;

  /// Event stream reads: the `events` reply facts without
  /// page and progress, and one page of the same journal read.
  [[nodiscard]] CanonicalValue EventStreamState() const;
  [[nodiscard]] CanonicalValue EventStreamPage(std::uint64_t after) const;
  /// The executing direct-control operations' progress rows, as in the
  /// `events` reply's `operation_progress`.
  [[nodiscard]] CanonicalValue EventStreamProgress() const;
  [[nodiscard]] CanonicalValue
  OperationProgressValue(core::ResourceAddressedExecutionKernel &owner) const;

  [[nodiscard]] CanonicalValue
  Execute(const Command &command, Composition &composition,
          const core::ProjectionRevisionDTO &projection,
          const std::stop_token planning_stop_token);

  [[nodiscard]] std::optional<core::ErrorDTO> LastReconcileError() const;

  [[nodiscard]] core::Result<void>
  ReconcileDirectControlOperations(Composition &composition) const;

  void RunReconciliationLane(std::stop_token stop_token) noexcept;

  /// A socket lane's accepted connections that have not yet sent their
  /// (next) request, in serving order, and the poll set built from them.
  /// Owned by the lane's one thread; no time limit applies to them.
  struct LaneConnections final {
    LaneConnections();
    std::vector<FileDescriptor> waiting;
    std::vector<pollfd> descriptors;
  };

  /// Socket lanes only wait for clients; they need no periodic wakeup.
  /// Every reason to leave a lane (its stop request, a failed lane, a process
  /// stop signal) makes one of these descriptors readable. Waits on the
  /// listening `socket` and on every waiting connection of `lane`;
  /// `lane.descriptors[0]` is the listening socket's.
  [[nodiscard]] int WaitForLane(const int socket, LaneConnections &lane) const;

  /// Ends every socket-lane wait. The wake stays readable: it is raised only
  /// when this surface is stopping.
  void WakeLanes() noexcept;

  [[nodiscard]] bool LanesWoken() const noexcept;

  void RunControlLane(std::stop_token stop_token) noexcept;

  void RunObservationLane(std::stop_token stop_token) noexcept;

  void RunComponentLane(std::stop_token stop_token) noexcept;

  /// After a WaitForLane: drops the waiting connections that hung up,
  /// serves the oldest one whose request has arrived, then accepts one new
  /// connection if the listening socket is readable. One request at a time.
  void ServeLaneTurn(const CommandSocket &socket, const CommandLane lane,
                     LaneConnections &connections,
                     const std::stop_token stop_token = {}) noexcept;

  /// Accepts one connection of this process's user onto `waiting`.
  void AcceptClient(const CommandSocket &socket, const CommandLane lane,
                    std::vector<FileDescriptor> &waiting) noexcept;

  /// Receives, handles or hands off, and answers one request of `client`.
  /// True when the connection stays open for its next request (the
  /// observation lane after its reply).
  [[nodiscard]] bool ServeRequest(FileDescriptor &client, const CommandLane lane,
                                  const std::stop_token stop_token) noexcept;

  void SendResponse(
      const int client, const CanonicalValue &response,
      const std::optional<std::string> &cell_admission_command) const;

  /// Lane and ordering contract of the execution sockets.
  ///
  /// Every ordinary, control and component connection carries one request and
  /// its response, so a client that needs A before B already waits for A's
  /// response; requests on different connections or sockets are ordered by
  /// nothing but the owner fences below, never by arrival. Each socket's
  /// thread handles one request at a time, from whichever connection has
  /// sent one (oldest connection first); a connection that has not sent its
  /// request yet waits without a time limit and holds up no other.
  ///
  /// - Control socket: Stop, Program Stop, Program-selection Stop, full and
  ///   malformed Stop, plus Stop status, the carried-object-lost level, the
  ///   read, cancel and release of one planning job and the speed hand-off to
  ///   the runtime-scale lane. It never takes the
  ///   composition lock, so neither planning, an Apply nor a component
  ///   request delays it. Each request is a bounded in-memory step.
  /// - Component socket: component actions (plain and recover-enable), their
  ///   recovery preflight, the component model and the operator-alarm
  ///   attention heartbeat. Its own thread, one request at a time; it
  ///   takes the composition only by try_lock, so an Apply never delays it.
  /// - Ordinary socket: operation controls (Reset,
  ///   acknowledgement), events, reads, Program selection and capture, cell
  ///   admission and recovery. It never waits for planning, submission
  ///   ordering, a Manager round trip or an output write; those go to
  ///   deferred lanes.
  /// - Submission-ordered lane: requests whose Kernel entry takes submission
  ///   ordering (IsSubmissionOrdered), pick/place planning jobs among them:
  ///   their client is answered at accept, the outcome is read from the job
  ///   table (PlanningJobTable) on the control socket. One worker,
  ///   FIFO in accept order, one in flight, as the single ordinary lane
  ///   executed them before.
  /// - Scene-read lane: projections that first read the live Scene revision
  ///   from Manager (ReadsManagerScene). One worker, FIFO.
  /// - Output lane: `set_io_output`, manual and Program alike. One worker,
  ///   FIFO in accept order, one in flight; the Kernel admits each write
  ///   when it starts, and a started write completes.
  ///
  /// Cross-lane fences: Reset and recovery name the exact operation
  /// revision, or Stop id and generation they act on, and their clients issue
  /// them only after reading that state
  /// on the control or component socket. A stale name is refused (the Program
  /// owner also refuses a cycle Reset while its Stop is not terminal); a
  /// Kernel Reset of a participant still stopping hard-stops it first, so no
  /// ordering of the sockets lets Reset restart motion or release a newer Stop. Alarm
  /// attention is fenced by host boot and monotonic generation
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
  /// planning consults the token: a queued submit or read is unaffected, and
  /// a pick/place planning job plans with its own source (PreemptPlanningJobs).
  /// Called before a Stop or a component-lifecycle cell-admission claim is
  /// dispatched and after a Reset is accepted; none waits for the cancelled
  /// planning to return. (The Kernel's own
  /// Reset of a participant additionally preempts that participant's preview
  /// before taking its lifecycle lock.)
  void PreemptPlanning() noexcept;

  /// Pick/place planning jobs (OD-213 (1)). A job is accepted at once and
  /// planned on the submission-ordered lane; read, cancel and release are
  /// served on the control socket, memory only (on the ordinary socket a
  /// value publication waiting for the submission lock can hold them for a
  /// whole plan). A Stop never reaches a job; a Program Reset claim, a reset-position
  /// move and an accepted Reset preempt every job (their own reasons, as for
  /// PreemptPlanning), and a Program Reset claim releases its run's jobs.
  [[nodiscard]] CanonicalValue AcceptPlanningJob(const Command &command,
                                                 Composition &composition);
  [[nodiscard]] CanonicalValue PlanningJobResponse(const Command &command);
  void RunPlanningJob(const Command &command,
                      const std::stop_token lane_stop_token) noexcept;
  void PreemptPlanningJobs(
      const component_manager::detail::KernelCellAdmissionRpcPacket
          &packet) noexcept;

  [[nodiscard]] CanonicalValue
  DeferredResponse(const Command &command, const std::stop_token cancellation,
                   const std::stop_token lane_stop_token);

  void RunDeferredLane(DeferredLane &lane,
                       const std::stop_token stop_token) noexcept;

  /// `A` (contract 1.1) and its 16-hex text, the one form on every wire.
  core::ApplicationStartId application_start_id_;
  std::string start_id_text_;
  ExecutionCompositionAccess &composition_access_;
  component_manager::detail::ManagerLivenessCommandAdmission
      &manager_liveness_admission_;
  HostLock host_lock_;
  CommandSocket command_socket_;
  CommandSocket control_socket_;
  CommandSocket observation_socket_;
  CommandSocket component_socket_;
  // Each command lane is serviced by exactly one thread.
  std::array<bool, 4> accept_failure_logged_{};
  FileDescriptor lane_wake_{::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)};
  std::atomic<bool> control_socket_failed_{false};
  std::atomic<bool> component_socket_failed_{false};
  std::atomic<bool> reconciliation_lane_failed_{false};
  mutable std::mutex reconcile_error_mutex_;
  std::optional<core::ErrorDTO> last_reconcile_error_;
  std::exception_ptr reconciliation_lane_failure_;
  DeferredLane submission_ordered_lane_{"execution_submission_lane_busy"};
  PlanningJobTable planning_jobs_;
  DeferredLane scene_read_lane_{"execution_scene_read_lane_busy"};
  DeferredLane runtime_scale_lane_{"execution_runtime_scale_lane_busy"};
  DeferredLane io_output_lane_{"execution_io_output_lane_busy"};
  // Motion-trace snapshot encoding and file writes; its own thread.
  DeferredLane motion_trace_lane_{"motion_trace_store_busy"};
  // Event subscriptions: own socket, acceptor and one writer per subscriber.
  CommandSocket event_stream_socket_;
  ExecutionEventStream event_stream_{ExecutionEventStreamSource{
      [this] { return EventStreamState(); },
      [this](const std::uint64_t after) { return EventStreamPage(after); },
      [this] { return EventStreamProgress(); }}};
};
} // namespace manyforge::roboplan_backend::host_detail
