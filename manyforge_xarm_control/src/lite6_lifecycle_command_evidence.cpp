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

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include <manyforge_robot_lifecycle/local_operator_gateway.hpp>
#include <manyforge_xarm_control/lifecycle_ros_port.hpp>

namespace
{

namespace robot_lifecycle = manyforge::robot_lifecycle;
namespace xarm_control = manyforge::xarm_control;

constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr std::int64_t kCallerContextMarginNs = 10'000'000'000;

struct Arguments
{
  std::string robot_id;
  std::string supervisor_instance_id;
  std::string owner_id;
  std::string state_topic;
  std::string execute_service;
  std::string command_gate_service;
  std::string shutdown_controller_service;
  std::string controller_manager;
  std::string hardware_component_name;
  std::string joint_state_broadcaster_name;
  std::string controller_activity_topic;
  std::string trajectory_controller_name;
  std::string action_id;
  std::string confirm_action;
  std::string request_id;
  std::string authority_session_id;
  std::string command_source;
  std::string caller_context_id;
  std::string confirmation_evidence;
  int timeout_ms = 8000;
  int sample_period_ms = 20;
};

std::int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

int parse_positive_int(std::string_view value, std::string_view label)
{
  int parsed = 0;
  const auto converted = std::from_chars(
    value.data(), value.data() + value.size(), parsed);
  if (converted.ec != std::errc{} ||
    converted.ptr != value.data() + value.size() || parsed <= 0)
  {
    throw std::invalid_argument(
            std::string(label) + " must be a positive integer");
  }
  return parsed;
}

Arguments parse_arguments(int argc, char ** argv)
{
  Arguments result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view flag(argv[index]);
    if (index + 1 >= argc) {
      throw std::invalid_argument(
              std::string("missing value for ") + std::string(flag));
    }
    const std::string value(argv[++index]);
    if (flag == "--robot-id") {
      result.robot_id = value;
    } else if (flag == "--supervisor-instance-id") {
      result.supervisor_instance_id = value;
    } else if (flag == "--owner-id") {
      result.owner_id = value;
    } else if (flag == "--state-topic") {
      result.state_topic = value;
    } else if (flag == "--execute-service") {
      result.execute_service = value;
    } else if (flag == "--command-gate-service") {
      result.command_gate_service = value;
    } else if (flag == "--shutdown-controller-service") {
      result.shutdown_controller_service = value;
    } else if (flag == "--controller-manager") {
      result.controller_manager = value;
    } else if (flag == "--hardware-component-name") {
      result.hardware_component_name = value;
    } else if (flag == "--joint-state-broadcaster-name") {
      result.joint_state_broadcaster_name = value;
    } else if (flag == "--controller-activity-topic") {
      result.controller_activity_topic = value == "-" ? "" : value;
    } else if (flag == "--trajectory-controller-name") {
      result.trajectory_controller_name = value == "-" ? "" : value;
    } else if (flag == "--action-id") {
      result.action_id = value;
    } else if (flag == "--confirm-action") {
      result.confirm_action = value;
    } else if (flag == "--request-id") {
      result.request_id = value;
    } else if (flag == "--authority-session-id") {
      result.authority_session_id = value;
    } else if (flag == "--command-source") {
      result.command_source = value;
    } else if (flag == "--caller-context-id") {
      result.caller_context_id = value;
    } else if (flag == "--confirmation-evidence") {
      result.confirmation_evidence = value;
    } else if (flag == "--timeout-ms") {
      result.timeout_ms = parse_positive_int(value, flag);
    } else if (flag == "--sample-period-ms") {
      result.sample_period_ms = parse_positive_int(value, flag);
    } else {
      throw std::invalid_argument(
              std::string("unknown argument: ") + std::string(flag));
    }
  }
  if (result.robot_id.empty() ||
    result.supervisor_instance_id.empty() ||
    result.owner_id.empty() ||
    result.state_topic.empty() ||
    result.execute_service.empty() ||
    result.command_gate_service.empty() ||
    result.shutdown_controller_service.empty() ||
    result.controller_manager.empty() ||
    result.hardware_component_name.empty() ||
    result.joint_state_broadcaster_name.empty() ||
    result.controller_activity_topic.empty() ||
    result.trajectory_controller_name.empty() ||
    result.action_id.empty() ||
    result.request_id.empty() ||
    result.authority_session_id.empty() ||
    result.command_source.empty() ||
    result.caller_context_id.empty())
  {
    throw std::invalid_argument(
            "robot, owner, controller binding, action, request, session, "
            "and caller-context arguments are required");
  }
  if (result.action_id != result.confirm_action ||
    result.confirmation_evidence != "CONFIRM:" + result.action_id)
  {
    throw std::invalid_argument(
            "confirm-action and confirmation-evidence must exactly match "
            "the requested action");
  }
  if (result.timeout_ms < 1000 || result.timeout_ms > 30000 ||
    result.sample_period_ms < 5 || result.sample_period_ms > 1000 ||
    result.sample_period_ms >= result.timeout_ms)
  {
    throw std::invalid_argument("invalid evidence timing bounds");
  }
  return result;
}

