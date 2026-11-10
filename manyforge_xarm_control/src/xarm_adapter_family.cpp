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

// The xArm adapter family `xarm`: one library, registered in the ament resource index
// `manyforge_component_adapters`. One factory call per component, by role:
// - the machine's owner (`machine_owner`): the owner adapter with the driver
//   services, the controller's report and the I/O subscriptions; its handle
//   (`owner_handle`) goes to its members' calls;
// - the arm member (`arm`): the arm adapter on the owner's report, with the
//   tool-context hook (`ApplyToolContext`) and the display joint source (the
//   driver's own `joint_states`, `JointSampleUse`);
// - the I/O members (`controller_io`, `tool_io`): one I/O adapter each, with
//   the signals of its row.
// An xArm arm is always a machine's member.
// Members register with their owner, which names them in its faults'
// `concerns` (the core disables them before the owner's power actions). The family also exports
// the launch contribution and takes the host's measured joints,
// driver-restart entry and I/O update sink.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <ament_index_cpp/get_package_share_path.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#include <manyforge_roboplan/ros2_control_component_adapter.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <manyforge_xarm_control/xarm_adapters.hpp>
#include <manyforge_xarm_control/xarm_launch_contribution.hpp>

namespace {

using manyforge::roboplan_backend::ComponentAdapterContext;
using manyforge::roboplan_backend::ComponentAdapterFamily;
using manyforge::roboplan_backend::ComponentAdapters;
using manyforge::roboplan_backend::DisplayJointSampleUse;
using manyforge::roboplan_backend::DisplayJointSource;
using manyforge::roboplan_backend::LaunchContribution;
using manyforge::roboplan_backend::ModelLaneRow;
using manyforge::roboplan_backend::ToolContextHook;
namespace xarm = manyforge::xarm_control;
namespace core = manyforge::core;

/// Between reads of the arm's waits (20 ms).
constexpr std::chrono::milliseconds kXArmPollInterval{20};

/// How long the driver takes to reactivate its controllers after the re-arm
/// of a TCP offset send (`set_state(0)`); the handle binds them within it.
constexpr std::chrono::nanoseconds kXArmRearmBound = std::chrono::seconds(5);

[[nodiscard]] std::filesystem::path vendor_dir() {
  return ament_index_cpp::get_package_share_path("manyforge_xarm_control") / "vendor";
}

/// The driver packages' share folders (`driver.yaml` base files).
[[nodiscard]] xarm::DriverShare driver_share() {
  return [](const std::string &package) {
    return ament_index_cpp::get_package_share_path(package);
  };
}

template <typename Value> Value value_or_throw(core::Result<Value> made) {
  if (!made.ok()) throw std::invalid_argument(made.error().message);
  return std::move(made.value());
}

LaunchContribution xarm_contribution(const ModelLaneRow &row, const bool arm) {
  return value_or_throw(
      xarm::make_xarm_launch_contribution(row, vendor_dir(), driver_share(), arm));
}

/// The vendor's controllers of an xArm model on another lane (Mock, Isaac).
std::string xarm_controllers(const ModelLaneRow &row) {
  return value_or_throw(xarm::make_xarm_controllers(row, vendor_dir(), driver_share()));
}

/// The machine: the owner adapter and everything it runs on (the clock,
/// the transports, the driver services); members keep it alive.
struct Machine final : manyforge::roboplan_backend::AdapterTransportConsumer {
  std::shared_ptr<core::IClock> clock;
  xarm::XArmVendorSettings settings;
  xarm::XArmMachine parts;
  std::string driver_namespace;
  std::weak_ptr<Owner> transport_owner;
  mutable std::mutex mutex;

  struct Rebind final {
    std::shared_ptr<Owner> old_owner;
    std::shared_ptr<Owner> new_owner;
    Gate gate;
    std::shared_ptr<xarm::XArmRobotStatesTransport> robot;
    std::shared_ptr<xarm::XArmDriverServices> driver;
    std::shared_ptr<xarm::XArmIoStatesTransport> io;
  };

