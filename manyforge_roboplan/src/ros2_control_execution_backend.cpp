#include <manyforge_roboplan/ros2_control_execution_backend.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <manyforge_core/motion_interrupt/soft_on_path_stop_planner.hpp>
#include <manyforge_core/trajectory/trajectory_sampler.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/collision/parallel_collision_validation_worker_pool.hpp>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>

namespace manyforge::roboplan_backend {
namespace {

constexpr double kRuntimeScaleSlewPerSecond = 4.0;  // 0→1 in 250 ms.

core::CollisionPolicySnapshot default_collision_policy_snapshot() {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "default_free_space";
  policy.revision.revision_id = 1;
  policy.revision.content_hash = "default_free_space_v1";
  policy.require_full_diagnostics = false;
  return policy;
}

double clamp_progress(double progress) {
  return std::max(0.0, std::min(1.0, progress));
}

core::ErrorDTO make_error(core::ErrorCode code, std::string message) {
  return core::ErrorDTO{std::string(core::to_string(code)), std::move(message), {}};
}

core::ErrorDTO make_error(core::ErrorCode code,
                          std::string message,
                          std::map<std::string, std::string> details) {
  return core::ErrorDTO{
      std::string(core::to_string(code)), std::move(message), std::move(details)};
}

builtin_interfaces::msg::Duration duration_from_seconds(double seconds) {
  builtin_interfaces::msg::Duration duration;
  const double clamped = std::max(0.0, seconds);
  const auto whole = static_cast<int32_t>(std::floor(clamped));
  const auto nanosec =
      static_cast<uint32_t>(std::round((clamped - static_cast<double>(whole)) * 1e9));
  duration.sec = whole;
  duration.nanosec = std::min<uint32_t>(nanosec, 999999999u);
  return duration;
}

double seconds_since(std::chrono::steady_clock::time_point t,
                     std::chrono::steady_clock::time_point now) {
  return std::chrono::duration<double>(now - t).count();
}

bool wait_for_fjt_action_server(
    const rclcpp_action::Client<control_msgs::action::FollowJointTrajectory>::SharedPtr&
        client,
    double timeout_s) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(std::max(0.0, timeout_s));
  do {
    if (client->wait_for_action_server(std::chrono::milliseconds(100))) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  } while (std::chrono::steady_clock::now() < deadline);
  return client->wait_for_action_server(std::chrono::milliseconds(0));
}

void disable_fjt_controller_tolerances(
    control_msgs::action::FollowJointTrajectory::Goal& goal,
    const std::vector<std::string>& joint_names) {
  goal.path_tolerance.clear();
  goal.goal_tolerance.clear();
  for (const auto& joint_name : joint_names) {
    control_msgs::msg::JointTolerance tolerance;
    tolerance.name = joint_name;
    tolerance.position = -1.0;
    tolerance.velocity = -1.0;
    tolerance.acceleration = -1.0;
    goal.path_tolerance.push_back(tolerance);
    goal.goal_tolerance.push_back(std::move(tolerance));
  }
}

}  // namespace

Ros2ControlExecutionBackend::Ros2ControlExecutionBackend(
    std::shared_ptr<roboplan::Scene> scene,
    std::string group_name,
    core::IClock& clock,
    Ros2ControlExecutionBackendConfig config,
    std::shared_ptr<std::mutex> scene_mutex)
    : scene_(std::move(scene)),
      group_name_(std::move(group_name)),
      clock_(clock),
      config_(std::move(config)),
      scene_mutex_(std::move(scene_mutex)) {
  if (!scene_) {
    throw std::invalid_argument("scene must not be null");
  }
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
  validate_config();
  snapshot_.timestamp_ns = clock_.now();
  start_ros();
}

Ros2ControlExecutionBackend::~Ros2ControlExecutionBackend() {
  GoalHandleFJT::SharedPtr handle_to_cancel;
  GoalHandleFJT::SharedPtr managed_fjt_handle_to_cancel;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    handle_to_cancel = active_goal_handle_;
    running_ = false;
    terminal_ = true;
    stop_requested_ = true;
    pause_requested_ = false;
    active_goal_handle_.reset();
    active_goal_id_.reset();
  }
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_handle_to_cancel = managed_fjt_goal_;
    managed_fjt_goal_.reset();
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = true;
    managed_fjt_terminal_error_.reset();
    managed_fjt_cv_.notify_all();
  }
  cancel_goal_if_known(handle_to_cancel);
  cancel_goal_if_known(managed_fjt_handle_to_cancel);
  cancel_all_gripper_commands();
  set_command_gate(false);
  execution_cv_.notify_all();
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  stop_ros();
}

void Ros2ControlExecutionBackend::validate_config() const {
  if (config_.action_name.empty()) {
    throw std::invalid_argument("ros2_control action_name must not be empty");
  }
  if (config_.joint_state_topic.empty() && config_.controller_state_topic.empty()) {
    throw std::invalid_argument(
        "ros2_control execution requires joint_state_topic or controller_state_topic");
  }
  if (config_.required_joints.empty()) {
    throw std::invalid_argument("ros2_control required_joints must not be empty");
  }
  if (config_.joint_state_max_age_s < 0.0 ||
      config_.wait_for_state_timeout_s < 0.0 ||
      config_.wait_for_action_server_timeout_s < 0.0 ||
      config_.start_tolerance_rad < 0.0 ||
      config_.final_tolerance_rad < 0.0 ||
      config_.tracking_error_tolerance_rad < 0.0 ||
      config_.tracking_error_timeout_s < 0.0 ||
      config_.stop_completion_timeout_s < 0.0 ||
      config_.sim_reset_motion_duration_s < 0.0 ||
      !std::isfinite(config_.soft_on_path_stop_traj_delta_s) ||
      config_.soft_on_path_stop_traj_delta_s < 0.0 ||
      !std::isfinite(config_.soft_on_path_sample_period_s) ||
      config_.soft_on_path_sample_period_s <= 0.0 ||
      config_.monitor_period_s <= 0.0) {
    throw std::invalid_argument("ros2_control timing/tolerance config is invalid");
  }
  if (config_.soft_on_path_max_initial_path_deviation_rad.has_value() &&
      (!std::isfinite(*config_.soft_on_path_max_initial_path_deviation_rad) ||
       *config_.soft_on_path_max_initial_path_deviation_rad < 0.0)) {
    throw std::invalid_argument(
        "soft_on_path_max_initial_path_deviation_rad must be finite and >= 0");
  }
  if (config_.soft_on_path_max_initial_velocity_deviation_rad_s.has_value() &&
      (!std::isfinite(*config_.soft_on_path_max_initial_velocity_deviation_rad_s) ||
       *config_.soft_on_path_max_initial_velocity_deviation_rad_s < 0.0)) {
    throw std::invalid_argument(
        "soft_on_path_max_initial_velocity_deviation_rad_s must be finite and >= 0");
  }
  for (const auto& [joint, jerk] : config_.soft_on_path_max_jerk_rad_s3) {
    if (joint.empty() || !std::isfinite(jerk) || jerk <= 0.0) {
      throw std::invalid_argument(
          "soft_on_path_max_jerk_rad_s3 entries must have non-empty joint names and finite positive values");
    }
  }
  if (config_.execution_state_source_mode != "authored_preview" &&
      config_.execution_state_source_mode != "commanded_sim" &&
      config_.execution_state_source_mode != "observed_ros2" &&
      config_.execution_state_source_mode != "observed_robot") {
    throw std::invalid_argument(
        "execution_state_source_mode must be one of authored_preview, commanded_sim, observed_ros2");
  }
  if (config_.execution_state_source_mode == "commanded_sim" &&
      config_.initial_joint_positions_rad.size() != config_.required_joints.size()) {
    throw std::invalid_argument(
        "commanded_sim requires initial_joint_positions_rad with one value per required joint");
  }
  if (config_.disable_controller_path_tolerances &&
      config_.execution_state_source_mode != "commanded_sim") {
    throw std::invalid_argument(
        "disable_controller_path_tolerances is allowed only for commanded_sim deployments");
  }
}

void Ros2ControlExecutionBackend::start_ros() {
  context_ = std::make_shared<rclcpp::Context>();
  int argc = 0;
  char** argv = nullptr;
  context_->init(argc, argv);
  rclcpp::NodeOptions options;
  options.context(context_);
  node_ = std::make_shared<rclcpp::Node>(config_.node_name, options);

  action_client_ =
      rclcpp_action::create_client<FollowJointTrajectory>(node_, config_.action_name);

  if (!config_.joint_state_topic.empty()) {
    joint_state_sub_ = node_->create_subscription<sensor_msgs::msg::JointState>(
        config_.joint_state_topic, rclcpp::SystemDefaultsQoS(),
        [this](const sensor_msgs::msg::JointState::SharedPtr msg) {
          on_joint_state(msg);
        });
  }
  if (!config_.controller_state_topic.empty()) {
    controller_state_sub_ =
        node_->create_subscription<control_msgs::msg::JointTrajectoryControllerState>(
            config_.controller_state_topic, rclcpp::SystemDefaultsQoS(),
            [this](
                const control_msgs::msg::JointTrajectoryControllerState::SharedPtr msg) {
              on_controller_state(msg);
            });
  }
  if (!config_.command_gate_topic.empty()) {
    auto qos = rclcpp::QoS(1).reliable().transient_local();
    command_gate_pub_ = node_->create_publisher<std_msgs::msg::Bool>(
        config_.command_gate_topic, qos);
    set_command_gate(false);
  }
#ifdef MANYFORGE_HAS_SPEED_SCALING_FACTOR_MSG
  if (config_.enable_jtc_speed_scaling_input) {
    std::string speed_scaling_topic = config_.speed_scaling_topic;
    if (speed_scaling_topic.empty()) {
      constexpr char suffix[] = "/follow_joint_trajectory";
      if (config_.action_name.size() >= std::char_traits<char>::length(suffix) &&
          config_.action_name.compare(
              config_.action_name.size() - std::char_traits<char>::length(suffix),
              std::char_traits<char>::length(suffix),
              suffix) == 0) {
        speed_scaling_topic =
            config_.action_name.substr(
                0, config_.action_name.size() - std::char_traits<char>::length(suffix)) +
            "/speed_scaling_input";
      } else {
        speed_scaling_topic = config_.action_name + "/speed_scaling_input";
      }
    }
    auto qos = rclcpp::QoS(1).reliable().transient_local();
    speed_scaling_pub_ =
        node_->create_publisher<control_msgs::msg::SpeedScalingFactor>(
            speed_scaling_topic, qos);
    publish_runtime_velocity_scale(
        runtime_velocity_scale_.load(std::memory_order_acquire));
  }
#endif

  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context_;
  executor_ = std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
      executor_options, 2);
  executor_->add_node(node_);
  spin_thread_ = std::thread([this]() { executor_->spin(); });
}

void Ros2ControlExecutionBackend::stop_ros() {
  if (executor_) {
    executor_->cancel();
  }
  if (spin_thread_.joinable()) {
    spin_thread_.join();
  }
  if (executor_ && node_) {
    executor_->remove_node(node_);
  }
  node_.reset();
  executor_.reset();
  if (context_) {
    context_->shutdown("manyforge ros2_control execution backend shutdown");
    context_.reset();
  }
}

void Ros2ControlExecutionBackend::on_joint_state(
    const sensor_msgs::msg::JointState::SharedPtr msg) {
  JointSample sample;
  sample.joint_names = msg->name;
  sample.positions = msg->position;
  sample.velocities = msg->velocity;
  sample.received_at = std::chrono::steady_clock::now();
  sample.source = "joint_state";
  sample.topic = config_.joint_state_topic;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_joint_state_ = std::move(sample);
  }
  state_cv_.notify_all();
  sync_idle_scene_from_latest_sample();
}

void Ros2ControlExecutionBackend::on_controller_state(
    const control_msgs::msg::JointTrajectoryControllerState::SharedPtr msg) {
  JointSample sample;
  sample.joint_names = msg->joint_names;
  sample.positions = msg->feedback.positions;
  sample.velocities = msg->feedback.velocities;
  sample.received_at = std::chrono::steady_clock::now();
  sample.source = "controller_state";
  sample.topic = config_.controller_state_topic;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    latest_controller_state_ = std::move(sample);
  }
  state_cv_.notify_all();
  sync_idle_scene_from_latest_sample();
}

void Ros2ControlExecutionBackend::on_action_feedback(
    GoalHandleFJT::SharedPtr goal_handle,
    const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback) {
  const auto now = std::chrono::steady_clock::now();
  double elapsed_s = 0.0;
  bool active_goal_feedback = false;
  bool managed_fjt_terminal_feedback = false;
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_feedback =
        goal_handle && managed_fjt_terminal_goal_id_.has_value() &&
        goal_handle->get_goal_id() == *managed_fjt_terminal_goal_id_;
  }
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (resync_required_) {
      return;
    }
    active_goal_feedback =
        goal_handle && active_goal_id_.has_value() &&
        goal_handle->get_goal_id() == *active_goal_id_;
    if (!active_goal_feedback && !managed_fjt_terminal_feedback) {
      return;
    }
    if (terminal_ && !managed_fjt_terminal_feedback) {
      return;
    }
    if (active_goal_feedback && running_ && !first_feedback_seen_) {
      first_feedback_seen_ = true;
      execution_started_at_ = now;
      tracking_error_since_.reset();
    }
    elapsed_s = seconds_since(execution_started_at_, now);
  }
  JointSample sample;
  sample.joint_names = feedback->joint_names;
  sample.positions = feedback->actual.positions;
  sample.velocities = feedback->actual.velocities;
  sample.received_at = now;
  sample.source = "action_feedback";
  sample.topic = config_.action_name;
  JointSample error_sample;
  error_sample.joint_names = feedback->joint_names;
  error_sample.positions = feedback->error.positions;
  error_sample.velocities = feedback->error.velocities;
  error_sample.received_at = now;
  error_sample.source = "action_feedback_error";
  error_sample.topic = config_.action_name;
  const auto& observed_positions = sample.positions;
  const auto& expected_positions = feedback->desired.positions;
  const auto& observed_velocities = sample.velocities;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (active_goal_feedback && !error_sample.positions.empty()) {
      update_tracking_error_peak_locked(
          error_sample, elapsed_s,
          observed_positions.empty() ? nullptr : &observed_positions,
          expected_positions.empty() ? nullptr : &expected_positions,
          observed_velocities.empty() ? nullptr : &observed_velocities);
      latest_action_feedback_error_ = std::move(error_sample);
    }
    latest_action_feedback_ = std::move(sample);
  }
  if (managed_fjt_terminal_feedback && feedback) {
    record_controlled_stop_path_deviation_feedback(
        feedback->joint_names, feedback->actual.positions);
  }
  state_cv_.notify_all();
}

void Ros2ControlExecutionBackend::on_action_result(
    const GoalHandleFJT::WrappedResult& result) {
  const bool canceled = result.code == rclcpp_action::ResultCode::CANCELED;
  bool stop_requested = false;
  bool pause_requested = false;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_ || resync_required_) {
      return;
    }
    if (!active_goal_id_.has_value() || result.goal_id != *active_goal_id_) {
      return;
    }
    result_received_ = true;
    // Persist the raw result code so a parallel
    // `execute_controlled_stop(kHardDeceleration)` can verify the
    // cancel was actually honored. The stop_requested branch below
    // collapses SUCCEEDED + CANCELED into "stopped"; the raw code is
    // the only place the distinction survives.
    last_action_result_code_ = result.code;
    stop_requested = stop_requested_;
    pause_requested = pause_requested_;
  }

  if (stop_requested || pause_requested || canceled) {
    const auto snapshot = get_execution_snapshot();
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      terminal_ = true;
      running_ = false;
      active_goal_handle_.reset();
      active_goal_id_.reset();
      active_trajectory_.reset();
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = 1.0;
    }
    publish_snapshot(pause_requested ? core::ExecutionState::kPaused
                                     : core::ExecutionState::kStopped,
                     snapshot.progress,
                     snapshot.joint_positions_rad);
    execution_cv_.notify_all();
    return;
  }

  const bool controller_success =
      result.result &&
      result.result->error_code == FollowJointTrajectory::Result::SUCCESSFUL;
  if (result.code != rclcpp_action::ResultCode::SUCCEEDED || !controller_success) {
    const std::string controller_error =
        result.result ? result.result->error_string : "controller returned no result";
    const std::string feedback_error = latest_feedback_error_summary();
    std::string detail = controller_error;
    if (!feedback_error.empty()) {
      detail = detail.empty() ? feedback_error : detail + "; " + feedback_error;
    }
    fail_execution(detail.empty()
                       ? "FollowJointTrajectory goal failed"
                       : "FollowJointTrajectory goal failed: " + detail);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    terminal_ = true;
    running_ = false;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
  }
  complete_execution();
}

