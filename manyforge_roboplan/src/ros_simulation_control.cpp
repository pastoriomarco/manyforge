#include "ros_simulation_control.hpp"

#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

[[nodiscard]] const std::string &member(const ModelLaneRow &row,
                                        const std::string_view name) {
  const auto found = row.adapter_keys.find(name);
  if (found == row.adapter_keys.end())
    throw std::invalid_argument("the simulator's lane row has no " +
                                std::string(name));
  return found->second;
}

[[nodiscard]] std::string service_name(const ModelLaneRow &row,
                                       const std::string_view name) {
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
    throw std::invalid_argument("the simulator's " + std::string(name) +
                                " must be seconds");
  } catch (const std::out_of_range &) {
    throw std::invalid_argument("the simulator's " + std::string(name) +
                                " must be seconds");
  }
  if (!std::isfinite(number) || number <= 0.0)
    throw std::invalid_argument("the simulator's " + std::string(name) +
                                " must be positive");
  return std::chrono::nanoseconds(static_cast<std::int64_t>(number * 1e9));
}

[[nodiscard]] std::optional<SimulationState>
from_message(const std::uint8_t value) {
  using Message = simulation_interfaces::msg::SimulationState;
  switch (value) {
  case Message::STATE_STOPPED:
    return SimulationState::kStopped;
  case Message::STATE_PLAYING:
    return SimulationState::kPlaying;
  case Message::STATE_PAUSED:
    return SimulationState::kPaused;
  default:
    return std::nullopt; // quitting or unknown: no usable simulator
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

[[nodiscard]] SimulationCall
failed(std::string code, std::string message,
       std::optional<std::string> vendor_code = std::nullopt,
       std::optional<std::string> vendor_text = std::nullopt) {
  return SimulationCall{core::ActionOutcome::kFailed,
                        core::ActionError{std::move(code), std::move(message),
                                          std::move(vendor_code),
                                          std::move(vendor_text)}};
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
                  "The simulator doesn't offer " +
                      std::string(client.get_service_name()) + ".");
  auto sent = client.async_send_request(request);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (sent.future.wait_for(std::chrono::milliseconds(20)) !=
         std::future_status::ready) {
    if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
      client.remove_pending_request(sent.request_id);
      return SimulationCall{
          core::ActionOutcome::kUnknown,
          core::ActionError{"simulation_no_response",
                            "The simulator didn't answer the request.",
                            std::nullopt, std::nullopt}};
    }
  }
  const auto response = sent.future.get();
  const auto &result = response->result;
  if (!accept(*response))
    return failed("simulation_request_failed",
                  "The simulator refused the request.",
                  std::to_string(result.result), result.error_message);
  return SimulationCall{core::ActionOutcome::kSucceeded, std::nullopt};
}

} // namespace

SimulationServices simulation_services_from_row(const ModelLaneRow &row) {
  return SimulationServices{service_name(row, "get_state_service"),
                            service_name(row, "set_state_service"),
                            service_name(row, "reset_service"),
                            seconds(row, "response_timeout_s"),
                            seconds(row, "state_poll_period_s")};
}

struct RosSimulationControl::State final {
  struct Observation final {
    mutable std::mutex mutex;
    std::optional<SimulationState> value;
  };
  rclcpp::Node::SharedPtr node;
  std::weak_ptr<ros2_control_execution::Ros2TransportResources> owner;
  std::shared_ptr<std::atomic<bool>> active;
  SimulationServices services;
  rclcpp::Client<GetState>::SharedPtr get_state;
  rclcpp::Client<SetState>::SharedPtr set_state;
  rclcpp::Client<ResetSimulation>::SharedPtr reset;
  rclcpp::TimerBase::SharedPtr timer;
  mutable std::mutex mutex;
  std::shared_ptr<Observation> observation = std::make_shared<Observation>();
  std::optional<std::int64_t> poll_request;
  std::chrono::steady_clock::time_point poll_sent;

  ~State() {
    if (timer)
      timer->cancel();
  }
  void Record(std::optional<SimulationState> value) {
    const std::lock_guard lock(observation->mutex);
    observation->value = value;
  }
  // Runs on the single-threaded owner, as does the response callback, so the
  // request id recorded after the send precedes its response. The client
  // calls are made without `mutex` (no state lock across a ROS call).
  void Poll(const std::weak_ptr<State> &weak) {
    std::optional<std::int64_t> expired;
    {
      const std::lock_guard lock(mutex);
      if (poll_request) {
        if (std::chrono::steady_clock::now() - poll_sent <
            services.response_timeout)
          return;
        expired = std::exchange(poll_request, std::nullopt);
      }
    }
    if (expired) {
      get_state->remove_pending_request(*expired);
      Record(std::nullopt);
    }
    if (!get_state->service_is_ready()) {
      Record(std::nullopt);
      return;
    }
    const auto sent_at = std::chrono::steady_clock::now();
    const auto request_id =
        get_state
            ->async_send_request(
                std::make_shared<GetState::Request>(),
                [weak](rclcpp::Client<GetState>::SharedFuture future) {
                  if (auto state = weak.lock()) {
                    const auto response = future.get();
                    const std::lock_guard lock(state->mutex);
                    state->poll_request.reset();
                    state->Record(
                        response->result.result == response->result.RESULT_OK
                            ? from_message(response->state.state)
                            : std::nullopt);
                  }
                })
            .request_id;
    const std::lock_guard lock(mutex);
    poll_sent = sent_at;
    poll_request = request_id;
  }
};