std::string json_string(std::string_view value)
{
  constexpr char kHexDigits[] = "0123456789abcdef";
  std::ostringstream output;
  output << '"';
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20) {
          output
            << "\\u00"
            << kHexDigits[(character >> 4) & 0x0f]
            << kHexDigits[character & 0x0f];
        } else {
          output << character;
        }
        break;
    }
  }
  output << '"';
  return output.str();
}

const char * command_name(xarm_control::XArmLifecycleCommand command)
{
  switch (command) {
    case xarm_control::XArmLifecycleCommand::kClearError:
      return "clear_error";
    case xarm_control::XArmLifecycleCommand::kClearWarning:
      return "clear_warning";
    case xarm_control::XArmLifecycleCommand::kEnableMotion:
      return "enable_motion";
    case xarm_control::XArmLifecycleCommand::kDisableMotion:
      return "disable_motion";
    case xarm_control::XArmLifecycleCommand::kSelectJointServoMode:
      return "select_joint_servo_mode";
    case xarm_control::XArmLifecycleCommand::kSetReadyState:
      return "set_ready_state";
    case xarm_control::XArmLifecycleCommand::kSetStoppedState:
      return "set_stopped_state";
    case xarm_control::XArmLifecycleCommand::kShutdownController:
      return "shutdown_controller";
    case xarm_control::XArmLifecycleCommand::kActivateHardwareComponent:
      return "activate_hardware_component";
    case xarm_control::XArmLifecycleCommand::
      kActivateTrajectoryControllers:
      return "activate_trajectory_controllers";
    case xarm_control::XArmLifecycleCommand::
      kDeactivateTrajectoryControllers:
      return "deactivate_trajectory_controllers";
    case xarm_control::XArmLifecycleCommand::kDeactivateHardwareComponent:
      return "deactivate_hardware_component";
  }
  return "unknown";
}

const char * result_state_name(
  xarm_control::XArmLifecycleResultState state)
{
  switch (state) {
    case xarm_control::XArmLifecycleResultState::kNotDispatched:
      return "not_dispatched";
    case xarm_control::XArmLifecycleResultState::kExecutionDeferred:
      return "execution_deferred";
    case xarm_control::XArmLifecycleResultState::kResponseReceived:
      return "response_received";
    case xarm_control::XArmLifecycleResultState::
      kOutcomeUnknownAfterDispatch:
      return "outcome_unknown_after_dispatch";
  }
  return "unknown";
}

