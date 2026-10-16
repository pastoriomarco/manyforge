#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/configured_robot_measured_state.hpp>
#include <manyforge_core/execution_port_participant.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <manyforge_roboplan/controller_launch_artifacts.hpp>
#include <manyforge_roboplan/retained_externally_managed_lifecycle_ports.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <manyforge_roboplan/retained_ros2_control_statistics_monitor.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#include <manyforge_ros2_control_execution/ros2_gripper_command_backend.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

namespace manyforge::roboplan_backend {

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

  friend bool operator==(const RetainedRos2ControlArmActivity &,
                         const RetainedRos2ControlArmActivity &) = default;
};

/// Outcome of one CCLC controller switch primitive.
enum class RetainedRos2ControlSwitchOutcome {
  /// Nothing was sent (refused before effect); see the error.
  kNotSent,
  /// The controller manager answered `ok`.
  kSucceeded,
  /// The controller manager answered not `ok`.
  kFailed,
  /// Sent, but cancelled before an answer arrived.
  kUncertain,
};

struct RetainedRos2ControlSwitchResult final {
  RetainedRos2ControlSwitchOutcome outcome{
      RetainedRos2ControlSwitchOutcome::kNotSent};
  std::optional<core::ErrorDTO> error{};
};

enum class MockResetPositionPhase {
  kSwitchingToWriter,
  kSettingState,
  kSwitchingToJtc,
  kContaining,
};

/// Mock has no stock writer service ACK. One fresh exact GenericSystem
/// loopback sample acknowledges consumption, without a convergence/dwell wait.
/// DX1093: the fault bound is the stock ros2_control 6.9.0 CM switch default
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

using MockResetPositionPhaseCommit = std::function<core::Result<void>()>;
using MockResetPositionPhaseCallback = std::function<core::Result<void>(
    const MockResetPositionTransition &, const MockResetPositionPhaseCommit &)>;

/// Host sequencing of the arm controller around a lane-specific guard (the
/// Isaac command-age guard, design §4.3, Q41, Q43). Every member is required.
struct RetainedRos2ControlArmGuardHooks final {
  /// Q43: every motion path of the arm (goals and replacements) is admitted
  /// only while this holds.
  std::function<bool()> motion_admitted;
  /// Run before every CCLC activation; an error refuses it before effect.
  std::function<core::Result<void>()> before_activation;
  /// The operator's Recover of the arm (Q41: the Reset after a trip).
  std::function<void()> on_recover;
  /// A guard fault awaiting the operator (reported as a `fault` cause).
  std::function<std::optional<std::string>()> fault;
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
  /// Supplied only from the installed Mock graph's generated artifacts.
  std::optional<MockResetPositionTarget> mock_reset_position{};
};

