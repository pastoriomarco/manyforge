#pragma once

#include <limits>
#include <manyforge_core/types/stop_event.hpp>
#include <manyforge_core/types/operation_runtime.hpp>

namespace manyforge::core::rule_codec_test {

inline IoMotionRuleOutcomeDTO Outcome(const OperationStateDTO& state) {
  const auto& p = state.participants.front();
  const InterruptSourceId source("program.blackboard");
  const IoSourceIncarnation incarnation("actual.bb.epoch");
  const IoSourceCursorDTO cursor{UINT64_MAX, source, UINT64_MAX};
  IoRuleRegistrationDTO registration{
      UINT64_MAX, UINT64_MAX, InterruptPolicyId("pick.complete"),
      IoOperationRuleScopeDTO{state.operation_id, state.operation_revision, p.participant_id,
                              p.controllable_component_id, p.command_target_id,
                              p.command_id, UINT64_MAX,
                              ContentDigest("prepared.digest"),
                              MotionInterruptScopeId("scope.pick")}};
  const IoProgramBlackboardScopeDTO scope{7U,
                                          CellAdmissionClaimGeneration(9U),
                                          state.program_run_id,
                                          state.program_binding_id,
                                          ActiveBundlePublicationGeneration(11U)};
  IoBlackboardTripEvidenceDTO bb{scope, source, incarnation, UINT64_MAX, state.updated_at_ns, {}};
  for (auto value : std::vector<IoBlackboardValueDTO>{
           {"bool", IoConditionValueKind::kBoolean, IoConditionValueDTO(false)},
           {"float", IoConditionValueKind::kFloat64, IoConditionValueDTO(0.25)},
           {"integer_high", IoConditionValueKind::kInt64, IoConditionValueDTO(INT64_MAX)},
           {"integer_low", IoConditionValueKind::kInt64, IoConditionValueDTO(INT64_MIN)},
           {"string", IoConditionValueKind::kString, IoConditionValueDTO(std::string(5000U, 's'))}})
    bb.values.push_back(std::make_shared<const IoBlackboardValueDTO>(std::move(value)));
  IoRuleTripEvidenceDTO trip{registration,
                             {},
                             {cursor},
                             state.updated_at_ns,
                             IoRuleAcquisitionKeyDTO{source, incarnation, UINT64_MAX, cursor},
                             bb};
  IoRuleSourceLossEvidenceDTO loss{
      registration,
      source,
      incarnation,
      cursor,
      IoRuleSourceFailureKind::kUnavailable,
      std::nullopt,
      std::nullopt,
      state.updated_at_ns,
      IoBlackboardSourceLossEvidenceDTO{scope, source, incarnation, UINT64_MAX, "string"}};
  IoMotionRuleOutcomeDTO result{registration,
                                {IoRuleRegistrationState::kRetired, trip, {loss}, std::nullopt}};
  result.stop_event_id = StopEventId(1000U);
  result.stop_result = StopEventResult::kStoppedEscalated;
  result.terminal_cause = MotionExecutionTerminalCause::kSignalTrip;
  result.arbitration_disposition = IoRuleArbitrationDispositionDTO::kActive;
  return result;
}

inline OperationStateDTO State() {
  OperationParticipantStateDTO participant{OperationParticipantId{OperationNumber(1U), 1U},
                                           ControllerGoalId{OperationNumber(1U), 1U, 1U},
                                           ControllableComponentId("robot.1"),
                                           CommandTargetId("joints"),
                                           CapabilityId("execution.trajectory.execute"),
                                           ControlDomainId("domain.1"),
                                           ControlDomainRevision("domain.revision"),
                                           ControlBindingId("binding.1"),
                                           ControlBindingRevision("binding.revision"),
                                           std::nullopt,
                                           std::nullopt,
                                           OperationParticipantPayloadKind::kMotion,
                                           OperationStateKind::kStopped,
                                           1.0,
                                           {ResourceId("joint.1")},
                                           std::nullopt};
  OperationStateDTO state{OperationNumber(1U),
                          OperationRevision(1U),
                          ProgramBindingId("program.binding"),
                          ProgramRunNumber(1U),
                          OperationCoordinationPolicy::kIndependent,
                          OperationStateKind::kStopped,
                          {std::move(participant)},
                          INT64_MAX,
                          std::nullopt,
                          std::nullopt};
  state.participants.front().io_rule_outcomes.push_back(
      Outcome(state));
  return state;
}
}  // namespace manyforge::core::rule_codec_test
