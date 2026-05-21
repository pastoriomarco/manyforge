#include <manyforge_roboplan/ros2_control_execution_backend.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_roboplan/native_kernel_factory.hpp>

namespace manyforge::roboplan_backend {
namespace {

constexpr double kRuntimeScaleSlewPerSecond = 4.0;  // 0→1 in 250 ms.

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
  GoalHandleFJT::SharedPtr streaming_handle_to_cancel;
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
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_handle_to_cancel = streaming_active_goal_;
    streaming_active_goal_.reset();
    streaming_terminal_goal_id_.reset();
    streaming_terminal_result_received_ = true;
    streaming_terminal_result_success_ = true;
    streaming_terminal_error_.reset();
  }
  if (streaming_thread_.joinable()) {
    streaming_thread_.request_stop();
    streaming_cv_.notify_all();
  }
  cancel_goal_if_known(handle_to_cancel);
  cancel_goal_if_known(streaming_handle_to_cancel);
  cancel_all_gripper_commands();
  set_command_gate(false);
  execution_cv_.notify_all();
  if (streaming_thread_.joinable()) {
    streaming_thread_.join();
  }
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
      config_.monitor_period_s <= 0.0) {
    throw std::invalid_argument("ros2_control timing/tolerance config is invalid");
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
  {
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
  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (terminal_ || resync_required_) {
      return;
    }
    if (!goal_handle || !active_goal_id_.has_value() ||
        goal_handle->get_goal_id() != *active_goal_id_) {
      return;
    }
    if (running_ && !first_feedback_seen_) {
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
    if (!error_sample.positions.empty()) {
      update_tracking_error_peak_locked(
          error_sample, elapsed_s,
          observed_positions.empty() ? nullptr : &observed_positions,
          expected_positions.empty() ? nullptr : &expected_positions,
          observed_velocities.empty() ? nullptr : &observed_velocities);
      latest_action_feedback_error_ = std::move(error_sample);
    }
    latest_action_feedback_ = std::move(sample);
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

std::vector<double> Ros2ControlExecutionBackend::interpolate_joint_positions(
    const core::TrajectoryDTO& trajectory, double time_s) const {
  if (trajectory.waypoints.empty()) {
    return {};
  }
  if (trajectory.waypoints.size() == 1 ||
      time_s <= trajectory.waypoints.front().time_from_start_s) {
    return trajectory.waypoints.front().positions_rad;
  }
  if (time_s >= trajectory.waypoints.back().time_from_start_s) {
    return trajectory.waypoints.back().positions_rad;
  }
  for (size_t i = 1; i < trajectory.waypoints.size(); ++i) {
    const auto& prev = trajectory.waypoints[i - 1];
    const auto& next = trajectory.waypoints[i];
    if (time_s > next.time_from_start_s) {
      continue;
    }
    const double dt = next.time_from_start_s - prev.time_from_start_s;
    if (dt <= 1e-9) {
      return next.positions_rad;
    }
    const double alpha = clamp_progress((time_s - prev.time_from_start_s) / dt);
    std::vector<double> interpolated(prev.positions_rad.size(), 0.0);
    for (size_t joint = 0; joint < prev.positions_rad.size(); ++joint) {
      interpolated[joint] =
          prev.positions_rad[joint] +
          alpha * (next.positions_rad[joint] - prev.positions_rad[joint]);
    }
    return interpolated;
  }
  return trajectory.waypoints.back().positions_rad;
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
    std::optional<core::ErrorDTO> error) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  snapshot_.timestamp_ns = clock_.now();
  snapshot_.sequence_number++;
  snapshot_.state = state;
  snapshot_.progress = clamp_progress(progress);
  snapshot_.joint_positions_rad = joint_positions;
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
      stop_requested_at_.reset();
      tracking_error_since_.reset();
      peak_tracking_error_.reset();
      execution_started_at_ = std::chrono::steady_clock::now();
      applied_runtime_velocity_scale_.store(
          runtime_velocity_scale_.load(std::memory_order_acquire),
          std::memory_order_release);
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
    const double target_scale = clamp_progress(
        runtime_velocity_scale_.load(std::memory_order_acquire));
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
    const double progress =
        total_duration_s <= 1e-9 ? 1.0 : clamp_progress(trajectory_time_s / total_duration_s);
    auto predicted_positions = interpolate_joint_positions(trajectory, trajectory_time_s);
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
          const auto expected = interpolate_joint_positions(trajectory, trajectory_time_s);
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
  GoalHandleFJT::SharedPtr streaming_goal_to_cancel;
  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_goal_to_cancel = streaming_active_goal_;
    streaming_active_goal_.reset();
    streaming_terminal_goal_id_.reset();
    streaming_terminal_result_received_ = true;
    streaming_terminal_result_success_ = true;
    streaming_terminal_error_.reset();
  }
  if (streaming_thread_.joinable()) {
    streaming_thread_.request_stop();
    streaming_cv_.notify_all();
    cancel_goal_if_known(streaming_goal_to_cancel);
    {
      std::unique_lock<std::mutex> lock(streaming_mutex_);
      const bool streaming_stopped = streaming_cv_.wait_for(
          lock,
          std::chrono::duration<double>(config_.stop_completion_timeout_s),
          [this]() { return !streaming_active_.load(); });
      if (!streaming_stopped) {
        const auto snapshot = get_execution_snapshot();
        const auto error = operator_intervention_error(
            core::ErrorCode::kResetFailed,
            "Timed out waiting for ros2_control streaming worker to stop",
            "reset_streaming_join_timeout",
            snapshot.state);
        publish_snapshot(core::ExecutionState::kInterventionRequired,
                         snapshot.progress,
                         snapshot.joint_positions_rad,
                         error);
        return core::ResetResultDTO{false, error};
      }
    }
    streaming_thread_.join();
  } else {
    cancel_goal_if_known(streaming_goal_to_cancel);
  }
  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_target_.reset();
    streaming_active_goal_.reset();
    streaming_terminal_goal_id_.reset();
    streaming_terminal_result_received_ = true;
    streaming_terminal_result_success_ = true;
    streaming_terminal_error_.reset();
    streaming_config_ = core::StreamingSessionConfigDTO{};
  }

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
  auto streaming_ik_opts = ik_opts;
  streaming_ik_opts.max_time = kernel_config.streaming_ik_timeout_s;
  bundle->streaming_ik =
      std::make_shared<roboplan::SimpleIk>(bundle->scene, streaming_ik_opts);

  auto scene_mutex = std::make_shared<std::mutex>();
  bundle->planner = std::make_unique<RoboPlanPlannerBackend>(
      bundle->scene, rrt_opts, bundle->shortcutter, bundle->toppra, bundle->ik,
      scene_mutex, bundle->streaming_ik);
  bundle->context = std::make_unique<RoboPlanPlanningContext>(
      bundle->scene, group_name, scene_mutex);
  bundle->clock = std::make_unique<WallClock>();
  bundle->execution = std::make_unique<Ros2ControlExecutionBackend>(
      bundle->scene, group_name, *bundle->clock, ros_config, scene_mutex);
  bundle->scheduler = std::make_unique<SimpleScheduler>();
  bundle->cache = std::make_unique<core::TrajectoryCache>();
  bundle->kernel = std::make_unique<core::ExecutionKernel>(
      *bundle->planner, *bundle->context, *bundle->execution,
      *bundle->clock, *bundle->scheduler, *bundle->cache, kernel_config);
  bundle->event_collector.subscribe(*bundle->kernel);
  return bundle;
}

