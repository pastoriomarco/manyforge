#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace manyforge::core {

enum class ActionSource {
  kProgramNode,
  kDirectControl,
  kRobotControl,
  kMcp,
  kSystem,
};

inline const char* to_string(ActionSource source) {
  switch (source) {
    using enum ActionSource;
    case kProgramNode: return "program_node";
    case kDirectControl: return "direct_control";
    case kRobotControl: return "robot_control";
    case kMcp: return "mcp";
    case kSystem: return "system";
  }
  return "system";
}

inline std::optional<ActionSource> action_source_from_string(
    std::string_view value) {
  if (value == "program_node") {
    return ActionSource::kProgramNode;
  }
  if (value == "direct_control") {
    return ActionSource::kDirectControl;
  }
  if (value == "robot_control") {
    return ActionSource::kRobotControl;
  }
  if (value == "mcp") {
    return ActionSource::kMcp;
  }
  if (value == "system") {
    return ActionSource::kSystem;
  }
  return std::nullopt;
}

struct ActionSpanDTO {
  ActionSource action_source = ActionSource::kSystem;
  std::string action_span_id;
  std::string program_run_id;
  std::string node_instance_id;
  std::string node_attempt_id;
  std::string parent_node_attempt_id;
  friend bool operator==(const ActionSpanDTO&, const ActionSpanDTO&) = default;
};

}  // namespace manyforge::core