std::optional<Ros2ControlExecutionBackend::JointSample>
Ros2ControlExecutionBackend::select_latest_sample_locked(
    std::chrono::steady_clock::time_point now) const {
  const auto is_fresh = [&](const std::optional<JointSample>& sample) {
    return sample.has_value() &&
           seconds_since(sample->received_at, now) <= config_.joint_state_max_age_s;
  };
  if (is_fresh(latest_action_feedback_)) {
    return latest_action_feedback_;
  }
  if (is_fresh(latest_controller_state_)) {
    return latest_controller_state_;
  }
  if (is_fresh(latest_joint_state_)) {
    return latest_joint_state_;
  }
  return std::nullopt;
}

void Ros2ControlExecutionBackend::sync_idle_scene_from_latest_sample() {
  if (uses_authored_backend_state()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (running_) {
      return;
    }
  }

  std::optional<JointSample> sample;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    sample = select_latest_sample_locked(std::chrono::steady_clock::now());
  }
  if (!sample.has_value()) {
    return;
  }

  std::vector<double> positions;
  try {
    positions = positions_for_joints(*sample, config_.required_joints);
  } catch (const std::exception&) {
    return;
  }
  update_scene_positions(positions, SceneUpdateMode::kBestEffort);
  publish_snapshot(core::ExecutionState::kIdle, 0.0, positions);
}

bool Ros2ControlExecutionBackend::is_commanded_sim_mode() const {
  return config_.execution_state_source_mode == "commanded_sim";
}

bool Ros2ControlExecutionBackend::uses_authored_backend_state() const {
  return config_.execution_state_source_mode == "authored_preview" ||
         is_commanded_sim_mode();
}

void Ros2ControlExecutionBackend::set_command_gate(bool enabled) {
  if (!command_gate_pub_) {
    return;
  }
  std_msgs::msg::Bool msg;
  msg.data = enabled;
  command_gate_pub_->publish(msg);
}

void Ros2ControlExecutionBackend::publish_runtime_velocity_scale(double scale) {
#ifdef MANYFORGE_HAS_SPEED_SCALING_FACTOR_MSG
  if (!speed_scaling_pub_) {
    return;
  }
  control_msgs::msg::SpeedScalingFactor msg;
  msg.factor = std::max(0.0, std::min(1.0, scale));
  speed_scaling_pub_->publish(msg);
#else
  (void)scale;
#endif
}

core::Result<void> Ros2ControlExecutionBackend::command_sim_initial_state(
    std::string phase) {
  if (!is_commanded_sim_mode()) {
    return core::Result<void>::Ok();
  }
  if (config_.initial_joint_positions_rad.size() != config_.required_joints.size()) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorInvalidRequest,
        "commanded_sim initial_joint_positions_rad is incomplete",
        {{"phase", std::move(phase)},
         {"state_source_mode", config_.execution_state_source_mode}},
    });
  }
  if (!wait_for_fjt_action_server(action_client_,
                                  config_.wait_for_action_server_timeout_s)) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        "FollowJointTrajectory action server unavailable for commanded_sim reset",
        {{"phase", std::move(phase)},
         {"action_name", config_.action_name},
         {"wait_for_action_server_timeout_s",
          std::to_string(config_.wait_for_action_server_timeout_s)}},
    });
  }

  FollowJointTrajectory::Goal goal;
  goal.trajectory.joint_names = config_.required_joints;
  disable_fjt_controller_tolerances(goal, config_.required_joints);
  trajectory_msgs::msg::JointTrajectoryPoint home_point;
  home_point.positions = config_.initial_joint_positions_rad;
  home_point.time_from_start =
      duration_from_seconds(config_.sim_reset_motion_duration_s);
  goal.trajectory.points.push_back(std::move(home_point));

  // The commanded-sim profile is backend-authoritative: suppress any stale
  // topic-based command stream until this reset/home goal has been accepted.
  set_command_gate(false);
  auto goal_handle_future = action_client_->async_send_goal(goal);
  if (goal_handle_future.wait_for(
          std::chrono::duration<double>(config_.wait_for_action_server_timeout_s)) !=
      std::future_status::ready) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionTimeout,
        "Timed out waiting for commanded_sim reset goal response",
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }
  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception& exc) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        std::string("Failed to dispatch commanded_sim reset goal: ") + exc.what(),
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }
  if (!goal_handle) {
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        "commanded_sim reset goal was rejected",
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }

  set_command_gate(true);
  const auto result_future = action_client_->async_get_result(goal_handle);
  const double timeout_s = config_.sim_reset_motion_duration_s +
                           config_.wait_for_action_server_timeout_s +
                           config_.stop_completion_timeout_s;
  if (result_future.wait_for(std::chrono::duration<double>(timeout_s)) !=
      std::future_status::ready) {
    (void)action_client_->async_cancel_goal(goal_handle);
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionTimeout,
        "Timed out waiting for commanded_sim reset goal result",
        {{"phase", std::move(phase)},
         {"action_name", config_.action_name},
         {"timeout_s", std::to_string(timeout_s)}},
    });
  }
  const auto result = result_future.get();
  const bool success =
      result.code == rclcpp_action::ResultCode::SUCCEEDED &&
      result.result &&
      result.result->error_code == FollowJointTrajectory::Result::SUCCESSFUL;
  if (!success) {
    std::string detail =
        result.result ? result.result->error_string : "controller returned no result";
    if (detail.empty()) {
      detail = "controller returned non-success result";
    }
    set_command_gate(false);
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorExecutionAborted,
        "commanded_sim reset goal failed: " + detail,
        {{"phase", std::move(phase)}, {"action_name", config_.action_name}},
    });
  }

  update_scene_positions(config_.initial_joint_positions_rad);
  publish_snapshot(core::ExecutionState::kIdle, 0.0,
                   config_.initial_joint_positions_rad);
  return core::Result<void>::Ok();
}

core::Result<void> Ros2ControlExecutionBackend::sync_observed_state_before_planning() {
  auto resync_result = resync_after_intervention_if_needed("pre_plan_resync");
  if (!resync_result.ok()) {
    return resync_result;
  }

  if (uses_authored_backend_state()) {
    return core::Result<void>::Ok();
  }
  if (!config_.require_fresh_state_before_planning) {
    return core::Result<void>::Ok();
  }

  auto sample = wait_for_fresh_sample(config_.wait_for_state_timeout_s);
  if (!sample.has_value()) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorStartStateMismatch,
        "No fresh complete observed robot state before planning",
        {
            {"state_source_mode", config_.execution_state_source_mode},
            {"max_age_s", std::to_string(config_.joint_state_max_age_s)},
            {"timeout_s", std::to_string(config_.wait_for_state_timeout_s)},
        },
    });
  }

  std::vector<double> positions;
  try {
    positions = positions_for_joints(*sample, config_.required_joints);
  } catch (const std::exception& exc) {
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorStartStateMismatch,
        std::string("Observed robot state is incomplete before planning: ") + exc.what(),
        {
            {"state_source_mode", config_.execution_state_source_mode},
            {"sample_source", sample->source},
            {"sample_topic", sample->topic},
        },
    });
  }

  update_scene_positions(positions);
  publish_snapshot(core::ExecutionState::kIdle, 0.0, positions);
  return core::Result<void>::Ok();
}

std::optional<Ros2ControlExecutionBackend::JointSample>
Ros2ControlExecutionBackend::wait_for_fresh_sample(double timeout_s) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(timeout_s));
  std::unique_lock<std::mutex> lock(state_mutex_);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    auto sample = select_latest_sample_locked(now);
    if (sample.has_value()) {
      try {
        (void)positions_for_joints(*sample, config_.required_joints);
        return sample;
      } catch (const std::exception&) {
        // Keep waiting for a complete sample.
      }
    }
    if (now >= deadline) {
      return std::nullopt;
    }
    state_cv_.wait_until(lock, deadline);
  }
}

std::optional<Ros2ControlExecutionBackend::JointSample>
Ros2ControlExecutionBackend::wait_for_fresh_sample_after(
    double timeout_s,
    std::chrono::steady_clock::time_point not_before) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(timeout_s));
  std::unique_lock<std::mutex> lock(state_mutex_);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    auto sample = select_latest_sample_locked(now);
    if (sample.has_value() && sample->received_at >= not_before) {
      try {
        (void)positions_for_joints(*sample, config_.required_joints);
        return sample;
      } catch (const std::exception&) {
        // Keep waiting for a complete sample.
      }
    }
    if (now >= deadline) {
      return std::nullopt;
    }
    state_cv_.wait_until(lock, deadline);
  }
}

std::vector<double> Ros2ControlExecutionBackend::positions_for_joints(
    const JointSample& sample,
    const std::vector<std::string>& joints) const {
  std::vector<double> result;
  result.reserve(joints.size());
  for (const auto& joint : joints) {
    const auto it = std::find(sample.joint_names.begin(), sample.joint_names.end(), joint);
    if (it == sample.joint_names.end()) {
      throw std::runtime_error("sample missing joint " + joint);
    }
    const auto index = static_cast<size_t>(std::distance(sample.joint_names.begin(), it));
    if (index >= sample.positions.size()) {
      throw std::runtime_error("sample missing position for joint " + joint);
    }
    result.push_back(sample.positions[index]);
  }
  return result;
}

bool Ros2ControlExecutionBackend::update_scene_positions(
    const std::vector<double>& joint_positions,
    SceneUpdateMode mode) {
  if (joint_positions.empty()) {
    return true;
  }
  std::unique_lock<std::mutex> scene_lock(*scene_mutex_, std::defer_lock);
  if (mode == SceneUpdateMode::kBestEffort) {
    if (!scene_lock.try_lock()) {
      return false;
    }
  } else {
    scene_lock.lock();
  }
  Eigen::VectorXd q(joint_positions.size());
  for (size_t i = 0; i < joint_positions.size(); ++i) {
    q[static_cast<Eigen::Index>(i)] = joint_positions[i];
  }
  if (q.size() != scene_->getCurrentJointPositions().size()) {
    q = scene_->toFullJointPositions(group_name_, q);
  }
  scene_->setJointPositions(q);
  return true;
}

std::optional<double> Ros2ControlExecutionBackend::scene_group_joint_position(
    const std::string& group_id,
    const std::string& joint_name) const {
  if (group_id.empty() || joint_name.empty() || !scene_) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> scene_lock(*scene_mutex_);
  const auto maybe_group_info = scene_->getJointGroupInfo(group_id);
  if (!maybe_group_info) {
    return std::nullopt;
  }
  const auto& group_info = *maybe_group_info;
  const auto joint_it =
      std::find(group_info.joint_names.begin(), group_info.joint_names.end(),
                joint_name);
  if (joint_it == group_info.joint_names.end()) {
    return std::nullopt;
  }
  const auto index =
      static_cast<size_t>(std::distance(group_info.joint_names.begin(), joint_it));
  if (index >= static_cast<size_t>(group_info.q_indices.size())) {
    return std::nullopt;
  }
  const auto q = scene_->getCurrentJointPositions();
  const auto q_index = group_info.q_indices[static_cast<Eigen::Index>(index)];
  if (q_index < 0 || q_index >= q.size()) {
    return std::nullopt;
  }
  return q[q_index];
}

bool Ros2ControlExecutionBackend::update_scene_group_joint_position(
    const std::string& group_id,
    const std::string& joint_name,
    double position,
    SceneUpdateMode mode) {
  if (group_id.empty() || joint_name.empty() || !std::isfinite(position) || !scene_) {
    return false;
  }
  std::unique_lock<std::mutex> scene_lock(*scene_mutex_, std::defer_lock);
  if (mode == SceneUpdateMode::kBestEffort) {
    if (!scene_lock.try_lock()) {
      return false;
    }
  } else {
    scene_lock.lock();
  }
  const auto maybe_group_info = scene_->getJointGroupInfo(group_id);
  if (!maybe_group_info) {
    return false;
  }
  const auto& group_info = *maybe_group_info;
  const auto joint_it =
      std::find(group_info.joint_names.begin(), group_info.joint_names.end(),
                joint_name);
  if (joint_it == group_info.joint_names.end()) {
    return false;
  }
  if (group_info.joint_names.size() !=
      static_cast<size_t>(group_info.q_indices.size())) {
    return false;
  }
  const auto q_current = scene_->getCurrentJointPositions();
  Eigen::VectorXd group_q(group_info.q_indices.size());
  for (Eigen::Index i = 0; i < group_info.q_indices.size(); ++i) {
    const auto q_index = group_info.q_indices[i];
    if (q_index < 0 || q_index >= q_current.size()) {
      return false;
    }
    group_q[i] = q_current[q_index];
  }
  const auto group_index =
      static_cast<Eigen::Index>(std::distance(group_info.joint_names.begin(), joint_it));
  group_q[group_index] = position;
  auto q_full = group_q;
  if (q_full.size() != q_current.size()) {
    q_full = scene_->toFullJointPositions(group_id, group_q);
  }
  scene_->setJointPositions(q_full);
  return true;
}

void Ros2ControlExecutionBackend::project_gripper_position_to_scene(
    const Ros2GripperActionConfig& config,
    double position,
    SceneUpdateMode mode) {
  if (config.group_id.empty() || config.joint_name.empty()) {
    return;
  }
  (void)update_scene_group_joint_position(config.group_id, config.joint_name,
                                          position, mode);
}

void Ros2ControlExecutionBackend::publish_snapshot(
    core::ExecutionState state,
    double progress,
    const std::vector<double>& joint_positions,
    std::optional<core::ErrorDTO> error,
    std::string state_source) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto now_ns = clock_.now();
  snapshot_.timestamp_ns = now_ns;
  snapshot_.sequence_number++;
  snapshot_.state = state;
  snapshot_.progress = clamp_progress(progress);
  snapshot_.joint_positions_rad = joint_positions;
  if (state_source.empty()) {
    if (is_commanded_sim_mode()) {
      state_source = "commanded_sim_predicted";
    } else if (config_.execution_state_source_mode == "authored_preview") {
      state_source = "authored_preview";
    } else {
      state_source = "observed_ros2";
    }
  }
  snapshot_.state_source = std::move(state_source);
  snapshot_.source_timestamp_ns = now_ns;
  snapshot_.source_age_ns = 0;
  snapshot_.error = std::move(error);
}

core::ErrorDTO Ros2ControlExecutionBackend::operator_intervention_error(
    core::ErrorCode code,
    std::string message,
    std::string phase,
    core::ExecutionState backend_state) const {
  return make_error(
      code,
      std::move(message),
      {{"requires_operator_intervention", "true"},
       {"resync_required", "true"},
       {"phase", std::move(phase)},
       {"backend_state", core::to_string(backend_state)},
       {"action_name", config_.action_name},
       {"stop_completion_timeout_s",
        std::to_string(config_.stop_completion_timeout_s)}});
}

