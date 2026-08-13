// Copyright 2026 Flexin Group SRL
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <manyforge_xarm_control/xarm_ros_transport_session.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <limits>
#include <stdexcept>
#include <utility>

#include <lifecycle_msgs/msg/state.hpp>

namespace manyforge::xarm_control {
namespace {

constexpr int kLite6JointCount = 6;
constexpr int kLite6DeviceType = 9;
constexpr int kJointServoMode = 1;
constexpr int kMovingState = 1;
constexpr int kReadyFeedbackState = 2;

void append_reason(std::vector<std::string> &reasons, std::string reason) {
  if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end()) {
    reasons.push_back(std::move(reason));
  }
}

std::string node_suffix(std::string_view owner_id) {
  std::string result;
  result.reserve(owner_id.size());
  for (const unsigned char value : owner_id) {
    result.push_back(std::isalnum(value) != 0
                         ? static_cast<char>(std::tolower(value))
                         : '_');
  }
  if (result.size() > 40) {
    result.resize(40);
  }
  return result;
}

} // namespace

XArmRosTransportSession::XArmRosTransportSession(
    XArmRosTransportSessionConfig config)
    : config_(std::move(config)) {
  validate_config();
  if (!rclcpp::ok()) {
    throw std::runtime_error(
        "rclcpp must be initialized before creating xArm ROS session");
  }
  rclcpp::NodeOptions options;
  // This transport node is an internal, separately-spun ROS client. The
  // provider executable is given a process-level __node remap, which rclcpp
  // otherwise applies to every node created in the process. Ignore global
  // arguments here so the transport keeps its distinct owner-derived identity
  // instead of creating two graph nodes with the provider's name.
  options.use_global_arguments(false);
  options.start_parameter_services(false);
  options.start_parameter_event_publisher(false);
  node_ = std::make_shared<rclcpp::Node>(
      "manyforge_xarm_supervisor_" + node_suffix(config_.owner_id), options);
  const auto qos =
      rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local();
  state_subscription_ =
      node_->create_subscription<xarm_msgs::msg::SupervisedLifecycleState>(
          config_.state_topic, qos,
          [this](
              const xarm_msgs::msg::SupervisedLifecycleState::SharedPtr state) {
            handle_state(*state);
          });
  if (!config_.controller_activity_topic.empty()) {
    controller_activity_subscription_ = node_->create_subscription<
        controller_manager_msgs::msg::ControllerManagerActivity>(
        config_.controller_activity_topic, qos,
        [this](const controller_manager_msgs::msg::ControllerManagerActivity::
                   SharedPtr activity) {
          handle_controller_activity(*activity);
        });
  }
  execute_client_ =
      node_->create_client<xarm_msgs::srv::ExecuteSupervisedLifecycleCommand>(
          config_.execute_service);
  command_gate_client_ =
      node_->create_client<xarm_msgs::srv::SetSupervisedCommandGate>(
          config_.command_gate_service);
  shutdown_controller_client_ =
      node_->create_client<xarm_msgs::srv::ShutdownSupervisedController>(
          config_.shutdown_controller_service);
  if (!config_.controller_manager.empty()) {
    hardware_state_client_ = node_->create_client<
        controller_manager_msgs::srv::SetHardwareComponentState>(
        config_.controller_manager + "/set_hardware_component_state");
    switch_controller_client_ =
        node_->create_client<controller_manager_msgs::srv::SwitchController>(
            config_.controller_manager + "/switch_controller");
  }
  executor_ = std::make_shared<rclcpp::executors::SingleThreadedExecutor>();
  executor_->add_node(node_);
  executor_thread_ =
      std::thread([executor = executor_]() { executor->spin(); });
}

XArmRosTransportSession::~XArmRosTransportSession() {
  try {
    if (executor_ != nullptr) {
      executor_->cancel();
    }
  } catch (...) {
  }
  if (executor_thread_.joinable()) {
    executor_thread_.join();
  }
  try {
    if (executor_ != nullptr && node_ != nullptr) {
      executor_->remove_node(node_);
    }
  } catch (...) {
    // Destruction is a fail-closed local teardown boundary. No exception may
    // escape while unwinding an unavailable ROS context.
  }
}

std::string_view XArmRosTransportSession::owner_id() const noexcept {
  return config_.owner_id;
}

