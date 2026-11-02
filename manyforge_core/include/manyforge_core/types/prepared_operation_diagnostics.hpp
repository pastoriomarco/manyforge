#pragma once

#include <manyforge_core/types/prepared_operation_participant.hpp>

#include <string>
#include <vector>

namespace manyforge::core {

/// Bounded cold-path evidence for one exact prepared motion participant.
///
/// This deliberately retains only the measurements and speed facts needed
/// to explain the accepted trajectory.  The trajectory itself remains in the
/// immutable prepared participant and is not duplicated in the operation
/// journal.
struct PreparedMotionDiagnosticsDTO final {
  OperationParticipantId participant_id;
  ContentDigest prepared_contract_digest;
  MoveId move_id;
  std::vector<std::string> joint_names;
  TrajectoryValidationMetricsDTO trajectory_metrics;
  std::optional<double> requested_tcp_linear_speed_mps;
  double deployment_maximum_tcp_linear_speed_mps{};
  double admitted_tcp_linear_speed_mps{};
  /// Frame role held under the admitted speed: the active TCP's role (empty
  /// for a cell frame).
  std::vector<std::string> speed_check_frame_role_ids{};

  friend bool operator==(const PreparedMotionDiagnosticsDTO&,
                         const PreparedMotionDiagnosticsDTO&) = default;
};

}  // namespace manyforge::core
