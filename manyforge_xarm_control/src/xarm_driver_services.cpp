// Copyright 2026 Marco Pastorio
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

#include <manyforge_xarm_control/xarm_driver_services.hpp>

#include <algorithm>
#include <cmath>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <manyforge_xarm_control/xarm_robot_states_transport.hpp>

namespace manyforge::xarm_control {
namespace {

[[nodiscard]] std::string service_name(const std::string_view driver_namespace,
                                       const std::string_view service) {
  // Validates the namespace the same way as the status topic.
  static_cast<void>(xarm_robot_states_topic(driver_namespace));
  return std::string(driver_namespace) + "/" + std::string(service);
}

} // namespace

/// One request on a ready client, bounded and never retried; a non-zero
/// vendor return is an unknown outcome (R7 §2.3). M6 XADAPT: every sent
/// request logs one INFO line with its measured duration (per-call caps are
/// to be set from these data).
template <typename Service>
[[nodiscard]] XArmServiceCallResult
await_response(const typename rclcpp::Client<Service>::SharedPtr &client,
               typename Service::Request::SharedPtr request,
               const std::chrono::nanoseconds timeout,
               const std::stop_token cancellation, const rclcpp::Logger &logger,
               const char *call,
               typename Service::Response::SharedPtr *answered = nullptr) {
  XArmServiceCallResult result;
  if (cancellation.stop_requested()) {
    result.reason = "xarm_service_cancelled";
    return result;
  }
  // No discovery wait: an undiscovered service is refused before effect.
  if (!client->service_is_ready()) {
    result.reason = "xarm_service_unavailable";
    return result;
  }
  auto future = client->async_send_request(std::move(request));
  result.outcome = XArmServiceCallOutcome::kUnknown;
  const auto started = std::chrono::steady_clock::now();
  const auto log = [&](const char *outcome) {
    RCLCPP_INFO(logger, "manyforge_component_call call=xarm.%s outcome=%s duration_ms=%.1f",
                call, outcome,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          started)
                    .count());
  };
  const auto deadline = started + timeout;
  while (future.wait_for(std::chrono::milliseconds(10)) !=
         std::future_status::ready) {
    if (cancellation.stop_requested()) {
      client->remove_pending_request(future);
      log("cancelled");
      result.reason = "xarm_service_cancelled";
      return result;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      client->remove_pending_request(future);
      log("timeout");
      result.reason = "xarm_service_timeout";
      return result;
    }
  }
  const auto response = future.get();
  log(response ? "answered" : "no_response");
  if (!response) {
    result.reason = "xarm_service_no_response";
    return result;
  }
  result.vendor_return = response->ret;
  if (answered != nullptr) *answered = response;
  if (response->ret != 0) {
    // A non-zero vendor return is not a known failure: the next status
    // report shows what the controller did (R7 §2.3).
    result.reason = "xarm_service_returned_nonzero";
    return result;
  }
  result.outcome = XArmServiceCallOutcome::kSucceeded;
  return result;
}

XArmDriverServices::XArmDriverServices(rclcpp::Node &node,
                                       const std::string_view driver_namespace)
    : logger_(node.get_logger()),
      clean_error_(node.create_client<xarm_msgs::srv::Call>(
          service_name(driver_namespace, "clean_error"))),
      clean_warn_(node.create_client<xarm_msgs::srv::Call>(
          service_name(driver_namespace, "clean_warn"))),
      set_tcp_load_(node.create_client<xarm_msgs::srv::SetTcpLoad>(
          service_name(driver_namespace, "set_tcp_load"))),
      set_tcp_offset_(node.create_client<xarm_msgs::srv::SetFloat32List>(
          service_name(driver_namespace, "set_tcp_offset"))),
      set_mode_(node.create_client<xarm_msgs::srv::SetInt16>(
          service_name(driver_namespace, "set_mode"))),
      set_state_(node.create_client<xarm_msgs::srv::SetInt16>(
          service_name(driver_namespace, "set_state"))),
      get_state_(node.create_client<xarm_msgs::srv::GetInt16>(
          service_name(driver_namespace, "get_state"))),
      get_err_warn_code_(node.create_client<xarm_msgs::srv::GetInt16List>(
          service_name(driver_namespace, "get_err_warn_code"))) {}

XArmServiceCallResult
XArmDriverServices::Call(const XArmLifecycleCommand command,
                         const std::chrono::nanoseconds timeout,
                         const std::stop_token cancellation) const {
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr client;
  if (command == XArmLifecycleCommand::kClearError)
    client = clean_error_;
  else if (command == XArmLifecycleCommand::kClearWarning)
    client = clean_warn_;
  if (!client) {
    XArmServiceCallResult result;
    result.reason = "xarm_service_command_unsupported";
    return result;
  }
  return await_response<xarm_msgs::srv::Call>(
      client, std::make_shared<xarm_msgs::srv::Call::Request>(), timeout,
      cancellation, logger_,
      command == XArmLifecycleCommand::kClearError ? "clean_error" : "clean_warn");
}