XArmObservedState
XArmRosTransportSession::observe_vendor(std::int64_t now_ns) const {
  std::scoped_lock lock(state_mutex_);
  XArmObservedState result;
  if (!have_state_) {
    result.blocking_reason_codes.push_back("xarm_ros_state_unavailable");
    return result;
  }
  if (terminal_session_change_) {
    result.blocking_reason_codes.push_back(
        "xarm_owner_session_replacement_requires_restart");
    return result;
  }

  const auto &raw = latest_state_;
  result.transport_owner_component_epoch = raw.session_id;
  result.transport_owner_progress_sequence = raw.owner_publication_sequence;
  result.transport_owner_published_timestamp_ns =
      raw.owner_published_steady_time_ns;
  result.transport_owner_received_timestamp_ns = latest_receive_ns_;
  const bool owner_publication_valid =
      raw.owner_publication_sequence > 0 &&
      raw.owner_published_steady_time_ns > 0 &&
      raw.owner_published_steady_time_ns <= now_ns && latest_receive_ns_ > 0 &&
      latest_receive_ns_ <= now_ns;
  const bool owner_delivery_fresh =
      owner_publication_valid &&
      now_ns - latest_receive_ns_ <= config_.owner_progress_max_age_ns;
  const bool bounded_lifecycle_command =
      raw.lifecycle_command_in_flight && raw.lifecycle_command_sequence > 0 &&
      raw.lifecycle_command > 0 &&
      raw.lifecycle_command_started_steady_time_ns > 0 &&
      raw.lifecycle_command_started_steady_time_ns <= now_ns &&
      raw.lifecycle_command_deadline_steady_time_ns >
          raw.lifecycle_command_started_steady_time_ns &&
      raw.lifecycle_command_deadline_steady_time_ns > now_ns;
  const bool lifecycle_command_deadline_expired =
      raw.lifecycle_command_in_flight && !bounded_lifecycle_command;
  result.transport_owner_progress_healthy =
      owner_delivery_fresh && !terminal_session_change_ &&
      raw.owner_id == config_.owner_id && !raw.session_id.empty() &&
      !raw.process_restart_required && !lifecycle_command_deadline_expired;
  if (!owner_publication_valid) {
    result.transport_owner_progress_reason = "xarm_owner_publication_invalid";
  } else if (!owner_delivery_fresh) {
    result.transport_owner_progress_reason = "xarm_owner_publication_stale";
  } else if (terminal_session_change_) {
    result.transport_owner_progress_reason = "xarm_owner_session_replaced";
  } else if (raw.owner_id != config_.owner_id) {
    result.transport_owner_progress_reason = "xarm_ros_owner_mismatch";
  } else if (raw.session_id.empty()) {
    result.transport_owner_progress_reason =
        "xarm_owner_component_epoch_unavailable";
  } else if (raw.process_restart_required) {
    result.transport_owner_progress_reason =
        "xarm_owner_process_restart_required";
  } else if (lifecycle_command_deadline_expired) {
    result.transport_owner_progress_reason =
        "xarm_lifecycle_command_deadline_expired";
  }
  result.generation = raw.generation;
  result.source_timestamp_ns = raw.source_steady_time_ns;
  const std::int64_t local_deadline =
      latest_receive_ns_ > std::numeric_limits<std::int64_t>::max() -
                               config_.observation_max_age_ns
          ? std::numeric_limits<std::int64_t>::max()
          : latest_receive_ns_ + config_.observation_max_age_ns;
  result.fresh_until_ns =
      std::min(raw.fresh_until_steady_time_ns, local_deadline);
  result.transport_connected =
      fact(raw.transport_connected && raw.report_connected &&
           !raw.process_restart_required);
  result.identity_verified =
      fact(raw.identity_verified && raw.identity_axis == kLite6JointCount &&
           raw.identity_device_type == kLite6DeviceType);
  result.position_valid = fact(raw.position_valid);
  result.position_ever_initialized = fact(raw.position_ever_initialized);
  result.stationary = fact(raw.stationary);
  const bool shutdown_error_allowed =
      raw.error_code == 0 || raw.error_code == 1 || raw.error_code == 2 ||
      raw.error_code == 3;
  result.controller_shutdown_eligible =
      controller_activity_known_
          ? fact(raw.transport_connected && raw.report_connected &&
                 raw.identity_verified && !raw.process_restart_required &&
                 !raw.command_gate_open && !raw.hardware_component_active &&
                 !trajectory_controller_active_ &&
                 raw.position_ever_initialized && raw.stationary &&
                 raw.state == 4 && raw.mode == 1 && raw.brake_mask == 0 &&
                 raw.servo_enable_mask == 0 && shutdown_error_allowed &&
                 raw.warning_code == 0)
          : FactState::kUnknown;
  result.hardware_component_active = fact(raw.hardware_component_active);
  result.trajectory_controller_active =
      controller_activity_known_ ? fact(trajectory_controller_active_)
                                 : FactState::kUnknown;
  result.identity_axis = raw.identity_axis;
  result.identity_device_type = raw.identity_device_type;
  result.mode = raw.mode;
  result.state = raw.state;
  result.command_count = raw.command_count;
  result.brake_mask = raw.brake_mask;
  result.servo_enable_mask = raw.servo_enable_mask;
  result.error_code = raw.error_code;
  result.warning_code = raw.warning_code;
  result.report_sample_count = raw.report_sample_count;
  result.joint_read_attempt_count = raw.joint_read_attempt_count;
  result.joint_read_success_count = raw.joint_read_success_count;
  result.joint_write_attempt_count = raw.joint_write_attempt_count;
  result.lifecycle_command_attempt_count = raw.lifecycle_command_attempt_count;
  result.shutdown_controller_attempt_count =
      raw.shutdown_controller_attempt_count;

  if (raw.owner_id != config_.owner_id) {
    result.transport_owner_progress_healthy = false;
    result.transport_owner_progress_reason = "xarm_ros_owner_mismatch";
    result.generation = 0;
    result.blocking_reason_codes.push_back("xarm_ros_owner_mismatch");
    return result;
  }
  if (raw.session_id.empty()) {
    result.transport_owner_progress_healthy = false;
    result.transport_owner_progress_reason =
        "xarm_owner_component_epoch_unavailable";
    result.generation = 0;
    result.blocking_reason_codes.push_back(
        "xarm_owner_component_epoch_unavailable");
    return result;
  }
  if (raw.process_restart_required) {
    append_reason(result.blocking_reason_codes,
                  "xarm_owner_process_restart_required");
  }
  if (result.fresh_until_ns <= now_ns || result.source_timestamp_ns <= 0 ||
      result.source_timestamp_ns > now_ns) {
    result.generation = 0;
    append_reason(result.blocking_reason_codes, "xarm_ros_state_stale");
    return result;
  }
  if (result.transport_connected != FactState::kTrue) {
    append_reason(result.blocking_reason_codes, "xarm_transport_disconnected");
  }
  if (result.identity_verified != FactState::kTrue) {
    append_reason(result.blocking_reason_codes, "xarm_identity_unverified");
  }
  if (!raw.report_received) {
    append_reason(result.blocking_reason_codes, "xarm_report_unavailable");
  }
  if (!raw.position_valid) {
    append_reason(result.blocking_reason_codes, "xarm_position_uninitialized");
  }

  if (!raw.report_received) {
    result.motion_enabled = FactState::kUnknown;
    result.fault_present = FactState::kUnknown;
    result.recovery_required = FactState::kUnknown;
    result.vendor_control_ready = FactState::kUnknown;
    return result;
  }

  constexpr int kExpectedMask = (1 << kLite6JointCount) - 1;
  const int brake_mask = raw.brake_mask & kExpectedMask;
  const int servo_mask = raw.servo_enable_mask & kExpectedMask;
  if (brake_mask == kExpectedMask && servo_mask == kExpectedMask) {
    result.motion_enabled = FactState::kTrue;
  } else if (brake_mask == 0 && servo_mask == 0) {
    result.motion_enabled = FactState::kFalse;
  } else {
    result.motion_enabled = FactState::kUnknown;
    append_reason(result.blocking_reason_codes, "xarm_drive_mask_partial");
  }

  if (raw.error_code == 0) {
    result.fault_present = FactState::kFalse;
    result.recovery_required = FactState::kFalse;
  } else if (raw.error_code > 0) {
    result.fault_present = FactState::kTrue;
    result.recovery_required = FactState::kTrue;
    append_reason(result.blocking_reason_codes, "xarm_fault_present");
  } else {
    result.fault_present = FactState::kUnknown;
    result.recovery_required = FactState::kUnknown;
  }
  if (raw.warning_code > 0) {
    append_reason(result.blocking_reason_codes, "xarm_warning_present");
  }
  const bool ready_known = result.motion_enabled != FactState::kUnknown &&
                           result.fault_present != FactState::kUnknown &&
                           raw.warning_code >= 0;
  result.vendor_control_ready =
      ready_known ? fact(result.motion_enabled == FactState::kTrue &&
                         result.position_valid == FactState::kTrue &&
                         result.fault_present == FactState::kFalse &&
                         raw.warning_code == 0 && raw.mode == kJointServoMode &&
                         (raw.state == kMovingState ||
                          raw.state == kReadyFeedbackState))
                  : FactState::kUnknown;
  if (result.vendor_control_ready != FactState::kTrue) {
    append_reason(result.blocking_reason_codes,
                  "xarm_vendor_control_not_ready");
  }
  return result;
}

