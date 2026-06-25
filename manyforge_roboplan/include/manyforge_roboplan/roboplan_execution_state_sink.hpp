#pragma once

#include <memory>
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
  std::shared_ptr<roboplan::Scene> scene_;
  std::shared_ptr<std::mutex> scene_mutex_;
};

}  // namespace manyforge::roboplan_backend