// =================================================================
// Streaming-session mode (ADR-0016).
//
// Joint-only in v0.1. Cartesian targets are rejected; the IK pre-
// resolution path is deferred. Anchored two-point FJT goals preserve
// JTC spline continuity across preempts.
// =================================================================

bool Ros2ControlExecutionBackend::streaming_send_anchored_goal(
    const std::vector<std::string>& joint_names,
    const std::vector<double>& target_positions,
    double target_time_from_start_s,
    bool track_terminal_goal) {
  if (joint_names.empty() || target_positions.empty()) {
    if (track_terminal_goal) {
      std::lock_guard<std::mutex> lock(streaming_mutex_);
      streaming_terminal_result_received_ = true;
      streaming_terminal_result_success_ = false;
      streaming_terminal_error_ = make_error(
          core::ErrorCode::kStopFailed,
          "streaming terminal soft-stop target is empty",
          {{"reason_code", "soft_stop_dispatch_failed"},
           {"failure_stage", "empty_terminal_target"}});
      streaming_cv_.notify_all();
    }
    return false;
  }
  if (target_positions.size() != joint_names.size()) {
    if (track_terminal_goal) {
      std::lock_guard<std::mutex> lock(streaming_mutex_);
      streaming_terminal_result_received_ = true;
      streaming_terminal_result_success_ = false;
      streaming_terminal_error_ = make_error(
          core::ErrorCode::kStopFailed,
          "streaming terminal soft-stop target size does not match joint names",
          {{"reason_code", "soft_stop_dispatch_failed"},
           {"failure_stage", "terminal_target_size_mismatch"},
           {"joint_count", std::to_string(joint_names.size())},
           {"position_count", std::to_string(target_positions.size())}});
      streaming_cv_.notify_all();
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
    // This degrades smoothness on the first frame but the second frame
    // and on will recover. The kernel's freshness gates should normally
    // ensure observed state is present before begin_streaming_session.
    anchor_positions = target_positions;
    anchor_velocities.assign(joint_names.size(), 0.0);
  }

  if (!wait_for_fjt_action_server(action_client_,
                                  config_.wait_for_action_server_timeout_s)) {
    if (track_terminal_goal) {
      std::lock_guard<std::mutex> lock(streaming_mutex_);
      streaming_terminal_result_received_ = true;
      streaming_terminal_result_success_ = false;
      streaming_terminal_error_ = make_error(
          core::ErrorCode::kStopFailed,
          "FollowJointTrajectory action server unavailable for streaming "
          "terminal soft-stop",
          {{"reason_code", "soft_stop_dispatch_failed"},
           {"failure_stage", "action_server_unavailable"},
           {"action_name", config_.action_name}});
      streaming_cv_.notify_all();
    }
    return false;
  }

  if (track_terminal_goal) {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_terminal_goal_id_.reset();
    streaming_terminal_result_received_ = false;
    streaming_terminal_result_success_ = false;
    streaming_terminal_error_.reset();
  }
  const auto mark_terminal_dispatch_failed = [&](std::string message) {
    if (!track_terminal_goal) {
      return;
    }
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_terminal_result_received_ = true;
    streaming_terminal_result_success_ = false;
    streaming_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed, std::move(message));
    streaming_cv_.notify_all();
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
      streaming_config_.anchor_negative_time_s);
  goal.trajectory.points.push_back(std::move(anchor_point));

  // Target point: requested final position, zero velocity (the next
  // streamed target will reset velocity at the next preempt anyway).
  trajectory_msgs::msg::JointTrajectoryPoint target_point;
  target_point.positions = target_positions;
  target_point.velocities.assign(joint_names.size(), 0.0);
  target_point.accelerations.assign(joint_names.size(), 0.0);
  target_point.time_from_start = duration_from_seconds(
      std::max(target_time_from_start_s,
               streaming_config_.target_time_from_start_min_s));
  goal.trajectory.points.push_back(std::move(target_point));

  rclcpp_action::Client<FollowJointTrajectory>::SendGoalOptions options;
  options.feedback_callback =
      [this](GoalHandleFJT::SharedPtr goal_handle,
             const std::shared_ptr<const FollowJointTrajectory::Feedback> feedback) {
        on_action_feedback(goal_handle, feedback);
      };
  options.result_callback =
      [this, track_terminal_goal](const GoalHandleFJT::WrappedResult& result) {
        // The streaming dispatcher does not block on per-goal results;
        // it preempts via async_send_goal. Route the result through the
        // existing callback so feedback freshness gates and tracking
        // peak telemetry continue to work uniformly.
        if (track_terminal_goal) {
          on_streaming_terminal_result(result);
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
        "timed out waiting for streaming terminal soft-stop goal acceptance");
    return false;
  }
  GoalHandleFJT::SharedPtr goal_handle;
  try {
    goal_handle = goal_handle_future.get();
  } catch (const std::exception&) {
    mark_terminal_dispatch_failed(
        "streaming terminal soft-stop goal acceptance threw an exception");
    return false;
  }
  if (!goal_handle) {
    mark_terminal_dispatch_failed(
        "streaming terminal soft-stop goal was rejected by the controller");
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_active_goal_ = goal_handle;
    if (track_terminal_goal) {
      streaming_terminal_goal_id_ = goal_handle->get_goal_id();
    }
  }
  return true;
}