core::Result<void> Ros2ControlExecutionBackend::resync_after_intervention_if_needed(
    std::string phase) {
  {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    if (!resync_required_) {
      return core::Result<void>::Ok();
    }
    if (resync_in_progress_) {
      const auto timeout =
          std::chrono::duration<double>(config_.stop_completion_timeout_s +
                                        config_.wait_for_state_timeout_s);
      const bool finished = execution_cv_.wait_for(
          lock, timeout, [this]() { return !resync_in_progress_; });
      if (!finished) {
        lock.unlock();
        const auto snapshot = get_execution_snapshot();
        return core::Result<void>::Err(operator_intervention_error(
            core::ErrorCode::kResetFailed,
            "Cannot resync ros2_control backend: another resync is still in progress",
            phase,
            snapshot.state));
      }
      if (!resync_required_) {
        return core::Result<void>::Ok();
      }
    }
    if (running_) {
      lock.unlock();
      const auto snapshot = get_execution_snapshot();
      return core::Result<void>::Err(operator_intervention_error(
          core::ErrorCode::kResetFailed,
          "Cannot resync ros2_control backend while execution is still in flight",
          phase,
          snapshot.state));
    }
    resync_in_progress_ = true;
  }
  const auto resync_guard = std::shared_ptr<void>(
      nullptr,
      [this](void*) {
        {
          std::lock_guard<std::mutex> lock(execution_mutex_);
          resync_in_progress_ = false;
        }
        execution_cv_.notify_all();
      });
  (void)resync_guard;

  if (monitor_thread_.joinable() && monitor_active_.load()) {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    const bool monitor_stopped = execution_cv_.wait_for(
        lock,
        std::chrono::duration<double>(config_.stop_completion_timeout_s),
        [this]() { return !monitor_active_.load(); });
    if (!monitor_stopped) {
      lock.unlock();
      const auto snapshot = get_execution_snapshot();
      return core::Result<void>::Err(operator_intervention_error(
          core::ErrorCode::kResetFailed,
          "Cannot resync ros2_control backend: monitor thread is still stopping",
          phase,
          snapshot.state));
    }
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }

  if (is_commanded_sim_mode()) {
    auto reset_result = command_sim_initial_state(phase + "_commanded_sim_reset");
    if (!reset_result.ok()) {
      return reset_result;
    }
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      resync_required_ = false;
      running_ = false;
      terminal_ = true;
      stop_requested_ = false;
      pause_requested_ = false;
      goal_accepted_ = false;
      first_feedback_seen_ = false;
      result_received_ = false;
      active_goal_handle_.reset();
      active_goal_id_.reset();
      active_trajectory_.reset();
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = 1.0;
      stop_requested_at_.reset();
      tracking_error_since_.reset();
    }
    execution_cv_.notify_all();
    return core::Result<void>::Ok();
  }

  auto sample = wait_for_fresh_sample(config_.wait_for_state_timeout_s);
  if (!sample.has_value()) {
    const auto snapshot = get_execution_snapshot();
    return core::Result<void>::Err(operator_intervention_error(
        core::ErrorCode::kStartStateMismatch,
        "Cannot resync ros2_control backend: no fresh complete controller state sample",
        phase,
        snapshot.state));
  }

  std::vector<double> positions;
  try {
    positions = positions_for_joints(*sample, config_.required_joints);
  } catch (const std::exception& exc) {
    const auto snapshot = get_execution_snapshot();
    return core::Result<void>::Err(operator_intervention_error(
        core::ErrorCode::kStartStateMismatch,
        std::string("Cannot resync ros2_control backend: observed state is incomplete: ") +
            exc.what(),
        phase,
        snapshot.state));
  }

  update_scene_positions(positions);
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resync_required_ = false;
    running_ = false;
    terminal_ = true;
    stop_requested_ = false;
    pause_requested_ = false;
    goal_accepted_ = false;
    first_feedback_seen_ = false;
    result_received_ = false;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
    stop_requested_at_.reset();
    tracking_error_since_.reset();
  }
  publish_snapshot(core::ExecutionState::kIdle, 0.0, positions);
  execution_cv_.notify_all();
  return core::Result<void>::Ok();
}

void Ros2ControlExecutionBackend::cancel_goal_if_known(
    const GoalHandleFJT::SharedPtr& goal_handle) noexcept {
  if (!goal_handle) {
    return;
  }
  try {
    (void)action_client_->async_cancel_goal(goal_handle);
  } catch (const std::exception&) {
    // rclcpp_action may throw if the action result already removed this goal
    // handle from the client. Stop/reset/destruction must remain idempotent.
  }
}

std::string Ros2ControlExecutionBackend::latest_feedback_error_summary() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!peak_tracking_error_.has_value() || peak_tracking_error_->abs_error_rad <= 0.0) {
    return {};
  }
  const auto& peak = *peak_tracking_error_;
  std::ostringstream out;
  out << std::fixed << std::setprecision(4)
      << "peak controller feedback max position error: " << peak.joint_name
      << "=" << peak.signed_error_rad << " rad"
      << " (abs=" << peak.abs_error_rad << " rad"
      << ", elapsed=" << peak.elapsed_s << " s)";
  if (peak.has_expected_position && peak.has_observed_position) {
    out << " expected=" << peak.expected_position_rad
        << " observed=" << peak.observed_position_rad;
  }
  if (peak.has_observed_velocity) {
    out << " observed_velocity=" << peak.observed_velocity_rad_s << " rad/s";
  }
  return out.str();
}

std::optional<Ros2ControlExecutionBackend::TrackingErrorPeak>
Ros2ControlExecutionBackend::update_tracking_error_peak_locked(
    const JointSample& error_sample,
    double elapsed_s,
    const std::vector<double>* observed_positions,
    const std::vector<double>* expected_positions,
    const std::vector<double>* observed_velocities) {
  if (error_sample.positions.empty()) {
    return peak_tracking_error_;
  }
  size_t max_index = 0;
  double max_abs_error = 0.0;
  for (size_t i = 0; i < error_sample.positions.size(); ++i) {
    const double abs_error = std::abs(error_sample.positions[i]);
    if (abs_error > max_abs_error) {
      max_abs_error = abs_error;
      max_index = i;
    }
  }
  if (max_abs_error <= 0.0 ||
      (peak_tracking_error_.has_value() &&
       max_abs_error <= peak_tracking_error_->abs_error_rad)) {
    return peak_tracking_error_;
  }

  TrackingErrorPeak peak;
  peak.joint_name =
      max_index < error_sample.joint_names.size()
          ? error_sample.joint_names[max_index]
          : "joint[" + std::to_string(max_index) + "]";
  peak.signed_error_rad = error_sample.positions[max_index];
  peak.abs_error_rad = max_abs_error;
  peak.elapsed_s = elapsed_s;
  if (expected_positions != nullptr && max_index < expected_positions->size()) {
    peak.expected_position_rad = (*expected_positions)[max_index];
    peak.has_expected_position = true;
  }
  if (observed_positions != nullptr && max_index < observed_positions->size()) {
    peak.observed_position_rad = (*observed_positions)[max_index];
    peak.has_observed_position = true;
  }
  if (observed_velocities != nullptr && max_index < observed_velocities->size()) {
    peak.observed_velocity_rad_s = (*observed_velocities)[max_index];
    peak.has_observed_velocity = true;
  }
  peak_tracking_error_ = peak;
  return peak_tracking_error_;
}

void Ros2ControlExecutionBackend::fail_execution(std::string message,
                                                 core::ErrorCode code) {
  const auto snapshot = get_execution_snapshot();
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_) {
      return;
    }
    running_ = false;
    terminal_ = true;
    resync_required_ = true;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
  }
  publish_snapshot(core::ExecutionState::kFailed,
                   snapshot.progress,
                   snapshot.joint_positions_rad,
                   make_error(code, std::move(message)));
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::require_operator_intervention(
    std::string message,
    core::ErrorCode code) {
  const auto snapshot = get_execution_snapshot();
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_) {
      return;
    }
    running_ = false;
    terminal_ = true;
    resync_required_ = true;
    active_goal_handle_.reset();
    active_goal_id_.reset();
    active_trajectory_.reset();
    active_trajectory_time_s_ = 0.0;
    active_runtime_velocity_scale_ = 1.0;
  }
  publish_snapshot(
      core::ExecutionState::kInterventionRequired,
      snapshot.progress,
      snapshot.joint_positions_rad,
      operator_intervention_error(
          code, std::move(message), "controller_cancel_timeout", snapshot.state));
  execution_cv_.notify_all();
}

void Ros2ControlExecutionBackend::complete_execution() {
  const auto completed_at = std::chrono::steady_clock::now();
  std::vector<double> final_positions = get_execution_snapshot().joint_positions_rad;
  if (!uses_authored_backend_state()) {
    auto sample = wait_for_fresh_sample_after(
        config_.wait_for_state_timeout_s, completed_at);
    if (!sample.has_value()) {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      sample = select_latest_sample_locked(std::chrono::steady_clock::now());
    }
    if (sample.has_value()) {
      try {
        final_positions = positions_for_joints(*sample, config_.required_joints);
      } catch (const std::exception&) {
        // Keep the last snapshot positions.
      }
    }
  }
  if (!final_positions.empty()) {
    update_scene_positions(final_positions);
  }
  publish_snapshot(core::ExecutionState::kCompleted, 1.0, final_positions);
  execution_cv_.notify_all();
}

