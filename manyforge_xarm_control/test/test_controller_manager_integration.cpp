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

#include <gtest/gtest.h>

#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <controller_manager/controller_manager.hpp>
#include <controller_manager_msgs/srv/switch_controller.hpp>
#include <hardware_interface/hardware_info.hpp>
#include <hardware_interface/resource_manager.hpp>
#include <hardware_interface/types/hardware_component_params.hpp>
#include <hardware_interface/types/hardware_interface_type_values.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <rclcpp/executors/multi_threaded_executor.hpp>
#include <rclcpp/node.hpp>
#include <rclcpp/parameter.hpp>
#include <rclcpp_lifecycle/state.hpp>
#include <trajectory_msgs/msg/joint_trajectory.hpp>
#include <trajectory_msgs/msg/joint_trajectory_point.hpp>

#include <manyforge_robot_lifecycle/local_operator_gateway.hpp>
#include <manyforge_xarm_control/system_hardware.hpp>

namespace manyforge::xarm_control
{
namespace
{

using namespace std::chrono_literals;

std::int64_t steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t thread_cpu_now_ns()
{
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
    return 0;
  }
  return
    static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000 +
    value.tv_nsec;
}

std::string read_text_file(const std::filesystem::path & path)
{
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("unable to open generated binding artifact");
  }
  return {
    std::istreambuf_iterator<char>(stream),
    std::istreambuf_iterator<char>()};
}

hardware_interface::InterfaceInfo interface(std::string name)
{
  hardware_interface::InterfaceInfo result;
  result.name = std::move(name);
  result.size = 1;
  result.enable_limits = false;
  return result;
}

std::vector<std::string> joint_names(std::string_view prefix)
{
  std::vector<std::string> result;
  result.reserve(6);
  for (std::size_t index = 0; index < 6; ++index) {
    result.push_back(
      std::string(prefix) + "joint" + std::to_string(index + 1));
  }
  return result;
}

std::string ros_identifier(std::string value)
{
  std::replace_if(
    value.begin(), value.end(),
    [](char character) {
      return !std::isalnum(static_cast<unsigned char>(character)) &&
             character != '_';
    },
    '_');
  return value;
}

std::string test_instance_suffix()
{
  const char * configured =
    std::getenv("MANYFORGE_TEST_INSTANCE_SUFFIX");
  if (configured == nullptr || *configured == '\0') {
    return {};
  }
  return "_" + ros_identifier(configured);
}

hardware_interface::HardwareInfo hardware_info(
  std::string robot_id,
  std::string_view joint_prefix,
  std::string controller_name,
  std::string activity_topic)
{
  hardware_interface::HardwareInfo result;
  result.name = ros_identifier(robot_id) + "_system";
  result.type = "system";
  result.hardware_plugin_name =
    "manyforge_xarm_control/ManyForgeXArmSystemHardware";
  result.hardware_parameters = {
    {"backend", "fake"},
    {"fake_backend_confirmation", "ALLOW_TEST_ONLY_FAKE_BACKEND"},
    {"robot_id", robot_id},
    {"transport_owner_id", robot_id + ":sole-owner"},
    {"authority_profile", "local_operator"},
    {"trajectory_controller_name", std::move(controller_name)},
    {"controller_manager_activity_topic", std::move(activity_topic)},
    {"observation_lease_ms", "1000"},
    {"observation_refresh_ms", "20"},
  };
  for (const auto & name : joint_names(joint_prefix)) {
    hardware_interface::ComponentInfo joint;
    joint.name = name;
    joint.type = "joint";
    joint.command_interfaces = {
      interface(hardware_interface::HW_IF_POSITION),
      interface(hardware_interface::HW_IF_VELOCITY)};
    joint.state_interfaces = {
      interface(hardware_interface::HW_IF_POSITION),
      interface(hardware_interface::HW_IF_VELOCITY)};
    result.joints.push_back(std::move(joint));
  }
  return result;
}

