#pragma once

#include <action_msgs/srv/cancel_goal.hpp>
#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <control_msgs/msg/joint_trajectory_controller_state.hpp>
#include <control_msgs/msg/speed_scaling_factor.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <memory>
#include <manyforge_ros2_control_execution/volatile_motion_trace.hpp>
#include <mutex>
#include <rclcpp_action/rclcpp_action.hpp>
#include <string>

namespace manyforge::ros2_control_execution {

struct JtcExecutionAdapterConfig {
  std::string action_name;
  std::string state_topic;
  std::string scale_topic;
  core::ExecutionBindingEpoch binding_epoch{0U};
  core::ExecutionLaneFactsDTO lane_facts;
  std::shared_ptr<VolatileMotionTrace> motion_trace{};
};

/// Transport and facts for one owner-verified controller binding. No planner,
/// shadow clock, Stop selection, completion policy, lifecycle actions or
/// recovery. The owner supplies readiness and retires this instance on a new
/// binding epoch; shared transport resources and the injected clock outlive the
/// adapter.
class Ros2ControlExecutionBackend final : public core::IExecutionPort {
public:
  Ros2ControlExecutionBackend(Ros2TransportResources &resources,
                              core::IClock &clock,
                              JtcExecutionAdapterConfig config);
  ~Ros2ControlExecutionBackend() override;
  Ros2ControlExecutionBackend(const Ros2ControlExecutionBackend &) = delete;
  Ros2ControlExecutionBackend &
  operator=(const Ros2ControlExecutionBackend &) = delete;

  /// Facts from the existing lifecycle/binding authority, not a new authority.
  /// Publications naming another epoch are ignored. This method sends no
  /// motion.
  void ObserveReadiness(core::ExecutionReadinessDTO readiness);
  /// Close admission and release this binding's SDK entities outside adapter
  /// locks. Executing SDK callbacks retain their entity and closed State;
  /// retirement does not stop the shared executor or issue commands.
  void shutdown() noexcept;

  core::ExecutionPortCommandResultDTO
  Submit(const core::ExecutionGoalKey &key,
         const core::TrajectoryDTO &trajectory) override;
  core::ExecutionPortCommandResultDTO
  Replace(const core::ExecutionGoalKey &active_key,
          const core::ExecutionGoalKey &next_key,
          const core::TrajectoryDTO &trajectory) override;
  core::ExecutionPortCommandResultDTO
  ReplaceScheduled(const core::ExecutionGoalKey &active_key,
                   const core::ExecutionGoalKey &next_key,
                   const core::TrajectoryDTO &trajectory,
                   const core::ExecutionReplacementStartDTO &start) override;
  core::ExecutionPortCommandResultDTO
  Cancel(const core::ExecutionGoalKey &key) override;
  core::ExecutionPortCommandResultDTO SetScale(double value) override;
  std::optional<core::ExecutionGoalObservationDTO>
  ReadGoal(const core::ExecutionGoalKey &key) const override;
  std::optional<core::ExecutionControllerSampleDTO> ReadSample() const override;
  core::ExecutionReadinessDTO ReadReadiness() const override;
  /// The controller's own goal state, for the component's activity
  /// (CONTRACTS §1): a goal it has not reported finished, and whether its
  /// latest finished goal was cancelled (JTC reports a cancelled goal finished
  /// before it brakes; the caller then reads the reference in `ReadSample`).
  /// `expected_end`: the earliest expected end of a goal the controller has
  /// not reported finished (every open goal, the Stop's included), on the
  /// backend's clock: its last progress report (feedback cursor) plus its
  /// remaining trajectory time, else its acceptance (or send) plus its whole
  /// trajectory time; a cancelled goal is expected to end at its cancel.
  struct GoalActivity final {
    bool goal_running{false};
    bool last_cancelled{false};
    std::optional<core::TimePoint> expected_end;
  };
  [[nodiscard]] GoalActivity ReadGoalActivity() const;
  /// Read this epoch's own FJT client availability for the Enable
  /// postcondition. No graph wait or change to execution readiness.
  [[nodiscard]] bool ActionServerIsReady() const;
  /// Existing NRT lifecycle/observer checkpoints maintain selected publisher
  /// facts. Neither method changes execution readiness or sends a command.
  void RefreshEndpointFacts();
  [[nodiscard]] std::optional<core::ErrorDTO> ReadEndpointDiagnostic() const;
  core::ExecutionTimingDTO ReadTiming() override;
  core::ExecutionLaneFactsDTO ReadLaneFacts() const override;

private:
  friend class JtcExecutionPortTestPeer;
  using Action = control_msgs::action::FollowJointTrajectory;
  using GoalHandle = rclcpp_action::ClientGoalHandle<Action>;
  struct State;
  core::ExecutionPortCommandResultDTO SendGoal(
      const core::ExecutionGoalKey &key, const core::TrajectoryDTO &trajectory,
      std::optional<core::ExecutionGoalKey> active_key,
      VolatileMotionTrace::Garbage &trace_garbage,
      std::optional<core::ExecutionReplacementStartDTO> start = std::nullopt);
  static void ReceiveState(
      const std::shared_ptr<State> &state,
      const control_msgs::msg::JointTrajectoryControllerState &message);
  static void ReceiveResponse(const std::shared_ptr<State> &state,
                              const core::ExecutionGoalKey &key,
                              const GoalHandle::SharedPtr &handle);
  static core::ExecutionPortCommandResultDTO
  SendCancel(const std::shared_ptr<State> &state,
             const core::ExecutionGoalKey &key,
             const GoalHandle::SharedPtr &handle);
  static void ReceiveCancelResponse(
      const std::shared_ptr<State> &state, const core::ExecutionGoalKey &key,
      std::uint64_t cancel_sequence,
      const action_msgs::srv::CancelGoal::Response::SharedPtr &response);
  static void ReceiveFeedback(const std::shared_ptr<State> &state,
                              const core::ExecutionGoalKey &key,
                              const GoalHandle::SharedPtr &handle,
                              const Action::Feedback &feedback);
  static void ReceiveResult(const std::shared_ptr<State> &state,
                            const core::ExecutionGoalKey &key,
                            const GoalHandle::WrappedResult &result);

  std::shared_ptr<State> state_;
  rclcpp_action::Client<Action>::SharedPtr action_client_;
  rclcpp::Subscription<
      control_msgs::msg::JointTrajectoryControllerState>::SharedPtr state_sub_;
  rclcpp::Publisher<control_msgs::msg::SpeedScalingFactor>::SharedPtr
      scale_pub_;
};

} // namespace manyforge::ros2_control_execution
