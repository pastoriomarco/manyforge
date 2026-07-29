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

#include <manyforge_xarm_control/system_hardware.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <controller_manager_msgs/msg/controller_manager_activity.hpp>
#include <hardware_interface/types/hardware_interface_type_values.hpp>
#include <lifecycle_msgs/msg/state.hpp>
#include <pluginlib/class_list_macros.hpp>

#include <manyforge_xarm_control/fake_hardware_backend.hpp>

namespace manyforge::xarm_control
{
namespace
{

constexpr std::string_view kFakeBackend = "fake";
constexpr std::string_view kFakeConfirmation =
  "ALLOW_TEST_ONLY_FAKE_BACKEND";
constexpr std::int64_t kDefaultObservationLeaseMs = 250;
constexpr std::int64_t kMaximumObservationLeaseMs = 60'000;
constexpr std::int64_t kDefaultObservationRefreshMs = 50;
constexpr std::size_t kMaximumJointCount = 32;

bool has_interface(
  const std::vector<hardware_interface::InterfaceInfo> & interfaces,
  std::string_view name)
{
  return std::any_of(
    interfaces.begin(), interfaces.end(),
    [name](const hardware_interface::InterfaceInfo & interface) {
      return interface.name == name;
    });
}

bool has_supported_command_interfaces(
  const hardware_interface::ComponentInfo & joint)
{
  return joint.command_interfaces.size() == 2 &&
         has_interface(
    joint.command_interfaces, hardware_interface::HW_IF_POSITION) &&
         has_interface(
    joint.command_interfaces, hardware_interface::HW_IF_VELOCITY);
}

bool has_supported_state_interfaces(
  const hardware_interface::ComponentInfo & joint)
{
  return joint.state_interfaces.size() == 2 &&
         has_interface(
    joint.state_interfaces, hardware_interface::HW_IF_POSITION) &&
         has_interface(
    joint.state_interfaces, hardware_interface::HW_IF_VELOCITY);
}

bool belongs_to_joint(
  std::string_view interface_name,
  std::string_view joint_name)
{
  if (
    interface_name.size() <= joint_name.size() ||
    interface_name.compare(0, joint_name.size(), joint_name) != 0)
  {
    return false;
  }
  const auto suffix = interface_name.substr(joint_name.size());
  return suffix == "/position" || suffix == "/velocity";
}

bool parse_positive_milliseconds(
  const std::string & value, std::int64_t & result)
{
  std::int64_t parsed = 0;
  const auto conversion = std::from_chars(
    value.data(), value.data() + value.size(), parsed);
  if (conversion.ec != std::errc{} ||
    conversion.ptr != value.data() + value.size() ||
    parsed <= 0 || parsed > kMaximumObservationLeaseMs)
  {
    return false;
  }
  result = parsed;
  return true;
}

bool parse_authority_profile(
  std::string_view value,
  robot_lifecycle::AuthorityProfile & result)
{
  if (value == "local_operator") {
    result = robot_lifecycle::AuthorityProfile::kLocalOperator;
    return true;
  }
  if (value == "external_plc") {
    result = robot_lifecycle::AuthorityProfile::kExternalPlc;
    return true;
  }
  return false;
}

}  // namespace

CallbackReturn ManyForgeXArmSystemHardware::on_init(
  const hardware_interface::HardwareComponentInterfaceParams & params)
{
  std::scoped_lock lock(nrt_mutex_);
  release_backend();
  trajectory_controller_name_.clear();
  controller_activity_topic_.clear();
  observation_lease_ns_ = 0;
  observation_refresh_ms_ = 0;
  if (hardware_interface::SystemInterface::on_init(params) !=
    CallbackReturn::SUCCESS)
  {
    return CallbackReturn::ERROR;
  }
  hardware_info_ = params.hardware_info;

  const std::string * backend = hardware_parameter("backend");
  const std::string * fake_confirmation =
    hardware_parameter("fake_backend_confirmation");
  const std::string * robot_id = hardware_parameter("robot_id");
  const std::string * owner_id = hardware_parameter("transport_owner_id");
  const std::string * authority_profile =
    hardware_parameter("authority_profile");
  const std::string * trajectory_controller_name =
    hardware_parameter("trajectory_controller_name");
  if (backend == nullptr || *backend != kFakeBackend ||
    robot_id == nullptr || robot_id->empty() ||
    owner_id == nullptr || owner_id->empty() ||
    authority_profile == nullptr ||
    trajectory_controller_name == nullptr ||
    trajectory_controller_name->empty())
  {
    return CallbackReturn::ERROR;
  }
  if (fake_confirmation == nullptr ||
    *fake_confirmation != kFakeConfirmation)
  {
    return CallbackReturn::ERROR;
  }

  robot_lifecycle::AuthorityProfile selected_profile;
  if (!parse_authority_profile(*authority_profile, selected_profile)) {
    return CallbackReturn::ERROR;
  }
  if (hardware_info_.joints.empty() ||
    hardware_info_.joints.size() > kMaximumJointCount)
  {
    return CallbackReturn::ERROR;
  }
  const bool joints_valid = std::all_of(
    hardware_info_.joints.begin(), hardware_info_.joints.end(),
    [](const hardware_interface::ComponentInfo & joint) {
      return !joint.name.empty() &&
             has_supported_command_interfaces(joint) &&
             has_supported_state_interfaces(joint);
    });
  if (!joints_valid) {
    return CallbackReturn::ERROR;
  }

  std::int64_t observation_lease_ms = kDefaultObservationLeaseMs;
  if (const std::string * configured =
    hardware_parameter("observation_lease_ms");
    configured != nullptr &&
    !parse_positive_milliseconds(*configured, observation_lease_ms))
  {
    return CallbackReturn::ERROR;
  }
  constexpr std::int64_t kNanosecondsPerMillisecond = 1'000'000;
  const std::int64_t observation_lease_ns =
    observation_lease_ms * kNanosecondsPerMillisecond;
  std::int64_t observation_refresh_ms = kDefaultObservationRefreshMs;
  if (const std::string * configured =
    hardware_parameter("observation_refresh_ms");
    configured != nullptr &&
    !parse_positive_milliseconds(*configured, observation_refresh_ms))
  {
    return CallbackReturn::ERROR;
  }
  if (observation_refresh_ms >= observation_lease_ms) {
    return CallbackReturn::ERROR;
  }
  std::string activity_topic = "/controller_manager/activity";
  if (const std::string * configured =
    hardware_parameter("controller_manager_activity_topic");
    configured != nullptr)
  {
    if (configured->empty()) {
      return CallbackReturn::ERROR;
    }
    activity_topic = *configured;
  }

  const std::size_t joint_count = hardware_info_.joints.size();
  position_commands_.assign(joint_count, 0.0);
  velocity_commands_.assign(joint_count, 0.0);
  position_states_.assign(joint_count, 0.0);
  velocity_states_.assign(joint_count, 0.0);
  joint_state_sample_.positions.assign(joint_count, 0.0);
  joint_state_sample_.velocities.assign(joint_count, 0.0);
  trajectory_controller_name_ = *trajectory_controller_name;
  controller_activity_topic_ = std::move(activity_topic);
  authority_profile_ = selected_profile;
  observation_lease_ns_ = observation_lease_ns;
  observation_refresh_ms_ = observation_refresh_ms;
  return CallbackReturn::SUCCESS;
}

CallbackReturn ManyForgeXArmSystemHardware::on_configure(
  const rclcpp_lifecycle::State &)
{
  std::scoped_lock lock(nrt_mutex_);
  if (coordinator_ != nullptr || !configure_backend()) {
    release_backend();
    return CallbackReturn::ERROR;
  }
  try {
    configure_nrt_callbacks(
      controller_activity_topic_, observation_refresh_ms_);
  } catch (const std::exception &) {
    release_backend();
    return CallbackReturn::ERROR;
  }
  return CallbackReturn::SUCCESS;
}

CallbackReturn ManyForgeXArmSystemHardware::on_cleanup(
  const rclcpp_lifecycle::State &)
{
  std::scoped_lock lock(nrt_mutex_);
  release_backend();
  return CallbackReturn::SUCCESS;
}

CallbackReturn ManyForgeXArmSystemHardware::on_shutdown(
  const rclcpp_lifecycle::State &)
{
  std::scoped_lock lock(nrt_mutex_);
  release_backend();
  return CallbackReturn::SUCCESS;
}

CallbackReturn ManyForgeXArmSystemHardware::on_error(
  const rclcpp_lifecycle::State &)
{
  std::scoped_lock lock(nrt_mutex_);
  release_backend();
  return CallbackReturn::SUCCESS;
}

std::vector<hardware_interface::StateInterface>
ManyForgeXArmSystemHardware::export_state_interfaces()
{
  std::vector<hardware_interface::StateInterface> interfaces;
  interfaces.reserve(hardware_info_.joints.size() * 2);
  for (std::size_t index = 0; index < hardware_info_.joints.size(); ++index) {
    interfaces.emplace_back(
      hardware_info_.joints[index].name,
      hardware_interface::HW_IF_POSITION, &position_states_[index]);
    interfaces.emplace_back(
      hardware_info_.joints[index].name,
      hardware_interface::HW_IF_VELOCITY, &velocity_states_[index]);
  }
  return interfaces;
}

std::vector<hardware_interface::CommandInterface>
ManyForgeXArmSystemHardware::export_command_interfaces()
{
  std::vector<hardware_interface::CommandInterface> interfaces;
  interfaces.reserve(hardware_info_.joints.size() * 2);
  for (std::size_t index = 0; index < hardware_info_.joints.size(); ++index) {
    interfaces.emplace_back(
      hardware_info_.joints[index].name,
      hardware_interface::HW_IF_POSITION, &position_commands_[index]);
    interfaces.emplace_back(
      hardware_info_.joints[index].name,
      hardware_interface::HW_IF_VELOCITY, &velocity_commands_[index]);
  }
  return interfaces;
}

CallbackReturn ManyForgeXArmSystemHardware::on_activate(
  const rclcpp_lifecycle::State &)
{
  std::scoped_lock lock(nrt_mutex_);
  if (coordinator_ == nullptr) {
    return CallbackReturn::ERROR;
  }
  coordinator_->set_hardware_component_active(true);
  if (!coordinator_->read_joint_state(joint_state_sample_) ||
    joint_state_sample_.positions.size() != position_states_.size() ||
    joint_state_sample_.velocities.size() != velocity_states_.size())
  {
    coordinator_->set_hardware_component_active(false);
    return CallbackReturn::ERROR;
  }
  position_states_ = joint_state_sample_.positions;
  velocity_states_ = joint_state_sample_.velocities;
  position_commands_ = joint_state_sample_.positions;
  velocity_commands_.assign(velocity_states_.size(), 0.0);
  if (!coordinator_->refresh(steady_now_ns())) {
    coordinator_->set_hardware_component_active(false);
    return CallbackReturn::ERROR;
  }
  return CallbackReturn::SUCCESS;
}

CallbackReturn ManyForgeXArmSystemHardware::on_deactivate(
  const rclcpp_lifecycle::State &)
{
  std::scoped_lock lock(nrt_mutex_);
  if (coordinator_ == nullptr) {
    return CallbackReturn::ERROR;
  }
  coordinator_->set_trajectory_controller_active(false);
  coordinator_->set_hardware_component_active(false);
  return coordinator_->refresh(steady_now_ns()) ?
         CallbackReturn::SUCCESS : CallbackReturn::ERROR;
}

hardware_interface::return_type
ManyForgeXArmSystemHardware::perform_command_mode_switch(
  const std::vector<std::string> &,
  const std::vector<std::string> & stop_interfaces)
{
  const bool releases_our_command = std::any_of(
    stop_interfaces.begin(), stop_interfaces.end(),
    [this](const std::string & interface_name)
    {
      return std::any_of(
        hardware_info_.joints.begin(), hardware_info_.joints.end(),
        [&interface_name](
          const hardware_interface::ComponentInfo & joint)
        {
          return belongs_to_joint(interface_name, joint.name);
        });
    });
  if (releases_our_command) {
    std::copy(
      position_states_.begin(),
      position_states_.end(),
      position_commands_.begin());
    std::fill(
      velocity_commands_.begin(),
      velocity_commands_.end(),
      0.0);
  }
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type ManyForgeXArmSystemHardware::read(
  const rclcpp::Time &, const rclcpp::Duration &)
{
  if (coordinator_ == nullptr) {
    return hardware_interface::return_type::ERROR;
  }
  if (!coordinator_->read_joint_state(joint_state_sample_) ||
    joint_state_sample_.positions.size() != position_states_.size() ||
    joint_state_sample_.velocities.size() != velocity_states_.size())
  {
    return hardware_interface::return_type::ERROR;
  }
  std::copy(
    joint_state_sample_.positions.begin(),
    joint_state_sample_.positions.end(),
    position_states_.begin());
  std::copy(
    joint_state_sample_.velocities.begin(),
    joint_state_sample_.velocities.end(),
    velocity_states_.begin());
  return hardware_interface::return_type::OK;
}

hardware_interface::return_type ManyForgeXArmSystemHardware::write(
  const rclcpp::Time &, const rclcpp::Duration &)
{
  if (coordinator_ == nullptr) {
    return hardware_interface::return_type::ERROR;
  }
  const std::int64_t now_ns = steady_now_ns();
  if (!coordinator_->motion_admitted(now_ns)) {
    return hardware_interface::return_type::OK;
  }
  const JointCommandView command{position_commands_, velocity_commands_};
  if (coordinator_->write_joint_command(command, now_ns)) {
    return hardware_interface::return_type::OK;
  }

  // Admission can be revoked by the NRT lifecycle thread after the first
  // check but before the transport gate observes this write. That refusal is
  // an expected fail-closed outcome, not a hardware fault: returning ERROR
  // would make controller_manager deactivate the hardware component and turn
  // an ordinary authority revocation into an unrelated recovery operation.
  // A refusal while admission is still current remains a genuine write
  // failure.
  return coordinator_->motion_admitted(steady_now_ns()) ?
         hardware_interface::return_type::ERROR :
         hardware_interface::return_type::OK;
}

bool ManyForgeXArmSystemHardware::establish_authority_session(
  const robot_lifecycle::AuthoritySession & session,
  std::int64_t)
{
  std::scoped_lock lock(nrt_mutex_);
  return coordinator_ != nullptr &&
         coordinator_->establish_authority_session(
    session, steady_now_ns());
}

bool ManyForgeXArmSystemHardware::clear_authority_session(
  std::string_view session_id,
  std::uint64_t session_generation,
  std::int64_t)
{
  std::scoped_lock lock(nrt_mutex_);
  return coordinator_ != nullptr &&
         coordinator_->clear_authority_session(
    session_id, session_generation, steady_now_ns());
}

robot_lifecycle::CommandDecision
ManyForgeXArmSystemHardware::submit_lifecycle(
  const robot_lifecycle::CommandRequest & request,
  std::int64_t)
{
  std::scoped_lock lock(nrt_mutex_);
  if (coordinator_ == nullptr) {
    robot_lifecycle::CommandDecision result;
    result.reason = "hardware_not_initialized";
    return result;
  }
  return coordinator_->submit(request, steady_now_ns());
}

bool ManyForgeXArmSystemHardware::reconcile_lifecycle(
  std::uint64_t transaction_generation,
  std::string_view request_id,
  std::string reason,
  std::int64_t)
{
  std::scoped_lock lock(nrt_mutex_);
  return coordinator_ != nullptr &&
         coordinator_->reconcile(
    transaction_generation, request_id, std::move(reason),
    steady_now_ns());
}

bool ManyForgeXArmSystemHardware::refresh_lifecycle(std::int64_t)
{
  std::scoped_lock lock(nrt_mutex_);
  return coordinator_ != nullptr &&
         coordinator_->refresh(steady_now_ns());
}

void ManyForgeXArmSystemHardware::set_trajectory_controller_active(
  bool active)
{
  std::scoped_lock lock(nrt_mutex_);
  if (coordinator_ != nullptr) {
    coordinator_->set_trajectory_controller_active(active);
    coordinator_->refresh(steady_now_ns());
  }
}

robot_lifecycle::LifecycleSnapshot
ManyForgeXArmSystemHardware::lifecycle_snapshot(
  std::int64_t) const
{
  std::scoped_lock lock(nrt_mutex_);
  if (coordinator_ == nullptr) {
    return {};
  }
  return coordinator_->snapshot(steady_now_ns());
}

std::string_view ManyForgeXArmSystemHardware::transport_owner_id() const
noexcept
{
  return coordinator_ == nullptr ? std::string_view{} :
         coordinator_->owner_id();
}

std::size_t
ManyForgeXArmSystemHardware::test_only_fake_joint_write_count() const
noexcept
{
  return fake_backend_view_ == nullptr ? 0 :
         fake_backend_view_->joint_write_count();
}

std::vector<double>
ManyForgeXArmSystemHardware::test_only_fake_last_joint_write() const
{
  if (fake_backend_view_ == nullptr) {
    return {};
  }
  return fake_backend_view_->last_joint_write();
}

std::vector<double>
ManyForgeXArmSystemHardware::test_only_fake_last_joint_velocity_write() const
{
  if (fake_backend_view_ == nullptr) {
    return {};
  }
  return fake_backend_view_->last_joint_velocity_write();
}

std::int64_t ManyForgeXArmSystemHardware::steady_now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
}

const std::string * ManyForgeXArmSystemHardware::hardware_parameter(
  std::string_view name) const
{
  const auto found = hardware_info_.hardware_parameters.find(
    std::string(name));
  return found == hardware_info_.hardware_parameters.end() ?
         nullptr : &found->second;
}

bool ManyForgeXArmSystemHardware::configure_backend()
{
  const std::string * robot_id = hardware_parameter("robot_id");
  const std::string * owner_id = hardware_parameter("transport_owner_id");
  if (robot_id == nullptr || owner_id == nullptr ||
    observation_lease_ns_ <= 0)
  {
    return false;
  }
  try {
    auto fake_backend = std::make_unique<FakeXArmHardwareBackend>(
      *owner_id, hardware_info_.joints.size(), observation_lease_ns_);
    fake_backend_view_ = fake_backend.get();
    std::unique_ptr<IXArmHardwareBackend> backend_owner =
      std::move(fake_backend);
    auto action_plans = make_fake_xarm_action_plans();
    robot_lifecycle::SupervisorConfig supervisor_config;
    supervisor_config.robot_id = *robot_id;
    supervisor_config.supervisor_instance_id =
      hardware_info_.name + ":lifecycle";
    supervisor_config.authority_profile = authority_profile_;
    coordinator_ = std::make_unique<XArmLifecycleCoordinator>(
      std::move(supervisor_config), std::move(backend_owner),
      std::move(action_plans), observation_lease_ns_);
    if (!coordinator_->refresh(steady_now_ns())) {
      release_backend();
      return false;
    }
  } catch (const std::exception &) {
    release_backend();
    return false;
  }
  return true;
}

void ManyForgeXArmSystemHardware::release_backend()
{
  observation_refresh_timer_.reset();
  controller_activity_subscription_.reset();
  if (coordinator_ != nullptr) {
    coordinator_->set_trajectory_controller_active(false);
    coordinator_->set_hardware_component_active(false);
  }
  fake_backend_view_ = nullptr;
  coordinator_.reset();
}

void ManyForgeXArmSystemHardware::configure_nrt_callbacks(
  std::string activity_topic,
  std::int64_t refresh_period_ms)
{
  const auto node = get_node();
  if (node == nullptr) {
    return;
  }
  auto activity_qos = rclcpp::QoS(rclcpp::KeepLast(1));
  activity_qos.reliable();
  activity_qos.transient_local();
  controller_activity_subscription_ =
    node->create_subscription<
    controller_manager_msgs::msg::ControllerManagerActivity>(
    std::move(activity_topic), activity_qos,
    [this](
      const controller_manager_msgs::msg::ControllerManagerActivity::
      SharedPtr activity)
    {
      handle_controller_activity(*activity);
    });
  observation_refresh_timer_ = node->create_wall_timer(
    std::chrono::milliseconds(refresh_period_ms),
    [this]()
    {
      std::scoped_lock callback_lock(nrt_mutex_);
      if (coordinator_ != nullptr) {
        coordinator_->refresh(steady_now_ns());
      }
    });
}

void ManyForgeXArmSystemHardware::handle_controller_activity(
  const controller_manager_msgs::msg::ControllerManagerActivity & activity)
{
  const bool active = std::any_of(
    activity.controllers.begin(), activity.controllers.end(),
    [this](
      const controller_manager_msgs::msg::NamedLifecycleState &
      controller)
    {
      return controller.name == trajectory_controller_name_ &&
             controller.state.id ==
             lifecycle_msgs::msg::State::PRIMARY_STATE_ACTIVE;
    });
  std::scoped_lock lock(nrt_mutex_);
  if (coordinator_ != nullptr) {
    coordinator_->set_trajectory_controller_active(active);
    coordinator_->refresh(steady_now_ns());
  }
}

}  // namespace manyforge::xarm_control

PLUGINLIB_EXPORT_CLASS(
  manyforge::xarm_control::ManyForgeXArmSystemHardware,
  hardware_interface::SystemInterface)
