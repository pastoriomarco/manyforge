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
/// vendor return is an unknown outcome (R7 §2.3).
template <typename Service>
[[nodiscard]] XArmServiceCallResult
await_response(const typename rclcpp::Client<Service>::SharedPtr &client,
               typename Service::Request::SharedPtr request,
               const std::chrono::nanoseconds timeout,
               const std::stop_token cancellation) {
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
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (future.wait_for(std::chrono::milliseconds(10)) !=
         std::future_status::ready) {
    if (cancellation.stop_requested()) {
      client->remove_pending_request(future);
      result.reason = "xarm_service_cancelled";
      return result;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      client->remove_pending_request(future);
      result.reason = "xarm_service_timeout";
      return result;
    }
  }
  const auto response = future.get();
  if (!response) {
    result.reason = "xarm_service_no_response";
    return result;
  }
  result.vendor_return = response->ret;
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
    : clean_error_(node.create_client<xarm_msgs::srv::Call>(
          service_name(driver_namespace, "clean_error"))),
      clean_warn_(node.create_client<xarm_msgs::srv::Call>(
          service_name(driver_namespace, "clean_warn"))),
      set_tcp_load_(node.create_client<xarm_msgs::srv::SetTcpLoad>(
          service_name(driver_namespace, "set_tcp_load"))) {}

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
      cancellation);
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
                                                    cancellation);
}

} // namespace manyforge::xarm_control