XArmLifecycleCommandResult
XArmRosTransportSession::execute_primitive(XArmLifecycleCommand command,
                                           std::string_view request_id,
                                           std::uint64_t expected_generation) {
  XArmLifecycleCommandResult result;
  result.observed_generation = expected_generation;
  if (request_id.empty() || request_id.size() > 128) {
    result.reason = "invalid_request_id";
    return result;
  }
  if (command == XArmLifecycleCommand::kActivateHardwareComponent) {
    return set_hardware_component_state(true, expected_generation);
  }
  if (command == XArmLifecycleCommand::kDeactivateHardwareComponent) {
    return set_hardware_component_state(false, expected_generation);
  }
  if (command == XArmLifecycleCommand::kActivateTrajectoryControllers) {
    return switch_trajectory_controllers(true, expected_generation);
  }
  if (command == XArmLifecycleCommand::kDeactivateTrajectoryControllers) {
    return switch_trajectory_controllers(false, expected_generation);
  }
  try {
    const auto service_timeout =
        std::chrono::nanoseconds(config_.service_timeout_ns);
    const auto execute_deadline =
        std::chrono::steady_clock::now() +
        std::chrono::nanoseconds(config_.execute_timeout_ns);
    const auto discovery_budget =
        std::min(service_timeout,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                     execute_deadline - std::chrono::steady_clock::now()));
    if (command == XArmLifecycleCommand::kShutdownController) {
      if (discovery_budget <= std::chrono::nanoseconds::zero() ||
          !shutdown_controller_client_->wait_for_service(discovery_budget)) {
        result.reason = "xarm_ros_shutdown_controller_service_unavailable";
        return result;
      }
      auto request = std::make_shared<
          xarm_msgs::srv::ShutdownSupervisedController::Request>();
      request->request_id = request_id;
      request->expected_generation = expected_generation;
      request->confirmation = "SHUTDOWN_CONTROLLER";
      auto future = shutdown_controller_client_->async_send_request(request);
      result.state = XArmLifecycleResultState::kOutcomeUnknownAfterDispatch;
      if (future.wait_until(execute_deadline) != std::future_status::ready) {
        static_cast<void>(
            shutdown_controller_client_->remove_pending_request(future));
        result.reason = "xarm_ros_shutdown_controller_service_timeout";
        return result;
      }
      const auto response = future.get();
      result.state = XArmLifecycleResultState::kResponseReceived;
      result.permitted = response->permitted;
      result.attempted = response->attempted;
      result.return_code = response->return_code;
      result.reason = response->reason;
      result.observed_generation = response->observed_generation;
      result.process_restart_required = response->process_restart_required;
      return result;
    }
    if (discovery_budget <= std::chrono::nanoseconds::zero() ||
        !execute_client_->wait_for_service(discovery_budget)) {
      result.reason = "xarm_ros_execute_service_unavailable";
      return result;
    }
    auto request = std::make_shared<
        xarm_msgs::srv::ExecuteSupervisedLifecycleCommand::Request>();
    request->command = command_value(command);
    request->request_id = request_id;
    request->expected_generation = expected_generation;
    auto future = execute_client_->async_send_request(request);
    result.state = XArmLifecycleResultState::kOutcomeUnknownAfterDispatch;
    if (future.wait_until(execute_deadline) != std::future_status::ready) {
      static_cast<void>(execute_client_->remove_pending_request(future));
      result.reason = "xarm_ros_execute_service_timeout";
      return result;
    }
    const auto response = future.get();
    result.state = XArmLifecycleResultState::kResponseReceived;
    result.permitted = response->permitted;
    result.attempted = response->attempted;
    result.return_code = response->return_code;
    result.reason = response->reason;
    result.observed_generation = response->observed_generation;
    return result;
  } catch (const std::exception &) {
    result.reason =
        result.state == XArmLifecycleResultState::kOutcomeUnknownAfterDispatch
            ? "xarm_ros_execute_service_error_after_dispatch"
            : "xarm_ros_execute_service_error";
    return result;
  }
}

