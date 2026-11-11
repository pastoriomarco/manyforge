#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_ros2_control_execution/ros2_control_execution_backend.hpp>
#include <manyforge_ros2_control_execution/selected_ros_endpoint_facts.hpp>
#include <sensor_msgs/msg/joint_state.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::ros2_control_execution {
// Exercise malformed SDK callback inputs unavailable from a well-formed ROS
// action server. Normal sends, acceptance and positive ACK use the real client.
class JtcExecutionPortTestPeer {
public:
  static void receive_state(Ros2ControlExecutionBackend& port,
      const control_msgs::msg::JointTrajectoryControllerState& message) {
    Ros2ControlExecutionBackend::ReceiveState(port.state_, message);
  }
  static void receive_gated_state(Ros2ControlExecutionBackend& port,
      const std::shared_ptr<std::atomic<bool>>& gate,
      const control_msgs::msg::JointTrajectoryControllerState& message) {
    Ros2ControlExecutionBackend::ReceiveGatedState(port.state_, gate, message);
  }
  static bool executor_is_spinning(const Ros2TransportResources &resources) {
    return resources.executor_->is_spinning();
  }
  struct SdkOwners {
    std::weak_ptr<rclcpp_action::Client<control_msgs::action::FollowJointTrajectory>> client;
    std::weak_ptr<rclcpp::Subscription<control_msgs::msg::JointTrajectoryControllerState>> subscription;
    std::weak_ptr<rclcpp::Publisher<control_msgs::msg::SpeedScalingFactor>> publisher;
    bool expired() const {
      return client.expired() && subscription.expired() && publisher.expired();
    }
  };
  static SdkOwners sdk_owners(const Ros2ControlExecutionBackend &port) {
    return {port.action_client_, port.state_sub_, port.scale_pub_};
  }
  static auto client(const Ros2ControlExecutionBackend &port) {
    return port.action_client_;
  }
  static auto client_group(const Ros2ControlExecutionBackend &port) {
    return port.action_callback_group_;
  }
  static bool owns_sdk_entities(const Ros2ControlExecutionBackend &port) {
    return port.action_client_ && port.state_sub_ && port.scale_pub_;
  }
  static void cancel_response(
      Ros2ControlExecutionBackend &port, const core::ExecutionGoalKey &key,
      std::uint64_t sequence,
      const action_msgs::srv::CancelGoal::Response::SharedPtr &response) {
    Ros2ControlExecutionBackend::ReceiveCancelResponse(port.state_, key,
                                                       sequence, response);
  }
  static void terminal(
      Ros2ControlExecutionBackend &port, const core::ExecutionGoalKey &key,
      const rclcpp_action::ClientGoalHandle<
          control_msgs::action::FollowJointTrajectory>::WrappedResult &result) {
    Ros2ControlExecutionBackend::ReceiveResult(port.state_, key, result);
  }
};
namespace {
using namespace std::chrono_literals;
using namespace core;
using Action = control_msgs::action::FollowJointTrajectory;
using ServerGoal = rclcpp_action::ServerGoalHandle<Action>;

bool wait_until(const std::function<bool()> &condition) {
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition())
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return condition();
}
struct ScopedTransportQuiesce {
  Ros2TransportResources &resources;
  ~ScopedTransportQuiesce() { resources.quiesce(); }
};
// Holds an owner busy in one dispatched task (bounded at 3 s, below the
// owner-stall bound) until released or destroyed.
class BlockedOwner {
public:
  explicit BlockedOwner(Ros2TransportResources &owner)
      : thread_([this, &owner] {
          owner.invoke([this] {
            entered_.set_value();
            (void)release_future_.wait_for(3s);
          });
          finished_.store(true);
        }) {
    entered = entered_future_.wait_for(3s) == std::future_status::ready;
  }
  ~BlockedOwner() {
    release();
    thread_.join();
  }
  void release() {
    std::call_once(released_, [this] { release_.set_value(); });
  }
  bool finished() const { return finished_.load(); }
  bool entered{false};

private:
  std::promise<void> entered_;
  std::future<void> entered_future_ = entered_.get_future();
  std::promise<void> release_;
  std::future<void> release_future_ = release_.get_future();
  std::once_flag released_;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};
ExecutionGoalKey key(std::uint64_t generation) {
  return {ControllerGoalId("test.operation.arm"),
          ControllerGoalGeneration(generation)};
}
TrajectoryDTO trajectory() {
  TrajectoryDTO value;
  value.joint_names = {"b", "a"};
  value.waypoints = {{0.0, {0.2, 0.1}, {0.0, 0.0}, {0.0, 0.0}},
                     {1.0, {0.4, 0.3}, {0.0, 0.0}, {0.0, 0.0}}};
  return value;
}
class Sink final : public IExecutionStateSink {
public:
  ExecutionStateSinkResult
  apply_group_joint_positions(const std::string &, const std::vector<double> &,
                              ExecutionStateUpdateMode) override {
    return {};
  }
  ExecutionStateSinkResult
  apply_group_joint_position(const std::string &, const std::string &, double,
                             ExecutionStateUpdateMode) override {
    return {};
  }
  std::optional<double>
  group_joint_position(const std::string &,
                       const std::string &) const override {
    return std::nullopt;
  }
};

// The production scheduler has an independent StopControl worker. This
// small test scheduler preserves that asynchronous boundary for late cancel.
class TestStopScheduler final : public IScheduler {
public:
  TestStopScheduler() : worker_([this] {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return closing_ || (!paused_ && !tasks_.empty()); });
        if (tasks_.empty() && closing_) return;
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }) {}
  ~TestStopScheduler() override {
    { std::lock_guard lock(mutex_); closing_ = true; }
    cv_.notify_all();
    worker_.join();
  }
  ScheduleHandle schedule(TimePoint, std::function<void()> task,
                          SchedulerPriority priority) override {
    if (reject.load()) throw std::runtime_error("injected StopControl refusal");
    if (priority != SchedulerPriority::StopControl)
      throw std::logic_error("unexpected test scheduler lane");
    std::lock_guard lock(mutex_);
    const auto handle = ++next_;
    tasks_.push_back(std::move(task));
    cv_.notify_one();
    return handle;
  }
  void cancel(ScheduleHandle) override {}
  // Holds accepted tasks queued (not run) until resume(); closing runs them.
  void pause() { std::lock_guard lock(mutex_); paused_ = true; }
  void resume() {
    { std::lock_guard lock(mutex_); paused_ = false; }
    cv_.notify_all();
  }
  std::size_t queued() {
    std::lock_guard lock(mutex_);
    return tasks_.size();
  }
  std::atomic<bool> reject{false};
private:
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  bool closing_{false};
  bool paused_{false};
  ScheduleHandle next_{0};
  std::thread worker_;
};

class Rig {
public:
  std::shared_ptr<FakeClock> clock_owner = std::make_shared<FakeClock>();
  FakeClock &clock = *clock_owner;
  TestStopScheduler scheduler;
  std::shared_ptr<Sink> sink = std::make_shared<Sink>();
  // The fake server remains live independently of each adapter epoch.
  std::shared_ptr<Ros2TransportResources> resources_owner;
  Ros2TransportResources &resources;
  std::shared_ptr<Ros2TransportResources> epoch_resources;
  std::shared_ptr<Ros2TransportResources> successor_resources;
  std::unique_ptr<Ros2ControlExecutionBackend> adapter;
  std::unique_ptr<Ros2ControlExecutionBackend> successor;
  std::atomic<std::uint64_t> cancel_count{0};
  std::atomic<std::uint64_t> scale_count{0};
  std::atomic<double> last_scale{-1.0};
  std::atomic<bool> reject_next{false};
  std::atomic<bool> reject_cancel{false};
  std::atomic<bool> delay_next_response{false};
  std::atomic<bool> response_blocked{false};
  std::atomic<bool> delay_next_cancel{false};
  std::atomic<bool> cancel_blocked{false};
  std::atomic<bool> accepted_callback_context_valid{false};
  ExecutionGoalTolerancePolicy goal_tolerance_policy_{
      ExecutionGoalTolerancePolicy::kErased};

