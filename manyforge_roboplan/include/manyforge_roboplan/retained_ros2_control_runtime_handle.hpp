#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/configured_robot_measured_state.hpp>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <manyforge_core/types/tcp_entry.hpp>
#include <manyforge_core/types/tool_context.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>
#include <manyforge_roboplan/active_tcp.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/retained_ros2_control_statistics_monitor.hpp>
#include <manyforge_roboplan/ros2_control_component_state.hpp>
#include <manyforge_roboplan/tool_context_hook.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

namespace manyforge::ros2_control_execution {
class SelectedRosEndpointFacts;
}

namespace manyforge::roboplan_backend {

namespace host_detail {
struct Composition;
struct ExecutionCompositionAccess;
core::Result<void> ResetCompositionActiveTcps(Composition &, ExecutionCompositionAccess &);
}

class RetainedPlannerSceneProjection;
struct DisplayJointSource;
struct RetainedPreparedRobotMeasuredSources;

/// The outcome of one controller-manager step of the lane (a switch, a
/// hardware-state request): `kUncertain` when its effect is unknown (never
/// resent).
enum class RetainedRos2ControlSwitchOutcome {
  kNotSent,
  kSucceeded,
  kFailed,
  kFailedNoEffect,
  kFailedEffectObserved,
  kUncertain,
};

struct RetainedRos2ControlSwitchResult final {
  RetainedRos2ControlSwitchOutcome outcome{RetainedRos2ControlSwitchOutcome::kNotSent};
  std::optional<core::ErrorDTO> error{};
};

/// Controller-manager identity of the arm's trajectory controller (lane
/// profile facts).
struct RetainedRos2ControlArmController final {
  /// Absolute controller-manager node name: its `switch_controller` service
  /// and `~/activity` topic.
  std::string controller_manager;
  /// The trajectory controller's name inside that controller manager.
  std::string controller_name;
  /// The ros2_control hardware component whose interfaces it claims.
  std::string hardware_component_name;

  friend bool operator==(const RetainedRos2ControlArmController &,
                         const RetainedRos2ControlArmController &) = default;
};

/// The arm controller as the controller manager last reported it on its own
/// transient-local `~/activity` topic. No list service is queried.
struct RetainedRos2ControlArmActivity final {
  /// A controller manager is matched on the activity topic.
  bool controller_manager_present{false};
  /// At least one activity sample was received from it.
  bool observed{false};
  bool controller_loaded{false};
  bool controller_active{false};
  bool hardware_active{false};
  /// The joint state broadcaster (`kJointStateBroadcasterControllerName`) is
  /// loaded (inactive or active), and active.
  bool broadcaster_loaded{false};
  bool broadcaster_active{false};

  friend bool operator==(const RetainedRos2ControlArmActivity &,
                         const RetainedRos2ControlArmActivity &) = default;
};

/// The joint state broadcaster of every generated controllers file. A vendor
/// driver that deactivates all controllers while its robot is not ready
/// deactivates it too; ManyForge never does. `SwitchArmHardware`
/// switches it back on with the hardware when the driver's reactivation did
/// not.
inline constexpr std::string_view kJointStateBroadcasterControllerName =
    "joint_state_broadcaster";

enum class MockResetPositionPhase {
  kSwitchingToWriter,
  kSettingState,
  kSwitchingToJtc,
  kContaining,
};

/// Mock has no stock writer service ACK. One fresh exact GenericSystem
/// loopback sample acknowledges consumption, without a convergence/dwell wait.
/// The fault bound is the stock ros2_control 6.9.0 CM switch default
/// (controller_manager.cpp:2366-2369), not a state freshness budget.
inline constexpr core::Duration kMockResetPositionWriteAcknowledgmentTimeoutNs =
    1'000'000'000LL;

struct MockResetPositionTransition final {
  MockResetPositionPhase phase;
  core::ExecutionBindingEpoch predecessor_epoch;
  std::optional<core::ExecutionBindingEpoch> successor_epoch;
  /// True only after a positive exact controller-manager switch response.
  bool switch_acknowledged{false};
  /// SettingState: false captures the receipt baseline before the one publish;
  /// true waits for consumption ACK and publishes/projects backend command.
  bool target_published{false};
};

using MockResetPositionStepDeadline = std::chrono::steady_clock::time_point;
/// The existing step budget starts at entry, including validation/dispatch.
/// Owner and SDK consume the SAME
/// absolute deadline; no new overall Reset budget or timer is introduced.
using MockResetPositionPhaseCallback = std::function<core::Result<void>(
    const MockResetPositionTransition &, MockResetPositionStepDeadline)>;

/// Host sequencing of the arm controller around a lane-specific guard (the
/// Isaac command-age guard). Every member is required.
struct RetainedRos2ControlArmGuardHooks final {
  /// Every motion path of the arm (goals and replacements) is admitted
  /// only while this holds.
  std::function<bool()> motion_admitted;
  /// Run before every activation; an error refuses it before effect.
  std::function<core::Result<void>()> before_activation;
  /// The operator's Recover of the arm (the Reset after a trip).
  std::function<void()> on_recover;
  /// A guard fault awaiting the operator (reported as a `fault` cause).
  std::function<std::optional<std::string>()> fault;
  /// Optional: stops and joins the guard's own worker, which polls this
  /// handle. The last external owner calls it first, off that worker.
  std::function<void()> stop{};
};

/// The robot's tool context with the TCP at a compiled frame, or at
/// its default TCP when absent (the flange without one).
using RetainedToolContextSourceFn = std::function<core::Result<core::ToolContextDTO>(
    const std::optional<std::string> &tcp_frame)>;
using CommittedSceneSourceFn = std::function<std::shared_ptr<const core::LogicalSceneSnapshotDTO>()>;
using CommittedToolContextSourceFn = std::function<core::Result<core::ToolContextDTO>(
    const std::optional<std::string> &, const core::LogicalSceneSnapshotDTO &)>;

/// Active TCP: appends one confirmed change to the host's
/// ordered events stream (and History). Called under the handle's lock, so
/// the stream order is the commit order; it never calls back into the handle.
using ActiveTcpEventSink = std::function<void(const core::ActiveTcpChangeDTO &change)>;

/// Active TCP: the robot's Active TCP with its order: the owner's
/// `registration_generation`, from one host-process counter incremented at
/// every install (every runtime start or restart; the stable Kernel lives in
/// the same process, so it is monotonic within its owner epoch), and a
/// `sequence` from 0 at install, +1 on every confirmed change, resets
/// included. The host adds the Kernel owner epoch; a reader keeps the
/// greatest `{owner_epoch, registration_generation, sequence}`.
struct ActiveTcpReading final {
  /// The robot; none when this handle owns no Active TCP (a gripper-only
  /// component).
  std::optional<core::ControllableComponentId> robot;
  std::uint64_t registration_generation{};
  std::optional<RobotTcp> value;
  std::uint64_t sequence{};
  std::optional<core::TcpEntryReferenceDTO> reference;
};

/// Build-owned measured JointState role of the selected lane. Instance
/// identity remains the verified physical provider/binding/runtime tuple.
inline constexpr std::string_view kMeasuredJointStateAuthorityId =
    "manyforge.ros2_control.measured_joint_state";
inline constexpr std::string_view kMeasuredJointStateProfileId =
    "manyforge.ros2_control.joint_state.measured.v1";

struct RetainedMeasuredJointStateLane final {
  std::string absolute_topic;
  core::Duration freshness_budget_ns;
  core::StateAuthorityId measured_state_authority_id;
  std::string measured_source_profile_id;
  /// Defaults of the component's `measured_joints` stream row (budget =
  /// `freshness_budget_ns`, message-stamp liveness).
  core::StreamSeverityDTO stream_severity;
  core::StreamRecoveryDTO stream_recovery;
  /// Mock grippers (stock ParallelGripperCommand) only: the gripper's
  /// `gripper_feedback` stream rides this lane. Its sample is the gripper
  /// joint in this JointState; defaults are the component's row. Isaac
  /// grippers record their raw feedback in the gripper backend instead.
  std::optional<core::StreamDeclarationDTO> gripper_feedback_stream;