  std::shared_ptr<void> Stage(const std::shared_ptr<Owner> &expected,
                              const std::shared_ptr<Owner> &replacement,
                              Gate gate) override {
    if (!expected || !replacement || !gate || gate->load(std::memory_order_acquire))
      throw std::invalid_argument("xArm ROS rebind requires an inactive gate and owner");
    {
      std::lock_guard lock(mutex);
      if (transport_owner.lock() != expected)
        throw std::invalid_argument("xArm ROS rebind owner changed");
    }
    xarm::XArmRosOwner ros;
    ros.node = replacement->node();
    ros.clock = clock;
    ros.active = gate;
    ros.invoke = [replacement](std::function<void()> work) {
      replacement->invoke_creation(std::move(work));
    };
    ros.retain = [replacement](std::shared_ptr<void> entity) {
      return replacement->retain_entity(std::move(entity));
    };
    ros.time_critical_group = [replacement] {
      return replacement->create_time_critical_callback_group();
    };
    ros.retain_time_critical = [replacement](std::shared_ptr<void> entity,
                                             rclcpp::CallbackGroup::SharedPtr group) {
      return replacement->retain_entity(std::move(entity), std::move(group));
    };
    auto next = std::make_shared<Rebind>();
    next->old_owner = expected;
    next->new_owner = replacement;
    next->gate = std::move(gate);
    next->robot = std::make_shared<xarm::XArmRobotStatesTransport>(
        *ros.node, driver_namespace, *clock, &ros);
    next->driver = std::make_shared<xarm::XArmDriverServices>(
        *ros.node, driver_namespace, &ros);
    next->io = std::make_shared<xarm::XArmIoStatesTransport>(
        *ros.node, driver_namespace, *clock, parts.owner, &ros);
    return next;
  }
  bool Validate(const std::shared_ptr<void> &staged) const override {
    auto next = std::static_pointer_cast<Rebind>(staged);
    if (!next || next->gate->load(std::memory_order_acquire)) return false;
    std::lock_guard lock(mutex);
    return transport_owner.lock() == next->old_owner;
  }
  void Commit(const std::shared_ptr<void> &staged) noexcept override {
    auto next = std::static_pointer_cast<Rebind>(staged);
    std::lock_guard lock(mutex);
    parts.robot_states->SwapRosState(*next->robot);
    parts.owner->ForgetMemberEstops();
    parts.driver->SwapRosState(*next->driver);
    parts.io_states.swap(next->io);
    transport_owner = next->new_owner;
  }
};

/// One member adapter and the machine it runs on.
template <typename Adapter> struct Member final {
  std::shared_ptr<Machine> machine;
  std::shared_ptr<Adapter> adapter;
};

/// The returned adapter shares its member's lifetime.
template <typename Adapter>
std::shared_ptr<Adapter> member_of(std::shared_ptr<Machine> machine,
                                   std::shared_ptr<Adapter> adapter) {
  auto member = std::make_shared<Member<Adapter>>(Member<Adapter>{std::move(machine), adapter});
  return std::shared_ptr<Adapter>(member, adapter.get());
}

[[nodiscard]] std::string row_json(const ComponentAdapterContext &context) {
  return manyforge::content_identity::canonical_json(context.lane_row);
}

std::shared_ptr<Machine> make_machine(const ComponentAdapterContext &context) {
  if (!context.node || !context.clock || !context.lane_adapter || !context.restart_driver)
    throw std::invalid_argument("an xArm machine runs on its ros2_control lane");
  auto machine = std::make_shared<Machine>();
  machine->clock = context.clock;
  machine->transport_owner = context.ros_entity_owner;
  machine->driver_namespace = context.ros_namespace + "/xarm";
  machine->settings = value_or_throw(xarm::load_xarm_vendor_settings(vendor_dir()));
  // The callbacks are used only while constructing the family entities. The
  // retirement handle does not retain the transport owner.
  xarm::XArmRosOwner ros_owner;
  if (context.ros_entity_owner) {
    ros_owner.node = context.node;
    ros_owner.clock = context.clock;
    ros_owner.invoke = [&context](std::function<void()> work) {
      context.ros_entity_owner->invoke_creation(std::move(work));
    };
    ros_owner.retain = [&context](std::shared_ptr<void> entity) {
      return context.ros_entity_owner->retain_entity(std::move(entity));
    };
    ros_owner.time_critical_group = [&context] {
      return context.ros_entity_owner->create_time_critical_callback_group();
    };
    ros_owner.retain_time_critical = [&context](std::shared_ptr<void> entity,
                                                rclcpp::CallbackGroup::SharedPtr group) {
      return context.ros_entity_owner->retain_entity(std::move(entity), std::move(group));
    };
  }
  machine->parts = xarm::make_xarm_machine(
      *context.node, *context.clock,
      xarm::XArmMachineInputs{machine->driver_namespace, context.address, machine->settings,
                              context.lane_adapter, context.restart_driver, context.io_updates,
                              context.lifecycle_command_timeout,
                              context.ros2_control
                                  ? std::function<bool()>(
                                        [handle = std::weak_ptr(context.ros2_control)] {
                                          const auto locked = handle.lock();
                                          return locked && locked->ControllerLaunchSettling();
                                        })
                                  : std::function<bool()>{}},
      context.ros_entity_owner ? &ros_owner : nullptr);
  return machine;
}

/// The arm adapter on its machine's report and driver services.
std::shared_ptr<xarm::XArmArmAdapter> make_arm(const ComponentAdapterContext &context,
                                               const Machine &machine) {
  if (!context.clock || !context.lane_adapter || !context.ros2_control || !context.measured_joints)
    throw std::invalid_argument("an xArm arm runs on its ros2_control lane");
  // The model row's `lanes.<lane>` object is the xArm part of its row.
  const auto row = value_or_throw(
      xarm::parse_xarm_lane_row("{\"lanes\": {\"real\": " + row_json(context) + "}}"));
  return std::make_shared<xarm::XArmArmAdapter>(
      xarm::XArmArmAdapterConfig{row.dof, row.joint_agreement_tolerance_rad,
                                 context.lifecycle_command_timeout},
      xarm::XArmArmAdapterPorts{
          machine.parts.robot_states, machine.parts.driver, context.lane_adapter,
          [handle = context.ros2_control](const bool active, const std::stop_token stop) {
            return manyforge::roboplan_backend::ros2_control_action_result(
                active ? core::ComponentAction::kEnable : core::ComponentAction::kDisable,
                handle->SwitchArmHardware(active, stop));
          },
          [handle = context.ros2_control] { return handle->arm_activity().hardware_active; },
          // The poll of the waits after a re-arm and a TCP offset send.
          [] { std::this_thread::sleep_for(kXArmPollInterval); }, context.measured_joints,
          machine.clock.get(),
          [handle = context.ros2_control] { return handle->arm_activity().broadcaster_active; },
          [handle = context.ros2_control] {
            const auto activity = handle->arm_activity();
            return activity.controller_manager_present && activity.observed &&
                   activity.controller_loaded && activity.broadcaster_loaded &&
                   !activity.controller_active && !activity.broadcaster_active;
          }});
}

/// The component's adapters with the arm's hook and display joints: they
/// live on the arm's ros2_control connection, which the arm adapter
/// composes, so they hold it weakly.
ComponentAdapters with_arm_hooks(std::shared_ptr<core::IComponentAdapter> component,
                                 const ComponentAdapterContext &context,
                                 const std::shared_ptr<xarm::XArmArmAdapter> &arm) {
  ComponentAdapters made{std::move(component), nullptr, std::nullopt, std::nullopt};
  made.tool_context = ToolContextHook{
      [weak = std::weak_ptr(arm)](
          const std::optional<core::Pose3DTO> &offset,
          const std::optional<core::ToolContextPointMassDTO> &payload, const bool at_enable,
          const std::optional<std::chrono::steady_clock::time_point> deadline,
          const std::stop_token stop) -> core::Result<bool> {
        const auto adapter = weak.lock();
        if (!adapter)
          return core::Result<bool>::Err(
              {"execution_rejected", "the robot's adapter has retired",
               {{"reason", "tool_context_not_applied"}, {"effect", "not_started"}}});
        return adapter->ApplyToolContext(offset, payload, at_enable,
                                         xarm::XArmToolContextBound{deadline, stop});
      },
      kXArmRearmBound};
  // The driver publishes its
  // own joint states on every hardware read, in every mode.
  made.display_joints = DisplayJointSource{
      context.ros_namespace + "/xarm/joint_states",
      [weak = std::weak_ptr(arm)](const std::int64_t now_ns,
                                  const std::vector<double> &positions) {
        const auto adapter = weak.lock();
        if (!adapter) return DisplayJointSampleUse::kIgnore;
        switch (adapter->JointSampleUse(now_ns, positions)) {
        case xarm::XArmJointSampleUse::kIgnore:
          return DisplayJointSampleUse::kIgnore;
        case xarm::XArmJointSampleUse::kDisplay:
          return DisplayJointSampleUse::kDisplay;
        case xarm::XArmJointSampleUse::kLeads:
          return DisplayJointSampleUse::kLeads;
        }
        return DisplayJointSampleUse::kIgnore;
      }};
  return made;
}

ComponentAdapters make_xarm(const ComponentAdapterContext &context) {
  const auto &id = context.component_id.value();
  if (context.machine_owner) {
    auto machine = make_machine(context);
    const std::shared_ptr<core::IComponentAdapter> owner(machine, machine->parts.owner.get());
    return {owner, machine, std::nullopt, std::nullopt,
            context.ros_entity_owner ? machine : nullptr};
  }
  if (!context.owner)
    throw std::invalid_argument("an xArm member needs its machine's owner");
  auto machine = std::static_pointer_cast<Machine>(context.owner);
  auto &owner = *machine->parts.owner;
  if (context.role == xarm::kXArmArmRole) {
    const auto arm = member_of(machine, make_arm(context, *machine));
    owner.AddMember({context.role, id, arm, {}, arm});
    auto result = with_arm_hooks(arm, context, arm);
    if (context.ros_entity_owner) result.ros_transport_consumer = machine;
    return result;
  }
  if (context.role == xarm::kXArmControllerIoRole || context.role == xarm::kXArmToolIoRole) {
    if (!context.clock) throw std::invalid_argument("an xArm I/O member needs the host clock");
    auto row = value_or_throw(xarm::parse_xarm_io_row(row_json(context)));
    const auto io = member_of(
        machine,
        std::make_shared<xarm::XArmIoComponent>(
            xarm::XArmIoComponentConfig{id, row.bank, std::move(row.signals),
                                        machine->settings.io, context.lifecycle_command_timeout,
                                        [] { std::this_thread::sleep_for(kXArmPollInterval); }},
            machine->parts.driver,
            [owner = machine->parts.owner](const std::int64_t now_ns) {
              return owner->Usable(now_ns);
            },
            *machine->clock));
    owner.AddMember({context.role, id, io, io, {}});
    return {io, nullptr, std::nullopt, std::nullopt,
            context.ros_entity_owner ? machine : nullptr};
  }
  throw std::invalid_argument("an xArm machine has no member role '" + context.role + "'");
}

constexpr ComponentAdapterFamily kFamily{xarm::kXArmAdapterFamily, &make_xarm,
                                         &xarm_contribution, nullptr, &xarm_controllers};

} // namespace

extern "C" const manyforge::roboplan_backend::ComponentAdapterFamily *
manyforge_component_adapter_family() {
  return &kFamily;
}
