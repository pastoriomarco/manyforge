#pragma once

#include <memory>
#include <string>
#include <vector>

#include <manyforge_core/ports/iexecution_state_sink.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

namespace manyforge::roboplan_backend {

struct RetainedExecutionStateSinkBinding final {
  std::string compiled_group_name;
  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  /// The motion backend publishes directly into its addressed channel before
  /// requesting projection. A scalar gripper transport has no such raw writer,
  /// so its sink request commits the commanded state before projection.
  bool publish_on_apply{false};
};

/// Exact bridge from an execution adapter's historical group-state sink to
/// retained, addressed state channels.
///
/// Projection-only bindings never publish the supplied vector: they project
/// the latest owner-issued addressed state, preventing a scene callback from
/// becoming a second state authority. A binding explicitly marked
/// `publish_on_apply` is itself the single writer for that scalar/group source.
class RetainedExecutionStateSink final : public core::IExecutionStateSink {
 public:
  RetainedExecutionStateSink(
      core::IClock& clock,
      std::vector<RetainedExecutionStateSinkBinding> bindings);

  core::ExecutionStateSinkResult apply_group_joint_positions(
      const std::string& group_id,
      const std::vector<double>& positions_si,
      core::ExecutionStateUpdateMode mode) override;

  [[nodiscard]] std::optional<double> group_joint_position(
      const std::string& group_id,
      const std::string& joint_name) const override;

  core::ExecutionStateSinkResult apply_group_joint_position(
      const std::string& group_id,
      const std::string& joint_name,
      double position_si,
      core::ExecutionStateUpdateMode mode) override;

 private:
  [[nodiscard]] const RetainedExecutionStateSinkBinding* find_binding(
      const std::string& group_id) const noexcept;
  [[nodiscard]] static core::ExecutionStateSinkResult map_result(
      const RetainedStateProjectionResult& result) noexcept;
  [[nodiscard]] static double position_value(
      const core::CoordinatePositionDTO& position) noexcept;

  core::IClock* clock_{};
  std::vector<RetainedExecutionStateSinkBinding> bindings_;
};

}  // namespace manyforge::roboplan_backend