bool Ros2ControlExecutionBackend::start_state_matches(
    const core::TrajectoryDTO& trajectory,
    const JointSample& sample,
    std::string& error) const {
  if (trajectory.waypoints.empty()) {
    return true;
  }
  std::vector<double> observed;
  try {
    observed = positions_for_joints(sample, trajectory.joint_names);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  const auto& start = trajectory.waypoints.front().positions_rad;
  const double max_error = max_abs_error(observed, start);
  if (max_error > config_.start_tolerance_rad) {
    error = "Start state mismatch: max joint error " + std::to_string(max_error) +
            " rad exceeds tolerance " + std::to_string(config_.start_tolerance_rad);
    return false;
  }
  return true;
}

bool Ros2ControlExecutionBackend::final_state_matches(
    const core::TrajectoryDTO& trajectory,
    const JointSample& sample,
    std::string& error) const {
  if (trajectory.waypoints.empty()) {
    return true;
  }
  std::vector<double> observed;
  try {
    observed = positions_for_joints(sample, trajectory.joint_names);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
  const auto& goal = trajectory.waypoints.back().positions_rad;
  const double max_error = max_abs_error(observed, goal);
  if (max_error > config_.final_tolerance_rad) {
    error = "Final state mismatch: max joint error " + std::to_string(max_error) +
            " rad exceeds tolerance " + std::to_string(config_.final_tolerance_rad);
    return false;
  }
  return true;
}

double Ros2ControlExecutionBackend::max_abs_error(
    const std::vector<double>& lhs,
    const std::vector<double>& rhs) const {
  const size_t count = std::min(lhs.size(), rhs.size());
  double max_error = 0.0;
  for (size_t i = 0; i < count; ++i) {
    max_error = std::max(max_error, std::abs(lhs[i] - rhs[i]));
  }
  return max_error;
}

core::ExecutionHandle Ros2ControlExecutionBackend::execute_trajectory(
    const core::TrajectoryDTO& trajectory) {
  auto resync_result = resync_after_intervention_if_needed("execute_resync");
  if (!resync_result.ok()) {
    const auto snapshot = get_execution_snapshot();
    publish_snapshot(core::ExecutionState::kInterventionRequired,
                     snapshot.progress,
                     snapshot.joint_positions_rad,
                     resync_result.error());
    return core::ExecutionHandle{
        [](int64_t) { return true; },
        [this]() { return get_execution_snapshot(); }};
  }

  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (running_) {
      publish_snapshot(
          core::ExecutionState::kFailed,
          get_execution_snapshot().progress,
          get_execution_snapshot().joint_positions_rad,
          make_error(core::ErrorCode::kInvalidRequest, "Execution already in flight"));
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    } else {
      running_ = true;
      terminal_ = false;
      stop_requested_ = false;
      pause_requested_ = false;
      goal_accepted_ = false;
      first_feedback_seen_ = false;
      result_received_ = false;
      active_goal_handle_.reset();
      active_goal_id_.reset();
      active_trajectory_.reset();
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = 1.0;
      stop_requested_at_.reset();
      tracking_error_since_.reset();
      peak_tracking_error_.reset();
      execution_started_at_ = std::chrono::steady_clock::now();
      const double initial_runtime_scale =
          config_.enable_jtc_speed_scaling_input
              ? runtime_velocity_scale_.load(std::memory_order_acquire)
              : 1.0;
      applied_runtime_velocity_scale_.store(initial_runtime_scale,
                                            std::memory_order_release);
      active_trajectory_ = trajectory;
      active_trajectory_time_s_ = 0.0;
      active_runtime_velocity_scale_ = initial_runtime_scale;
    }
  }

  std::vector<double> start_positions;
  if (uses_authored_backend_state()) {
    if (trajectory.waypoints.empty()) {
      fail_execution("Trajectory has no waypoints");
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
    start_positions = trajectory.waypoints.front().positions_rad;
  } else {
    auto start_sample = wait_for_fresh_sample(config_.wait_for_state_timeout_s);
    if (!start_sample.has_value()) {
      fail_execution("No fresh complete controller state sample before dispatch");
      return core::ExecutionHandle{
          [this](int64_t timeout_ns) {
            std::unique_lock<std::mutex> lock(execution_mutex_);
            if (timeout_ns > 0) {
              return execution_cv_.wait_for(
                  lock, std::chrono::nanoseconds(timeout_ns),
                  [this]() { return terminal_; });
            }
            execution_cv_.wait(lock, [this]() { return terminal_; });
            return true;
          },
          [this]() { return get_execution_snapshot(); }};
    }

    std::string start_error;
    if (!start_state_matches(trajectory, *start_sample, start_error)) {
      fail_execution(start_error);
      return core::ExecutionHandle{
          [](int64_t) { return true; },
          [this]() { return get_execution_snapshot(); }};
    }
    start_positions = positions_for_joints(*start_sample, config_.required_joints);
  }

  update_scene_positions(start_positions);
  publish_snapshot(core::ExecutionState::kExecuting, 0.0, start_positions);

  if (!wait_for_fjt_action_server(action_client_,
                                  config_.wait_for_action_server_timeout_s)) {
    fail_execution("FollowJointTrajectory action server unavailable: " +
                   config_.action_name);
    return core::ExecutionHandle{
        [](int64_t) { return true; },
        [this]() { return get_execution_snapshot(); }};
  }

  FollowJointTrajectory::Goal goal;
  goal.trajectory.joint_names = trajectory.joint_names;
  if (config_.disable_controller_path_tolerances) {
    disable_fjt_controller_tolerances(goal, trajectory.joint_names);
  }
  publish_runtime_velocity_scale(
      runtime_velocity_scale_.load(std::memory_order_acquire));
  for (const auto& waypoint : trajectory.waypoints) {
    trajectory_msgs::msg::JointTrajectoryPoint point;
    point.positions = waypoint.positions_rad;
    point.velocities = waypoint.velocities_rad_s;
    point.accelerations = waypoint.accelerations_rad_s2;
    point.time_from_start = duration_from_seconds(waypoint.time_from_start_s);
    goal.trajectory.points.push_back(std::move(point));
  }

  rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions options;
  options.feedback_callback =
      [this](GoalHandleFJT::SharedPtr goal_handle,
             const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback) {
        on_action_feedback(goal_handle, feedback);
      };
  options.result_callback =
      [this](const GoalHandleFJT::WrappedResult& result) {
        on_action_result(result);
      };

  auto goal_handle_future = action_client_->async_send_goal(goal, options);
  if (goal_handle_future.wait_for(
          std::chrono::duration<double>(config_.wait_for_action_server_timeout_s)) !=
      std::future_status::ready) {
    fail_execution("Timed out waiting for FollowJointTrajectory goal response: " +
                   config_.action_name);
    return core::ExecutionHandle{
        [](int64_t) { return true; },
        [this]() { return get_execution_snapshot(); }};
  }

  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception& exc) {
    fail_execution(std::string("FollowJointTrajectory goal dispatch failed: ") + exc.what());
    return core::ExecutionHandle{
        [](int64_t) { return true; },
        [this]() { return get_execution_snapshot(); }};
  }
  if (!goal_handle) {
    fail_execution("FollowJointTrajectory goal rejected");
    return core::ExecutionHandle{
        [](int64_t) { return true; },
        [this]() { return get_execution_snapshot(); }};
  }

  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    active_goal_handle_ = goal_handle;
    active_goal_id_ = goal_handle->get_goal_id();
    // Reset the persisted action result code at each new dispatch
    // so a fresh execute_controlled_stop call doesn't observe
    // stale codes from the previous trajectory.
    last_action_result_code_.reset();
    goal_accepted_ = true;
    execution_started_at_ = std::chrono::steady_clock::now();
  }
  if (is_commanded_sim_mode()) {
    set_command_gate(true);
  }

  if (monitor_thread_.joinable() && monitor_active_.load()) {
    fail_execution("Previous ros2_control monitor thread is still shutting down");
    return core::ExecutionHandle{
        [](int64_t) { return true; },
        [this]() { return get_execution_snapshot(); }};
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  monitor_thread_ =
      std::thread([this, trajectory_copy = core::TrajectoryDTO(trajectory)]() mutable {
        monitor_worker(std::move(trajectory_copy));
      });

  return core::ExecutionHandle{
      [this](int64_t timeout_ns) {
        std::unique_lock<std::mutex> lock(execution_mutex_);
        if (timeout_ns > 0) {
          return execution_cv_.wait_for(
              lock, std::chrono::nanoseconds(timeout_ns),
              [this]() { return terminal_; });
        }
        execution_cv_.wait(lock, [this]() { return terminal_; });
        return true;
      },
      [this]() { return get_execution_snapshot(); }};
}

void Ros2ControlExecutionBackend::monitor_worker(core::TrajectoryDTO trajectory) {
  monitor_active_.store(true);
  const auto monitor_guard =
      std::shared_ptr<void>(nullptr, [this](void*) {
        monitor_active_.store(false);
        execution_cv_.notify_all();
      });
  (void)monitor_guard;

  const double total_duration_s =
      trajectory.waypoints.empty() ? 0.0 : trajectory.waypoints.back().time_from_start_s;
  auto last_monitor_tick = std::chrono::steady_clock::now();
  double trajectory_time_s = 0.0;
  double applied_runtime_scale =
      applied_runtime_velocity_scale_.load(std::memory_order_acquire);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    bool cancel_pending = false;
    bool cancel_timeout = false;
    bool waiting_for_first_feedback = false;
    bool first_feedback_timeout = false;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      if (terminal_ || !running_) {
        return;
      }
      cancel_pending = stop_requested_ || pause_requested_;
      cancel_timeout =
          cancel_pending && stop_requested_at_.has_value() &&
          seconds_since(*stop_requested_at_, now) >= config_.stop_completion_timeout_s;
      waiting_for_first_feedback = !first_feedback_seen_;
      first_feedback_timeout =
          waiting_for_first_feedback &&
          seconds_since(execution_started_at_, now) >= config_.wait_for_state_timeout_s;
    }
    if (cancel_timeout) {
      require_operator_intervention(
          "Timed out waiting for FollowJointTrajectory cancel confirmation");
      return;
    }
    if (cancel_pending) {
      const auto snapshot = get_execution_snapshot();
      publish_snapshot(core::ExecutionState::kStopping,
                       snapshot.progress,
                       snapshot.joint_positions_rad);
      std::this_thread::sleep_for(
          std::chrono::duration<double>(config_.monitor_period_s));
      continue;
    }
    if (first_feedback_timeout) {
      fail_execution("Timed out waiting for first FollowJointTrajectory feedback");
      return;
    }
    if (waiting_for_first_feedback) {
      last_monitor_tick = now;
      std::vector<double> snapshot_positions;
      if (uses_authored_backend_state() && !trajectory.waypoints.empty()) {
        snapshot_positions = trajectory.waypoints.front().positions_rad;
      } else {
        std::lock_guard<std::mutex> state_lock(state_mutex_);
        auto sample = select_latest_sample_locked(now);
        if (sample.has_value()) {
          try {
            snapshot_positions = positions_for_joints(*sample, trajectory.joint_names);
          } catch (const std::exception&) {
            snapshot_positions.clear();
          }
        }
      }
      if (snapshot_positions.empty() && !trajectory.waypoints.empty()) {
        snapshot_positions = trajectory.waypoints.front().positions_rad;
      }
      publish_snapshot(core::ExecutionState::kExecuting, 0.0, snapshot_positions);
      std::this_thread::sleep_for(
          std::chrono::duration<double>(config_.monitor_period_s));
      continue;
    }
    const double dt_s =
        std::max(0.0, std::chrono::duration<double>(now - last_monitor_tick).count());
    last_monitor_tick = now;
    const double target_scale =
        config_.enable_jtc_speed_scaling_input
            ? clamp_progress(
                  runtime_velocity_scale_.load(std::memory_order_acquire))
            : 1.0;
    const double max_delta = kRuntimeScaleSlewPerSecond * dt_s;
    if (target_scale > applied_runtime_scale + max_delta) {
      applied_runtime_scale += max_delta;
    } else if (target_scale < applied_runtime_scale - max_delta) {
      applied_runtime_scale -= max_delta;
    } else {
      applied_runtime_scale = target_scale;
    }
    applied_runtime_velocity_scale_.store(
        applied_runtime_scale, std::memory_order_release);
    trajectory_time_s += dt_s * applied_runtime_scale;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      active_trajectory_time_s_ = trajectory_time_s;
      active_runtime_velocity_scale_ = applied_runtime_scale;
    }
    const double progress =
        total_duration_s <= 1e-9 ? 1.0 : clamp_progress(trajectory_time_s / total_duration_s);
    std::vector<double> predicted_positions;
    {
      auto sample =
          core::sample_trajectory(trajectory, trajectory_time_s);
      if (sample.ok()) {
        predicted_positions = std::move(sample.value().positions_rad);
      } else {
        std::cerr
            << "trajectory_sampler error in Ros2ControlExecutionBackend "
               "monitor predicted-state: "
            << sample.error().code << " (time_s=" << trajectory_time_s
            << ", waypoints=" << trajectory.waypoints.size() << ")"
            << std::endl;
      }
    }
    if (!predicted_positions.empty()) {
      update_scene_positions(predicted_positions, SceneUpdateMode::kBestEffort);
    }

    std::vector<double> snapshot_positions = predicted_positions;
    std::optional<std::string> tracking_failure;
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      auto sample = select_latest_sample_locked(now);
      if (sample.has_value()) {
        try {
          auto observed = positions_for_joints(*sample, trajectory.joint_names);
          if (!uses_authored_backend_state()) {
            snapshot_positions = observed;
          }
          std::vector<double> expected;
          {
            auto expected_sample =
                core::sample_trajectory(trajectory, trajectory_time_s);
            if (expected_sample.ok()) {
              expected = std::move(expected_sample.value().positions_rad);
            } else {
              std::cerr
                  << "trajectory_sampler error in Ros2ControlExecutionBackend "
                     "monitor tracking-error: "
                  << expected_sample.error().code
                  << " (time_s=" << trajectory_time_s
                  << ", waypoints=" << trajectory.waypoints.size()
                  << ")" << std::endl;
            }
          }
          // Guard: an empty expected vector means either the
          // trajectory was empty (no predicted state to compare
          // against) or the sampler refused the input (logged
          // separately). max_abs_error compares only
          // min(lhs.size(), rhs.size()) and would silently return
          // 0.0 on a zero-length overlap, which would falsely
          // declare tracking-clean. Skip the diagnostic computation
          // for this tick when the predicted state is unavailable;
          // the monitor's snapshot publish and sleep continue
          // normally after the diagnostic block.
          if (expected.empty()) {
            // Fall through to the post-try monitor publish/sleep
            // without computing or accumulating tracking error.
          } else {
          const double error = max_abs_error(observed, expected);
          JointSample error_sample;
          error_sample.joint_names = trajectory.joint_names;
          error_sample.positions.reserve(observed.size());
          for (size_t i = 0; i < std::min(observed.size(), expected.size()); ++i) {
            error_sample.positions.push_back(observed[i] - expected[i]);
          }
          error_sample.velocities = sample->velocities;
          error_sample.received_at = now;
          error_sample.source = sample->source + "_tracking_error";
          error_sample.topic = sample->topic;
          update_tracking_error_peak_locked(
              error_sample, trajectory_time_s, &observed, &expected, &sample->velocities);
          if (error > config_.tracking_error_tolerance_rad &&
              config_.tracking_error_timeout_s > 0.0) {
            if (!tracking_error_since_.has_value()) {
              tracking_error_since_ = now;
            }
            if (seconds_since(*tracking_error_since_, now) >=
                config_.tracking_error_timeout_s) {
              tracking_failure =
                  "Controller tracking error exceeded tolerance: " +
                  std::to_string(error) + " rad";
            }
          } else {
            tracking_error_since_.reset();
          }
          }  // closes the `if (!expected.empty()) else` block above.
        } catch (const std::exception&) {
          // A malformed/incomplete sample is ignored here; start gate already
          // proves that at least one complete controller sample existed.
        }
      }
    }
    if (tracking_failure.has_value()) {
      const auto feedback_error = latest_feedback_error_summary();
      if (!feedback_error.empty()) {
        *tracking_failure += "; " + feedback_error;
      }
      GoalHandleFJT::SharedPtr handle_to_cancel;
      {
        std::lock_guard<std::mutex> lock(execution_mutex_);
        handle_to_cancel = active_goal_handle_;
      }
      cancel_goal_if_known(handle_to_cancel);
      fail_execution(*tracking_failure);
      return;
    }
    publish_snapshot(core::ExecutionState::kExecuting, progress, snapshot_positions);
    std::this_thread::sleep_for(
        std::chrono::duration<double>(config_.monitor_period_s));
  }
}

core::StopResultDTO Ros2ControlExecutionBackend::stop() {
  core::ExecutionSnapshot snapshot;
  bool cancel_requested = false;
  bool resync_required = false;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resync_required = resync_required_;
    if (resync_required_) {
      snapshot = get_execution_snapshot();
    } else if (!running_ && terminal_) {
      return core::StopResultDTO{
          false, make_error(core::ErrorCode::kInvalidRequest, "No execution in flight")};
    } else {
      stop_requested_ = true;
      pause_requested_ = false;
      stop_requested_at_ = std::chrono::steady_clock::now();
      cancel_requested = static_cast<bool>(active_goal_handle_);
      if (cancel_requested) {
        cancel_goal_if_known(active_goal_handle_);
        snapshot = get_execution_snapshot();
      } else {
        running_ = false;
        terminal_ = true;
        snapshot = get_execution_snapshot();
      }
    }
  }
  if (resync_required) {
    return core::StopResultDTO{
        false,
        operator_intervention_error(
            core::ErrorCode::kStopFailed,
            "ros2_control execution backend requires observed-state resync before reuse",
            "stop_resync_required",
            snapshot.state)};
  }
  publish_snapshot(cancel_requested ? core::ExecutionState::kStopping
                                    : core::ExecutionState::kStopped,
                   snapshot.progress,
                   snapshot.joint_positions_rad);
  cancel_all_gripper_commands();
  execution_cv_.notify_all();
  return core::StopResultDTO{true, std::nullopt};
}

// Controlled-stop capability + result surface.
//
// The ros2_control adapter exposes `kSpringBackHold` only in v1.
// `kSoftOnPath` requires a computed replacement trajectory with
// path-deviation evidence; `kHardDeceleration` requires
// `decelerate_on_cancel=true` + per-joint deceleration limits in the
// controller config. Declaring only the qualified mode keeps
// the kernel's pre-dispatch capability gate honest.

core::MotionInterruptStopCapabilityDTO
Ros2ControlExecutionBackend::compute_motion_interrupt_stop_capability(
    const Ros2ControlExecutionBackendConfig& config) {
  return compute_ros2_control_motion_interrupt_stop_capability(config);
}

core::MotionInterruptStopCapabilityDTO
Ros2ControlExecutionBackend::get_motion_interrupt_stop_capability() const {
  return compute_motion_interrupt_stop_capability(config_);
}

