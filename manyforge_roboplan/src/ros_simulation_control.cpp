#include "ros_simulation_control.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] const std::string &member(const ModelLaneRow &row, const std::string_view name) {
  const auto found = row.adapter_keys.find(name);
  if (found == row.adapter_keys.end())
    throw std::invalid_argument("the simulator's lane row has no " + std::string(name));
  return found->second;
}

[[nodiscard]] std::string service_name(const ModelLaneRow &row, const std::string_view name) {
  const auto &value = member(row, name);
  if (value.empty() || value.front() != '/')
    throw std::invalid_argument("the simulator's " + std::string(name) +
                                " must be an absolute service name");
  return value;
}

/// A positive duration in seconds (the row's scalar text).
[[nodiscard]] std::chrono::nanoseconds seconds(const ModelLaneRow &row,
                                               const std::string_view name) {
  double number{};
  try {
    number = std::stod(member(row, name));
  } catch (const std::invalid_argument &) {
    throw std::invalid_argument("the simulator's " + std::string(name) + " must be seconds");
  } catch (const std::out_of_range &) {
    throw std::invalid_argument("the simulator's " + std::string(name) + " must be seconds");
  }
  if (!std::isfinite(number) || number <= 0.0)
    throw std::invalid_argument("the simulator's " + std::string(name) + " must be positive");
  return std::chrono::nanoseconds(static_cast<std::int64_t>(number * 1e9));
}

[[nodiscard]] std::optional<SimulationState> from_message(const std::uint8_t value) {
  using Message = simulation_interfaces::msg::SimulationState;
  switch (value) {
    case Message::STATE_STOPPED:
      return SimulationState::kStopped;
    case Message::STATE_PLAYING:
      return SimulationState::kPlaying;
    case Message::STATE_PAUSED:
      return SimulationState::kPaused;
    default:
      return std::nullopt;  // quitting or unknown: no usable simulator
  }
}

[[nodiscard]] std::uint8_t to_message(const SimulationState state) {
  using Message = simulation_interfaces::msg::SimulationState;
  switch (state) {
    case SimulationState::kStopped:
      return Message::STATE_STOPPED;
    case SimulationState::kPlaying:
      return Message::STATE_PLAYING;
    case SimulationState::kPaused:
      return Message::STATE_PAUSED;
  }
  return Message::STATE_STOPPED;
}

[[nodiscard]] SimulationCall failed(std::string code, std::string message,
                                    std::optional<std::string> vendor_code = std::nullopt,
                                    std::optional<std::string> vendor_text = std::nullopt) {
  return SimulationCall{core::ActionOutcome::kFailed,
                        core::ActionError{std::move(code), std::move(message),
                                          std::move(vendor_code), std::move(vendor_text)}};
}

/// Sends one request and waits for its response, the response timeout or a
/// Stop. No response is `unknown` (the request may have been executed); it
/// is never resent.
template <class Service, class Accept>
[[nodiscard]] SimulationCall call(rclcpp::Client<Service> &client,
                                  typename Service::Request::SharedPtr request,
                                  const std::chrono::nanoseconds timeout,
                                  const std::stop_token &stop, Accept accept) {
  if (!client.service_is_ready())
    return failed("simulator_unreachable",
                  "The simulator doesn't offer " + std::string(client.get_service_name()) + ".");
  auto sent = client.async_send_request(request);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (sent.future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
    if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
      client.remove_pending_request(sent.request_id);
      return SimulationCall{core::ActionOutcome::kUnknown,
                            core::ActionError{"simulation_no_response",
                                              "The simulator didn't answer the request.",
                                              std::nullopt, std::nullopt}};
    }
  }
  const auto response = sent.future.get();
  const auto &result = response->result;
  if (!accept(*response))
    return failed("simulation_request_failed", "The simulator refused the request.",
                  std::to_string(result.result), result.error_message);
  return SimulationCall{core::ActionOutcome::kSucceeded, std::nullopt};
}

}  // namespace