void Ros2ControlExecutionBackend::on_streaming_terminal_result(
    const GoalHandleFJT::WrappedResult& result) {
  const bool controller_success =
      result.result &&
      result.result->error_code == FollowJointTrajectory::Result::SUCCESSFUL;
  const bool terminal_success =
      result.code == rclcpp_action::ResultCode::SUCCEEDED && controller_success;

  std::lock_guard<std::mutex> lock(streaming_mutex_);
  if (streaming_terminal_goal_id_.has_value() &&
      result.goal_id != *streaming_terminal_goal_id_) {
    return;
  }
  streaming_terminal_result_received_ = true;
  streaming_terminal_result_success_ = terminal_success;
  streaming_active_goal_.reset();
  streaming_terminal_goal_id_.reset();
  if (!terminal_success) {
    const std::string controller_error =
        result.result ? result.result->error_string : "controller returned no result";
    streaming_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed,
        controller_error.empty()
            ? "streaming terminal soft-stop goal failed"
            : "streaming terminal soft-stop goal failed: " + controller_error,
        {{"result_code", std::to_string(static_cast<int>(result.code))}});
  } else {
    streaming_terminal_error_.reset();
  }
  streaming_cv_.notify_all();
}

bool Ros2ControlExecutionBackend::streaming_send_spring_back_stop() {
  // Final-goal soft-stop: anchor at observed state (neg time) + target at
  // observed *position* with zero velocity at +soft_stop_deceleration_time_s.
  std::vector<double> observed_positions;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (latest_action_feedback_.has_value()) {
      observed_positions = positions_for_joints(
          *latest_action_feedback_, config_.required_joints);
    } else if (latest_controller_state_.has_value()) {
      observed_positions = positions_for_joints(
          *latest_controller_state_, config_.required_joints);
    } else if (latest_joint_state_.has_value()) {
      observed_positions = positions_for_joints(
          *latest_joint_state_, config_.required_joints);
    }
  }
  if (observed_positions.empty() && is_commanded_sim_mode()) {
    // Simulator-commanded profiles may not have emitted controller feedback
    // yet when the operator releases immediately after a target push. In that
    // topology the backend-authored command is the execution source of truth,
    // so a terminal hold may safely anchor to the last normalized streaming
    // target. Real hardware profiles intentionally do not take this fallback.
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    if (streaming_target_.has_value() &&
        streaming_target_->joint_positions_rad.size() ==
            config_.required_joints.size()) {
      observed_positions = streaming_target_->joint_positions_rad;
    } else if (config_.initial_joint_positions_rad.size() ==
               config_.required_joints.size()) {
      observed_positions = config_.initial_joint_positions_rad;
    }
  }
  if (observed_positions.empty()) {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    streaming_terminal_result_received_ = true;
    streaming_terminal_result_success_ = false;
    streaming_terminal_error_ = make_error(
        core::ErrorCode::kStopFailed,
        "no state available to anchor streaming terminal soft-stop",
        {{"reason_code", "soft_stop_dispatch_failed"},
         {"failure_stage", "missing_terminal_anchor_state"},
         {"state_source_mode", config_.execution_state_source_mode}});
    streaming_cv_.notify_all();
    return false;
  }
  return streaming_send_anchored_goal(
      config_.required_joints,
      observed_positions,
      streaming_config_.soft_stop_deceleration_time_s,
      true);
}

