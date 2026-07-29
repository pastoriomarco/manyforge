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

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include <rclcpp/rclcpp.hpp>

#include <manyforge_robot_lifecycle/admission_publication_driver.hpp>
#include <manyforge_robot_lifecycle/local_operator_gateway.hpp>
#include <manyforge_robot_lifecycle_msgs/msg/execution_admission_snapshot.hpp>
#include <manyforge_robot_lifecycle_msgs/msg/lifecycle_capability.hpp>
#include <manyforge_robot_lifecycle_msgs/msg/lifecycle_transaction.hpp>
#include <manyforge_robot_lifecycle_msgs/msg/robot_lifecycle_snapshot.hpp>
#include <manyforge_robot_lifecycle_msgs/srv/close_robot_lifecycle_session.hpp>
#include <manyforge_robot_lifecycle_msgs/srv/open_robot_lifecycle_session.hpp>
#include <manyforge_robot_lifecycle_msgs/srv/reconcile_robot_lifecycle_command.hpp>
#include <manyforge_robot_lifecycle_msgs/srv/submit_robot_lifecycle_command.hpp>
#include <manyforge_xarm_control/lifecycle_ros_port.hpp>

namespace manyforge::xarm_control
{
namespace
{

using manyforge::robot_lifecycle::AdmissionPublication;
using manyforge::robot_lifecycle::AdmissionPublicationDriver;
using manyforge::robot_lifecycle::AdmissionProjectorConfig;
using manyforge::robot_lifecycle::AuthorityProfile;
using manyforge::robot_lifecycle::CallerAssurance;
using manyforge::robot_lifecycle::CallerContext;
using manyforge::robot_lifecycle::LifecycleSnapshot;
using manyforge::robot_lifecycle::LocalAuthoritySessionIntent;
using manyforge::robot_lifecycle::LocalLifecycleCommandIntent;
using manyforge::robot_lifecycle::LocalOperatorGateway;
using manyforge::robot_lifecycle::LocalOperatorGatewayConfig;
using manyforge_robot_lifecycle_msgs::msg::ExecutionAdmissionSnapshot;
using LifecycleCapability =
  manyforge_robot_lifecycle_msgs::msg::LifecycleCapability;
using LifecycleTransaction =
  manyforge_robot_lifecycle_msgs::msg::LifecycleTransaction;
using RobotLifecycleSnapshot =
  manyforge_robot_lifecycle_msgs::msg::RobotLifecycleSnapshot;
using CloseRobotLifecycleSession =
  manyforge_robot_lifecycle_msgs::srv::CloseRobotLifecycleSession;
using OpenRobotLifecycleSession =
  manyforge_robot_lifecycle_msgs::srv::OpenRobotLifecycleSession;
using ReconcileRobotLifecycleCommand =
  manyforge_robot_lifecycle_msgs::srv::ReconcileRobotLifecycleCommand;
using SubmitRobotLifecycleCommand =
  manyforge_robot_lifecycle_msgs::srv::SubmitRobotLifecycleCommand;

constexpr char kAdmissionSchema[] =
  "manyforge.robot_lifecycle.execution_admission.v0.experimental";
constexpr char kLifecycleSnapshotSchema[] =
  "manyforge.robot_lifecycle.snapshot.v1";
constexpr std::int64_t kMaximumAuthorityLeaseNs = 300'000'000'000;

std::string new_component_epoch(std::string_view role)
{
  std::random_device source;
  std::array<std::uint8_t, 16> bytes{};
  for (auto & value : bytes) {
    value = static_cast<std::uint8_t>(source());
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3fU) | 0x80U);
  std::ostringstream result;
  result << role << ':' << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    result << std::setw(2) << static_cast<unsigned>(bytes[index]);
    if (index == 3 || index == 5 || index == 7 || index == 9) {
      result << '-';
    }
  }
  return result.str();
}

CallerAssurance caller_assurance(const std::string & value)
{
  if (value == "unverified") {
    return CallerAssurance::kUnverified;
  }
  if (value == "authenticated") {
    return CallerAssurance::kAuthenticated;
  }
  throw std::invalid_argument(
          "caller_assurance must be unverified or authenticated");
}

CallerContext caller_context(
  const std::string & command_source,
  const std::string & caller_context_id,
  const std::string & assurance,
  std::int64_t now_ns)
{
  CallerContext result;
  result.command_source = command_source;
  result.caller_context_id = caller_context_id;
  result.assurance = caller_assurance(assurance);
  result.asserted_at_ns = now_ns;
  result.valid_until_ns = now_ns + kMaximumAuthorityLeaseNs;
  return result;
}