core::ControlledStopResultDTO
Ros2ControlExecutionBackend::execute_controlled_stop(
    const core::MotionInterruptStopProfileDTO& profile) {
  core::ControlledStopResultDTO result;
  result.executed_stop_mode = profile.stop_mode;

  // Defense in depth: kernel arm-time gate already rejects modes the
  // backend has not declared; this guard is a second line of defense
  // against a mis-wired call. The capability set is the source of
  // truth — re-evaluate it from the current config so deployments
  // that have been reconfigured at runtime cannot slip through.
  const auto current_capability =
      compute_motion_interrupt_stop_capability(config_);
  if (!current_capability.supported_stop_modes.contains(profile.stop_mode)) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "requested stop_mode is not in this backend's capability profile",
        {{"reason", "stop_mode_not_supported_by_backend"},
         {"backend_capability_profile_id",
          current_capability.capability_profile_id.value_or("")}}};
    return result;
  }

  // Capture the observed joint state BEFORE preempting so the
  // recorded anchor reflects what the controller actually committed
  // to. Use the existing freshness-aware selector so stale samples
  // (older than `joint_state_max_age_s`) fail closed with a
  // structured diagnostic — the kernel relies on this anchor for the
  // signal_trip outcome's `ExecutionStopSnapshotDTO`, and a stale
  // sample would silently certify the wrong stop position.
  const std::vector<std::string> joint_names = config_.required_joints;
  std::vector<double> observed_positions;
  std::vector<double> observed_velocities;
  std::string observed_sample_source;
  double observed_sample_age_s = 0.0;
  {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto sample = select_latest_sample_locked(now);
    if (!sample.has_value()) {
      // Build a precise age diagnostic from the freshest available
      // sample (regardless of freshness) so operators can see WHICH
      // source went stale and by how much. When no sample exists at
      // all, report -1 s to distinguish from a fresh-zero age.
      double freshest_age_s = -1.0;
      std::string freshest_source;
      const auto consider = [&](const std::optional<JointSample>& s) {
        if (s.has_value()) {
          const double age =
              std::chrono::duration_cast<std::chrono::duration<double>>(
                  now - s->received_at)
                  .count();
          if (freshest_age_s < 0.0 || age < freshest_age_s) {
            freshest_age_s = age;
            freshest_source = s->source;
          }
        }
      };
      consider(latest_action_feedback_);
      consider(latest_controller_state_);
      consider(latest_joint_state_);
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "no fresh observed state available to anchor controlled stop",
          {{"reason", "controlled_stop_no_observed_state"},
           {"max_age_s",
            std::to_string(config_.joint_state_max_age_s)},
           {"freshest_sample_source", freshest_source},
           {"freshest_sample_age_s", std::to_string(freshest_age_s)}}};
      return result;
    }
    observed_positions = positions_for_joints(*sample, joint_names);
    if (sample->velocities.size() == sample->joint_names.size()) {
      observed_velocities.reserve(joint_names.size());
      for (const auto& joint : joint_names) {
        const auto it = std::find(
            sample->joint_names.begin(), sample->joint_names.end(), joint);
        if (it == sample->joint_names.end()) {
          observed_velocities.clear();
          break;
        }
        const auto idx = static_cast<std::size_t>(
            it - sample->joint_names.begin());
        if (idx >= sample->velocities.size()) {
          observed_velocities.clear();
          break;
        }
        observed_velocities.push_back(sample->velocities[idx]);
      }
    }
    observed_sample_source = sample->source;
    observed_sample_age_s =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            now - sample->received_at)
            .count();
  }
  if (observed_positions.empty() ||
      observed_positions.size() != joint_names.size()) {
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "fresh sample is missing required joints for controlled-stop anchor",
        {{"reason", "controlled_stop_anchor_size_mismatch"},
         {"sample_source", observed_sample_source},
         {"sample_age_s", std::to_string(observed_sample_age_s)},
         {"required_joints",
          std::to_string(joint_names.size())},
         {"sample_positions",
          std::to_string(observed_positions.size())}}};
    return result;
  }

  const auto execute_soft_on_path_spring_back_fallback =
      [&](std::string reason,
          std::optional<core::ErrorDTO> cause = std::nullopt) {
        core::MotionInterruptStopProfileDTO fallback_profile = profile;
        fallback_profile.stop_mode =
            core::MotionInterruptStopMode::kSpringBackHold;
        fallback_profile.allowed_fallback_modes.clear();
        if (!fallback_profile.deceleration_time_s.has_value()) {
          fallback_profile.deceleration_time_s = 0.05;
        }
        auto fallback = execute_controlled_stop(fallback_profile);
        fallback.executed_stop_mode =
            core::MotionInterruptStopMode::kSpringBackHold;
        fallback.fallback_reason = reason;
        fallback.diagnostics["requested_stop_mode"] = "soft_on_path";
        fallback.diagnostics["fallback_reason"] = reason;
        fallback.diagnostics["fallback_pair"] = "soft_on_path->spring_back_hold";
        if (cause.has_value()) {
          fallback.diagnostics["soft_on_path_error_code"] = cause->code;
          fallback.diagnostics["soft_on_path_error_message"] = cause->message;
          for (const auto& [key, value] : cause->details) {
            fallback.diagnostics["soft_on_path_error_detail_" + key] = value;
          }
          const auto it = cause->details.find("reason");
          if (it != cause->details.end()) {
            fallback.diagnostics["soft_on_path_error_reason"] = it->second;
          }
        }
        return fallback;
      };

  if (profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath &&
      config_.enable_soft_on_path_path_ramp) {
    if (config_.enable_jtc_speed_scaling_input) {
      return execute_soft_on_path_spring_back_fallback(
          "soft_on_path_scale_isolation_unavailable",
          core::ErrorDTO{
              std::string(core::kErrorMotionInterruptBackendStopFailed),
              "ros2_control soft_on_path path ramp requires JTC speed_scaling_input disabled",
              {{"reason", "soft_on_path_scale_isolation_unavailable"}}});
    }

    core::TrajectoryDTO active_trajectory;
    double active_trajectory_time_s = 0.0;
    double active_runtime_scale = 1.0;
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      if (!running_ && terminal_) {
        result.accepted = false;
        result.backend_stop_confirmed = false;
        result.error = core::ErrorDTO{
            std::string(core::kErrorMotionInterruptBackendStopFailed),
            "no discrete-trajectory execution in flight",
            {{"reason", "controlled_stop_no_execution_in_flight"}}};
        return result;
      }
      if (!active_trajectory_.has_value()) {
        return execute_soft_on_path_spring_back_fallback(
            "soft_on_path_missing_active_trajectory",
            core::ErrorDTO{
                std::string(core::kErrorMotionInterruptBackendStopFailed),
                "soft_on_path requires an active accepted trajectory record",
                {{"reason", "soft_on_path_missing_active_trajectory"}}});
      }
      active_trajectory = *active_trajectory_;
      active_trajectory_time_s = active_trajectory_time_s_;
      active_runtime_scale = active_runtime_velocity_scale_;
    }

    core::SoftOnPathInitialStateDTO initial;
    initial.state.joint_names = joint_names;
    initial.state.positions_rad = observed_positions;
    initial.position_source = core::SoftOnPathStateSource::kMeasuredFeedback;
    initial.trajectory_time_from_start_s = active_trajectory_time_s;
    initial.runtime_velocity_scale_at_trip = active_runtime_scale;
    initial.sample_age_ns = static_cast<std::int64_t>(
        std::max(0.0, observed_sample_age_s) * 1e9);
    if (observed_velocities.size() == joint_names.size()) {
      initial.state.velocities_rad_s = observed_velocities;
      initial.velocity_source = core::SoftOnPathStateSource::kMeasuredFeedback;
    } else {
      initial.velocity_source = core::SoftOnPathStateSource::kModelDerived;
    }

    core::SoftOnPathPlannerConfig planner_config;
    planner_config.stop_traj_delta_s = config_.soft_on_path_stop_traj_delta_s;
    planner_config.sample_period_s = config_.soft_on_path_sample_period_s;
    planner_config.max_initial_path_deviation_rad =
        config_.soft_on_path_max_initial_path_deviation_rad;
    planner_config.max_initial_velocity_deviation_rad_s =
        config_.soft_on_path_max_initial_velocity_deviation_rad_s;
    planner_config.per_joint_max_jerk_rad_s3 =
        config_.soft_on_path_max_jerk_rad_s3;
    planner_config.allow_model_velocity_fallback =
        config_.allow_soft_on_path_model_velocity_fallback ||
        is_commanded_sim_mode();

    auto planned = core::plan_soft_on_path_stop(
        active_trajectory, profile, initial, planner_config);
    if (!planned.ok()) {
      if (profile.allow_natural_completion_on_short_path &&
          core::soft_on_path_error_recommends_natural_completion(
              planned.error())) {
        const auto call_start = std::chrono::steady_clock::now();
        const double wait_s = profile.max_stop_time_s.value_or(1.0);
        bool terminal = false;
        {
          std::unique_lock<std::mutex> lock(execution_mutex_);
          terminal = execution_cv_.wait_for(
              lock,
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::duration<double>(wait_s)),
              [this]() { return terminal_; });
        }
        const auto call_end = std::chrono::steady_clock::now();
        result.confirmation_latency_ns = std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                   call_end - call_start)
                   .count());
        result.stop_command_source =
            core::MotionInterruptStopCommandSource::kDiagnosticOther;
        result.diagnostics = planned.error().details;
        result.diagnostics["command_source"] =
            "ros2_control_natural_completion";
        result.diagnostics["requested_stop_mode"] = "soft_on_path";
        result.diagnostics["wait_s"] = std::to_string(wait_s);
        const auto terminal_snapshot = get_execution_snapshot();
        if (!terminal ||
            terminal_snapshot.state != core::ExecutionState::kCompleted) {
          result.accepted = true;
          result.backend_stop_confirmed = false;
          result.error = core::ErrorDTO{
              std::string(core::kErrorMotionInterruptBackendStopFailed),
              "ros2_control soft_on_path natural completion did not reach "
              "the trajectory endpoint within max_stop_time_s",
              {{"reason", "soft_on_path_natural_completion_timeout"},
               {"wait_s", std::to_string(wait_s)},
               {"terminal_state",
                std::string(core::to_string(terminal_snapshot.state))}}};
          return result;
        }
        result.accepted = true;
        result.backend_stop_confirmed = true;
        result.completed_by_natural_completion = true;
        return result;
      }
      return execute_soft_on_path_spring_back_fallback(
          planned.error().details.contains("recommended_fallback")
              ? "soft_on_path_planner_rejected"
              : "soft_on_path_planner_failed",
          planned.error());
    }

    begin_controlled_stop_path_deviation_tracking(
        active_trajectory, planned.value().path_deviation_max_rad);
    const auto call_start = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(execution_mutex_);
      stop_requested_ = true;
      pause_requested_ = false;
      stop_requested_at_ = std::chrono::steady_clock::now();
      // Do not publish a scale jump or explicitly cancel the old goal here.
      // The replacement FJT goal preempts the original at the controller
      // boundary; old-goal terminal callbacks route through stop_requested_.
    }

    const auto dispatch_start = std::chrono::steady_clock::now();
    const bool dispatched = send_fjt_replacement_trajectory(
        planned.value().replacement_trajectory, /*track_terminal_goal=*/true);
    const auto dispatch_accepted = std::chrono::steady_clock::now();
    const auto backend_call_to_goal_dispatch_ns =
        std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                   dispatch_start - call_start)
                   .count());
    const auto goal_dispatch_to_accept_ns =
        std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                   dispatch_accepted - dispatch_start)
                   .count());
    const auto backend_call_to_goal_accept_ns =
        std::max<std::int64_t>(
            0, std::chrono::duration_cast<std::chrono::nanoseconds>(
                   dispatch_accepted - call_start)
                   .count());
    const auto stop_delta_budget_ns = static_cast<std::int64_t>(
        std::max(0.0, config_.soft_on_path_stop_traj_delta_s) * 1e9);
    if (!dispatched) {
      std::optional<core::ErrorDTO> dispatch_error;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        dispatch_error = managed_fjt_terminal_error_;
      }
      auto error = dispatch_error.value_or(core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "failed to dispatch soft_on_path path-ramp replacement trajectory",
          {{"reason", "soft_on_path_path_ramp_dispatch_failed"}}});
      error.details["configured_stop_traj_delta_ns"] =
          std::to_string(stop_delta_budget_ns);
      error.details["backend_call_to_goal_accept_ns"] =
          std::to_string(backend_call_to_goal_accept_ns);
      error.details["backend_call_to_goal_dispatch_ns"] =
          std::to_string(backend_call_to_goal_dispatch_ns);
      error.details["goal_dispatch_to_accept_ns"] =
          std::to_string(goal_dispatch_to_accept_ns);
      clear_controlled_stop_path_deviation_tracking();
      return execute_soft_on_path_spring_back_fallback(
          "soft_on_path_path_ramp_dispatch_failed", error);
    }
    if (stop_delta_budget_ns > 0 &&
        backend_call_to_goal_accept_ns > stop_delta_budget_ns) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      clear_controlled_stop_path_deviation_tracking();
      return execute_soft_on_path_spring_back_fallback(
          "soft_on_path_stop_delta_budget_exceeded",
          core::ErrorDTO{
              std::string(core::kErrorMotionInterruptBackendStopFailed),
              "soft_on_path path-ramp goal was accepted after the configured handoff budget",
              {{"reason", "soft_on_path_stop_delta_budget_exceeded"},
               {"configured_stop_traj_delta_ns",
                std::to_string(stop_delta_budget_ns)},
               {"backend_call_to_goal_accept_ns",
                std::to_string(backend_call_to_goal_accept_ns)},
               {"backend_call_to_goal_dispatch_ns",
                std::to_string(backend_call_to_goal_dispatch_ns)},
               {"goal_dispatch_to_accept_ns",
                std::to_string(goal_dispatch_to_accept_ns)}}});
    }

    const double wait_s = profile.max_stop_time_s.value_or(1.0);
    bool received = false;
    bool success = false;
    std::optional<core::ErrorDTO> terminal_error;
    {
      std::unique_lock<std::mutex> lock(managed_fjt_mutex_);
      received = managed_fjt_cv_.wait_for(
          lock,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>(wait_s)),
          [this]() { return managed_fjt_terminal_result_received_; });
      success = managed_fjt_terminal_result_success_;
      terminal_error = managed_fjt_terminal_error_;
    }
    const auto call_end = std::chrono::steady_clock::now();
    result.confirmation_latency_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(
               call_end - call_start)
               .count());
    if (!received) {
      GoalHandleFJT::SharedPtr path_ramp_goal;
      {
        std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
        path_ramp_goal = managed_fjt_goal_;
      }
      cancel_goal_if_known(path_ramp_goal);
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "controller did not confirm soft_on_path path-ramp goal within the deadline",
          {{"reason", "backend_stop_not_confirmed"},
           {"wait_s", std::to_string(wait_s)},
           {"mode", "soft_on_path"}}};
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    if (!success) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = terminal_error.value_or(core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "soft_on_path path-ramp goal terminated unsuccessfully",
          {{"reason", "controller_terminal_result_unsuccessful"},
           {"mode", "soft_on_path"}}});
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }

    std::vector<double> post_stop_positions;
    std::string post_stop_source;
    {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lock(state_mutex_);
      const auto post_stop_sample = select_latest_sample_locked(now);
      if (post_stop_sample.has_value()) {
        post_stop_positions =
            positions_for_joints(*post_stop_sample, joint_names);
        post_stop_source = post_stop_sample->source;
      }
    }
    if (post_stop_positions.empty() ||
        post_stop_positions.size() != joint_names.size()) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptStopStateInvalid),
          "controller confirmed soft_on_path path ramp but no fresh post-stop sample is available",
          {{"reason", "controlled_stop_post_hold_state_unavailable"},
           {"sample_source", post_stop_source},
           {"mode", "soft_on_path"}}};
      clear_controlled_stop_path_deviation_tracking();
      return result;
    }
    double path_deviation_max_rad = planned.value().path_deviation_max_rad;
    std::size_t path_deviation_feedback_samples = 0;
    std::optional<core::ErrorDTO> path_deviation_feedback_error;
    {
      std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
      if (controlled_stop_path_deviation_max_rad_.has_value()) {
        path_deviation_max_rad =
            std::max(path_deviation_max_rad,
                     *controlled_stop_path_deviation_max_rad_);
      }
      path_deviation_feedback_samples =
          controlled_stop_path_deviation_feedback_samples_;
      path_deviation_feedback_error = controlled_stop_path_deviation_error_;
    }
    core::RobotStateDTO post_stop_state;
    post_stop_state.joint_names = joint_names;
    post_stop_state.positions_rad = post_stop_positions;
    auto post_stop_deviation = core::measure_soft_on_path_path_deviation(
        active_trajectory, post_stop_state);
    if (post_stop_deviation.ok()) {
      path_deviation_max_rad =
          std::max(path_deviation_max_rad, post_stop_deviation.value());
    } else {
      path_deviation_feedback_error = post_stop_deviation.error();
    }
    double stop_distance_sq = 0.0;
    for (std::size_t i = 0; i < joint_names.size(); ++i) {
      const double delta = post_stop_positions[i] - observed_positions[i];
      stop_distance_sq += delta * delta;
    }
    const auto snapshot_before_publish = get_execution_snapshot();
    publish_snapshot(core::ExecutionState::kStopped,
                     snapshot_before_publish.progress,
                     post_stop_positions);

    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.executed_stop_mode = core::MotionInterruptStopMode::kSoftOnPath;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kFjtPreemptPathRamp;
    result.stop_distance_rad = std::sqrt(stop_distance_sq);
    result.initial_path_deviation_rad =
        planned.value().initial_path_deviation_rad;
    result.path_deviation_max_rad = path_deviation_max_rad;
    result.diagnostics = planned.value().diagnostics;
    result.diagnostics["runtime_scale_isolation"] =
        "jtc_speed_scaling_input_disabled";
    result.diagnostics["stop_command_source"] = "fjt_preempt_path_ramp";
    result.diagnostics["configured_stop_traj_delta_ns"] =
        std::to_string(stop_delta_budget_ns);
    result.diagnostics["backend_call_to_goal_dispatch_ns"] =
        std::to_string(backend_call_to_goal_dispatch_ns);
    result.diagnostics["goal_dispatch_to_accept_ns"] =
        std::to_string(goal_dispatch_to_accept_ns);
    result.diagnostics["backend_call_to_goal_accept_ns"] =
        std::to_string(backend_call_to_goal_accept_ns);
    result.diagnostics["stop_traj_delta_budget_margin_ns"] =
        std::to_string(stop_delta_budget_ns - backend_call_to_goal_accept_ns);
    result.diagnostics["post_stop_sample_source"] = post_stop_source;
    result.diagnostics["path_deviation_source"] =
        path_deviation_feedback_samples > 0
            ? "action_feedback_plus_post_stop_sample"
            : "post_stop_sample_plus_planner_projection_no_action_feedback";
    result.diagnostics["path_deviation_feedback_samples"] =
        std::to_string(path_deviation_feedback_samples);
    result.diagnostics["path_deviation_max_rad"] =
        std::to_string(path_deviation_max_rad);
    if (path_deviation_feedback_error.has_value()) {
      result.diagnostics["path_deviation_feedback_error_code"] =
          path_deviation_feedback_error->code;
      result.diagnostics["path_deviation_feedback_error_message"] =
          path_deviation_feedback_error->message;
      const auto it = path_deviation_feedback_error->details.find("reason");
      if (it != path_deviation_feedback_error->details.end()) {
        result.diagnostics["path_deviation_feedback_error_reason"] =
            it->second;
      }
    }
    clear_controlled_stop_path_deviation_tracking();
    return result;
  }

  // The kernel's arm-time + trip-time invariant work has already
  // selected this trip path; mark the discrete-trajectory state
  // machine so the OLD action goal's terminal callback routes through
  // the cancel branch (not fail_execution).
  const auto cancel_start_ns = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (!running_ && terminal_) {
      result.accepted = false;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "no discrete-trajectory execution in flight",
          {{"reason", "controlled_stop_no_execution_in_flight"}}};
      return result;
    }
    stop_requested_ = true;
    pause_requested_ = false;
    stop_requested_at_ = std::chrono::steady_clock::now();
    if (active_goal_handle_) {
      cancel_goal_if_known(active_goal_handle_);
    }
  }

  // spec/260 §5.6 hard_deceleration: native JTC deceleration path.
  // Cancelling the action goal triggers the
  // controller's `decelerate_on_cancel` profile; we wait for the
  // cancel callback to terminate (running_ flips false) and then
  // read the post-stop sample and compute joint-space stop
  // distance from the pre-trip anchor. No new goal is dispatched —
  // the controller's native deceleration IS the stop.
  if (profile.stop_mode == core::MotionInterruptStopMode::kHardDeceleration) {
    const double wait_s = profile.max_stop_time_s.value_or(1.0);
    bool stopped = false;
    std::optional<rclcpp_action::ResultCode> observed_result_code;
    double observed_progress_at_stop = 0.0;
    {
      std::unique_lock<std::mutex> lock(execution_mutex_);
      stopped = execution_cv_.wait_for(
          lock,
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::duration<double>(wait_s)),
          [this]() { return !running_ || terminal_; });
      observed_result_code = last_action_result_code_;
    }
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      observed_progress_at_stop = snapshot_.progress;
    }
    const auto cancel_end_ns = std::chrono::steady_clock::now();
    result.confirmation_latency_ns = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(
               cancel_end_ns - cancel_start_ns)
               .count());
    if (!stopped) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "controller did not finish decelerate_on_cancel within the deadline",
          {{"reason", "backend_stop_not_confirmed"},
           {"wait_s", std::to_string(wait_s)},
           {"mode", "hard_deceleration"}}};
      return result;
    }

    // Proof of cancel.
    // `on_action_result` collapses SUCCEEDED + CANCELED into a single
    // "stopped" branch when `stop_requested_` is set, so the only
    // place that distinction survives is the persisted raw result
    // code. Without this check, a normal trajectory completion
    // racing with cancel would silently be reported as a successful
    // `kJtcCancelDecelerateOnCancel` outcome — the move actually
    // completed and the kernel would believe `decelerate_on_cancel`
    // ran when it did not.
    if (!observed_result_code.has_value()) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "decelerate_on_cancel termination raced ahead of the action result "
          "callback; cancel proof is not yet available",
          {{"reason", "hard_deceleration_result_code_unavailable"},
           {"mode", "hard_deceleration"}}};
      return result;
    }
    if (*observed_result_code != rclcpp_action::ResultCode::CANCELED) {
      const char* code_name = "unknown";
      switch (*observed_result_code) {
        case rclcpp_action::ResultCode::SUCCEEDED: code_name = "succeeded"; break;
        case rclcpp_action::ResultCode::ABORTED:   code_name = "aborted"; break;
        case rclcpp_action::ResultCode::CANCELED:  code_name = "canceled"; break;
        case rclcpp_action::ResultCode::UNKNOWN:   code_name = "unknown"; break;
      }
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptBackendStopFailed),
          "active trajectory terminated with a non-CANCELED result code; "
          "decelerate_on_cancel did not actually run",
          {{"reason", "hard_deceleration_not_canceled"},
           {"observed_result_code", code_name},
           {"mode", "hard_deceleration"}}};
      return result;
    }

    // Read the post-stop sample. The controller's terminal callback
    // fires after `decelerate_on_cancel` completes, so the latest
    // sample should reflect the actuator's rest position. Use the
    // same freshness gate as the pre-trip capture so we never
    // certify a stale post-stop position.
    std::vector<double> post_stop_positions;
    std::string post_stop_source;
    {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lock(state_mutex_);
      const auto post_stop_sample = select_latest_sample_locked(now);
      if (post_stop_sample.has_value()) {
        post_stop_positions =
            positions_for_joints(*post_stop_sample, joint_names);
        post_stop_source = post_stop_sample->source;
      }
    }
    if (post_stop_positions.empty() ||
        post_stop_positions.size() != joint_names.size()) {
      result.accepted = true;
      result.backend_stop_confirmed = false;
      result.error = core::ErrorDTO{
          std::string(core::kErrorMotionInterruptStopStateInvalid),
          "controller finished decelerate_on_cancel but no fresh post-stop "
          "sample is available",
          {{"reason", "controlled_stop_post_hold_state_unavailable"},
           {"sample_source", post_stop_source},
           {"mode", "hard_deceleration"}}};
      return result;
    }

    // Off-path stop distance: joint-space euclidean norm of the
    // delta between the pre-cancel anchor and the post-stop sample.
    // This is the spec/210 §3.11 `stop_distance_rad` evidence the
    // signal_trip outcome requires for `kHardDeceleration` runtime
    // qualification.
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < joint_names.size(); ++i) {
      const double delta = post_stop_positions[i] - observed_positions[i];
      sum_sq += delta * delta;
    }
    result.stop_distance_rad = std::sqrt(sum_sq);

    // Publish the snapshot with the trajectory progress observed at
    // the moment of cancel,
    // NOT progress=1.0. The trajectory did not reach its planned
    // endpoint — `decelerate_on_cancel` stopped it mid-flight, and
    // an off-path interrupt with progress=1.0 would falsely signal
    // trajectory completion to downstream stop-snapshot consumers.
    publish_snapshot(core::ExecutionState::kStopped,
                     observed_progress_at_stop, post_stop_positions);

    result.accepted = true;
    result.backend_stop_confirmed = true;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kJtcCancelDecelerateOnCancel;
    return result;
  }

  // Reset the managed-FJT terminal-result tracking state used by the
  // anchored hold goal before dispatching this controlled stop.
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = false;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_.reset();
  }

  // Dispatch the anchored hold goal: target = observed position with
  // zero velocity at +deceleration_time_s. The existing helper
  // captures the anchor, builds the JTC trajectory message, and
  // tracks the terminal result via on_managed_fjt_terminal_result.
  const double ramp_s = profile.deceleration_time_s.value_or(0.05);
  const auto call_start = std::chrono::steady_clock::now();
  const bool dispatched = send_fjt_anchored_goal(
      joint_names, observed_positions, ramp_s, /*track_terminal_goal=*/true);
  if (!dispatched) {
    std::optional<core::ErrorDTO> dispatch_error;
    {
      std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
      dispatch_error = managed_fjt_terminal_error_;
    }
    result.accepted = false;
    result.backend_stop_confirmed = false;
    result.error = dispatch_error.value_or(core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "failed to dispatch anchored hold goal",
        {{"reason", "controlled_stop_dispatch_failed"}}});
    return result;
  }

  // Bounded wait for the terminal result. The kernel's
  // complete_move_on_signal_timeout_ns deadline is applied on top of
  // this; we still bound here so a misbehaving controller never pins
  // the kernel worker forever. profile.max_stop_time_s wins when set;
  // otherwise use a generous backend default that's still bounded.
  const double wait_s = profile.max_stop_time_s.value_or(1.0);
  bool received = false;
  bool success = false;
  std::optional<core::ErrorDTO> terminal_error;
  {
    std::unique_lock<std::mutex> lock(managed_fjt_mutex_);
    received = managed_fjt_cv_.wait_for(
        lock,
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<double>(wait_s)),
        [this]() { return managed_fjt_terminal_result_received_; });
    success = managed_fjt_terminal_result_success_;
    terminal_error = managed_fjt_terminal_error_;
  }
  const auto call_end = std::chrono::steady_clock::now();
  result.confirmation_latency_ns = std::max<std::int64_t>(
      0, std::chrono::duration_cast<std::chrono::nanoseconds>(
             call_end - call_start)
             .count());

  if (!received) {
    result.accepted = true;  // command accepted; controller didn't confirm
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "controller did not confirm anchored hold goal within the deadline",
        {{"reason", "backend_stop_not_confirmed"},
         {"wait_s", std::to_string(wait_s)}}};
    return result;
  }
  if (!success) {
    result.accepted = true;
    result.backend_stop_confirmed = false;
    result.error = terminal_error.value_or(core::ErrorDTO{
        std::string(core::kErrorMotionInterruptBackendStopFailed),
        "anchored hold goal terminated unsuccessfully",
        {{"reason", "controller_terminal_result_unsuccessful"}}});
    return result;
  }

  // Publish a fresh stopped snapshot from the post-hold sample BEFORE
  // returning success. The kernel calls `get_execution_snapshot()`
  // immediately after this method returns to populate the
  // `ExecutionStopSnapshotDTO`; without an updated publish the
  // snapshot would still reflect the pre-trip state. Failing to
  // refresh the snapshot here is a fail-closed condition because the
  // signal_trip outcome MUST certify where the robot actually came to
  // rest under the hold goal (spec/210 §3.11 stop-snapshot semantic
  // rules).
  std::vector<double> hold_positions;
  std::string hold_source;
  {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(state_mutex_);
    const auto post_hold_sample = select_latest_sample_locked(now);
    if (post_hold_sample.has_value()) {
      hold_positions =
          positions_for_joints(*post_hold_sample, joint_names);
      hold_source = post_hold_sample->source;
    }
  }
  if (hold_positions.empty() || hold_positions.size() != joint_names.size()) {
    result.accepted = true;
    result.backend_stop_confirmed = false;
    result.error = core::ErrorDTO{
        std::string(core::kErrorMotionInterruptStopStateInvalid),
        "controller confirmed hold but no fresh post-hold sample is available",
        {{"reason", "controlled_stop_post_hold_state_unavailable"},
         {"sample_source", hold_source}}};
    return result;
  }
  publish_snapshot(core::ExecutionState::kStopped, /*progress=*/1.0,
                    hold_positions);

  result.accepted = true;
  result.backend_stop_confirmed = true;
  // `executed_stop_mode` MUST describe the physical behavior the
  // controller exhibited, not
  // the requested mode. The scaffold dispatch is functionally
  // identical to spring_back_hold (anchored hold at observed
  // position). Reporting `executed_stop_mode = kSoftOnPath` would let
  // audit consumers believe a path-preserving ramp ran when none did.
  //
  // The honest sequence: physical behavior is spring_back_hold;
  // surface that. The kernel's conjunctive fallback rule then gates
  // the outcome against the scope's `allowed_fallback_modes`: scopes
  // that do not list kSpringBackHold as a fallback fail closed with
  // motion_interrupt_backend_stop_failed. Deployments using the
  // experimental scaffold MUST opt in to the fallback explicitly,
  // which makes the substitution visible in the operational log.
  if (profile.stop_mode == core::MotionInterruptStopMode::kSoftOnPath) {
    result.executed_stop_mode = core::MotionInterruptStopMode::kSpringBackHold;
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kDiagnosticOther;
    result.fallback_reason =
        std::string("soft_on_path_scaffold_anchored_hold_fallback");
    result.diagnostics["command_source_reason"] =
        std::string("soft_on_path_scaffold_anchored_hold_fallback");
    result.diagnostics["scaffold_phase"] = std::string("4.5D-5");
    result.diagnostics["qualification_pending"] = std::string("4.5F");
    result.diagnostics["requested_stop_mode"] = std::string("soft_on_path");
  } else {
    result.stop_command_source =
        core::MotionInterruptStopCommandSource::kFjtPreemptHold;
  }
  return result;
}