  friend bool operator==(const RetainedMeasuredJointStateLane &,
                         const RetainedMeasuredJointStateLane &) = default;
};

/// Fresh empty source ownership staged by the existing route/product owner.
/// Aliasing readers independently retain storage and the actual clock/node.
/// Destruction of this staging value never retires a pinned predecessor.
class PreparedMeasuredStateSources final {
public:
  [[nodiscard]] const std::vector<core::MeasuredStateSourceDescriptor> &
  descriptors() const noexcept;
  [[nodiscard]] const std::vector<
      std::shared_ptr<const core::IObservedStateProvider>> &
  readers() const noexcept {
    return readers_;
  }
  /// The route owner calls this only after positive exact readback and its
  /// coherent no-throw association commit. No sample is copied or minted.
  void EnablePublication() noexcept;
  /// First publication sequence eligible after the last owner rebuild for
  /// this exact reader, if that source participated in such a rebuild.
  [[nodiscard]] std::optional<std::uint64_t> OwnerRebuildNextSequence(std::size_t reader_index) const;
  struct RebuildBoundary final {
    std::shared_ptr<const void> owner;
    std::uint64_t next_sequence{};
  };
  [[nodiscard]] std::optional<RebuildBoundary> OwnerRebuildBoundary(std::size_t reader_index) const;

private:
  friend class RetainedRos2ControlRuntimeHandle;
  struct Storage;
  PreparedMeasuredStateSources(
      std::shared_ptr<Storage> storage,
      std::vector<std::shared_ptr<const core::IObservedStateProvider>> readers);
  std::shared_ptr<Storage> storage_;
  const std::vector<std::shared_ptr<const core::IObservedStateProvider>>
      readers_;
};

/// Lane facts the handle needs to construct one JTC adapter instance per
/// binding epoch.
struct RetainedRos2ControlArmTransport final {
  /// Adapter configuration in the controller's own joint names; the binding
  /// epoch is assigned by the handle.
  ros2_control_execution::JtcExecutionAdapterConfig adapter;
  RetainedRos2ControlArmController controller;
  /// The participant binding's compiled joint names, index-aligned with
  /// `adapter.lane_facts.joint_names` (same coordinate order).
  std::vector<std::string> compiled_joint_names;
  /// An explicitly configured gripper controller in this same controller
  /// manager/hardware. Absent for non-CM transports; never derived from an URL.
  std::optional<RetainedRos2ControlArmController> gripper_controller{};
  /// Absent is unavailable, never a zero-robot or commanded-state fallback.
  std::optional<RetainedMeasuredJointStateLane> measured_joint_state_lane;
  /// The launch's reset-position writer (the Mock and Isaac lanes' generated
  /// artifacts); with a `command_echo_topic` the handle subscribes to it.
  std::optional<MockResetPositionTarget> mock_reset_position{};
};

/// Retained ROS resource lifetime of one runtime composition: the shared
/// transport, the arm's JTC adapter (one instance per binding epoch), the Core
/// participant over it, the gripper adapter and the arm controller's
/// lifecycle facts.
///
/// Binding epochs: the first adapter instance carries epoch 1.
/// Before every component enable that follows an earlier activation the handle
/// retires the current instance (`shutdown()`: admission closed, late
/// callbacks and samples rejected) and constructs the next epoch's instance,
/// which it hands to the participant with `RebindPort` before the switch is
/// sent. A retired instance is shut down first: it detaches and releases its
/// SDK entities while the shared executor keeps running for the other
/// transport users (in-flight SDK callbacks keep their own ownership; its
/// closed state blocks late effects); it is released once Core drops it.
/// Final teardown still shuts down every child, then quiesces the transport.
///
/// Readiness fed to the current instance: controller active and hardware
/// active from the controller manager's activity topic; controller bound only
/// after the execution host's component coordinator enabled it on the current
/// epoch and until that component is disabled.
class RetainedRos2ControlRuntimeHandle final
    : public std::enable_shared_from_this<RetainedRos2ControlRuntimeHandle> {
  struct ConstructionKey final {
    explicit ConstructionKey() = default;
  };
  struct ExternalOwner;

public:
  using ParticipantFactory =
      std::function<std::shared_ptr<core::ExecutionPortParticipant>(
          std::shared_ptr<core::IExecutionPort>)>;

  /// The only owner a host or test holds. Runtime callbacks lock
  /// `weak_from_this()`, a separate internal lease. When the last external
  /// owner releases, that releasing (non-executor) thread stops the guard
  /// and children and quiesces the transport (cancel, drain, join), and
  /// only then drops its lease. A callback lease in flight therefore ends
  /// before that join returns and is never the last owner on the executor.
  [[nodiscard]] static std::shared_ptr<RetainedRos2ControlRuntimeHandle>
  Create();
  explicit RetainedRos2ControlRuntimeHandle(ConstructionKey) {}
  ~RetainedRos2ControlRuntimeHandle();
  enum class HostQuiesceOutcome { kSettled, kUnresolved };
  struct HostQuiesceResult final {
    HostQuiesceOutcome outcome;
    const char *stage;
    std::string code;
  };
  [[nodiscard]] HostQuiesceResult QuiesceForHostShutdown(
      std::chrono::steady_clock::time_point deadline) noexcept;
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  OwnerShutdownDeadline() const noexcept;
  [[nodiscard]] core::Result<void> SetOwnerShutdownTimeout(core::Duration timeout_ns);

  RetainedRos2ControlRuntimeHandle(const RetainedRos2ControlRuntimeHandle &) =
      delete;
  RetainedRos2ControlRuntimeHandle &
  operator=(const RetainedRos2ControlRuntimeHandle &) = delete;

  /// The participant factory must only construct; it must not send through
  /// the supplied port until Bind has returned success.
  /// If a gripper backend is supplied, SetOwnerShutdownTimeout must already
  /// have received that gripper's authored lifecycle timeout.
  /// Binds once: constructs the initial adapter (binding epoch 1), the
  /// participant over it and takes the gripper adapter. Fails closed
  /// (everything shut down) on any mismatch.
  /// An optional genuine clock owner must name this same receiver clock and
  /// remains alive through child and transport teardown.
  [[nodiscard]] core::Result<void> Bind(
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> resources,
      core::IClock &clock, core::IScheduler &scheduler, RetainedRos2ControlArmTransport arm,
      const ParticipantFactory &make_participant,
      std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend>
          gripper,
      const core::RuntimeParticipantBinding &motion_binding,
      const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
      std::shared_ptr<core::IClock> clock_owner = {});

  /// Bind a gripper-only component to its actual transport and controller.
  /// No arm participant, execution binding epoch or motion adapter is created.
  /// SetOwnerShutdownTimeout must already have received its authored timeout.
  [[nodiscard]] core::Result<void> BindGripperOnly(
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> resources,
      core::IClock& clock,
      std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend> gripper,
      const core::RuntimeParticipantBinding& binding,
      RetainedRos2ControlArmController controller,
      std::shared_ptr<core::IClock> clock_owner = {},
      std::optional<RetainedMeasuredJointStateLane> measured_lane = {});

  /// A machine's process with its arm deactivated (the owner
  /// runs it; no arm, no gripper). Only the controller manager and its
  /// hardware are observed (`controller` names no controller), so the
  /// observation is the process's: its exit, and the controller manager
  /// present or not. Nothing is ever switched.
  [[nodiscard]] core::Result<void> BindProcessOnly(
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> resources,
      core::IClock &clock, RetainedRos2ControlArmController controller,
      std::shared_ptr<core::IClock> clock_owner = {});

  [[nodiscard]] std::shared_ptr<core::ExecutionPortParticipant>
  participant() const;

  /// The current binding epoch's port as the participant sees it.
  [[nodiscard]] std::shared_ptr<core::IExecutionPort> current_port() const;

  /// The arm's lane facts in the controller's own joint names (the guard
  /// status joint-name check).
  [[nodiscard]] std::vector<std::string> controller_joint_names() const;

  /// The shared transport node (controller switch client, guard binding).
  [[nodiscard]] rclcpp::Node::SharedPtr node() const;

  /// Unwired measured-source construction on the already bound node/executor.
  /// The explicit topic carries JointState; descriptors explicitly map
  /// their source_joint_names to addressed coordinates, index for index.
  /// Receipt uses the same owner clock supplied to Bind.
  /// Construct empty inactive sources on the bound lane's existing node.
  /// Candidate bindings come from the qualified route owner before effects.
  /// Construction checks physical transport reuse, not route effect authority.
  /// Never replaces or retires predecessor sources on a shared handle.
  /// Successful null means subscription construction is operationally
  /// unavailable; the actual exception is logged. Invalid association is Err.
  /// An optional render wake is the cell viewer wake: it advances only when a
  /// source's visible coordinates (or their completeness) change.
  [[nodiscard]] core::Result<std::shared_ptr<PreparedMeasuredStateSources>>
  StageMeasuredStateSources(
      std::vector<core::MeasuredStateSourceDescriptor> descriptors,
      std::shared_ptr<core::IClock> receiver_clock,
      const core::RuntimeParticipantBinding &candidate_motion,
      const std::optional<core::RuntimeParticipantBinding> &candidate_gripper,
      std::shared_ptr<RetainedObservationWake> render_wake = {});

  [[nodiscard]] core::Result<std::shared_ptr<PreparedMeasuredStateSources>>
  StageMeasuredStateSources(
      std::vector<core::MeasuredStateSourceDescriptor> descriptors,
      std::shared_ptr<core::IClock> receiver_clock,
      const std::optional<core::RuntimeParticipantBinding>& candidate_motion,
      const std::optional<core::RuntimeParticipantBinding>& candidate_gripper,
      std::shared_ptr<RetainedObservationWake> render_wake = {});

  struct StagedMeasuredOwnerRebind;
  /// Stage against the host's exact installed source record. The host keeps
  /// its existing recovery exclusion and revalidates that record at commit.
  [[nodiscard]] std::shared_ptr<StagedMeasuredOwnerRebind> StageMeasuredOwnerRebind(
      std::shared_ptr<PreparedMeasuredStateSources> installed,
      std::shared_ptr<ros2_control_execution::Ros2TransportResources> candidate,
      std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> candidate_activity = {});
  [[nodiscard]] core::Result<void> CommitMeasuredOwnerRebind(
      const std::shared_ptr<StagedMeasuredOwnerRebind> &stage);
  /// Validate the measured slots under handle/storage locks and invoke an
  /// enclosing no-throw commit before publishing the measured replacement.
  [[nodiscard]] bool CommitMeasuredOwnerRebindWith(
      const std::shared_ptr<StagedMeasuredOwnerRebind> &stage,
      bool (*commit)(void *) noexcept, void *context,
      const std::shared_ptr<ros2_control_execution::Ros2TransportResources> &failed_dependency = {});
  [[nodiscard]] core::Result<void> ActivateMeasuredOwnerRebind(
      const std::shared_ptr<StagedMeasuredOwnerRebind> &stage);
  [[nodiscard]] bool OwnsPreparedMeasuredSource(
      const std::shared_ptr<PreparedMeasuredStateSources> &source) const;

  /// Installs the lane guard's hooks; only before `StartBackend`.
  void InstallGuardHooks(RetainedRos2ControlArmGuardHooks hooks);

  /// The vendor family's tool-context hook and the
  /// robot's tool context sources, which the hook applies at Enable (the
  /// Active TCP) and before every new motion goal (that motion's TCP). Once,
  /// at any time (the family's adapters are made after the backend starts);
  /// never for a lane without a vendor driver (Mock, Isaac).
  void InstallToolContext(ToolContextHook hook, RetainedToolContextSourceFn source,
                          CommittedToolContextSourceFn committed_source,
                          CommittedToolContextSourceFn enable_source);
  void InstallCommittedSceneSource(CommittedSceneSourceFn source);
  /// Display joint source (the vendor family's own joint
  /// topic): the arm's display channel follows it in every mode. A sample
  /// naming each bound joint exactly once (by its source name), finite, that
  /// the family's check does not ignore moves the display; any other keeps
  /// the last valid pose. A leading sample also refreshes the
  /// `measured_joints` stream's liveness (a dead driver still goes stale) and
  /// leads planning previews. Never the addressed state, the planning scene
  /// or the Enable gate's joints. Once, at any time, on a measured arm.
  void InstallDisplayJointSource(const DisplayJointSource &source,
                                 std::shared_ptr<RetainedObservedStateChannel> channel);

  /// Active TCP: the robot's TCP list, its
  /// Active TCP starting at the End-effector row (none without one), and the
  /// Deployment's "Send TCP offset to controller" (`controller_tcp_offset_sync`).
  /// Once, before `StartBackend`; every lane (Mock, Isaac, Real).
  /// `direct_control_binding`: Robot control's Program binding for this
  /// robot; a move under it records its TCP change as Robot control's.
  void InstallActiveTcp(core::ControllableComponentId robot,
                        std::vector<RobotTcp> rows,
                        std::vector<TcpFrameAssociation> associations,
                        bool controller_tcp_offset_sync,
                        std::optional<core::ProgramBindingId> direct_control_binding = {});

  [[nodiscard]] bool HasToolContext() const;

  /// Install already-staged candidate facts after publication under the Apply
  /// fence. With a tool-context hook, an enabled robot gets the new offset.
  core::Result<void> RebindRetainedToolContext(
      std::vector<RobotTcp> rows,
      std::vector<TcpFrameAssociation> retained_associations,
      std::optional<core::ProgramBindingId> direct_control_binding,
      RetainedToolContextSourceFn source, CommittedToolContextSourceFn committed_source,
      std::function<std::optional<core::Pose3DTO>(const std::string &)> fixed_pose,
      CommittedSceneSourceFn scene_source = {},
      CommittedToolContextSourceFn enable_source = {});

  /// Active TCP: where confirmed changes are appended (the host's events
  /// stream). Any time; a change before it is only in the baseline.
  void BindActiveTcpEvents(ActiveTcpEventSink sink);

  /// Active TCP: the value and its events-stream sequence, read together.
  [[nodiscard]] ActiveTcpReading ReadActiveTcp() const;
  /// Active TCP: the robot's list and value (Robot control's resolver).
  [[nodiscard]] ActiveTcp ReadRobotTcps() const;

  /// "Send TCP offset to controller" on: Set TCP holds the robot.
  [[nodiscard]] bool ActiveTcpHoldsRobot() const;

  /// Active TCP: Set TCP, a Program's or Robot
  /// control's. `entry` not in the list: `active_tcp_unknown`. The same
  /// entry: Ok, nothing sent. Otherwise, with the robot enabled, the
  /// checkbox on and a tool-context hook, the confirmed TCP step (offset,
  /// settle, check); then the commit and its event. Not enabled: the commit
  /// alone, and the next Enable re-syncs. A step error leaves the Active TCP
  /// unchanged. Serialized with the lifecycle steps (a `kArm` step).
  [[nodiscard]] core::Result<void>
  SetActiveTcp(const core::TcpEntryReferenceDTO &entry, core::ActiveTcpChangeSource source);

  /// Active TCP: before a new motion goal, nothing
  /// in flight, the move's TCP entry. The same as the Active TCP: nothing,
  /// no adapter call. Otherwise the confirmed TCP step (as Set TCP, the robot
  /// enabled), then the commit before the goal is sent (source Robot control
  /// when `program_binding` is the robot's direct-control binding, else
  /// Program);
  /// a move that fails later keeps it. An error refuses the motion
  /// before effect with the Active TCP unchanged. Mock, Isaac and the checkbox
  /// off: the commit alone.
  [[nodiscard]] core::Result<void>
  ApplyMotionToolContext(const core::TcpEntryReferenceDTO &entry,
                         const std::optional<core::ProgramBindingId> &program_binding = {});

  /// Active TCP: reset to the End-effector TCP
  /// (Program Reset with "reload initial scene", `simulation.reset`, an
  /// Isaac world reload). Already there: Ok, nothing sent. Otherwise, with
  /// the robot enabled, the checkbox on and a tool-context hook, the End-effector
  /// offset is sent through the same step; if the controller cannot take it,
  /// the reset still completes and the next Enable re-syncs. A transport
  /// admission refusal is returned without changing Active TCP.
  [[nodiscard]] core::Result<void> ResetActiveTcp();
  /// The committed Scene's total payload, before each motion goal. Without a
  /// tool-context hook nothing is sent. With one it waits only for another
  /// payload update; an arm lifecycle step in flight refuses it at once
  /// (`execution_rejected`, `component_lifecycle_step_in_flight`, OD-69); a
  /// gripper switch doesn't concern it.
  [[nodiscard]] core::Result<void> ApplyLatestCommittedToolPayload();
  /// The same update from the payload follower (a worker): it waits for a
  /// step in flight instead of being refused.
  [[nodiscard]] core::Result<void> FollowLatestCommittedToolPayload();
  [[nodiscard]] core::Result<void> ApplyCommittedToolPayload(
      const CommittedSceneSourceFn &read);

  /// The participant's pre-planning sync of an admitted motion: after a
  /// tool-context send that may have re-armed the robot, binds the
  /// controllers its driver reactivated (see `RebindAfterRearm`) before the
  /// motion is planned. Ok with nothing pending. A failure refuses the motion
  /// before effect.
  [[nodiscard]] core::Result<void> RebindPendingRearm();

private:
  friend core::Result<void> host_detail::ResetCompositionActiveTcps(
      host_detail::Composition &, host_detail::ExecutionCompositionAccess &);
  // Only the direct method or the all-member host admission may enter here.
  [[nodiscard]] core::Result<void> ResetActiveTcpAdmitted();
  /// The caller holds a step that excludes payload sends (a `kToolPayload`
  /// or `kArm` `SerializedStep`), or the handle has no tool-context hook
  /// (nothing is sent).
  [[nodiscard]] core::Result<void> ApplyCommittedToolPayloadClaimed(
      const CommittedSceneSourceFn &read);
  /// After a re-arm by the tool-context hook (the vendor driver then
  /// reactivates the controllers itself): wait, at most the hook's
  /// `rearm_bound`, for the controller manager to report the hardware, the
  /// trajectory controller and the broadcaster active, then bind the
  /// reactivated controller on a new epoch. Caller holds a `kArm` `SerializedStep`.
  [[nodiscard]] core::Result<void> RebindAfterRearm(std::chrono::nanoseconds bound);
  /// Active TCP: with `send`, the vendor adapter's TCP step at `tcp_frame`
  /// (none: no TCP) and its settle; then the commit of `entry` (none: the
  /// reset) and its event. `send_failure_completes`: a failed step still
  /// commits (a reset). Caller holds a `kArm` `SerializedStep`.
  [[nodiscard]] core::Result<void>
  ChangeActiveTcp(const std::optional<core::TcpEntryReferenceDTO> &entry,
                  const std::optional<std::string> &tcp_frame,
                  core::ActiveTcpChangeSource source, bool send,
                  bool send_failure_completes);
  /// Active TCP: the tool-context hook's TCP step: Ok(true) when it
  /// sent an offset (and the rebind after its re-arm succeeded). Without a
  /// hook, a source or the checkbox: Ok(false). Caller holds a `kArm` `SerializedStep`.
  [[nodiscard]] core::Result<bool>
  SendToolContextAtRest(const std::optional<std::string> &tcp_frame);
  [[nodiscard]] core::Result<bool> SendPreparedToolOffset(const ToolContextHook &hook,
                                                          const core::Pose3DTO &offset);
  /// A failed call whose effect may remain (`details.effect`) sets
  /// `tool_context_fault_` (persistent until Recover).
  void ReportToolContextFailure(const core::ErrorDTO &error);

public:

  /// The component's Manager-owned controller launch. Once the source
  /// reports an exit reason, the arm and gripper domains report a `fault`
  /// (the component is failed and motion admission closes); nothing restarts
  /// it here. Only before `StartBackend` (it may precede `Bind`); at most
  /// once. The optional health source reads the same exact owner cache without
  /// advancement; when absent, health is unavailable rather than calling the
  /// lifecycle advancement source.
  void InstallControllerProcessFaultSource(
      std::function<std::optional<std::string>()> source,
      std::function<core::Result<std::optional<std::string>>()> health_source =
          {});
  /// Exact local Kernel callbacks for a fault-owner rebuild. The host binds
  /// these once to the Kernel paired with this handle. The assessment must
  /// match both the participant execution and its observation boundary.
  void InstallOwnerRecoveryHooks(
      std::function<void()> notify,
      std::function<bool(std::uint64_t, std::uint64_t)> qualified);
  void InstallLateCancelJournalHook(const void *engine_identity,
                                    std::function<core::Result<void>(std::uint64_t)> hook);
  [[nodiscard]] core::Result<void> InstallGripperLateCancelJournalHook(
      const void *engine_identity,
      ros2_control_execution::Ros2GripperCommandBackend::LateCancelJournalHook hook);
  void InstallOwnerFaultWake(std::function<void()> wake);
  /// Host-owned non-blocking exclusion for direct TCP effects.
  void InstallTransportEffectClaim(
      std::function<core::Result<std::shared_ptr<void>>()> claim);
  /// Existing runtime authority supplies and revalidates its installed
  /// measured-source record across an owner rebuild.
  void InstallMeasuredSourceRecoveryHooks(
      std::function<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>()> snapshot,
      std::function<bool(const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &)> matches);
  void InstallMeasuredSourceCommitHook(
      std::function<bool(const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &,
                         void (*)(void *) noexcept, void *)> commit);
  /// Retains the original machine-owner construction inputs for an explicit
  /// fault rebuild. Installed once before starting this handle.
  void InstallTransportFactory(
      std::function<std::shared_ptr<ros2_control_execution::Ros2TransportResources>()> factory);
  struct TransportOwners final {
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> machine;
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> epoch;
  };
  [[nodiscard]] TransportOwners TransportOwnerSnapshot() const;
  [[nodiscard]] std::shared_ptr<const TransportOwners> PublishedTransportOwners() const noexcept;
  /// Called under the host's accepted exclusive recovery permit.
  [[nodiscard]] core::ActionResult RecoverFaultedOwner(
      TransportOwners expected, std::stop_token stop, TransportOwners &replacement);
  /// The bring-up bound: the component's controller launch, started at
  /// `started_at`, has `settled` (every spawner it started has exited) within
  /// `span`. A Prepare first waits for it (`WaitForControllerLaunch`); only
  /// then does the component's lifecycle command time apply. Only before
  /// `StartBackend`; at most once.
  void InstallControllerLaunchSettle(std::function<bool()> settled,
                                     std::chrono::steady_clock::time_point started_at,
                                     std::chrono::steady_clock::duration span);
  /// Driver restart: the restarted launch (started at
  /// `started_at`) settles within the installed span. Nothing without an
  /// installed settle source.
  void ReplaceControllerLaunchSettle(std::function<bool()> settled,
                                     std::chrono::steady_clock::time_point started_at);
  /// Waits until the installed launch has settled; `component_launch_not_settled`
  /// at its bound. Nothing to wait for without an installed launch.
  [[nodiscard]] std::optional<RetainedRos2ControlSwitchResult>
  WaitForControllerLaunch(std::stop_token cancellation) const;
  /// Whether the installed launch is still starting: not settled, its process
  /// not exited, and within its bound. False without an installed launch.
  [[nodiscard]] bool ControllerLaunchSettling() const;
  /// Cache the already captured diagnostic of this exact process occurrence.
  /// The host checks its installed handle/process under the existing fence;
  /// construction/encoding occurs outside all observation/control locks.
  [[nodiscard]] bool CacheControllerProcessIncidentDiagnostic(
      std::string incident_id,
      std::shared_ptr<const content_identity::CanonicalValue> snapshot);
  [[nodiscard]] bool
  HasControllerProcessIncidentDiagnostic(const std::string &incident_id) const;
  /// Whether this exact handle/process occurrence already caches its first
  /// exit snapshot. This makes no claim about durable incident persistence.
  [[nodiscard]] bool HasControllerProcessIncidentDiagnostic() const;

  /// Called on the transport executor, at the lane's state rate, with every
  /// new controller sample of the current binding epoch (the arm's
  /// commanded-state publication). Only before `StartBackend`.
  void InstallSampleObserver(
      std::function<void(const core::ExecutionControllerSampleDTO &)> observer);

  /// A measured-authority arm's addressed state comes from its
  /// bound measured JointState lane instead of the controller reference.
  /// Called on the transport executor with every received message. Only
  /// before `StartBackend`; exclusive with `InstallSampleObserver`.
  void InstallMeasuredStateObserver(
      std::function<void(const sensor_msgs::msg::JointState &)> observer);

  /// Driver restart: the component's controller launch was
  /// restarted; its exit is now read from the new process's sources.
  void ReplaceControllerProcessFaultSource(
      std::function<std::optional<std::string>()> source,
      std::function<core::Result<std::optional<std::string>>()> health_source);

  /// Verify the live factory-bound participant and its current sealed physical
  /// bindings.
  [[nodiscard]] bool
  OwnsBackend(const core::IExecutionBackend *backend,
              const core::RuntimeParticipantBinding &motion_binding,
              const std::optional<core::RuntimeParticipantBinding>
                  &gripper_binding) const;

  [[nodiscard]] bool OwnsGripper(
      const core::IGripperCommandBackend *gripper,
      const core::RuntimeParticipantBinding &gripper_binding) const;

  /// Product resealing uses the existing runtime's fenced callback boundary.
  /// These operations preserve the transport, participant, and binding epoch.
  [[nodiscard]] core::Result<void>
  RebindMotionRoute(core::RuntimeParticipantBinding binding,
                    std::vector<core::RuntimeParticipantBinding> aliases);
  /// Display-only bridge follows the current verified route; it never feeds
  /// authoritative state publication or readiness.
  [[nodiscard]] core::Result<void> BindGripperDisplayChannel(
      const std::shared_ptr<RetainedObservedStateChannel>& channel);
  [[nodiscard]] core::Result<void> PublishGripperDisplayPosition(double position);
  [[nodiscard]] core::Result<void>
  RebindGripperRoute(core::RuntimeParticipantBinding binding,
                     std::shared_ptr<IRetainedObservedStateChannel> channel);
  [[nodiscard]] bool
  OwnsBinding(const core::RuntimeParticipantBinding &binding) const;

  /// Nonwaiting, one-shot transport start after the host retains fenced
  /// ownership: completes the gripper's ROS configuration, creates the
  /// controller-manager activity subscription, switch client and controller
  /// statistics monitor, then starts the executor. A partial-start
  /// exception is retained; observing it never retries startup.
  void StartBackend();

  /// The adapter's `prepare`: the loaded (inactive) controller on active
  /// hardware with the joint state broadcaster active is the prepared state.
  /// Until `deadline` (the component's lifecycle command time) it waits for
  /// its own endpoints (the controller manager reporting the loaded
  /// controller). The hardware is left as it is: a vendor adapter switches
  /// it on at its Enable (`SwitchArmHardware`).
  [[nodiscard]] RetainedRos2ControlSwitchResult
  PrepareArmController(std::stop_token cancellation, MockResetPositionStepDeadline deadline);

  /// The adapter's `enable`: activate the arm's trajectory controller
  /// (`switch_controller`, strict). Waits for the answer until `cancellation` or `deadline`.
  /// With a tool-context hook its re-sync runs before the switch. The
  /// re-sync is at the Active TCP, which Enable keeps.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  ActivateArmController(std::stop_token cancellation, MockResetPositionStepDeadline deadline);

  /// The adapter's `disable`: deactivate it. Known success without a request
  /// when the controller manager already reports it inactive.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  DeactivateArmController(std::stop_token cancellation, MockResetPositionStepDeadline deadline);

  /// The adapter's `recover`: an active controller is switched off, the
  /// guard hook runs and the controller is left inactive and prepared (the
  /// hardware as it is); the next Enable activates it on a new binding
  /// epoch.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  RecoverArmController(std::stop_token cancellation, MockResetPositionStepDeadline deadline);

  /// A vendor adapter's arming (xArm Enable and Disable): the arm's
  /// hardware component set active or inactive through the controller
  /// manager's stock `set_hardware_component_state`, with the trajectory
  /// controller off (a vendor driver arms its robot on activation). Switched
  /// on, the joint state broadcaster too when the controller manager reports
  /// it loaded but off. Known success without a request when the controller
  /// manager already reports it so. Each request once, never retried.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SwitchArmHardware(bool active, std::stop_token cancellation);

  /// A gripper's `prepare`: its controller loaded inactive on active
  /// hardware (or its configured local owner, or, for an Isaac gripper
  /// without a controller manager, its ready command bridge). Until
  /// `deadline` it waits for its own endpoints, as the arm's does.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  PrepareGripperController(std::stop_token cancellation, MockResetPositionStepDeadline deadline);
  /// A gripper's `enable` / `disable`: switch its controller (or its local
  /// owner) on or off.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SwitchGripperController(bool activate, std::stop_token cancellation,
                          MockResetPositionStepDeadline deadline);

