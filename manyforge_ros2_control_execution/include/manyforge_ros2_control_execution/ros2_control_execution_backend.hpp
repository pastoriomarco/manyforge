#pragma once

#include <atomic>
#include <action_msgs/srv/cancel_goal.hpp>
#include <control_msgs/action/follow_joint_trajectory.hpp>
#include <control_msgs/msg/joint_trajectory_controller_state.hpp>
#include <control_msgs/msg/speed_scaling_factor.hpp>
#include <manyforge_core/ports/iexecution_port.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <memory>
#include <condition_variable>
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
  core::Duration lifecycle_command_timeout_ns{};
};

/// Transport and facts for one owner-verified controller binding. No planner,
/// shadow clock, Stop selection, completion policy, lifecycle actions or
/// recovery. The owner supplies readiness and retires this instance on a new
/// binding epoch; the adapter retains its transport and borrows the clock.
class Ros2ControlExecutionBackend final : public core::IExecutionPort {
public:
  Ros2ControlExecutionBackend(std::shared_ptr<Ros2TransportResources> resources,
                              std::shared_ptr<core::IClock> clock,
                              core::IScheduler &scheduler,
                              JtcExecutionAdapterConfig config,
                              std::shared_ptr<std::atomic<bool>> state_gate = {});
  ~Ros2ControlExecutionBackend() override;
  Ros2ControlExecutionBackend(const Ros2ControlExecutionBackend &) = delete;
  Ros2ControlExecutionBackend &
  operator=(const Ros2ControlExecutionBackend &) = delete;

  /// Facts from the existing lifecycle/binding authority, not a new authority.
  /// Publications naming another epoch are ignored. This method sends no
  /// motion.
  void ObserveReadiness(core::ExecutionReadinessDTO readiness);
  /// Close admission, join the epoch transport and release its ROS entities.
  bool shutdown() noexcept;
  bool shutdown(std::chrono::steady_clock::time_point deadline) noexcept;
  /// For a dedicated epoch owner: close facts on the owner, join its transport,
  /// then release the ROS entities. Both operations are idempotent.
  /// Call off the epoch owner: on it, admission closes and the call returns
  /// false without waiting for cancel responses only that owner delivers.
  bool close_admission() noexcept;
  bool close_admission(std::chrono::steady_clock::time_point deadline) noexcept;
  /// Fault-owner rebuild: retire this epoch without sending cancellation.
  void close_admission_without_cancel() noexcept;
  void release_ros_entities() noexcept;

