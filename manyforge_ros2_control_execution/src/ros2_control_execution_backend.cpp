#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#include <manyforge_ros2_control_execution/selected_ros_endpoint_facts.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace manyforge::ros2_control_execution {
namespace {
using namespace core;

ExecutionPortCommandResultDTO refused(ExecutionPortRefusal reason,
                                      std::string detail) {
  return {ExecutionPortDispatch::kNotDispatched, reason,
          ErrorDTO{"execution_rejected", std::move(detail), {}}};
}
ExecutionPortCommandResultDTO sent() {
  return {ExecutionPortDispatch::kDispatched, std::nullopt, std::nullopt};
}
ExecutionPortCommandResultDTO uncertain(std::string detail) {
  return {ExecutionPortDispatch::kEffectUncertain, std::nullopt,
          ErrorDTO{"execution_effect_uncertain", std::move(detail), {}}};
}
template <class Stamp> TimePoint nanoseconds(const Stamp &stamp) {
  return static_cast<TimePoint>(stamp.sec) * 1'000'000'000 + stamp.nanosec;
}
template <class Id> MechanismGoalId mechanism_id(const Id &id) {
  std::ostringstream text;
  text << std::hex << std::setfill('0');
  for (const auto byte : id)
    text << std::setw(2) << static_cast<unsigned>(byte);
  return MechanismGoalId(text.str());
}
std::optional<std::vector<std::size_t>>
joint_mapping(const std::vector<std::string> &source,
              const std::vector<std::string> &target) {
  if (source.size() != target.size() ||
      std::set(source.begin(), source.end()).size() != source.size())
    return std::nullopt;
  std::vector<std::size_t> order;
  for (const auto &name : target) {
    const auto found = std::find(source.begin(), source.end(), name);
    if (found == source.end())
      return std::nullopt;
    order.push_back(static_cast<std::size_t>(found - source.begin()));
  }
  return order;
}
bool copy_vector(const std::vector<double> &source,
                 const std::vector<std::size_t> &order,
                 std::vector<double> &destination, bool required = false) {
  if (source.empty() && !required) {
    destination.clear();
    return true;
  }
  if (source.size() != order.size() ||
      !std::all_of(source.begin(), source.end(),
                   [](double value) { return std::isfinite(value); }))
    return false;
  destination.clear();
  destination.reserve(order.size());
  for (const auto index : order)
    destination.push_back(source[index]);
  return true;
}
bool make_goal(const TrajectoryDTO &trajectory,
               const ExecutionLaneFactsDTO &lane,
               control_msgs::action::FollowJointTrajectory::Goal &goal,
               bool replacement) {
  const auto order = joint_mapping(trajectory.joint_names, lane.joint_names);
  if (!order || trajectory.waypoints.empty())
    return false;
  goal.trajectory.joint_names = lane.joint_names;
  double previous = -std::numeric_limits<double>::infinity();
  std::optional<std::int64_t> previous_wire_time;
  for (const auto &waypoint : trajectory.waypoints) {
    const double time = waypoint.time_from_start_s;
    if (!std::isfinite(time) || (!replacement && time < 0.0) ||
        time <= previous || time > std::numeric_limits<std::int32_t>::max() ||
        time < std::numeric_limits<std::int32_t>::min())
      return false;
    trajectory_msgs::msg::JointTrajectoryPoint point;
    if (!copy_vector(waypoint.positions_rad, *order, point.positions, true) ||
        !copy_vector(waypoint.velocities_rad_s, *order, point.velocities,
                     true) ||
        !copy_vector(waypoint.accelerations_rad_s2, *order, point.accelerations,
                     true))
      return false;
    const auto value = static_cast<std::int64_t>(std::llround(time * 1e9));
    if (previous_wire_time && value <= *previous_wire_time)
      return false;
    previous_wire_time = value;
    auto seconds = value / 1'000'000'000;
    auto nanoseconds = value % 1'000'000'000;
    if (nanoseconds < 0) {
      --seconds;
      nanoseconds += 1'000'000'000;
    }
    point.time_from_start.sec = static_cast<std::int32_t>(seconds);
    point.time_from_start.nanosec = static_cast<std::uint32_t>(nanoseconds);
    goal.trajectory.points.push_back(std::move(point));
    previous = time;
  }
  if (lane.goal_tolerance_policy == ExecutionGoalTolerancePolicy::kErased) {
    for (const auto &name : lane.joint_names) {
      control_msgs::msg::JointTolerance tolerance;
      tolerance.name = name;
      tolerance.position = tolerance.velocity = tolerance.acceleration = -1.0;
      goal.goal_tolerance.push_back(std::move(tolerance));
    }
  }
  return true;
}
} // namespace