core::PauseResultDTO Ros2ControlExecutionBackend::pause() {
  core::ExecutionSnapshot snapshot;
  bool cancel_requested = false;
  bool resync_required = false;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resync_required = resync_required_;
    if (resync_required_) {
      snapshot = get_execution_snapshot();
    } else if (!running_ && terminal_) {
      return core::PauseResultDTO{
          false, make_error(core::ErrorCode::kInvalidRequest, "No execution in flight")};
    } else {
      pause_requested_ = true;
      stop_requested_ = false;
      stop_requested_at_ = std::chrono::steady_clock::now();
      cancel_requested = static_cast<bool>(active_goal_handle_);
      if (cancel_requested) {
        cancel_goal_if_known(active_goal_handle_);
        snapshot = get_execution_snapshot();
      } else {
        running_ = false;
        terminal_ = true;
        snapshot = get_execution_snapshot();
      }
    }
  }
  if (resync_required) {
    return core::PauseResultDTO{
        false,
        operator_intervention_error(
            core::ErrorCode::kPauseFailed,
            "ros2_control execution backend requires observed-state resync before reuse",
            "pause_resync_required",
            snapshot.state)};
  }
  publish_snapshot(cancel_requested ? core::ExecutionState::kStopping
                                    : core::ExecutionState::kPaused,
                   snapshot.progress,
                   snapshot.joint_positions_rad);
  cancel_all_gripper_commands();
  execution_cv_.notify_all();
  return core::PauseResultDTO{true, std::nullopt};
}

core::ExecutionHandle Ros2ControlExecutionBackend::resume(
    const core::TrajectoryDTO& trajectory) {
  return execute_trajectory(trajectory);
}

core::ResetResultDTO Ros2ControlExecutionBackend::reset() {
  GoalHandleFJT::SharedPtr managed_fjt_goal_to_cancel;
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_goal_to_cancel = managed_fjt_goal_;
    managed_fjt_goal_.reset();
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = true;
    managed_fjt_terminal_error_.reset();
    managed_fjt_cv_.notify_all();
  }
  cancel_goal_if_known(managed_fjt_goal_to_cancel);

  bool resync_required = false;
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    resync_required = resync_required_;
    if (!resync_required && active_goal_handle_) {
      cancel_goal_if_known(active_goal_handle_);
    }
    if (!resync_required) {
      running_ = false;
      terminal_ = true;
      stop_requested_ = false;
      pause_requested_ = false;
      stop_requested_at_.reset();
      active_goal_handle_.reset();
      active_goal_id_.reset();
    }
  }
  if (resync_required) {
    auto resync_result = resync_after_intervention_if_needed("reset_resync");
    if (!resync_result.ok()) {
      const auto snapshot = get_execution_snapshot();
      publish_snapshot(core::ExecutionState::kInterventionRequired,
                       snapshot.progress,
                       snapshot.joint_positions_rad,
                       resync_result.error());
      return core::ResetResultDTO{false, resync_result.error()};
    }
    return core::ResetResultDTO{true, std::nullopt};
  }
  cancel_all_gripper_commands();
  execution_cv_.notify_all();
  if (monitor_thread_.joinable() && monitor_active_.load()) {
    std::unique_lock<std::mutex> lock(execution_mutex_);
    const bool monitor_stopped = execution_cv_.wait_for(
        lock,
        std::chrono::duration<double>(config_.stop_completion_timeout_s),
        [this]() { return !monitor_active_.load(); });
    if (!monitor_stopped) {
      lock.unlock();
      const auto snapshot = get_execution_snapshot();
      {
        std::lock_guard<std::mutex> exec_lock(execution_mutex_);
        resync_required_ = true;
        running_ = false;
        terminal_ = true;
        active_goal_handle_.reset();
        active_goal_id_.reset();
        active_trajectory_.reset();
        active_trajectory_time_s_ = 0.0;
        active_runtime_velocity_scale_ = 1.0;
      }
      const auto error = operator_intervention_error(
          core::ErrorCode::kResetFailed,
          "Timed out waiting for ros2_control monitor thread to stop",
          "reset_monitor_join_timeout",
          snapshot.state);
      publish_snapshot(
          core::ExecutionState::kInterventionRequired,
          snapshot.progress,
          snapshot.joint_positions_rad,
          error);
      return core::ResetResultDTO{false, error};
    }
  }
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
  if (is_commanded_sim_mode()) {
    auto reset_result = command_sim_initial_state("reset_commanded_sim");
    if (!reset_result.ok()) {
      const auto snapshot = get_execution_snapshot();
      auto error = reset_result.error();
      publish_snapshot(core::ExecutionState::kInterventionRequired,
                       snapshot.progress,
                       snapshot.joint_positions_rad,
                       error);
      return core::ResetResultDTO{false, error};
    }
    return core::ResetResultDTO{true, std::nullopt};
  }
  publish_snapshot(core::ExecutionState::kIdle, 0.0, {});
  return core::ResetResultDTO{true, std::nullopt};
}

core::ExecutionSnapshot Ros2ControlExecutionBackend::get_execution_snapshot() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return snapshot_;
}