std::string snapshot_json(
  const robot_lifecycle::LifecycleSnapshot & snapshot)
{
  std::ostringstream output;
  output
    << "{\"generation\":" << snapshot.generation
    << ",\"capability_generation\":" << snapshot.capability_generation
    << ",\"observation_generation\":"
    << snapshot.observed.generation
    << ",\"observation_fresh\":"
    << (snapshot.observation_fresh ? "true" : "false")
    << ",\"authority_fresh\":"
    << (snapshot.authority_fresh ? "true" : "false")
    << ",\"observe_ready\":"
    << (snapshot.observe_ready ? "true" : "false")
    << ",\"controller_ready\":"
    << (snapshot.controller_ready ? "true" : "false")
    << ",\"motion_admitted\":"
    << (snapshot.motion_admitted ? "true" : "false")
    << ",\"recovery_required\":"
    << (snapshot.recovery_required ? "true" : "false")
    << ",\"transport_connected\":"
    << json_string(
    robot_lifecycle::fact_state_name(
      snapshot.observed.transport_connected))
    << ",\"identity_verified\":"
    << json_string(
    robot_lifecycle::fact_state_name(
      snapshot.observed.identity_verified))
    << ",\"position_valid\":"
    << json_string(
    robot_lifecycle::fact_state_name(
      snapshot.observed.position_valid))
    << ",\"robot_enabled\":"
    << json_string(
    robot_lifecycle::fact_state_name(
      snapshot.observed.robot_enabled))
    << ",\"vendor_control_ready\":"
    << json_string(
    robot_lifecycle::fact_state_name(
      snapshot.observed.vendor_control_ready))
    << ",\"fault_present\":"
    << json_string(
    robot_lifecycle::fact_state_name(
      snapshot.observed.fault_present))
    << '}';
  return output.str();
}

const robot_lifecycle::CapabilityDescriptor * find_capability(
  const robot_lifecycle::LifecycleSnapshot & snapshot,
  std::string_view action_id)
{
  for (const auto & capability : snapshot.capabilities) {
    if (capability.action_id == action_id) {
      return &capability;
    }
  }
  return nullptr;
}

struct RunResult
{
  bool passed = false;
  std::string failure_reason = "not_started";
  std::int64_t duration_ms = 0;
  robot_lifecycle::LifecycleSnapshot before;
  robot_lifecycle::LifecycleSnapshot after;
  robot_lifecycle::CapabilityDescriptor capability;
  bool capability_found = false;
  robot_lifecycle::GatewayMutationResult open_result;
  robot_lifecycle::GatewayMutationResult submit_result;
  robot_lifecycle::GatewayMutationResult close_result;
  std::vector<xarm_control::XArmPrimitiveAuditRecord> primitive_audit;
  std::vector<robot_lifecycle::GatewayAuditRecord> gateway_audit;
};

