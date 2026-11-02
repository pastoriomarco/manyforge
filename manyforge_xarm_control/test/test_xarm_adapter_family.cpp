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

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include <rclcpp/rclcpp.hpp>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <yaml-cpp/yaml.h>

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/io_output_writer.hpp>
#include <manyforge_roboplan/retained_ros2_control_lane_config.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>

namespace manyforge::xarm_control {
namespace {

using roboplan_backend::ComponentAdapterContext;
using roboplan_backend::ComponentAdapterRegistry;

/// The machine's ros2_control lane: prepared, does nothing.
class StillLane final : public core::IComponentAdapter {
public:
  [[nodiscard]] core::ComponentState State(std::int64_t now_ns) const override {
    core::ComponentState made;
    made.lifecycle = core::ComponentLifecycle::kPrepared;
    made.observed_at_ns = now_ns;
    return made;
  }
  [[nodiscard]] core::ActionResult Run(core::ComponentAction action, std::stop_token) override {
    core::ActionResult made;
    made.action = action;
    made.outcome = core::ActionOutcome::kSucceeded;
    return made;
  }
  [[nodiscard]] std::vector<core::Capability> capabilities() const override { return {}; }
};

/// A test install prefix: the family under `xarm` and this package's share
/// folder with its vendor folder.
[[nodiscard]] core::Result<ComponentAdapterRegistry> load_family() {
  const std::filesystem::path library(MANYFORGE_XARM_ADAPTER_FAMILY_LIBRARY);
  const auto prefix = std::filesystem::temp_directory_path() /
                      ("manyforge_xarm_family_index_" + std::to_string(::getpid()));
  std::filesystem::remove_all(prefix);
  const auto index = prefix / "share" / "ament_index" / "resource_index";
  const auto adapters = index / std::string(roboplan_backend::kComponentAdapterResourceType);
  std::filesystem::create_directories(adapters);
  std::filesystem::create_directories(index / "packages");
  std::filesystem::create_directories(prefix / "lib");
  std::filesystem::create_directories(prefix / "share" / "manyforge_xarm_control");
  std::ofstream(adapters / "xarm") << library.filename().string() << '\n';
  std::ofstream(index / "packages" / "manyforge_xarm_control");
  std::filesystem::create_symlink(library, prefix / "lib" / library.filename());
  std::filesystem::create_directory_symlink(
      std::filesystem::path(MANYFORGE_XARM_CONTROL_SOURCE_DIR) / "vendor",
      prefix / "share" / "manyforge_xarm_control" / "vendor");
  ::setenv("AMENT_PREFIX_PATH", prefix.c_str(), 1);
  return ComponentAdapterRegistry::LoadInstalled();
}

struct Host final {
  Host() {
    if (!rclcpp::ok()) rclcpp::init(0, nullptr);
    node = std::make_shared<rclcpp::Node>("xarm_family_test");
  }
  /// A component's context on the real lane, with the machine's connection.
  [[nodiscard]] ComponentAdapterContext Context(const std::string &id, std::string role,
                                                const std::string &row) const {
    ComponentAdapterContext context{core::ControllableComponentId(id), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, false, {}, {}, {}};
    context.lane = "real";
    context.row.adapter = "xarm";
    context.lane_row = content_identity::parse_canonical_json(row);
    context.role = std::move(role);
    context.ros_namespace = "/lite6";
    context.address = "192.0.2.1";
    context.node = node;
    context.clock = clock;
    context.lane_adapter = lane;
    context.ros2_control = handle;
    context.lifecycle_command_timeout = std::chrono::seconds(10);
    context.measured_joints = [] { return std::optional<std::vector<double>>(); };
    context.restart_driver = [](std::stop_token) { return core::Result<void>::Ok(); };
    return context;
  }