void Ros2ControlExecutionBackend::configure_gripper_action(
    std::string gripper_id,
    Ros2GripperActionConfig config) {
  if (gripper_id.empty()) {
    throw std::invalid_argument("gripper_id must not be empty");
  }
  if (config.action_name.empty()) {
    throw std::invalid_argument("gripper action_name must not be empty");
  }
  if (config.wait_for_action_server_timeout_s < 0.0 ||
      config.result_timeout_s < 0.0) {
    throw std::invalid_argument("gripper action timing config is invalid");
  }
  if (config.group_id.empty() != config.joint_name.empty()) {
    throw std::invalid_argument(
        "gripper group_id and joint_name must be configured together");
  }
  auto runtime = std::make_shared<GripperRuntime>();
  runtime->config = std::move(config);
  runtime->action_client =
      rclcpp_action::create_client<GripperCommand>(node_, runtime->config.action_name);
  std::lock_guard<std::mutex> lock(gripper_map_mutex_);
  grippers_[std::move(gripper_id)] = std::move(runtime);
}

void Ros2ControlExecutionBackend::on_gripper_feedback(
    const std::shared_ptr<GripperRuntime>& runtime,
    GoalHandleGripper::SharedPtr,
    const std::shared_ptr<const GripperCommand::Feedback> feedback) {
  if (!runtime || !feedback) {
    return;
  }
  Ros2GripperActionConfig runtime_config;
  std::optional<double> feedback_position;
  bool reached_goal = false;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (!runtime->executing) {
      return;
    }
    runtime_config = runtime->config;
    runtime->current_position = feedback->position;
    feedback_position = feedback->position;
    reached_goal = feedback->reached_goal;
    if (reached_goal) {
      runtime->state = "completed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->active_goal_handle.reset();
      runtime->error.clear();
    }
  }
  if (feedback_position.has_value() && (!is_commanded_sim_mode() || reached_goal)) {
    project_gripper_position_to_scene(runtime_config, *feedback_position,
                                      SceneUpdateMode::kBestEffort);
  }
}

void Ros2ControlExecutionBackend::on_gripper_result(
    const std::shared_ptr<GripperRuntime>& runtime,
    const GoalHandleGripper::WrappedResult& result) {
  if (!runtime) {
    return;
  }
  Ros2GripperActionConfig runtime_config;
  std::optional<double> result_position;
  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    runtime_config = runtime->config;
    runtime->active_goal_handle.reset();
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->executing = false;
    if (result.result) {
      runtime->current_position = result.result->position;
      result_position = result.result->position;
    }
    switch (result.code) {
      case rclcpp_action::ResultCode::SUCCEEDED:
        if (result.result && result.result->reached_goal) {
          runtime->state = "completed";
          runtime->error.clear();
          if (!result_position.has_value()) {
            result_position = runtime->target_position;
          }
        } else {
          runtime->state = "failed";
          runtime->error = "Gripper action succeeded without reaching the goal";
        }
        break;
      case rclcpp_action::ResultCode::ABORTED:
        runtime->state = "failed";
        runtime->error = "Gripper action aborted";
        break;
      case rclcpp_action::ResultCode::CANCELED:
        runtime->state = "idle";
        runtime->error.clear();
        break;
      default:
        runtime->state = "failed";
        runtime->error = "Gripper action returned an unknown result code";
        break;
    }
  }
  if (result_position.has_value()) {
    project_gripper_position_to_scene(runtime_config, *result_position,
                                      SceneUpdateMode::kBestEffort);
  }
}

void Ros2ControlExecutionBackend::cancel_all_gripper_commands() {
  std::vector<std::shared_ptr<GripperRuntime>> runtimes;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    for (const auto& [_, runtime] : grippers_) {
      runtimes.push_back(runtime);
    }
  }
  for (auto& runtime : runtimes) {
    GoalHandleGripper::SharedPtr goal_handle;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (!runtime->executing && !runtime->active_goal_handle) {
        continue;
      }
      goal_handle = runtime->active_goal_handle;
      action_client = runtime->action_client;
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = true;
      runtime->active_goal_handle.reset();
      runtime->state = "idle";
      runtime->error.clear();
    }
    if (action_client && goal_handle) {
      try {
        (void)action_client->async_cancel_goal(goal_handle);
      } catch (const std::exception& exc) {
        RCLCPP_WARN(node_->get_logger(),
                    "Failed to cancel gripper action goal: %s", exc.what());
      }
    }
  }
}

Ros2GripperCommandResult Ros2ControlExecutionBackend::command_gripper(
    const std::string& gripper_id,
    double target_position,
    double duration_s,
    double max_effort) {
  if (!std::isfinite(target_position) || !std::isfinite(duration_s) ||
      !std::isfinite(max_effort) || duration_s < 0.0) {
    return {false, "Invalid gripper command target, duration, or max effort"};
  }
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(gripper_id);
    if (it == grippers_.end()) {
      return {false, "No ROS GripperCommand action configured for " + gripper_id};
    }
    runtime = it->second;
  }
  const Ros2GripperActionConfig runtime_config = runtime->config;
  std::optional<double> start_position;
  if (!runtime_config.group_id.empty() || !runtime_config.joint_name.empty()) {
    start_position = scene_group_joint_position(runtime_config.group_id,
                                                runtime_config.joint_name);
    if (!start_position.has_value()) {
      return {false,
              "Cannot project gripper '" + gripper_id +
                  "' into RoboPlan scene group '" + runtime_config.group_id +
                  "' joint '" + runtime_config.joint_name + "'"};
    }
  }

  const auto wait_timeout = std::chrono::duration<double>(
      runtime->config.wait_for_action_server_timeout_s);
  if (!runtime->action_client->wait_for_action_server(wait_timeout)) {
    return {false,
            "Timed out waiting for gripper action server " +
                runtime->config.action_name};
  }

  {
    std::lock_guard<std::mutex> lock(runtime->mutex);
    if (runtime->executing) {
      return {false, "Gripper " + gripper_id + " is already executing"};
    }
    runtime->state = "executing";
    runtime->executing = true;
    runtime->goal_accepted = false;
    runtime->cancel_pending = false;
    runtime->start_position = start_position;
    runtime->current_position = start_position;
    runtime->target_position = target_position;
    runtime->duration_s = duration_s;
    runtime->max_effort = max_effort;
    runtime->started_at = std::chrono::steady_clock::now();
    runtime->deadline_at.reset();
    if (runtime->config.result_timeout_s > 0.0) {
      runtime->deadline_at =
          runtime->started_at +
          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
              std::chrono::duration<double>(
                  duration_s + runtime->config.result_timeout_s));
    }
    runtime->active_goal_handle.reset();
    runtime->error.clear();
  }

  GripperCommand::Goal goal;
  goal.command.position = target_position;
  goal.command.max_effort = max_effort;

  rclcpp_action::Client<GripperCommand>::SendGoalOptions options;
  options.goal_response_callback =
      [runtime](const GoalHandleGripper::SharedPtr& goal_handle) {
        GoalHandleGripper::SharedPtr handle_to_cancel;
        rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
        {
          std::lock_guard<std::mutex> lock(runtime->mutex);
          if (!goal_handle) {
            if (runtime->executing) {
              runtime->state = "failed";
              runtime->executing = false;
              runtime->goal_accepted = false;
              runtime->cancel_pending = false;
              runtime->error = "Gripper action goal was rejected";
            }
            return;
          }
          if (runtime->cancel_pending || !runtime->executing) {
            handle_to_cancel = goal_handle;
            action_client = runtime->action_client;
            runtime->active_goal_handle.reset();
            runtime->goal_accepted = false;
            runtime->executing = false;
            runtime->cancel_pending = false;
            runtime->state = "idle";
            runtime->error.clear();
          } else {
            runtime->goal_accepted = true;
            runtime->active_goal_handle = goal_handle;
          }
        }
        if (action_client && handle_to_cancel) {
          try {
            (void)action_client->async_cancel_goal(handle_to_cancel);
          } catch (const std::exception&) {
            // Reset/stop remains best-effort if the remote goal disappears during
            // negotiation; the next status/result callback will reconcile state.
          }
        }
      };
  options.feedback_callback =
      [this, runtime](
          GoalHandleGripper::SharedPtr goal_handle,
          const std::shared_ptr<const GripperCommand::Feedback> feedback) {
        on_gripper_feedback(runtime, goal_handle, feedback);
      };
  options.result_callback =
      [this, runtime](const GoalHandleGripper::WrappedResult& result) {
        on_gripper_result(runtime, result);
      };

  try {
    (void)runtime->action_client->async_send_goal(goal, options);
  } catch (const std::exception& exc) {
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      runtime->state = "failed";
      runtime->executing = false;
      runtime->goal_accepted = false;
      runtime->cancel_pending = false;
      runtime->error = std::string("Failed to dispatch gripper action goal: ") +
                       exc.what();
    }
    return {false, runtime->error};
  }
  if (duration_s <= 0.0 && start_position.has_value()) {
    project_gripper_position_to_scene(runtime_config, target_position);
  }
  return {true, ""};
}

void Ros2ControlExecutionBackend::update_gripper_commands() {
  std::vector<std::shared_ptr<GripperRuntime>> runtimes;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    for (const auto& [_, runtime] : grippers_) {
      runtimes.push_back(runtime);
    }
  }
  const auto now = std::chrono::steady_clock::now();
  for (auto& runtime : runtimes) {
    GoalHandleGripper::SharedPtr goal_handle;
    rclcpp_action::Client<GripperCommand>::SharedPtr action_client;
    Ros2GripperActionConfig runtime_config;
    std::optional<double> scene_position;
    {
      std::lock_guard<std::mutex> lock(runtime->mutex);
      if (!runtime->executing) {
        continue;
      }
      runtime_config = runtime->config;
      if (runtime->deadline_at.has_value() && now >= *runtime->deadline_at) {
        goal_handle = runtime->active_goal_handle;
        action_client = runtime->action_client;
        runtime->active_goal_handle.reset();
        runtime->state = "failed";
        runtime->executing = false;
        runtime->goal_accepted = false;
        runtime->cancel_pending = false;
        runtime->error = "Timed out waiting for gripper action result";
      } else if (!runtime_config.group_id.empty() &&
                 !runtime_config.joint_name.empty()) {
        if (is_commanded_sim_mode() && runtime->start_position.has_value()) {
          const double duration = std::max(1e-9, runtime->duration_s);
          const double progress =
              clamp_progress(seconds_since(runtime->started_at, now) / duration);
          scene_position =
              *runtime->start_position +
              (runtime->target_position - *runtime->start_position) * progress;
          runtime->current_position = scene_position;
        } else if (runtime->current_position.has_value()) {
          scene_position = *runtime->current_position;
        } else if (runtime->start_position.has_value()) {
          scene_position = *runtime->start_position;
        }
      }
    }
    if (scene_position.has_value()) {
      project_gripper_position_to_scene(runtime_config, *scene_position,
                                        SceneUpdateMode::kBestEffort);
    }
    if (action_client && goal_handle) {
      try {
        (void)action_client->async_cancel_goal(goal_handle);
      } catch (const std::exception& exc) {
        RCLCPP_WARN(node_->get_logger(),
                    "Failed to cancel timed-out gripper action goal: %s",
                    exc.what());
      }
    }
  }
}

Ros2GripperStatus Ros2ControlExecutionBackend::get_gripper_status(
    const std::string& gripper_id) const {
  std::shared_ptr<GripperRuntime> runtime;
  {
    std::lock_guard<std::mutex> lock(gripper_map_mutex_);
    const auto it = grippers_.find(gripper_id);
    if (it == grippers_.end()) {
      return {};
    }
    runtime = it->second;
  }
  std::lock_guard<std::mutex> lock(runtime->mutex);
  Ros2GripperStatus status;
  status.state = runtime->state;
  status.current_position = runtime->current_position;
  status.target_position = runtime->target_position;
  if (runtime->executing) {
    const double duration = std::max(1e-9, runtime->duration_s);
    status.progress =
        clamp_progress(seconds_since(runtime->started_at,
                                     std::chrono::steady_clock::now()) /
                       duration);
  } else if (runtime->state == "completed") {
    status.progress = 1.0;
  } else {
    status.progress = 0.0;
  }
  status.error = runtime->error;
  return status;
}

std::shared_ptr<NativeKernelBundle> create_ros2_control_kernel(
    const std::string& model_name,
    const std::string& group_name,
    const std::string& urdf_path,
    const std::string& srdf_path,
    const std::vector<std::string>& package_paths,
    const Ros2ControlExecutionBackendConfig& ros_config,
    const core::ExecutionKernelConfig& kernel_config) {
  auto bundle = std::make_shared<NativeKernelBundle>();
  bundle->group_name = group_name;

  std::vector<std::filesystem::path> pkg_paths;
  for (const auto& path : package_paths) {
    pkg_paths.emplace_back(path);
  }

  bundle->scene = std::make_shared<roboplan::Scene>(
      model_name, std::filesystem::path(urdf_path),
      std::filesystem::path(srdf_path), pkg_paths);

  roboplan::RRTOptions rrt_opts;
  rrt_opts.group_name = group_name;
  rrt_opts.max_planning_time = 5.0;
  rrt_opts.max_nodes = 5000;
  bundle->rrt = std::make_shared<roboplan::RRT>(bundle->scene, rrt_opts);
  bundle->shortcutter = std::make_shared<roboplan::PathShortcutter>(
      bundle->scene, group_name);
  bundle->toppra = std::make_shared<roboplan::PathParameterizerTOPPRA>(
      bundle->scene, group_name);

  roboplan::SimpleIkOptions ik_opts;
  ik_opts.group_name = group_name;
  ik_opts.step_size = 0.25;
  ik_opts.max_restarts = 50;
  ik_opts.max_time = 1.0;
  bundle->ik = std::make_shared<roboplan::SimpleIk>(bundle->scene, ik_opts);
  auto single_shot_ik_opts = ik_opts;
  single_shot_ik_opts.max_time = kernel_config.single_shot_ik_timeout_s;
  bundle->single_shot_ik =
      std::make_shared<roboplan::SimpleIk>(bundle->scene, single_shot_ik_opts);

  auto scene_mutex = std::make_shared<std::mutex>();
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene, rrt_opts, bundle->shortcutter, bundle->toppra, bundle->ik,
      scene_mutex, bundle->single_shot_ik);
  bundle->context = std::make_unique<RoboPlanPlanningContext>(
      bundle->scene, group_name, scene_mutex);
  bundle->clock = std::make_unique<WallClock>();
  bundle->execution = std::make_unique<Ros2ControlExecutionBackend>(
      bundle->scene, group_name, *bundle->clock, ros_config, scene_mutex);
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();

  const auto policy = default_collision_policy_snapshot();
  bundle->context->set_collision_policy_snapshot(policy);
  const int collision_worker_count =
      core::env_validation_collision_workers(
          std::max(1, kernel_config.validation_collision_workers));
  bundle->collision_validation_engines.reserve(collision_worker_count);
  for (int i = 0; i < collision_worker_count; ++i) {
    bundle->collision_validation_engines.push_back(
        std::make_shared<manyforge::collision::ManyForgeCollisionEngine>(
            model_name,
            std::filesystem::path(urdf_path),
            std::filesystem::path(srdf_path),
            pkg_paths,
            group_name));
  }

  core::ExecutionKernelConfig resolved_kernel_config = kernel_config;
  resolved_kernel_config.validation_collision_workers = collision_worker_count;
  bundle->kernel = std::make_unique<core::ExecutionKernel>(
      *bundle->planner, *bundle->context, *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache,
      resolved_kernel_config);
  bundle->kernel->set_collision_validation_policy_snapshot(policy);
  bundle->kernel->set_collision_validation_pipeline(
      *bundle->context,
      bundle->collision_validation_engines);
  bundle->event_collector.subscribe(*bundle->kernel);
  return bundle;
}

// =================================================================
// FollowJointTrajectory preempt/controlled-stop dispatch helpers.
//
// Anchored two-point FJT goals preserve JTC spline continuity across
// preempts. Used by the motion-interrupt controlled-stop paths
// (spring_back_hold / soft_on_path) — see execute_controlled_stop.
// =================================================================