  explicit Rig(bool two_bindings = false, bool scheduled_replacement = false,
               ExecutionGoalTolerancePolicy goal_tolerance_policy =
                   ExecutionGoalTolerancePolicy::kErased)
      : resources_owner(Ros2TransportResources::create(
            "jtc_adapter_test_" + std::to_string(++sequence_), "", 2, sink, "arm", clock)),
        resources(*resources_owner) {
    goal_tolerance_policy_ = goal_tolerance_policy;
    route_ = "/adapter_test_" + std::to_string(sequence_.load());
    server_ = rclcpp_action::create_server<Action>(
        resources.node(), route_ + "/fjt",
        [this](const rclcpp_action::GoalUUID &,
               std::shared_ptr<const Action::Goal>) {
          if (delay_next_response.exchange(false)) {
            std::unique_lock lock(response_mutex_);
            response_blocked = true;
            response_cv_.wait_for(lock, 3s,
                                  [this] { return response_released_; });
          }
          return reject_next.exchange(false)
                     ? rclcpp_action::GoalResponse::REJECT
                     : rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
        },
        [this](std::shared_ptr<ServerGoal> handle) {
          {
            std::lock_guard lock(cancel_ids_mutex_);
            cancel_ids_.push_back(handle->get_goal_id());
          }
          ++cancel_count;
          if (delay_next_cancel.exchange(false)) {
            std::unique_lock lock(response_mutex_);
            cancel_blocked = true;
            response_cv_.wait_for(lock, 3s,
                                  [this] { return response_released_; });
          }
          return reject_cancel.load() ? rclcpp_action::CancelResponse::REJECT
                                      : rclcpp_action::CancelResponse::ACCEPT;
        },
        [this](std::shared_ptr<ServerGoal> handle) {
          accepted_callback_context_valid =
              resources.node()->get_node_base_interface()->get_context()->is_valid();
          std::lock_guard lock(mutex_);
          goals_.push_back(std::move(handle));
        });
    state_pub_ = resources.node()
                     ->create_publisher<
                         control_msgs::msg::JointTrajectoryControllerState>(
                         route_ + "/state", rclcpp::QoS(10).reliable());
    scale_sub_ = make_scale_subscriber();
    clock.set(42);
    epoch_resources = make_epoch();
    adapter = std::make_unique<Ros2ControlExecutionBackend>(
        epoch_resources, clock_owner, scheduler, config(1, scheduled_replacement));
    if (two_bindings) {
      successor_resources = make_epoch();
      successor = std::make_unique<Ros2ControlExecutionBackend>(
          successor_resources, clock_owner, scheduler, config(2, scheduled_replacement));
    }
    adapter->ObserveReadiness({true, true, true, ExecutionBindingEpoch{1}});
    resources.start();
    epoch_resources->start();
    if (successor_resources)
      successor_resources->start();
  }
  ~Rig() {
    release_response();
    if (adapter)
      adapter->shutdown();
    if (successor)
      successor->shutdown();
    resources.quiesce();
    adapter.reset();
    successor.reset();
  }
  rclcpp::Subscription<control_msgs::msg::SpeedScalingFactor>::SharedPtr
  make_scale_subscriber() {
    return resources.node()
        ->create_subscription<control_msgs::msg::SpeedScalingFactor>(
            route_ + "/scale", rclcpp::QoS(1).reliable().transient_local(),
            [this](control_msgs::msg::SpeedScalingFactor::ConstSharedPtr value) {
              last_scale = value->factor;
              ++scale_count;
            });
  }
  // The fake controller stops and starts listening for scale input; its
  // subscription is created and destroyed on its own owner thread.
  void drop_scale_subscriber() {
    resources.invoke([this] { scale_sub_.reset(); });
  }
  void restore_scale_subscriber() {
    resources.invoke_creation([this] { scale_sub_ = make_scale_subscriber(); });
  }
  std::shared_ptr<Ros2TransportResources> make_epoch() {
    return Ros2TransportResources::create(
        "jtc_epoch_test_" + std::to_string(++sequence_), "", 1);
  }
  void rebind(std::uint64_t epoch) {
    epoch_resources = make_epoch();
    adapter = std::make_unique<Ros2ControlExecutionBackend>(
        epoch_resources, clock_owner, scheduler, config(epoch));
    adapter->ObserveReadiness({true, true, true, ExecutionBindingEpoch{epoch}});
    epoch_resources->start();
  }
  void release_response() {
    std::lock_guard lock(response_mutex_);
    response_released_ = true;
    response_cv_.notify_all();
  }
  void remove_action_server() {
    // The server executor's wait set can retain the waitable after the last
    // fixture reference is reset. Unregister and wake recollection first.
    auto server = std::move(server_);
    const auto node = resources.node();
    node->get_node_waitables_interface()->remove_waitable(server, nullptr);
    node->get_node_base_interface()->get_default_callback_group()
        ->trigger_notify_guard_condition();
    server.reset();
  }
  JtcExecutionAdapterConfig config(std::uint64_t epoch,
                                   bool scheduled_replacement = false) const {
    JtcExecutionAdapterConfig config;
    config.action_name = route_ + "/fjt";
    config.state_topic = route_ + "/state";
    config.scale_topic = route_ + "/scale";
    config.binding_epoch = ExecutionBindingEpoch(epoch);
    config.lifecycle_command_timeout_ns = 1'000'000'000;
    config.lane_facts.joint_names = {"a", "b"};
    config.lane_facts.supports_scale = true;
    config.lane_facts.supports_scheduled_replacement = scheduled_replacement;
    config.lane_facts.has_measured_velocity = true;
    config.lane_facts.controller_update_rate_hz = 100;
    config.lane_facts.goal_tolerance_policy = goal_tolerance_policy_;
    return config;
  }
  ExecutionPortCommandResultDTO submit(const ExecutionGoalKey &goal_key) {
    // Test waits for ROS discovery outside the port; port calls never wait.
    ExecutionPortCommandResultDTO result;
    EXPECT_TRUE(wait_until([&] {
      result = adapter->Submit(goal_key, trajectory());
      return result.dispatch != ExecutionPortDispatch::kNotDispatched ||
             result.refusal != ExecutionPortRefusal::kNotReady;
    }));
    return result;
  }
  bool accepted(const ExecutionGoalKey &goal_key,
                Ros2ControlExecutionBackend *port = nullptr) {
    if (!port)
      port = adapter.get();
    return wait_until([&] {
      const auto goal = port->ReadGoal(goal_key);
      return goal && goal->accepted_stamp_ns.has_value();
    });
  }
  std::shared_ptr<ServerGoal> handle(std::size_t index = 0) {
    std::shared_ptr<ServerGoal> result;
    const bool received = wait_until([&] {
      std::lock_guard lock(mutex_);
      if (goals_.size() <= index)
        return false;
      result = goals_[index];
      return true;
    });
    if (!received)
      throw std::runtime_error("Fake FJT server did not receive goal");
    return result;
  }
  std::size_t goal_count() {
    std::lock_guard lock(mutex_);
    return goals_.size();
  }
  std::vector<rclcpp_action::GoalUUID> cancel_ids() {
    std::lock_guard lock(cancel_ids_mutex_);
    return cancel_ids_;
  }
  // Publishes one controller state sample and reports whether the adapter
  // received it within `bound`. The state subscription runs on the epoch's
  // time-critical thread, with the action client's callbacks: its arrival
  // shows that thread is not blocked.
  bool state_arrives_within(std::uint32_t nanosecond,
                            std::chrono::milliseconds bound) {
    if (!wait_until([&] { return state_pub_->get_subscription_count() > 0; }))
      return false;
    control_msgs::msg::JointTrajectoryControllerState sample;
    sample.header.stamp.sec = 10;
    sample.header.stamp.nanosec = nanosecond;
    sample.joint_names = {"b", "a"};
    sample.reference.positions = {0.2, 0.1};
    sample.output.positions = {0.21, 0.11};
    sample.feedback.positions = {0.19, 0.09};
    sample.error.positions = {0.01, 0.01};
    state_pub_->publish(sample);
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto observed = adapter->ReadSample();
      if (observed && observed->source_stamp_ns == 10'000'000'000 + nanosecond)
        return true;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }
  void state(double factor = 0.25, std::uint32_t nanosecond = 0) {
    EXPECT_TRUE(
        wait_until([&] { return state_pub_->get_subscription_count() > 0; }));
    control_msgs::msg::JointTrajectoryControllerState sample;
    sample.header.stamp.sec = 10;
    sample.header.stamp.nanosec = nanosecond;
    sample.joint_names = {"b", "a"};
    sample.reference.positions = {0.2, 0.1};
    sample.reference.velocities = {0.02, 0.01};
    sample.reference.accelerations = {0.002, 0.001};
    sample.reference.time_from_start.sec = 1;
    sample.output.positions = {0.21, 0.11};
    sample.feedback.positions = {0.19, 0.09};
    sample.feedback.velocities = {0.03, 0.04};
    sample.error.positions = {0.01, 0.01};
    sample.speed_scaling_factor = factor;
    state_pub_->publish(sample);
    EXPECT_TRUE(wait_until([&] {
      const auto observed = adapter->ReadSample();
      return observed &&
             observed->source_stamp_ns == 10'000'000'000 + nanosecond;
    }));
  }

private:
  static std::atomic<std::uint64_t> sequence_;
  std::string route_;
  std::mutex mutex_;
  std::mutex cancel_ids_mutex_;
  std::vector<rclcpp_action::GoalUUID> cancel_ids_;
  std::mutex response_mutex_;
  std::condition_variable response_cv_;
  bool response_released_{false};
  std::vector<std::shared_ptr<ServerGoal>> goals_;
  rclcpp_action::Server<Action>::SharedPtr server_;
  rclcpp::Publisher<
      control_msgs::msg::JointTrajectoryControllerState>::SharedPtr state_pub_;
  rclcpp::Subscription<control_msgs::msg::SpeedScalingFactor>::SharedPtr
      scale_sub_;
};
std::atomic<std::uint64_t> Rig::sequence_{0};

TEST(Ros2ControlExecutionBackendTest, StagedStateGateDoesNotReplayPremarkerSample) {
  Rig rig;
  auto epoch = rig.make_epoch();
  auto gate = std::make_shared<std::atomic<bool>>(false);
  Ros2ControlExecutionBackend adapter(epoch, rig.clock_owner, rig.scheduler, rig.config(2), gate);
  // State samples are admitted only once the backend's readiness is observed.
  adapter.ObserveReadiness({true, true, true, ExecutionBindingEpoch(2)});
  control_msgs::msg::JointTrajectoryControllerState message;
  message.joint_names = {"a", "b"};
  message.reference.positions = {0.1, 0.2};
  message.reference.velocities = {0.0, 0.0};
  message.output.positions = {0.1, 0.2};
  message.feedback.positions = {0.1, 0.2};
  message.error.positions = {0.0, 0.0};
  JtcExecutionPortTestPeer::receive_gated_state(adapter, gate, message);
  EXPECT_FALSE(adapter.ReadSample());
  gate->store(true, std::memory_order_release);
  EXPECT_FALSE(adapter.ReadSample());
  message.header.stamp.sec = 9;
  JtcExecutionPortTestPeer::receive_gated_state(adapter, gate, message);
  const auto sample = adapter.ReadSample();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->source_stamp_ns, 9'000'000'000LL);
}

TEST(JtcExecutionPort, StateReceiptIsAcquiredBeforeStateLockAndNeverRestamped) {
  Rig rig;
  class AcquisitionClock final : public IClock {
   public:
    std::atomic<TimePoint> value{100};
    std::function<void()> on_read;
    TimePoint now() const override {
      const auto acquired = value.load();
      if (on_read) on_read();
      return acquired;
    }
  };
  auto clock_owner = std::make_shared<AcquisitionClock>();
  auto &clock = *clock_owner;
  auto epoch = rig.make_epoch();
  Ros2ControlExecutionBackend adapter(epoch, clock_owner, rig.scheduler, rig.config(2));
  adapter.ObserveReadiness({true, true, true, ExecutionBindingEpoch(2)});
  std::promise<void> entered;
  auto acquisition = entered.get_future();
  auto lock_probe = std::async(std::launch::async, [&] {
    acquisition.wait();
    return adapter.ReadReadiness();
  });
  bool read_outside_state_lock = false;
  clock.on_read = [&] {
    entered.set_value();
    read_outside_state_lock = lock_probe.wait_for(1s) == std::future_status::ready;
    clock.value.store(200); // Initialization/delivery time advances independently.
  };
  control_msgs::msg::JointTrajectoryControllerState message;
  message.header.stamp.sec = 7;
  message.joint_names = {"a", "b"};
  message.reference.positions = {0.1, 0.2};
  message.reference.velocities = {0.0, 0.0};
  message.output.positions = {0.1, 0.2};
  message.feedback.positions = {0.1, 0.2};
  message.error.positions = {0.0, 0.0};
  JtcExecutionPortTestPeer::receive_state(adapter, message);
  static_cast<void>(lock_probe.get());
  EXPECT_TRUE(read_outside_state_lock);
  const auto sample = adapter.ReadSample();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->local_receipt_stamp_ns, 100);
  EXPECT_EQ(sample->source_stamp_ns, 7'000'000'000LL);
  EXPECT_EQ(clock.value.load(), 200);
}

TEST(JtcExecutionPort, EndpointFactsKeepRetainedBindingsAndScalingPublishersSeparate) {
  Rig rig(true);
  ASSERT_TRUE(wait_until([&] {
    rig.adapter->RefreshEndpointFacts();
    rig.successor->RefreshEndpointFacts();
    return !rig.adapter->ReadEndpointDiagnostic() &&
           !rig.successor->ReadEndpointDiagnostic();
  }));
  rig.state();
  rig.adapter->RefreshEndpointFacts();
  EXPECT_FALSE(rig.adapter->ReadEndpointDiagnostic());
  EXPECT_EQ(rig.goal_count(), 0U);
}

// OD-62 (b): the scale publisher's matched event tracks whether the
// controller listens; readers load an atomic, nothing polls the graph.
TEST(JtcExecutionPort, ScaleInputListeningFollowsTheMatchedSubscriber) {
  Rig rig;
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ScaleInputListening() == std::optional(true); }));
  rig.drop_scale_subscriber();
  EXPECT_TRUE(wait_until([&] { return rig.adapter->ScaleInputListening() == std::optional(false); }));
  rig.restore_scale_subscriber();
  EXPECT_TRUE(wait_until([&] { return rig.adapter->ScaleInputListening() == std::optional(true); }));
}

TEST(JtcExecutionPort, EndpointFactsNameSecondPublisherAndRestartOverlap) {
  Rig rig;
  std::optional<ErrorDTO> observed;
  const auto describe = [&] {
    if (!observed) return std::string("clean");
    std::ostringstream out;
    out << observed->code << ": " << observed->message;
    for (const auto &[key, value] : observed->details)
      out << " " << key << "=" << value;
    return out.str();
  };
  ASSERT_TRUE(wait_until([&] {
    rig.adapter->RefreshEndpointFacts();
    observed = rig.adapter->ReadEndpointDiagnostic();
    return !observed;
  })) << "initial publisher: " << describe();
  const auto topic = rig.config(1).state_topic;
  // A second instance's publisher, on its own unspun node (see
  // EndpointFactsCountZeroOneAndTwoPublishers).
  auto peer_owner = Ros2TransportResources::create("endpoint_second_instance", "", 1);
  auto &peer = *peer_owner;
  auto second = peer.node()->create_publisher<
      control_msgs::msg::JointTrajectoryControllerState>(topic, rclcpp::QoS(10));
  ASSERT_TRUE(wait_until([&] {
    rig.adapter->RefreshEndpointFacts();
    observed = rig.adapter->ReadEndpointDiagnostic();
    return observed && observed->code == "duplicate_component_instance";
  })) << "second publisher: " << describe();
  const auto error = rig.adapter->ReadEndpointDiagnostic();
  ASSERT_TRUE(error);
  EXPECT_EQ(error->details.at("topic"), topic);
  EXPECT_EQ(error->details.at("endpoint_count"), "2");
  second.reset();
  ASSERT_TRUE(wait_until([&] {
    rig.adapter->RefreshEndpointFacts();
    observed = rig.adapter->ReadEndpointDiagnostic();
    return !observed;
  })) << "publisher removed: " << describe();
  EXPECT_EQ(rig.goal_count(), 0U);
}

TEST(JtcExecutionPort, EndpointGraphFindsIncompatibleQosPublisherWithoutMatchedCount) {
  Rig rig;
  ASSERT_TRUE(wait_until([&] {
    rig.adapter->RefreshEndpointFacts();
    return !rig.adapter->ReadEndpointDiagnostic();
  }));
  auto incompatible = rig.resources.node()->create_publisher<
      control_msgs::msg::JointTrajectoryControllerState>(
      rig.config(1).state_topic, rclcpp::QoS(10).best_effort());
  ASSERT_TRUE(wait_until([&] {
    rig.adapter->RefreshEndpointFacts();
    const auto error = rig.adapter->ReadEndpointDiagnostic();
    return error && error->code == "duplicate_component_instance";
  }));
  EXPECT_EQ(rig.adapter->ReadEndpointDiagnostic()->details.at("endpoint_count"), "2");
  EXPECT_EQ(rig.goal_count(), 0U);
}

TEST(JtcExecutionPort, EndpointFactsDoNotCountSharedJointStatePublishers) {
  Rig rig;
  auto first = rig.resources.node()->create_publisher<sensor_msgs::msg::JointState>(
      "/shared_isaac_joint_states", 10);
  auto second = rig.resources.node()->create_publisher<sensor_msgs::msg::JointState>(
      "/shared_isaac_joint_states", 10);
  ASSERT_TRUE(wait_until([&] {
    rig.adapter->RefreshEndpointFacts();
    return !rig.adapter->ReadEndpointDiagnostic();
  }));
  EXPECT_EQ(rig.goal_count(), 0U);
}

TEST(JtcExecutionPort, EndpointFactsReportGraphReadFailureAsNotEstablished) {
  Rig rig;
  SelectedRosEndpointFacts invalid(rig.resources.node(), "invalid topic name");
  invalid.Refresh();
  const auto error = invalid.ReadError();
  ASSERT_TRUE(error);
  EXPECT_EQ(error->code, "component_instance_graph_unsettled");
  EXPECT_FALSE(error->details.at("cause").empty());
  EXPECT_EQ(rig.goal_count(), 0U);
}

// The selected topic's publisher count alone decides: 0 is not
// established, 1 is clean, 2+ is a duplicate instance.
std::optional<std::string> endpoint_code(SelectedRosEndpointFacts &facts) {
  facts.Refresh();
  const auto error = facts.ReadError();
  return error ? std::optional<std::string>(error->code) : std::nullopt;
}

