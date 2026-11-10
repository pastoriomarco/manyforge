#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#include <manyforge_ros2_control_execution/selected_ros_endpoint_facts.hpp>
#include <manyforge_ros2_control_execution/detail/leaf_timing.hpp>

#include <algorithm>
#include <array>
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
  // Stock JTC 6.10 (`tolerances.hpp` resolve_tolerance_source, control_msgs
  // JointTolerance.msg): -1 erases a tolerance (0 would keep the configured
  // default), and an erased goal tolerance is never checked. A `kKept` lane
  // sends no override and no goal time, so every goal (motion, and a Stop
  // ramp, spring-back or brake alike) gets the lane's arrival
  // tolerance and `goal_time`.
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

struct Ros2ControlExecutionBackend::CancelAttempt {
  std::weak_ptr<rclcpp_action::Client<Action>> client;
  std::weak_ptr<GoalHandle> handle;
  bool retired = false;
  bool attempted = false; // Guarded by State::mutex.
  bool acknowledged = false;
  bool responded = false;
  std::atomic<bool> cancel_admission_open{true};
  std::atomic<std::uint64_t> handle_generation{0};
  std::optional<LateCancelUnresolvedReason> unresolved;
  bool shutdown_tracked = false;
  bool shutdown_done = false;
  bool shutdown_failed = false;
  std::chrono::steady_clock::time_point shutdown_deadline{};
  std::atomic<std::int64_t> shutdown_deadline_ns{std::numeric_limits<std::int64_t>::max()};
};

struct Ros2ControlExecutionBackend::State {
  struct Goal {
    ExecutionGoalObservationDTO observation;
    VolatileMotionTrace::TokenPtr trace_token;
    GoalHandle::SharedPtr handle;
    std::shared_ptr<CancelAttempt> cancel_attempt;
    std::shared_ptr<std::atomic<bool>> send_started;
    std::optional<std::uint64_t> execution_id;
    std::optional<ExecutionGoalKey> successor;
    bool successor_accepted{false};
    std::optional<ExecutionGoalKey> predecessor;
    std::optional<GoalHandle::WrappedResult> terminal;
    bool retired{false};
    bool cancel_requested{false};
    bool cancel_sent{false};
    std::uint64_t cancel_sequence{0};
    /// When it was sent and its trajectory's time (last waypoint), for its
    /// expected end (`GoalActivity::expected_end`).
    std::optional<TimePoint> sent_stamp_ns;
    Duration trajectory_ns{0};
  };
  // Serializes readiness retirement, admission close and entity release
  // (always taken before `mutex`). Never held across an SDK call or a wait.
  std::mutex command_mutex;
  // Leaf lock: no state or authority lock is held when it is acquired; it
  // protects only the immediate scale publish.
  std::timed_mutex scale_send_mutex;
  std::optional<std::pair<std::uint64_t, double>> last_attempted_scale;
  std::timed_mutex action_send_mutex;
  std::atomic<std::uint64_t> stop_fence{0};
  std::atomic<std::uint64_t> scale_admission_generation{0};
  /// Matched subscribers of the scale publisher (the controller listening):
  /// seeded once at creation, then updated only by the publisher's matched
  /// event on the epoch owner; readers load it without a lock.
  std::atomic<std::int64_t> scale_listeners{0};
  std::atomic<bool> closed_admission{false};
  Ros2TransportResources *epoch_resources = nullptr;
  std::weak_ptr<Ros2TransportResources> epoch_owner;
  core::IScheduler &scheduler;
  std::mutex mutex;
  std::condition_variable shutdown_cv;
  std::array<GoalHandle::SharedPtr, kExecutionGoalRecordCapacity>
      retired_handles{};
  struct LossCancel {
    ExecutionGoalKey key;
    GoalHandle::SharedPtr handle;
    std::shared_ptr<CancelAttempt> attempt;
  };
  std::array<std::optional<LossCancel>, kExecutionGoalRecordCapacity>
      loss_cancels{};
  std::size_t loss_cancel_count = 0;
  std::weak_ptr<rclcpp_action::Client<Action>> action_client;
  std::weak_ptr<rclcpp::Node> node;
  std::shared_ptr<SelectedRosEndpointFacts> endpoints;
  std::shared_ptr<core::IClock> clock_owner;
  core::IClock &clock;
  JtcExecutionAdapterConfig config;
  bool closed{false};
  ExecutionReadinessDTO readiness;
  // Core owns cross-operation replay. These are only the two retained facts.
  std::map<ExecutionGoalKey, Goal> goals;
  std::deque<ExecutionGoalKey> goal_order;
  std::optional<ExecutionGoalKey> current;
  std::optional<ExecutionGoalKey> late_cancel_not_issued;
  std::optional<ExecutionControllerSampleDTO> sample;
  ExecutionTimingDTO timing;
  std::optional<TimePoint> previous_source_stamp;
  std::uint64_t receipt_count{0};