class ControllerManagerLane final
{
public:
  ControllerManagerLane(
    std::string robot_id,
    std::string node_namespace,
    std::string joint_prefix)
  : robot_id_(std::move(robot_id) + test_instance_suffix()),
    node_namespace_(std::move(node_namespace) + test_instance_suffix()),
    controller_name_(
      ros_identifier(robot_id_) + "_trajectory_controller"),
    joint_names_(joint_names(joint_prefix)),
    executor_(std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
        rclcpp::ExecutorOptions(), 2)),
    host_node_(std::make_shared<rclcpp::Node>(
        ros_identifier(robot_id_) + "_test_host", node_namespace_))
  {
    executor_->add_node(host_node_);
    hardware_interface::ResourceManagerParams resource_params;
    resource_params.robot_description =
      "<robot name=\"manyforge_fake_harness\">"
      "<link name=\"bootstrap_base\"/>"
      "<link name=\"bootstrap_tip\"/>"
      "<joint name=\"bootstrap_joint\" type=\"revolute\">"
      "<parent link=\"bootstrap_base\"/>"
      "<child link=\"bootstrap_tip\"/>"
      "<axis xyz=\"0 0 1\"/>"
      "<limit lower=\"-1\" upper=\"1\" effort=\"1\" velocity=\"1\"/>"
      "</joint>"
      "<ros2_control name=\"bootstrap_system\" type=\"system\">"
      "<hardware><plugin>mock_components/GenericSystem</plugin></hardware>"
      "<joint name=\"bootstrap_joint\">"
      "<command_interface name=\"position\"/>"
      "<state_interface name=\"position\"/>"
      "<state_interface name=\"velocity\"/>"
      "</joint>"
      "</ros2_control>"
      "</robot>";
    resource_params.clock = host_node_->get_clock();
    resource_params.logger = host_node_->get_logger();
    resource_params.node_namespace = node_namespace_;
    resource_params.executor = executor_;
    resource_params.update_rate = 1000;
    auto resources = std::make_unique<hardware_interface::ResourceManager>(
      resource_params, true);

    const std::string activity_topic =
      node_namespace_ + "/controller_manager/activity";
    hardware_info_ = hardware_info(
      robot_id_, joint_prefix, controller_name_, activity_topic);
    auto hardware = std::make_unique<ManyForgeXArmSystemHardware>();
    hardware_ = hardware.get();
    hardware_interface::HardwareComponentParams component_params;
    component_params.hardware_info = hardware_info_;
    component_params.logger = host_node_->get_logger();
    component_params.clock = host_node_->get_clock();
    component_params.node_namespace = node_namespace_;
    component_params.executor = executor_;
    resources->import_component(
      std::move(hardware), component_params);

    rclcpp_lifecycle::State active_state(
      lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE, "active");
    if (resources->set_component_state(
        hardware_info_.name, active_state) !=
      hardware_interface::return_type::OK)
    {
      throw std::runtime_error("fake hardware activation failed");
    }

    auto manager_options = controller_manager::get_cm_node_options();
    manager_options.parameter_overrides(
      {rclcpp::Parameter("update_rate", 1000)});
    manager_ = std::make_shared<controller_manager::ControllerManager>(
      std::move(resources), executor_, "controller_manager",
      node_namespace_, manager_options);
    executor_->add_node(manager_);

    controller_parameter_file_ =
      std::filesystem::temp_directory_path() /
      ("manyforge_" + ros_identifier(robot_id_) + "_jtc.yaml");
    std::ofstream parameter_stream(controller_parameter_file_);
    parameter_stream << "/**:\n"
                     << "  ros__parameters:\n"
                     << "    joints:\n";
    for (const auto & joint : joint_names_) {
      parameter_stream << "      - " << joint << '\n';
    }
    parameter_stream << "    command_interfaces: [position, velocity]\n"
                     << "    state_interfaces: [position, velocity]\n";
    parameter_stream.close();
    if (!parameter_stream) {
      throw std::runtime_error("JTC parameter file write failed");
    }
    manager_->declare_parameter(
      controller_name_ + ".params_file",
      controller_parameter_file_.string());
    controller_ = manager_->load_controller(
      controller_name_,
      "joint_trajectory_controller/JointTrajectoryController");
    if (controller_ == nullptr) {
      throw std::runtime_error("JTC load failed");
    }
    if (manager_->configure_controller(controller_name_) !=
      controller_interface::return_type::OK)
    {
      throw std::runtime_error("JTC configure failed");
    }

    trajectory_publisher_ =
      host_node_->create_publisher<trajectory_msgs::msg::JointTrajectory>(
      controller_name_ + "/joint_trajectory", rclcpp::QoS(10));

    robot_lifecycle::LocalOperatorGatewayConfig gateway_config;
    gateway_config.robot_id = robot_id_;
    gateway_config.maximum_authority_lease_ns = 60'000'000'000;
    gateway_ =
      std::make_unique<robot_lifecycle::LocalOperatorGateway>(
      std::move(gateway_config), *hardware_);
  }

  ~ControllerManagerLane()
  {
    stop();
    std::error_code ignored;
    std::filesystem::remove(controller_parameter_file_, ignored);
  }

  ControllerManagerLane(const ControllerManagerLane &) = delete;
  ControllerManagerLane & operator=(const ControllerManagerLane &) = delete;

  void start()
  {
    if (running_.exchange(true)) {
      return;
    }
    executor_thread_ = std::thread([this]() {executor_->spin();});
    update_thread_ = std::thread(
      [this]()
      {
        auto next = std::chrono::steady_clock::now();
        const auto period = rclcpp::Duration::from_nanoseconds(1'000'000);
        while (running_.load(std::memory_order_acquire)) {
          const auto started_ns = thread_cpu_now_ns();
          const auto now = manager_->now();
          manager_->read(now, period);
          manager_->update(now, period);
          manager_->write(now, period);
          if (record_timing_.load(std::memory_order_acquire)) {
            const auto finished_ns = thread_cpu_now_ns();
            std::scoped_lock timing_lock(timing_mutex_);
            if (record_timing_.load(std::memory_order_acquire)) {
              if (started_ns <= 0 || finished_ns < started_ns) {
                timing_clock_failed_ = true;
              } else if (sample_count_ < timing_samples_.size()) {
                timing_samples_[sample_count_++] =
                finished_ns - started_ns;
              }
            }
          }
          next += 1ms;
          std::this_thread::sleep_until(next);
        }
      });
  }

  void stop()
  {
    if (!running_.exchange(false)) {
      return;
    }
    if (update_thread_.joinable()) {
      update_thread_.join();
    }
    executor_->cancel();
    if (executor_thread_.joinable()) {
      executor_thread_.join();
    }
  }

  bool activate_controller()
  {
    return manager_->switch_controller(
      {controller_name_}, {},
      controller_manager_msgs::srv::SwitchController::Request::STRICT,
      true, rclcpp::Duration::from_nanoseconds(1'000'000'000)) ==
           controller_interface::return_type::OK;
  }

  bool deactivate_controller()
  {
    return manager_->switch_controller(
      {}, {controller_name_},
      controller_manager_msgs::srv::SwitchController::Request::STRICT,
      true, rclcpp::Duration::from_nanoseconds(1'000'000'000)) ==
           controller_interface::return_type::OK;
  }

  robot_lifecycle::CallerContext operator_caller() const
  {
    const auto now_ns = steady_now_ns();
    robot_lifecycle::CallerContext result;
    result.command_source = "test.lifecycle_operator";
    result.caller_context_id = robot_id_ + ":test-context";
    result.assurance =
      robot_lifecycle::CallerAssurance::kAuthenticated;
    result.asserted_at_ns = now_ns - 1;
    result.valid_until_ns = now_ns + 50'000'000'000;
    return result;
  }

  bool open_session(
    const robot_lifecycle::CallerContext & caller,
    std::uint64_t generation)
  {
    robot_lifecycle::LocalAuthoritySessionIntent intent;
    intent.session_id =
      robot_id_ + ":session:" + std::to_string(generation);
    intent.generation = generation;
    intent.valid_until_ns = steady_now_ns() + 30'000'000'000;
    intent.component_epoch =
      robot_id_ + ":authority-epoch:" + std::to_string(generation);
    return gateway_->open_session(
      caller, intent, steady_now_ns()).accepted;
  }

  bool enable(
    const robot_lifecycle::CallerContext & caller,
    std::uint64_t session_generation,
    std::uint64_t request_sequence)
  {
    const auto now_ns = steady_now_ns();
    const auto current = hardware_->lifecycle_snapshot(now_ns);
    robot_lifecycle::LocalLifecycleCommandIntent intent;
    intent.request_id =
      robot_id_ + ":enable:" + std::to_string(request_sequence);
    intent.request_sequence = request_sequence;
    intent.action_id = "robot.enable";
    intent.authority_session_id =
      robot_id_ + ":session:" + std::to_string(session_generation);
    intent.authority_session_generation = session_generation;
    intent.expected_snapshot_generation = current.generation;
    intent.confirmation_evidence = "test-operator-confirmed";
    return gateway_->submit(caller, intent, now_ns).accepted;
  }

  bool close_session(
    const robot_lifecycle::CallerContext & caller,
    std::uint64_t generation)
  {
    return gateway_->close_session(
      caller,
      robot_id_ + ":session:" + std::to_string(generation),
      generation, steady_now_ns()).accepted;
  }

  void publish_target(double target, std::chrono::milliseconds duration)
  {
    trajectory_msgs::msg::JointTrajectory message;
    message.joint_names = joint_names_;
    trajectory_msgs::msg::JointTrajectoryPoint point;
    point.positions.assign(joint_names_.size(), target);
    point.time_from_start.sec =
      static_cast<std::int32_t>(duration.count() / 1000);
    point.time_from_start.nanosec =
      static_cast<std::uint32_t>(
      (duration.count() % 1000) * 1'000'000);
    message.points.push_back(std::move(point));
    trajectory_publisher_->publish(message);
  }

  bool wait_for_admission(
    bool expected,
    std::chrono::milliseconds timeout = 2s) const
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (hardware_->lifecycle_snapshot(
          steady_now_ns()).motion_admitted == expected)
      {
        return true;
      }
      std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  bool wait_for_observation_ready(
    std::chrono::milliseconds timeout = 5s) const
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      const auto current = snapshot();
      if (current.observation_fresh &&
        current.observed.transport_connected ==
        robot_lifecycle::FactState::kTrue &&
        current.observed.identity_verified ==
        robot_lifecycle::FactState::kTrue &&
        current.observed.position_valid ==
        robot_lifecycle::FactState::kTrue &&
        current.observed.trajectory_controller_active ==
        robot_lifecycle::FactState::kFalse)
      {
        return true;
      }
      std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  bool wait_for_write_count_above(
    std::size_t value,
    std::chrono::milliseconds timeout = 2s) const
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      if (write_count() > value) {
        return true;
      }
      std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  std::size_t write_count() const
  {
    return hardware_->test_only_fake_joint_write_count();
  }

  robot_lifecycle::LifecycleSnapshot snapshot() const
  {
    return hardware_->lifecycle_snapshot(steady_now_ns());
  }

  std::vector<double> last_write() const
  {
    return hardware_->test_only_fake_last_joint_write();
  }

  std::vector<std::int64_t> capture_timing(
    std::chrono::milliseconds duration)
  {
    record_timing_.store(false, std::memory_order_release);
    {
      std::scoped_lock timing_lock(timing_mutex_);
      sample_count_ = 0;
      timing_clock_failed_ = false;
    }
    record_timing_.store(true, std::memory_order_release);
    std::this_thread::sleep_for(duration);
    record_timing_.store(false, std::memory_order_release);
    std::scoped_lock timing_lock(timing_mutex_);
    if (timing_clock_failed_) {
      return {};
    }
    return {
      timing_samples_.begin(),
      timing_samples_.begin() +
      static_cast<std::ptrdiff_t>(sample_count_)};
  }

private:
  std::string robot_id_;
  std::string node_namespace_;
  std::string controller_name_;
  std::vector<std::string> joint_names_;
  hardware_interface::HardwareInfo hardware_info_;
  ManyForgeXArmSystemHardware * hardware_ = nullptr;
  std::shared_ptr<rclcpp::executors::MultiThreadedExecutor> executor_;
  rclcpp::Node::SharedPtr host_node_;
  std::shared_ptr<controller_manager::ControllerManager> manager_;
  controller_interface::ControllerInterfaceBaseSharedPtr controller_;
  std::filesystem::path controller_parameter_file_;
  rclcpp::Publisher<
    trajectory_msgs::msg::JointTrajectory>::SharedPtr
    trajectory_publisher_;
  std::unique_ptr<robot_lifecycle::LocalOperatorGateway> gateway_;
  std::atomic<bool> running_{false};
  std::thread executor_thread_;
  std::thread update_thread_;
  std::vector<std::int64_t> timing_samples_ =
    std::vector<std::int64_t>(5000, 0);
  std::size_t sample_count_ = 0;
  bool timing_clock_failed_ = false;
  std::mutex timing_mutex_;
  std::atomic<bool> record_timing_{false};
};

class ControllerManagerIntegrationTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    if (!rclcpp::ok()) {
      rclcpp::init(0, nullptr);
    }
  }

  static void TearDownTestSuite()
  {
    if (rclcpp::ok()) {
      rclcpp::shutdown();
    }
  }
};

TEST_F(
  ControllerManagerIntegrationTest,
  ExactGeneratedProductionBindingLoadsAndClaimsExpectedInterfaces)
{
  const char * urdf_path =
    std::getenv("MANYFORGE_XARM_EXACT_FAKE_URDF");
  const char * controller_path =
    std::getenv("MANYFORGE_XARM_EXACT_CONTROLLER_YAML");
  if (urdf_path == nullptr || controller_path == nullptr) {
    GTEST_SKIP() << "exact generated binding fixture was not prepared";
  }

  auto executor =
    std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
    rclcpp::ExecutorOptions(), 2);
  auto host_node =
    std::make_shared<rclcpp::Node>("exact_lite6_binding_test_host");
  executor->add_node(host_node);
  hardware_interface::ResourceManagerParams resource_params;
  resource_params.robot_description = read_text_file(urdf_path);
  resource_params.clock = host_node->get_clock();
  resource_params.logger = host_node->get_logger();
  resource_params.node_namespace = "";
  resource_params.executor = executor;
  resource_params.update_rate = 150;
  auto resources = std::make_unique<hardware_interface::ResourceManager>(
    resource_params, true);
  auto * resource_view = resources.get();

  constexpr std::string_view kHardwareComponent = "lite6_system";
  const auto component_status = resource_view->get_components_status();
  ASSERT_NE(
    component_status.find(std::string(kHardwareComponent)),
    component_status.end());
  rclcpp_lifecycle::State active_state(
    lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE, "active");
  ASSERT_EQ(
    resource_view->set_component_state(
      std::string(kHardwareComponent), active_state),
    hardware_interface::return_type::OK);

  auto manager_options = controller_manager::get_cm_node_options();
  manager_options.arguments(
    {"--ros-args", "--params-file", controller_path});
  auto manager = std::make_shared<controller_manager::ControllerManager>(
    std::move(resources), executor, "controller_manager", "",
    manager_options);
  executor->add_node(manager);
  EXPECT_EQ(manager->get_update_rate(), 150U);

  constexpr std::string_view kController = "lite6_traj_controller";
  if (!manager->has_parameter(
      std::string(kController) + ".params_file"))
  {
    manager->declare_parameter(
      std::string(kController) + ".params_file",
      std::string(controller_path));
  }
  auto controller = manager->load_controller(
    std::string(kController),
    "joint_trajectory_controller/JointTrajectoryController");
  ASSERT_NE(controller, nullptr);
  ASSERT_EQ(
    manager->configure_controller(std::string(kController)),
    controller_interface::return_type::OK);
  EXPECT_TRUE(
    controller->get_node()->get_parameter(
      "constraints.decelerate_on_cancel").as_bool());
  for (std::size_t index = 1; index <= 6; ++index) {
    const auto parameter_name =
      "constraints.joint" + std::to_string(index) +
      ".max_deceleration_on_cancel";
    EXPECT_DOUBLE_EQ(
      controller->get_node()->get_parameter(parameter_name).as_double(),
      2.0) << parameter_name;
  }

  std::atomic<bool> running{true};
  std::thread executor_thread([&executor]() {executor->spin();});
  std::thread update_thread(
    [&manager, &running]()
    {
      const auto period =
      rclcpp::Duration::from_nanoseconds(1'000'000'000 / 150);
      auto next = std::chrono::steady_clock::now();
      while (running.load(std::memory_order_acquire)) {
        const auto now = manager->now();
        manager->read(now, period);
        manager->update(now, period);
        manager->write(now, period);
        next += std::chrono::nanoseconds(period.nanoseconds());
        std::this_thread::sleep_until(next);
      }
    });

  const auto activation_result = manager->switch_controller(
    {std::string(kController)}, {},
    controller_manager_msgs::srv::SwitchController::Request::STRICT,
    true, rclcpp::Duration::from_nanoseconds(1'000'000'000));
  EXPECT_EQ(
    activation_result, controller_interface::return_type::OK);

  const auto loaded = manager->get_loaded_controllers();
  const auto found = std::find_if(
    loaded.begin(), loaded.end(),
    [](const controller_manager::ControllerSpec & spec) {
      return spec.info.name == "lite6_traj_controller";
    });
  EXPECT_NE(found, loaded.end());
  if (found != loaded.end()) {
    EXPECT_EQ(
      found->info.type,
      "joint_trajectory_controller/JointTrajectoryController");
  }

  std::vector<std::string> expected_command_interfaces;
  std::vector<std::string> expected_state_interfaces;
  for (std::size_t index = 1; index <= 6; ++index) {
    const std::string joint = "joint" + std::to_string(index);
    expected_command_interfaces.push_back(joint + "/position");
    expected_command_interfaces.push_back(joint + "/velocity");
    expected_state_interfaces.push_back(joint + "/position");
    expected_state_interfaces.push_back(joint + "/velocity");
  }
  auto command_interfaces = resource_view->command_interface_keys();
  auto state_interfaces = resource_view->state_interface_keys();
  std::sort(command_interfaces.begin(), command_interfaces.end());
  std::sort(state_interfaces.begin(), state_interfaces.end());
  auto sorted_expected_commands = expected_command_interfaces;
  auto sorted_expected_states = expected_state_interfaces;
  std::sort(
    sorted_expected_commands.begin(), sorted_expected_commands.end());
  std::sort(sorted_expected_states.begin(), sorted_expected_states.end());
  EXPECT_EQ(command_interfaces, sorted_expected_commands);
  EXPECT_EQ(state_interfaces, sorted_expected_states);
  for (const auto & name : expected_command_interfaces) {
    EXPECT_TRUE(resource_view->command_interface_is_claimed(name))
      << name;
  }

  if (activation_result == controller_interface::return_type::OK) {
    EXPECT_EQ(
      manager->switch_controller(
        {}, {std::string(kController)},
        controller_manager_msgs::srv::SwitchController::Request::STRICT,
        true, rclcpp::Duration::from_nanoseconds(1'000'000'000)),
      controller_interface::return_type::OK);
  }
  running.store(false, std::memory_order_release);
  update_thread.join();
  executor->cancel();
  executor_thread.join();
}

TEST_F(
  ControllerManagerIntegrationTest,
  LiveResolvedRealBindingConfiguresAndCleansUpWithEstopPressed)
{
  const char * urdf_path =
    std::getenv("MANYFORGE_XARM_RESOLVED_REAL_URDF");
  const char * confirmation =
    std::getenv("MANYFORGE_XARM_LIVE_ESTOP_CONFIRM");
  if (urdf_path == nullptr || confirmation == nullptr) {
    GTEST_SKIP() << "live resolved binding was not selected";
  }
  ASSERT_STREQ(confirmation, "OBSERVE_ONLY_ESTOP_PRESSED");

  auto executor =
    std::make_shared<rclcpp::executors::MultiThreadedExecutor>(
    rclcpp::ExecutorOptions(), 2);
  auto host_node =
    std::make_shared<rclcpp::Node>(
    "resolved_real_lite6_preflight_host");
  executor->add_node(host_node);
  hardware_interface::ResourceManagerParams resource_params;
  resource_params.robot_description = read_text_file(urdf_path);
  resource_params.clock = host_node->get_clock();
  resource_params.logger = host_node->get_logger();
  resource_params.node_namespace = "";
  resource_params.executor = executor;
  resource_params.update_rate = 150;
  auto resources = std::make_unique<hardware_interface::ResourceManager>(
    resource_params, true);

  constexpr std::string_view kHardwareComponent = "lite6_system";
  auto component_status = resources->get_components_status();
  ASSERT_NE(
    component_status.find(std::string(kHardwareComponent)),
    component_status.end());
  EXPECT_EQ(
    component_status.at(std::string(kHardwareComponent)).state.id(),
    lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED);

  rclcpp_lifecycle::State inactive_state(
    lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE, "inactive");
  ASSERT_EQ(
    resources->set_component_state(
      std::string(kHardwareComponent), inactive_state),
    hardware_interface::return_type::OK);
  component_status = resources->get_components_status();
  ASSERT_EQ(
    component_status.at(std::string(kHardwareComponent)).state.id(),
    lifecycle_msgs::msg::State::PRIMARY_STATE_INACTIVE);

  rclcpp_lifecycle::State unconfigured_state(
    lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED,
    "unconfigured");
  ASSERT_EQ(
    resources->set_component_state(
      std::string(kHardwareComponent), unconfigured_state),
    hardware_interface::return_type::OK);
  component_status = resources->get_components_status();
  ASSERT_EQ(
    component_status.at(std::string(kHardwareComponent)).state.id(),
    lifecycle_msgs::msg::State::PRIMARY_STATE_UNCONFIGURED);

  ASSERT_EQ(
    resources->set_component_state(
      std::string(kHardwareComponent), inactive_state),
    hardware_interface::return_type::OK);
  rclcpp_lifecycle::State active_state(
    lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE, "active");
  EXPECT_EQ(
    resources->set_component_state(
      std::string(kHardwareComponent), active_state),
    hardware_interface::return_type::ERROR);

  resources.reset();
  executor->remove_node(host_node);
}

TEST_F(
  ControllerManagerIntegrationTest,
  RealJtcObeysAuthorityLossAndRejectsStaleCommandResumption)
{
  ControllerManagerLane lane(
    "lite6-a", "/manyforge_fake_a", "lite6_a_");
  lane.start();
  ASSERT_TRUE(lane.wait_for_observation_ready());
  const auto principal = lane.operator_caller();
  ASSERT_TRUE(lane.open_session(principal, 1));
  ASSERT_TRUE(lane.enable(principal, 1, 1));
  ASSERT_TRUE(lane.activate_controller());
  ASSERT_TRUE(lane.wait_for_admission(true));

  const auto before_motion = lane.write_count();
  lane.publish_target(0.2, 100ms);
  ASSERT_TRUE(lane.wait_for_write_count_above(before_motion));
  std::this_thread::sleep_for(200ms);

  ASSERT_TRUE(lane.close_session(principal, 1));
  ASSERT_TRUE(lane.wait_for_admission(false));
  std::this_thread::sleep_for(10ms);
  const auto blocked_count = lane.write_count();
  lane.publish_target(0.8, 100ms);
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(lane.write_count(), blocked_count);

  ASSERT_TRUE(lane.open_session(principal, 2));
  EXPECT_TRUE(lane.wait_for_admission(false));
  ASSERT_TRUE(lane.deactivate_controller());
  ASSERT_TRUE(lane.activate_controller());
  ASSERT_TRUE(lane.wait_for_admission(true));
  ASSERT_TRUE(lane.wait_for_write_count_above(blocked_count));
  std::this_thread::sleep_for(100ms);

  lane.stop();
  const auto final_positions = lane.last_write();
  ASSERT_EQ(final_positions.size(), 6U);
  for (const double position : final_positions) {
    EXPECT_NEAR(position, 0.2, 0.02);
    EXPECT_GT(std::abs(position - 0.8), 0.5);
  }
}

TEST_F(
  ControllerManagerIntegrationTest,
  UpdateLoopTimingRemainsBounded)
{
  ControllerManagerLane lane(
    "lite6-timing", "/manyforge_fake_timing", "lite6_timing_");
  lane.start();
  ASSERT_TRUE(lane.wait_for_observation_ready());
  const auto principal = lane.operator_caller();
  ASSERT_TRUE(lane.activate_controller());
  ASSERT_TRUE(lane.wait_for_admission(false));
  auto blocked_samples = lane.capture_timing(500ms);

  ASSERT_TRUE(lane.open_session(principal, 1));
  ASSERT_TRUE(lane.enable(principal, 1, 1));
  ASSERT_TRUE(lane.deactivate_controller());
  ASSERT_TRUE(lane.activate_controller());
  ASSERT_TRUE(lane.wait_for_admission(true));
  const auto initial_count = lane.write_count();
  ASSERT_TRUE(lane.wait_for_write_count_above(initial_count));
  auto admitted_samples = lane.capture_timing(500ms);
  lane.stop();

  ASSERT_GE(blocked_samples.size(), 250U);
  ASSERT_GE(admitted_samples.size(), 250U);
  std::sort(blocked_samples.begin(), blocked_samples.end());
  std::sort(admitted_samples.begin(), admitted_samples.end());
  const auto percentile =
    [](const std::vector<std::int64_t> & samples, double fraction)
    {
      const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(samples.size() - 1));
      return samples[index];
    };
  const auto blocked_p50_ns = percentile(blocked_samples, 0.50);
  const auto blocked_p95_ns = percentile(blocked_samples, 0.95);
  const auto blocked_p99_ns = percentile(blocked_samples, 0.99);
  const auto admitted_p50_ns = percentile(admitted_samples, 0.50);
  const auto admitted_p95_ns = percentile(admitted_samples, 0.95);
  const auto admitted_p99_ns = percentile(admitted_samples, 0.99);
  const auto admitted_max_ns = admitted_samples.back();
  const auto p95_overhead_ns =
    admitted_p95_ns - blocked_p95_ns;
  std::cout << "manyforge_xarm_control_update_loop_ns"
            << " blocked_p50=" << blocked_p50_ns
            << " blocked_p95=" << blocked_p95_ns
            << " blocked_p99=" << blocked_p99_ns
            << " admitted_p50=" << admitted_p50_ns
            << " admitted_p95=" << admitted_p95_ns
            << " admitted_p99=" << admitted_p99_ns
            << " admitted_max=" << admitted_max_ns
            << " p95_overhead=" << p95_overhead_ns
            << " blocked_samples=" << blocked_samples.size()
            << " admitted_samples=" << admitted_samples.size() << '\n';
  EXPECT_LT(admitted_p99_ns, 1'000'000);
  EXPECT_LT(admitted_max_ns, 10'000'000);
  EXPECT_LT(admitted_p95_ns, blocked_p95_ns + 250'000);
}

TEST_F(
  ControllerManagerIntegrationTest,
  TwoControllerManagersRemainAuthorityAndTransportIsolated)
{
  ControllerManagerLane first(
    "lite6-a", "/manyforge_fake_multi_a", "lite6_multi_a_");
  ControllerManagerLane second(
    "lite6-b", "/manyforge_fake_multi_b", "lite6_multi_b_");
  first.start();
  second.start();
  ASSERT_TRUE(first.wait_for_observation_ready());
  ASSERT_TRUE(second.wait_for_observation_ready());
  const auto first_principal = first.operator_caller();
  const auto second_principal = second.operator_caller();

  ASSERT_TRUE(first.open_session(first_principal, 1));
  ASSERT_TRUE(first.enable(first_principal, 1, 1));
  ASSERT_TRUE(first.activate_controller());
  ASSERT_TRUE(second.activate_controller());
  ASSERT_TRUE(first.wait_for_admission(true));
  ASSERT_TRUE(second.wait_for_admission(false));

  const auto second_before = second.write_count();
  first.publish_target(0.15, 100ms);
  ASSERT_TRUE(first.wait_for_write_count_above(0));
  std::this_thread::sleep_for(150ms);
  EXPECT_EQ(second.write_count(), second_before);

  ASSERT_TRUE(second.open_session(second_principal, 1));
  ASSERT_TRUE(second.enable(second_principal, 1, 1));
  ASSERT_TRUE(second.wait_for_admission(false));
  ASSERT_TRUE(second.deactivate_controller());
  ASSERT_TRUE(second.activate_controller());
  ASSERT_TRUE(second.wait_for_admission(true));
  second.publish_target(-0.25, 100ms);
  ASSERT_TRUE(second.wait_for_write_count_above(second_before));
  std::this_thread::sleep_for(150ms);

  ASSERT_TRUE(first.close_session(first_principal, 1));
  ASSERT_TRUE(first.wait_for_admission(false));
  EXPECT_TRUE(second.wait_for_admission(true));
  const auto second_active_count = second.write_count();
  ASSERT_TRUE(second.wait_for_write_count_above(second_active_count));

  first.stop();
  second.stop();
  const auto first_positions = first.last_write();
  const auto second_positions = second.last_write();
  ASSERT_EQ(first_positions.size(), 6U);
  ASSERT_EQ(second_positions.size(), 6U);
  for (const double position : first_positions) {
    EXPECT_NEAR(position, 0.15, 0.02);
  }
  for (const double position : second_positions) {
    EXPECT_NEAR(position, -0.25, 0.02);
  }
}

TEST_F(
  ControllerManagerIntegrationTest,
  ProcessShapeRestartReturnsFailClosed)
{
  {
    ControllerManagerLane initial(
      "lite6-restart", "/manyforge_fake_restart",
      "lite6_restart_");
    initial.start();
    ASSERT_TRUE(initial.wait_for_observation_ready());
    const auto principal = initial.operator_caller();
    ASSERT_TRUE(initial.open_session(principal, 1));
    ASSERT_TRUE(initial.enable(principal, 1, 1));
    ASSERT_TRUE(initial.activate_controller());
    ASSERT_TRUE(initial.wait_for_admission(true));
    initial.publish_target(0.1, 50ms);
    ASSERT_TRUE(initial.wait_for_write_count_above(0));
    initial.stop();
  }

  ControllerManagerLane restarted(
    "lite6-restart", "/manyforge_fake_restart",
    "lite6_restart_");
  restarted.start();
  ASSERT_TRUE(restarted.wait_for_observation_ready());
  ASSERT_TRUE(restarted.activate_controller());
  ASSERT_TRUE(restarted.wait_for_admission(false));
  const auto after_restart = restarted.snapshot();
  EXPECT_FALSE(after_restart.authority_session.has_value());
  EXPECT_EQ(
    after_restart.observed.robot_enabled,
    robot_lifecycle::FactState::kFalse);
  EXPECT_EQ(restarted.write_count(), 0U);
}

}  // namespace
}  // namespace manyforge::xarm_control