TEST(JtcExecutionPort, EndpointFactsCountZeroOneAndTwoPublishers) {
  Rig rig;
  const auto node = rig.resources.node();
  const std::string topic = "/endpoint_count_rule";
  SelectedRosEndpointFacts facts(node, topic);
  // Before any Refresh and with no publisher: not established.
  ASSERT_TRUE(facts.ReadError());
  EXPECT_EQ(facts.ReadError()->code, "component_instance_graph_unsettled");
  EXPECT_EQ(endpoint_code(facts), "component_instance_graph_unsettled");
  EXPECT_EQ(facts.ReadError()->details.at("endpoint_count"), "0");
  // The publishers live on another, unspun node: a publisher on the spinning
  // rig node stays in the graph until its executor wakes up again.
  auto peer_owner = Ros2TransportResources::create("endpoint_count_peer", "", 1);
  auto &peer = *peer_owner;
  auto first = peer.node()->create_publisher<sensor_msgs::msg::JointState>(topic, 10);
  ASSERT_TRUE(wait_until([&] { return !endpoint_code(facts); }));
  auto second = peer.node()->create_publisher<sensor_msgs::msg::JointState>(topic, 10);
  ASSERT_TRUE(wait_until(
      [&] { return endpoint_code(facts) == "duplicate_component_instance"; }));
  EXPECT_EQ(facts.ReadError()->details.at("topic"), topic);
  EXPECT_EQ(facts.ReadError()->details.at("endpoint_count"), "2");
  second.reset();
  ASSERT_TRUE(wait_until([&] { return !endpoint_code(facts); }));
  first.reset();
  ASSERT_TRUE(wait_until(
      [&] { return endpoint_code(facts) == "component_instance_graph_unsettled"; }));
  EXPECT_EQ(rig.goal_count(), 0U);
}

TEST(JtcExecutionPort, EndpointFactsMatchedCountAboveOneIsADuplicate) {
  Rig rig;
  const auto node = rig.resources.node();
  const std::string topic = "/endpoint_count_matched";
  auto publisher = node->create_publisher<sensor_msgs::msg::JointState>(topic, 10);
  SelectedRosEndpointFacts facts(node, topic);
  ASSERT_TRUE(wait_until([&] { return !endpoint_code(facts); }));
  facts.ObserveMatchedCount(2);
  EXPECT_EQ(facts.ReadError()->code, "duplicate_component_instance");
  facts.ObserveMatchedCount(1);
  EXPECT_FALSE(facts.ReadError());
  EXPECT_EQ(rig.goal_count(), 0U);
}

TEST(JtcExecutionPort, EndpointFactsIgnoreUnrelatedNodesStartingAndStopping) {
  Rig rig;
  const auto node = rig.resources.node();
  const std::string topic = "/endpoint_count_unrelated";
  auto publisher = node->create_publisher<sensor_msgs::msg::JointState>(topic, 10);
  SelectedRosEndpointFacts facts(node, topic);
  ASSERT_TRUE(wait_until([&] { return !endpoint_code(facts); }));
  const auto seen = [&](const std::string &name) {
    const auto names = node->get_node_names();
    return std::find(names.begin(), names.end(), "/" + name) != names.end();
  };
  for (int cycle = 0; cycle < 3; ++cycle) {
    const auto name = "endpoint_count_unrelated_" + std::to_string(cycle);
    auto unrelated = Ros2TransportResources::create(name, "", 1);
    auto other = unrelated->node()->create_publisher<sensor_msgs::msg::JointState>(
        "/endpoint_count_unrelated_other", 10);
    ASSERT_TRUE(wait_until([&] {
      EXPECT_FALSE(endpoint_code(facts));
      return seen(name);
    }));
    EXPECT_FALSE(endpoint_code(facts));
    other.reset();
    unrelated.reset();
    ASSERT_TRUE(wait_until([&] {
      EXPECT_FALSE(endpoint_code(facts));
      return !seen(name);
    }));
    EXPECT_FALSE(endpoint_code(facts));
  }
  EXPECT_EQ(rig.goal_count(), 0U);
}

TEST(JtcExecutionPort, SubmitMapsFullPvaAndReportsLocalAcceptanceAndUuid) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto observation = rig.adapter->ReadGoal(key(1));
  ASSERT_TRUE(observation);
  EXPECT_EQ(observation->binding_epoch, ExecutionBindingEpoch{1});
  EXPECT_EQ(observation->accepted_stamp_ns, TimePoint{42});
  EXPECT_FALSE(observation->mechanism_goal_id.empty());
  EXPECT_FALSE(observation->result);
  const auto actual = rig.handle()->get_goal();
  EXPECT_EQ(actual->trajectory.joint_names,
            (std::vector<std::string>{"a", "b"}));
  EXPECT_EQ(actual->trajectory.points[0].positions,
            (std::vector<double>{0.1, 0.2}));
  EXPECT_EQ(actual->trajectory.points[0].accelerations.size(), 2U);
  ASSERT_EQ(actual->goal_tolerance.size(), 2U);
  EXPECT_EQ(actual->goal_tolerance[0].position, -1.0);
}

// Arrival: a lane that keeps goal tolerances sends none, so stock JTC
// applies its configured per-joint `goal` constraint (the lane's arrival
// tolerance) and succeeds only when finished within it.
TEST(JtcExecutionPort, KeptGoalTolerancesSendNoOverride) {
  Rig rig(false, false, ExecutionGoalTolerancePolicy::kKept);
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto actual = rig.handle()->get_goal();
  EXPECT_TRUE(actual->goal_tolerance.empty());
  EXPECT_TRUE(actual->path_tolerance.empty());
}

// On a lane that keeps goal tolerances,
// a Stop goal (ramp, spring-back, brake), immediate or scheduled, carries no
// per-goal override and no goal time, exactly like a motion goal: stock JTC
// applies the lane's arrival tolerance and `goal_time`. A cancel-type Stop
// sends no goal at all.
TEST(JtcExecutionPort, StopGoalsKeepTheLaneArrivalCheckLikeMotionGoals) {
  const auto lane_check = [](const control_msgs::action::FollowJointTrajectory::Goal &goal) {
    return goal.goal_tolerance.empty() && goal.path_tolerance.empty() &&
           goal.goal_time_tolerance.sec == 0 && goal.goal_time_tolerance.nanosec == 0U;
  };
  Rig rig(false, true, ExecutionGoalTolerancePolicy::kKept);
  // The fake server never ends goals: preempt each predecessor like stock JTC,
  // so at most two goal records stay unfinished.
  const auto preempt = [&](std::size_t index, const ExecutionGoalKey &predecessor) {
    auto result = std::make_shared<Action::Result>();
    result->error_code = Action::Result::INVALID_GOAL;
    // Stock JTC's preemption result, so the record ends known (evictable).
    result->error_string = "Current goal preempted by new incoming action.";
    rig.handle(index)->abort(result);
    ASSERT_TRUE(wait_until([&] {
      const auto goal = rig.adapter->ReadGoal(predecessor);
      return !goal || goal->result;
    }));
  };
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  EXPECT_TRUE(lane_check(*rig.handle(0)->get_goal()));
  // An ordinary motion replacement (the default purpose).
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  EXPECT_TRUE(lane_check(*rig.handle(1)->get_goal()));
  preempt(0, key(1));
  // A Stop replacement.
  const auto stop_replace = rig.adapter->Replace(key(2), key(3), trajectory());
  ASSERT_EQ(stop_replace.dispatch, ExecutionPortDispatch::kDispatched)
      << (stop_replace.error ? stop_replace.error->message : std::string("no error"));
  ASSERT_TRUE(rig.accepted(key(3)));
  EXPECT_TRUE(lane_check(*rig.handle(2)->get_goal()));
  preempt(1, key(2));
  // A scheduled Stop replacement.
  ExecutionReplacementStartDTO start{ExecutionBindingEpoch(1U), 1'234'567'890'123LL};
  ASSERT_EQ(rig.adapter
                ->ReplaceScheduled(key(3), key(4), trajectory(), start)
                .dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(4)));
  EXPECT_TRUE(lane_check(*rig.handle(3)->get_goal()));
  // A cancel-type Stop: the cancel request only, no trajectory goal.
  const auto cancels = rig.cancel_count.load();
  ASSERT_NE(rig.adapter->Cancel(key(4)).dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_TRUE(wait_until([&] { return rig.cancel_count.load() == cancels + 1U; }));
  EXPECT_EQ(rig.goal_count(), 4U);
}

TEST(JtcExecutionPort, RefusesInvalidPvaAndReplayedKeyBeforeEffect) {
  Rig rig;
  auto invalid = trajectory();
  invalid.waypoints[0].accelerations_rad_s2.clear();
  EXPECT_EQ(rig.adapter->Submit(key(1), invalid).refusal,
            ExecutionPortRefusal::kInvalidInput);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1)));
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  EXPECT_EQ(rig.adapter->Submit(key(1), trajectory()).refusal,
            ExecutionPortRefusal::kStaleKey);
  EXPECT_EQ(rig.adapter->Cancel(key(9)).refusal,
            ExecutionPortRefusal::kStaleKey);
  EXPECT_EQ(rig.cancel_count.load(), 0U);
}

TEST(JtcExecutionPort, PendingCancelIsRetainedAndSentOnAcceptance) {
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  const auto pending = rig.adapter->ReadGoal(key(1));
  ASSERT_TRUE(pending);
  EXPECT_TRUE(pending->mechanism_goal_id.empty());
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kEffectUncertain);
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kEffectUncertain);
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp);
  rig.release_response();
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))
        ->cancel_ack_local_receipt_stamp.has_value();
  }));
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp,
            TimePoint{42});
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->result);
  EXPECT_FALSE(rig.adapter->ReadSample());
}

// Readiness-loss cancels still leave through the StopControl scheduler; a
// refused schedule is reported at once, on the caller's thread.
TEST(JtcExecutionPort, RejectedLossCancelScheduleReportsUnknownActivity) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  rig.scheduler.reject.store(true);
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{1}});
  EXPECT_TRUE(rig.adapter->ReadGoalActivity().late_cancel_not_issued);
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  const auto goal = rig.adapter->ReadGoal(key(1));
  ASSERT_TRUE(goal);
  EXPECT_FALSE(goal->cancel_sent_local_stamp);
  EXPECT_EQ(goal->result, ExecutionGoalResult::kEndedUnknown);
}

// OD-111: a cancel requested before acceptance leaves through the
// StopControl lane, never from the goal-response callback. A refused schedule
// is not retried: that exact goal's late cancel is recorded as not issued,
// which raises the component's typed fault (ReadGoalActivity).
TEST(JtcExecutionPort, RefusedPendingCancelScheduleIsRecordedNotIssued) {
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kEffectUncertain);
  rig.scheduler.reject.store(true);
  rig.release_response();
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_TRUE(wait_until(
      [&] { return rig.adapter->ReadGoalActivity().late_cancel_not_issued; }));
  const auto unresolved = rig.adapter->ReadUnresolvedLateCancels();
  ASSERT_EQ(unresolved.size(), 1U);
  EXPECT_EQ(unresolved.front().key, key(1));
  EXPECT_EQ(unresolved.front().reason,
            Ros2ControlExecutionBackend::LateCancelUnresolvedReason::kNotIssued);
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp);
}

// The same refusal for a shutdown-tracked attempt completes that tracking at
// once: close does not wait for its deadline.
TEST(JtcExecutionPort, RefusedLateCancelScheduleCompletesTrackedShutdown) {
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  rig.scheduler.reject.store(true);
  // Close tracks the sent, unaccepted goal; its acceptance arrives during
  // close's wait and claims the late cancel, whose schedule is refused.
  std::jthread responder([&] {
    (void)wait_until([&] { return !rig.adapter->ReadReadiness().controller_active; });
    rig.release_response();
  });
  const auto started = std::chrono::steady_clock::now();
  EXPECT_TRUE(rig.adapter->close_admission(started + 6s));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 3s);
  responder.join();
  EXPECT_TRUE(rig.adapter->ReadGoalActivity().late_cancel_not_issued);
  const auto unresolved = rig.adapter->ReadUnresolvedLateCancels();
  ASSERT_EQ(unresolved.size(), 1U);
  EXPECT_EQ(unresolved.front().key, key(1));
  EXPECT_EQ(unresolved.front().reason,
            Ros2ControlExecutionBackend::LateCancelUnresolvedReason::kNotIssued);
  EXPECT_FALSE(rig.adapter->loss_cancel_shutdown_settled());
  EXPECT_EQ(rig.cancel_count.load(), 0U);
}

// A Stop arriving while the deferred cancel is queued joins it: one cancel
// request with the exact goal id; the Stop reports it dispatched and the
// deferred cancel records its send and acknowledgement.
TEST(JtcExecutionPort, StopJoinsQueuedPendingCancelExactlyOnce) {
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kEffectUncertain);
  rig.scheduler.pause();
  struct ResumeOnExit {
    TestStopScheduler &scheduler;
    ~ResumeOnExit() { scheduler.resume(); }
  } resume{rig.scheduler};
  rig.release_response();
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_TRUE(wait_until([&] { return rig.scheduler.queued() == 1U; }));
  const auto accepted = rig.handle();
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  EXPECT_EQ(rig.scheduler.queued(), 1U);
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  rig.scheduler.resume();
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))
        ->cancel_ack_local_receipt_stamp.has_value();
  }));
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_EQ(rig.cancel_ids(),
            (std::vector<rclcpp_action::GoalUUID>{accepted->get_goal_id()}));
  EXPECT_TRUE(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp);
  EXPECT_FALSE(rig.adapter->ReadGoalActivity().late_cancel_not_issued);
}