RunResult execute(const Arguments & arguments)
{
  RunResult result;
  const std::int64_t started_ns = steady_now_ns();
  const std::int64_t timeout_ns =
    static_cast<std::int64_t>(arguments.timeout_ms) *
    kNanosecondsPerMillisecond;
  const std::int64_t deadline_ns = started_ns + timeout_ns;

  xarm_control::XArmLifecycleRosPortConfig port_config;
  port_config.robot_id = arguments.robot_id;
  port_config.supervisor_instance_id =
    arguments.supervisor_instance_id;
  port_config.transport.owner_id = arguments.owner_id;
  port_config.transport.state_topic = arguments.state_topic;
  port_config.transport.execute_service = arguments.execute_service;
  port_config.transport.command_gate_service =
    arguments.command_gate_service;
  port_config.transport.shutdown_controller_service =
    arguments.shutdown_controller_service;
  port_config.transport.controller_manager =
    arguments.controller_manager;
  port_config.transport.hardware_component_name =
    arguments.hardware_component_name;
  port_config.transport.joint_state_broadcaster_name =
    arguments.joint_state_broadcaster_name;
  port_config.transport.controller_activity_topic =
    arguments.controller_activity_topic;
  port_config.transport.trajectory_controller_name =
    arguments.trajectory_controller_name;
  xarm_control::XArmLifecycleRosPort port(std::move(port_config));

  robot_lifecycle::LocalOperatorGatewayConfig gateway_config;
  gateway_config.robot_id = arguments.robot_id;
  gateway_config.maximum_authority_lease_ns =
    timeout_ns + kCallerContextMarginNs;
  gateway_config.minimum_assurance =
    robot_lifecycle::CallerAssurance::kUnverified;
  robot_lifecycle::LocalOperatorGateway gateway(
    std::move(gateway_config), port);

  robot_lifecycle::CallerContext caller;
  caller.command_source = arguments.command_source;
  caller.caller_context_id = arguments.caller_context_id;
  caller.assurance = robot_lifecycle::CallerAssurance::kUnverified;
  caller.asserted_at_ns = started_ns;
  caller.valid_until_ns = deadline_ns + kCallerContextMarginNs;

  while (steady_now_ns() < deadline_ns) {
    const std::int64_t now_ns = steady_now_ns();
    static_cast<void>(port.refresh_lifecycle(now_ns));
    const auto snapshot_result = gateway.snapshot(caller, now_ns);
    if (snapshot_result.caller_context_valid &&
      snapshot_result.snapshot.has_value() &&
      snapshot_result.snapshot->observed.generation != 0)
    {
      result.before = *snapshot_result.snapshot;
      break;
    }
    std::this_thread::sleep_for(
      std::chrono::milliseconds(arguments.sample_period_ms));
  }
  if (result.before.observed.generation == 0) {
    result.failure_reason = "initial_observation_unavailable";
    result.duration_ms =
      (steady_now_ns() - started_ns) / kNanosecondsPerMillisecond;
    return result;
  }

  const std::int64_t session_now_ns = steady_now_ns();
  robot_lifecycle::LocalAuthoritySessionIntent session;
  session.session_id = arguments.authority_session_id;
  session.generation = 1;
  session.valid_until_ns = deadline_ns + kCallerContextMarginNs;
  session.component_epoch = arguments.authority_session_id;
  result.open_result =
    gateway.open_session(caller, session, session_now_ns);
  if (!result.open_result.accepted) {
    result.failure_reason = result.open_result.reason;
    result.duration_ms =
      (steady_now_ns() - started_ns) / kNanosecondsPerMillisecond;
    result.gateway_audit = gateway.audit_snapshot();
    return result;
  }

  static_cast<void>(port.refresh_lifecycle(steady_now_ns()));
  const auto armed_snapshot_result =
    gateway.snapshot(caller, steady_now_ns());
  if (!armed_snapshot_result.snapshot.has_value()) {
    result.failure_reason = "authority_snapshot_unavailable";
  } else {
    result.before = *armed_snapshot_result.snapshot;
    result.after = result.before;
    const auto * capability =
      find_capability(result.before, arguments.action_id);
    if (capability == nullptr) {
      result.failure_reason = "action_capability_missing";
    } else {
      result.capability = *capability;
      result.capability_found = true;
      if (!capability->supported || !capability->available) {
        result.failure_reason = capability->availability_reason;
      } else {
        robot_lifecycle::LocalLifecycleCommandIntent intent;
        intent.request_id = arguments.request_id;
        intent.request_sequence = 1;
        intent.action_id = arguments.action_id;
        intent.authority_session_id = arguments.authority_session_id;
        intent.authority_session_generation = session.generation;
        intent.expected_snapshot_generation = result.before.generation;
        intent.confirmation_evidence =
          arguments.confirmation_evidence;
        result.submit_result =
          gateway.submit(caller, intent, steady_now_ns());
        if (!result.submit_result.accepted) {
          result.failure_reason = result.submit_result.reason;
        }
      }
    }
  }

  if (result.submit_result.accepted) {
    while (steady_now_ns() < deadline_ns) {
      const std::int64_t now_ns = steady_now_ns();
      static_cast<void>(port.refresh_lifecycle(now_ns));
      const auto snapshot_result = gateway.snapshot(caller, now_ns);
      if (snapshot_result.snapshot.has_value()) {
        result.after = *snapshot_result.snapshot;
        if (result.after.last_terminal_transaction.has_value() &&
          result.after.last_terminal_transaction->request_id ==
          arguments.request_id)
        {
          break;
        }
      }
      std::this_thread::sleep_for(
        std::chrono::milliseconds(arguments.sample_period_ms));
    }
  }

  result.primitive_audit = port.primitive_audit_snapshot();
  const auto terminal =
    result.after.last_terminal_transaction;
  const auto transaction_generation =
    result.submit_result.command_decision.has_value() ?
    result.submit_result.command_decision->transaction_generation : 0;
  const bool shutdown_controller =
    arguments.action_id == "shutdown_controller";
  const bool exact_result_generation =
    shutdown_controller ?
    (result.primitive_audit.size() == 1 &&
    result.primitive_audit.front().result.observed_generation != 0 &&
    result.primitive_audit.front().result.process_restart_required) :
    (result.primitive_audit.size() == 1 &&
    result.primitive_audit.front().result.observed_generation ==
    result.primitive_audit.front().expected_observation_generation &&
    !result.primitive_audit.front().result.process_restart_required);
  const bool exact_primitive =
    result.primitive_audit.size() == 1 &&
    result.primitive_audit.front().transaction_generation ==
    transaction_generation &&
    result.primitive_audit.front().action_id == arguments.action_id &&
    result.primitive_audit.front().stage_action_id ==
    arguments.action_id &&
    result.primitive_audit.front().step_index == 0 &&
    !result.primitive_audit.front().transport_request_id.empty() &&
    result.primitive_audit.front().expected_observation_generation != 0 &&
    result.primitive_audit.front().result.state ==
    xarm_control::XArmLifecycleResultState::kResponseReceived &&
    result.primitive_audit.front().result.permitted &&
    result.primitive_audit.front().result.attempted &&
    result.primitive_audit.front().result.return_code == 0 &&
    exact_result_generation;
  const bool command_succeeded =
    result.submit_result.accepted &&
    terminal.has_value() &&
    terminal->request_id == arguments.request_id &&
    terminal->phase == robot_lifecycle::TransactionPhase::kSucceeded &&
    exact_primitive;
  const bool terminal_mismatch =
    terminal.has_value() &&
    (terminal->request_id != arguments.request_id ||
    terminal->phase != robot_lifecycle::TransactionPhase::kSucceeded);
  if (!command_succeeded && result.failure_reason == "not_started") {
    if (!terminal.has_value()) {
      result.failure_reason = "postcondition_timeout";
    } else if (terminal_mismatch) {
      result.failure_reason = terminal->result_reason.empty() ?
        "terminal_transaction_mismatch" : terminal->result_reason;
    } else {
      result.failure_reason = "primitive_audit_mismatch";
    }
  }

  result.close_result = gateway.close_session(
    caller, arguments.authority_session_id, session.generation,
    steady_now_ns());
  result.gateway_audit = gateway.audit_snapshot();
  const bool exact_gateway_audit =
    result.gateway_audit.size() == 3 &&
    result.gateway_audit[0].operation == "open_session" &&
    result.gateway_audit[0].accepted &&
    result.gateway_audit[1].operation == "submit" &&
    result.gateway_audit[1].accepted &&
    result.gateway_audit[2].operation == "close_session" &&
    result.gateway_audit[2].accepted;
  result.passed =
    command_succeeded &&
    result.close_result.accepted &&
    exact_gateway_audit;
  if (result.passed) {
    result.failure_reason.clear();
  } else if (command_succeeded && !result.close_result.accepted) {
    result.failure_reason = result.close_result.reason.empty() ?
      "authority_session_close_rejected" : result.close_result.reason;
  } else if (command_succeeded && !exact_gateway_audit) {
    result.failure_reason = "gateway_audit_incomplete";
  }
  result.duration_ms =
    (steady_now_ns() - started_ns) / kNanosecondsPerMillisecond;
  return result;
}