  State(std::shared_ptr<core::IClock> clock_value, core::IScheduler &scheduler_value,
        JtcExecutionAdapterConfig configuration)
      : scheduler(scheduler_value), clock_owner(std::move(clock_value)),
        clock(clock_owner ? *clock_owner : throw std::invalid_argument("JTC adapter requires an owned clock")),
        config(std::move(configuration)) {
    readiness.binding_epoch = config.binding_epoch;
    timing.binding_epoch = config.binding_epoch;
  }
  bool ready() const {
    return !closed && readiness.controller_bound &&
           readiness.controller_active && readiness.hardware_active;
  }
  bool records_full() const {
    return std::count_if(goals.begin(), goals.end(), [](const auto &entry) {
             return !evictable(entry.second);
           }) >= static_cast<std::ptrdiff_t>(kExecutionGoalRecordCapacity);
  }
  static bool evictable(const Goal &goal) {
    return goal.observation.phase == ExecutionGoalPhase::kTerminal &&
           goal.observation.result != ExecutionGoalResult::kEndedUnknown &&
           (!goal.cancel_attempt->attempted || goal.cancel_attempt->responded);
  }
  void make_record_room() {
    if (goals.size() < kExecutionGoalRecordCapacity)
      return;
    const auto victim = std::find_if(goal_order.begin(), goal_order.end(),
                                     [&](const auto &key) {
                                       return evictable(goals.at(key));
                                     });
    // Caller checks records_full before any effect or mutation.
    if (victim == goal_order.end())
      throw std::logic_error("No finished goal record to retire");
    if (current == *victim)
      current.reset();
    goals.at(*victim).cancel_attempt->cancel_admission_open.store(false, std::memory_order_release);
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

struct Ros2ControlExecutionBackend::LateCancelItem final {
  std::shared_ptr<State> state;
  std::shared_ptr<core::IClock> clock;
  rclcpp_action::Client<Action>::SharedPtr client;
  GoalHandle::SharedPtr handle;
  std::shared_ptr<CancelAttempt> attempt;
  ExecutionGoalKey key;
};

Ros2ControlExecutionBackend::Ros2ControlExecutionBackend(
    std::shared_ptr<Ros2TransportResources> resources_owner, std::shared_ptr<core::IClock> clock,
    core::IScheduler &scheduler, JtcExecutionAdapterConfig config,
    std::shared_ptr<std::atomic<bool>> state_gate)
    : state_(std::make_shared<State>(std::move(clock), scheduler, std::move(config))),
      epoch_resources_(std::move(resources_owner)) {
  auto &resources = *epoch_resources_;
  const auto &configuration = state_->config;
  const auto &joints = configuration.lane_facts.joint_names;
  if (!configuration.binding_epoch.nonzero() ||
      configuration.action_name.empty() || configuration.state_topic.empty() ||
      joints.empty() ||
      std::set(joints.begin(), joints.end()).size() != joints.size() ||
      std::any_of(joints.begin(), joints.end(),
                  [](const auto &joint) { return joint.empty(); }) ||
      configuration.lifecycle_command_timeout_ns <= 0 ||
      (configuration.lane_facts.supports_scale &&
       configuration.scale_topic.empty()))
    throw std::invalid_argument(
        "JTC adapter requires an exact binding epoch, route and joint set");
  state_->node = resources.node();
  state_->endpoints = std::make_shared<SelectedRosEndpointFacts>(
      resources.node(), configuration.state_topic);
  std::weak_ptr<State> weak = state_;
  // OD-66: controller state and the action client's goal response, feedback,
  // result and cancel responses run on the epoch's time-critical thread, one
  // group each. Their handlers take only the facts mutex and make non-waiting
  // calls (scheduler admission, retain_entity). The subscription's matched
  // event joins its group (rclcpp adds event handlers to the subscription's
  // group). Both entities live until this epoch's owner has joined.
  rclcpp::SubscriptionOptions options;
  options.callback_group = resources.create_time_critical_callback_group();
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
              [weak, state_gate](control_msgs::msg::JointTrajectoryControllerState::
                         ConstSharedPtr message) {
                if (const auto state = weak.lock())
                  ReceiveGatedState(state, state_gate, *message);
              }, options);
  if (configuration.lane_facts.supports_scale) {
    rclcpp::PublisherOptions scale_options;
    scale_options.event_callbacks.matched_callback = [weak](rclcpp::MatchedInfo &info) {
      if (const auto state = weak.lock())
        state->scale_listeners.store(info.current_count, std::memory_order_release);
    };
    scale_pub_ = resources.retain_entity(resources.node()
                     ->create_publisher<control_msgs::msg::SpeedScalingFactor>(
                         configuration.scale_topic,
                         rclcpp::QoS(1).reliable().transient_local(), scale_options));
    // Seed once; the matched event keeps it current from here on.
    state_->scale_listeners.store(
        static_cast<std::int64_t>(scale_pub_->get_subscription_count()),
        std::memory_order_release);
  }
  action_callback_group_ = resources.create_time_critical_callback_group();
  action_client_ = resources.retain_entity(
      rclcpp_action::create_client<Action>(resources.node(), configuration.action_name,
                                           action_callback_group_),
      action_callback_group_);
  state_->action_client = action_client_;
  state_->epoch_resources = epoch_resources_.get();
  state_->epoch_owner = epoch_resources_;
}

Ros2ControlExecutionBackend::~Ros2ControlExecutionBackend() { shutdown(); }

void Ros2ControlExecutionBackend::RefreshEndpointFacts() {
  // A thread-safe graph read under the facts' own mutex: called directly,
  // never dispatched to the epoch owner (the owner is not a work queue, and a
  // caller on another owner's timer must not wait on this one).
  state_->endpoints->Refresh();
}

std::optional<core::ErrorDTO>
Ros2ControlExecutionBackend::ReadEndpointDiagnostic() const {
  return state_->endpoints->ReadError();
}

void Ros2ControlExecutionBackend::ObserveReadinessImpl(
    ExecutionReadinessDTO readiness) {
  std::array<GoalHandle::SharedPtr, kExecutionGoalRecordCapacity>
      retired_handles{};
  std::size_t retired_count = 0;
  bool loss_pending = false;
  {
    std::lock_guard command_lock(state_->command_mutex);
    std::lock_guard facts_lock(state_->mutex);
    if (state_->closed ||
        readiness.binding_epoch != state_->config.binding_epoch)
      return;
    state_->readiness = readiness;
    if (!state_->ready()) {
      state_->stop_fence.fetch_add(1, std::memory_order_acq_rel);
      state_->scale_admission_generation.fetch_add(1,
                                                   std::memory_order_acq_rel);
      for (auto &[key, goal] : state_->goals) {
        const bool unresolved =
            goal.observation.phase != ExecutionGoalPhase::kTerminal ||
            goal.observation.result == ExecutionGoalResult::kEndedUnknown;
        (void)key;
        if (goal.observation.phase != ExecutionGoalPhase::kTerminal) {
          goal.observation.phase = ExecutionGoalPhase::kTerminal;
          goal.observation.result = ExecutionGoalResult::kEndedUnknown;
        }
        goal.retired = true;
        goal.cancel_attempt->retired = true;
        if (unresolved && goal.handle && !goal.cancel_attempt->attempted) {
          if (state_->loss_cancel_count < state_->loss_cancels.size()) {
            state_->loss_cancels[state_->loss_cancel_count++] =
                State::LossCancel{key, goal.handle, goal.cancel_attempt};
          } else {
            goal.cancel_attempt->attempted = true;
            std::fprintf(
                stderr,
                "manyforge_loss_cancel status=skipped reason=overflow\n");
          }
        }
        if (goal.handle)
          retired_handles[retired_count++] = std::move(goal.handle);
      }
      state_->sample.reset();
      loss_pending = state_->loss_cancel_count != 0;
    }
  }
  // Loss cancels are scheduled directly from this thread, after every lock is
  // released: no hop through the epoch owner. A closed owner skips them, as
  // before.
  if (loss_pending && !state_->epoch_resources->dispatch_closed()) {
    DrainLossCancels(state_);
  } else if (loss_pending) {
    decltype(state_->loss_cancels) missed;
    {
      std::lock_guard facts_lock(state_->mutex);
      missed.swap(state_->loss_cancels);
      state_->loss_cancel_count = 0;
      for (auto &entry : missed) {
        if (!entry)
          continue;
        entry->attempt->attempted = true;
      }
    }
    for (const auto &entry : missed)
      if (entry)
        std::fprintf(stderr, "manyforge_loss_cancel status=skipped reason=owner_closed\n");
  }
}
bool Ros2ControlExecutionBackend::shutdown() noexcept {
  return shutdown(std::chrono::steady_clock::now() +
                  std::chrono::nanoseconds(state_->config.lifecycle_command_timeout_ns));
}
bool Ros2ControlExecutionBackend::shutdown(
    std::chrono::steady_clock::time_point deadline) noexcept {
  if (!close_admission(deadline)) return false;
  // Waiting for an unanswered cancel may use the whole deadline; the owner
  // join then gets its own authored bound instead of none, so a pending
  // cancel (now marked unknown) cannot by itself make shutdown unresolved.
  const auto join_deadline = std::max(
      deadline, std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(state_->config.lifecycle_command_timeout_ns));
  if (!epoch_resources_->quiesce_until(join_deadline)) return false;
  release_ros_entities();
  return true;
}

bool Ros2ControlExecutionBackend::close_admission() noexcept {
  return close_admission(std::chrono::steady_clock::now() +
                         std::chrono::nanoseconds(state_->config.lifecycle_command_timeout_ns));
}
bool Ros2ControlExecutionBackend::close_admission(
    std::chrono::steady_clock::time_point deadline) noexcept {
  close_admission_impl(true, deadline);
  // The wait below is for cancel responses only this epoch's executor
  // threads deliver. A callback never waits (and on the time-critical thread,
  // which delivers them, it could never be satisfied): admission is closed
  // (above), the wait is refused and the shutdown stays unresolved.
  if (epoch_resources_->on_owner_thread() || epoch_resources_->on_time_critical_thread()) {
    std::fprintf(stderr, "manyforge_execution_close status=refused reason=on_owner owner=%s\n",
                 epoch_resources_->owner_name().c_str());
    return false;
  }
  std::unique_lock lock(state_->mutex);
  const auto settled = [&] {
    return std::none_of(state_->goals.begin(), state_->goals.end(), [](const auto &row) {
      return row.second.cancel_attempt->shutdown_tracked &&
             !row.second.cancel_attempt->shutdown_done;
    });
  };
  const bool all_settled = state_->shutdown_cv.wait_until(lock, deadline, settled);
  lock.unlock();
  bool leaf_acquired = false;
  {
    MANYFORGE_LEAF_DEFERRED_LOCK(leaf, state_->action_send_mutex,
                                detail::LeafEntity::kMotionStopBarrier);
    leaf_acquired = leaf.try_lock_until(deadline);
  }
  if (!all_settled) {
    lock.lock();
    for (auto &[key, goal] : state_->goals)
      if (goal.cancel_attempt->shutdown_tracked && !goal.cancel_attempt->shutdown_done) {
        goal.cancel_attempt->shutdown_failed = true;
        goal.cancel_attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
        state_->late_cancel_not_issued = key;
      }
  }
  return leaf_acquired;
}

void Ros2ControlExecutionBackend::close_admission_without_cancel() noexcept {
  close_admission_impl(false, std::chrono::steady_clock::now());
}

void Ros2ControlExecutionBackend::close_admission_impl(
    const bool send_loss_cancels,
    std::chrono::steady_clock::time_point deadline) noexcept {
  state_->closed_admission.store(true, std::memory_order_release);
  {
    std::lock_guard command_lock(state_->command_mutex);
    std::lock_guard facts_lock(state_->mutex);
    if (state_->closed)
      return;
    state_->closed = true;
    state_->stop_fence.fetch_add(1, std::memory_order_acq_rel);
    state_->scale_admission_generation.fetch_add(1, std::memory_order_acq_rel);
    state_->readiness.controller_active = state_->readiness.controller_bound =
        state_->readiness.hardware_active = false;
    std::size_t retired_count = 0;
    for (auto &[key, goal] : state_->goals) {
            const bool unresolved_sent =
                (goal.observation.phase != ExecutionGoalPhase::kTerminal ||
                 goal.observation.result == ExecutionGoalResult::kEndedUnknown) &&
                goal.send_started && goal.send_started->load(std::memory_order_acquire);
            if (!send_loss_cancels && unresolved_sent && !goal.cancel_attempt->acknowledged) {
              goal.cancel_attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
              state_->late_cancel_not_issued = key;
            }
      if (send_loss_cancels &&
          (goal.observation.phase != ExecutionGoalPhase::kTerminal ||
           goal.observation.result == ExecutionGoalResult::kEndedUnknown) &&
          goal.send_started &&
          goal.send_started->load(std::memory_order_acquire)) {
        goal.cancel_attempt->shutdown_tracked = true;
        goal.cancel_attempt->shutdown_deadline = deadline;
        goal.cancel_attempt->shutdown_deadline_ns.store(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                deadline.time_since_epoch())
                .count(),
            std::memory_order_release);
        // A prior exact ACK already satisfies this goal's shutdown
        // wait; no second cancel or fabricated new receipt is needed.
        goal.cancel_attempt->shutdown_done =
            goal.cancel_attempt->acknowledged ||
            goal.observation.cancel_ack_local_receipt_stamp.has_value();
      }
      if (goal.observation.phase != ExecutionGoalPhase::kTerminal) {
        goal.observation.phase = ExecutionGoalPhase::kTerminal;
        goal.observation.result = ExecutionGoalResult::kEndedUnknown;
      }
      goal.cancel_attempt->retired = true;
      if (goal.cancel_attempt->shutdown_tracked && goal.handle &&
          !goal.cancel_attempt->attempted) {
        if (state_->loss_cancel_count < state_->loss_cancels.size())
          state_->loss_cancels[state_->loss_cancel_count++] =
              State::LossCancel{key, goal.handle, goal.cancel_attempt};
        else {
          goal.cancel_attempt->attempted = true;
          std::fprintf(
              stderr, "manyforge_loss_cancel status=skipped reason=overflow\n");
        }
      }
      if (goal.handle)
        state_->retired_handles[retired_count++] = std::move(goal.handle);
    }
    state_->sample.reset();
  }
  if (send_loss_cancels)
    DrainLossCancels(state_);
  else {
    decltype(state_->loss_cancels) missed;
    {
      std::lock_guard facts_lock(state_->mutex);
      missed.swap(state_->loss_cancels);
      state_->loss_cancel_count = 0;
      for (auto &entry : missed)
        if (entry)
          entry->attempt->attempted = true;
    }
    for (const auto &entry : missed)
      if (entry)
        std::fprintf(
            stderr,
            "manyforge_loss_cancel status=skipped reason=owner_closed\n");
  }
  std::lock_guard facts_lock(state_->mutex);
  state_->action_client.reset();
}

