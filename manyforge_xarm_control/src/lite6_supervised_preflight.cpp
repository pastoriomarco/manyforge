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
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <rclcpp/rclcpp.hpp>

#include <manyforge_xarm_control/xarm_ros_transport_session.hpp>

namespace
{

using manyforge::robot_lifecycle::FactState;
using manyforge::xarm_control::XArmObservedState;
using manyforge::xarm_control::XArmRosTransportSession;
using manyforge::xarm_control::XArmRosTransportSessionConfig;

struct Arguments
{
  std::string owner_id;
  std::string state_topic;
  std::string execute_service;
  std::string command_gate_service;
  std::string shutdown_controller_service;
  int duration_ms = 5000;
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
    const std::string_view value(argv[++index]);
    if (flag == "--owner-id") {
      result.owner_id = value;
    } else if (flag == "--state-topic") {
      result.state_topic = value;
    } else if (flag == "--execute-service") {
      result.execute_service = value;
    } else if (flag == "--command-gate-service") {
      result.command_gate_service = value;
    } else if (flag == "--shutdown-controller-service") {
      result.shutdown_controller_service = value;
    } else if (flag == "--duration-ms") {
      result.duration_ms = parse_positive_int(value, flag);
    } else if (flag == "--sample-period-ms") {
      result.sample_period_ms = parse_positive_int(value, flag);
    } else {
      throw std::invalid_argument(
              std::string("unknown argument: ") + std::string(flag));
    }
  }
  if (result.owner_id.empty() || result.state_topic.empty() ||
    result.execute_service.empty() ||
    result.command_gate_service.empty() ||
    result.shutdown_controller_service.empty())
  {
    throw std::invalid_argument(
            "owner and all supervised ROS endpoint names are required");
  }
  if (result.duration_ms < 1000 || result.duration_ms > 30000 ||
    result.sample_period_ms < 5 || result.sample_period_ms > 1000 ||
    result.sample_period_ms >= result.duration_ms)
  {
    throw std::invalid_argument("invalid preflight sampling interval");
  }
  return result;
}

std::string json_string(std::string_view value)
{
  std::ostringstream output;
  output << '"';
  for (const unsigned char character : value) {
    if (character == '"' || character == '\\') {
      output << '\\';
    }
    output << character;
  }
  output << '"';
  return output.str();
}

std::string_view fact_name(FactState value)
{
  switch (value) {
    case FactState::kFalse:
      return "false";
    case FactState::kTrue:
      return "true";
    case FactState::kUnknown:
      return "unknown";
  }
  return "unknown";
}

int run(const Arguments & arguments)
{
  XArmRosTransportSessionConfig config;
  config.owner_id = arguments.owner_id;
  config.state_topic = arguments.state_topic;
  config.execute_service = arguments.execute_service;
  config.command_gate_service = arguments.command_gate_service;
  config.shutdown_controller_service =
    arguments.shutdown_controller_service;
  auto session = std::make_unique<XArmRosTransportSession>(
    std::move(config));

  XArmObservedState final;
  std::uint64_t initial_command_attempts = 0;
  bool saw_fresh_exact_identity = false;
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(arguments.duration_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    final = session->observe_vendor(steady_now_ns());
    if (final.generation != 0) {
      if (initial_command_attempts == 0) {
        initial_command_attempts =
          final.lifecycle_command_attempt_count;
      }
      saw_fresh_exact_identity =
        saw_fresh_exact_identity ||
        (final.transport_connected == FactState::kTrue &&
        final.identity_verified == FactState::kTrue &&
        final.identity_axis == 6 &&
        final.identity_device_type == 9);
    }
    std::this_thread::sleep_for(
      std::chrono::milliseconds(arguments.sample_period_ms));
  }
  final = session->observe_vendor(steady_now_ns());
  const bool pass =
    saw_fresh_exact_identity &&
    final.generation != 0 &&
    final.transport_connected == FactState::kTrue &&
    final.identity_verified == FactState::kTrue &&
    final.lifecycle_command_attempt_count == initial_command_attempts;

  std::cout << "{\"schema\":"
            << json_string(
    "manyforge.xarm_control.ros_supervised_preflight.v1")
            << ",\"result\":" << json_string(pass ? "pass" : "fail")
            << ",\"generation\":" << final.generation
            << ",\"transport_connected\":"
            << json_string(fact_name(final.transport_connected))
            << ",\"identity_verified\":"
            << json_string(fact_name(final.identity_verified))
            << ",\"position_valid\":"
            << json_string(fact_name(final.position_valid))
            << ",\"motion_enabled\":"
            << json_string(fact_name(final.motion_enabled))
            << ",\"fault_present\":"
            << json_string(fact_name(final.fault_present))
            << ",\"lifecycle_command_attempt_count\":"
            << final.lifecycle_command_attempt_count
            << "}\n";
  return pass ? 0 : 1;
}

}  // namespace

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  int result = 2;
  try {
    result = run(parse_arguments(argc, argv));
  } catch (const std::exception & error) {
    std::cerr << error.what() << '\n';
  }
  rclcpp::shutdown();
  return result;
}