XArmLifecycleCommandResult
XArmRosTransportSession::set_hardware_component_state(
    bool active, std::uint64_t expected_generation) {
  XArmLifecycleCommandResult result;
  result.observed_generation = expected_generation;
  if (hardware_state_client_ == nullptr) {
    result.reason = "controller_manager_binding_unavailable";
    return result;
  }
  try {
    const auto discovery_timeout =
        std::chrono::nanoseconds(config_.service_timeout_ns);
    const auto response_timeout =
        std::chrono::nanoseconds(config_.execute_timeout_ns);
    if (!hardware_state_client_->wait_for_service(discovery_timeout)) {
      result.reason = "controller_manager_hardware_state_service_unavailable";
      return result;
    }
    auto request = std::make_shared<
        controller_manager_msgs::srv::SetHardwareComponentState::Request>();
    request->name = config_.hardware_component_name;
    request->target_state.id =
        active ? lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE
               : lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE;
    request->target_state.label = active ? "active" : "inactive";
    auto future = hardware_state_client_->async_send_request(request);
    result.state = XArmLifecycleResultState::kOutcomeUnknownAfterDispatch;
    if (future.wait_for(response_timeout) != std::future_status::ready) {
      static_cast<void>(hardware_state_client_->remove_pending_request(future));
      result.reason = "hardware_component_transition_timeout";
      return result;
    }
    result.state = XArmLifecycleResultState::kResponseReceived;
    result.permitted = true;
    result.attempted = true;
    result.return_code = future.get()->ok ? 0 : 1;
    result.reason = result.return_code == 0
                        ? "hardware_component_transition_accepted"
                        : "hardware_component_transition_rejected";
    return result;
  } catch (const std::exception &) {
    result.reason =
        result.state == XArmLifecycleResultState::kOutcomeUnknownAfterDispatch
            ? "hardware_component_transition_error_after_dispatch"
            : "hardware_component_transition_error";
    return result;
  }
}