struct Ros2ControlExecutionBackend::State {
  struct Goal {
    ExecutionGoalObservationDTO observation;
    VolatileMotionTrace::TokenPtr trace_token;
    GoalHandle::SharedPtr handle;
    std::optional<ExecutionGoalKey> successor;
    bool successor_accepted{false};
    std::optional<ExecutionGoalKey> predecessor;
    std::optional<GoalHandle::WrappedResult> terminal;
    bool retired{false};
    bool cancel_requested{false};
    bool cancel_sent{false};
    std::uint64_t cancel_sequence{0};
  };
  // Serializes SDK sends against retirement. No controller wait is permitted.
  std::mutex command_mutex;
  std::mutex mutex;
  std::weak_ptr<rclcpp_action::Client<Action>> action_client;
  std::weak_ptr<rclcpp::Node> node;
  std::shared_ptr<SelectedRosEndpointFacts> endpoints;
  core::IClock &clock;
  JtcExecutionAdapterConfig config;
  bool closed{false};
  ExecutionReadinessDTO readiness;
  // Core owns cross-operation replay. These are only the two retained facts.
  std::map<ExecutionGoalKey, Goal> goals;
  std::deque<ExecutionGoalKey> goal_order;
  std::optional<ExecutionGoalKey> current;
  std::optional<ExecutionControllerSampleDTO> sample;
  ExecutionTimingDTO timing;
  std::optional<TimePoint> previous_source_stamp;
  std::uint64_t receipt_count{0};

  State(core::IClock &clock_value, JtcExecutionAdapterConfig configuration)
      : clock(clock_value), config(std::move(configuration)) {
    readiness.binding_epoch = config.binding_epoch;
    timing.binding_epoch = config.binding_epoch;
  }
  bool ready() const {
    return !closed && readiness.controller_bound &&
           readiness.controller_active && readiness.hardware_active;
  }
  bool records_full() const {
    return std::count_if(goals.begin(), goals.end(), [](const auto &entry) {
             return entry.second.observation.phase !=
                    ExecutionGoalPhase::kTerminal;
           }) >= static_cast<std::ptrdiff_t>(kExecutionGoalRecordCapacity);
  }
  void make_record_room() {
    if (goals.size() < kExecutionGoalRecordCapacity)
      return;
    const auto victim = std::find_if(goal_order.begin(), goal_order.end(),
                                     [&](const auto &key) {
                                       return goals.at(key).observation.phase ==
                                              ExecutionGoalPhase::kTerminal;
                                     });
    // Caller checks records_full before any effect or mutation.
    if (victim == goal_order.end())
      throw std::logic_error("No finished goal record to retire");
    if (current == *victim)
      current.reset();
    goals.erase(*victim);
    goal_order.erase(victim);
  }
  void classify(Goal &goal) {
    if (!goal.terminal || goal.retired)
      return;
    const auto &terminal = *goal.terminal;
    auto &observation = goal.observation;
    observation.phase = ExecutionGoalPhase::kTerminal;
    observation.result = ExecutionGoalResult::kEndedUnknown;
    observation.preempted_by.reset();
    if (!ready() || !terminal.result)
      return;
    const int code = terminal.result->error_code;
    if (terminal.code == rclcpp_action::ResultCode::SUCCEEDED &&
        code == Action::Result::SUCCESSFUL)
      observation.result = ExecutionGoalResult::kSucceeded;
    else if (terminal.code == rclcpp_action::ResultCode::CANCELED)
      observation.result = ExecutionGoalResult::kCanceled;
    else if (code == Action::Result::PATH_TOLERANCE_VIOLATED)
      observation.result = ExecutionGoalResult::kAbortedPathTolerance;
    else if (code == Action::Result::GOAL_TOLERANCE_VIOLATED)
      observation.result = ExecutionGoalResult::kAbortedGoalTolerance;
    else if (code == Action::Result::INVALID_GOAL &&
             terminal.result->error_string ==
                 "Current goal preempted by new incoming action." &&
             goal.successor && goal.successor_accepted) {
      observation.result = ExecutionGoalResult::kPreemptedBy;
      observation.preempted_by = *goal.successor;
    }
  }
};

