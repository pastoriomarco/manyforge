#pragma once

#include <manyforge_core/types/io_activation_rule.hpp>
#include <manyforge_core/types/motion_preview.hpp>
#include <manyforge_core/runtime_collision_monitor.hpp>

#include <exception>

#include "execution_host_commands.hpp"
#include <manyforge_core/stop_state.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_roboplan/retained_gripper_position_limits.hpp>

namespace manyforge::roboplan_backend::host_detail {

[[nodiscard]] CanonicalValue ErrorValue(const core::ErrorDTO &error);

[[nodiscard]] CanonicalValue
ErrorResponse(std::string code, std::string message,
              std::map<std::string, std::string> details = {});

[[nodiscard]] core::ErrorDTO
ExecutionCommandExceptionError(const std::exception &error);

[[nodiscard]] CanonicalValue ExecutionOwnerNotReadyIdentityResponse(
    const core::ResourceAddressedExecutionKernel &owner);

[[nodiscard]] CanonicalValue PoseValue(const core::Pose3DTO &value);

[[nodiscard]] CanonicalValue
SceneRevisionValue(const core::SceneRevision &value);

[[nodiscard]] CanonicalValue MotionPathPreviewValue(
    const core::MotionPathPreview &path,
    const core::RuntimeParticipantBindingDTO &direct_control_binding,
    const std::string *diagnostic_reason = nullptr);

[[nodiscard]] CanonicalValue MotionPreviewReceiptValue(
    const core::MotionPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const core::RuntimeParticipantBindingDTO &direct_control_binding,
    const CanonicalValue &path_preview);

[[nodiscard]] CanonicalValue CapabilityCommandPreviewReceiptValue(
    const core::CapabilityCommandPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const std::optional<RetainedGripperPositionLimits> &limits);

[[nodiscard]] CanonicalValue
MotionSpecValue(const core::MotionSpecDTO &motion);

[[nodiscard]] CanonicalValue
ProjectionRevisionValue(const core::ProjectionRevisionDTO &value);

[[nodiscard]] CanonicalValue
PickCandidateValue(const core::PickPoseCandidateDTO &value);

[[nodiscard]] CanonicalValue
PickPlacePlanningResultValue(const std::string_view sequence_id,
                             const core::PickPlaceExecutionKindDTO kind,
                             const core::PickPlacePlanningResultDTO &value);

[[nodiscard]] std::string_view StateName(core::OperationStateKind state);

[[nodiscard]] CanonicalValue
AcceptedValue(const core::OperationAcceptedDTO &value);

[[nodiscard]] std::string_view ControlName(core::OperationControlKind control);

[[nodiscard]] CanonicalValue
ControlAcceptedValue(const core::OperationControlAcceptedDTO &value);

[[nodiscard]] CanonicalValue
StopAcceptedValue(const core::StopAcceptedDTO &value);

/// `null` for an absent event id, else the number.
[[nodiscard]] CanonicalValue
StopEventIdValue(const std::optional<core::StopEventId> &value);

/// Program execution scope `manyforge.program_execution_scope.v3`.
[[nodiscard]] CanonicalValue
ProgramExecutionScopeValue(const core::ProgramExecutionScopeStateDTO &value);

/// The event stream `state` member `program_run_hold` (K18): null without a
/// claimed run.
[[nodiscard]] CanonicalValue
ProgramRunHoldValue(const std::optional<core::ProgramRunHoldFactsDTO> &value);

/// Appends `program_run_hold`, `last_stop_event_id` and
/// `stop_progress_sequence` (K18) to a stream or `events` reply state.
void AppendStopStreamFacts(CanonicalValue::Object &state,
                           const core::StopStreamFactsDTO &facts);

/// The event stream `state` member `operations` (OD-205 (b), OD-213): the
/// Kernel's change counter, its compact faulted operations and every resident
/// operation as `read` projects it, from memory. No progress, no times and no
/// host log line (the `read` encoder keeps those). A resident operation whose
/// encoded state exceeds `kOperationTableMaximumResidentOperationBytes` is
/// listed with `operation: null`.
[[nodiscard]] CanonicalValue
OperationTableValue(const core::OperationTableDTO &table);

/// Program execution selection `manyforge.program_execution_selection.v3`.
[[nodiscard]] CanonicalValue ProgramSelectionAcceptedValue(
    const core::ProgramExecutionSelectionAcceptedDTO &value,
    const std::vector<std::string> &group_ids);

/// `read_stop` reply `manyforge.execution_stop_status.v2` (K16).
[[nodiscard]] CanonicalValue
StopEventStatusValue(const core::StopEventStatusDTO &value);

/// `cell_stop_action` of `manyforge.execution_control_state.v8` (K19).
[[nodiscard]] CanonicalValue
CellStopActionValue(const core::CellStopActionProjectionDTO &value);

/// The participant `stop` object of the operation state (§6.3.1).
[[nodiscard]] CanonicalValue
ParticipantStopObservationValue(const core::ParticipantStopObservationDTO &value);

[[nodiscard]] CanonicalValue
RecoveryAssessmentValue(const core::KernelRecoveryAssessmentDTO &value);

[[nodiscard]] CanonicalValue RecoveryParticipantsValue(
    const std::vector<core::OperationRecoveryParticipantDTO> &values);

[[nodiscard]] CanonicalValue
RecoveryActionResultValue(const core::KernelRecoveryActionResultDTO &value);

[[nodiscard]] CanonicalValue RuntimeScaleControlSelectorValue(
    const core::RuntimeScaleControlSelectorDTO &value);

[[nodiscard]] CanonicalValue RuntimeScaleControlAcceptedValue(
    const core::RuntimeScaleControlAcceptedDTO &value);

[[nodiscard]] CanonicalValue RuntimeScaleControlStatusValue(
    const core::RuntimeScaleControlStatusDTO &value);

[[nodiscard]] CanonicalValue
OperationStateValue(const core::OperationStateDTO &value);
/// `OperationStateValue` for the operations table: without the
/// participants' `progress`, the operation's `updated_at_ns` and the
/// collision log line.
[[nodiscard]] CanonicalValue
OperationTableStateValue(const core::OperationStateDTO &value);

/// A Program run number, or null for manual work.
[[nodiscard]] inline CanonicalValue
RunValue(const std::optional<core::ProgramRunNumber> &run) {
  return run ? CanonicalValue(static_cast<double>(run->value()))
             : CanonicalValue(nullptr);
}

/// Both existing events siblings derive from this one actual owner snapshot.
[[nodiscard]] CanonicalValue::Object InterruptRuleEventMetadataFields(
    const std::optional<core::IoRuleStateProjectionDTO> &actual);


[[nodiscard]] CanonicalValue
OperationEventPageValue(const core::OperationJournalEventPageDTO &page);

[[nodiscard]] CanonicalValue RuntimeFaultsValue(
    const core::ResourceAddressedExecutionKernel &owner);

/// OD-217: one robot's row of `status.runtime_collision_checks`: its check
/// monitor's counters since its engine was built, and the check period.
[[nodiscard]] CanonicalValue RuntimeCollisionChecksRowValue(
    std::string_view controllable_component_id,
    const core::RuntimeCollisionMonitorTimingMetrics &timing,
    core::Duration check_period_ns);

[[nodiscard]] CanonicalValue
CoordinatePositionValue(const core::CoordinatePositionDTO &position);
[[nodiscard]] CanonicalValue
IoSourceCursorValue(const core::IoSourceCursorDTO &value);

[[nodiscard]] CanonicalValue IoSignalObservationValue(
    const core::IoSignalObservationDTO& value);

/// Wire values (JSON, snake_case, `*_ns`).
[[nodiscard]] CanonicalValue FaultValue(const core::Fault& fault);
[[nodiscard]] CanonicalValue ActionResultValue(const core::ActionResult& result);
[[nodiscard]] CanonicalValue ComponentStopValue(const core::ComponentStop& stop);
[[nodiscard]] CanonicalValue ComponentStateValue(const core::ComponentState& state);
[[nodiscard]] CanonicalValue CellLatchValue(const core::CellLatch& latch);
/// The Kernel's `ReadRuleHolds()` rows as errors: `{code, message,
/// details: {rule_id}}`.
[[nodiscard]] CanonicalValue RuleHoldsValue(const std::vector<core::ErrorDTO>& holds);
/// A signal's wire id: `<controllable_component_id>/<signal_name>`.
[[nodiscard]] std::string SignalIdText(const core::IoSignalId& signal);
[[nodiscard]] std::optional<core::IoSignalId> ParseSignalIdText(std::string_view text);
/// `controller_owned` is present on outputs only; `age_ns`
/// only with `age` (the event stream's state compares samples without it).
[[nodiscard]] CanonicalValue SignalSampleValue(const core::SignalSample& sample,
                                               const core::IoSignalDescriptorDTO& descriptor,
                                               bool age = true);
[[nodiscard]] CanonicalValue IoWatchRegistrationValue(
    const core::IoWatchRegistrationDTO& value);
[[nodiscard]] CanonicalValue IoWatchResultValue(
    const core::IoWatchResultDTO& value);

} // namespace manyforge::roboplan_backend::host_detail