TEST(JtcExecutionPort, RejectedPendingCancelSendsNothing) {
  Rig rig;
  rig.delay_next_response = true;
  rig.reject_next = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kEffectUncertain);
  rig.release_response();
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->result ==
           ExecutionGoalResult::kRejected;
  }));
  EXPECT_TRUE(rig.adapter->ReadGoal(key(1))->mechanism_goal_id.empty());
  EXPECT_EQ(rig.cancel_count.load(), 0U);
}

TEST(JtcExecutionPort, RetiredPendingCancelSendsOnceOnLateAcceptance) {
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kEffectUncertain);
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{1}});
  rig.release_response();
  const auto accepted = rig.handle();
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1U; }));
  EXPECT_EQ(rig.cancel_ids(), (std::vector<rclcpp_action::GoalUUID>{accepted->get_goal_id()}));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result,
            ExecutionGoalResult::kEndedUnknown);
}

TEST(JtcExecutionPort, ShutdownReleasesSdkOwnersWithoutSendingPendingCancel) {
  Rig rig;
  const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kEffectUncertain);
  rig.adapter->shutdown();
  EXPECT_FALSE(JtcExecutionPortTestPeer::owns_sdk_entities(*rig.adapter));
  EXPECT_FALSE(rig.adapter->ReadReadiness().controller_active);
  EXPECT_EQ(rig.adapter->SetScale(0.5).dispatch,
            ExecutionPortDispatch::kNotDispatched);
  rig.release_response();
  (void)rig.handle();
  ASSERT_TRUE(wait_until([&] { return sdk.expired(); }));
  const auto observed = rig.adapter->ReadGoal(key(1));
  ASSERT_TRUE(observed);
  EXPECT_EQ(observed->result, ExecutionGoalResult::kEndedUnknown);
  EXPECT_FALSE(observed->accepted_stamp_ns);
  EXPECT_FALSE(observed->cancel_sent_local_stamp);
  EXPECT_FALSE(observed->cancel_ack_local_receipt_stamp);
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  EXPECT_EQ(rig.scale_count.load(), 0U);
  rig.adapter.reset();
}

TEST(JtcExecutionPort, ShutdownUnregistersClientWhileCollectionIsInProgress) {
  Rig rig;
  auto client = JtcExecutionPortTestPeer::client(*rig.adapter);
  const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
  // The client lives in its own time-critical group (OD-66).
  auto group = JtcExecutionPortTestPeer::client_group(*rig.adapter);
  std::mutex mutex;
  std::condition_variable cv;
  bool entered = false;
  bool release = false;
  bool saw_client = false;
  auto noop = [](const auto &) {};
  std::thread collector([&] {
    group->collect_all_ptrs(noop, noop, noop, noop, [&](const auto &waitable) {
      if (waitable.get() != client.get())
        return;
      std::unique_lock lock(mutex);
      saw_client = true;
      entered = true;
      cv.notify_all();
      cv.wait_for(lock, 3s, [&] { return release; });
    });
  });
  {
    std::unique_lock lock(mutex);
    cv.wait_for(lock, 3s, [&] { return entered; });
  }
  std::thread retire([&] { rig.adapter->shutdown(); });
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  cv.notify_all();
  collector.join();
  retire.join();
  EXPECT_TRUE(saw_client);
  bool still_registered = false;
  group->collect_all_ptrs(noop, noop, noop, noop, [&](const auto &waitable) {
    if (waitable.get() == client.get())
      still_registered = true;
  });
  EXPECT_FALSE(still_registered);
  client.reset();
  ASSERT_TRUE(wait_until([&] { return sdk.expired(); }));
  EXPECT_TRUE(rig.resources.started());
}

TEST(JtcExecutionPort, IdleShutdownReclaimsEntitiesWhileSharedExecutorRuns) {
  Rig rig;
  const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
  // No publisher/controller traffic wakes this node after retirement.
  rig.adapter->shutdown();
  EXPECT_FALSE(JtcExecutionPortTestPeer::owns_sdk_entities(*rig.adapter));
  ASSERT_TRUE(wait_until([&] { return sdk.expired(); }));
  EXPECT_TRUE(rig.resources.started());
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  EXPECT_EQ(rig.scale_count.load(), 0U);
}

TEST(JtcExecutionPort, PendingCancelResponseCannotReviveRetiredFactsOrNewEpoch) {
  Rig rig(true);
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
  rig.delay_next_cancel = true;
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.cancel_blocked.load(); }));
  rig.adapter->shutdown();
  rig.successor->ObserveReadiness({true, true, true, ExecutionBindingEpoch{2}});
  rig.release_response();
  ASSERT_TRUE(wait_until([&] { return sdk.expired(); }));
  const auto old = rig.adapter->ReadGoal(key(1));
  ASSERT_TRUE(old);
  EXPECT_EQ(old->result, ExecutionGoalResult::kEndedUnknown);
  EXPECT_FALSE(old->cancel_ack_local_receipt_stamp);
  EXPECT_FALSE(rig.successor->ReadGoal(key(1)));
  EXPECT_TRUE(rig.successor->ReadReadiness().controller_active);
  EXPECT_EQ(rig.cancel_count.load(), 1U); // Only the cancel sent before closure.
  EXPECT_EQ(rig.scale_count.load(), 0U);
}

TEST(JtcExecutionPort, OwnedShutdownDrainsFiniteFeedbackCallback) {
  Rig rig;
  struct FeedbackState {
    std::atomic<bool> entered{false};
    std::atomic<bool> completed{false};
  };
  auto feedback = std::make_shared<FeedbackState>();
  auto client = JtcExecutionPortTestPeer::client(*rig.adapter);
  const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
  rclcpp_action::Client<Action>::SendGoalOptions options;
  options.feedback_callback = [feedback](auto, auto) {
    feedback->entered = true;
    std::this_thread::sleep_for(200ms);
    feedback->completed = true;
  };
  Action::Goal goal;
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  auto future = client->async_send_goal(goal, options);
  ASSERT_TRUE(wait_until([&] { return future.wait_for(0s) == std::future_status::ready; }));
  ASSERT_TRUE(future.get());
  client.reset();
  rig.handle()->publish_feedback(std::make_shared<Action::Feedback>());
  ASSERT_TRUE(wait_until([&] { return feedback->entered.load(); }));
  rig.adapter->shutdown();
  EXPECT_TRUE(feedback->completed.load());
  EXPECT_FALSE(JtcExecutionPortTestPeer::owns_sdk_entities(*rig.adapter));
  rig.adapter.reset();
  ASSERT_TRUE(wait_until([&] { return sdk.expired(); }));
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  EXPECT_EQ(rig.scale_count.load(), 0U);
}

TEST(JtcExecutionPort, ConcurrentShutdownIsIdempotentAndDoesNotTouchNewEpoch) {
  Rig rig(true);
  const auto old_sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
  rig.successor->ObserveReadiness({true, true, true, ExecutionBindingEpoch{2}});
  std::thread first([&] { rig.adapter->shutdown(); });
  std::thread second([&] { rig.adapter->shutdown(); });
  first.join();
  second.join();
  ASSERT_TRUE(wait_until([&] { return old_sdk.expired(); }));
  EXPECT_TRUE(JtcExecutionPortTestPeer::owns_sdk_entities(*rig.successor));
  EXPECT_TRUE(rig.successor->ReadReadiness().controller_active);
  EXPECT_EQ(rig.adapter->SetScale(0.5).dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  EXPECT_EQ(rig.scale_count.load(), 0U);
}

TEST(JtcExecutionPort, RepeatedBindingsReturnAllSdkEntityCountsToBaseline) {
  Rig rig;
  auto assert_no_entities = [&] {
    return rig.resources.node()->count_subscribers(rig.config(1).state_topic) == 0U &&
           rig.resources.node()->count_publishers(rig.config(1).scale_topic) == 0U &&
           rig.resources.node()->count_subscribers(rig.config(1).action_name + "/_action/feedback") == 0U &&
           rig.resources.node()->count_subscribers(rig.config(1).action_name + "/_action/status") == 0U;
  };
  for (std::uint64_t epoch = 1; epoch <= 20; ++epoch) { // Fixture count, never a product cap.
    const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
    rig.adapter->shutdown();
    rig.adapter.reset();
    ASSERT_TRUE(wait_until([&] { return sdk.expired() && assert_no_entities(); }));
    if (epoch < 20) {
      rig.rebind(epoch + 1);
    }
  }
}

// Run this test under a process-level timeout: a deadlocked SDK shutdown or
// executor join cannot be bounded by the loop deadline inside this process.
TEST(JtcExecutionPort, RepeatedRebindAndTransportShutdownStress) {
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  for (unsigned cycle = 0; cycle < 10U; ++cycle) {
    Rig rig;
    for (std::uint64_t epoch = 1; epoch <= 20; ++epoch) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline);
      const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
      rig.adapter->shutdown();
      rig.adapter.reset();
      ASSERT_TRUE(wait_until([&] { return sdk.expired(); }));
      EXPECT_TRUE(rig.resources.started());
      if (epoch < 20) {
        rig.rebind(epoch + 1);
      }
    }
    rig.resources.quiesce();
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
  }
}

TEST(JtcExecutionPort, QuiescenceDrainsInFlightGoalResponseBeforeContextShutdown) {
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  rig.adapter->shutdown();
  std::atomic<bool> quiesced{false};
  std::thread shutdown([&] {
    rig.resources.quiesce();
    quiesced = true;
  });
  // The SDK goal callback holds the real executor worker. Observe cancellation
  // through the SDK predicate, then release that callback without a delay.
  EXPECT_TRUE(wait_until([&] {
    return !JtcExecutionPortTestPeer::executor_is_spinning(rig.resources);
  }));
  EXPECT_FALSE(quiesced.load());
  EXPECT_TRUE(rig.resources.node()->get_node_base_interface()->get_context()->is_valid());
  rig.release_response();
  shutdown.join();
  EXPECT_TRUE(quiesced.load());
  EXPECT_EQ(rig.goal_count(), 1U);
  EXPECT_TRUE(rig.accepted_callback_context_valid.load());
  EXPECT_FALSE(rig.resources.node()->get_node_base_interface()->get_context()->is_valid());
}

TEST(JtcExecutionPort, ImmediateQuiescenceAfterStartCannotLoseExecutorCancellation) {
  FakeClock clock;
  const auto sink = std::make_shared<Sink>();
  for (unsigned iteration = 0; iteration < 8U; ++iteration) {
    auto resources_owner = Ros2TransportResources::create("jtc_immediate_quiescence_" + std::to_string(iteration),
                                    "", 2, sink, "arm", clock);
    auto &resources = *resources_owner;
    resources.start();
    EXPECT_TRUE(JtcExecutionPortTestPeer::executor_is_spinning(resources));
    resources.quiesce();
    EXPECT_FALSE(JtcExecutionPortTestPeer::executor_is_spinning(resources));
    EXPECT_FALSE(resources.node()->get_node_base_interface()->get_context()->is_valid());
  }
}