LifecycleTransaction transaction_message(
  const manyforge::robot_lifecycle::TransactionRecord & record)
{
  LifecycleTransaction message;
  message.generation = record.generation;
  message.request_id = record.request_id;
  message.request_sequence = record.request_sequence;
  message.action_id = record.action_id;
  message.command_source = record.principal_id;
  message.authority_session_generation =
    record.authority_session_generation;
  message.phase =
    manyforge::robot_lifecycle::transaction_phase_name(record.phase);
  message.accepted_at_steady_time_ns = record.accepted_at_ns;
  message.deadline_steady_time_ns = record.deadline_ns;
  message.terminal_at_steady_time_ns = record.terminal_at_ns;
  message.result_reason = record.result_reason;
  return message;
}

std::string required_parameter(
  rclcpp::Node & node,
  const std::string & name)
{
  const auto value = node.declare_parameter<std::string>(name, "");
  if (value.empty()) {
    throw std::invalid_argument(
            "required ROS parameter is empty: " + name);
  }
  return value;
}

AuthorityProfile authority_profile(const std::string & value)
{
  if (value == "local_operator") {
    return AuthorityProfile::kLocalOperator;
  }
  if (value == "plc_roxsie") {
    return AuthorityProfile::kExternalPlc;
  }
  throw std::invalid_argument(
          "authority_profile must be local_operator or plc_roxsie");
}