  core::ExecutionPortCommandResultDTO
  Submit(const core::ExecutionGoalKey &key,
         const core::TrajectoryDTO &trajectory,
         std::shared_ptr<core::ExecutionDispatchToken> token = {}) override;
  core::ExecutionPortCommandResultDTO
  Replace(const core::ExecutionGoalKey &active_key,
          const core::ExecutionGoalKey &next_key,
          const core::TrajectoryDTO &trajectory,
          std::shared_ptr<core::ExecutionDispatchToken> token = {}) override;
  core::ExecutionPortCommandResultDTO
  ReplaceScheduled(const core::ExecutionGoalKey &active_key,
                   const core::ExecutionGoalKey &next_key,
                   const core::TrajectoryDTO &trajectory,
                   const core::ExecutionReplacementStartDTO &start,
                   std::shared_ptr<core::ExecutionDispatchToken> token = {}) override;
  core::ExecutionPortCommandResultDTO
  Cancel(const core::ExecutionGoalKey &key,
         std::chrono::steady_clock::time_point deadline =
             std::chrono::steady_clock::time_point::max()) override;
  core::ExecutionPortCommandResultDTO SetScale(double value, std::uint64_t sequence) override;
  /// Whether the controller listens on the scale topic now (lock-free).
  [[nodiscard]] std::optional<bool> ScaleInputListening() const noexcept override;
  core::ScaleSnapshot SnapshotScaleAfterFence(
      std::chrono::steady_clock::time_point deadline) override;
  core::ExecutionPortCommandResultDTO SetScale(double value) {
    return SetScale(value, ReserveScaleSequence());
  }
  std::optional<core::ExecutionGoalObservationDTO>
  ReadGoal(const core::ExecutionGoalKey &key) const override;
  std::optional<core::ExecutionControllerSampleDTO> ReadSample() const override;
  core::ExecutionReadinessDTO ReadReadiness() const override;
  /// The controller's own goal state, for the component's activity:
  /// a goal it has not reported finished, and whether its
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
    bool late_cancel_not_issued{false};
  };
  [[nodiscard]] GoalActivity ReadGoalActivity() const;
  enum class LateCancelUnresolvedReason { kNotIssued, kUnknown };
  struct UnresolvedLateCancel final {
    core::ExecutionBindingEpoch binding_epoch;
    core::ExecutionGoalKey key;
    std::optional<std::uint64_t> execution_id;
    LateCancelUnresolvedReason reason;
  };
  [[nodiscard]] std::vector<UnresolvedLateCancel> ReadUnresolvedLateCancels() const;
  [[nodiscard]] bool loss_cancel_shutdown_settled() const noexcept;
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
  void close_admission_impl(bool send_loss_cancels,
                            std::chrono::steady_clock::time_point deadline) noexcept;
  friend class JtcExecutionPortTestPeer;
  using Action = control_msgs::action::FollowJointTrajectory;
  using GoalHandle = rclcpp_action::ClientGoalHandle<Action>;
  struct State;
  struct CancelAttempt;
  struct LateCancelItem;
  void ObserveReadinessImpl(core::ExecutionReadinessDTO readiness);
  bool ActionServerIsReadyImpl() const;
  core::ExecutionPortCommandResultDTO SubmitImpl(const core::ExecutionGoalKey &, const core::TrajectoryDTO &, std::shared_ptr<core::ExecutionDispatchToken>);
  core::ExecutionPortCommandResultDTO ReplaceImpl(const core::ExecutionGoalKey &, const core::ExecutionGoalKey &, const core::TrajectoryDTO &, std::shared_ptr<core::ExecutionDispatchToken>);
  core::ExecutionPortCommandResultDTO ReplaceScheduledImpl(const core::ExecutionGoalKey &, const core::ExecutionGoalKey &, const core::TrajectoryDTO &, const core::ExecutionReplacementStartDTO &, std::shared_ptr<core::ExecutionDispatchToken>);
  core::ExecutionPortCommandResultDTO CancelImpl(
      const core::ExecutionGoalKey &, std::chrono::steady_clock::time_point);
  core::ExecutionPortCommandResultDTO SetScaleImpl(double, std::uint64_t);
  core::ExecutionPortCommandResultDTO SendGoal(
      const core::ExecutionGoalKey &key, const core::TrajectoryDTO &trajectory,
      std::optional<core::ExecutionGoalKey> active_key,
      VolatileMotionTrace::Garbage &trace_garbage,
      std::optional<core::ExecutionReplacementStartDTO> start = std::nullopt,
      std::shared_ptr<core::ExecutionDispatchToken> token = {});
  static void ReceiveState(
      const std::shared_ptr<State> &state,
      const control_msgs::msg::JointTrajectoryControllerState &message);
  static void ReceiveGatedState(
      const std::shared_ptr<State> &state,
      const std::shared_ptr<std::atomic<bool>> &gate,
      const control_msgs::msg::JointTrajectoryControllerState &message);
  static void ReceiveResponse(const std::shared_ptr<State> &state,
                              const core::ExecutionGoalKey &key,
                              const GoalHandle::SharedPtr &handle,
                              const std::shared_ptr<CancelAttempt> &attempt);
  static void DrainLossCancels(const std::shared_ptr<State> &state) noexcept;
  static void
  SendAutomaticCancel(const std::shared_ptr<State> &state,
                      const core::ExecutionGoalKey &key,
                      const GoalHandle::SharedPtr &handle,
                      const std::shared_ptr<CancelAttempt> &attempt,
                      const rclcpp_action::Client<Action>::SharedPtr &client) noexcept;
  static void
  QueueAutomaticCancel(const std::shared_ptr<State> &state,
                       const core::ExecutionGoalKey &key,
                       const GoalHandle::SharedPtr &handle,
                       const std::shared_ptr<CancelAttempt> &attempt) noexcept;
  static core::ExecutionPortCommandResultDTO
  SendCancel(const std::shared_ptr<State> &state,
             const core::ExecutionGoalKey &key,
             const GoalHandle::SharedPtr &handle,
             std::chrono::steady_clock::time_point deadline);
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
  std::shared_ptr<Ros2TransportResources> epoch_resources_;
  std::mutex release_mutex_;
  std::condition_variable release_cv_;
  bool releasing_ = false;
  bool released_ = false;
  rclcpp_action::Client<Action>::SharedPtr action_client_;
  // OD-66: the action client's time-critical group (null on an owner without
  // a time-critical thread: the default group).
  rclcpp::CallbackGroup::SharedPtr action_callback_group_;
  rclcpp::Subscription<
      control_msgs::msg::JointTrajectoryControllerState>::SharedPtr state_sub_;
  rclcpp::Publisher<control_msgs::msg::SpeedScalingFactor>::SharedPtr
      scale_pub_;
};

} // namespace manyforge::ros2_control_execution
