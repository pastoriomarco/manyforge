#pragma once

#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/ports/iexecution_state_sink.hpp>

namespace manyforge::roboplan_backend {

/// RoboPlan implementation of the execution-state sink port.
///
/// This adapter is the only place where execution-observed arm/gripper joint
/// state should be projected into a RoboPlan planning scene. It preserves the
/// previous ros2_control backend semantics: group-sized vectors are expanded
/// through RoboPlan, full-sized vectors are applied directly, and best-effort
/// updates never block behind an already-held scene mutex.
class RoboPlanExecutionStateSink final : public core::IExecutionStateSink {
 public:
  RoboPlanExecutionStateSink(std::shared_ptr<roboplan::Scene> scene,
                             std::shared_ptr<std::mutex> scene_mutex = nullptr);

  void set_robot_model_authority_metadata(
      std::map<std::string, std::string> canonical_to_view_joint_names,
      std::map<std::string, std::string> view_to_canonical_joint_names,
      std::map<std::string, std::vector<std::string>>
          group_commandable_joint_names);

  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string& group_id,
      const std::vector<double>& positions_rad,
      core::ExecutionStateUpdateMode mode) override;

  std::optional<double> group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const override;

  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name,
      double position_rad,
      core::ExecutionStateUpdateMode mode) override;

 private:
  struct CommandableGroupMapping {
    std::vector<std::string> commandable_view_joint_names;
    std::vector<size_t> expanded_group_indices;
  };

  std::string view_joint_name_locked(const std::string& joint_name) const;
  std::string commandable_group_cache_key_locked(
      const std::string& group_id,
      const std::vector<std::string>& group_joint_names) const;
  std::optional<CommandableGroupMapping> commandable_group_mapping_locked(
      const std::string& group_id,
      const std::vector<std::string>& group_joint_names) const;
  std::vector<std::string> commandable_view_joint_names_locked(
      const std::string& group_id,
      const std::vector<std::string>& group_joint_names) const;

  std::shared_ptr<roboplan::Scene> scene_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::map<std::string, std::string> canonical_to_view_joint_names_;
  std::map<std::string, std::string> view_to_canonical_joint_names_;
  std::map<std::string, std::vector<std::string>>
      group_commandable_joint_names_;
  mutable std::map<std::string, CommandableGroupMapping>
      commandable_group_cache_;
};

}  // namespace manyforge::roboplan_backend