bool Ros2ControlExecutionBackend::send_fjt_anchored_goal(
    const std::vector<std::string>& joint_names,
    const std::vector<double>& target_positions,
    double target_time_from_start_s,
    bool track_terminal_goal) {
  // FJT preempt-goal anchor timing. The anchor point sits a small negative
  // time before "now" so JTC's spline has a continuous start; the target
  // floor bounds the replacement goal's minimum duration. Fixed constants so
  // controlled-stop does not depend on any deleted ADR-0016 session config.
  constexpr double kFjtAnchorNegativeTimeS = -0.010;
  constexpr double kFjtTargetTimeFromStartMinS = 0.030;
  if (joint_names.empty() || target_positions.empty()) {
    if (track_terminal_goal) {
      std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
      managed_fjt_terminal_result_received_ = true;
      managed_fjt_terminal_result_success_ = false;
      managed_fjt_terminal_error_ = make_error(
          core::ErrorCode::kStopFailed,
          "managed FJT terminal soft-stop target is empty",
          {{"reason_code", "soft_stop_dispatch_failed"},
           {"failure_stage", "empty_terminal_target"}});
      managed_fjt_cv_.notify_all();
    }
    return false;
  }
  if (target_positions.size() != joint_names.size()) {
    if (track_terminal_goal) {
      std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
      managed_fjt_terminal_result_received_ = true;
      managed_fjt_terminal_result_success_ = false;
      managed_fjt_terminal_error_ = make_error(
          core::ErrorCode::kStopFailed,
          "managed FJT terminal soft-stop target size does not match joint names",
          {{"reason_code", "soft_stop_dispatch_failed"},
           {"failure_stage", "terminal_target_size_mismatch"},
           {"joint_count", std::to_string(joint_names.size())},
           {"position_count", std::to_string(target_positions.size())}});
      managed_fjt_cv_.notify_all();
    }
    return false;
  }

  // Anchor at the latest controller-source sample if available; otherwise
  // anchor at the requested target (degenerate but safe for the first goal
  // of a session).
  std::vector<double> anchor_positions;
  std::vector<double> anchor_velocities;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (latest_action_feedback_.has_value()) {
      anchor_positions = positions_for_joints(
          *latest_action_feedback_, joint_names);
      anchor_velocities.resize(joint_names.size(), 0.0);
      const auto& sample = *latest_action_feedback_;
      for (size_t i = 0; i < joint_names.size(); ++i) {
        const auto it = std::find(
            sample.joint_names.begin(), sample.joint_names.end(),
            joint_names[i]);
        if (it != sample.joint_names.end() &&
            static_cast<size_t>(it - sample.joint_names.begin()) <
                sample.velocities.size()) {
          anchor_velocities[i] =
              sample.velocities[it - sample.joint_names.begin()];
        }
      }
    } else if (latest_controller_state_.has_value()) {
      anchor_positions = positions_for_joints(
          *latest_controller_state_, joint_names);
      anchor_velocities.assign(joint_names.size(), 0.0);
    } else if (latest_joint_state_.has_value()) {
      anchor_positions = positions_for_joints(
          *latest_joint_state_, joint_names);
      anchor_velocities.assign(joint_names.size(), 0.0);
    }
  }
  if (anchor_positions.empty() || anchor_positions.size() != joint_names.size()) {
    // No fresh state to anchor on — fall back to the target as anchor.
    // This degrades smoothness on the first preempt but recovers on the
    // next. The kernel's freshness gates should normally ensure observed
    // state is present before a controlled stop dispatches.
    anchor_positions = target_positions;
    anchor_velocities.assign(joint_names.size(), 0.0);
  }

  if (!wait_for_fjt_action_server(action_client_,
                                  config_.wait_for_action_server_timeout_s)) {
    if (track_terminal_goal) {
      std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
      managed_fjt_terminal_result_received_ = true;
      managed_fjt_terminal_result_success_ = false;
      managed_fjt_terminal_error_ = make_error(
          core::ErrorCode::kStopFailed,
          "FollowJointTrajectory action server unavailable for managed FJT "
          "terminal soft-stop",
          {{"reason_code", "soft_stop_dispatch_failed"},
           {"failure_stage", "action_server_unavailable"},
           {"action_name", config_.action_name}});
      managed_fjt_cv_.notify_all();
    }
    return false;
  }

  if (track_terminal_goal) {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = false;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_.reset();
  }
  const auto mark_terminal_dispatch_failed = [&](std::string message) {
    if (!track_terminal_goal) {
      return;
    }
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed, std::move(message));
    managed_fjt_cv_.notify_all();
  };

  FollowJointTrajectory::Goal goal;
  goal.trajectory.joint_names = joint_names;
  if (config_.disable_controller_path_tolerances) {
    disable_fjt_controller_tolerances(goal, joint_names);
  }

  // Anchor point: observed state with negative time_from_start. JTC's
  // spline uses this to compute continuous starting conditions for the
  // new trajectory, so the preempt does not introduce a velocity
  // discontinuity at the goal-replacement seam.
  trajectory_msgs::msg::JointTrajectoryPoint anchor_point;
  anchor_point.positions = anchor_positions;
  anchor_point.velocities = anchor_velocities;
  anchor_point.accelerations.assign(joint_names.size(), 0.0);
  anchor_point.time_from_start = duration_from_seconds(
      kFjtAnchorNegativeTimeS);
  goal.trajectory.points.push_back(std::move(anchor_point));

  // Target point: requested final position, zero velocity (the next
  // streamed target will reset velocity at the next preempt anyway).
  trajectory_msgs::msg::JointTrajectoryPoint target_point;
  target_point.positions = target_positions;
  target_point.velocities.assign(joint_names.size(), 0.0);
  target_point.accelerations.assign(joint_names.size(), 0.0);
  target_point.time_from_start = duration_from_seconds(
      std::max(target_time_from_start_s,
               kFjtTargetTimeFromStartMinS));
  goal.trajectory.points.push_back(std::move(target_point));

  rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions options;
  options.feedback_callback =
      [this](GoalHandleFJT::SharedPtr goal_handle,
             const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback) {
        on_action_feedback(goal_handle, feedback);
      };
  options.result_callback =
      [this, track_terminal_goal](const GoalHandleFJT::WrappedResult& result) {
        // Preempting goals are dispatched async (async_send_goal) and do
        // not block on per-goal results. Route the result through the
        // existing callback so feedback freshness gates and tracking
        // peak telemetry continue to work uniformly.
        if (track_terminal_goal) {
          on_managed_fjt_terminal_result(result);
        } else {
          on_action_result(result);
        }
      };

  auto goal_handle_future = action_client_->async_send_goal(goal, options);
  if (goal_handle_future.wait_for(
          std::chrono::duration<double>(
              config_.wait_for_action_server_timeout_s)) !=
      std::future_status::ready) {
    mark_terminal_dispatch_failed(
        "timed out waiting for managed FJT terminal soft-stop goal acceptance");
    return false;
  }
  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception&) {
    mark_terminal_dispatch_failed(
        "managed FJT terminal soft-stop goal acceptance threw an exception");
    return false;
  }
  if (!goal_handle) {
    mark_terminal_dispatch_failed(
        "managed FJT terminal soft-stop goal was rejected by the controller");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_goal_ = goal_handle;
    if (track_terminal_goal) {
      managed_fjt_terminal_goal_id_ = goal_handle->get_goal_id();
    }
  }
  return true;
}

bool Ros2ControlExecutionBackend::send_fjt_replacement_trajectory(
    const core::TrajectoryDTO& trajectory,
    bool track_terminal_goal) {
  const auto fail_terminal = [&](std::string message,
                                 std::string failure_stage) {
    if (!track_terminal_goal) {
      return;
    }
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed,
        std::move(message),
        {{"reason_code", "soft_on_path_dispatch_failed"},
         {"failure_stage", std::move(failure_stage)}});
    managed_fjt_cv_.notify_all();
  };

  if (trajectory.joint_names.empty() || trajectory.waypoints.empty()) {
    fail_terminal("soft_on_path replacement trajectory is empty",
                  "empty_replacement_trajectory");
    return false;
  }
  double previous_time = -std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < trajectory.waypoints.size(); ++i) {
    const auto& waypoint = trajectory.waypoints[i];
    if (waypoint.positions_rad.size() != trajectory.joint_names.size()) {
      fail_terminal(
          "soft_on_path replacement waypoint position size does not match joints",
          "replacement_position_size_mismatch");
      return false;
    }
    if (!waypoint.velocities_rad_s.empty() &&
        waypoint.velocities_rad_s.size() != trajectory.joint_names.size()) {
      fail_terminal(
          "soft_on_path replacement waypoint velocity size does not match joints",
          "replacement_velocity_size_mismatch");
      return false;
    }
    if (!waypoint.accelerations_rad_s2.empty() &&
        waypoint.accelerations_rad_s2.size() != trajectory.joint_names.size()) {
      fail_terminal(
          "soft_on_path replacement waypoint acceleration size does not match joints",
          "replacement_acceleration_size_mismatch");
      return false;
    }
    if (!std::isfinite(waypoint.time_from_start_s) ||
        waypoint.time_from_start_s < 0.0 ||
        waypoint.time_from_start_s < previous_time) {
      fail_terminal(
          "soft_on_path replacement waypoint times must be finite and monotonic",
          "replacement_time_invalid");
      return false;
    }
    previous_time = waypoint.time_from_start_s;
  }

  if (!wait_for_fjt_action_server(action_client_,
                                  config_.wait_for_action_server_timeout_s)) {
    fail_terminal(
        "FollowJointTrajectory action server unavailable for soft_on_path path ramp",
        "action_server_unavailable");
    return false;
  }

  if (track_terminal_goal) {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_goal_id_.reset();
    managed_fjt_terminal_result_received_ = false;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_.reset();
  }
  const auto mark_terminal_dispatch_failed = [&](std::string message) {
    if (!track_terminal_goal) {
      return;
    }
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_terminal_result_received_ = true;
    managed_fjt_terminal_result_success_ = false;
    managed_fjt_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed, std::move(message));
    managed_fjt_cv_.notify_all();
  };

  FollowJointTrajectory::Goal goal;
  goal.trajectory.joint_names = trajectory.joint_names;
  if (config_.disable_controller_path_tolerances) {
    disable_fjt_controller_tolerances(goal, trajectory.joint_names);
  }
  for (const auto& waypoint : trajectory.waypoints) {
    trajectory_msgs::msg::JointTrajectoryPoint point;
    point.positions = waypoint.positions_rad;
    point.velocities = waypoint.velocities_rad_s;
    point.accelerations = waypoint.accelerations_rad_s2;
    point.time_from_start = duration_from_seconds(waypoint.time_from_start_s);
    goal.trajectory.points.push_back(std::move(point));
  }

  rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions options;
  options.feedback_callback =
      [this](GoalHandleFJT::SharedPtr goal_handle,
             const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback) {
        on_action_feedback(goal_handle, feedback);
      };
  options.result_callback =
      [this, track_terminal_goal](const GoalHandleFJT::WrappedResult& result) {
        if (track_terminal_goal) {
          on_managed_fjt_terminal_result(result);
        } else {
          on_action_result(result);
        }
      };

  auto goal_handle_future = action_client_->async_send_goal(goal, options);
  if (goal_handle_future.wait_for(
          std::chrono::duration<double>(
              config_.wait_for_action_server_timeout_s)) !=
      std::future_status::ready) {
    mark_terminal_dispatch_failed(
        "timed out waiting for soft_on_path path-ramp goal acceptance");
    return false;
  }
  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception&) {
    mark_terminal_dispatch_failed(
        "soft_on_path path-ramp goal acceptance threw an exception");
    return false;
  }
  if (!goal_handle) {
    mark_terminal_dispatch_failed(
        "soft_on_path path-ramp goal was rejected by the controller");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
    managed_fjt_goal_ = goal_handle;
    if (track_terminal_goal) {
      managed_fjt_terminal_goal_id_ = goal_handle->get_goal_id();
    }
  }
  return true;
}

void Ros2ControlExecutionBackend::begin_controlled_stop_path_deviation_tracking(
    const core::TrajectoryDTO& accepted_trajectory,
    double initial_path_deviation_rad) {
  std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
  controlled_stop_path_deviation_reference_ = accepted_trajectory;
  controlled_stop_path_deviation_max_rad_ =
      std::isfinite(initial_path_deviation_rad)
          ? std::max(0.0, initial_path_deviation_rad)
          : 0.0;
  controlled_stop_path_deviation_feedback_samples_ = 0;
  controlled_stop_path_deviation_error_.reset();
}

void Ros2ControlExecutionBackend::record_controlled_stop_path_deviation_feedback(
    const std::vector<std::string>& joint_names,
    const std::vector<double>& positions) {
  std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
  if (!controlled_stop_path_deviation_reference_.has_value()) {
    return;
  }
  if (joint_names.empty() || positions.empty()) {
    controlled_stop_path_deviation_error_ = make_error(
        core::ErrorCode::kMotionInterruptStopStateInvalid,
        "soft_on_path path-ramp feedback omitted joint names or positions",
        {{"reason", "path_deviation_feedback_empty"}});
    return;
  }
  core::RobotStateDTO observed;
  observed.joint_names = joint_names;
  observed.positions_rad = positions;
  auto measured = core::measure_soft_on_path_path_deviation(
      *controlled_stop_path_deviation_reference_, observed);
  if (!measured.ok()) {
    controlled_stop_path_deviation_error_ = measured.error();
    return;
  }
  controlled_stop_path_deviation_max_rad_ = std::max(
      controlled_stop_path_deviation_max_rad_.value_or(0.0), measured.value());
  ++controlled_stop_path_deviation_feedback_samples_;
}

void Ros2ControlExecutionBackend::clear_controlled_stop_path_deviation_tracking() {
  std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
  controlled_stop_path_deviation_reference_.reset();
  controlled_stop_path_deviation_max_rad_.reset();
  controlled_stop_path_deviation_feedback_samples_ = 0;
  controlled_stop_path_deviation_error_.reset();
}

void Ros2ControlExecutionBackend::on_managed_fjt_terminal_result(
    const GoalHandleFJT::WrappedResult& result) {
  const bool controller_success =
      result.result &&
      result.result->error_code == FollowJointTrajectory::Result::SUCCESSFUL;
  const bool terminal_success =
      result.code == rclcpp_action::ResultCode::SUCCEEDED && controller_success;

  std::lock_guard<std::mutex> lock(managed_fjt_mutex_);
  if (managed_fjt_terminal_goal_id_.has_value() &&
      result.goal_id != *managed_fjt_terminal_goal_id_) {
    return;
  }
  managed_fjt_terminal_result_received_ = true;
  managed_fjt_terminal_result_success_ = terminal_success;
  managed_fjt_goal_.reset();
  managed_fjt_terminal_goal_id_.reset();
  if (!terminal_success) {
    const std::string controller_error =
        result.result ? result.result->error_string : "controller returned no result";
    managed_fjt_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed,
        controller_error.empty()
            ? "managed FJT terminal soft-stop goal failed"
            : "managed FJT terminal soft-stop goal failed: " + controller_error,
        {{"result_code", std::to_string(static_cast<int>(result.code))}});
  } else {
    managed_fjt_terminal_error_.reset();
  }
  managed_fjt_cv_.notify_all();
}

void Ros2ControlExecutionBackend::set_runtime_velocity_scale(double scale) {
  // ADR-0018: clamp + atomic store. Caller (kernel) already validated
  // finiteness, but defense-in-depth keeps monitor/dispatcher workers from
  // ever reading NaN/Inf via this path.
  if (!std::isfinite(scale)) return;
  if (scale < 0.0) scale = 0.0;
  if (scale > 1.0) scale = 1.0;
  runtime_velocity_scale_.store(scale, std::memory_order_release);
  publish_runtime_velocity_scale(scale);
  execution_cv_.notify_all();
  managed_fjt_cv_.notify_all();
}

}  // namespace manyforge::roboplan_backend
