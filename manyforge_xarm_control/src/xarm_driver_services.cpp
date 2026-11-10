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
/// vendor return is reported with the answer. Every sent request logs one
/// INFO line with its measured duration.
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
    // The caller reads the vendor return (`ret=1`: a controller error is
    // present; the call may still have taken effect).
    result.reason = "xarm_service_returned_nonzero";
    return result;
  }
  result.outcome = XArmServiceCallOutcome::kSucceeded;
  return result;
}

namespace {
struct DriverEntities final {
  rclcpp::Node::SharedPtr node;
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_error;
  rclcpp::Client<xarm_msgs::srv::Call>::SharedPtr clean_warn;
  rclcpp::Client<xarm_msgs::srv::GetInt16List>::SharedPtr get_err_warn_code;
  rclcpp::Client<xarm_msgs::srv::SetInt16>::SharedPtr set_state;
  rclcpp::Client<xarm_msgs::srv::SetFloat32List>::SharedPtr set_tcp_offset;
  rclcpp::Client<xarm_msgs::srv::SetTcpLoad>::SharedPtr set_tcp_load;
  rclcpp::Client<xarm_msgs::srv::SetDigitalIO>::SharedPtr set_cgpio_digital;
  rclcpp::Client<xarm_msgs::srv::SetDigitalIO>::SharedPtr set_tgpio_digital;
  rclcpp::Client<xarm_msgs::srv::SetInt16>::SharedPtr system_control;
};
} // namespace

XArmDriverServices::XArmDriverServices(rclcpp::Node &node,
                                       const std::string_view driver_namespace,
                                       const XArmRosOwner *ros_owner)
    : logger_(node.get_logger()) {
  const auto create = [&] {
    auto entities = std::make_shared<DriverEntities>();
    if (ros_owner) entities->node = ros_owner->node;
    entities->clean_error = node.create_client<xarm_msgs::srv::Call>(
        service_name(driver_namespace, "clean_error"));
    entities->clean_warn = node.create_client<xarm_msgs::srv::Call>(
        service_name(driver_namespace, "clean_warn"));
    entities->get_err_warn_code = node.create_client<xarm_msgs::srv::GetInt16List>(
        service_name(driver_namespace, "get_err_warn_code"));
    entities->set_state = node.create_client<xarm_msgs::srv::SetInt16>(
        service_name(driver_namespace, "set_state"));
    entities->set_tcp_offset = node.create_client<xarm_msgs::srv::SetFloat32List>(
        service_name(driver_namespace, "set_tcp_offset"));
    entities->set_tcp_load = node.create_client<xarm_msgs::srv::SetTcpLoad>(
        service_name(driver_namespace, "set_tcp_load"));
    entities->set_cgpio_digital = node.create_client<xarm_msgs::srv::SetDigitalIO>(
        service_name(driver_namespace, "set_cgpio_digital"));
    entities->set_tgpio_digital = node.create_client<xarm_msgs::srv::SetDigitalIO>(
        service_name(driver_namespace, "set_tgpio_digital"));
    entities->system_control = node.create_client<xarm_msgs::srv::SetInt16>(
        service_name(driver_namespace, "system_control"));
    std::shared_ptr<void> held = ros_owner ? ros_owner->retain(std::move(entities)) : entities;
    auto *raw = static_cast<DriverEntities *>(held.get());
    clean_error_ = decltype(clean_error_)(held, raw->clean_error.get());
    clean_warn_ = decltype(clean_warn_)(held, raw->clean_warn.get());
    get_err_warn_code_ = decltype(get_err_warn_code_)(held, raw->get_err_warn_code.get());
    set_state_ = decltype(set_state_)(held, raw->set_state.get());
    set_tcp_offset_ = decltype(set_tcp_offset_)(held, raw->set_tcp_offset.get());
    set_tcp_load_ = decltype(set_tcp_load_)(held, raw->set_tcp_load.get());
    set_cgpio_digital_ = decltype(set_cgpio_digital_)(held, raw->set_cgpio_digital.get());
    set_tgpio_digital_ = decltype(set_tgpio_digital_)(held, raw->set_tgpio_digital.get());
    system_control_ = decltype(system_control_)(held, raw->system_control.get());
  };
  if (ros_owner) ros_owner->invoke(create);
  else create();
}

