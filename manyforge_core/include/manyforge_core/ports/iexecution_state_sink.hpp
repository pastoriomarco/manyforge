#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

enum class ExecutionStateUpdateMode {
  kBlocking,
  kBestEffort,
};

enum class ExecutionStateSinkStatus {
  kApplied,
  kBusy,
  kInvalidGroup,
  kInvalidJoint,
  kInvalidPositions,
  kStateUnavailable,
  kException,
};

struct ExecutionStateSinkResult {
  ExecutionStateSinkStatus status = ExecutionStateSinkStatus::kApplied;

  constexpr bool applied() const noexcept {
    return status == ExecutionStateSinkStatus::kApplied;
  }
};

inline constexpr std::string_view execution_state_sink_status_reason(
    ExecutionStateSinkStatus status) noexcept {
  switch (status) {
    case ExecutionStateSinkStatus::kApplied:
      return "applied";
    case ExecutionStateSinkStatus::kBusy:
      return "scene_sink_busy";
    case ExecutionStateSinkStatus::kInvalidGroup:
      return "invalid_group";
    case ExecutionStateSinkStatus::kInvalidJoint:
      return "invalid_joint";
    case ExecutionStateSinkStatus::kInvalidPositions:
      return "invalid_positions";
    case ExecutionStateSinkStatus::kStateUnavailable:
      return "scene_state_unavailable";
    case ExecutionStateSinkStatus::kException:
      return "scene_sink_exception";
  }
  return "unknown_execution_state_sink_status";
}

class IExecutionStateSink {
 public:
  virtual ~IExecutionStateSink() = default;

  virtual ExecutionStateSinkResult apply_group_joint_positions(
      const std::string& group_id,
      const std::vector<double>& positions_rad,
      ExecutionStateUpdateMode mode) = 0;

  virtual std::optional<double> group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const = 0;

  virtual ExecutionStateSinkResult apply_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name,
      double position_rad,
      ExecutionStateUpdateMode mode) = 0;
};

}  // namespace manyforge::core
