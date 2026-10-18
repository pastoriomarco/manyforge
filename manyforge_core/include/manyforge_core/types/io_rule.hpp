#pragma once

#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/io_blackboard.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <memory>
#include <variant>

namespace manyforge::core {

class CheckedIoCondition;

/// Minted only by the existing operation/participant owner. This module checks
/// identity shape; Kernel must revalidate membership before releasing dispatch
/// and before accepting an eventual response. This is not physical authority.
struct IoOperationRuleScopeDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  std::uint64_t preparation_id;
  ContentDigest prepared_contract_digest;
  MotionInterruptScopeId motion_interrupt_scope_id;
  friend bool operator==(const IoOperationRuleScopeDTO&, const IoOperationRuleScopeDTO&) = default;
};

/// Activation lifetime is independent of a Program Run and its wait watches.
struct IoActivationRuleScopeDTO final {
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  friend bool operator==(const IoActivationRuleScopeDTO&,
                         const IoActivationRuleScopeDTO&) = default;
};

using IoRuleScopeDTO = std::variant<IoOperationRuleScopeDTO, IoActivationRuleScopeDTO>;

struct IoRuleRegistrationDTO final {
  RequestOwnerEpoch owner_epoch;
  std::uint64_t catalog_generation;
  std::uint64_t registration_id;
  InterruptPolicyId policy_id;
  IoRuleScopeDTO scope;
  friend bool operator==(const IoRuleRegistrationDTO&, const IoRuleRegistrationDTO&) = default;
};

/// Caller compiles the bounded immutable condition outside ingress. Initial
/// wrapper qualifies Boolean signals and the exact installed ordered typed BB
/// source; there is no renewal or caller-granted source validity. Required sources are canonical
/// and exactly the condition's referenced sources, independently qualified even
/// when Boolean short circuiting would determine the expression.
/// Host-only, already-qualified search intent. Guards and compound conditions
/// never receive an expectation; the evaluator does not solve expressions.
struct IoBooleanLevelExpectationDTO final {
  IoSignalId signal_id;
  bool expected_value;
  friend bool operator==(const IoBooleanLevelExpectationDTO&,
                         const IoBooleanLevelExpectationDTO&) = default;
};

struct IoRuleArmRequestDTO final {
  InterruptPolicyId policy_id;
  IoRuleScopeDTO scope;
  std::shared_ptr<const CheckedIoCondition> condition;
  MotionInterruptActivationMode activation_mode;
  std::vector<InterruptSourceId> required_sources;
  std::optional<IoBooleanLevelExpectationDTO> search_expectation{};
};

/// Exact causal source acquisition, distinct from the supporting snapshot.
/// Baseline matches have no causal acquisition and retain only baseline data.
struct IoRuleAcquisitionKeyDTO final {
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  std::uint64_t source_sequence;
  IoSourceCursorDTO source_cursor;
  friend bool operator==(const IoRuleAcquisitionKeyDTO&, const IoRuleAcquisitionKeyDTO&) = default;
};

struct IoRuleTripEvidenceDTO final {
  IoRuleRegistrationDTO registration;
  std::vector<IoSignalSampleDTO> matched_samples;
  std::vector<IoSourceCursorDTO> source_cursors;
  TimePoint evaluated_at_ns;
  std::optional<IoRuleAcquisitionKeyDTO> triggering_acquisition{};
  std::optional<IoBlackboardTripEvidenceDTO> blackboard{};
  friend bool operator==(const IoRuleTripEvidenceDTO&, const IoRuleTripEvidenceDTO&) = default;
};

enum class IoRuleSourceFailureKind { kContinuityLoss, kMissing, kStale, kInvalid, kUnavailable };

/// First observed failure for this required source stays retained independently
/// of a predicate match. Actual loss records preserve exact loss kind/flags;
/// derived stale/invalid/unavailable failure is never labelled a signal trip.
struct IoRuleSourceLossEvidenceDTO final {
  IoRuleRegistrationDTO registration;
  InterruptSourceId source_id;
  IoSourceIncarnation incarnation;
  IoSourceCursorDTO cursor;
  IoRuleSourceFailureKind kind;
  std::optional<IoSourceLossDTO> source_loss;
  std::optional<IoSignalSampleDTO> sample;
  TimePoint evaluated_at_ns;
  std::optional<IoBlackboardSourceLossEvidenceDTO> blackboard{};
  friend bool operator==(const IoRuleSourceLossEvidenceDTO&,
                         const IoRuleSourceLossEvidenceDTO&) = default;
};

enum class IoRuleRegistrationState { kArmed, kRetired };

struct IoRuleEvidenceDTO final {
  IoRuleRegistrationState state;
  std::optional<IoRuleTripEvidenceDTO> trip;
  std::vector<IoRuleSourceLossEvidenceDTO> source_losses;
  /// Unexpected evaluator invariant failure remains separate from source loss.
  std::optional<ErrorDTO> evaluation_error;
  friend bool operator==(const IoRuleEvidenceDTO&, const IoRuleEvidenceDTO&) = default;
};

}  // namespace manyforge::core