class Lite6ExecutionAdmissionProvider final : public rclcpp::Node
{
public:
  Lite6ExecutionAdmissionProvider()
  : Node("manyforge_lite6_execution_admission_provider")
  {
    const std::string robot_id =
      required_parameter(*this, "robot_id");
    const std::string provider_instance_id =
      required_parameter(*this, "provider_instance_id");
    robot_id_ = robot_id;
    provider_instance_id_ = provider_instance_id;
    provider_component_epoch_ =
      new_component_epoch("lifecycle-provider");
    const std::string binding_generation =
      required_parameter(*this, "binding_contract_generation_id");
    runtime_generation_id_ =
      required_parameter(*this, "runtime_generation_id");
    cell_supervisor_component_epoch_ =
      required_parameter(*this, "cell_supervisor_component_epoch");
    const std::string admission_topic =
      required_parameter(*this, "execution_admission_topic");
    const std::string lifecycle_status_topic =
      required_parameter(*this, "lifecycle_status_topic");
    const std::string open_session_service =
      required_parameter(*this, "lifecycle_open_session_service");
    const std::string close_session_service =
      required_parameter(*this, "lifecycle_close_session_service");
    const std::string submit_command_service =
      required_parameter(*this, "lifecycle_submit_command_service");
    const std::string reconcile_command_service =
      required_parameter(*this, "lifecycle_reconcile_command_service");
    const std::string controller_manager =
      required_parameter(*this, "controller_manager");
    const auto refresh_period_ms =
      declare_parameter<std::int64_t>("refresh_period_ms", 50);
    const auto maximum_lease_ms =
      declare_parameter<std::int64_t>("maximum_lease_ms", 250);
    const auto observation_max_age_ms =
      declare_parameter<std::int64_t>(
      "observation_max_age_ms", 1'000);
    const auto owner_progress_max_age_ms =
      declare_parameter<std::int64_t>(
      "owner_progress_max_age_ms", 500);
    if (refresh_period_ms <= 0 ||
      maximum_lease_ms <= refresh_period_ms ||
      maximum_lease_ms > 10'000 ||
      observation_max_age_ms <= refresh_period_ms ||
      observation_max_age_ms > 10'000 ||
      owner_progress_max_age_ms <= refresh_period_ms ||
      owner_progress_max_age_ms > 10'000)
    {
      throw std::invalid_argument(
              "admission refresh/lease/observation/owner-progress "
              "parameters are invalid");
    }

    XArmLifecycleRosPortConfig port_config;
    port_config.robot_id = robot_id;
    port_config.supervisor_instance_id =
      required_parameter(*this, "supervisor_instance_id");
    port_config.authority_profile = authority_profile(
      required_parameter(*this, "authority_profile"));
    // Keep lifecycle observation/gate validity independent from the shorter
    // public admission publication lease. The producer's own fresh-until
    // timestamp remains the authoritative upper bound.
    port_config.observation_lease_ns =
      observation_max_age_ms * 1'000'000;
    port_config.transport.owner_id =
      required_parameter(*this, "transport_owner_id");
    port_config.transport.state_topic =
      required_parameter(*this, "supervised_state_topic");
    port_config.transport.execute_service =
      required_parameter(*this, "supervised_execute_service");
    port_config.transport.command_gate_service =
      required_parameter(*this, "supervised_command_gate_service");
    port_config.transport.shutdown_controller_service =
      required_parameter(
      *this, "supervised_shutdown_controller_service");
    port_config.transport.controller_activity_topic =
      required_parameter(*this, "controller_activity_topic");
    port_config.transport.trajectory_controller_name =
      required_parameter(*this, "trajectory_controller_name");
    port_config.transport.controller_manager = controller_manager;
    port_config.transport.hardware_component_name =
      required_parameter(*this, "hardware_component_name");
    port_config.transport.joint_state_broadcaster_name =
      required_parameter(*this, "joint_state_broadcaster_name");
    port_config.transport.observation_max_age_ns =
      observation_max_age_ms * 1'000'000;
    port_config.transport.owner_progress_max_age_ns =
      owner_progress_max_age_ms * 1'000'000;

    const auto qos = rclcpp::QoS(rclcpp::KeepLast(1)).
      reliable().transient_local();
    publisher_ = create_publisher<ExecutionAdmissionSnapshot>(
      admission_topic, qos);
    lifecycle_publisher_ = create_publisher<RobotLifecycleSnapshot>(
      lifecycle_status_topic, qos);
    port_ = std::make_unique<XArmLifecycleRosPort>(
      std::move(port_config));
    gateway_ = std::make_unique<LocalOperatorGateway>(
      LocalOperatorGatewayConfig{
          robot_id,
          kMaximumAuthorityLeaseNs,
          512,
          CallerAssurance::kUnverified,
      },
      *port_);
    AdmissionProjectorConfig projector_config;
    projector_config.provider_instance_id = provider_instance_id;
    projector_config.robot_id = robot_id;
    projector_config.binding_contract_generation_id =
      binding_generation;
    projector_config.maximum_lease_ns =
      maximum_lease_ms * 1'000'000;
    projector_config.runtime_generation_id = runtime_generation_id_;
    projector_config.provider_component_epoch =
      provider_component_epoch_;
    projector_config.cell_supervisor_component_epoch =
      cell_supervisor_component_epoch_;
    driver_ = std::make_unique<AdmissionPublicationDriver>(
      *port_,
      std::move(projector_config),
      [this](const AdmissionPublication & publication) {
        return publish(publication);
      });
    status_callback_group_ = create_callback_group(
      rclcpp::CallbackGroupType::MutuallyExclusive);
    authority_callback_group_ = create_callback_group(
      rclcpp::CallbackGroupType::MutuallyExclusive);
    mutation_callback_group_ = create_callback_group(
      rclcpp::CallbackGroupType::MutuallyExclusive);
    open_session_service_ = create_service<OpenRobotLifecycleSession>(
      open_session_service,
      [this](
        const OpenRobotLifecycleSession::Request::SharedPtr request,
        OpenRobotLifecycleSession::Response::SharedPtr response)
      {
        open_session(*request, *response);
      },
      rclcpp::ServicesQoS(),
      authority_callback_group_);
    close_session_service_ = create_service<CloseRobotLifecycleSession>(
      close_session_service,
      [this](
        const CloseRobotLifecycleSession::Request::SharedPtr request,
        CloseRobotLifecycleSession::Response::SharedPtr response)
      {
        close_session(*request, *response);
      },
      rclcpp::ServicesQoS(),
      authority_callback_group_);
    submit_command_service_ =
      create_service<SubmitRobotLifecycleCommand>(
      submit_command_service,
      [this](
        const SubmitRobotLifecycleCommand::Request::SharedPtr request,
        SubmitRobotLifecycleCommand::Response::SharedPtr response)
      {
        submit_command(*request, *response);
      },
      rclcpp::ServicesQoS(),
      mutation_callback_group_);
    reconcile_command_service_ =
      create_service<ReconcileRobotLifecycleCommand>(
      reconcile_command_service,
      [this](
        const ReconcileRobotLifecycleCommand::Request::SharedPtr request,
        ReconcileRobotLifecycleCommand::Response::SharedPtr response)
      {
        reconcile_command(*request, *response);
      },
      rclcpp::ServicesQoS(),
      mutation_callback_group_);
    timer_ = create_wall_timer(
      std::chrono::milliseconds(refresh_period_ms),
      [this]() {tick();},
      status_callback_group_);
  }

private:
  static std::int64_t steady_now_ns()
  {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  bool publish(const AdmissionPublication & publication)
  {
    try {
      ExecutionAdmissionSnapshot message;
      message.schema = kAdmissionSchema;
      message.provider_instance_id =
        publication.provider_instance_id;
      message.robot_id = publication.robot_id;
      message.binding_contract_generation_id =
        publication.binding_contract_generation_id;
      message.runtime_generation_id =
        publication.runtime_generation_id;
      message.provider_component_epoch =
        publication.provider_component_epoch;
      message.transport_owner_component_epoch =
        publication.transport_owner_component_epoch;
      message.authority_component_epoch =
        publication.authority_component_epoch;
      message.cell_supervisor_component_epoch =
        publication.cell_supervisor_component_epoch;
      message.publication_generation =
        publication.publication_generation;
      message.lifecycle_generation =
        publication.lifecycle_generation;
      message.source_snapshot_generation =
        publication.source_snapshot_generation;
      message.projected_at_steady_time_ns =
        publication.projected_at_steady_time_ns;
      message.lease_duration_ns = publication.lease_duration_ns;
      message.controller_ready = publication.controller_ready;
      message.motion_admitted = publication.motion_admitted;
      message.reason_codes = publication.reason_codes;
      publisher_->publish(std::move(message));
      return true;
    } catch (const std::exception & error) {
      RCLCPP_ERROR(
        get_logger(), "Admission publication failed: %s", error.what());
      return false;
    }
  }

  RobotLifecycleSnapshot lifecycle_message(
    const LifecycleSnapshot & snapshot) const
  {
    RobotLifecycleSnapshot message;
    message.schema = kLifecycleSnapshotSchema;
    message.provider_instance_id = provider_instance_id_;
    message.robot_id = snapshot.robot_id;
    message.supervisor_instance_id = snapshot.supervisor_instance_id;
    message.authority_profile =
      manyforge::robot_lifecycle::authority_profile_name(
      snapshot.authority_profile);
    message.runtime_generation_id = runtime_generation_id_;
    message.provider_component_epoch = provider_component_epoch_;
    message.transport_owner_component_epoch =
      snapshot.observed.transport_owner_component_epoch;
    message.authority_component_epoch =
      snapshot.authority_session.has_value() ?
      snapshot.authority_session->component_epoch : "";
    message.cell_supervisor_component_epoch =
      cell_supervisor_component_epoch_;
    message.transport_owner_progress_sequence =
      snapshot.transport_owner_progress_sequence;
    message.transport_owner_progress_received_steady_time_ns =
      snapshot.transport_owner_progress_received_timestamp_ns;
    message.transport_owner_progress_healthy =
      snapshot.transport_owner_progress_healthy;
    message.transport_owner_progress_reason =
      snapshot.transport_owner_progress_reason;
    message.snapshot_generation = snapshot.generation;
    message.capability_generation = snapshot.capability_generation;
    message.observation_generation = snapshot.observed.generation;
    message.source_steady_time_ns =
      snapshot.observed.source_timestamp_ns;
    message.received_steady_time_ns =
      snapshot.observed.received_timestamp_ns;
    message.fresh_until_steady_time_ns =
      snapshot.observed.fresh_until_ns;
    message.transport_connected =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.transport_connected);
    message.identity_verified =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.identity_verified);
    message.position_valid =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.position_valid);
    message.robot_enabled =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.robot_enabled);
    message.vendor_control_ready =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.vendor_control_ready);
    message.hardware_component_active =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.hardware_component_active);
    message.trajectory_controller_active =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.trajectory_controller_active);
    message.fault_present =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.fault_present);
    message.warning_present =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.warning_present);
    message.recovery_required =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.recovery_required);
    message.stationary =
      manyforge::robot_lifecycle::fact_state_name(
      snapshot.observed.stationary);
    message.observation_fresh = snapshot.observation_fresh;
    message.authority_fresh = snapshot.authority_fresh;
    message.observe_ready = snapshot.observe_ready;
    message.controller_ready = snapshot.controller_ready;
    message.motion_admitted = snapshot.motion_admitted;
    message.reconciliation_required =
      snapshot.reconciliation_required;
    message.admission_reason_codes = snapshot.admission_reason_codes;
    message.adapter_status_keys =
      snapshot.observed.adapter_status_keys;
    message.adapter_status_values =
      snapshot.observed.adapter_status_values;
    if (snapshot.authority_session.has_value()) {
      message.authority_session_id =
        snapshot.authority_session->session_id;
      message.authority_session_generation =
        snapshot.authority_session->generation;
      message.command_source =
        snapshot.authority_session->principal_id;
      message.authority_valid_until_steady_time_ns =
        snapshot.authority_session->valid_until_ns;
    }
    for (const auto & capability : snapshot.capabilities) {
      LifecycleCapability item;
      item.action_id = capability.action_id;
      item.label = capability.label;
      item.supported = capability.supported;
      item.available = capability.available;
      item.availability_reason = capability.availability_reason;
      item.confirmation_class =
        manyforge::robot_lifecycle::confirmation_class_name(
        capability.confirmation);
      item.side_effect_summary = capability.side_effect_summary;
      item.expected_postcondition = capability.expected_postcondition;
      item.timeout_ns = capability.timeout_ns;
      message.capabilities.push_back(std::move(item));
    }
    if (snapshot.pending_transaction.has_value()) {
      message.has_pending_transaction = true;
      message.pending_transaction =
        transaction_message(*snapshot.pending_transaction);
    }
    if (snapshot.last_terminal_transaction.has_value()) {
      message.has_last_terminal_transaction = true;
      message.last_terminal_transaction =
        transaction_message(*snapshot.last_terminal_transaction);
    }
    return message;
  }

  void open_session(
    const OpenRobotLifecycleSession::Request & request,
    OpenRobotLifecycleSession::Response & response)
  {
    const auto now_ns = steady_now_ns();
    try {
      if (request.robot_id != robot_id_ ||
        request.authority_component_epoch.empty() ||
        request.lease_duration_ns <= 0 ||
        request.lease_duration_ns > kMaximumAuthorityLeaseNs)
      {
        response.reason = "authority_session_request_invalid";
      } else {
        LocalAuthoritySessionIntent intent;
        intent.session_id = request.session_id;
        intent.generation = request.session_generation;
        intent.valid_until_ns = now_ns + request.lease_duration_ns;
        intent.component_epoch = request.authority_component_epoch;
        const auto result = gateway_->open_session(
          caller_context(
            request.command_source, request.caller_context_id,
            request.caller_assurance, now_ns),
          intent, now_ns);
        response.accepted = result.accepted;
        response.reason = result.reason;
      }
    } catch (const std::exception & error) {
      response.reason = error.what();
    }
    response.snapshot_generation =
      port_->lifecycle_snapshot(now_ns).generation;
  }

  void close_session(
    const CloseRobotLifecycleSession::Request & request,
    CloseRobotLifecycleSession::Response & response)
  {
    const auto now_ns = steady_now_ns();
    try {
      if (request.robot_id != robot_id_) {
        response.reason = "robot_identity_mismatch";
      } else {
        const auto result = gateway_->close_session(
          caller_context(
            request.command_source, request.caller_context_id,
            request.caller_assurance, now_ns),
          request.session_id, request.session_generation, now_ns);
        response.accepted = result.accepted;
        response.reason = result.reason;
      }
    } catch (const std::exception & error) {
      response.reason = error.what();
    }
    response.snapshot_generation =
      port_->lifecycle_snapshot(now_ns).generation;
  }

  void submit_command(
    const SubmitRobotLifecycleCommand::Request & request,
    SubmitRobotLifecycleCommand::Response & response)
  {
    const auto now_ns = steady_now_ns();
    try {
      if (request.robot_id != robot_id_) {
        response.reason = "robot_identity_mismatch";
      } else {
        LocalLifecycleCommandIntent intent;
        intent.request_id = request.request_id;
        intent.request_sequence = request.request_sequence;
        intent.action_id = request.action_id;
        intent.authority_session_id = request.session_id;
        intent.authority_session_generation =
          request.session_generation;
        intent.expected_snapshot_generation =
          request.expected_snapshot_generation;
        intent.confirmation_evidence = request.confirmed ?
          "operator_confirmed" : "";
        const auto result = gateway_->submit(
          caller_context(
            request.command_source, request.caller_context_id,
            request.caller_assurance, now_ns),
          intent, now_ns);
        response.accepted = result.accepted;
        response.reason = result.reason;
        if (result.command_decision.has_value()) {
          response.disposition =
            manyforge::robot_lifecycle::command_disposition_name(
            result.command_decision->disposition);
          response.transaction_generation =
            result.command_decision->transaction_generation;
          response.transaction_phase =
            manyforge::robot_lifecycle::transaction_phase_name(
            result.command_decision->phase);
          response.deadline_steady_time_ns =
            result.command_decision->deadline_ns;
        }
      }
    } catch (const std::exception & error) {
      response.reason = error.what();
    }
    response.snapshot_generation =
      port_->lifecycle_snapshot(now_ns).generation;
  }

  void reconcile_command(
    const ReconcileRobotLifecycleCommand::Request & request,
    ReconcileRobotLifecycleCommand::Response & response)
  {
    const auto now_ns = steady_now_ns();
    try {
      if (request.robot_id != robot_id_) {
        response.reason = "robot_identity_mismatch";
      } else {
        const auto result = gateway_->reconcile(
          caller_context(
            request.command_source, request.caller_context_id,
            request.caller_assurance, now_ns),
          request.transaction_generation, request.request_id,
          request.reason, now_ns);
        response.accepted = result.accepted;
        response.reason = result.reason;
      }
    } catch (const std::exception & error) {
      response.reason = error.what();
    }
    response.snapshot_generation =
      port_->lifecycle_snapshot(now_ns).generation;
  }

  void tick()
  {
    const auto result = driver_->tick(steady_now_ns());
    if (!result.projection_succeeded || !result.sink_succeeded) {
      RCLCPP_ERROR_THROTTLE(
        get_logger(), steady_clock_, 1000,
        "Execution admission projection/publication is unavailable");
    }
    try {
      auto message =
        lifecycle_message(port_->lifecycle_snapshot(steady_now_ns()));
      message.provider_publication_sequence =
        ++provider_publication_sequence_;
      message.provider_published_steady_time_ns = steady_now_ns();
      lifecycle_publisher_->publish(std::move(message));
    } catch (const std::exception & error) {
      RCLCPP_ERROR_THROTTLE(
        get_logger(), steady_clock_, 1000,
        "Robot lifecycle status publication failed: %s", error.what());
    }
  }

  std::string robot_id_;
  std::string provider_instance_id_;
  std::string runtime_generation_id_;
  std::string provider_component_epoch_;
  std::string cell_supervisor_component_epoch_;
  std::uint64_t provider_publication_sequence_ = 0;
  rclcpp::Clock steady_clock_{RCL_STEADY_TIME};
  rclcpp::Publisher<ExecutionAdmissionSnapshot>::SharedPtr publisher_;
  rclcpp::Publisher<RobotLifecycleSnapshot>::SharedPtr
    lifecycle_publisher_;
  std::unique_ptr<XArmLifecycleRosPort> port_;
  std::unique_ptr<LocalOperatorGateway> gateway_;
  std::unique_ptr<AdmissionPublicationDriver> driver_;
  rclcpp::CallbackGroup::SharedPtr status_callback_group_;
  rclcpp::CallbackGroup::SharedPtr authority_callback_group_;
  rclcpp::CallbackGroup::SharedPtr mutation_callback_group_;
  rclcpp::Service<OpenRobotLifecycleSession>::SharedPtr
    open_session_service_;
  rclcpp::Service<CloseRobotLifecycleSession>::SharedPtr
    close_session_service_;
  rclcpp::Service<SubmitRobotLifecycleCommand>::SharedPtr
    submit_command_service_;
  rclcpp::Service<ReconcileRobotLifecycleCommand>::SharedPtr
    reconcile_command_service_;
  rclcpp::TimerBase::SharedPtr timer_;
};

}  // namespace
}  // namespace manyforge::xarm_control

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  int result = 0;
  try {
    auto node = std::make_shared<
      manyforge::xarm_control::Lite6ExecutionAdmissionProvider>();
    rclcpp::executors::MultiThreadedExecutor executor(
      rclcpp::ExecutorOptions(), 4);
    executor.add_node(node);
    executor.spin();
  } catch (const std::exception & error) {
    RCLCPP_FATAL(
      rclcpp::get_logger("manyforge_lite6_execution_admission_provider"),
      "Provider startup failed: %s", error.what());
    result = 1;
  }
  rclcpp::shutdown();
  return result;
}