TEST(Ros2TransportResources, TransportOnlyRunsCallbacksWithoutSceneProjection) {
  auto resources_owner = Ros2TransportResources::create("generic_io_transport_test",
                                   "/generic_io_test", 2);
  auto &resources = *resources_owner;
  EXPECT_EQ(std::string(resources.node()->get_namespace()), "/generic_io_test");
  EXPECT_THROW(resources.projection_worker(), std::logic_error);
  auto received = std::make_shared<std::promise<void>>();
  auto completed = received->get_future();
  auto notified = std::make_shared<std::atomic<bool>>(false);
  const auto timer =
      resources.node()->create_wall_timer(1ms, [received, notified] {
        if (!notified->exchange(true))
          received->set_value();
      });
  resources.start();
  EXPECT_EQ(completed.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(JtcExecutionPortTestPeer::executor_is_spinning(resources));
  resources.quiesce();
  EXPECT_FALSE(JtcExecutionPortTestPeer::executor_is_spinning(resources));
  EXPECT_FALSE(
      resources.node()->get_node_base_interface()->get_context()->is_valid());
  EXPECT_THROW(resources.start(), std::logic_error);
  resources.quiesce();
}

// A callback exception ends the stock single-threaded owner and publishes the
// existing typed fault exactly once. Entities are created before spin.
TEST(Ros2TransportResources, ExecutorCallbackThrowEndsOwnerWithTypedFault) {
  auto resources_owner = Ros2TransportResources::create("executor_fault_transport_test",
                                   "/executor_fault_test", 3);
  auto &resources = *resources_owner;
  EXPECT_EQ(resources.owner_name(), "/executor_fault_test/executor_fault_transport_test");
  std::atomic<int> observed{0};
  resources.set_executor_fault_observer([&](std::string_view reason) {
    EXPECT_EQ(reason, "ros_transport_executor_failed");
    ++observed;
  });
  EXPECT_FALSE(resources.executor_fault().has_value());
  const auto timer = resources.node()->create_wall_timer(
      1ms, [] { throw std::runtime_error("executor callback failed"); });
  ScopedTransportQuiesce join{resources};
  resources.start();
  ASSERT_TRUE(wait_until([&] { return resources.executor_fault().has_value(); }));
  resources.quiesce();
  EXPECT_EQ(resources.executor_fault(),
            std::optional<std::string>("executor callback failed"));
  EXPECT_FALSE(JtcExecutionPortTestPeer::executor_is_spinning(resources));
  EXPECT_EQ(observed.load(), 1);
}

// A fault remains contained in each independently owned transport.
TEST(Ros2TransportResources, ExecutorFaultIsContainedInIndependentOwners) {
  for (int cycle = 0; cycle < 3; ++cycle) {
    auto resources_owner = Ros2TransportResources::create("executor_fault_repeat_test",
                                     "/executor_fault_test", 3);
    auto &resources = *resources_owner;
    std::atomic<int> observed{0};
    resources.set_executor_fault_observer([&](std::string_view) { ++observed; });
    const auto timer = resources.node()->create_wall_timer(
        1ms, [] { throw std::runtime_error("executor wait failed"); });
    ScopedTransportQuiesce join{resources};
    resources.start();
    ASSERT_TRUE(wait_until([&] { return resources.executor_fault().has_value(); }));
    resources.quiesce();
    EXPECT_FALSE(JtcExecutionPortTestPeer::executor_is_spinning(resources));
    EXPECT_EQ(resources.executor_fault(),
              std::optional<std::string>("executor wait failed"));
    EXPECT_EQ(observed.load(), 1);
  }
}

TEST(Ros2TransportResources, TransportOnlyCanQuiesceBeforeStart) {
  auto resources_owner = Ros2TransportResources::create("generic_io_unstarted_transport_test", "",
                                   2);
  auto &resources = *resources_owner;
  EXPECT_FALSE(resources.started());
  resources.quiesce();
  EXPECT_FALSE(
      resources.node()->get_node_base_interface()->get_context()->is_valid());
  EXPECT_THROW(resources.start(), std::logic_error);
  resources.quiesce();
}

TEST(Ros2TransportResources, SuppliedContextSurvivesLocalQuiescenceAndRetainedNode) {
  std::weak_ptr<rclcpp::Context> lifetime;
  std::shared_ptr<rclcpp::Node> retained_node;
  {
    auto context = std::make_shared<rclcpp::Context>();
    int argc = 0;
    context->init(argc, nullptr);
    lifetime = context;
    unsigned provider_calls = 0;
    Ros2TransportResources::ContextProvider provider = [&] {
      ++provider_calls;
      return context;
    };
    auto first = Ros2TransportResources::create(
        "shared_context_first", "", 2, provider);
    auto second_owner = Ros2TransportResources::create("shared_context_second", "", 2, provider);
    auto &second = *second_owner;
    EXPECT_EQ(provider_calls, 2U);
    EXPECT_NE(first->node(), second.node());
    EXPECT_EQ(first->node()->get_node_base_interface()->get_context(), context);
    EXPECT_EQ(second.node()->get_node_base_interface()->get_context(), context);
    auto publisher = first->node()->create_publisher<sensor_msgs::msg::JointState>(
        "/shared_context_lifetime_samples", 1);
    auto received = std::make_shared<std::atomic<unsigned>>(0U);
    const auto subscription = second.node()->create_subscription<sensor_msgs::msg::JointState>(
        "/shared_context_lifetime_samples", 1,
        [received](sensor_msgs::msg::JointState::ConstSharedPtr) { ++*received; });
    first->start();
    second.start();
    ASSERT_TRUE(wait_until([&] { return publisher->get_subscription_count() == 1U; }));
    publisher->publish(sensor_msgs::msg::JointState{});
    ASSERT_TRUE(wait_until([&] { return received->load() == 1U; }));
    first->quiesce();
    first.reset();
    EXPECT_TRUE(context->is_valid());
    EXPECT_TRUE(JtcExecutionPortTestPeer::executor_is_spinning(second));
    publisher->publish(sensor_msgs::msg::JointState{});
    ASSERT_TRUE(wait_until([&] { return received->load() == 2U; }));
    retained_node = second.node();
    provider = {};
    context.reset();
    second.quiesce();
    EXPECT_EQ(provider_calls, 2U);
    EXPECT_TRUE(retained_node->get_node_base_interface()->get_context()->is_valid());
  }
  ASSERT_FALSE(lifetime.expired());
  EXPECT_TRUE(lifetime.lock()->is_valid());
  retained_node.reset();
  EXPECT_TRUE(lifetime.expired());
}

TEST(Ros2TransportResources, SuppliedContextMustAlreadyBeValid) {
  auto context = std::make_shared<rclcpp::Context>();
  EXPECT_THROW((Ros2TransportResources::create("uninitialized_context", "", 2,
                                      [context] { return context; })),
               std::invalid_argument);
  EXPECT_THROW((Ros2TransportResources::create("null_context", "", 2,
                                      [] { return std::shared_ptr<rclcpp::Context>{}; })),
               std::invalid_argument);
  int argc = 0;
  context->init(argc, nullptr);
  context->shutdown("test invalid supplied context");
  EXPECT_THROW((Ros2TransportResources::create("shutdown_context", "", 2,
                                      [context] { return context; })),
               std::invalid_argument);
}

TEST(Ros2TransportResources, DefaultPrivateContextsRemainIndependent) {
  auto first_owner = Ros2TransportResources::create("private_context_first", "", 2);
  auto &first = *first_owner;
  auto second_owner = Ros2TransportResources::create("private_context_second", "", 2);
  auto &second = *second_owner;
  const auto first_context = first.node()->get_node_base_interface()->get_context();
  const auto second_context = second.node()->get_node_base_interface()->get_context();
  EXPECT_NE(first_context, second_context);
  first.quiesce();
  EXPECT_FALSE(first_context->is_valid());
  EXPECT_TRUE(second_context->is_valid());
  second.quiesce();
  EXPECT_FALSE(second_context->is_valid());
}

// Readiness-loss cancels no longer hop through the epoch owner, so the
// backend installs no owner notification.
TEST(JtcExecutionPort, BackendInstallsNoOwnerNotification) {
  Rig rig;
  auto resources = rig.make_epoch();
  Ros2ControlExecutionBackend adapter(resources, rig.clock_owner, rig.scheduler, rig.config(3));
  EXPECT_NO_THROW(resources->set_owner_notification([] {}));
  EXPECT_TRUE(adapter.shutdown());
}

// The fake controller has its own executor; only the adapter is on the
// dedicated epoch owner, as in the production split.
struct DedicatedEpochRig {
  Rig server;
  std::shared_ptr<Ros2TransportResources> resources =
      Ros2TransportResources::create("jtc_dedicated_epoch", "", 1);
  std::shared_ptr<Ros2ControlExecutionBackend> adapter =
      std::make_shared<Ros2ControlExecutionBackend>(resources, server.clock_owner, server.scheduler,
                                                     server.config(2));

  DedicatedEpochRig() {
    adapter->ObserveReadiness({true, true, true, ExecutionBindingEpoch{2}});
  }
  ~DedicatedEpochRig() {
    adapter->close_admission();
    resources->quiesce();
    adapter->release_ros_entities();
  }
  void start() { resources->start(); }
};

TEST(JtcExecutionPort, DedicatedEpochSendsAndReceivesFeedbackAndResult) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ASSERT_EQ(rig.adapter->Submit(key(1), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.server.accepted(key(1), rig.adapter.get()));
  auto feedback = std::make_shared<Action::Feedback>();
  feedback->joint_names = {"b", "a"};
  feedback->desired.positions = {0.2, 0.1};
  feedback->desired.velocities = {2.0, 1.0};
  feedback->desired.accelerations = {20.0, 10.0};
  feedback->actual.positions = {0.19, 0.09};
  rig.server.handle()->publish_feedback(feedback);
  ASSERT_TRUE(wait_until([&] {
    const auto goal = rig.adapter->ReadGoal(key(1));
    return goal && goal->latest_feedback.has_value();
  }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->latest_feedback->desired_positions_rad,
            (std::vector<double>{0.1, 0.2}));
  auto result = std::make_shared<Action::Result>();
  result->error_code = Action::Result::SUCCESSFUL;
  rig.server.handle()->succeed(result);
  ASSERT_TRUE(wait_until([&] {
    const auto goal = rig.adapter->ReadGoal(key(1));
    return goal && goal->result == ExecutionGoalResult::kSucceeded;
  }));
}

// OD-66: controller state runs on the epoch's time-critical thread, so an
// ordinary owner task blocked on a latch does not delay it.
TEST(JtcExecutionPort, DedicatedEpochStateArrivesWhileOwnerTaskBlocks) {
  std::atomic<bool> released{false};
  std::promise<void> entered;
  auto in_task = entered.get_future();
  DedicatedEpochRig rig;
  std::future<void> task;
  struct Release final {
    std::atomic<bool> &flag;
    ~Release() { flag.store(true); }
  } release{released};
  rig.start();
  const auto received = [&](std::uint32_t nanosecond) {
    const auto sample = rig.adapter->ReadSample();
    return sample && sample->source_stamp_ns == 10'000'000'000 + nanosecond;
  };
  // Prove the epoch's subscription is matched before blocking its owner.
  bool matched = false;
  for (std::uint32_t attempt = 1; attempt <= 20 && !matched; ++attempt) {
    rig.server.state(0.25, attempt);
    matched = wait_until([&] { return received(attempt); });
  }
  ASSERT_TRUE(matched);
  // Shorter than the owner stall bound.
  task = std::async(std::launch::async, [&] {
    rig.resources->invoke([&] {
      entered.set_value();
      while (!released.load()) std::this_thread::sleep_for(1ms);
    });
  });
  ASSERT_EQ(in_task.wait_for(2s), std::future_status::ready);
  rig.server.state(0.25, 777U);
  EXPECT_TRUE(wait_until([&] { return received(777U); }));
  EXPECT_FALSE(released.load());  // Arrived while the owner thread was blocked.
  released.store(true);
  task.get();
}

TEST(JtcExecutionPort, DedicatedEpochDefersCancelUntilAcceptance) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ExecutionPortCommandResultDTO sent;
  ExecutionPortCommandResultDTO pending;
  // Both calls run in one owner callback. The goal response callback cannot
  // run on that owner until this callback returns, without blocking any thread.
  rig.resources->invoke([&] {
    sent = rig.adapter->Submit(key(1), trajectory());
    pending = rig.adapter->Cancel(key(1));
  });
  ASSERT_EQ(sent.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_EQ(pending.dispatch, ExecutionPortDispatch::kEffectUncertain);
  ASSERT_TRUE(wait_until([&] {
    const auto goal = rig.adapter->ReadGoal(key(1));
    return goal && goal->cancel_ack_local_receipt_stamp.has_value();
  }));
  EXPECT_EQ(rig.server.cancel_count.load(), 1U);
}

TEST(JtcExecutionPort, DedicatedEpochReadinessLossCancelsAcceptedGoalOnce) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ASSERT_EQ(rig.adapter->Submit(key(1), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.server.accepted(key(1), rig.adapter.get()));
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{2}});
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{2}});
  ASSERT_TRUE(wait_until([&] { return rig.server.cancel_count.load() == 1U; }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result,
            ExecutionGoalResult::kEndedUnknown);
  rig.adapter->close_admission();
  EXPECT_EQ(rig.server.cancel_count.load(), 1U);
}

TEST(JtcExecutionPort, FaultRebuildRetiresAcceptedGoalWithoutCancel) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ASSERT_EQ(rig.adapter->Submit(key(1), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.server.accepted(key(1), rig.adapter.get()));
  rig.adapter->close_admission_without_cancel();
  rig.resources->quiesce();
  rig.adapter->release_ros_entities();
  const auto retained = rig.adapter->ReadGoal(key(1));
  ASSERT_TRUE(retained.has_value());
  EXPECT_EQ(retained->result, ExecutionGoalResult::kEndedUnknown);
  EXPECT_EQ(rig.server.cancel_count.load(), 0U);
  EXPECT_EQ(rig.adapter->Submit(key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kNotDispatched);
}

TEST(JtcExecutionPort, DedicatedEpochExplicitCancelCanBeRepeated) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ASSERT_EQ(rig.adapter->Submit(key(1), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.server.accepted(key(1), rig.adapter.get()));
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
}

TEST(JtcExecutionPort, DedicatedEpochCloseDrainsBothExactLossCancels) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ASSERT_EQ(rig.adapter->Submit(key(1), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.server.accepted(key(1), rig.adapter.get()));
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.server.accepted(key(2), rig.adapter.get()));
  const auto first = rig.server.handle(0)->get_goal_id();
  const auto second = rig.server.handle(1)->get_goal_id();
  rig.resources->invoke([&] {
    rig.adapter->ObserveReadiness(
        {false, true, true, ExecutionBindingEpoch{2}});
    rig.adapter->close_admission();
  });
  ASSERT_TRUE(wait_until([&] { return rig.server.cancel_count.load() == 2U; }));
  const auto ids = rig.server.cancel_ids();
  ASSERT_EQ(ids.size(), 2U);
  EXPECT_NE(std::find(ids.begin(), ids.end(), first), ids.end());
  EXPECT_NE(std::find(ids.begin(), ids.end(), second), ids.end());
}

TEST(JtcExecutionPort, DedicatedEpochLateAcceptanceAfterLossCancelsOnce) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ExecutionPortCommandResultDTO submitted;
  ExecutionPortCommandResultDTO pending;
  // The response is delivered on a later owner turn; no executor is blocked.
  rig.resources->invoke([&] {
    submitted = rig.adapter->Submit(key(1), trajectory());
    pending = rig.adapter->Cancel(key(1));
    rig.adapter->ObserveReadiness(
        {false, true, true, ExecutionBindingEpoch{2}});
  });
  ASSERT_EQ(submitted.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_EQ(pending.dispatch, ExecutionPortDispatch::kEffectUncertain);
  ASSERT_TRUE(wait_until([&] { return rig.server.cancel_count.load() == 1U; }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result,
            ExecutionGoalResult::kEndedUnknown);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->accepted_stamp_ns);
  rig.adapter->close_admission();
  EXPECT_EQ(rig.server.cancel_count.load(), 1U);
}