void Ros2ControlExecutionBackend::streaming_dispatch_worker(std::stop_token st) {
  const double tick_s = [&]() {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    const double rate = streaming_config_.max_dispatch_rate_hz;
    return rate > 0.0 ? (1.0 / rate) : 0.033;
  }();
  const auto tick = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::duration<double>(tick_s));

  std::vector<double> last_dispatched_positions;
  std::chrono::steady_clock::time_point last_dispatch_at =
      std::chrono::steady_clock::time_point::min();

  while (!st.stop_requested()) {
    std::optional<core::StreamingTargetDTO> target;
    {
      std::lock_guard<std::mutex> lock(streaming_mutex_);
      target = streaming_target_;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto since_dispatch =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            now - last_dispatch_at).count();

    if (target.has_value() &&
        target->kind == core::StreamingInputMode::kJoint &&
        !target->joint_positions_rad.empty()) {
      const auto& new_pos = target->joint_positions_rad;
      bool changed = (new_pos != last_dispatched_positions);
      // Honor target_max_age_s by re-dispatching the last target as a
      // hold if no new target arrives within the window. JTC will treat
      // it as the same anchored goal; this keeps the controller from
      // timing out the action.
      const bool stale_hold =
          !changed &&
          since_dispatch > streaming_config_.target_max_age_s;

      if (changed || stale_hold) {
        const auto names = streaming_config_.joint_names.empty()
                               ? config_.required_joints
                               : streaming_config_.joint_names;
        // Compute target_time_from_start from joint deltas (velocity-cap
        // bound) when we have a previous dispatch; otherwise fall back
        // to the configured minimum.
        double target_dt = streaming_config_.target_time_from_start_min_s;
        if (!last_dispatched_positions.empty() &&
            last_dispatched_positions.size() == new_pos.size()) {
          // Per-joint velocity + acceleration caps from
          // deployment.robot_config.streaming_joint_limits
          // (ADR-0016 / spec/430 §3.2).
          //
          // For each joint we compute two lower bounds on dt:
          //   dt_v_i = |Δq_i| / v_cap_i     (velocity constraint)
          //   dt_a_i = sqrt(2 * |Δq_i| / a_cap_i)
          //                                  (triangular accel-decel
          //                                   profile starting and
          //                                   ending at rest — the
          //                                   shape JTC interpolates
          //                                   for a 1-anchor + 1-goal
          //                                   FJT trajectory)
          // The dispatched `time_from_start` must be >= each lower
          // bound for every joint, so we take the max across both
          // constraints AND across joints. Falls back to engine
          // defaults (0.5 rad/s, 2.0 rad/s²) for joints without
          // explicit caps.
          constexpr double kDefaultVelRadS = 0.5;
          constexpr double kDefaultAccelRadS2 = 2.0;
          const bool have_per_joint_v =
              streaming_config_.per_joint_velocity_caps_rad_s.size()
              == new_pos.size();
          const bool have_per_joint_a =
              streaming_config_.per_joint_acceleration_caps_rad_s2.size()
              == new_pos.size();
          // ADR-0018: per-tick runtime read so HMI slider updates take effect
          // on this in-flight session without tearing it down.
          const double v_scale = std::max(
              runtime_velocity_scale_.load(std::memory_order_acquire),
              0.0);
          double required_dt = 0.0;
          for (size_t i = 0; i < new_pos.size(); ++i) {
            double v_cap_i = kDefaultVelRadS;
            if (have_per_joint_v
                && streaming_config_.per_joint_velocity_caps_rad_s[i] > 0.0) {
              v_cap_i = streaming_config_.per_joint_velocity_caps_rad_s[i];
            }
            v_cap_i *= v_scale;
            double a_cap_i = kDefaultAccelRadS2;
            if (have_per_joint_a
                && streaming_config_.per_joint_acceleration_caps_rad_s2[i] > 0.0) {
              a_cap_i = streaming_config_.per_joint_acceleration_caps_rad_s2[i];
            }
            const double delta_i = std::abs(new_pos[i] -
                                            last_dispatched_positions[i]);
            if (v_cap_i > 1e-6) {
              const double dt_v_i = delta_i / v_cap_i;
              if (dt_v_i > required_dt) required_dt = dt_v_i;
            }
            if (a_cap_i > 1e-6 && delta_i > 0.0) {
              const double dt_a_i = std::sqrt(2.0 * delta_i / a_cap_i);
              if (dt_a_i > required_dt) required_dt = dt_a_i;
            }
          }
          if (required_dt > 0.0) {
            // 20 ms margin so the controller's interpolator has headroom.
            target_dt = std::max(target_dt, required_dt + 0.020);
          }
        }
        // ADR-0015 enforcement: honor the kernel-supplied Cartesian
        // speed floor (target.min_time_from_start_s). The kernel
        // populates this when a max_tcp_linear_speed_mps cap is in
        // effect, derived from |Δpos_cartesian| / max_speed. Taking
        // the max with joint-velocity-derived dt means BOTH joint and
        // Cartesian caps are respected on every dispatched goal.
        if (target->min_time_from_start_s > target_dt) {
          target_dt = target->min_time_from_start_s;
        }
        if (streaming_send_anchored_goal(names, new_pos, target_dt)) {
          last_dispatched_positions = new_pos;
          last_dispatch_at = now;
        } else {
          // Surface dispatch failures via the existing snapshot/error
          // channel so the kernel + WS path can correlate operator
          // frames with controller-side failures. Without this, a
          // failing JTC action server or stale joint state would
          // silently break the chain.
          publish_snapshot(
              core::ExecutionState::kStreaming,
              0.0,
              {},
              core::ErrorDTO{
                  std::string(
                      core::to_string(core::ErrorCode::kExecutionRejected)),
                  "streaming dispatch failed: action server unavailable, "
                  "stale joint state, or empty target",
                  {}});
        }
      }
    }

    std::unique_lock<std::mutex> lock(streaming_mutex_);
    streaming_cv_.wait_for(lock, tick, [&st]() {
      return st.stop_requested();
    });
  }

  streaming_active_.store(false);
  streaming_cv_.notify_all();
}