SimulationServices simulation_services_from_row(const ModelLaneRow &row) {
  return SimulationServices{service_name(row, "get_state_service"),
                            service_name(row, "set_state_service"),
                            service_name(row, "reset_service"),
                            seconds(row, "response_timeout_s"),
                            seconds(row, "state_poll_period_s")};
}

RosSimulationControl::RosSimulationControl(rclcpp::Node::SharedPtr node,
                                           SimulationServices services)
    : node_(std::move(node)), services_(std::move(services)) {
  if (!node_) throw std::invalid_argument("the simulator adapter needs a ROS node");
  get_state_ = node_->create_client<GetState>(services_.get_state);
  set_state_ = node_->create_client<SetState>(services_.set_state);
  reset_ = node_->create_client<ResetSimulation>(services_.reset);
  timer_ = node_->create_wall_timer(services_.state_poll_period, [this] { Poll(); });
}

RosSimulationControl::~RosSimulationControl() { timer_->cancel(); }

std::optional<SimulationState> RosSimulationControl::Observed() const {
  const std::lock_guard lock(mutex_);
  return observed_;
}

void RosSimulationControl::Record(std::optional<SimulationState> state) {
  const std::lock_guard lock(mutex_);
  observed_ = state;
}

void RosSimulationControl::Poll() {
  {
    const std::lock_guard lock(mutex_);
    if (poll_request_) {
      if (std::chrono::steady_clock::now() - poll_sent_ < services_.response_timeout) return;
      get_state_->remove_pending_request(*poll_request_);
      poll_request_.reset();
      observed_.reset();
    }
  }
  if (!get_state_->service_is_ready()) {
    Record(std::nullopt);
    return;
  }
  const std::lock_guard lock(mutex_);
  poll_sent_ = std::chrono::steady_clock::now();
  poll_request_ = get_state_
                      ->async_send_request(
                          std::make_shared<GetState::Request>(),
                          [this](rclcpp::Client<GetState>::SharedFuture future) {
                            const auto response = future.get();
                            const std::lock_guard lock(mutex_);
                            poll_request_.reset();
                            observed_ =
                                response->result.result == response->result.RESULT_OK
                                    ? from_message(response->state.state)
                                    : std::nullopt;
                          })
                      .request_id;
}

SimulationCall RosSimulationControl::Read(const std::stop_token stop) {
  std::optional<SimulationState> state;
  auto result = call(*get_state_, std::make_shared<GetState::Request>(),
                     services_.response_timeout, stop, [&](const GetState::Response &response) {
                       state = from_message(response.state.state);
                       return response.result.result == response.result.RESULT_OK &&
                              state.has_value();
                     });
  if (result.outcome == core::ActionOutcome::kSucceeded) Record(state);
  return result;
}

SimulationCall RosSimulationControl::Set(const SimulationState state,
                                         const std::stop_token stop) {
  auto request = std::make_shared<SetState::Request>();
  request->state.state = to_message(state);
  auto result = call(*set_state_, request, services_.response_timeout, stop,
                     [](const SetState::Response &response) {
                       return response.result.result == response.result.RESULT_OK ||
                              response.result.result ==
                                  SetState::Response::ALREADY_IN_TARGET_STATE;
                     });
  // CONTRACTS §6: the accepted state is the simulator's state from now on;
  // the next poll confirms it.
  if (result.outcome == core::ActionOutcome::kSucceeded) Record(state);
  return result;
}

SimulationCall RosSimulationControl::Reset(const std::stop_token stop) {
  auto request = std::make_shared<ResetSimulation::Request>();
  request->scope = ResetSimulation::Request::SCOPE_STATE;
  return call(*reset_, request, services_.response_timeout, stop,
              [](const ResetSimulation::Response &response) {
                return response.result.result == response.result.RESULT_OK;
              });
}

}  // namespace manyforge::roboplan_backend
