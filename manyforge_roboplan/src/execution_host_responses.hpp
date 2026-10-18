#pragma once

#include <manyforge_core/types/io_activation_rule.hpp>

#include "execution_host_commands.hpp"
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_roboplan/retained_gripper_position_limits.hpp>

namespace manyforge::roboplan_backend::host_detail {

[[nodiscard]] CanonicalValue ErrorValue(const core::ErrorDTO &error);

[[nodiscard]] CanonicalValue
ErrorResponse(std::string code, std::string message,
              std::map<std::string, std::string> details = {});

[[nodiscard]] CanonicalValue ExecutionOwnerNotReadyIdentityResponse(
    const core::ResourceAddressedExecutionKernel &owner,
    const core::RequestOwnerEpoch &owner_epoch);

[[nodiscard]] CanonicalValue PoseValue(const core::Pose3DTO &value);

[[nodiscard]] CanonicalValue
SceneRevisionValue(const core::SceneRevision &value);

[[nodiscard]] CanonicalValue MotionPreviewReceiptValue(
    const core::MotionPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const core::RuntimeParticipantBindingDTO &direct_control_binding,
    const core::RequestOwnerEpoch &owner_epoch);

[[nodiscard]] CanonicalValue CapabilityCommandPreviewReceiptValue(
    const core::CapabilityCommandPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const std::optional<RetainedGripperPositionLimits> &limits,
    const core::RequestOwnerEpoch &owner_epoch);

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

[[nodiscard]] std::string_view StopProjectionStateName(
    const core::StopDurableProjectionStateDTO state) noexcept;

[[nodiscard]] std::string_view
StopStatusStateName(const core::StopStatusStateDTO state) noexcept;

[[nodiscard]] std::string_view
StopFaultTriggerName(const core::StopFaultTriggerDTO trigger) noexcept;

[[nodiscard]] std::string_view
StopFaultLatchStateName(const core::StopFaultLatchStateDTO state) noexcept;

[[nodiscard]] std::string_view StopFaultRecoveryStatusName(
    const core::StopFaultRecoveryStatusDTO status) noexcept;

[[nodiscard]] std::string_view StopFaultRecoveryEvidenceKindName(
    const core::StopFaultRecoveryEvidenceKindDTO kind) noexcept;

[[nodiscard]] CanonicalValue
StopCertificateRefValue(const core::StopCertificateRefDTO &value);

[[nodiscard]] CanonicalValue
ProgramExecutionScopeValue(const core::ProgramExecutionScopeStateDTO &value);

[[nodiscard]] CanonicalValue ProgramSelectionAcceptedValue(
    const core::ProgramExecutionSelectionAcceptedDTO &value,
    const std::vector<std::string> &group_ids);

[[nodiscard]] CanonicalValue
StopStatusSummaryValue(const core::StopStatusDTO &value);

[[nodiscard]] CanonicalValue
StopProjectionStatusValue(const core::StopDurableProjectionStatusDTO &value);

[[nodiscard]] CanonicalValue
StopFaultLatchValue(const core::StopFaultLatchDTO &value);

[[nodiscard]] CanonicalValue
GlobalIoRuleRecoveryOfferValue(const core::IoGlobalRuleRecoveryOfferDTO &offer);
[[nodiscard]] CanonicalValue
GlobalIoRuleRecoveryResultValue(const core::IoGlobalRuleRecoveryResultDTO &result);

[[nodiscard]] CanonicalValue
StopFaultOfferValue(const core::StopFaultRecoveryOfferDTO &offer);

[[nodiscard]] CanonicalValue
StopFaultRecoveryResultValue(const core::StopFaultRecoveryResultDTO &value);

[[nodiscard]] std::string_view
StopCertificateResultName(core::StopCertificateResultDTO result) noexcept;

[[nodiscard]] std::string_view
StopStageOutcomeName(core::ParticipantStopStageOutcomeDTO outcome) noexcept;

[[nodiscard]] std::string_view StopAttemptDispositionName(
    const core::StopStageAttemptDispositionDTO disposition) noexcept;

[[nodiscard]] std::string_view
StopEvidenceStateName(const core::StopEvidenceItemStateDTO state) noexcept;

[[nodiscard]] std::string_view StopStageAuthorityName(
    const core::StopStageResultAuthorityDTO authority) noexcept;

[[nodiscard]] std::string_view StopParticipantSettlementName(
    const core::StopParticipantScopeFactsDTO::TerminalSettlementState
        settlement) noexcept;

[[nodiscard]] std::string_view
StopSelectorKindName(const core::StopRequestedSelectorKindDTO kind) noexcept;

[[nodiscard]] std::string_view
MotionInterruptStopModeName(const core::MotionInterruptStopMode mode) noexcept;

[[nodiscard]] CanonicalValue
StopHistoryJsonValue(const core::JsonValueDTO &value);

[[nodiscard]] CanonicalValue
StopHistoryStructuredValue(const core::InterruptStructuredValue &value);

[[nodiscard]] CanonicalValue
StopHistoryAccelerationValue(const core::CoordinateAccelerationDTO &value);

[[nodiscard]] CanonicalValue
StopHistoryVelocityValue(const core::CoordinateVelocityDTO &value);

[[nodiscard]] CanonicalValue
StopHistoryScopeKeyValue(const core::StopScopeEntryKeyDTO &key);

[[nodiscard]] CanonicalValue
StopHistoryScopeKeyArray(const std::vector<core::StopScopeEntryKeyDTO> &keys);

[[nodiscard]] CanonicalValue
StopHistoryProfileValue(const core::MotionInterruptStopProfileDTO &profile);

[[nodiscard]] CanonicalValue StopHistoryEvidenceSourceValue(
    const core::ResolvedStopEvidenceSourceDTO &source);

[[nodiscard]] CanonicalValue
StopHistoryEvidenceItemValue(const core::StopEvidenceItemDTO &item);

[[nodiscard]] CanonicalValue
StopHistoryAttemptValue(const core::StopAttemptedStageResultDTO &attempt,
                        const std::int64_t accepted_at_ns);

[[nodiscard]] CanonicalValue
StopHistorySettlementValue(const core::StopScopeEntryDTO &entry);

[[nodiscard]] CanonicalValue
StopHistoryFaultValue(const std::optional<core::StopFaultLatchDTO> &fault);

[[nodiscard]] CanonicalValue StopTerminalHistoryProjectionValue(
    const core::StopJournalTerminalProjectionDTO &projection,
    const std::string_view source_owner_epoch,
    const std::string_view durable_projection_digest,
    const bool include_details);

[[nodiscard]] CanonicalValue StopFaultClearHistoryProjectionValue(
    const core::StopJournalFaultClearProjectionDTO &projection,
    const std::string_view source_owner_epoch);

[[nodiscard]] CanonicalValue BoundedStopTerminalHistoryProjectionValue(
    const core::StopJournalTerminalProjectionDTO &projection,
    const std::string_view source_owner_epoch,
    const std::string_view durable_projection_digest);

[[nodiscard]] CanonicalValue
CellStopActionValue(const core::CellStopActionProjectionDTO &value);

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

/// Both existing events siblings derive from this one actual owner snapshot.
[[nodiscard]] CanonicalValue::Object InterruptRuleEventMetadataFields(
    const std::optional<core::IoRuleStateProjectionDTO> &actual,
    const core::RequestOwnerEpoch &owner_epoch);


[[nodiscard]] CanonicalValue
OperationEventPageValue(const core::OperationJournalEventPageDTO &page);

[[nodiscard]] CanonicalValue RuntimeFaultsValue(
    const core::ResourceAddressedExecutionKernel &owner);

[[nodiscard]] CanonicalValue
CoordinatePositionValue(const core::CoordinatePositionDTO &position);
[[nodiscard]] CanonicalValue
IoSourceCursorValue(const core::IoSourceCursorDTO &value);

[[nodiscard]] CanonicalValue IoSignalObservationValue(
    const core::IoSignalObservationDTO& value);
[[nodiscard]] CanonicalValue IoWatchRegistrationValue(
    const core::IoWatchRegistrationDTO& value);
[[nodiscard]] CanonicalValue IoWatchResultValue(
    const core::IoWatchResultDTO& value);

} // namespace manyforge::roboplan_backend::host_detail