core::StreamingSessionBeginResultDTO
Ros2ControlExecutionBackend::begin_streaming_session(
    const core::StreamingSessionConfigDTO& config) {
  core::StreamingSessionBeginResultDTO result;

  {
    std::lock_guard<std::mutex> lock(execution_mutex_);
    if (running_) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(
              core::to_string(core::ErrorCode::kInvalidStateTransition)),
          "backend is currently executing a discrete trajectory",
          {}};
      return result;
    }
  }
  if (!action_client_) {
    result.accepted = false;
    result.error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInternalError)),
        "FollowJointTrajectory action client not initialized",
        {}};
    return result;
  }
  if (!wait_for_fjt_action_server(action_client_,
                                  config_.wait_for_action_server_timeout_s)) {
    result.accepted = false;
    result.error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kExecutionRejected)),
        "FollowJointTrajectory action server unavailable",
        {{"action_name", config_.action_name}}};
    return result;
  }

  uint64_t session_id = 0;
  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    if (streaming_active_.load()) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(
              core::to_string(core::ErrorCode::kInvalidStateTransition)),
          "streaming session already active on backend",
          {}};
      return result;
    }
    session_id = ++streaming_session_id_;
    streaming_config_ = config;
    streaming_target_.reset();
    streaming_active_goal_.reset();
    next_streaming_backend_seq_ = 1;
    // Seed the runtime atomic from the session config so the first dispatch
    // tick reads the same value as the begin payload. Subsequent operator
    // slider changes update this atomic through set_runtime_velocity_scale.
    {
      double initial = config.velocity_scale;
      if (!std::isfinite(initial) || initial < 0.0) initial = 0.0;
      if (initial > 1.0) initial = 1.0;
      runtime_velocity_scale_.store(
          initial, std::memory_order_release);
      applied_runtime_velocity_scale_.store(initial, std::memory_order_release);
      publish_runtime_velocity_scale(initial);
    }
    streaming_active_.store(true);
  }

  streaming_thread_ = std::jthread(
      [this](std::stop_token st) { this->streaming_dispatch_worker(st); });

  publish_snapshot(core::ExecutionState::kStreaming, 0.0, {});

  result.accepted = true;
  result.session_id = session_id;
  return result;
}