XArmLifecycleCommandResult
XArmRosTransportSession::switch_trajectory_controllers(
    bool active, std::uint64_t expected_generation) {
  XArmLifecycleCommandResult result;
  result.observed_generation = expected_generation;
  if (switch_controller_client_ == nullptr) {
    result.reason = "controller_manager_binding_unavailable";
    return result;
  }
  try {
    const auto discovery_timeout =
        std::chrono::nanoseconds(config_.service_timeout_ns);
    const auto response_timeout =
        std::chrono::nanoseconds(config_.execute_timeout_ns);
    if (!switch_controller_client_->wait_for_service(discovery_timeout)) {
      result.reason = "controller_manager_switch_service_unavailable";
      return result;
    }
    auto request = std::make_shared<
        controller_manager_msgs::srv::SwitchController::Request>();
    if (active) {
      request->activate_controllers = {
          config_.joint_state_broadcaster_name,
          config_.trajectory_controller_name,
      };
    } else {
      request->deactivate_controllers = {
          config_.trajectory_controller_name,
          config_.joint_state_broadcaster_name,
      };
    }
    request->strictness =
        controller_manager_msgs::srv::SwitchController::Request::STRICT;
    request->activate_asap = true;
    request->timeout.sec = 3;
    auto future = switch_controller_client_->async_send_request(request);
    result.state = XArmLifecycleResultState::kOutcomeUnknownAfterDispatch;
    if (future.wait_for(response_timeout) != std::future_status::ready) {
      static_cast<void>(
          switch_controller_client_->remove_pending_request(future));
      result.reason = "controller_stack_transition_timeout";
      return result;
    }
    result.state = XArmLifecycleResultState::kResponseReceived;
    result.permitted = true;
    result.attempted = true;
    result.return_code = future.get()->ok ? 0 : 1;
    result.reason = result.return_code == 0
                        ? "controller_stack_transition_accepted"
                        : "controller_stack_transition_rejected";
    return result;
  } catch (const std::exception &) {
    result.reason =
        result.state == XArmLifecycleResultState::kOutcomeUnknownAfterDispatch
            ? "controller_stack_transition_error_after_dispatch"
            : "controller_stack_transition_error";
    return result;
  }
}

