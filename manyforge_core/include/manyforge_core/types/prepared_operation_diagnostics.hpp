#pragma once

#include <manyforge_core/types/prepared_operation_participant.hpp>

#include <string>
#include <vector>

namespace manyforge::core {

/// Bounded cold-path evidence for one exact prepared motion participant.
///
/// This deliberately retains only the measurements and policy identity needed
/// to explain the accepted trajectory.  The trajectory itself remains in the
/// immutable prepared participant and is not duplicated in the operation
/// journal.
struct PreparedMotionDiagnosticsDTO final {
  OperationParticipantId participant_id;
  ContentDigest prepared_contract_digest;
  MoveId move_id;
  std::vector<std::string> joint_names;
  TrajectoryValidationMetricsDTO trajectory_metrics;
  PreparedRuntimePolicyRefDTO runtime_policy;

  friend bool operator==(const PreparedMotionDiagnosticsDTO&,
                         const PreparedMotionDiagnosticsDTO&) = default;
};

}  // namespace manyforge::core