XArmServiceCallResult XArmDriverServices::SetTcpLoad(
    const double mass_kg, const std::array<double, 3> &center_of_mass_m,
    const std::chrono::nanoseconds timeout,
    const std::stop_token cancellation) const {
  auto request = std::make_shared<xarm_msgs::srv::SetTcpLoad::Request>();
  request->weight = static_cast<float>(mass_kg);
  // The SDK takes the centre of gravity in millimetres (xarm_api.h).
  for (const auto value : center_of_mass_m)
    request->center_of_gravity.push_back(static_cast<float>(value * 1000.0));
  return await_response<xarm_msgs::srv::SetTcpLoad>(set_tcp_load_,
                                                    std::move(request), timeout,
                                                    cancellation, logger_, "set_tcp_load");
}

XArmServiceCallResult XArmDriverServices::SetTcpOffset(
    const core::Pose3DTO &tcp_in_flange, const std::chrono::nanoseconds timeout,
    const std::stop_token cancellation) const {
  auto request = std::make_shared<xarm_msgs::srv::SetFloat32List::Request>();
  for (const auto value : xarm_tcp_offset(tcp_in_flange))
    request->datas.push_back(static_cast<float>(value));
  return await_response<xarm_msgs::srv::SetFloat32List>(
      set_tcp_offset_, std::move(request), timeout, cancellation, logger_,
      "set_tcp_offset");
}

XArmServiceCallResult
XArmDriverServices::SetMode(const int mode, const std::chrono::nanoseconds timeout,
                            const std::stop_token cancellation) const {
  auto request = std::make_shared<xarm_msgs::srv::SetInt16::Request>();
  request->data = static_cast<std::int16_t>(mode);
  return await_response<xarm_msgs::srv::SetInt16>(set_mode_, std::move(request),
                                                  timeout, cancellation, logger_, "set_mode");
}

XArmServiceCallResult
XArmDriverServices::SetState(const int state, const std::chrono::nanoseconds timeout,
                             const std::stop_token cancellation) const {
  auto request = std::make_shared<xarm_msgs::srv::SetInt16::Request>();
  request->data = static_cast<std::int16_t>(state);
  return await_response<xarm_msgs::srv::SetInt16>(set_state_, std::move(request),
                                                  timeout, cancellation, logger_, "set_state");
}

XArmErrWarnRead
XArmDriverServices::GetErrWarnCode(const std::chrono::nanoseconds timeout,
                                   const std::stop_token cancellation) const {
  XArmErrWarnRead read;
  xarm_msgs::srv::GetInt16List::Response::SharedPtr response;
  read.call = await_response<xarm_msgs::srv::GetInt16List>(
      get_err_warn_code_, std::make_shared<xarm_msgs::srv::GetInt16List::Request>(),
      timeout, cancellation, logger_, "get_err_warn_code", &response);
  // The SDK answers 0 also with an error present (`_check_code` maps the
  // error and warning states to 0 for a non-motion command).
  if (read.call.outcome == XArmServiceCallOutcome::kSucceeded && response &&
      response->datas.size() >= 2U) {
    read.error = response->datas[0];
    read.warning = response->datas[1];
  }
  return read;
}

XArmStateRead XArmDriverServices::GetState(
    const std::chrono::nanoseconds timeout,
    const std::stop_token cancellation) const {
  XArmStateRead read;
  xarm_msgs::srv::GetInt16::Response::SharedPtr response;
  read.call = await_response<xarm_msgs::srv::GetInt16>(
      get_state_, std::make_shared<xarm_msgs::srv::GetInt16::Request>(), timeout,
      cancellation, logger_, "get_state", &response);
  if (read.call.outcome == XArmServiceCallOutcome::kSucceeded && response)
    read.state = response->data;
  return read;
}

std::array<double, 6> xarm_tcp_offset(const core::Pose3DTO &pose) {
  const auto &q = pose.orientation;
  const double norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  const double x = q.x / norm;
  const double y = q.y / norm;
  const double z = q.z / norm;
  const double w = q.w / norm;
  const double sinr_cosp = 2.0 * (w * x + y * z);
  const double cosr_cosp = 1.0 - 2.0 * (x * x + y * y);
  const double sinp = std::clamp(2.0 * (w * y - z * x), -1.0, 1.0);
  double roll = std::atan2(sinr_cosp, cosr_cosp);
  const double pitch = std::asin(sinp);
  double yaw = std::atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y * y + z * z));
  // Gimbal lock (cos pitch ~ 0): both atan2 pairs are rounding noise. Yaw is
  // 0 there and roll carries the whole turn, read from R01 and R11.
  if (std::hypot(sinr_cosp, cosr_cosp) < 1e-9) {
    roll = std::atan2(std::copysign(1.0, sinp) * 2.0 * (x * y - w * z),
                      1.0 - 2.0 * (x * x + z * z));
    yaw = 0.0;
  }
  return {pose.position_m.x_m * 1000.0, pose.position_m.y_m * 1000.0,
          pose.position_m.z_m * 1000.0, roll, pitch, yaw};
}

} // namespace manyforge::xarm_control