struct RosSimulationControl::Rebind final {
  std::shared_ptr<State> old_state;
  std::shared_ptr<State> replacement;
};

RosSimulationControl::RosSimulationControl(
    rclcpp::Node::SharedPtr node, SimulationServices services,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner)
    : services_(std::move(services)) {
  if (!node)
    throw std::invalid_argument("the simulator adapter needs a ROS node");
  state_ = CreateState(std::move(node), std::move(owner),
                       std::make_shared<std::atomic<bool>>(true));
}

std::shared_ptr<RosSimulationControl::State> RosSimulationControl::CreateState(
    rclcpp::Node::SharedPtr node,
    std::shared_ptr<ros2_control_execution::Ros2TransportResources> owner,
    std::shared_ptr<std::atomic<bool>> active) {
  std::shared_ptr<State> result;
  auto create = [&] {
    auto state = std::make_shared<State>();
    state->node = std::move(node);
    state->owner = owner;
    state->services = services_;
    state->active = std::move(active);
    state->get_state =
        state->node->create_client<GetState>(state->services.get_state);
    state->set_state =
        state->node->create_client<SetState>(state->services.set_state);
    state->reset =
        state->node->create_client<ResetSimulation>(state->services.reset);
    std::weak_ptr<State> weak = state;
    state->timer = state->node->create_wall_timer(
        state->services.state_poll_period, [weak] {
          if (auto state = weak.lock()) {
            if (state->active->load(std::memory_order_acquire))
              state->Poll(weak);
          }
        });
    result = owner ? owner->retain_entity(std::move(state)) : std::move(state);
  };
  if (owner)
    owner->invoke_creation(create);
  else
    create();
  return result;
}

RosSimulationControl::~RosSimulationControl() = default;

std::shared_ptr<RosSimulationControl::State> RosSimulationControl::Snapshot() const {
  std::lock_guard lock(state_mutex_);
  return state_;
}

std::shared_ptr<void> RosSimulationControl::Stage(
    const std::shared_ptr<Owner> &expected, const std::shared_ptr<Owner> &owner,
    Gate active) {
  if (!expected || !owner || !active || active->load(std::memory_order_acquire))
    throw std::invalid_argument("simulator ROS rebind requires an inactive gate and owner");
  auto old = Snapshot();
  if (old->owner.lock() != expected)
    throw std::invalid_argument("simulator ROS rebind owner changed");
  auto replacement = CreateState(owner->node(), owner, std::move(active));
  return std::make_shared<Rebind>(Rebind{std::move(old), std::move(replacement)});
}

bool RosSimulationControl::Validate(const std::shared_ptr<void> &staged) const {
  const auto rebind = std::static_pointer_cast<Rebind>(staged);
  if (!rebind || rebind->replacement->active->load(std::memory_order_acquire)) return false;
  std::lock_guard lock(state_mutex_);
  return state_ == rebind->old_state;
}

void RosSimulationControl::Commit(const std::shared_ptr<void> &staged) noexcept {
  const auto rebind = std::static_pointer_cast<Rebind>(staged);
  std::lock_guard lock(state_mutex_);
  // Preserve the latest visible value at publication, but keep each owner's
  // subsequent responses in its own observation. An old in-flight Read must
  // not overwrite a newer observation from the replacement owner.
  {
    std::lock_guard observation_lock(rebind->old_state->observation->mutex);
    rebind->replacement->observation->value =
        rebind->old_state->observation->value;
  }
  state_ = rebind->replacement;
}

std::optional<SimulationState> RosSimulationControl::Observed() const {
  auto state = Snapshot();
  const std::lock_guard lock(state->observation->mutex);
  return state->observation->value;
}

SimulationCall RosSimulationControl::Read(const std::stop_token stop) {
  auto selected = Snapshot();
  std::optional<SimulationState> state;
  auto result =
      call(*selected->get_state, std::make_shared<GetState::Request>(),
           selected->services.response_timeout, stop,
           [&](const GetState::Response &response) {
             state = from_message(response.state.state);
             return response.result.result == response.result.RESULT_OK &&
                    state.has_value();
           });
  if (result.outcome == core::ActionOutcome::kSucceeded)
    selected->Record(state);
  return result;
}

SimulationCall RosSimulationControl::Set(const SimulationState state,
                                         const std::stop_token stop) {
  auto selected = Snapshot();
  auto request = std::make_shared<SetState::Request>();
  request->state.state = to_message(state);
  auto result =
      call(*selected->set_state, request, selected->services.response_timeout, stop,
           [](const SetState::Response &response) {
             return response.result.result == response.result.RESULT_OK ||
                    response.result.result ==
                        SetState::Response::ALREADY_IN_TARGET_STATE;
           });
  // The accepted state is the simulator's state from now on;
  // the next poll confirms it.
  if (result.outcome == core::ActionOutcome::kSucceeded)
    selected->Record(state);
  return result;
}

SimulationCall RosSimulationControl::Reset(const std::stop_token stop) {
  auto selected = Snapshot();
  auto request = std::make_shared<ResetSimulation::Request>();
  request->scope = ResetSimulation::Request::SCOPE_STATE;
  return call(*selected->reset, request, selected->services.response_timeout, stop,
              [](const ResetSimulation::Response &response) {
                return response.result.result == response.result.RESULT_OK;
              });
}

} // namespace manyforge::roboplan_backend