  [[nodiscard]] std::optional<MockResetPositionTarget>
  mock_reset_position_target() const;
  [[nodiscard]] bool mock_reset_writer_inactive() const;
  [[nodiscard]] bool mock_reset_writer_active() const;
  [[nodiscard]] bool mock_reset_successor_ready() const;
  [[nodiscard]] std::shared_ptr<RetainedObservationWake>
  mock_reset_observation_wake() const noexcept;
  /// The latest sample on the target's `command_echo_topic` (it also wakes
  /// `mock_reset_observation_wake`); none before the first one or without
  /// an echo topic.
  [[nodiscard]] std::optional<ResetCommandEcho> reset_command_echo() const;
  /// Owner-private transport primitive. The execution host's reset-action
  /// path pins the exact Reset claim/product before calling; callbacks run
  /// without a handle state lock.
  /// The callback arms the existing Core scope with the supplied actual step
  /// deadline before SDK effects or waits; no driver lock spans SDK calls.
  /// A switching callback runs before send, then again only after positive
  /// ACK; that second callback waits for exact CM destination observation.
  /// SettingState(target_published=true) proves one fresh exact ACK (the
  /// hardware's loopback state, or its command echo) and publishes/projects
  /// backend command before returning Ok. It performs
  /// no measured velocity, reference, convergence or dwell verification.
  /// No caller-supplied coordinates or blind rollback.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  ResetMockPosition(std::stop_token cancellation,
                    const MockResetPositionPhaseCallback &phase);

  /// What the controller manager and the controllers report now
  /// (`ros2_control_component_state` maps it). A pure read.
  [[nodiscard]] Ros2ControlObservation ReadObservation(core::TimePoint now_ns) const;

  [[nodiscard]] RetainedRos2ControlArmActivity arm_activity() const;

  /// Read-only transport diagnostics for the exactly bound gripper target.
  [[nodiscard]] std::optional<
      ros2_control_execution::Ros2GripperTransportReadinessSnapshot>
  gripper_transport_readiness_snapshot() const;

  /// The arm's motion control domain, if bound.
  [[nodiscard]] std::optional<core::ControlDomainId> motion_domain() const;

  /// Monitor only: the arm controller's statistics as observed from
  /// the controller manager since StartBackend (absent before it).
  [[nodiscard]] std::optional<RetainedControllerStatisticsSnapshot>
  controller_statistics() const;

