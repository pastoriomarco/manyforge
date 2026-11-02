#pragma once

#include <limits>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/types/operation_runtime.hpp>

namespace manyforge::core::rule_codec_test {

inline IoMotionRuleOutcomeDTO Outcome(const OperationStateDTO& state,
                                      const RequestOwnerEpoch& owner) {
  const auto& p = state.participants.front();
  const InterruptSourceId source("program.blackboard");
  const IoSourceIncarnation incarnation("actual.bb.epoch");
  const IoSourceCursorDTO cursor{owner, UINT64_MAX, source, UINT64_MAX};
  IoRuleRegistrationDTO registration{
      owner, UINT64_MAX, UINT64_MAX, InterruptPolicyId("pick.complete"),
      IoOperationRuleScopeDTO{state.operation_id, state.operation_revision, p.participant_id,
                              p.controllable_component_id, p.command_target_id,
                              p.command_id, p.command_generation, UINT64_MAX,
                              ContentDigest("prepared.digest"),
                              MotionInterruptScopeId("scope.pick")}};
  const IoProgramBlackboardScopeDTO scope{owner,
                                          RequestOwnerEpoch("runner.owner"),
                                          7U,
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
  result.stop_id = StopId("stop.actual");
  result.stop_generation = StopGeneration(2U);
  result.stop_certificate_id = StopCertificateId("certificate.actual");
  result.stop_certificate_revision = StopCertificateRevision(3U);
  result.stop_result = StopCertificateResultDTO::kSucceededEscalated;
  result.terminal_cause = MotionExecutionTerminalCause::kSignalTrip;
  result.arbitration_disposition = IoRuleArbitrationDispositionDTO::kActive;
  return result;
}

inline OperationStateDTO State() {
  OperationParticipantStateDTO participant{OperationParticipantId("participant.1"),
                                           ControllerGoalId("goal.1"),
                                           ControllerGoalGeneration(1U),
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
  OperationStateDTO state{OperationId("operation.1"),
                          OperationRevision(1U),
                          ProgramBindingId("program.binding"),
                          ProgramRunId("run.1"),
                          OperationCoordinationPolicy::kIndependent,
                          OperationStateKind::kStopped,
                          {std::move(participant)},
                          INT64_MAX,
                          std::nullopt,
                          std::nullopt,
                          std::nullopt};
  state.participants.front().io_rule_outcomes.push_back(
      Outcome(state, RequestOwnerEpoch("owner.1")));
  return state;
}
}  // namespace manyforge::core::rule_codec_test