TEST(JtcExecutionPort,
     DedicatedEpochLateAcceptanceWithoutPriorCancelIsContained) {
  DedicatedEpochRig rig;
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  ExecutionPortCommandResultDTO submitted;
  rig.resources->invoke([&] {
    submitted = rig.adapter->Submit(key(1), trajectory());
    rig.adapter->ObserveReadiness(
        {false, true, true, ExecutionBindingEpoch{2}});
  });
  ASSERT_EQ(submitted.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.server.cancel_count.load() == 1U; }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result,
            ExecutionGoalResult::kEndedUnknown);
}

TEST(JtcExecutionPort, DedicatedEpochReadinessDoesNotWaitForUnrelatedTask) {
  DedicatedEpochRig rig;
  rig.start();
  std::atomic<bool> running{false};
  std::jthread caller([&] {
    rig.resources->invoke([&] {
      running.store(true);
      std::this_thread::sleep_for(2s);
      running.store(false);
    });
  });
  ASSERT_TRUE(wait_until([&] { return running.load(); }));
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{2}});
  const bool returned_during_task = running.load();
  caller.join();
  EXPECT_TRUE(returned_during_task);
}

TEST(JtcExecutionPort, DedicatedEpochFencesSendsAfterOwnerFault) {
  DedicatedEpochRig rig;
  std::atomic<bool> request_fault{false};
  const auto timer = rig.resources->node()->create_wall_timer(1ms, [&] {
    if (request_fault.load())
      throw std::runtime_error("dedicated epoch test fault");
  });
  // Stop the owner before the timer and its captured flag leave scope, also
  // on an early assertion return.
  struct JoinBeforeTimerDestruction {
    Ros2TransportResources &resources;
    ~JoinBeforeTimerDestruction() { resources.quiesce(); }
  } join_before_timer_destruction{*rig.resources};
  rig.start();
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  request_fault = true;
  ASSERT_TRUE(wait_until([&] { return rig.resources->executor_fault().has_value(); }));
  EXPECT_EQ(rig.resources->executor_fault(),
            std::optional<std::string>("dedicated epoch test fault"));
  // A faulted owner is a component fault: no further goal is sent.
  const auto after_fault = rig.adapter->Submit(key(1), trajectory());
  EXPECT_EQ(after_fault.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(after_fault.refusal, ExecutionPortRefusal::kStaleKey);
  rig.adapter->close_admission();
  EXPECT_EQ(rig.adapter->Submit(key(1), trajectory()).refusal,
            ExecutionPortRefusal::kNotReady);
  EXPECT_EQ(rig.adapter->SetScale(0.5).refusal, ExecutionPortRefusal::kNotReady);
}

TEST(JtcExecutionPort, DedicatedEpochClosesBeforeReleaseAndRetainsClosedResults) {
  Rig rig;
  auto resources = Ros2TransportResources::create(
      "jtc_dedicated_epoch_test", "", 1);
  auto adapter = std::make_shared<Ros2ControlExecutionBackend>(
      resources, rig.clock_owner, rig.scheduler, rig.config(2));
  adapter->ObserveReadiness({true, true, true, ExecutionBindingEpoch{2}});
  resources->start();
  adapter->close_admission();
  EXPECT_FALSE(adapter->ReadReadiness().controller_active);
  EXPECT_FALSE(adapter->ReadSample());
  EXPECT_EQ(adapter->SetScale(2.0).refusal, ExecutionPortRefusal::kInvalidInput);
  EXPECT_EQ(adapter->SetScale(0.5).refusal, ExecutionPortRefusal::kNotReady);
  resources->quiesce();
  adapter->release_ros_entities();
  adapter->release_ros_entities();
  resources.reset(); // The retained adapter owns the stopped epoch resources.
  EXPECT_EQ(adapter->SetScale(0.5).refusal, ExecutionPortRefusal::kNotReady);
  EXPECT_FALSE(adapter->ActionServerIsReady());
}

TEST(JtcExecutionPort, ShutdownAfterContextStoppedNeedsNoNotificationOrCommands) {
  Rig rig;
  const auto sdk = JtcExecutionPortTestPeer::sdk_owners(*rig.adapter);
  rig.epoch_resources->quiesce();
  rig.adapter->shutdown();
  rig.adapter->shutdown();
  EXPECT_FALSE(JtcExecutionPortTestPeer::owns_sdk_entities(*rig.adapter));
  EXPECT_TRUE(sdk.expired());
  EXPECT_EQ(rig.cancel_count.load(), 0U);
  EXPECT_EQ(rig.scale_count.load(), 0U);
}

TEST(JtcExecutionPort, CancelAcknowledgementDoesNotEndGoalOrManufactureRest) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))
        ->cancel_ack_local_receipt_stamp.has_value();
  }));
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp,
            TimePoint{42});
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->result);
  EXPECT_FALSE(rig.adapter->ReadSample());
  ASSERT_TRUE(wait_until([&] { return rig.handle()->is_canceling(); }));
  rig.handle()->canceled(std::make_shared<Action::Result>());
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->result ==
           ExecutionGoalResult::kCanceled;
  }));
  EXPECT_FALSE(rig.adapter->ReadSample());
}

TEST(JtcExecutionPort, ScheduledReplacementSerializesStampAndUnchangedPastPva) {
  Rig rig(false, true);
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  auto past = trajectory();
  past.waypoints = {{-0.125, {0.0, 0.0}, {0.2, 0.1}, {0.3, 0.4}},
                    {-0.01, {0.02, 0.01}, {0.2, 0.1}, {0.3, 0.4}},
                    {0.2, {0.04, 0.03}, {0.0, 0.0}, {0.0, 0.0}}};
  const ExecutionReplacementStartDTO start{ExecutionBindingEpoch(1U),
                                           1'234'567'890'123LL};
  ASSERT_EQ(rig.adapter->ReplaceScheduled(key(1), key(2), past, start).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  const auto wire = rig.handle(1)->get_goal()->trajectory;
  EXPECT_EQ(wire.header.stamp.sec, 1234);
  EXPECT_EQ(wire.header.stamp.nanosec, 567'890'123U);
  EXPECT_EQ(wire.joint_names, (std::vector<std::string>{"a", "b"}));
  ASSERT_EQ(wire.points.size(), past.waypoints.size());
  for (std::size_t index = 0; index < wire.points.size(); ++index) {
    const auto &actual = wire.points[index];
    const auto &expected = past.waypoints[index];
    const auto nanoseconds =
        static_cast<std::int64_t>(actual.time_from_start.sec) * 1'000'000'000 +
        actual.time_from_start.nanosec;
    EXPECT_EQ(nanoseconds, static_cast<std::int64_t>(
                               std::llround(expected.time_from_start_s * 1e9)));
    EXPECT_EQ(actual.positions,
              (std::vector<double>{expected.positions_rad[1],
                                   expected.positions_rad[0]}));
    EXPECT_EQ(actual.velocities,
              (std::vector<double>{expected.velocities_rad_s[1],
                                   expected.velocities_rad_s[0]}));
    EXPECT_EQ(actual.accelerations,
              (std::vector<double>{expected.accelerations_rad_s2[1],
                                   expected.accelerations_rad_s2[0]}));
  }
  EXPECT_EQ(rig.adapter->ReplaceScheduled(key(1), key(3), past, start).refusal,
            ExecutionPortRefusal::kStaleKey);
}

TEST(JtcExecutionPort, ExperimentRecordIsDiagnosticOnlyAndLeavesTheWireUnchanged) {
  // EXPERIMENTAL: Core holds before the send; the port never waits. A start
  // carrying an experiment record sends at once with the same header stamp.
  Rig rig(false, true);
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ExecutionReplacementStartDTO start{ExecutionBindingEpoch(1U), 1'234'567'890'123LL};
  auto& experiment = start.experiment.emplace();
  experiment.source_stamp_ns = 1'234'560'000'000LL;
  experiment.target_tick_ns = 1'234'567'890'123LL;
  experiment.release_local_ns = 42 + 3'600'000'000'000LL;  // far future: never waited for
  experiment.release_hold = ExecutionReleaseHold::kHeld;
  experiment.factor = 0.5;
  const auto before = std::chrono::steady_clock::now();
  ASSERT_EQ(rig.adapter->ReplaceScheduled(key(1), key(2), trajectory(), start).dispatch,
            ExecutionPortDispatch::kDispatched);
  EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(60));
  ASSERT_TRUE(rig.accepted(key(2)));
  const auto wire = rig.handle(1)->get_goal()->trajectory;
  EXPECT_EQ(wire.header.stamp.sec, 1234);
  EXPECT_EQ(wire.header.stamp.nanosec, 567'890'123U);
}

TEST(JtcExecutionPort, AllowanceOverrideWithoutTimerLogsItsValueAndPreservesTheWire) {
  for (const Duration allowance : {20'000'000, 30'000'000, 40'000'000}) {
    Rig rig(false, true);
    ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
    ASSERT_TRUE(rig.accepted(key(1)));
    ExecutionReplacementStartDTO start{ExecutionBindingEpoch(1U), 1'234'567'890'123LL};
    auto &experiment = start.experiment.emplace();
    experiment.start_allowance_override = true;
    experiment.start_allowance_ns = allowance;
    testing::internal::CaptureStderr();
    const auto sent = rig.adapter->ReplaceScheduled(key(1), key(2), trajectory(), start);
    const auto log = testing::internal::GetCapturedStderr();
    ASSERT_EQ(sent.dispatch, ExecutionPortDispatch::kDispatched);
    EXPECT_NE(log.find("release_policy=immediate"), std::string::npos);
    EXPECT_NE(log.find("release_hold=none"), std::string::npos);
    EXPECT_NE(log.find("start_allowance_ns=" + std::to_string(allowance) +
                       " allowance_override=1"), std::string::npos);
    ASSERT_TRUE(rig.accepted(key(2)));
    const auto wire = rig.handle(1)->get_goal()->trajectory;
    EXPECT_EQ(wire.header.stamp.sec, 1234);
    EXPECT_EQ(wire.header.stamp.nanosec, 567'890'123U);
    EXPECT_EQ(wire.points, rig.handle(0)->get_goal()->trajectory.points);
  }
}

TEST(JtcExecutionPort,
     ScheduledReplacementRefusesForeignEpochAndInvalidRosStampBeforeEffect) {
  Rig rig(false, true);
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto foreign = rig.adapter->ReplaceScheduled(
      key(1), key(2), trajectory(), {ExecutionBindingEpoch(2U), 1});
  EXPECT_EQ(foreign.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(foreign.refusal, ExecutionPortRefusal::kStaleKey);
  for (const TimePoint stamp :
       {TimePoint{0}, TimePoint{-1},
        (static_cast<TimePoint>(std::numeric_limits<std::int32_t>::max()) + 1) *
            1'000'000'000}) {
    const auto result = rig.adapter->ReplaceScheduled(
        key(1), key(2), trajectory(), {ExecutionBindingEpoch(1U), stamp});
    EXPECT_EQ(result.dispatch, ExecutionPortDispatch::kNotDispatched);
    EXPECT_EQ(result.refusal, ExecutionPortRefusal::kInvalidInput);
  }
  // The client can observe the goal response before the fake server's
  // accept callback has run: they are served by different owner executors.
  ASSERT_TRUE(wait_until([&] { return rig.goal_count() == 1U; }));
  EXPECT_FALSE(rig.adapter->ReadGoal(key(2)));
  // Refusals do not reserve the successor key or alter the current goal.
  ASSERT_EQ(rig.adapter
                ->ReplaceScheduled(key(1), key(2), trajectory(),
                                   {ExecutionBindingEpoch(1U), 999'999'999})
                .dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
}

TEST(JtcExecutionPort,
     UnqualifiedLaneRefusesSchedulingAndKeepsLegacyReplacement) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto result = rig.adapter->ReplaceScheduled(
      key(1), key(2), trajectory(), {ExecutionBindingEpoch(1U), 1});
  EXPECT_EQ(result.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(result.refusal, ExecutionPortRefusal::kUnsupported);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(2)));
  EXPECT_EQ(rig.goal_count(), 1U);
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  const auto wire = rig.handle(1)->get_goal()->trajectory;
  EXPECT_EQ(wire.header.stamp.sec, 0);
  EXPECT_EQ(wire.header.stamp.nanosec, 0U);
}

TEST(JtcExecutionPort, ReplacementPreservesMultiplePastPointsAndPvaOnWire) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  auto past = trajectory();
  past.waypoints = {{-0.125, {0.0, 0.0}, {0.2, 0.1}, {0.3, 0.4}},
                    {-0.01, {0.02, 0.01}, {0.2, 0.1}, {0.3, 0.4}},
                    {0.2, {0.04, 0.03}, {0.0, 0.0}, {0.0, 0.0}}};
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), past).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  const auto wire = rig.handle(1)->get_goal()->trajectory;
  EXPECT_EQ(wire.header.stamp.sec, 0);
  EXPECT_EQ(wire.header.stamp.nanosec, 0U);
  ASSERT_EQ(wire.points.size(), 3U);
  EXPECT_EQ(wire.points[0].time_from_start.sec, -1);
  EXPECT_EQ(wire.points[0].time_from_start.nanosec, 875'000'000U);
  EXPECT_EQ(wire.points[1].time_from_start.sec, -1);
  EXPECT_EQ(wire.points[1].time_from_start.nanosec, 990'000'000U);
  EXPECT_EQ(wire.points[0].velocities, (std::vector<double>{0.1, 0.2}));
  EXPECT_EQ(wire.points[0].accelerations, (std::vector<double>{0.4, 0.3}));
  EXPECT_EQ(rig.adapter->Replace(key(1), key(3), past).refusal,
            ExecutionPortRefusal::kStaleKey);
}