Ros2ControlExecutionBackend::Ros2ControlExecutionBackend(
    Ros2TransportResources &resources, core::IClock &clock,
    JtcExecutionAdapterConfig config)
    : state_(std::make_shared<State>(clock, std::move(config))) {
  const auto &configuration = state_->config;
  const auto &joints = configuration.lane_facts.joint_names;
  if (!configuration.binding_epoch.nonzero() ||
      configuration.action_name.empty() || configuration.state_topic.empty() ||
      joints.empty() ||
      std::set(joints.begin(), joints.end()).size() != joints.size() ||
      std::any_of(joints.begin(), joints.end(),
                  [](const auto &joint) { return joint.empty(); }) ||
      (configuration.lane_facts.supports_scale &&
       configuration.scale_topic.empty()))
    throw std::invalid_argument(
        "JTC adapter requires an exact binding epoch, route and joint set");
  action_client_ = rclcpp_action::create_client<Action>(
      resources.node(), configuration.action_name);
  state_->action_client = action_client_;
  state_->node = resources.node();
  state_->endpoints = std::make_shared<SelectedRosEndpointFacts>(
      resources.node(), configuration.state_topic,
      "control_msgs/msg/JointTrajectoryControllerState");
  std::weak_ptr<State> weak = state_;
  rclcpp::SubscriptionOptions options;
  options.event_callbacks.matched_callback =
      [weak](rclcpp::MatchedInfo &info) {
        if (const auto state = weak.lock()) {
          state->endpoints->ObserveMatchedCount(info.current_count);
          state->endpoints->Refresh();
        }
      };
  state_sub_ =
      resources.node()
          ->create_subscription<
              control_msgs::msg::JointTrajectoryControllerState>(
              configuration.state_topic, rclcpp::QoS(10).reliable(),
              [weak](control_msgs::msg::JointTrajectoryControllerState::
                         ConstSharedPtr message) {
                if (const auto state = weak.lock())
                  ReceiveState(state, *message);
              }, options);
  if (configuration.lane_facts.supports_scale)
    scale_pub_ = resources.node()
                     ->create_publisher<control_msgs::msg::SpeedScalingFactor>(
                         configuration.scale_topic,
                         rclcpp::QoS(1).reliable().transient_local());
}
Ros2ControlExecutionBackend::~Ros2ControlExecutionBackend() { shutdown(); }

void Ros2ControlExecutionBackend::RefreshEndpointFacts() {
  state_->endpoints->Refresh();
}

std::optional<core::ErrorDTO>
Ros2ControlExecutionBackend::ReadEndpointDiagnostic() const {
  return state_->endpoints->ReadError();
}