void XArmDriverServices::SwapRosState(XArmDriverServices &replacement) noexcept {
  std::scoped_lock lock(mutex_, replacement.mutex_);
  clean_error_.swap(replacement.clean_error_);
  clean_warn_.swap(replacement.clean_warn_);
  get_err_warn_code_.swap(replacement.get_err_warn_code_);
  set_state_.swap(replacement.set_state_);
  set_tcp_offset_.swap(replacement.set_tcp_offset_);
  set_tcp_load_.swap(replacement.set_tcp_load_);
  set_cgpio_digital_.swap(replacement.set_cgpio_digital_);
  set_tgpio_digital_.swap(replacement.set_tgpio_digital_);
  system_control_.swap(replacement.system_control_);
}

XArmServiceCallResult
XArmDriverServices::CleanError(const std::chrono::nanoseconds timeout,
                               const std::stop_token cancellation) const {
  return await_response<xarm_msgs::srv::Call>(
      Snapshot(clean_error_), std::make_shared<xarm_msgs::srv::Call::Request>(), timeout,
      cancellation, logger_, "clean_error");
}

XArmServiceCallResult
XArmDriverServices::CleanWarn(const std::chrono::nanoseconds timeout,
                              const std::stop_token cancellation) const {
  return await_response<xarm_msgs::srv::Call>(
      Snapshot(clean_warn_), std::make_shared<xarm_msgs::srv::Call::Request>(), timeout,
      cancellation, logger_, "clean_warn");
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
  return await_response<xarm_msgs::srv::SetTcpLoad>(Snapshot(set_tcp_load_),
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
      Snapshot(set_tcp_offset_), std::move(request), timeout, cancellation, logger_,
      "set_tcp_offset");
}

XArmServiceCallResult
XArmDriverServices::SetState(const int state, const std::chrono::nanoseconds timeout,
                             const std::stop_token cancellation) const {
  auto request = std::make_shared<xarm_msgs::srv::SetInt16::Request>();
  request->data = static_cast<std::int16_t>(state);
  return await_response<xarm_msgs::srv::SetInt16>(Snapshot(set_state_), std::move(request),
                                                  timeout, cancellation, logger_, "set_state");
}

XArmServiceCallResult XArmDriverServices::SetDigitalOutput(
    const XArmIoBank bank, const int ionum, const bool value,
    const std::chrono::nanoseconds timeout, const std::stop_token cancellation) const {
  // The driver's defaults: no delay, the motion queue (`sync`) kept.
  auto request = std::make_shared<xarm_msgs::srv::SetDigitalIO::Request>();
  request->ionum = static_cast<std::int16_t>(ionum);
  request->value = value ? 1 : 0;
  const bool tool = bank == XArmIoBank::kTool;
  return await_response<xarm_msgs::srv::SetDigitalIO>(
      tool ? Snapshot(set_tgpio_digital_) : Snapshot(set_cgpio_digital_), std::move(request), timeout,
      cancellation, logger_, tool ? "set_tgpio_digital" : "set_cgpio_digital");
}

XArmServiceCallResult
XArmDriverServices::SystemControl(const int value, const std::chrono::nanoseconds timeout,
                                  const std::stop_token cancellation) const {
  auto request = std::make_shared<xarm_msgs::srv::SetInt16::Request>();
  request->data = static_cast<std::int16_t>(value);
  return await_response<xarm_msgs::srv::SetInt16>(Snapshot(system_control_), std::move(request),
                                                  timeout, cancellation, logger_,
                                                  "system_control");
}

XArmErrWarnRead
XArmDriverServices::GetErrWarnCode(const std::chrono::nanoseconds timeout,
                                   const std::stop_token cancellation) const {
  XArmErrWarnRead read;
  xarm_msgs::srv::GetInt16List::Response::SharedPtr response;
  read.call = await_response<xarm_msgs::srv::GetInt16List>(
      Snapshot(get_err_warn_code_), std::make_shared<xarm_msgs::srv::GetInt16List::Request>(),
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