std::string result_json(
  const Arguments & arguments,
  const RunResult & result)
{
  std::ostringstream output;
  output
    << "{\"schema\":"
    << json_string(
    "manyforge.xarm_control.lifecycle_command_evidence.v2")
    << ",\"result\":" << json_string(result.passed ? "pass" : "fail")
    << ",\"failure_reason\":" << json_string(result.failure_reason)
    << ",\"scope\":"
    << json_string(
    "one_attended_action_via_local_gateway_and_restricted_ros")
    << ",\"duration_ms\":" << result.duration_ms
    << ",\"action_id\":" << json_string(arguments.action_id)
    << ",\"request_id\":" << json_string(arguments.request_id)
    << ",\"authority_session_id\":"
    << json_string(arguments.authority_session_id)
    << ",\"command_source\":" << json_string(arguments.command_source)
    << ",\"caller_context_id\":"
    << json_string(arguments.caller_context_id)
    << ",\"caller_assurance\":\"unverified\""
    << ",\"confirmation_evidence\":"
    << json_string(arguments.confirmation_evidence)
    << ",\"capability\":";
  if (result.capability_found) {
    output
      << "{\"label\":" << json_string(result.capability.label)
      << ",\"supported\":"
      << (result.capability.supported ? "true" : "false")
      << ",\"available\":"
      << (result.capability.available ? "true" : "false")
      << ",\"availability_reason\":"
      << json_string(result.capability.availability_reason)
      << ",\"confirmation_class\":"
      << json_string(
      robot_lifecycle::confirmation_class_name(
        result.capability.confirmation))
      << ",\"side_effect_summary\":"
      << json_string(result.capability.side_effect_summary)
      << ",\"expected_postcondition\":"
      << json_string(result.capability.expected_postcondition)
      << '}';
  } else {
    output << "null";
  }
  output
    << ",\"before\":" << snapshot_json(result.before)
    << ",\"after\":" << snapshot_json(result.after)
    << ",\"open_session\":{\"accepted\":"
    << (result.open_result.accepted ? "true" : "false")
    << ",\"reason\":" << json_string(result.open_result.reason)
    << "},\"submit\":{\"accepted\":"
    << (result.submit_result.accepted ? "true" : "false")
    << ",\"reason\":" << json_string(result.submit_result.reason);
  if (result.submit_result.command_decision.has_value()) {
    const auto & decision = *result.submit_result.command_decision;
    output
      << ",\"disposition\":"
      << json_string(
      robot_lifecycle::command_disposition_name(decision.disposition))
      << ",\"transaction_generation\":"
      << decision.transaction_generation;
  }
  output
    << "},\"close_session\":{\"accepted\":"
    << (result.close_result.accepted ? "true" : "false")
    << ",\"reason\":" << json_string(result.close_result.reason)
    << "},\"primitive_audit\":[";
  for (std::size_t index = 0;
    index < result.primitive_audit.size(); ++index)
  {
    if (index != 0) {
      output << ',';
    }
    const auto & record = result.primitive_audit[index];
    output
      << "{\"sequence\":" << record.sequence
      << ",\"occurred_at_ns\":" << record.occurred_at_ns
      << ",\"transaction_generation\":"
      << record.transaction_generation
      << ",\"action_id\":" << json_string(record.action_id)
      << ",\"stage_action_id\":"
      << json_string(record.stage_action_id)
      << ",\"step_index\":" << record.step_index
      << ",\"transport_request_id\":"
      << json_string(record.transport_request_id)
      << ",\"expected_observation_generation\":"
      << record.expected_observation_generation
      << ",\"command\":" << json_string(command_name(record.command))
      << ",\"result_state\":"
      << json_string(result_state_name(record.result.state))
      << ",\"permitted\":"
      << (record.result.permitted ? "true" : "false")
      << ",\"attempted\":"
      << (record.result.attempted ? "true" : "false")
      << ",\"return_code\":" << record.result.return_code
      << ",\"reason\":" << json_string(record.result.reason)
      << ",\"observed_generation\":"
      << record.result.observed_generation
      << ",\"process_restart_required\":"
      << (record.result.process_restart_required ? "true" : "false")
      << '}';
  }
  output << "],\"gateway_audit\":[";
  for (std::size_t index = 0;
    index < result.gateway_audit.size(); ++index)
  {
    if (index != 0) {
      output << ',';
    }
    const auto & record = result.gateway_audit[index];
    output
      << "{\"sequence\":" << record.sequence
      << ",\"occurred_at_ns\":" << record.occurred_at_ns
      << ",\"command_source\":" << json_string(record.command_source)
      << ",\"caller_context_id\":"
      << json_string(record.caller_context_id)
      << ",\"caller_assurance\":"
      << json_string(
      record.caller_assurance ==
      robot_lifecycle::CallerAssurance::kAuthenticated ?
      "authenticated" : "unverified")
      << ",\"operation\":" << json_string(record.operation)
      << ",\"target_id\":" << json_string(record.target_id)
      << ",\"caller_context_valid\":"
      << (record.caller_context_valid ? "true" : "false")
      << ",\"accepted\":" << (record.accepted ? "true" : "false")
      << ",\"reason\":" << json_string(record.reason)
      << '}';
  }
  output << "]}";
  return output.str();
}

}  // namespace

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  int status = 2;
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto result = execute(arguments);
    std::cout << result_json(arguments, result) << '\n';
    status = result.passed ? 0 : 1;
  } catch (const std::exception & error) {
    std::cerr << error.what() << '\n';
  }
  rclcpp::shutdown();
  return status;
}