core::StreamingPushResultDTO
Ros2ControlExecutionBackend::push_streaming_target(
    uint64_t session_id,
    const core::StreamingTargetDTO& target) {
  core::StreamingPushResultDTO result;

  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    if (!streaming_active_.load() || streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {}};
      return result;
    }
    if (target.kind == core::StreamingInputMode::kCartesian) {
      // The kernel resolves cartesian targets to joint targets via
      // planner.solve_ik before forwarding (ADR-0016 IK pre-resolution).
      // Reaching this branch means a caller bypassed the kernel — defense-
      // in-depth reject. Backends are joint-only by design (ADR-0007
      // scene-ownership boundary).
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(
              core::to_string(core::ErrorCode::kUnsupportedCapability)),
          "execution backends accept joint streaming targets only; "
          "cartesian targets must be resolved by the kernel's IK "
          "pre-resolution path before reaching the backend",
          {}};
      return result;
    }
    if (target.joint_positions_rad.empty()) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
          "joint streaming target carries no joint_positions_rad",
          {}};
      return result;
    }
    streaming_target_ = target;
    result.backend_sequence = next_streaming_backend_seq_++;
  }
  streaming_cv_.notify_all();

  result.accepted = true;
  return result;
}

core::StreamingSessionEndResultDTO
Ros2ControlExecutionBackend::end_streaming_session(
    uint64_t session_id,
    core::SoftStopStrategy strategy) {
  core::StreamingSessionEndResultDTO result;

  {
    std::lock_guard<std::mutex> lock(streaming_mutex_);
    if (!streaming_active_.load() || streaming_session_id_ != session_id) {
      result.accepted = false;
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
          "session_id does not match active streaming session",
          {}};
      return result;
    }
  }

  // Stop the dispatch worker first so it doesn't race the soft-stop
  // goal with a freshly arrived target.
  streaming_thread_.request_stop();
  streaming_cv_.notify_all();

  // velocity_scaling is documented as opt-in / qualification-gated and
  // is not implemented in v0.1; fall back to spring_back with a clear
  // note in the result for the caller's audit log.
  bool ok = false;
  if (strategy == core::SoftStopStrategy::kVelocityScaling) {
    ok = streaming_send_spring_back_stop();
    if (ok) {
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
          "velocity_scaling soft-stop not yet implemented; fell back to "
          "spring_back. See docs/open-points.md qualification matrix.",
          {{"requested_strategy", to_string(strategy)},
           {"executed_strategy",
            to_string(core::SoftStopStrategy::kSpringBack)}}};
    }
  } else {
    ok = streaming_send_spring_back_stop();
  }

  if (!ok) {
    result.accepted = false;
    if (streaming_terminal_error_.has_value()) {
      result.error = streaming_terminal_error_;
    } else if (!result.error.has_value()) {
      result.error = core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kStopFailed)),
          "soft-stop trajectory dispatch failed; check action server and "
          "observed-state freshness",
          {}};
    }
    return result;
  }

  // Bounded wait for both the dispatcher shutdown and the controller's
  // terminal soft-stop result. Reporting Idle before the controller accepts
  // and completes the terminal goal would let a new command overlap a
  // soft-stop that is still in flight.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::duration<double>(
                            config_.stop_completion_timeout_s);
  std::unique_lock<std::mutex> lock(streaming_mutex_);
  const bool completed = streaming_cv_.wait_until(lock, deadline, [this]() {
    return !streaming_active_.load() && streaming_terminal_result_received_;
  });

  if (!completed) {
    result.accepted = false;
    result.error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kStopFailed)),
        "soft-stop terminal result was not observed within configured timeout",
        {{"reason_code", "soft_stop_timeout"},
         {"timeout_s", std::to_string(config_.stop_completion_timeout_s)}}};
    return result;
  }

  if (!streaming_terminal_result_success_) {
    result.accepted = false;
    result.error = streaming_terminal_error_.value_or(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kStopFailed)),
        "soft-stop terminal goal failed",
        {}});
    return result;
  }

  result.accepted = true;
  return result;
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
  streaming_cv_.notify_all();
}

}  // namespace manyforge::roboplan_backend