void Ros2ControlExecutionBackend::release_ros_entities() noexcept {
  // The host has already joined this epoch's executor. Wait for an earlier
  // release without holding a lock over SDK destruction.
  {
    std::unique_lock lock(release_mutex_);
    release_cv_.wait(lock, [this] { return !releasing_; });
    if (released_)
      return;
    releasing_ = true;
  }
  decltype(action_client_) client;
  decltype(state_sub_) subscription;
  decltype(scale_pub_) publisher;
  std::array<GoalHandle::SharedPtr, kExecutionGoalRecordCapacity> handles{};
  {
    std::lock_guard command_lock(state_->command_mutex);
    std::lock_guard facts_lock(state_->mutex);
    handles = std::move(state_->retired_handles);
    client = std::move(action_client_);
    subscription = std::move(state_sub_);
    publisher = std::move(scale_pub_);
  }
  if (const auto node = state_->node.lock(); node && client)
    node->get_node_waitables_interface()->remove_waitable(client, action_callback_group_);
  for (auto &handle : handles)
    handle.reset();
  subscription.reset();
  publisher.reset();
  client.reset();
  {
    std::lock_guard lock(release_mutex_);
    released_ = true;
    releasing_ = false;
  }
  release_cv_.notify_all();
}

void Ros2ControlExecutionBackend::ObserveReadiness(ExecutionReadinessDTO readiness) {
  ObserveReadinessImpl(readiness);
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::Submit(
    const ExecutionGoalKey &key, const TrajectoryDTO &trajectory,
    std::shared_ptr<ExecutionDispatchToken> token) {
  return SubmitImpl(key, trajectory, std::move(token));
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::Replace(
    const ExecutionGoalKey &active, const ExecutionGoalKey &next,
    const TrajectoryDTO &trajectory, std::shared_ptr<ExecutionDispatchToken> token) {
  return ReplaceImpl(active, next, trajectory, std::move(token));
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::ReplaceScheduled(
    const ExecutionGoalKey &active, const ExecutionGoalKey &next,
    const TrajectoryDTO &trajectory, const ExecutionReplacementStartDTO &start,
    std::shared_ptr<ExecutionDispatchToken> token) {
  return ReplaceScheduledImpl(active, next, trajectory, start, std::move(token));
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::Cancel(
    const ExecutionGoalKey &key, std::chrono::steady_clock::time_point deadline) {
  return CancelImpl(key, deadline);
}
std::optional<bool> Ros2ControlExecutionBackend::ScaleInputListening() const noexcept {
  if (!state_->config.lane_facts.supports_scale) return std::nullopt;
  return state_->scale_listeners.load(std::memory_order_acquire) > 0;
}

ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::SetScale(double value, std::uint64_t sequence) {
  return SetScaleImpl(value, sequence);
}
core::ScaleSnapshot Ros2ControlExecutionBackend::SnapshotScaleAfterFence(
    std::chrono::steady_clock::time_point deadline) {
  MANYFORGE_LEAF_DEFERRED_LOCK(leaf, state_->scale_send_mutex,
                              detail::LeafEntity::kMotionScaleSnapshot);
  if (!leaf.try_lock_until(deadline))
    return {core::ScaleSnapshot::Status::kDeadlineExpired, 0.0};
  if (!state_->last_attempted_scale)
    return {core::ScaleSnapshot::Status::kNoPriorFactor, 0.0};
  return {core::ScaleSnapshot::Status::kFactor,
          state_->last_attempted_scale->second};
}

ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::SubmitImpl(const ExecutionGoalKey &key,
                                    const TrajectoryDTO &trajectory,
                                    std::shared_ptr<ExecutionDispatchToken> token) {
  VolatileMotionTrace::Garbage garbage;
  return SendGoal(key, trajectory, std::nullopt, garbage, std::nullopt, std::move(token));
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::SendGoal(
    const ExecutionGoalKey &key, const TrajectoryDTO &trajectory,
    std::optional<ExecutionGoalKey> active_key,
    VolatileMotionTrace::Garbage &trace_garbage,
    std::optional<ExecutionReplacementStartDTO> start,
    std::shared_ptr<ExecutionDispatchToken> token) {
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
  rclcpp_action::Client<Action>::SharedPtr client;
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
    client = state_->action_client.lock();
  }
  if (!client)
    return refused(ExecutionPortRefusal::kNotReady,
                   "FJT action server is not discovered");
  const auto *const experiment =
      start && start->experiment ? &*start->experiment : nullptr;
  VolatileMotionTrace::TokenPtr trace_token;
  std::shared_ptr<CancelAttempt> cancel_attempt;
  auto send_started = std::make_shared<std::atomic<bool>>(false);
  std::uint64_t reserved_fence;
  {
    std::lock_guard facts_lock(state_->mutex);
    if (!state_->ready() || state_->goals.contains(key) || state_->records_full() ||
        state_->action_client.lock() != client)
      return refused(ExecutionPortRefusal::kNotReady,
                     "Binding or goal reservation changed before send");
    if (active_key && (!state_->current || *state_->current != *active_key ||
                       state_->goals.at(*active_key).observation.phase ==
                           ExecutionGoalPhase::kTerminal))
      return refused(ExecutionPortRefusal::kStaleKey,
                     "Replacement goal is no longer current");
    state_->make_record_room();
    State::Goal record;
    cancel_attempt = std::make_shared<CancelAttempt>();
    cancel_attempt->client = state_->action_client;
    record.cancel_attempt = cancel_attempt;
    record.send_started = send_started;
    record.execution_id = token ? token->execution_id() : std::nullopt;
    reserved_fence = state_->stop_fence.load(std::memory_order_acquire);
    record.observation.key = key;
    record.observation.binding_epoch = state_->config.binding_epoch;
    record.trajectory_ns = static_cast<Duration>(
        std::max(0.0, trajectory.waypoints.back().time_from_start_s) * 1e9);
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
  options.goal_response_callback =
      [weak, key, cancel_attempt](GoalHandle::SharedPtr handle) {
        if (const auto state = weak.lock())
          ReceiveResponse(state, key, handle, cancel_attempt);
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
  const auto retire_unsent = [&] {
    std::lock_guard facts_lock(state_->mutex);
    const auto found = state_->goals.find(key);
    if (found == state_->goals.end() || found->second.send_started != send_started ||
        send_started->load(std::memory_order_acquire))
      return;
    const auto predecessor = found->second.predecessor;
    const auto successor = found->second.successor;
    if (predecessor) {
      const auto previous = state_->goals.find(*predecessor);
      if (previous != state_->goals.end() && previous->second.successor == key) {
        previous->second.successor = successor;
        const auto next = successor ? state_->goals.find(*successor) : state_->goals.end();
        previous->second.successor_accepted =
            next != state_->goals.end() && next->second.observation.accepted_stamp_ns.has_value();
      }
    }
    if (successor) {
      const auto next = state_->goals.find(*successor);
      if (next != state_->goals.end() && next->second.predecessor == key)
        next->second.predecessor = predecessor;
    }
    if (state_->current == key) {
      const auto previous = predecessor ? state_->goals.find(*predecessor) : state_->goals.end();
      state_->current = previous != state_->goals.end() &&
                                previous->second.observation.phase != ExecutionGoalPhase::kTerminal
                            ? predecessor : std::nullopt;
    }
    state_->goals.erase(found);
    std::erase(state_->goal_order, key);
  };
  bool server_ready;
  try {
    server_ready = client->action_server_is_ready();
  } catch (...) {
    retire_unsent();
    throw;
  }
  if (!server_ready) {
    retire_unsent();
    return refused(ExecutionPortRefusal::kNotReady,
                   "FJT action server is not discovered");
  }
  const auto send_ns = experiment ? state_->clock.now() : TimePoint{0};
  bool fenced = false;
  try {
    {
      MANYFORGE_LEAF_LOCK(leaf, state_->action_send_mutex, detail::LeafEntity::kMotionGoal);
      fenced = state_->closed_admission.load(std::memory_order_acquire) ||
               state_->stop_fence.load(std::memory_order_acquire) != reserved_fence ||
               epoch_resources_->executor_failure_started();
      if (!fenced && token && !token->TryAdmit())
        fenced = true;
      if (fenced && token)
        (void)token->Fence();
      if (!fenced) {
        // An SDK exception after this point is effect-uncertain. Stop sees
        // this exact reservation as sent/handle-pending after the leaf.
        send_started->store(true, std::memory_order_release);
        MANYFORGE_GOAL_ADMITTED_TEST_POINT();
        (void)client->async_send_goal(goal, options);
      }
    }
  } catch (const std::exception &error) {
    if (state_->config.motion_trace) {
      try { state_->config.motion_trace->Attach(trace_token, std::move(goal), state_->clock.now(),
              ExecutionPortDispatch::kEffectUncertain, trace_garbage,
              active_key.has_value(), start); } catch (...) {}
    }
    return uncertain(error.what());
  }
  if (fenced) {
    retire_unsent();
    return refused(ExecutionPortRefusal::kStaleKey,
                   "Goal reservation was fenced before send");
  }
  const auto sent_stamp = state_->clock.now();
  {
    std::lock_guard facts_lock(state_->mutex);
    const auto found = state_->goals.find(key);
    if (found != state_->goals.end() && found->second.send_started == send_started)
      found->second.sent_stamp_ns = sent_stamp;
  }
  if (experiment) {
    // EXPERIMENTAL diagnostics for the live harness, one line per replacement:
    // the wire policy actually sent (each control labelled on its own; more
    // than one active is a combined producer policy) and what arrival and
    // install are computed from, with the controller_state stamps.
    const auto hold = [&] {
      switch (experiment->release_hold) {
      case ExecutionReleaseHold::kHeld: return "held";
      case ExecutionReleaseHold::kMissed: return "missed";
      case ExecutionReleaseHold::kDeadline: return "deadline";
      case ExecutionReleaseHold::kNone: break;
      }
      return "none";
    }();
    const bool timer = experiment->release_local_ns.has_value();
    const int controls = (timer ? 1 : 0) + (experiment->midtick_stamp ? 1 : 0) +
                         (experiment->v0_payload ? 1 : 0) +
                         (experiment->spring_back_fast ? 1 : 0) +
                         (experiment->start_allowance_override ? 1 : 0);
    const auto written = std::fprintf(
        stderr,
        "manyforge_replacement_experiment goal=%.160s generation=%llu "
        "producer_policy=%s release_policy=%s stamp_policy=%s payload_policy=%s "
        "release_hold=%s source_stamp_ns=%lld target_tick_ns=%lld stamp_ns=%lld "
        "planned_release_ns=%lld send_ns=%lld returned_ns=%lld "
        "expected_latency_ns=%lld start_allowance_ns=%lld allowance_override=%d factor=%.17g\n",
        key.controller_goal_id.value().c_str(),
        static_cast<unsigned long long>(key.controller_goal_generation.value()),
        controls > 1 ? "combined" : "single", timer ? "timer" : "immediate",
        experiment->midtick_stamp ? "midtick" : "grid",
        experiment->spring_back_fast ? "spring_back_fast_v0"
        : experiment->v0_payload     ? "v0"
                                     : "v6_prefix",
        hold, static_cast<long long>(experiment->source_stamp_ns),
        static_cast<long long>(experiment->target_tick_ns),
        static_cast<long long>(start->controller_start_stamp_ns),
        static_cast<long long>(experiment->release_local_ns.value_or(0)),
        static_cast<long long>(send_ns), static_cast<long long>(state_->clock.now()),
        static_cast<long long>(experiment->expected_latency_ns),
        static_cast<long long>(experiment->start_allowance_ns),
        experiment->start_allowance_override ? 1 : 0, experiment->factor);
    static_cast<void>(written);
  }
  if (state_->config.motion_trace) {
    try { state_->config.motion_trace->Attach(trace_token, std::move(goal), state_->clock.now(),
            ExecutionPortDispatch::kDispatched, trace_garbage,
            active_key.has_value(), start); } catch (...) {}
  }
  return sent();
}
ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::ReplaceImpl(const ExecutionGoalKey &active_key,
                                     const ExecutionGoalKey &next_key,
                                     const TrajectoryDTO &trajectory,
                                     std::shared_ptr<ExecutionDispatchToken> token) {
  // Declared before the guard: retained payload garbage is released only
  // after the outer command mutex unlocks, including early returns.
  VolatileMotionTrace::Garbage garbage;
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
  return SendGoal(next_key, trajectory, active_key, garbage, std::nullopt, std::move(token));
}
ExecutionPortCommandResultDTO Ros2ControlExecutionBackend::ReplaceScheduledImpl(
    const ExecutionGoalKey &active_key, const ExecutionGoalKey &next_key,
    const TrajectoryDTO &trajectory,
    const ExecutionReplacementStartDTO &start,
    std::shared_ptr<ExecutionDispatchToken> token) {
  VolatileMotionTrace::Garbage garbage;
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
  return SendGoal(next_key, trajectory, active_key, garbage, start, std::move(token));
}
ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::CancelImpl(
    const ExecutionGoalKey &key, std::chrono::steady_clock::time_point deadline) {
  GoalHandle::SharedPtr handle;
  std::shared_ptr<CancelAttempt> attempt;
  {
    std::lock_guard facts_lock(state_->mutex);
    auto cursor = state_->current;
    while (cursor && *cursor != key) {
      const auto found = state_->goals.find(*cursor);
      cursor = found == state_->goals.end() ? std::nullopt : found->second.predecessor;
    }
    if (!cursor)
      return refused(ExecutionPortRefusal::kStaleKey,
                     "Cancel key is not current");
    auto &goal = state_->goals.at(key);
    if (goal.observation.phase == ExecutionGoalPhase::kTerminal)
      return refused(ExecutionPortRefusal::kStaleKey, "Goal already ended");
    if (!state_->ready())
      return refused(ExecutionPortRefusal::kNotReady,
                     "Binding is not active and bound");
    state_->stop_fence.fetch_add(1, std::memory_order_acq_rel);
    // Core owns the newest-first chain walk and the shared cancel slot for
    // each exact key. This port must never fan one request out to other keys.
    goal.cancel_requested = true;
    attempt = goal.cancel_attempt;
  }
  // Observe any send that entered before the fence before returning a
  // handle-pending cancellation; later sends cannot pass that fence.
  {
    MANYFORGE_LEAF_DEFERRED_LOCK(leaf, state_->action_send_mutex,
                                detail::LeafEntity::kMotionStopBarrier);
    if (!leaf.try_lock_until(deadline))
      return uncertain("Cancel deadline expired waiting for the action client");
  }
  {
    std::lock_guard facts_lock(state_->mutex);
    const auto found = state_->goals.find(key);
    if (found == state_->goals.end() || found->second.cancel_attempt != attempt ||
        found->second.retired || found->second.observation.phase == ExecutionGoalPhase::kTerminal)
      return uncertain("Goal changed before exact cancellation was sent");
    auto &goal = found->second;
    if (!goal.handle)
      return uncertain("Cancel intent retained until goal acceptance; not sent yet");
    // One cancel per exact goal is in flight at a time: a repeat joins it
    // without a second request. After its response a new cancel may be sent
    // (for example, re-cancelling after a rejection).
    if (attempt->attempted && !attempt->responded) return sent();
    goal.cancel_sent = true;
    attempt->attempted = true;
    attempt->responded = false;
    handle = goal.handle;
  }
  return SendCancel(state_, key, handle, deadline);
}
ExecutionPortCommandResultDTO
Ros2ControlExecutionBackend::SendCancel(const std::shared_ptr<State> &state,
                                        const ExecutionGoalKey &key,
                                        const GoalHandle::SharedPtr &handle,
                                        std::chrono::steady_clock::time_point deadline) {
  rclcpp_action::Client<Action>::SharedPtr client;
  std::shared_ptr<CancelAttempt> attempt;
  std::uint64_t cancel_sequence = 0;
  std::uint64_t handle_generation = 0;
  bool enqueue_retired = false;
  // No request left this port: close this exact attempt (state->mutex held),
  // as the deadline path does, so the record stays evictable and a later
  // cancel or the late-cancel path can still send it.
  const auto close_undispatched = [&] {
    const auto found = state->goals.find(key);
    if (found == state->goals.end() || found->second.cancel_attempt != attempt ||
        found->second.cancel_sequence != cancel_sequence)
      return;
    found->second.cancel_sent = false;
    attempt->attempted = false;
  };
  {
    std::lock_guard facts_lock(state->mutex);
    const auto found = state->goals.find(key);
    if (found == state->goals.end())
      return refused(ExecutionPortRefusal::kNotReady,
                     "Cancel binding retired before send");
    attempt = found->second.cancel_attempt;
    if (!state->ready() || found->second.retired) {
      if (attempt->retired && (!state->closed || attempt->shutdown_tracked))
        enqueue_retired = true;
      else
        return refused(ExecutionPortRefusal::kNotReady,
                       "Cancel binding retired before send");
    }
    if (!enqueue_retired) {
    client = state->action_client.lock();
    if (!client)
      return uncertain("Cancel transport no longer exists");
    auto &goal = found->second;
    handle_generation = attempt->handle_generation.load(std::memory_order_acquire);
    cancel_sequence = ++goal.cancel_sequence;
    goal.observation.cancel_sent_local_stamp.reset();
    goal.observation.cancel_ack_local_receipt_stamp.reset();
    }
  }
  if (enqueue_retired) {
    QueueAutomaticCancel(state, key, handle, attempt);
    return uncertain("Exact cancel retained after readiness retirement");
  }
  std::weak_ptr<State> weak = state;
  bool owner_failed = false;
  bool deadline_expired = false;
  bool goal_terminal = false;
  bool stale_handle = false;
  try {
    {
      MANYFORGE_LEAF_DEFERRED_LOCK(leaf, state->action_send_mutex,
                                  detail::LeafEntity::kMotionCancel);
      deadline_expired = !leaf.try_lock_until(deadline);
      const auto shutdown_ns = attempt->shutdown_deadline_ns.load(std::memory_order_acquire);
      if (!deadline_expired && shutdown_ns != std::numeric_limits<std::int64_t>::max())
        deadline_expired = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() >= shutdown_ns;
      if (!deadline_expired)
        owner_failed = state->epoch_resources->executor_failure_started();
      if (!deadline_expired && !owner_failed) {
        goal_terminal = !attempt->cancel_admission_open.load(std::memory_order_acquire);
        stale_handle = attempt->handle_generation.load(std::memory_order_acquire) != handle_generation;
      }
      if (!deadline_expired && !owner_failed && !goal_terminal && !stale_handle)
        (void)client->async_cancel_goal(
            handle,
            [weak, key, cancel_sequence](
                action_msgs::srv::CancelGoal::Response::SharedPtr response) {
              if (const auto locked = weak.lock())
                ReceiveCancelResponse(locked, key, cancel_sequence, response);
            });
    }
  } catch (const std::exception &error) {
    // SDK send threw: no definite dispatched receipt is manufactured.
    std::lock_guard facts_lock(state->mutex);
    close_undispatched();
    return uncertain(error.what());
  }
  if (deadline_expired) {
    bool enqueue_loss = false;
    {
      std::lock_guard facts_lock(state->mutex);
      const auto found = state->goals.find(key);
      if (found != state->goals.end() && found->second.cancel_attempt == attempt &&
          found->second.cancel_sequence == cancel_sequence) {
        found->second.cancel_sent = false;
        attempt->attempted = false;
        if (!attempt->shutdown_tracked ||
            std::chrono::steady_clock::now() < attempt->shutdown_deadline) {
          attempt->attempted = true;
          enqueue_loss = true;
        } else {
          attempt->shutdown_done = true;
          attempt->shutdown_failed = true;
          attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
          state->late_cancel_not_issued = key;
          state->shutdown_cv.notify_all();
        }
      }
    }
    if (enqueue_loss) QueueAutomaticCancel(state, key, handle, attempt);
    return uncertain("Cancel deadline expired waiting for the action client");
  }
  if (owner_failed) {
    std::lock_guard lock(state->mutex);
    close_undispatched();
    attempt->unresolved = LateCancelUnresolvedReason::kNotIssued;
    state->late_cancel_not_issued = key;
    if (attempt->shutdown_tracked) {
      attempt->shutdown_done = true;
      attempt->shutdown_failed = true;
      state->shutdown_cv.notify_all();
    }
    return refused(ExecutionPortRefusal::kNotReady, "Cancel owner has failed");
  }
  if (stale_handle) {
    std::lock_guard lock(state->mutex);
    close_undispatched();
    attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
    state->late_cancel_not_issued = key;
    if (attempt->shutdown_tracked) {
      attempt->shutdown_done = true;
      attempt->shutdown_failed = true;
      state->shutdown_cv.notify_all();
    }
    return uncertain("Exact cancel handle changed before admission");
  }
  if (goal_terminal) {
    std::lock_guard lock(state->mutex);
    attempt->responded = true; // Proven terminal closes this cancel obligation.
    if (attempt->shutdown_tracked) {
      attempt->shutdown_done = true;
      state->shutdown_cv.notify_all();
    }
    return refused(ExecutionPortRefusal::kStaleKey, "Goal ended before cancel admission");
  }
  // Conservative certification boundary: only samples after the SDK call
  // returned and the leaf was released can prove post-cancel state.
  const auto send_boundary = state->clock.now();
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
  if (found != state->goals.end() &&
      found->second.cancel_sequence == cancel_sequence && response)
    found->second.cancel_attempt->responded = true;
  const bool exact_ack = found != state->goals.end() &&
      found->second.cancel_sequence == cancel_sequence && response &&
      response->return_code == action_msgs::srv::CancelGoal::Response::ERROR_NONE &&
      response->goals_canceling.size() == 1 &&
      !found->second.observation.mechanism_goal_id.empty() &&
      mechanism_id(response->goals_canceling.front().goal_id.uuid) ==
          found->second.observation.mechanism_goal_id;
  if (exact_ack)
    found->second.cancel_attempt->acknowledged = true;
  if (found != state->goals.end() &&
      found->second.cancel_sequence == cancel_sequence &&
      found->second.cancel_attempt->shutdown_tracked) {
    found->second.cancel_attempt->shutdown_done = true;
    found->second.cancel_attempt->shutdown_failed = !exact_ack;
    if (!exact_ack) {
      found->second.cancel_attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
      state->late_cancel_not_issued = key;
    }
    state->shutdown_cv.notify_all();
  }
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
Ros2ControlExecutionBackend::SetScaleImpl(double value, std::uint64_t sequence) {
  decltype(scale_pub_) publisher;
  std::uint64_t admission_generation;
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
    publisher = scale_pub_;
    if (!publisher)
      return refused(ExecutionPortRefusal::kNotReady,
                     "Scale publisher is unavailable");
    admission_generation = state_->scale_admission_generation.load(std::memory_order_acquire);
  }
  control_msgs::msg::SpeedScalingFactor message;
  message.factor = value;
  bool stale = false;
  try {
    {
      MANYFORGE_LEAF_LOCK(leaf, state_->scale_send_mutex, detail::LeafEntity::kMotionScalePublish);
      stale = !IsLatestScaleSequence(sequence) ||
              state_->closed_admission.load(std::memory_order_acquire) ||
              state_->scale_admission_generation.load(std::memory_order_acquire) != admission_generation ||
              epoch_resources_->executor_failure_started();
      if (!stale) {
        try {
          publisher->publish(message);
        } catch (...) {
          // SDK entry with an exception is effect-uncertain, and Core's
          // existing freeze policy retains the attempted factor.
          state_->last_attempted_scale = std::pair{sequence, value};
          throw;
        }
        state_->last_attempted_scale = std::pair{sequence, value};
      }
    }
  } catch (const std::exception &error) {
    return uncertain(error.what());
  }
  if (stale)
    return refused(ExecutionPortRefusal::kStaleKey,
                   "Scale command was superseded or its owner retired");
  if (state_->config.motion_trace) {
    try { state_->config.motion_trace->PublishedScale(state_->config.action_name,
        state_->config.binding_epoch, state_->clock.now(), value); } catch (...) {}
  }
  return sent(); // Published is not applied; only ReceiveState updates that
                 // fact.
}

void Ros2ControlExecutionBackend::SendAutomaticCancel(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::SharedPtr &handle,
    const std::shared_ptr<CancelAttempt> &attempt,
    const rclcpp_action::Client<Action>::SharedPtr &client) noexcept {
  std::uint64_t sequence = 0;
  std::uint64_t handle_generation = 0;
  std::optional<std::chrono::steady_clock::time_point> deadline;
  {
    std::lock_guard lock(state->mutex);
    if (attempt->shutdown_tracked)
      deadline = attempt->shutdown_deadline;
    const auto found = state->goals.find(key);
    if (found == state->goals.end() || found->second.cancel_attempt != attempt) {
      attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
      state->late_cancel_not_issued = key;
      if (attempt->shutdown_tracked) {
        attempt->shutdown_done = true;
        attempt->shutdown_failed = true;
        state->shutdown_cv.notify_all();
      }
      return;
    }
    if (found->second.observation.phase == ExecutionGoalPhase::kTerminal &&
        found->second.observation.result != ExecutionGoalResult::kEndedUnknown) {
      attempt->responded = true;
      if (attempt->shutdown_tracked) {
        attempt->shutdown_done = true;
        state->shutdown_cv.notify_all();
      }
      return;
    }
    {
      const auto selected = attempt->handle.lock();
      if (!selected || selected != handle) {
        attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
        state->late_cancel_not_issued = key;
        if (attempt->shutdown_tracked) {
          attempt->shutdown_done = true;
          attempt->shutdown_failed = true;
          state->shutdown_cv.notify_all();
        }
        return;
      }
      handle_generation = attempt->handle_generation.load(std::memory_order_acquire);
      sequence = ++found->second.cancel_sequence;
      found->second.observation.cancel_sent_local_stamp.reset();
      found->second.observation.cancel_ack_local_receipt_stamp.reset();
    }
  }
  try {
    std::weak_ptr<State> weak = state;
    bool owner_failed = false;
    bool expired = false;
    bool terminal = false;
    bool stale_handle = false;
    {
      MANYFORGE_LEAF_DEFERRED_LOCK(leaf, state->action_send_mutex,
                                  detail::LeafEntity::kMotionAutomaticCancel);
      if (deadline)
        expired = !leaf.try_lock_until(*deadline) ||
                  std::chrono::steady_clock::now() >= *deadline;
      else
        leaf.lock();
      // Close may have fenced this attempt while the worker was already
      // waiting without a shutdown deadline. Recheck its published deadline
      // under the leaf before any SDK entry.
      const auto shutdown_ns = attempt->shutdown_deadline_ns.load(std::memory_order_acquire);
      if (!expired && shutdown_ns != std::numeric_limits<std::int64_t>::max())
        expired = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count() >= shutdown_ns;
      if (!expired)
        owner_failed = state->epoch_resources->executor_failure_started();
      if (!expired && !owner_failed) {
        terminal = !attempt->cancel_admission_open.load(std::memory_order_acquire);
        stale_handle = attempt->handle_generation.load(std::memory_order_acquire) != handle_generation;
      }
      if (!owner_failed && !expired && !terminal && !stale_handle)
        (void)client->async_cancel_goal(
            handle,
            [weak, key,
             sequence](action_msgs::srv::CancelGoal::Response::SharedPtr response) {
              if (sequence != 0)
                if (const auto locked = weak.lock())
                  ReceiveCancelResponse(locked, key, sequence, response);
            });
    }
    if (terminal && !stale_handle) {
      std::lock_guard lock(state->mutex);
      attempt->responded = true; // Proven terminal closes the cancel obligation.
      if (attempt->shutdown_tracked) {
        attempt->shutdown_done = true;
        state->shutdown_cv.notify_all();
      }
      return;
    }
    if (owner_failed || expired || stale_handle) {
      std::lock_guard lock(state->mutex);
      attempt->unresolved = (expired || stale_handle) ? LateCancelUnresolvedReason::kUnknown
                                                       : LateCancelUnresolvedReason::kNotIssued;
      state->late_cancel_not_issued = key;
      if (attempt->shutdown_tracked) {
        attempt->shutdown_done = true;
        attempt->shutdown_failed = true;
        state->shutdown_cv.notify_all();
      }
      std::fprintf(stderr, "manyforge_loss_cancel status=skipped reason=%s\n",
                   expired ? "deadline_expired" : stale_handle ? "handle_changed" : "owner_failed");
      return;
    }
    if (sequence != 0) {
      const auto send_boundary = state->clock.now();
      std::lock_guard lock(state->mutex);
      const auto found = state->goals.find(key);
      if (found != state->goals.end() &&
          found->second.cancel_attempt == attempt &&
          found->second.cancel_sequence == sequence && !found->second.retired)
        found->second.observation.cancel_sent_local_stamp = send_boundary;
    }
    std::fprintf(stderr, "manyforge_loss_cancel status=sent\n");
  } catch (...) {
    {
      std::lock_guard lock(state->mutex);
      attempt->unresolved = LateCancelUnresolvedReason::kUnknown;
      state->late_cancel_not_issued = key;
      if (attempt->shutdown_tracked) {
        attempt->shutdown_done = true;
        attempt->shutdown_failed = true;
        state->shutdown_cv.notify_all();
      }
    }
    std::fprintf(stderr,
                 "manyforge_loss_cancel status=skipped reason=sdk_error\n");
  }
}

void Ros2ControlExecutionBackend::QueueAutomaticCancel(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::SharedPtr &handle,
    const std::shared_ptr<CancelAttempt> &attempt) noexcept {
  try {
    auto owner = state->epoch_owner.lock();
    auto client = attempt->client.lock();
    if (!owner || !client)
      throw std::runtime_error("exact late-cancel owner or client is unavailable");
    // Keep a callback-side copy across schedule(). On rejection the last
    // SDK-borrow release is posted through the existing retirement path.
    auto item = owner->retain_entity(std::make_shared<LateCancelItem>(
        LateCancelItem{state, state->clock_owner, std::move(client), handle, attempt, key}));
    (void)state->scheduler.schedule(
        state->clock.now(),
        [owner, item] {
          (void)owner; // Pins the exact epoch until the SDK borrow is released.
          SendAutomaticCancel(item->state, item->key, item->handle,
                              item->attempt, item->client);
        }, core::SchedulerPriority::StopControl);
  } catch (const std::exception &error) {
    {
      std::lock_guard lock(state->mutex);
      state->late_cancel_not_issued = key;
      attempt->unresolved = LateCancelUnresolvedReason::kNotIssued;
      if (attempt->shutdown_tracked) {
        attempt->shutdown_done = true;
        attempt->shutdown_failed = true;
        state->shutdown_cv.notify_all();
      }
    }
    std::fprintf(stderr, "manyforge_loss_cancel status=not_issued reason=%s\n",
                 error.what());
  } catch (...) {
    {
      std::lock_guard lock(state->mutex);
      state->late_cancel_not_issued = key;
      attempt->unresolved = LateCancelUnresolvedReason::kNotIssued;
      if (attempt->shutdown_tracked) {
        attempt->shutdown_done = true;
        attempt->shutdown_failed = true;
        state->shutdown_cv.notify_all();
      }
    }
    std::fprintf(stderr, "manyforge_loss_cancel status=not_issued reason=unknown\n");
  }
}

void Ros2ControlExecutionBackend::DrainLossCancels(
    const std::shared_ptr<State> &state) noexcept {
  decltype(state->loss_cancels) pending;
  {
    std::lock_guard lock(state->mutex);
    pending.swap(state->loss_cancels);
    state->loss_cancel_count = 0;
  }
  for (const auto &entry : pending) {
    if (!entry)
      continue;
    bool send = false;
    {
      std::lock_guard lock(state->mutex);
      if (!entry->attempt->attempted) {
        entry->attempt->attempted = true;
        send = true;
      }
    }
    if (send)
      QueueAutomaticCancel(state, entry->key, entry->handle, entry->attempt);
  }
}

void Ros2ControlExecutionBackend::ReceiveResponse(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::SharedPtr &handle,
    const std::shared_ptr<CancelAttempt> &attempt) {
  // Claim a shared automatic attempt under facts and send outside the lock.
  const auto response_id = handle ? mechanism_id(handle->get_goal_id()) : MechanismGoalId{std::string{}};
  auto retained_handle = handle ? state->epoch_resources->retain_entity(handle)
                                : GoalHandle::SharedPtr{};
  GoalHandle::SharedPtr previous_handle;
  bool send_cancel = false;
  {
    std::lock_guard facts_lock(state->mutex);
    const auto found = state->goals.find(key);
    if (handle && found != state->goals.end() &&
        found->second.cancel_attempt == attempt) {
      attempt->handle = retained_handle;
      attempt->handle_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    if (attempt->retired || state->closed) {
      if (attempt->shutdown_tracked && found != state->goals.end() &&
          found->second.cancel_attempt == attempt) {
        if (handle)
          found->second.observation.mechanism_goal_id = response_id;
        else {
          attempt->cancel_admission_open.store(false, std::memory_order_release);
          found->second.observation.phase = ExecutionGoalPhase::kTerminal;
          found->second.observation.result = ExecutionGoalResult::kRejected;
          found->second.observation.terminal_local_receipt_stamp = state->clock.now();
          attempt->shutdown_done = true;
          state->shutdown_cv.notify_all();
        }
      }
      if (handle && !attempt->attempted) {
        attempt->attempted = true;
        if (!state->closed || attempt->shutdown_tracked)
          send_cancel = true;
        else
          std::fprintf(
              stderr,
              "manyforge_loss_cancel status=skipped reason=owner_closed\n");
      }
    } else {
      if (!state->ready() || found == state->goals.end() ||
          found->second.retired)
        return;
      auto &goal = found->second;
      if (!handle) {
        attempt->cancel_admission_open.store(false, std::memory_order_release);
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
        previous_handle = std::move(goal.handle);
        goal.handle = retained_handle;
        goal.observation.mechanism_goal_id = response_id;
        goal.observation.accepted_stamp_ns = state->clock.now();
        if (goal.observation.phase == ExecutionGoalPhase::kPending)
          goal.observation.phase = ExecutionGoalPhase::kAccepted;
        if (goal.cancel_requested && !goal.cancel_sent &&
            goal.observation.phase != ExecutionGoalPhase::kTerminal) {
          goal.cancel_sent = true;
          if (!attempt->attempted) {
            attempt->attempted = true;
            send_cancel = true;
          }
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
  }
  if (!send_cancel)
    return;
  // Cancel requested before acceptance: send the exact cancel now, from this
  // callback, under the action leaf (one non-waiting SDK call, nothing else
  // acquired under it). No scheduler hop. The owner is alive while it runs
  // this callback; it is checked only so an unavailable owner or client keeps
  // its not-issued report, which QueueAutomaticCancel records unchanged.
  const auto client = attempt->client.lock();
  if (!client || state->epoch_owner.expired()) {
    QueueAutomaticCancel(state, key, retained_handle, attempt);
    return;
  }
  SendAutomaticCancel(state, key, retained_handle, attempt, client);
}
void Ros2ControlExecutionBackend::ReceiveFeedback(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::SharedPtr &handle, const Action::Feedback &feedback) {
  const auto id = mechanism_id(handle->get_goal_id());
  GoalHandle::SharedPtr previous_handle;
  std::lock_guard lock(state->mutex);
  if (!state->ready())
    return;
  const auto found = state->goals.find(key);
  if (found == state->goals.end())
    return;
  auto &goal = found->second;
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
  if (!goal.handle || goal.handle.get() != handle.get()) {
    auto retained_handle = state->epoch_resources->retain_entity(handle);
    previous_handle = std::move(goal.handle);
    goal.handle = std::move(retained_handle);
  }
  goal.observation.mechanism_goal_id = id;
  goal.observation.phase = ExecutionGoalPhase::kExecuting;
  if (state->config.motion_trace)
    state->config.motion_trace->Observe(goal.trace_token, goal.observation);
}
void Ros2ControlExecutionBackend::ReceiveResult(
    const std::shared_ptr<State> &state, const ExecutionGoalKey &key,
    const GoalHandle::WrappedResult &result) {
  GoalHandle::SharedPtr previous_handle;
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
  previous_handle = std::move(goal.handle);
  state->classify(goal);
  if (goal.observation.phase == ExecutionGoalPhase::kTerminal &&
      goal.observation.result != ExecutionGoalResult::kEndedUnknown)
    goal.cancel_attempt->cancel_admission_open.store(false, std::memory_order_release);
  if (state->config.motion_trace)
    state->config.motion_trace->Observe(goal.trace_token, goal.observation);
}

void Ros2ControlExecutionBackend::ReceiveGatedState(
    const std::shared_ptr<State> &state,
    const std::shared_ptr<std::atomic<bool>> &gate,
    const control_msgs::msg::JointTrajectoryControllerState &message) {
  if (gate && !gate->load(std::memory_order_acquire)) return;
  ReceiveState(state, message);
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
Ros2ControlExecutionBackend::GoalActivity
Ros2ControlExecutionBackend::ReadGoalActivity() const {
  std::lock_guard lock(state_->mutex);
  GoalActivity activity;
  activity.late_cancel_not_issued = state_->late_cancel_not_issued.has_value();
  for (const auto &[key, goal] : state_->goals) {
    const auto &observed = goal.observation;
    if (observed.phase == ExecutionGoalPhase::kTerminal) continue;
    activity.goal_running = true;
    std::optional<TimePoint> end;
    if (observed.accepted_stamp_ns)
      end = *observed.accepted_stamp_ns + goal.trajectory_ns;
    else if (goal.sent_stamp_ns)
      end = *goal.sent_stamp_ns + goal.trajectory_ns;
    if (observed.latest_feedback)
      end = observed.latest_feedback->local_receipt_stamp_ns + goal.trajectory_ns -
            static_cast<Duration>(observed.latest_feedback->cursor_s * 1e9);
    if (end && observed.cancel_sent_local_stamp)
      end = std::min(*end, *observed.cancel_sent_local_stamp);
    if (end && (!activity.expected_end || *end < *activity.expected_end))
      activity.expected_end = *end;
  }
  if (!state_->goal_order.empty()) {
    const auto &latest = state_->goals.at(state_->goal_order.back()).observation;
    activity.last_cancelled = latest.phase == ExecutionGoalPhase::kTerminal &&
                              latest.result == ExecutionGoalResult::kCanceled;
  }
  return activity;
}
std::vector<Ros2ControlExecutionBackend::UnresolvedLateCancel>
Ros2ControlExecutionBackend::ReadUnresolvedLateCancels() const {
  std::lock_guard lock(state_->mutex);
  std::vector<UnresolvedLateCancel> result;
  for (const auto &[key, goal] : state_->goals) {
    const auto &attempt = *goal.cancel_attempt;
    if (attempt.unresolved)
      result.push_back({state_->config.binding_epoch, key, goal.execution_id, *attempt.unresolved});
    else if (attempt.shutdown_tracked && !attempt.shutdown_done)
      result.push_back({state_->config.binding_epoch, key, goal.execution_id,
                        LateCancelUnresolvedReason::kUnknown});
    else if (goal.observation.result == ExecutionGoalResult::kEndedUnknown &&
             goal.send_started && goal.send_started->load(std::memory_order_acquire))
      // A cancel ACK closes the cancel obligation, not the goal's physical
      // outcome. Preserve the exact unknown goal for durable retirement.
      result.push_back({state_->config.binding_epoch, key, goal.execution_id,
                        LateCancelUnresolvedReason::kUnknown});
  }
  if (state_->late_cancel_not_issued &&
      !state_->goals.contains(*state_->late_cancel_not_issued))
    result.push_back({state_->config.binding_epoch, *state_->late_cancel_not_issued, std::nullopt,
                      LateCancelUnresolvedReason::kUnknown});
  return result;
}
bool Ros2ControlExecutionBackend::loss_cancel_shutdown_settled() const noexcept {
  std::lock_guard lock(state_->mutex);
  return std::none_of(state_->goals.begin(), state_->goals.end(), [](const auto &row) {
    const auto &attempt = *row.second.cancel_attempt;
    return attempt.shutdown_tracked && (!attempt.shutdown_done || attempt.shutdown_failed);
  });
}
ExecutionReadinessDTO Ros2ControlExecutionBackend::ReadReadiness() const {
  std::lock_guard lock(state_->mutex);
  return state_->readiness;
}
bool Ros2ControlExecutionBackend::ActionServerIsReady() const {
  // Direct, like SendGoal: pins the client and makes one thread-safe query.
  return ActionServerIsReadyImpl();
}
bool Ros2ControlExecutionBackend::ActionServerIsReadyImpl() const {
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