TEST(JtcExecutionPort,
     ScaleSendDoesNotClaimAppliedAndSamplesFollowControllerOrder) {
  Rig rig;
  rig.state();
  ASSERT_EQ(rig.adapter->SetScale(0.5).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.scale_count.load() > 0; }));
  EXPECT_EQ(rig.last_scale.load(), 0.5);
  const auto sample = rig.adapter->ReadSample();
  ASSERT_TRUE(sample);
  EXPECT_EQ(sample->applied_factor, 0.25);
  EXPECT_EQ(sample->reference_positions_rad, (std::vector<double>{0.1, 0.2}));
  EXPECT_EQ(sample->reference_velocities_rad_s,
            (std::vector<double>{0.01, 0.02}));
  EXPECT_FALSE(sample->command_velocities_rad_s);
  ASSERT_TRUE(sample->measured_velocities_rad_s);
  EXPECT_EQ(*sample->measured_velocities_rad_s,
            (std::vector<double>{0.04, 0.03}));
  EXPECT_EQ(rig.adapter->SetScale(2).refusal,
            ExecutionPortRefusal::kInvalidInput);
  rig.state(0.5, 20'000'000);
  const auto timing = rig.adapter->ReadTiming();
  ASSERT_TRUE(timing.max_state_stamp_gap_s);
  EXPECT_NEAR(*timing.max_state_stamp_gap_s, 0.02, 1e-10);
  EXPECT_EQ(timing.received_samples, 2U);
  EXPECT_FALSE(timing.period_mean_s);
  EXPECT_FALSE(rig.adapter->ReadTiming().received_samples);
}

TEST(JtcExecutionPort, LaterActionDiscoveryLossDoesNotChangePortReadiness) {
  Rig rig;
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  const auto before = rig.adapter->ReadReadiness();
  ASSERT_TRUE(before.controller_bound);
  ASSERT_TRUE(before.controller_active);
  ASSERT_TRUE(before.hardware_active);
  rig.remove_action_server();
  ASSERT_TRUE(wait_until([&] { return !rig.adapter->ActionServerIsReady(); }));
  const auto after = rig.adapter->ReadReadiness();
  EXPECT_EQ(after, before);
}

TEST(JtcExecutionPort, FencedDispatchTokenIsKnownUnsent) {
  Rig rig;
  // Discovery precedes the fence check; wait so the fence is what refuses.
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  auto token = std::make_shared<ExecutionDispatchToken>();
  EXPECT_EQ(token->Fence(), ExecutionDispatchToken::State::kFenced);
  const auto result = rig.adapter->Submit(key(1), trajectory(), token);
  EXPECT_EQ(result.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(result.refusal, ExecutionPortRefusal::kStaleKey);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1)));
  EXPECT_EQ(rig.goal_count(), 0U);
}

TEST(JtcExecutionPort, ScalePublishDoesNotWaitForOwnerCallback) {
  using namespace std::chrono_literals;
  Rig rig;
  std::promise<void> entered, release;
  auto ready = entered.get_future();
  auto gate = release.get_future().share();
  auto owner = std::async(std::launch::async, [&] {
    rig.epoch_resources->invoke([&] { entered.set_value(); gate.wait(); });
  });
  if (ready.wait_for(3s) != std::future_status::ready) {
    release.set_value();
    owner.get();
    FAIL() << "owner callback did not start";
    return;
  }
  auto scale = std::async(std::launch::async, [&] { return rig.adapter->SetScale(0.25); });
  const auto completed = scale.wait_for(500ms);
  release.set_value();
  owner.get();
  const auto result = scale.get();
  EXPECT_EQ(completed, std::future_status::ready);
  EXPECT_EQ(result.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.scale_count.load() > 0; }));
  EXPECT_DOUBLE_EQ(rig.last_scale.load(), 0.25);
}

TEST(JtcExecutionPort, NewerScaleReservationFencesOlderAndSnapshotKeepsPublishedValue) {
  Rig rig;
  const auto initial = rig.adapter->SnapshotScaleAfterFence(
      std::chrono::steady_clock::now() + std::chrono::seconds(1));
  EXPECT_EQ(initial.status, ScaleSnapshot::Status::kNoPriorFactor);
  const auto older = rig.adapter->ReserveScaleSequence();
  const auto newer = rig.adapter->ReserveScaleSequence();
  ASSERT_EQ(rig.adapter->SetScale(0.8, newer).dispatch, ExecutionPortDispatch::kDispatched);
  const auto stale = rig.adapter->SetScale(0.3, older);
  EXPECT_EQ(stale.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(stale.refusal, ExecutionPortRefusal::kStaleKey);
  ASSERT_TRUE(wait_until([&] { return rig.scale_count.load() > 0; }));
  EXPECT_DOUBLE_EQ(rig.last_scale.load(), 0.8);
  (void)rig.adapter->ReserveScaleSequence(); // Freeze fences any older reservation.
  const auto snapshot = rig.adapter->SnapshotScaleAfterFence(
      std::chrono::steady_clock::now() + std::chrono::seconds(1));
  ASSERT_EQ(snapshot.status, ScaleSnapshot::Status::kFactor);
  EXPECT_DOUBLE_EQ(snapshot.factor, 0.8);
}

TEST(JtcExecutionPort, BindingLossRequestsCancelAndRejectsLateTerminal) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{9}});
  EXPECT_TRUE(rig.adapter->ReadReadiness().controller_active);
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{1}});
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result,
            ExecutionGoalResult::kEndedUnknown);
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1U; }));
  auto result = std::make_shared<Action::Result>();
  const auto retired = rig.handle();
  ASSERT_TRUE(wait_until([&] { return retired->is_canceling(); }));
  retired->canceled(result);
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result,
            ExecutionGoalResult::kEndedUnknown);
}

TEST(JtcExecutionPort, ReadinessRecoveryRetainsUnknownGoalRecordsAtCapacity) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));

  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{1}});
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 2U; }));
  for (const auto &goal_key : {key(1), key(2)}) {
    const auto goal = rig.adapter->ReadGoal(goal_key);
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->result, ExecutionGoalResult::kEndedUnknown);
  }
  rig.adapter->ObserveReadiness({true, true, true, ExecutionBindingEpoch{1}});
  const auto refused = rig.adapter->Submit(key(3), trajectory());
  EXPECT_EQ(refused.dispatch, ExecutionPortDispatch::kNotDispatched);
  EXPECT_EQ(refused.refusal, ExecutionPortRefusal::kNotReady);
  for (const auto &goal_key : {key(1), key(2)}) {
    const auto goal = rig.adapter->ReadGoal(goal_key);
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->result, ExecutionGoalResult::kEndedUnknown);
  }
}

TEST(JtcExecutionPort, OldCallbacksCannotCompleteNewBindingWithReusedKey) {
  Rig rig(true);
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto old = rig.handle();
  rig.adapter.reset();
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1U; }));
  ASSERT_TRUE(wait_until([&] { return old->is_canceling(); }));
  rig.successor->ObserveReadiness({true, true, true, ExecutionBindingEpoch{2}});
  ExecutionPortCommandResultDTO dispatched;
  ASSERT_TRUE(wait_until([&] {
    dispatched = rig.successor->Submit(key(1), trajectory());
    return dispatched.dispatch != ExecutionPortDispatch::kNotDispatched ||
           dispatched.refusal != ExecutionPortRefusal::kNotReady;
  }));
  ASSERT_EQ(dispatched.dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1), rig.successor.get()));
  const auto current = rig.successor->ReadGoal(key(1));
  ASSERT_TRUE(current);
  auto result = std::make_shared<Action::Result>();
  result->error_code = Action::Result::SUCCESSFUL;
  old->canceled(result);
  EXPECT_EQ(rig.successor->ReadGoal(key(1))->binding_epoch,
            ExecutionBindingEpoch{2});
  EXPECT_FALSE(rig.successor->ReadGoal(key(1))->result);
  rig.handle(1)->succeed(result);
  ASSERT_TRUE(wait_until([&] {
    return rig.successor->ReadGoal(key(1))->result ==
           ExecutionGoalResult::kSucceeded;
  }));
}

TEST(JtcExecutionPort, ActionRefusalIsGoalScopedAndNeverSucceeded) {
  Rig rig;
  rig.reject_next = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->result ==
           ExecutionGoalResult::kRejected;
  }));
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->accepted_stamp_ns);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(9)));
}

TEST(JtcExecutionPort,
     RejectedReplacementLeavesPredecessorCurrentAndCancelable) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  rig.reject_next = true;
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(2))->result ==
           ExecutionGoalResult::kRejected;
  }));
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->result);
  EXPECT_EQ(rig.adapter->Cancel(key(2)).refusal,
            ExecutionPortRefusal::kStaleKey);
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1; }));
}

TEST(JtcExecutionPort, CancelDoesNotReachARecordDisconnectedByTerminalEviction) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  auto result = std::make_shared<Action::Result>();
  result->error_code = Action::Result::SUCCESSFUL;
  rig.handle(1)->succeed(result);
  ASSERT_TRUE(wait_until([&] {
    const auto observation = rig.adapter->ReadGoal(key(2));
    return observation && observation->result == ExecutionGoalResult::kSucceeded;
  }));
  ASSERT_FALSE(rig.adapter->ReadGoal(key(1))->result);
  // Making room evicts the terminal current record. The still-live first
  // record is no longer a predecessor of this new goal.
  ASSERT_EQ(rig.submit(key(3)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(3)));
  ASSERT_EQ(rig.adapter->Cancel(key(3)).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1U; }));
  ASSERT_TRUE(wait_until([&] { return rig.handle(2)->is_canceling(); }));
  EXPECT_FALSE(rig.handle()->is_canceling());
}

TEST(JtcExecutionPort,
     ReportsControllerPathAndGoalToleranceAbortsAsDistinctFacts) {
  for (const auto &[code, expected] :
       {std::pair{Action::Result::PATH_TOLERANCE_VIOLATED,
                  ExecutionGoalResult::kAbortedPathTolerance},
        std::pair{Action::Result::GOAL_TOLERANCE_VIOLATED,
                  ExecutionGoalResult::kAbortedGoalTolerance}}) {
    Rig rig;
    ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
    ASSERT_TRUE(rig.accepted(key(1)));
    auto result = std::make_shared<Action::Result>();
    result->error_code = code;
    rig.handle()->abort(result);
    ASSERT_TRUE(wait_until(
        [&] { return rig.adapter->ReadGoal(key(1))->result == expected; }));
    EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->preempted_by);
  }
}

TEST(JtcExecutionPort, TwoUnfinishedRecordsRefuseNewGoalsBeforeEffect) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  EXPECT_EQ(rig.adapter->Submit(key(3), trajectory()).refusal,
            ExecutionPortRefusal::kNotReady);
  EXPECT_EQ(rig.adapter->Replace(key(2), key(3), trajectory()).refusal,
            ExecutionPortRefusal::kNotReady);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(3)));
  EXPECT_EQ(rig.goal_count(), 2U);
  // OD-55: the port cancels the exact key it is given; an unfinished
  // predecessor is still cancellable (Core walks the chain).
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  EXPECT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1U; }));
}

TEST(JtcExecutionPort,
     NewGoalEvictsOnlyOldestFinishedRecordAndIgnoresLateCallback) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto old_handle = rig.handle();
  auto success = std::make_shared<Action::Result>();
  success->error_code = Action::Result::SUCCESSFUL;
  old_handle->succeed(success);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->result ==
           ExecutionGoalResult::kSucceeded;
  }));
  ASSERT_EQ(rig.adapter->Submit(key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  ASSERT_EQ(rig.adapter->Replace(key(2), key(3), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(3)));
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1)));
  EXPECT_TRUE(rig.adapter->ReadGoal(key(2)));
  EXPECT_TRUE(rig.adapter->ReadGoal(key(3)));
  const auto before = rig.adapter->ReadGoal(key(3));
  rclcpp_action::ClientGoalHandle<Action>::WrappedResult late;
  late.goal_id = old_handle->get_goal_id();
  late.code = rclcpp_action::ResultCode::SUCCEEDED;
  late.result = success;
  JtcExecutionPortTestPeer::terminal(*rig.adapter, key(1), late);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1)));
  EXPECT_EQ(rig.adapter->ReadGoal(key(3)), before);
}

TEST(JtcExecutionPort, AcceptedSuccessorFactSurvivesItsFinishedRecordEviction) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto old = rig.handle();
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  auto success = std::make_shared<Action::Result>();
  success->error_code = Action::Result::SUCCESSFUL;
  rig.handle(1)->succeed(success);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(2))->result ==
           ExecutionGoalResult::kSucceeded;
  }));
  ASSERT_EQ(rig.adapter->Submit(key(3), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(3)));
  EXPECT_FALSE(rig.adapter->ReadGoal(key(2)));
  auto preempted = std::make_shared<Action::Result>();
  preempted->error_code = Action::Result::INVALID_GOAL;
  preempted->error_string = "Current goal preempted by new incoming action.";
  old->abort(preempted);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->result ==
           ExecutionGoalResult::kPreemptedBy;
  }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->preempted_by, key(2));
}