private:
  friend class RetainedRos2ControlRuntimeHandleTestAccess;
  struct ArmState;
  struct StagedMachineOwner;
  [[nodiscard]] std::shared_ptr<StagedMachineOwner> StageMachineOwner();
  void PublishStagedActivityLocked(const std::shared_ptr<StagedMachineOwner> &stage);
  void ActivateStagedMachineOwner(const std::shared_ptr<StagedMachineOwner> &stage);
  struct GripperOnlyState;
  class ArmPort;

  // Test access to the current epoch owner, without exposing it publicly.
  std::shared_ptr<ros2_control_execution::Ros2TransportResources>
  TestEpochResources() const;

  [[nodiscard]] static rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
  CreateMeasuredSubscription(
      const std::shared_ptr<PreparedMeasuredStateSources::Storage> &storage,
      const rclcpp::Node::SharedPtr &node, const std::string &topic,
      std::shared_ptr<ros2_control_execution::SelectedRosEndpointFacts> activity_endpoints,
      std::shared_ptr<std::atomic<bool>> callbacks_enabled = {},
      rclcpp::CallbackGroup::SharedPtr callback_group = {});
  [[nodiscard]] rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr
  CreateDisplayJointSubscription(
      const rclcpp::Node::SharedPtr &node, const DisplayJointSource &source,
      const std::shared_ptr<RetainedObservedStateChannel> &channel,
      std::shared_ptr<std::atomic<bool>> callbacks_enabled = {});

  void ObserveActivity(
      RetainedRos2ControlArmActivity activity,
      std::optional<RetainedRos2ControlArmActivity> gripper_activity = {});
  [[nodiscard]] core::Result<std::shared_ptr<void>> EnterTransportEffect() const;
  /// Graph refresh stays on the existing NRT Observe/observer path; health
  /// only consumes the maintained endpoint facts.
  void RefreshEndpointFacts();
  [[nodiscard]] std::optional<core::ErrorDTO> ReadEndpointDiagnostic() const;
  void PublishReadinessLocked();
  /// The lifecycle of a gripper without a controller manager (the Isaac
  /// command bridge), as a command-bridge domain: `prepare`, `activate`
  /// or `deactivate` through the gripper backend.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  RunGripperBridgeLifecycle(core::ComponentLifecycleStateDTO target, std::stop_token cancellation,
                            MockResetPositionStepDeadline deadline);
  /// Entered and left with `lock` (on `mutex_`) held; the caller holds a
  /// `kArm` `SerializedStep`. The lock is released while the old epoch retires, while
  /// the next epoch's node, backend, owner and port are built, and across
  /// `RebindPort`; each step re-validates the captured epoch identities.
  [[nodiscard]] core::Result<void> RotateEpochLocked(
      std::unique_lock<std::mutex> &lock, std::chrono::steady_clock::time_point deadline);
  /// One hardware-state request, allowed min(the call cap, `step_deadline`):
  /// no answer within it is `ros2_control_hardware_state_timeout` (the
  /// connection lost).
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SendHardwareState(bool active, std::stop_token cancellation,
                    std::optional<MockResetPositionStepDeadline> step_deadline);
  /// One strict switch of one controller. Every lifecycle action passes
  /// `lifecycle_deadline` (the component's lifecycle command time): no answer
  /// by it is an uncertain outcome.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SendSwitch(bool activate, std::stop_token cancellation,
             std::optional<std::string> selected_controller,
             std::optional<MockResetPositionStepDeadline> lifecycle_deadline);
  /// After an answered switch or hardware transition, waits
  /// until `deadline` for the controller manager's activity report to show
  /// the `activate`/`deactivate` controllers and, if given, the arm hardware
  /// in the requested states. A report that doesn't come by then (or a Stop
  /// meanwhile) is `component_action_result_not_observed`. Controllers this
  /// handle doesn't observe are not waited for.
  [[nodiscard]] std::optional<RetainedRos2ControlSwitchResult>
  WaitForReportedActivity(const std::vector<std::string> &activate,
                          const std::vector<std::string> &deactivate,
                          std::optional<bool> hardware_active,
                          const std::stop_token &cancellation,
                          std::chrono::steady_clock::time_point deadline);
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SendControllerSwitch(std::vector<std::string> activate,
                       std::vector<std::string> deactivate,
                       std::stop_token cancellation,
                       std::optional<MockResetPositionStepDeadline> response_deadline = {},
                       std::optional<MockResetPositionStepDeadline> lifecycle_deadline = {},
                       bool best_effort = false);

  /// One driver calls this for guard stop and transport quiescence; completed
  /// shutdown is a no-op when the last external owner and destructor repeat it.
  [[nodiscard]] HostQuiesceResult StopAndQuiesce(std::chrono::steady_clock::time_point deadline =
                                                    std::chrono::steady_clock::time_point::max()) noexcept;

  /// Set once by Create(): owners handed out by the handle are external.
  std::weak_ptr<RetainedRos2ControlRuntimeHandle> external_owner_;
  mutable std::mutex mutex_;
  bool closing_{false};  // guarded by mutex_; no Bind or owner replacement after close
  bool shutdown_complete_{false}; // guarded by mutex_; all exact owners joined
  /// OD-69: the handle's serialized steps, held as data under `mutex_` and
  /// never as a lock across a controller-manager, vendor or owner call. A
  /// step waits (on `serialized_step_released_`, holding nothing) only on the
  /// thread that runs it; no ROS callback or owner thread claims one.
  ///  - kArm: an arm lifecycle step (Prepare, Enable, Disable, Recover,
  ///    hardware, Reset, owner recovery, tool-context rebind, Active TCP at
  ///    rest, a motion's TCP step and re-arm rebind). Excludes every step.
  ///  - kGripper: a gripper controller switch. Excludes the lifecycle steps
  ///    only: it touches neither the arm's tool context nor its epoch.
  ///  - kToolPayload: a payload update (each motion start, the payload
  ///    follower). Excludes payload updates and arm steps, never a gripper
  ///    switch.
  /// Stop and preemption never claim one.
  /// A motion start (`kMotionStart`) never waits behind a lifecycle step
  /// (OD-69): its payload update meeting a kArm step, or its TCP / re-arm
  /// step meeting any lifecycle step, is not admitted (`admitted()` false;
  /// the caller refuses `component_lifecycle_step_in_flight`). It still
  /// waits for a payload update in flight.
  enum class SerializedStepKind : std::uint8_t { kArm, kGripper, kToolPayload };
  enum class SerializedStepEntry : std::uint8_t { kWait, kMotionStart };
  class SerializedStep final {
  public:
    SerializedStep(RetainedRos2ControlRuntimeHandle &handle, SerializedStepKind kind,
                   SerializedStepEntry entry = SerializedStepEntry::kWait);
    ~SerializedStep();
    SerializedStep(const SerializedStep &) = delete;
    SerializedStep &operator=(const SerializedStep &) = delete;
    [[nodiscard]] bool admitted() const noexcept { return admitted_; }

  private:
    RetainedRos2ControlRuntimeHandle &handle_;
    const SerializedStepKind kind_;
    bool admitted_{false};
  };
  /// A motion start meeting a lifecycle step it must not wait behind.
  [[nodiscard]] bool MotionStartMeetsLifecycleStepLocked(SerializedStepKind kind) const;
  /// The payload update of a motion start (`kMotionStart`) or of the
  /// payload follower (`kWait`).
  [[nodiscard]] core::Result<void> ApplyLatestCommittedToolPayloadAs(SerializedStepEntry entry);
  [[nodiscard]] bool SerializedStepAdmissibleLocked(SerializedStepKind kind) const;
  /// Guarded by `mutex_`: the lifecycle step in flight (kArm or kGripper)
  /// and whether a payload update is in flight.
  std::optional<SerializedStepKind> lifecycle_step_;
  bool tool_payload_step_{false};
  std::condition_variable serialized_step_released_;
  const std::shared_ptr<RetainedObservationWake> reset_observation_wake_{
      std::make_shared<RetainedObservationWake>()};
  // Declared before dependent resources: destroyed after their teardown.
  std::shared_ptr<core::IClock> clock_owner_;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> resources_;
  std::function<std::shared_ptr<ros2_control_execution::Ros2TransportResources>()>
      transport_factory_;
  core::IClock *clock_{};
  core::IScheduler *scheduler_{};
  std::optional<RetainedRos2ControlArmTransport> arm_transport_;
  std::shared_ptr<ArmState> arm_;
  std::shared_ptr<GripperOnlyState> gripper_only_;
  std::atomic<std::shared_ptr<const TransportOwners>> published_transport_owners_;
  std::shared_ptr<StagedMachineOwner> live_machine_stage_;
  std::shared_ptr<core::ExecutionPortParticipant> participant_;
  std::function<void()> owner_recovery_notify_;
  std::function<void()> owner_fault_wake_;
  std::function<bool(std::uint64_t, std::uint64_t)> owner_recovery_qualified_;
  mutable std::optional<std::pair<std::uint64_t, std::uint64_t>> owner_recovery_marker_;
  mutable std::string owner_recovery_pending_fault_;
  void RetainLateCancelObligationsLocked(
      std::vector<ros2_control_execution::Ros2ControlExecutionBackend::UnresolvedLateCancel> goals);
  [[nodiscard]] core::Result<void> FlushLateCancelHandoff();
  std::vector<ros2_control_execution::Ros2ControlExecutionBackend::UnresolvedLateCancel>
      pending_late_cancel_handoff_;
  std::function<core::Result<void>(std::uint64_t)> late_cancel_journal_hook_;
  const void *late_cancel_engine_identity_{};
  std::function<core::Result<std::shared_ptr<void>>()> transport_effect_claim_;
  std::function<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>()>
      measured_source_snapshot_;
  std::function<bool(const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &)>
      measured_source_matches_;
  std::function<bool(const std::shared_ptr<const RetainedPreparedRobotMeasuredSources> &,
                     void (*)(void *) noexcept, void *)> measured_source_commit_;
  std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend> gripper_;
  std::optional<core::Duration> owner_shutdown_timeout_ns_;
  std::optional<core::RuntimeParticipantBinding> motion_binding_;
  std::optional<core::RuntimeParticipantBinding> gripper_binding_;
  /// The `measured_joints` stream of the bound component instance, declared
  /// at bind (a replacement of the same component id starts from its own
  /// fresh slot) and recorded by every staged subscription.
  std::shared_ptr<core::StreamRecorder> measured_stream_;
  /// The Mock `gripper_feedback` stream (lane `gripper_feedback_stream`),
  /// declared and recorded the same way.
  std::shared_ptr<core::StreamRecorder> gripper_feedback_stream_;
  std::weak_ptr<RetainedObservedStateChannel> gripper_display_channel_;
  std::optional<RetainedRos2ControlArmGuardHooks> guard_hooks_;
  /// The vendor family's tool-context hook; none without a vendor driver.
  std::optional<ToolContextHook> tool_context_hook_;
  RetainedToolContextSourceFn tool_context_source_;
  CommittedToolContextSourceFn committed_tool_context_source_;
  CommittedToolContextSourceFn committed_enable_tool_context_source_;
  CommittedSceneSourceFn committed_scene_source_;
  /// Active TCP: the robot's value and list, its component, the
  /// checkbox, the events sink and the robot's Active TCP sequence.
  /// Guarded by `mutex_`; changed only within a `kArm` `SerializedStep` too.
  ActiveTcp active_tcp_;
  std::optional<core::ControllableComponentId> active_tcp_robot_;
  bool active_tcp_offset_sync_{true};
  std::optional<core::ProgramBindingId> active_tcp_direct_binding_;
  /// A tool-context call that may have changed the
  /// controller without confirmation (`effect_possible`): the controller's TCP
  /// or payload may differ from the Active TCP. A component fault only the
  /// operator's Recover ends; the Enable after it re-syncs.
  std::optional<core::ErrorDTO> tool_context_fault_;
  ActiveTcpEventSink active_tcp_events_;
  std::uint64_t active_tcp_registration_generation_{};
  std::uint64_t active_tcp_sequence_{};
  /// The total payload the controller last confirmed (Enable or a
  /// payload update); a payload update with the same total sends nothing.
  std::optional<core::ToolContextPointMassDTO> last_tool_load_;
  /// Last controller-confirmed offset (the identity pose: no TCP), for the
  /// post-Apply re-sync.
  std::optional<core::Pose3DTO> last_tool_offset_;
  std::function<std::optional<std::string>()> process_fault_source_;
  std::function<core::Result<std::optional<std::string>>()>
      process_health_source_;
  std::function<bool()> launch_settled_;
  std::chrono::steady_clock::time_point launch_settle_bound_{};
  std::chrono::steady_clock::duration launch_settle_span_{};
  std::string process_diagnostic_incident_id_;
  std::shared_ptr<const content_identity::CanonicalValue>
      process_diagnostic_snapshot_;
  std::function<void(const core::ExecutionControllerSampleDTO &)>
      sample_observer_;
  rclcpp::TimerBase::SharedPtr sample_timer_;
  std::function<void(const sensor_msgs::msg::JointState &)>
      measured_state_observer_;
  rclcpp::SubscriptionBase::SharedPtr measured_state_subscription_;
  rclcpp::SubscriptionBase::SharedPtr display_joint_subscription_;
  std::shared_ptr<const DisplayJointSource> display_joint_source_;
  std::weak_ptr<RetainedObservedStateChannel> display_joint_channel_;
  /// A tool-context send that re-armed the robot, or may still, has not been
  /// followed by its rebind: the next admitted motion binds the reactivated
  /// controller first. Cleared by that rebind or by an Enable.
  bool rearm_rebind_required_{false};
  std::optional<std::pair<std::uint64_t, std::uint64_t>> last_observed_sample_;
  bool start_attempted_{false};
  std::exception_ptr start_failure_;
  std::optional<core::ControlDomainId> motion_domain_id_;
  std::optional<core::ControlDomainId> gripper_domain_id_;
  std::optional<std::string> gripper_id_;
  RetainedRos2ControlArmActivity gripper_activity_{};
  core::ComponentLifecycleStateDTO gripper_requested_{
      core::ComponentLifecycleStateDTO::kDeclared};
};

} // namespace manyforge::roboplan_backend
