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

#include "manyforge_isaac_hardware/isaac_system.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <limits>
#include <string>
#include <thread>
#include <utility>

#include "hardware_interface/types/hardware_interface_type_values.hpp"
#include "pluginlib/class_list_macros.hpp"
#include "rclcpp/executors/single_threaded_executor.hpp"
#include "feedback_callback.hpp"

namespace manyforge_isaac_hardware
{
namespace
{
using hardware_interface::CallbackReturn;

std::int64_t steady_now_ns() noexcept
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool positive_parameter(
  const hardware_interface::HardwareInfo & info, const std::string & name, std::int64_t & result)
{
  const auto it = info.hardware_parameters.find(name);
  if (it == info.hardware_parameters.end()) {
    return false;
  }
  const auto & text = it->second;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result > 0;
}
}  // namespace

struct IsaacSystem::Impl
{
  std::array<std::string, detail::kJointCount> names;
  detail::Positions position;
  detail::Positions velocity;
  detail::Positions commands;
  bool require_velocity{false};
  detail::FeedbackLimits limits;
  detail::FeedbackSourceClock source_clock{detail::FeedbackSourceClock::kSystem};
  detail::SimulationClockInput simulation_clock;
  std::string feedback_topic;
  std::unique_ptr<detail::FeedbackHandoff> feedback;
  rclcpp::Node::SharedPtr node;
  rclcpp::Subscription<sensor_msgs::msg::JointState>::SharedPtr subscription;
  rclcpp::Subscription<rosgraph_msgs::msg::Clock>::SharedPtr clock_subscription;
  std::unique_ptr<rclcpp::executors::SingleThreadedExecutor> executor;
  std::thread worker;
  std::atomic<bool> observation_running{false};
  std::atomic<detail::WriteDisposition> write_disposition{
    detail::WriteDisposition::kSuppressedInactive};
  static_assert(std::atomic<bool>::is_always_lock_free);
  static_assert(std::atomic<detail::WriteDisposition>::is_always_lock_free);

  Impl()
  {
    invalidate();
    commands.fill(std::numeric_limits<double>::quiet_NaN());
  }

  void invalidate() noexcept
  {
    position.fill(std::numeric_limits<double>::quiet_NaN());
    velocity.fill(std::numeric_limits<double>::quiet_NaN());
  }