/// Retained ROS resource lifetime of one runtime composition: the shared
/// transport, the arm's JTC adapter (one instance per binding epoch), the Core
/// participant over it, the gripper adapter and the arm controller's
/// lifecycle facts.
///
/// Binding epochs (design §3.1): the first adapter instance carries epoch 1.
/// Before every CCLC activation that follows an earlier activation the handle
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
/// after this host's CCLC activation succeeded on the current epoch and until
/// its CCLC deactivation.
class RetainedRos2ControlRuntimeHandle final
    : public std::enable_shared_from_this<RetainedRos2ControlRuntimeHandle> {
public:
  using ParticipantFactory =
      std::function<std::shared_ptr<core::ExecutionPortParticipant>(
          std::shared_ptr<core::IExecutionPort>)>;

  RetainedRos2ControlRuntimeHandle() = default;
  ~RetainedRos2ControlRuntimeHandle();

  RetainedRos2ControlRuntimeHandle(const RetainedRos2ControlRuntimeHandle &) =
      delete;
  RetainedRos2ControlRuntimeHandle &
  operator=(const RetainedRos2ControlRuntimeHandle &) = delete;

  /// Binds once: constructs the initial adapter, the participant over it and
  /// takes the gripper adapter. Fails closed (everything shut down) on any
  /// mismatch. Selected bring-up consumes the exact owner-computed successor;
  /// absent seed preserves epoch 1 and the ordinary first observation.
  /// An optional genuine clock owner must name this same receiver clock and
  /// remains alive through child and transport teardown.
  [[nodiscard]] core::Result<void> Bind(
      std::unique_ptr<ros2_control_execution::Ros2TransportResources> resources,
      core::IClock &clock, RetainedRos2ControlArmTransport arm,
      const ParticipantFactory &make_participant,
      std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend>
          gripper,
      const core::RuntimeParticipantBinding &motion_binding,
      const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
      const std::optional<core::SelectedControllerOccurrenceDTO>
          &successor_occurrence = std::nullopt,
      std::shared_ptr<core::IClock> clock_owner = {});

  /// Bind a gripper-only component to its actual transport and controller.
  /// No arm participant, execution binding epoch or motion adapter is created.
  [[nodiscard]] core::Result<void> BindGripperOnly(
      std::unique_ptr<ros2_control_execution::Ros2TransportResources> resources,
      core::IClock& clock,
      std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend> gripper,
      const core::RuntimeParticipantBinding& binding,
      RetainedRos2ControlArmController controller,
      const std::optional<core::SelectedControllerOccurrenceDTO>& successor_occurrence = {},
      std::shared_ptr<core::IClock> clock_owner = {},
      std::optional<RetainedMeasuredJointStateLane> measured_lane = {});

  /// Exact owned arm epoch and canonical domain observation floors, including
  /// zero before an ordinary first observation. Unbound handles have no
  /// domains.
  [[nodiscard]] core::SelectedControllerOccurrenceDTO
  ReadOwnedControllerOccurrence() const;

  [[nodiscard]] std::shared_ptr<core::ExecutionPortParticipant>
  participant() const;

  /// The current binding epoch's port as the participant sees it.
  [[nodiscard]] std::shared_ptr<core::IExecutionPort> current_port() const;

  /// The arm's lane facts in the controller's own joint names (the guard
  /// status joint-name check).
  [[nodiscard]] std::vector<std::string> controller_joint_names() const;

  /// The shared transport node (CCLC switch client, guard binding).
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
  [[nodiscard]] core::Result<std::shared_ptr<PreparedMeasuredStateSources>>
  StageMeasuredStateSources(
      std::vector<core::MeasuredStateSourceDescriptor> descriptors,
      std::shared_ptr<core::IClock> receiver_clock,
      const core::RuntimeParticipantBinding &candidate_motion,
      const std::optional<core::RuntimeParticipantBinding> &candidate_gripper);

  [[nodiscard]] core::Result<std::shared_ptr<PreparedMeasuredStateSources>>
  StageMeasuredStateSources(
      std::vector<core::MeasuredStateSourceDescriptor> descriptors,
      std::shared_ptr<core::IClock> receiver_clock,
      const std::optional<core::RuntimeParticipantBinding>& candidate_motion,
      const std::optional<core::RuntimeParticipantBinding>& candidate_gripper);

  /// Installs the lane guard's hooks; only before `StartBackend`.
  void InstallGuardHooks(RetainedRos2ControlArmGuardHooks hooks);

  /// Q80: the component's Manager-owned controller launch. Once the source
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
  /// statistics monitor (Q54), then starts the executor. A partial-start
  /// exception is retained; observing it never retries startup.
  void StartBackend();

  /// CCLC Prepare primitive: no controller-manager effect; the loaded
  /// (inactive) controller on active hardware is the prepared state.
  [[nodiscard]] RetainedRos2ControlSwitchResult PrepareArmController();

  /// CCLC Enable primitive: activate the arm's trajectory controller
  /// (`switch_controller`, strict). Waits for the answer until `cancellation`.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  ActivateArmController(std::stop_token cancellation);

  /// CCLC Disable primitive: deactivate it. Known success without a request
  /// when the controller manager already reports it inactive.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  DeactivateArmController(std::stop_token cancellation);

  /// CCLC Recover primitive (the operator's Reset of an incident, Q41): the
  /// guard hook first, then the controller is left inactive and prepared; the
  /// target driver's next Enable activates it on a new binding epoch.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  RecoverArmController(std::stop_token cancellation);

  /// The guard trip's containment (Q41): deactivate the controller through
  /// the same switch path CCLC uses, without changing the requested state.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  DeactivateArmControllerForFault(std::stop_token cancellation);

  [[nodiscard]] std::optional<MockResetPositionTarget>
  mock_reset_position_target() const;
  [[nodiscard]] bool mock_reset_writer_inactive() const;
  [[nodiscard]] bool mock_reset_writer_active() const;
  [[nodiscard]] bool mock_reset_successor_ready() const;
  [[nodiscard]] std::shared_ptr<RetainedObservationWake>
  mock_reset_observation_wake() const noexcept;
  /// Compares evidence minted from the supplied observation's own raw inputs;
  /// never substitutes a later healthy read for a faulted observation.
  [[nodiscard]] bool IsExpectedMockPositionResetTransition(
      const core::BrandLifecycleObservationDTO &observation,
      const MockResetPositionTransition &transition) const;
  /// Owner-private transport primitive. The CCLC owner pins the exact Reset
  /// claim/product before calling; callbacks run without a handle state lock.
  /// The callback invokes its nonblocking local commit under the existing
  /// driver run mutex, atomically advancing Core/handle phase before waits.
  /// A switching callback runs before send, then again only after positive
  /// ACK; that second callback waits for exact CM destination observation.
  /// SettingState(target_published=true) proves one fresh exact loopback ACK
  /// and publishes/projects backend command before returning Ok. It performs
  /// no measured velocity, reference, convergence or dwell verification.
  /// No caller-supplied coordinates or blind rollback.
  [[nodiscard]] RetainedRos2ControlSwitchResult
  ResetMockPosition(std::stop_token cancellation,
                    const MockResetPositionPhaseCallback &phase);

  [[nodiscard]] RetainedExternallyManagedDomainSnapshotSource
  lifecycle_snapshot_source();

  /// CCLC primitives of the ros2_control catalog for the bound domains.
  [[nodiscard]] RetainedExternallyManagedPrimitiveDispatcher
  lifecycle_primitive_dispatcher();

  [[nodiscard]] core::Result<RetainedExternallyManagedDomainSnapshot>
  ReadLifecycleSnapshot(const core::ControlDomainId &control_domain_id,
                        core::TimePoint now_ns);

  /// Pure projection of the same continuously maintained health inputs.
  /// No lifecycle generation/signature/intervention/discovery commit.
  [[nodiscard]] core::Result<core::LifecycleDomainHealthDTO>
  ReadLifecycleHealth(const core::ControlDomainId &control_domain_id,
                      core::TimePoint now_ns) const;

  [[nodiscard]] RetainedRos2ControlArmActivity arm_activity() const;

  /// Read-only transport diagnostics for the exactly bound gripper target.
  [[nodiscard]] std::optional<
      ros2_control_execution::Ros2GripperTransportReadinessSnapshot>
  gripper_transport_readiness_snapshot() const;

  /// The arm's motion control domain, if bound.
  [[nodiscard]] std::optional<core::ControlDomainId> motion_domain() const;

  /// Q54 (monitor only): the arm controller's statistics as observed from
  /// the controller manager since StartBackend (absent before it).
  [[nodiscard]] std::optional<RetainedControllerStatisticsSnapshot>
  controller_statistics() const;

private:
  friend class RetainedRos2ControlRuntimeHandleTestAccess;
  struct ArmState;
  struct GripperOnlyState;
  struct LifecycleHealthInputs;
  struct LifecycleClassification;
  class ArmPort;

  [[nodiscard]] core::Result<LifecycleHealthInputs>
  CaptureLifecycleHealthInputs(const core::ControlDomainId &domain,
                               core::TimePoint now_ns) const;
  static void ReadLifecycleMonitorFacts(LifecycleHealthInputs &inputs);
  [[nodiscard]] static LifecycleClassification
  ComputeLifecycleHealth(const LifecycleHealthInputs &inputs);
  [[nodiscard]] core::Result<RetainedExternallyManagedDomainSnapshot>
  CommitLifecycleObservation(const core::ControlDomainId &domain,
                             LifecycleHealthInputs inputs,
                             LifecycleClassification classification);

  void ObserveActivity(
      RetainedRos2ControlArmActivity activity,
      std::optional<RetainedRos2ControlArmActivity> gripper_activity = {});
  /// Graph refresh stays on the existing NRT Observe/observer path; health
  /// only consumes the maintained endpoint facts.
  void RefreshEndpointFacts();
  [[nodiscard]] std::optional<core::ErrorDTO> ReadEndpointDiagnostic() const;
  [[nodiscard]] RetainedRos2ControlSwitchResult PrepareGripperController();
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SwitchGripperController(bool activate, std::stop_token cancellation);
  void PublishReadinessLocked();
  [[nodiscard]] core::Result<void> RotateEpochLocked();
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SendSwitch(bool activate, std::stop_token cancellation,
             std::optional<std::string> selected_controller = {});
  [[nodiscard]] RetainedRos2ControlSwitchResult
  SendControllerSwitch(std::vector<std::string> activate,
                       std::vector<std::string> deactivate,
                       std::stop_token cancellation,
                       std::optional<core::Duration> response_timeout_ns = {});

  struct DomainGenerationState final {
    std::uint64_t publication_generation{};
    bool first_observation_pending{true};
    std::string source_signature;
    core::LifecycleBlockingCauseClassDTO last_intervention_cause{
        core::LifecycleBlockingCauseClassDTO::kNone};
    bool exact_configuration_seen{false};
  };

  mutable std::mutex mutex_;
  /// Serializes CCLC switch primitives (one controller switch at a time).
  std::mutex switch_mutex_;
  const std::shared_ptr<RetainedObservationWake> reset_observation_wake_{
      std::make_shared<RetainedObservationWake>()};
  // Declared before dependent resources: destroyed after their teardown.
  std::shared_ptr<core::IClock> clock_owner_;
  std::unique_ptr<ros2_control_execution::Ros2TransportResources> resources_;
  core::IClock *clock_{};
  std::optional<RetainedRos2ControlArmTransport> arm_transport_;
  std::shared_ptr<ArmState> arm_;
  std::shared_ptr<GripperOnlyState> gripper_only_;
  std::shared_ptr<core::ExecutionPortParticipant> participant_;
  std::unique_ptr<ros2_control_execution::Ros2GripperCommandBackend> gripper_;
  std::optional<core::RuntimeParticipantBinding> motion_binding_;
  std::optional<core::RuntimeParticipantBinding> gripper_binding_;
  std::weak_ptr<RetainedObservedStateChannel> gripper_display_channel_;
  std::optional<RetainedRos2ControlArmGuardHooks> guard_hooks_;
  std::function<std::optional<std::string>()> process_fault_source_;
  std::function<core::Result<std::optional<std::string>>()>
      process_health_source_;
  std::string process_diagnostic_incident_id_;
  std::shared_ptr<const content_identity::CanonicalValue>
      process_diagnostic_snapshot_;
  std::function<void(const core::ExecutionControllerSampleDTO &)>
      sample_observer_;
  rclcpp::TimerBase::SharedPtr sample_timer_;
  std::optional<std::pair<std::uint64_t, std::uint64_t>> last_observed_sample_;
  bool start_attempted_{false};
  std::exception_ptr start_failure_;
  std::optional<core::ControlDomainId> motion_domain_id_;
  std::optional<core::ControlDomainId> gripper_domain_id_;
  std::optional<std::string> gripper_id_;
  RetainedRos2ControlArmActivity gripper_activity_{};
  core::ComponentLifecycleStateDTO gripper_requested_{
      core::ComponentLifecycleStateDTO::kDeclared};
  std::map<core::ControlDomainId, DomainGenerationState> domain_generations_;
};

} // namespace manyforge::roboplan_backend