void Ros2ControlExecutionBackend::ObserveReadiness(
    ExecutionReadinessDTO readiness) {
  std::lock_guard command_lock(state_->command_mutex);
  std::lock_guard facts_lock(state_->mutex);
  if (state_->closed || readiness.binding_epoch != state_->config.binding_epoch)
    return;
  state_->readiness = readiness;
  if (!state_->ready()) {
    for (auto &[key, goal] : state_->goals) {
      (void)key;
      if (goal.observation.phase != ExecutionGoalPhase::kTerminal) {
        goal.observation.phase = ExecutionGoalPhase::kTerminal;
        goal.observation.result = ExecutionGoalResult::kEndedUnknown;
      }
      goal.retired = true;
      goal.handle.reset();
    }
    state_->sample.reset();
  }
}
void Ros2ControlExecutionBackend::shutdown() noexcept {
  decltype(action_client_) client;
  decltype(state_sub_) subscription;
  decltype(scale_pub_) publisher;
  {
    std::lock_guard command_lock(state_->command_mutex);
    std::lock_guard facts_lock(state_->mutex);
    if (state_->closed)
      return;
    state_->closed = true;
    state_->readiness.controller_active = state_->readiness.controller_bound =
        state_->readiness.hardware_active = false;
    for (auto &[key, goal] : state_->goals) {
      (void)key;
      if (goal.observation.phase != ExecutionGoalPhase::kTerminal) {
        goal.observation.phase = ExecutionGoalPhase::kTerminal;
        goal.observation.result = ExecutionGoalResult::kEndedUnknown;
      }
      goal.handle.reset();
    }
    state_->sample.reset();
    state_->action_client.reset();
    client = std::move(action_client_);
    subscription = std::move(state_sub_);
    publisher = std::move(scale_pub_);
  }
  // Client callbacks capture raw SDK this internally. The classic executor
  // owns its selected waitable through execution; dropping our owners here
  // cannot destroy that client in an executing callback. SDK destruction must
  // run outside command/facts locks, which late callbacks may acquire.
  subscription.reset();
  publisher.reset();
  client.reset();
  // DynamicStorage owns entities during a blocked wait. Wake the existing
  // group's waitset so retirement also reclaims a silent binding, without
  // waiting for controller traffic or stopping the shared executor.
  if (const auto node = state_->node.lock();
      node && node->get_node_base_interface()->get_context()->is_valid()) {
    try {
      node->get_node_base_interface()
          ->get_default_callback_group()
          ->trigger_notify_guard_condition();
    } catch (const std::exception &error) {
      // Expected post-context shutdown needs no wake; an active-context SDK
      // cleanup failure remains observable and is never silently retried.
      if (node->get_node_base_interface()->get_context()->is_valid())
        RCLCPP_ERROR(node->get_logger(), "JTC retirement waitset notification failed: %s",
                     error.what());
    }
  }
}

ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::Submit(const ExecutionGoalKey &key,
                                    const TrajectoryDTO &trajectory) {
  VolatileMotionTrace::Garbage garbage;
  std::lock_guard command_lock(state_->command_mutex);
  return SendGoal(key, trajectory, std::nullopt, garbage);
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::SendGoal(
    const ExecutionGoalKey &key, const TrajectoryDTO &trajectory,
    std::optional<ExecutionGoalKey> active_key,
    VolatileMotionTrace::Garbage &trace_garbage,
    std::optional<ExecutionReplacementStartDTO> start) {
  if (start) {
    if (!state_->config.lane_facts.supports_scheduled_replacement)
      return refused(ExecutionPortRefusal::kUnsupported,
                     "This lane does not support scheduled replacement");
    if (start->binding_epoch != state_->config.binding_epoch)
      return refused(ExecutionPortRefusal::kStaleKey,
                     "Scheduled replacement names another binding epoch");
    if (!active_key || start->controller_start_stamp_ns <= 0 ||
        start->controller_start_stamp_ns / 1'000'000'000 >
            std::numeric_limits<std::int32_t>::max())
      return refused(
          ExecutionPortRefusal::kInvalidInput,
          "Scheduled replacement has no representable controller start stamp");
  }
  Action::Goal goal;
  if (key.controller_goal_id.empty() ||
      !key.controller_goal_generation.nonzero() ||
      !make_goal(trajectory, state_->config.lane_facts, goal,
                 active_key.has_value()))
    return refused(ExecutionPortRefusal::kInvalidInput,
                   "Invalid full-PVA trajectory or goal key");
  if (start) {
    goal.trajectory.header.stamp.sec = static_cast<std::int32_t>(
        start->controller_start_stamp_ns / 1'000'000'000);
    goal.trajectory.header.stamp.nanosec = static_cast<std::uint32_t>(
        start->controller_start_stamp_ns % 1'000'000'000);
  }
  {
    std::lock_guard facts_lock(state_->mutex);
    if (state_->goals.contains(key))
      return refused(ExecutionPortRefusal::kStaleKey, "Goal key already used");
    if (!state_->ready())
      return refused(ExecutionPortRefusal::kNotReady,
                     "Binding is not active and bound");
    if (state_->records_full())
      return refused(ExecutionPortRefusal::kNotReady,
                     "Both goal records are unfinished");
  }
  if (!action_client_ || !action_client_->action_server_is_ready())
    return refused(ExecutionPortRefusal::kNotReady,
                   "FJT action server is not discovered");
  VolatileMotionTrace::TokenPtr trace_token;
  {
    std::lock_guard facts_lock(state_->mutex);
    if (active_key && (!state_->current || *state_->current != *active_key ||
                       state_->goals.at(*active_key).observation.phase ==
                           ExecutionGoalPhase::kTerminal))
      return refused(ExecutionPortRefusal::kStaleKey,
                     "Replacement goal is no longer current");
    state_->make_record_room();
    State::Goal record;
    record.observation.key = key;
    record.observation.binding_epoch = state_->config.binding_epoch;
    record.predecessor = state_->current;
    if (state_->config.motion_trace) {
      try {
        const auto predecessor = active_key ? state_->goals.at(*active_key).trace_token
                                            : VolatileMotionTrace::TokenPtr{};
        trace_token = state_->config.motion_trace->Begin(state_->config.action_name,
            state_->config.binding_epoch, key, predecessor, state_->clock.now(), trace_garbage);
        record.trace_token = trace_token;
      } catch (...) {} // Diagnostic capture never changes dispatch.
    }
    state_->goals.emplace(key, std::move(record));
    state_->goal_order.push_back(key);
    if (state_->current) {
      auto &predecessor = state_->goals.at(*state_->current);
      predecessor.successor = key;
      predecessor.successor_accepted = false;
    }
    state_->current = key;
  }
  std::weak_ptr<State> weak = state_;
  rclcpp_action::Client<Action>::SendGoalOptions options;
  options.goal_response_callback = [weak, key](GoalHandle::SharedPtr handle) {
    if (const auto state = weak.lock())
      ReceiveResponse(state, key, handle);
  };
  options.feedback_callback =
      [weak, key](GoalHandle::SharedPtr handle,
                  const std::shared_ptr<const Action::Feedback> feedback) {
        if (const auto state = weak.lock())
          ReceiveFeedback(state, key, handle, *feedback);
      };
  options.result_callback = [weak,
                             key](const GoalHandle::WrappedResult &result) {
    if (const auto state = weak.lock())
      ReceiveResult(state, key, result);
  };
  try {
    (void)action_client_->async_send_goal(goal, options);
  } catch (const std::exception &error) {
    if (state_->config.motion_trace) {
      try { state_->config.motion_trace->Attach(trace_token, std::move(goal), state_->clock.now(),
              ExecutionPortDispatch::kEffectUncertain, trace_garbage); } catch (...) {}
    }
    return uncertain(error.what());
  }
  if (state_->config.motion_trace) {
    try { state_->config.motion_trace->Attach(trace_token, std::move(goal), state_->clock.now(),
            ExecutionPortDispatch::kDispatched, trace_garbage); } catch (...) {}
  }
  return sent();
}
ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::Replace(const ExecutionGoalKey &active_key,
                                     const ExecutionGoalKey &next_key,
                                     const TrajectoryDTO &trajectory) {
  // Declared before the guard: retained payload garbage is released only
  // after the outer command mutex unlocks, including early returns.
  VolatileMotionTrace::Garbage garbage;
  std::lock_guard command_lock(state_->command_mutex);
  {
    std::lock_guard facts_lock(state_->mutex);
    if (!state_->current || *state_->current != active_key ||
        state_->goals.contains(next_key) ||
        state_->goals.at(active_key).observation.phase ==
            ExecutionGoalPhase::kTerminal)
      return refused(ExecutionPortRefusal::kStaleKey,
                     "Replacement keys do not name current and unused goals");
  }
  // Core owns the approved ManyMove-shaped past samples and all correction.
  // Preserve supplied time/PVA exactly; no clamp, synthetic pin or rescaling.
  return SendGoal(next_key, trajectory, active_key, garbage);
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::ReplaceScheduled(
    const ExecutionGoalKey &active_key, const ExecutionGoalKey &next_key,
    const TrajectoryDTO &trajectory,
    const ExecutionReplacementStartDTO &start) {
  VolatileMotionTrace::Garbage garbage;
  std::lock_guard command_lock(state_->command_mutex);
  {
    std::lock_guard facts_lock(state_->mutex);
    if (!state_->current || *state_->current != active_key ||
        state_->goals.contains(next_key) ||
        state_->goals.at(active_key).observation.phase ==
            ExecutionGoalPhase::kTerminal)
      return refused(ExecutionPortRefusal::kStaleKey,
                     "Replacement keys do not name current and unused goals");
  }
  // The header and the unchanged signed-time/PVA payload cross the SDK
  // boundary together, under the same epoch's command serialization.
  return SendGoal(next_key, trajectory, active_key, garbage, start);
}
ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::Cancel(const ExecutionGoalKey &key) {
  std::lock_guard command_lock(state_->command_mutex);
  GoalHandle::SharedPtr handle;
  {
    std::lock_guard facts_lock(state_->mutex);
    if (!state_->current || *state_->current != key)
      return refused(ExecutionPortRefusal::kStaleKey,
                     "Cancel key is not current");
    auto &goal = state_->goals.at(key);
    if (goal.observation.phase == ExecutionGoalPhase::kTerminal)
      return refused(ExecutionPortRefusal::kStaleKey, "Goal already ended");
    if (!state_->ready())
      return refused(ExecutionPortRefusal::kNotReady,
                     "Binding is not active and bound");
    goal.cancel_requested = true;
    handle = goal.handle;
    if (!handle)
      return uncertain(
          "Cancel intent retained until goal acceptance; not sent yet");
    goal.cancel_sent = true;
  }
  return SendCancel(state_, key, handle);
}
ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::SendCancel(const std::shared_ptr<State> &state,
                                        const ExecutionGoalKey &key,
                                        const GoalHandle::SharedPtr &handle) {
  const auto client = state->action_client.lock();
  if (!client)
    return uncertain("Cancel transport no longer exists");
  TimePoint send_boundary;
  std::uint64_t cancel_sequence;
  {
    std::lock_guard facts_lock(state->mutex);
    const auto found = state->goals.find(key);
    if (!state->ready() || found == state->goals.end() || found->second.retired)
      return refused(ExecutionPortRefusal::kNotReady,
                     "Cancel binding retired before send");
    auto &goal = found->second;
    cancel_sequence = ++goal.cancel_sequence;
    send_boundary = state->clock.now();
    goal.observation.cancel_sent_local_stamp.reset();
    goal.observation.cancel_ack_local_receipt_stamp.reset();
  }
  std::weak_ptr<State> weak = state;
  try {
    (void)client->async_cancel_goal(
        handle,
        [weak, key, cancel_sequence](
            action_msgs::srv::CancelGoal::Response::SharedPtr response) {
          if (const auto locked = weak.lock())
            ReceiveCancelResponse(locked, key, cancel_sequence, response);
        });
  } catch (const std::exception &error) {
    // SDK send threw: no definite dispatched receipt is manufactured.
    return uncertain(error.what());
  }
  {
    std::lock_guard facts_lock(state->mutex);
    const auto found = state->goals.find(key);
    if (found != state->goals.end() &&
        found->second.cancel_sequence == cancel_sequence)
      found->second.observation.cancel_sent_local_stamp = send_boundary;
  }
  return sent(); // Send/ACK is not terminal completion or physical rest.
}
void Ros2ControlExecutionBackend::ReceiveCancelResponse(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    std::uint64_t cancel_sequence,
    const action_msgs::srv::CancelGoal::Response::SharedPtr &response) {
  std::lock_guard facts_lock(state->mutex);
  const auto found = state->goals.find(key);
  if (!state->ready() || found == state->goals.end() || found->second.retired ||
      found->second.cancel_sequence != cancel_sequence || !response ||
      response->return_code !=
          action_msgs::srv::CancelGoal::Response::ERROR_NONE ||
      response->goals_canceling.size() != 1)
    return;
  auto &goal = found->second;
  if (!goal.observation.mechanism_goal_id.empty() &&
      mechanism_id(response->goals_canceling.front().goal_id.uuid) ==
          goal.observation.mechanism_goal_id)
    goal.observation.cancel_ack_local_receipt_stamp = state->clock.now();
}
ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::SetScale(double value) {
  std::lock_guard command_lock(state_->command_mutex);
  if (!std::isfinite(value) || value < 0.0 || value > 1.0)
    return refused(ExecutionPortRefusal::kInvalidInput,
                   "Scale must be finite in [0,1]");
  {
    std::lock_guard facts_lock(state_->mutex);
    if (!state_->config.lane_facts.supports_scale)
      return refused(ExecutionPortRefusal::kUnsupported,
                     "Binding does not support scale input");
    if (!state_->ready())
      return refused(ExecutionPortRefusal::kNotReady,
                     "Binding is not active and bound");
  }
  control_msgs::msg::SpeedScalingFactor message;
  message.factor = value;
  try {
    scale_pub_->publish(message);
  } catch (const std::exception &error) {
    return uncertain(error.what());
  }
  if (state_->config.motion_trace) {
    try { state_->config.motion_trace->PublishedScale(state_->config.action_name,
        state_->config.binding_epoch, state_->clock.now(), value); } catch (...) {}
  }
  return sent(); // Published is not applied; only ReceiveState updates that
                 // fact.
}

void Ros2ControlExecutionBackend::ReceiveResponse(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::SharedPtr &handle) {
  // Pending cancel dispatch uses the same effect ordering as direct commands,
  // readiness loss and shutdown; SDK calls run outside the facts lock.
  std::lock_guard command_lock(state->command_mutex);
  bool send_cancel = false;
  {
    std::lock_guard facts_lock(state->mutex);
    const auto found = state->goals.find(key);
    if (!state->ready() || found == state->goals.end() || found->second.retired)
      return;
    auto &goal = found->second;
    if (!handle) {
      goal.observation.phase = ExecutionGoalPhase::kTerminal;
      goal.observation.result = ExecutionGoalResult::kRejected;
      goal.observation.terminal_local_receipt_stamp = state->clock.now();
      if (state->current == key && goal.predecessor) {
        const auto previous = state->goals.find(*goal.predecessor);
        if (previous != state->goals.end() &&
            previous->second.observation.phase !=
                ExecutionGoalPhase::kTerminal) {
          state->current = goal.predecessor;
          if (previous->second.successor == key) {
            previous->second.successor.reset();
            previous->second.successor_accepted = false;
          }
        }
      }
    } else {
      goal.handle = handle;
      goal.observation.mechanism_goal_id = mechanism_id(handle->get_goal_id());
      goal.observation.accepted_stamp_ns = state->clock.now();
      if (goal.observation.phase == ExecutionGoalPhase::kPending)
        goal.observation.phase = ExecutionGoalPhase::kAccepted;
      if (goal.cancel_requested && !goal.cancel_sent &&
          goal.observation.phase != ExecutionGoalPhase::kTerminal) {
        goal.cancel_sent = true;
        send_cancel = true;
      }
    }
    if (state->config.motion_trace)
      state->config.motion_trace->Observe(goal.trace_token, goal.observation);
    for (auto &[other_key, other] : state->goals) {
      (void)other_key;
      if (handle && other.successor == key)
        other.successor_accepted = true;
      state->classify(other);
    }
  }
  if (send_cancel)
    (void)SendCancel(state, key, handle);
}
void Ros2ControlExecutionBackend::ReceiveFeedback(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::SharedPtr &handle, const Action::Feedback &feedback) {
  std::lock_guard lock(state->mutex);
  if (!state->ready())
    return;
  const auto found = state->goals.find(key);
  if (found == state->goals.end())
    return;
  auto &goal = found->second;
  const auto id = mechanism_id(handle->get_goal_id());
  if (goal.observation.phase == ExecutionGoalPhase::kTerminal ||
      (!goal.observation.mechanism_goal_id.empty() &&
       goal.observation.mechanism_goal_id != id))
    return;
  const auto order =
      joint_mapping(feedback.joint_names, state->config.lane_facts.joint_names);
  if (!order)
    return;
  ExecutionGoalFeedbackDTO sample;
  if (!copy_vector(feedback.desired.positions, *order,
                   sample.desired_positions_rad) ||
      !copy_vector(feedback.desired.velocities, *order,
                   sample.desired_velocities_rad_s) ||
      !copy_vector(feedback.desired.accelerations, *order,
                   sample.desired_accelerations_rad_s2) ||
      !copy_vector(feedback.actual.positions, *order,
                   sample.actual_positions_rad))
    return;
  sample.source_stamp_ns = nanoseconds(feedback.header.stamp);
  sample.local_receipt_stamp_ns = state->clock.now();
  sample.cursor_s =
      static_cast<double>(nanoseconds(feedback.desired.time_from_start)) / 1e9;
  if (state->config.motion_trace && !goal.observation.latest_feedback) {
    const auto written = std::fprintf(
        stderr,
        "manyforge_goal_feedback_first goal=%.160s generation=%llu "
        "receipt_ns=%lld source_ns=%lld cursor_s=%.17g\n",
        key.controller_goal_id.value().c_str(),
        static_cast<unsigned long long>(key.controller_goal_generation.value()),
        static_cast<long long>(sample.local_receipt_stamp_ns),
        static_cast<long long>(sample.source_stamp_ns), sample.cursor_s);
    static_cast<void>(written);
  }
  goal.observation.latest_feedback = std::move(sample);
  goal.handle = handle;
  goal.observation.mechanism_goal_id = id;
  goal.observation.phase = ExecutionGoalPhase::kExecuting;
  if (state->config.motion_trace)
    state->config.motion_trace->Observe(goal.trace_token, goal.observation);
}
void Ros2ControlExecutionBackend::ReceiveResult(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::WrappedResult &result) {
  std::lock_guard lock(state->mutex);
  if (!state->ready())
    return;
  const auto found = state->goals.find(key);
  if (found == state->goals.end())
    return;
  auto &goal = found->second;
  if (goal.retired)
    return;
  const auto id = mechanism_id(result.goal_id);
  if (!goal.observation.mechanism_goal_id.empty() &&
      goal.observation.mechanism_goal_id != id)
    return;
  goal.observation.mechanism_goal_id = id;
  goal.terminal = result;
  goal.observation.terminal_local_receipt_stamp = state->clock.now();
  goal.handle.reset();
  state->classify(goal);
  if (state->config.motion_trace)
    state->config.motion_trace->Observe(goal.trace_token, goal.observation);
}

void Ros2ControlExecutionBackend::ReceiveState(
    const std::shared_ptr<State> &state,
    const control_msgs::msg::JointTrajectoryControllerState &message) {
  // Preserve callback acquisition time across state-lock contention and later
  // observer delivery. Backend initialization fences this original receipt,
  // never ROS wall time or a delayed callback's publication time.
  const auto acquired_at_ns = state->clock.now();
  std::lock_guard lock(state->mutex);
  if (!state->ready())
    return;
  const auto order =
      joint_mapping(message.joint_names, state->config.lane_facts.joint_names);
  if (!order)
    return;
  ExecutionControllerSampleDTO sample;
  if (!copy_vector(message.reference.positions, *order,
                   sample.reference_positions_rad) ||
      !copy_vector(message.reference.velocities, *order,
                   sample.reference_velocities_rad_s) ||
      !copy_vector(message.reference.accelerations, *order,
                   sample.reference_accelerations_rad_s2) ||
      !copy_vector(message.output.positions, *order,
                   sample.command_positions_rad) ||
      !copy_vector(message.feedback.positions, *order,
                   sample.measured_positions_rad) ||
      !copy_vector(message.error.positions, *order, sample.error_positions_rad))
    return;
  for (const auto &source :
       {message.output.velocities, message.feedback.velocities}) {
    std::vector<double> check;
    if (!copy_vector(source, *order, check))
      return;
  }
  if (!message.output.velocities.empty()) {
    std::vector<double> values;
    (void)copy_vector(message.output.velocities, *order, values);
    sample.command_velocities_rad_s = std::move(values);
  }
  if (!message.feedback.velocities.empty()) {
    std::vector<double> values;
    (void)copy_vector(message.feedback.velocities, *order, values);
    sample.measured_velocities_rad_s = std::move(values);
  }
  if (std::isfinite(message.speed_scaling_factor))
    sample.applied_factor = message.speed_scaling_factor;
  sample.source_stamp_ns = nanoseconds(message.header.stamp);
  sample.local_receipt_stamp_ns = acquired_at_ns;
  sample.local_receipt_count = ExecutionReceiptCount(++state->receipt_count);
  sample.binding_epoch = state->config.binding_epoch;
  sample.cursor_s =
      static_cast<double>(nanoseconds(message.reference.time_from_start)) / 1e9;
  if (state->previous_source_stamp &&
      sample.source_stamp_ns > *state->previous_source_stamp) {
    const double gap = static_cast<double>(sample.source_stamp_ns -
                                           *state->previous_source_stamp) /
                       1e9;
    state->timing.max_state_stamp_gap_s =
        std::max(state->timing.max_state_stamp_gap_s.value_or(0.0), gap);
  }
  state->previous_source_stamp = sample.source_stamp_ns;
  state->timing.received_samples =
      state->timing.received_samples.value_or(0U) + 1U;
  if (state->config.motion_trace)
    state->config.motion_trace->Sample(state->config.action_name, sample);
  state->sample = std::move(sample);
}
std::optional<ExecutionGoalObservationDTO>
Ros2ControlExecutionBackend::ReadGoal(const ExecutionGoalKey &key) const {
  std::lock_guard lock(state_->mutex);
  const auto found = state_->goals.find(key);
  if (found == state_->goals.end())
    return std::nullopt;
  return found->second.observation;
}
std::optional<ExecutionControllerSampleDTO>
Ros2ControlExecutionBackend::ReadSample() const {
  std::lock_guard lock(state_->mutex);
  return state_->sample;
}
ExecutionReadinessDTO Ros2ControlExecutionBackend::ReadReadiness() const {
  std::lock_guard lock(state_->mutex);
  return state_->readiness;
}
bool Ros2ControlExecutionBackend::ActionServerIsReady() const {
  rclcpp_action::Client<Action>::SharedPtr client;
  {
    std::lock_guard lock(state_->mutex);
    if (!state_->closed)
      client = state_->action_client.lock();
  }
  // Pin the same SDK client used by SendGoal without holding adapter facts or
  // lifecycle locks across the SDK query. No graph wait or controller command.
  const bool discovered = client && client->action_server_is_ready();
  std::lock_guard lock(state_->mutex);
  return discovered && !state_->closed && state_->action_client.lock() == client;
}
ExecutionTimingDTO Ros2ControlExecutionBackend::ReadTiming() {
  std::lock_guard lock(state_->mutex);
  auto timing = state_->timing;
  state_->timing = ExecutionTimingDTO{};
  state_->timing.binding_epoch = state_->config.binding_epoch;
  return timing;
}
ExecutionLaneFactsDTO Ros2ControlExecutionBackend::ReadLaneFacts() const {
  std::lock_guard lock(state_->mutex);
  return state_->config.lane_facts;
}

} // namespace manyforge::ros2_control_execution
