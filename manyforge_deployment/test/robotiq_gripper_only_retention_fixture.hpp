#pragma once

#include "gripper_only_retention_fixture.hpp"

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT is required"
#endif
#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_SOURCE_ROOT
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_SOURCE_ROOT is required"
#endif
#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_RESOURCE_ROOT
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_RESOURCE_ROOT is required"
#endif
#ifndef MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_CATALOG_PATH
#error "MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_CATALOG_PATH is required"
#endif

namespace manyforge::deployment::test {

[[nodiscard]] inline Lite6StructuralRetentionFixture
make_robotiq_gripper_only_structural_retention_fixture() {
  // The independently authored standalone asset selects only the qualified
  // Robotiq subtree.
  const std::filesystem::path source_root{
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_SOURCE_ROOT};
  return make_prepared_gripper_structural_retention_fixture(
      "robotiq_2f_85", MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_CATALOG_PATH,
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_FIXTURE_ROOT,
      source_root / "robotiq_2f_85.urdf", source_root / "robotiq_2f_85.srdf",
      MANYFORGE_ROBOTIQ_GRIPPER_ONLY_STRUCTURAL_RESOURCE_ROOT);
}

inline constexpr std::string_view kRobotiqGripperOnlyComponentId = "robotiq_gripper";
inline constexpr std::string_view kRobotiqGripperOnlyResourceId = "robotiq_gripper_joint";

// Map authored component input before its revision and qualification. This
// remains fixture input, never a mutation of a retained or qualified product.
inline void map_robotiq_gripper_only_component(CanonicalValue::Object &members) {
  for (auto &[name, value] : members) {
    if (name == "component_type_id") {
      if (value.as_string() != "lite6_gripper_component")
        throw std::logic_error("Robotiq fixture requires the scalar component input");
      value = CanonicalValue("robotiq_position_gripper_component");
    } else if (name == "tags") {
      value = CanonicalValue(CanonicalValue::Array{CanonicalValue("articulated-gripper")});
    }
  }
  const auto map = [](auto &self, CanonicalValue &value) -> void {
    if (value.is_string() && value.as_string() == "arm_model") {
      value = CanonicalValue("gripper_model");
    } else if (value.is_array()) {
      auto rows = value.as_array();
      for (auto &row : rows) self(self, row);
      value = CanonicalValue(std::move(rows));
    } else if (value.is_object()) {
      auto fields = value.as_object();
      for (auto &[key, item] : fields) {
        if (key == "asset_resource_role_id" && item.as_string() == "gripper_joint")
          item = CanonicalValue("fingers");
        else if (key == "asset_frame_role_id" && item.as_string() == "tcp")
          item = CanonicalValue("gripper_tcp");
        else self(self, item);
      }
      value = CanonicalValue(std::move(fields));
    }
  };
  for (auto &[_, value] : members) map(map, value);
}

inline void map_robotiq_gripper_only_graph(CanonicalValue::Object &members) {
  const std::map<std::string, std::string> identities{
      {"lite6_arm", std::string(kRobotiqGripperOnlyComponentId)},
      {"arm_model", "gripper_model"},
      {"lite6_fixture_instance", "robotiq_fixture_instance"},
      {"lite6_ros2_control_execution", "robotiq_ros2_control_execution"},
      {"lite6_gripper_binding", "robotiq_gripper_binding"},
      {"lite6_gripper_feedback", "robotiq_gripper_feedback"},
      {"lite6_gripper_domain", "robotiq_gripper_domain"},
      {"lite6_gripper_provider_instance", "robotiq_gripper_provider_instance"},
      {"lite6_gripper_commanded_state", "robotiq_gripper_commanded_state"},
      {"lite6_gripper_joint", std::string(kRobotiqGripperOnlyResourceId)},
  };
  const auto map = [&](auto &self, CanonicalValue &value) -> void {
    if (value.is_string()) {
      const auto found = identities.find(value.as_string());
      if (found != identities.end()) value = CanonicalValue(found->second);
    } else if (value.is_array()) {
      auto rows = value.as_array();
      for (auto &row : rows) self(self, row);
      value = CanonicalValue(std::move(rows));
    } else if (value.is_object()) {
      auto fields = value.as_object();
      for (auto &[_, item] : fields) self(self, item);
      value = CanonicalValue(std::move(fields));
    }
  };
  for (auto &[_, value] : members) map(map, value);
  for (auto &[name, value] : members) {
    if (name != "runtime_units") continue;
    auto units = value.as_array();
    if (units.size() != 1U)
      throw std::logic_error("Robotiq fixture requires its sole ROS runtime");
    auto unit = units.front().as_object();
    for (auto &[field, selected] : unit) {
      if (field != "configuration") continue;
      auto configuration = selected.as_object();
      for (auto &[key, setting] : configuration) {
        if (key == "controller_profiles") {
          // The tool's own model and its `lanes.mock` row, as the compiler
          // emits them for the Robotiq 2F-85 (models/robotiq_2f_85).
          auto profiles = setting.as_array();
          for (auto &row : profiles) {
            auto fields = row.as_object();
            for (auto &[field, entry] : fields) {
              if (field == "model_family_id") entry = CanonicalValue("robotiq_2f_85");
              if (field == "gripper_joint_name")
                entry = CanonicalValue("robotiq_85_left_knuckle_joint");
              if (field == "lane_row")
                entry = CanonicalValue(
                    R"({"adapter":"mock","controller":"robotiq_gripper_controller",)"
                    R"("controllers_file":"mock_controllers.yaml","gripper_result_timeout_s":15,)"
                    R"("gripper_stop_finish_tolerance":0.04,)"
                    R"("hard_stop_completion_timeout_s":0.5})");
            }
            row = CanonicalValue(std::move(fields));
          }
          setting = CanonicalValue(std::move(profiles));
          continue;
        }
        if (key != "gripper_target_configurations") continue;
        // gripper_tcp is the real base frame used as an authored fixture
        // reference. It is not a calibrated jaw TCP or tool mounting claim.
        setting = CanonicalValue(CanonicalValue::Array{
            CanonicalValue(CanonicalValue::Object{
                {"actuation_kind", CanonicalValue("position_commanded")},
                {"attach_frame", CanonicalValue("robotiq_85_base_link")},
                {"auxiliary_frames", CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("robotiq_85_base_link")})},
                {"closed_state_ref", CanonicalValue("closed")},
                {"command_duration_s", CanonicalValue(1.0)},
                {"command_target_id", CanonicalValue("gripper")},
                {"controllable_component_id", CanonicalValue(std::string(kRobotiqGripperOnlyComponentId))},
                {"description", CanonicalValue("Standalone Robotiq 2F-85 Position fixture.")},
                {"mount_frame", CanonicalValue("robotiq_85_base_link")},
                {"open_state_ref", CanonicalValue("open")},
                {"output_mode", CanonicalValue("commanded")},
                {"tcp_frame", CanonicalValue("robotiq_85_base_link")},
                {"touch_links", CanonicalValue(CanonicalValue::Array{
                    CanonicalValue("robotiq_85_base_link"),
                    CanonicalValue("robotiq_85_left_finger_link"),
                    CanonicalValue("robotiq_85_left_finger_tip_link"),
                    CanonicalValue("robotiq_85_left_inner_knuckle_link"),
                    CanonicalValue("robotiq_85_left_knuckle_link"),
                    CanonicalValue("robotiq_85_right_finger_link"),
                    CanonicalValue("robotiq_85_right_finger_tip_link"),
                    CanonicalValue("robotiq_85_right_inner_knuckle_link"),
                    CanonicalValue("robotiq_85_right_knuckle_link")})},
            })});
      }
      selected = CanonicalValue(std::move(configuration));
    }
    units.front() = CanonicalValue(std::move(unit));
    value = CanonicalValue(std::move(units));
  }
}

[[nodiscard]] inline Lite6ControllableRetentionFixture
make_robotiq_gripper_only_retention_fixture(std::string_view lifecycle_semantic_digest) {
  const auto structural = make_robotiq_gripper_only_structural_retention_fixture();
  return make_lite6_controllable_retention_fixture(
      false, false, 1U, "i6_robotiq_gripper_only_cell", false, true,
      "resource", true, lifecycle_semantic_digest, lifecycle_semantic_digest, true, true, std::nullopt, kLite6ArmTargetId,
      std::nullopt, std::nullopt, {}, true, {},
      "manyforge.provider.ros2_control", true, true, &structural,
      map_robotiq_gripper_only_graph, false, "position_commanded", 1.0,
      "commanded", map_robotiq_gripper_only_component);
}

}  // namespace manyforge::deployment::test