TEST(JtcExecutionPort,
     ActionFeedbackIsGoalScopedAndMappedToControllerJointOrder) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->latest_feedback);
  auto feedback = std::make_shared<Action::Feedback>();
  feedback->header.stamp.sec = 8;
  feedback->header.stamp.nanosec = 7;
  feedback->joint_names = {"b", "a"};
  feedback->desired.positions = {0.2, 0.1};
  feedback->desired.velocities = {2.0, 1.0};
  feedback->desired.accelerations = {20.0, 10.0};
  feedback->desired.time_from_start.nanosec = 250'000'000;
  feedback->actual.positions = {0.19, 0.09};
  rig.clock.set(84);
  rig.handle()->publish_feedback(feedback);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->latest_feedback.has_value();
  }));
  const auto observed = *rig.adapter->ReadGoal(key(1))->latest_feedback;
  EXPECT_EQ(observed.source_stamp_ns, TimePoint{8'000'000'007});
  EXPECT_EQ(observed.local_receipt_stamp_ns, TimePoint{84});
  EXPECT_DOUBLE_EQ(observed.cursor_s, 0.25);
  EXPECT_EQ(observed.desired_positions_rad, (std::vector<double>{0.1, 0.2}));
  EXPECT_EQ(observed.desired_velocities_rad_s, (std::vector<double>{1.0, 2.0}));
  EXPECT_EQ(observed.desired_accelerations_rad_s2,
            (std::vector<double>{10.0, 20.0}));
  EXPECT_EQ(observed.actual_positions_rad, (std::vector<double>{0.09, 0.19}));
  EXPECT_FALSE(
      rig.adapter->ReadSample()); // Feedback is never a controller-wide sample.
}

TEST(JtcExecutionPort,
     AppliedZeroOrPartialFactorDoesNotSuppressExplicitCancel) {
  for (const double factor : {0.0, 0.25, 0.5, 0.75}) {
    Rig rig;
    ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
    ASSERT_TRUE(rig.accepted(key(1)));
    rig.state(factor);
    const auto applied = rig.adapter->ReadSample()->applied_factor;
    ASSERT_TRUE(applied);
    EXPECT_EQ(rig.adapter->SetScale(*applied).dispatch,
              ExecutionPortDispatch::kDispatched);
    ASSERT_TRUE(wait_until([&] { return rig.scale_count.load() == 1; }));
    EXPECT_DOUBLE_EQ(rig.last_scale.load(), factor);
    EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
              ExecutionPortDispatch::kDispatched);
    ASSERT_TRUE(wait_until([&] {
      return rig.adapter->ReadGoal(key(1))
          ->cancel_ack_local_receipt_stamp.has_value();
    }));
    EXPECT_EQ(rig.cancel_count.load(), 1U);
    EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->result);
    EXPECT_EQ(rig.adapter->ReadSample()->reference_velocities_rad_s,
              (std::vector<double>{0.01, 0.02}));
  }
}

TEST(JtcExecutionPort, NegativeCancelResponseCannotSetPositiveAcknowledgement) {
  Rig rig;
  rig.reject_cancel = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  rig.clock.set(91);
  EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1; }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp,
            TimePoint{91});
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_ack_local_receipt_stamp);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->result);
}

TEST(JtcExecutionPort,
     AmbiguousWrongUuidAndNegativeCallbacksNeverManufactureAck) {
  Rig rig;
  rig.reject_cancel = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  auto response = std::make_shared<action_msgs::srv::CancelGoal::Response>();
  response->return_code = action_msgs::srv::CancelGoal::Response::ERROR_NONE;
  JtcExecutionPortTestPeer::cancel_response(*rig.adapter, key(1), 1,
                                            response); // Empty.
  action_msgs::msg::GoalInfo info;
  info.goal_id.uuid = rig.handle()->get_goal_id();
  auto wrong = info;
  wrong.goal_id.uuid[0] ^= 1;
  response->goals_canceling = {wrong};
  JtcExecutionPortTestPeer::cancel_response(*rig.adapter, key(1), 1, response);
  response->goals_canceling = {info, wrong};
  JtcExecutionPortTestPeer::cancel_response(*rig.adapter, key(1), 1,
                                            response); // Ambiguous list.
  response->goals_canceling = {info};
  response->return_code =
      action_msgs::srv::CancelGoal::Response::ERROR_REJECTED;
  JtcExecutionPortTestPeer::cancel_response(*rig.adapter, key(1), 1, response);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_ack_local_receipt_stamp);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->terminal_local_receipt_stamp);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->result);
}

TEST(JtcExecutionPort, TerminalResultMayPrecedeAckWithoutManufacturedFinish) {
  Rig rig;
  rig.reject_cancel = true; // Real server emits no positive ACK; inject
                            // callback ordering explicitly.
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto uuid = rig.handle()->get_goal_id();
  rig.clock.set(101);
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  rclcpp_action::ClientGoalHandle<Action>::WrappedResult terminal;
  terminal.goal_id = uuid;
  terminal.code = rclcpp_action::ResultCode::CANCELED;
  terminal.result = std::make_shared<Action::Result>();
  rig.clock.set(102);
  JtcExecutionPortTestPeer::terminal(*rig.adapter, key(1), terminal);
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->terminal_local_receipt_stamp,
            TimePoint{102});
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_ack_local_receipt_stamp);
  auto response = std::make_shared<action_msgs::srv::CancelGoal::Response>();
  response->return_code = action_msgs::srv::CancelGoal::Response::ERROR_NONE;
  action_msgs::msg::GoalInfo info;
  info.goal_id.uuid = uuid;
  response->goals_canceling = {info};
  rig.clock.set(103);
  JtcExecutionPortTestPeer::cancel_response(*rig.adapter, key(1), 1, response);
  const auto observed = rig.adapter->ReadGoal(key(1));
  EXPECT_EQ(observed->cancel_sent_local_stamp, TimePoint{101});
  EXPECT_EQ(observed->cancel_ack_local_receipt_stamp, TimePoint{103});
  EXPECT_EQ(observed->result, ExecutionGoalResult::kCanceled);
  EXPECT_FALSE(rig.adapter->ReadSample());
}

TEST(JtcExecutionPort,
     ObsoleteCancelRequestCallbackCannotAcknowledgeLaterRequest) {
  Rig rig;
  rig.reject_cancel = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch,
            ExecutionPortDispatch::kDispatched);
  // A repeat joins the pending request; a new request is sent only after the
  // first one's rejection arrived.
  rig.clock.set(93);
  ASSERT_TRUE(wait_until([&] {
    EXPECT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
    return rig.cancel_count.load() == 2U;
  }));
  auto response = std::make_shared<action_msgs::srv::CancelGoal::Response>();
  response->return_code = action_msgs::srv::CancelGoal::Response::ERROR_NONE;
  action_msgs::msg::GoalInfo info;
  info.goal_id.uuid = rig.handle()->get_goal_id();
  response->goals_canceling = {info};
  JtcExecutionPortTestPeer::cancel_response(*rig.adapter, key(1), 1, response);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_ack_local_receipt_stamp);
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp,
            TimePoint{93});
}

TEST(JtcExecutionPort, PreemptionRequiresAcceptedSuccessorOnSameActiveBinding) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  auto old = rig.handle();
  ASSERT_EQ(rig.adapter->Replace(key(1), key(2), trajectory()).dispatch,
            ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(2)));
  auto result = std::make_shared<Action::Result>();
  result->error_code = Action::Result::INVALID_GOAL;
  result->error_string = "Current goal preempted by new incoming action.";
  old->abort(result);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->result ==
           ExecutionGoalResult::kPreemptedBy;
  }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->preempted_by, key(2));
}

TEST(JtcExecutionPort, DeactivationErrorIsNotMisclassifiedAsPreemption) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  auto result = std::make_shared<Action::Result>();
  result->error_code = Action::Result::INVALID_GOAL;
  result->error_string = "Current goal cancelled during deactivate transition.";
  rig.handle()->abort(result);
  ASSERT_TRUE(wait_until([&] {
    return rig.adapter->ReadGoal(key(1))->phase ==
           ExecutionGoalPhase::kTerminal;
  }));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result,
            ExecutionGoalResult::kEndedUnknown);
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->preempted_by);
}

TEST(JtcExecutionPort, CloseDoesNotResendOrWaitForAlreadyAcknowledgedCancel) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] {
    const auto goal = rig.adapter->ReadGoal(key(1));
    return goal && goal->cancel_ack_local_receipt_stamp.has_value();
  }));
  ASSERT_FALSE(rig.adapter->ReadGoal(key(1))->result);
  ASSERT_EQ(rig.cancel_count.load(), 1U);
  const auto started = std::chrono::steady_clock::now();
  rig.adapter->close_admission(started + std::chrono::seconds(2));
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_TRUE(rig.adapter->loss_cancel_shutdown_settled());
}

TEST(JtcExecutionPort, CloseWaitsForAlreadySentCancelReplyWithoutResending) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  rig.delay_next_cancel = true;
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.cancel_blocked.load(); }));
  std::atomic<bool> started{false};
  std::promise<void> done;
  auto completed = done.get_future();
  std::jthread close([&] {
    started.store(true);
    rig.adapter->close_admission(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    done.set_value();
  });
  struct ReleaseOnExit { Rig& rig; ~ReleaseOnExit() { rig.release_response(); } } release{rig};
  ASSERT_TRUE(wait_until([&] { return started.load(); }));
  EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  rig.release_response();
  ASSERT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  close.join();
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_TRUE(rig.adapter->loss_cancel_shutdown_settled());
}

TEST(JtcExecutionPort, CloseContainsPendingLateAcceptanceBeforeOwnerShutdown) {
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  std::atomic<bool> started{false};
  std::promise<void> done;
  auto completed = done.get_future();
  std::jthread close([&] {
    started.store(true);
    rig.adapter->close_admission(std::chrono::steady_clock::now() + std::chrono::seconds(2));
    done.set_value();
  });
  struct ReleaseOnExit { Rig& rig; ~ReleaseOnExit() { rig.release_response(); } } release{rig};
  ASSERT_TRUE(wait_until([&] { return started.load(); }));
  EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  rig.release_response();
  ASSERT_EQ(completed.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  close.join();
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_TRUE(rig.epoch_resources->node()->get_node_base_interface()->get_context()->is_valid());
}

TEST(JtcExecutionPort, CloseReportsUnsettledWhenCancelReplyMissesDeadline) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  rig.delay_next_cancel = true;
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.cancel_blocked.load(); }));
  struct ReleaseOnExit { Rig& rig; ~ReleaseOnExit() { rig.release_response(); } } release{rig};
  rig.adapter->close_admission(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
  EXPECT_FALSE(rig.adapter->loss_cancel_shutdown_settled());
  rig.release_response();
}

// Graph reads are made on the calling thread, never dispatched to the epoch
// owner: both return while that owner is held in another task.
TEST(JtcExecutionPort, EndpointRefreshAndActionReadinessDoNotWaitForBlockedOwner) {
  Rig rig;
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  BlockedOwner blocked(*rig.epoch_resources);
  ASSERT_TRUE(blocked.entered);
  const auto started = std::chrono::steady_clock::now();
  rig.adapter->RefreshEndpointFacts();
  const bool ready = rig.adapter->ActionServerIsReady();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const bool returned_while_blocked = !blocked.finished();
  blocked.release();
  EXPECT_TRUE(returned_while_blocked);
  EXPECT_LT(elapsed, 1s);
  EXPECT_TRUE(ready);
}

// Readiness loss schedules the exact cancel from the caller's thread; the
// epoch owner is not involved, so the cancel leaves while it is held busy.
TEST(JtcExecutionPort, ReadinessLossCancelLeavesWhileEpochOwnerIsBlocked) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto accepted = rig.handle()->get_goal_id();
  BlockedOwner blocked(*rig.epoch_resources);
  ASSERT_TRUE(blocked.entered);
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{1}});
  const bool sent = wait_until([&] { return rig.cancel_count.load() == 1U; });
  const bool sent_while_blocked = sent && !blocked.finished();
  blocked.release();
  EXPECT_TRUE(sent_while_blocked);
  EXPECT_EQ(rig.cancel_ids(), (std::vector<rclcpp_action::GoalUUID>{accepted}));
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->result, ExecutionGoalResult::kEndedUnknown);
  rig.adapter->ObserveReadiness({false, true, true, ExecutionBindingEpoch{1}});
  EXPECT_EQ(rig.cancel_count.load(), 1U);
}

// The settle wait is for responses only the epoch owner delivers, so on that
// owner close_admission closes admission and refuses the wait at once.
TEST(JtcExecutionPort, CloseAdmissionOnEpochOwnerClosesAndRefusesWithoutWaiting) {
  DedicatedEpochRig rig;
  rig.start();
  bool closed = true;
  std::chrono::steady_clock::duration elapsed{};
  rig.resources->invoke([&] {
    const auto started = std::chrono::steady_clock::now();
    closed = rig.adapter->close_admission(started + 2s);
    elapsed = std::chrono::steady_clock::now() - started;
  });
  EXPECT_FALSE(closed);
  EXPECT_LT(elapsed, 1s);
  EXPECT_FALSE(rig.adapter->ReadReadiness().controller_active);
  EXPECT_EQ(rig.adapter->Submit(key(1), trajectory()).dispatch,
            ExecutionPortDispatch::kNotDispatched);
  EXPECT_TRUE(rig.adapter->close_admission());
}
} // namespace
} // namespace manyforge::ros2_control_execution