bool XArmRosTransportSession::set_command_gate(
    bool open, std::int64_t valid_until_ns, std::uint64_t expected_generation) {
  const std::int64_t now_ns = steady_now_ns();
  const std::int64_t lease_duration_ns =
      open && valid_until_ns > now_ns ? valid_until_ns - now_ns : 0;
  if (open && lease_duration_ns <= 0) {
    return false;
  }
  try {
    const auto timeout = std::chrono::nanoseconds(config_.service_timeout_ns);
    if (!command_gate_client_->wait_for_service(timeout)) {
      return false;
    }
    auto request =
        std::make_shared<xarm_msgs::srv::SetSupervisedCommandGate::Request>();
    request->request_id = "mf-gate-" + std::to_string(expected_generation) +
                          (open ? "-open" : "-close");
    request->expected_generation = expected_generation;
    request->open = open;
    request->lease_duration_ns = lease_duration_ns;
    auto future = command_gate_client_->async_send_request(request);
    if (future.wait_for(timeout) != std::future_status::ready) {
      static_cast<void>(command_gate_client_->remove_pending_request(future));
      return false;
    }
    return future.get()->accepted;
  } catch (const std::exception &) {
    return false;
  }
}

std::int64_t XArmRosTransportSession::steady_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::uint8_t
XArmRosTransportSession::command_value(XArmLifecycleCommand command) {
  return static_cast<std::uint8_t>(command);
}

FactState XArmRosTransportSession::fact(bool value) {
  return value ? FactState::kTrue : FactState::kFalse;
}

void XArmRosTransportSession::handle_state(
    const xarm_msgs::msg::SupervisedLifecycleState &state) {
  std::scoped_lock lock(state_mutex_);
  if (!source_session_id_.empty() && source_session_id_ != state.session_id) {
    terminal_session_change_ = true;
    return;
  }
  if (state.owner_publication_sequence == 0 ||
      state.owner_publication_sequence <= latest_owner_publication_sequence_) {
    return;
  }
  if (source_session_id_.empty()) {
    source_session_id_ = state.session_id;
  }
  latest_state_ = state;
  latest_receive_ns_ = steady_now_ns();
  latest_owner_publication_sequence_ = state.owner_publication_sequence;
  have_state_ = true;
}

void XArmRosTransportSession::handle_controller_activity(
    const controller_manager_msgs::msg::ControllerManagerActivity &activity) {
  std::scoped_lock lock(state_mutex_);
  trajectory_controller_active_ = std::any_of(
      activity.controllers.begin(), activity.controllers.end(),
      [this](
          const controller_manager_msgs::msg::NamedLifecycleState &controller) {
        return controller.name == config_.trajectory_controller_name &&
               controller.state.id ==
                   lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
      });
  controller_activity_known_ = true;
}

void XArmRosTransportSession::validate_config() const {
  if (config_.owner_id.empty() || config_.state_topic.empty() ||
      config_.execute_service.empty() || config_.command_gate_service.empty() ||
      config_.shutdown_controller_service.empty()) {
    throw std::invalid_argument("xArm ROS session binding is incomplete");
  }
  if (config_.observation_max_age_ns <= 0 ||
      config_.owner_progress_max_age_ns <= 0 ||
      config_.service_timeout_ns <= 0 || config_.execute_timeout_ns <= 0) {
    throw std::invalid_argument("xArm ROS session timing is invalid");
  }
  const bool any_controller_binding =
      !config_.controller_manager.empty() ||
      !config_.hardware_component_name.empty() ||
      !config_.joint_state_broadcaster_name.empty() ||
      !config_.controller_activity_topic.empty() ||
      !config_.trajectory_controller_name.empty();
  const bool complete_controller_binding =
      !config_.controller_manager.empty() &&
      !config_.hardware_component_name.empty() &&
      !config_.joint_state_broadcaster_name.empty() &&
      !config_.controller_activity_topic.empty() &&
      !config_.trajectory_controller_name.empty();
  if (any_controller_binding && !complete_controller_binding) {
    throw std::invalid_argument(
        "xArm controller-manager binding is incomplete");
  }
}

} // namespace manyforge::xarm_control