  rclcpp::Node::SharedPtr node;
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<StillLane> lane = std::make_shared<StillLane>();
  /// An unbound connection: the arm's hardware switch is refused typed.
  std::shared_ptr<roboplan_backend::RetainedRos2ControlRuntimeHandle> handle =
      roboplan_backend::RetainedRos2ControlRuntimeHandle::Create();
};

constexpr std::string_view kArmRow = R"({"adapter":"xarm","dof":6})";

// CONTRACTS §6 "What a family exports": the library registered under `xarm`
// exports the family with its launch contribution and no lane hooks of its
// own (the generic Real configuration: the driver reports its own state); a
// context it can't use is refused as invalid adapter data.
TEST(XArmAdapterFamily, LoadsFromTheIndexAndRefusesWhatItCantUse) {
  auto loaded = load_family();
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  const auto *family = loaded.value().Find("xarm");
  ASSERT_NE(family, nullptr);
  EXPECT_NE(family->launch_contribution, nullptr);
  EXPECT_EQ(family->lane, nullptr);

  Host host;
  auto no_lane = host.Context("lite6_robot", "arm", std::string(kArmRow));
  no_lane.lane_adapter = nullptr;
  auto no_connection = host.Context("lite6_robot", "arm", std::string(kArmRow));
  no_connection.ros2_control = nullptr;
  // A member without its owner's handle (an arm is always a machine's
  // member, AUD-5), and a role the machine hasn't.
  auto orphan = host.Context("lite6_robot", "arm", std::string(kArmRow));
  auto ownerless = host.Context("lite6_robot", "", std::string(kArmRow));
  auto stranger = host.Context("lite6_robot_camera", "camera", std::string(kArmRow));
  auto owner_context = host.Context("lite6", "", R"({"adapter":"xarm"})");
  owner_context.machine_owner = true;
  const auto owner = loaded.value().Make(owner_context);
  ASSERT_TRUE(owner.ok()) << owner.error().message;
  stranger.owner = owner.value().owner_handle;
  no_lane.owner = owner.value().owner_handle;
  no_connection.owner = owner.value().owner_handle;
  for (const auto *context : {&no_lane, &no_connection, &orphan, &ownerless, &stranger}) {
    const auto made = loaded.value().Make(*context);
    ASSERT_FALSE(made.ok()) << context->component_id.value();
    EXPECT_EQ(made.error().code, "component_adapter_invalid");
  }
}

// CONTRACTS §16: the owner returns its handle; the arm and I/O members are
// made on it.
TEST(XArmAdapterFamily, MakesTheOwnerAndItsMembers) {
  auto loaded = load_family();
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  Host host;
  auto owner_context = host.Context("lite6", "", R"({"adapter":"xarm"})");
  owner_context.machine_owner = true;
  const auto owner = loaded.value().Make(owner_context);
  ASSERT_TRUE(owner.ok()) << owner.error().message;
  ASSERT_NE(owner.value().owner_handle, nullptr);
  EXPECT_FALSE(owner.value().tool_context.has_value());
  EXPECT_EQ(owner.value().component->capabilities(),
            (std::vector<core::Capability>{core::Capability::kPowerOff,
                                           core::Capability::kReboot}));

  auto arm_context = host.Context("lite6_robot", "arm", std::string(kArmRow));
  arm_context.owner = owner.value().owner_handle;
  const auto arm = loaded.value().Make(arm_context);
  ASSERT_TRUE(arm.ok()) << arm.error().message;
  EXPECT_TRUE(arm.value().tool_context.has_value());
  ASSERT_TRUE(arm.value().display_joints.has_value());
  EXPECT_EQ(arm.value().display_joints->topic, "/lite6/xarm/joint_states");
  EXPECT_TRUE(arm.value().component->capabilities().empty());

  auto io_context = host.Context(
      "lite6_robot_controller_io", "controller_io",
      R"({"adapter":"xarm","bank":"controller","inputs":["CI0"],"outputs":["CO0"]})");
  io_context.lane_adapter = nullptr;
  io_context.measured_joints = nullptr;
  io_context.restart_driver = nullptr;
  io_context.owner = owner.value().owner_handle;
  const auto io = loaded.value().Make(io_context);
  ASSERT_TRUE(io.ok()) << io.error().message;
  EXPECT_NE(dynamic_cast<roboplan_backend::IIoOutputWriter *>(io.value().component.get()),
            nullptr);
}

// Each xArm model's `lanes.real` row carries everything the Real lane
// configuration requires (M7's measured-state gates), as the generator reads
// the row.
TEST(XArmAdapterFamily, EveryRealModelRowBuildsTheRealLaneGates) {
  const auto models = std::filesystem::path(MANYFORGE_XARM_CONTROL_SOURCE_DIR) / ".." /
                      "manyforge_example_models" / "models";
  for (const std::string model : {"lite6", "uf850"}) {
    const auto real = YAML::LoadFile((models / model / "component.yaml").string())["lanes"]["real"];
    roboplan_backend::ModelLaneRow row;
    row.adapter = real["adapter"].as<std::string>();
    row.controller = real["controller"].as<std::string>();
    row.arrival_tolerance_rad = real["arrival_tolerance_rad"].as<double>();
    for (const auto &entry : real)
      if (entry.second.IsScalar())
        row.adapter_keys.emplace(entry.first.as<std::string>(), entry.second.as<std::string>());
    const auto gates = roboplan_backend::real_lane_row_gates(row);
    EXPECT_DOUBLE_EQ(gates.joint_state_freshness_s, 0.25) << model;
    EXPECT_DOUBLE_EQ(gates.start_tolerance_rad, 0.02) << model;
    EXPECT_DOUBLE_EQ(gates.completion_state_timeout_s, 1.0) << model;
    EXPECT_DOUBLE_EQ(gates.hard_stop_completion_timeout_s, 1.5) << model;
    row.adapter_keys.erase("start_tolerance_rad");
    EXPECT_THROW(static_cast<void>(roboplan_backend::real_lane_row_gates(row)),
                 std::invalid_argument);
  }
}

// The arm's hardware switch goes to its ros2_control connection: on an
// unbound connection Disable fails typed after the controller switch.
TEST(XArmAdapterFamily, TheArmsHardwareSwitchUsesItsConnection) {
  auto loaded = load_family();
  ASSERT_TRUE(loaded.ok()) << loaded.error().message;
  Host host;
  auto owner_context = host.Context("lite6", "", R"({"adapter":"xarm"})");
  owner_context.machine_owner = true;
  const auto owner = loaded.value().Make(owner_context);
  ASSERT_TRUE(owner.ok()) << owner.error().message;
  auto arm_context = host.Context("lite6_robot", "arm", std::string(kArmRow));
  arm_context.owner = owner.value().owner_handle;
  const auto arm = loaded.value().Make(arm_context);
  ASSERT_TRUE(arm.ok()) << arm.error().message;
  const auto disabled = arm.value().component->Run(core::ComponentAction::kDisable, {});
  EXPECT_EQ(disabled.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(disabled.error.has_value());
  // The connection's typed refusal (reason ros2_control_arm_unbound).
  EXPECT_EQ(disabled.error->code, "component_action_refused");
  EXPECT_EQ(disabled.error->message, "no arm controller is bound");
}

}  // namespace
}  // namespace manyforge::xarm_control