  void stop_observation()
  {
    observation_running.store(false, std::memory_order_release);
    if (executor) {
      executor->cancel();
    }
    if (worker.joinable()) {
      worker.join();
    }
    observation_running.store(false, std::memory_order_release);
    subscription.reset();
    clock_subscription.reset();
    executor.reset();
    node.reset();
    feedback.reset();
    simulation_clock = {};
    invalidate();
  }
};

IsaacSystem::IsaacSystem()
: impl_(std::make_unique<Impl>()) {}
IsaacSystem::~IsaacSystem() {impl_->stop_observation();}

CallbackReturn IsaacSystem::on_init(
  const hardware_interface::HardwareComponentInterfaceParams & params)
{
  if (SystemInterface::on_init(params) != CallbackReturn::SUCCESS) {
    return CallbackReturn::ERROR;
  }
  if (info_.joints.size() != detail::kJointCount || !info_.sensors.empty() ||
    !info_.gpios.empty() || !info_.mimic_joints.empty() || info_.is_async)
  {
    return CallbackReturn::ERROR;
  }
  for (std::size_t i = 0; i < detail::kJointCount; ++i) {
    const auto & joint = info_.joints[i];
    if (joint.name.empty() || joint.command_interfaces.size() != 1 ||
      joint.command_interfaces.front().name != hardware_interface::HW_IF_POSITION ||
      joint.command_interfaces.front().data_type != "double" ||
      joint.state_interfaces.empty() || joint.state_interfaces.size() > 2 ||
      joint.parameters.contains("mimic") ||
      joint.is_mimic == hardware_interface::MimicAttribute::TRUE)
    {
      return CallbackReturn::ERROR;
    }
    bool position = false;
    bool velocity = false;
    for (const auto & interface : joint.state_interfaces) {
      if (interface.data_type != "double") {
        return CallbackReturn::ERROR;
      }
      if (interface.name == hardware_interface::HW_IF_POSITION && !position) {
        position = true;
      } else if (interface.name == hardware_interface::HW_IF_VELOCITY && !velocity) {
        velocity = true;
      } else {
        return CallbackReturn::ERROR;
      }
    }
    if (!position || (i != 0 && velocity != impl_->require_velocity)) {
      return CallbackReturn::ERROR;
    }
    impl_->require_velocity = velocity;
    for (std::size_t previous = 0; previous < i; ++previous) {
      if (impl_->names[previous] == joint.name) {
        return CallbackReturn::ERROR;
      }
    }
    impl_->names[i] = joint.name;
  }
  const auto topic = info_.hardware_parameters.find("joint_states_topic");
  if (topic == info_.hardware_parameters.end() || topic->second.empty() ||
    !positive_parameter(info_, "source_max_age_ns", impl_->limits.source_max_age_ns) ||
    !positive_parameter(info_, "receive_max_age_ns", impl_->limits.receive_max_age_ns))
  {
    return CallbackReturn::ERROR;
  }
  impl_->feedback_topic = topic->second;
  const auto source_clock = info_.hardware_parameters.find("feedback_source_clock");
  if (source_clock == info_.hardware_parameters.end()) {
    return CallbackReturn::ERROR;
  }
  if (source_clock->second == "system") {
    impl_->source_clock = detail::FeedbackSourceClock::kSystem;
  } else if (source_clock->second == "ros_simulation") {
    impl_->source_clock = detail::FeedbackSourceClock::kRosSimulation;
  } else {
    return CallbackReturn::ERROR;
  }
  // Authored initial_value is deliberately never a current-state observation.
  return CallbackReturn::SUCCESS;
}

std::vector<hardware_interface::StateInterface::ConstSharedPtr>
IsaacSystem::on_export_state_interfaces()
{
  std::vector<hardware_interface::StateInterface::ConstSharedPtr> interfaces;
  interfaces.reserve(detail::kJointCount * (impl_->require_velocity ? 2 : 1));
  for (std::size_t i = 0; i < detail::kJointCount; ++i) {
    interfaces.push_back(std::make_shared<hardware_interface::StateInterface>(
      impl_->names[i], hardware_interface::HW_IF_POSITION, &impl_->position[i]));
    if (impl_->require_velocity) {
      interfaces.push_back(std::make_shared<hardware_interface::StateInterface>(
        impl_->names[i], hardware_interface::HW_IF_VELOCITY, &impl_->velocity[i]));
    }
  }
  return interfaces;
}

std::vector<hardware_interface::CommandInterface::SharedPtr>
IsaacSystem::on_export_command_interfaces()
{
  std::vector<hardware_interface::CommandInterface::SharedPtr> interfaces;
  interfaces.reserve(detail::kJointCount);
  for (std::size_t i = 0; i < detail::kJointCount; ++i) {
    interfaces.push_back(std::make_shared<hardware_interface::CommandInterface>(
      impl_->names[i], hardware_interface::HW_IF_POSITION, &impl_->commands[i]));
  }
  return interfaces;
}

CallbackReturn IsaacSystem::on_configure(const rclcpp_lifecycle::State &)
{
  impl_->stop_observation();
  try {
    impl_->feedback = std::make_unique<detail::FeedbackHandoff>(
      impl_->names, impl_->require_velocity, impl_->limits, impl_->source_clock);
    const bool simulation_clock =
      impl_->source_clock == detail::FeedbackSourceClock::kRosSimulation;
    rclcpp::NodeOptions options;
    options.use_global_arguments(false);
    options.clock_type(RCL_SYSTEM_TIME);
    options.use_clock_thread(false);
    options.parameter_overrides({rclcpp::Parameter("use_sim_time", false)});
    impl_->node = std::make_shared<rclcpp::Node>("manyforge_isaac_hardware_" + info_.name, options);
    if (simulation_clock) {
      impl_->clock_subscription = impl_->node->create_subscription<rosgraph_msgs::msg::Clock>(
        "/clock", rclcpp::ClockQoS(),
        [this](const rosgraph_msgs::msg::Clock::ConstSharedPtr message) {
          impl_->simulation_clock.receive(*impl_->feedback, *message, steady_now_ns());
        });
    }
    impl_->subscription = impl_->node->create_subscription<sensor_msgs::msg::JointState>(
      impl_->feedback_topic, rclcpp::SensorDataQoS().keep_last(1),
      [this](const sensor_msgs::msg::JointState::ConstSharedPtr message) {
        const bool simulation =
        impl_->source_clock == detail::FeedbackSourceClock::kRosSimulation;
        detail::receive_joint_state(
          *impl_->feedback, *message,
          simulation ? impl_->simulation_clock.now_ns : impl_->node->now().nanoseconds(),
          steady_now_ns(), !simulation || impl_->simulation_clock.initialized);
      });
    impl_->executor = std::make_unique<rclcpp::executors::SingleThreadedExecutor>();
    impl_->executor->add_node(impl_->node);
    impl_->observation_running.store(true, std::memory_order_release);
    impl_->worker = std::thread([this]() {
          try {
          // spin_once's finite NRT wait also closes the cancel-before-spin
          // startup race: cleanup cannot wait forever on a late-starting spin.
            while (impl_->observation_running.load(std::memory_order_acquire)) {
              impl_->executor->spin_once(std::chrono::milliseconds(10));
            }
          } catch (const std::exception & error) {
            RCLCPP_ERROR(
            get_logger(), "Isaac observation worker unavailable for %s: %s",
            info_.name.c_str(), error.what());
          } catch (...) {
            RCLCPP_ERROR(
            get_logger(), "Isaac observation worker unavailable for %s: unknown exception",
            info_.name.c_str());
          }
          impl_->observation_running.store(false, std::memory_order_release);
      });
  } catch (const std::exception & error) {
    RCLCPP_ERROR(
      get_logger(), "Isaac observation configure failed for %s: %s",
      info_.name.c_str(), error.what());
    impl_->stop_observation();
    return CallbackReturn::FAILURE;
  } catch (...) {
    RCLCPP_ERROR(
      get_logger(), "Isaac observation configure failed for %s: unknown exception",
      info_.name.c_str());
    impl_->stop_observation();
    return CallbackReturn::FAILURE;
  }
  return CallbackReturn::SUCCESS;
}

CallbackReturn IsaacSystem::on_activate(const rclcpp_lifecycle::State &)
{
  RCLCPP_WARN(
    get_logger(),
      "Isaac command transport unavailable: binding/lease command path is not installed");
  return CallbackReturn::FAILURE;
}

CallbackReturn IsaacSystem::on_deactivate(const rclcpp_lifecycle::State &)
{
  return CallbackReturn::SUCCESS;
}

CallbackReturn IsaacSystem::on_cleanup(const rclcpp_lifecycle::State &)
{
  impl_->stop_observation();
  return CallbackReturn::SUCCESS;
}

CallbackReturn IsaacSystem::on_shutdown(const rclcpp_lifecycle::State & state)
{
  return on_cleanup(state);
}

CallbackReturn IsaacSystem::on_error(const rclcpp_lifecycle::State & state)
{
  return on_cleanup(state);
}

hardware_interface::return_type IsaacSystem::read(const rclcpp::Time &, const rclcpp::Duration &)
{
  if (!impl_->observation_running.load(std::memory_order_acquire)) {
    impl_->invalidate();
    return hardware_interface::return_type::OK;
  }
  const auto sample = impl_->feedback->read(steady_now_ns());
  if (sample.validity == detail::FeedbackValidity::kValid) {
    impl_->position = sample.position;
    if (impl_->require_velocity) {
      impl_->velocity = sample.velocity;
    }
  } else {
    impl_->invalidate();
  }
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type IsaacSystem::write(const rclcpp::Time &, const rclcpp::Duration &)
{
  // OK maintains an inactive interface; it makes no delivery/realization claim.
  impl_->write_disposition.store(
    detail::WriteDisposition::kSuppressedInactive, std::memory_order_release);
  return hardware_interface::return_type::OK;
}

}  // namespace manyforge_isaac_hardware

PLUGINLIB_EXPORT_CLASS(manyforge_isaac_hardware::IsaacSystem, hardware_interface::SystemInterface)
