#pragma once

#include "execution_host_config.hpp"
#include <array>
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/types/io_blackboard.hpp>
#include <manyforge_core/types/io_rule_recovery.hpp>
#include <manyforge_core/types/program_execution_control.hpp>
#include <manyforge_core/types/program_io.hpp>
#include <manyforge_core/types/program_mock_simulation.hpp>
#include <manyforge_roboplan/program_reset_position.hpp>

namespace manyforge::roboplan_backend::host_detail {

void RequireExactObjectSize(const CanonicalValue &value, std::size_t size,
                            std::string_view label);

const CanonicalValue &Member(const CanonicalValue &root,
                             std::string_view name);

std::string Text(const CanonicalValue &root, std::string_view name,
                 bool allow_empty = false);

double Number(const CanonicalValue &root, std::string_view name);

bool Boolean(const CanonicalValue &root, std::string_view name);

std::uint64_t Counter(const CanonicalValue &root, std::string_view name,
                      std::uint64_t minimum);

std::optional<double> OptionalNumber(const CanonicalValue &root,
                                     std::string_view name);

std::optional<std::uint64_t> OptionalCounter(const CanonicalValue &root,
                                             const std::string_view name,
                                             const std::uint64_t minimum);

std::optional<std::string> OptionalText(const CanonicalValue &root,
                                        std::string_view name);

std::optional<std::string> OptionalBoundedText(const CanonicalValue &root,
                                               std::string_view name,
                                               std::size_t maximum_bytes);

const CanonicalValue::Array &Array(const CanonicalValue &root,
                                   std::string_view name);

std::vector<std::string> TextArray(const CanonicalValue &root,
                                   std::string_view name, std::size_t maximum);

std::vector<double> NumberArray(const CanonicalValue &root,
                                std::string_view name, std::size_t maximum);

[[nodiscard]] core::Pose3DTO DecodePose(const CanonicalValue &value);

/// Decode one portable reference. `frame_role` may carry an exact robot
/// qualifier `target` ({target_role_id, controllable_component_id,
/// command_target_id}); its Program-Binding agreement is checked at admission
/// by `VerifyReferenceTargets`, never inferred here.
[[nodiscard]] core::ReferenceDTO
DecodeReference(const CanonicalValue &value, const bool allow_robot_target);

[[nodiscard]] std::optional<core::ReferenceDTO>
DecodeOptionalReference(const CanonicalValue &root, const std::string_view name,
                        const bool allow_robot_target = false);

[[nodiscard]] core::MotionSpecDTO
DecodeMotionSpec(const CanonicalValue &value);

[[nodiscard]] core::SceneRevision
DecodeSceneRevision(const CanonicalValue &value);

[[nodiscard]] core::ActionSpanDTO
DecodeActionSpan(const CanonicalValue &value);

[[nodiscard]] core::PublicRequestContextDTO
DecodeRequestContext(const CanonicalValue &value);

[[nodiscard]] core::RequestAttributionDTO
DecodeRequestAttribution(const CanonicalValue &value);

[[nodiscard]] core::JointDynamicsLimitDTO
DecodeDynamicsLimit(const CanonicalValue &value);

[[nodiscard]] core::MotionParticipantIntentDTO
DecodeMotionPayload(const CanonicalValue &value);

[[nodiscard]] core::CoordinatePositionDTO
DecodeCoordinatePosition(const CanonicalValue &value);

[[nodiscard]] core::CoordinateEffortDTO
DecodeCoordinateEffort(const CanonicalValue &value);

[[nodiscard]] core::CapabilityCommandIntentDTO
DecodeCapabilityCommandPayload(const CanonicalValue &value);

[[nodiscard]] core::OperationParticipantPayloadDTO
DecodeParticipantPayload(const CanonicalValue &value);

[[nodiscard]] std::optional<core::ProgramNodeExecutionScopeDTO>
DecodeProgramExecutionScope(const CanonicalValue &value,
                            const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::OperationIntentDTO
DecodeOperation(const CanonicalValue &value,
                const core::RequestAttributionDTO &attribution);

[[nodiscard]] std::array<double, 3U> DecodeTriple(const CanonicalValue &root,
                                                  const std::string_view name);

[[nodiscard]] core::PickOffsetEnvelopeDTO
DecodePickEnvelope(const CanonicalValue &value);

[[nodiscard]] core::PickCandidateSynthesisIntentDTO
DecodePickSynthesis(const CanonicalValue &value);

[[nodiscard]] core::PickSequencePlanningIntentDTO
DecodePickPlanningIntent(const CanonicalValue &value,
                         const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::PlaceSequencePlanningIntentDTO
DecodePlacePlanningIntent(const CanonicalValue &value,
                          const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::OperationControlRequestDTO
DecodeOperationControlRequest(const CanonicalValue &value,
                              const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::StopIntentDTO
DecodeStopIntent(const CanonicalValue &value,
                 const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::StopIntentDTO
DecodeProgramRunStopIntent(const CanonicalValue &value,
                           const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::FullStopIntentDTO
DecodeFullStopIntent(const CanonicalValue &value,
                     const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::StopFaultRecoveryIntentDTO
DecodeStopFaultRecoveryIntent(const CanonicalValue &value);

[[nodiscard]] core::RuntimeScaleControlSelectorDTO
DecodeRuntimeScaleControlSelector(const CanonicalValue &value);

[[nodiscard]] core::RuntimeScaleControlRequestDTO
DecodeRuntimeScaleControlRequest(
    const CanonicalValue &value,
    const core::RequestAttributionDTO &attribution);

[[nodiscard]] core::RecoveryAction
DecodeRecoveryAction(const std::string_view value);

[[nodiscard]] core::KernelRecoveryActionRequestDTO
DecodeOperationRecoveryRequest(const CanonicalValue &value);

enum class CommandKind {
  kStatus,
  kIdentity,
  kResetRobotPositionCapability,
  kResetRobotPosition,
  kTargets,
  kQueryTarget,
  kStartMockIoNodeSimulation,
  kEndMockIoNodeSimulation,
  kSetMockIoRunPaused,
  kRegisterIoWatch,
  kReadIoWatch,
  kReleaseIoWatch,
  kSetMockIoInput,
  kSetCarriedObjectLost,
  kPublishProgramBlackboard,
  kQueryOperation,
  kControlState,
  kObservation,
  kEvents,
  kPreviewMotion,
  kReadManualHandover,
  kPreviewCapabilityCommand,
  kRegisterProgramCapture,
  kSubmit,
  kRead,
  kReadStop,
  kGetOperationRecovery,
  kApplyOperationRecovery,
  kReset,
  kAcknowledgeCompleted,
  kStop,
  kStopProgramRun,
  kStopProgramSelection,
  kReadProgramExecutionScope,
  kReadVolatileMotionTrace,
  kStartProgramSelection,
  kReleaseProgramSelection,
  kCheckProgramNodeMotionAttention,
  kReleaseProgramRunStopFence,
  kFullStop,
  kMalformedStop,
  kGetStopFault,
  kRecoverStopFault,
  kSetRuntimeScaleControl,
  kSetOperationRuntimeScale,
  kGetRuntimeScaleControl,
  kPlanPick,
  kPlanPlace,
  kForceClose,
  /// Active TCP (M7): every robot's Active TCP (the Runner's baseline).
  kReadActiveTcp,
  kResolveAttachActiveTcp,
  /// A read queued behind earlier enqueued submissions on this host.
  kReadActiveTcpOrdered,
  /// Active TCP (M7, T-MQ3, T-MQ9): reset every robot to its End-effector
  /// TCP (Program Reset with reload, `simulation.reset`, world reload).
  kResetActiveTcp,
};

enum class CommandLane {
  kOrdinary,
  kStop,
  kObservation,
};

/// Commands whose addressed-Kernel entry takes its submission ordering. A
/// motion-preview session (and pick/place planning built on one) retains that
/// ordering for its whole planning transaction, so these commands run on the
/// submission-ordered lane, FIFO in host enqueue order, and never occupy the
/// ordinary lane that serves Stop status, Reset, events, cell admission and
/// operator-alarm attention.
/// Shared closed row decoder, also used after normalizing actual sealed
/// Program authoring omission/order. This never resolves/authorizes inputs.
[[nodiscard]] std::vector<core::ProgramMockSimulationSignalDTO>
DecodeProgramMockSimulationSignals(const CanonicalValue& rows);

[[nodiscard]] bool IsSubmissionOrdered(const CommandKind kind) noexcept;

/// Ordinary projections that first read the live Scene revision from Manager.
/// That round trip waits on Manager (for example during an Apply), so it runs
/// on the Scene-read lane and never occupies the ordinary lane.
[[nodiscard]] bool ReadsManagerScene(const CommandKind kind) noexcept;

/// In-memory Stop-progression reads of the stable Kernel. They are served
/// beside Stop on the independent control socket, never on the ordinary one.
[[nodiscard]] bool IsStopLaneRead(const CommandKind kind) noexcept;

/// Every Stop form preempts advisory planning before it is dispatched: a
/// preview is effect-free and re-requestable, and containment never waits
/// for one. An accepted Reset preempts it after acceptance (a refused Reset
/// changed nothing a preview depends on).
[[nodiscard]] bool PreemptsPlanning(const CommandKind kind) noexcept;

/// A component-lifecycle claim is a whole-cell configuration mutation: the
/// Kernel serializes it with the submission lock a preview session retains
/// for its whole planning time. Like Stop, it preempts advisory planning
/// before it is dispatched instead of waiting behind it. Other claim kinds
/// never take that lock.
[[nodiscard]] bool PreemptsPlanning(
    const component_manager::detail::KernelCellAdmissionRpcPacket
        &packet) noexcept;

struct MalformedStopCommand final {
  core::RequestId request_id;
  core::RequestAttributionDTO attribution;
  std::string recognized_route;
  std::string reason;
};

/// `frame_transform`: express `pose` (given in `source_frame`) in
/// `target_frame` through the cell frame resolver. The canonical request
/// members are retained verbatim for the response echo.
struct FrameTransformQuery final {
  core::Pose3DTO pose;
  core::ReferenceDTO source_frame;
  core::ReferenceDTO target_frame;
  CanonicalValue pose_value;
  CanonicalValue source_frame_value;
  CanonicalValue target_frame_value;
};

struct TargetObservationQuery final {
  core::RequestOwnerEpoch owner_epoch;
  core::ProgramBindingId program_binding_id;
  core::TargetRoleId target_role_id;
  std::optional<core::FrameRoleId> frame_role_id;
  std::optional<core::FrameRoleId> relative_to_frame_role_id;
  std::optional<FrameTransformQuery> frame_transform{};
  std::optional<core::ResourceRoleId> io_signal_role{};
};

struct MockIoInputSetRequest final {
  TargetObservationQuery query;
  core::IoSourceIncarnation expected_source_epoch;
  bool value;
};

/// Manager publication of one gripper's `carried_object_lost.<target>`
/// system signal level (contract §8.4, §10). The ids and the loss event id
/// are present iff `level`; `set_key` (the attachment's stored
/// `on_grasp_lost.set_key`) is null with `false`.
struct CarriedObjectLostSetRequest final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  bool level{};
  std::optional<std::string> carried_object_attachment_id;
  std::optional<std::string> scene_resource_id;
  std::optional<std::string> loss_event_id;
  std::optional<std::string> set_key;
};

struct ProgramOperationQuery final {
  core::RequestOwnerEpoch owner_epoch;
  core::ProgramBindingId program_binding_id;
  core::ProgramRunId program_run_id;
  core::TargetRoleId target_role_id;
};

struct Command final {
  explicit Command(CommandKind command_kind) : kind(command_kind) {}

  CommandKind kind;
  std::optional<TargetObservationQuery> target_query;
  std::optional<core::ProgramBindingId> attach_binding_id;
  std::optional<core::TargetRoleId> attach_holder_role;
  std::optional<MockIoInputSetRequest> mock_io_input_set{};
  std::optional<CarriedObjectLostSetRequest> carried_object_lost_set{};
  std::optional<core::ProgramNodeMockSimulationRequestDTO> mock_node_simulation{};
  std::optional<core::ProgramNodeMockSimulationScopeDTO> mock_node_simulation_end{};
  std::optional<core::ProgramRunMockSimulationPauseRequestDTO> mock_run_simulation_pause{};
  std::optional<core::ProgramIoWatchRequestDTO> io_watch_request{};
  std::optional<core::IoWatchRegistrationDTO> io_watch_registration{};
  std::optional<core::ProgramIoWatchScopeReleaseRequestDTO>
      io_watch_scope_release{};
  bool io_watch_condition_unsupported{};
  std::optional<core::IoProgramBlackboardPublicationDTO>
      blackboard_publication{};
  std::optional<ProgramOperationQuery> operation_query;
  std::optional<core::OperationIntentDTO> operation;
  std::optional<core::ProgramRunExecutionCaptureDTO> program_capture;
  std::optional<core::ProgramExecutionSelectionRequestDTO> program_selection;
  std::optional<core::ProgramNodeMotionAttentionCheckDTO> program_node_attention;
  std::optional<ProgramResetPositionRequest> reset_robot_position;
  std::uint64_t expected_selection_revision{};
  std::optional<core::ContentDigest> operation_digest;
  std::optional<core::OperationId> operation_id;
  std::optional<core::StopId> stop_id;
  std::optional<core::ProgramRunId> program_run_id;
  std::optional<core::StopGeneration> stop_generation;
  std::optional<core::OperationControlRequestDTO> control;
  std::optional<core::OperationRevision> expected_operation_revision;
  std::optional<core::KernelRecoveryActionRequestDTO> operation_recovery;
  std::optional<core::StopIntentDTO> stop;
  std::optional<core::FullStopIntentDTO> full_stop;
  std::optional<MalformedStopCommand> malformed_stop;
  std::optional<core::StopFaultRecoveryIntentDTO> stop_fault_recovery;
  std::optional<core::IoGlobalRuleRecoveryIntentDTO> global_io_rule_recovery{};
  std::optional<core::RuntimeScaleControlRequestDTO> runtime_scale_control;
  std::optional<core::RuntimeScaleRequestDTO> operation_runtime_scale;
  std::optional<core::RuntimeScaleControlSelectorDTO>
      runtime_scale_control_selector;
  std::optional<core::RequestOwnerEpoch> event_owner_epoch{};
  std::uint64_t after_sequence{};
  std::size_t event_limit{};
  std::size_t trace_allowance_bytes{};
  std::optional<core::RequestOwnerEpoch> observation_after_owner_epoch;
  std::optional<std::string> observation_after_source_epoch;
  std::optional<std::uint64_t> observation_after_source_sequence;
  bool observation_cursor_supplied{};
  std::optional<core::PickSequencePlanningIntentDTO> pick_planning;
  std::optional<core::PlaceSequencePlanningIntentDTO> place_planning;
  /// Item 37 Force close: `arm` (with `stop_generation`) or `close_operations`.
  std::optional<std::string> force_close_step{};
};

[[nodiscard]] Command DecodeCommand(std::string_view bytes);

[[nodiscard]] core::ProjectionRevisionDTO
DecodeProjectionRevision(const CanonicalValue &value);

[[nodiscard]] core::ErrorDTO DecodeManagerError(const CanonicalValue &value);

[[nodiscard]] CanonicalValue CanonicalCodecValue(const std::string_view bytes,
                                                 const std::string_view label);

[[nodiscard]] core::RequestReplayStatusDTO
DecodeRequestReplayStatus(const CanonicalValue &root,
                          const std::string_view name);

[[nodiscard]] core::SceneProjectionCommitStatusDTO
DecodeSceneProjectionCommitStatus(const CanonicalValue &root,
                                  const std::string_view name);

[[nodiscard]] core::SceneUpdateStatusDTO
DecodeSceneUpdateStatus(const CanonicalValue &value);
} // namespace manyforge::roboplan_backend::host_detail
