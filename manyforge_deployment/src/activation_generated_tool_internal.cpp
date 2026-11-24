#include "activation_generated_tool_internal.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/typed_error.hpp>

namespace manyforge::deployment::detail {
namespace {
using Value = CanonicalValue;
using Object = Value::Object;
using Array = Value::Array;

constexpr std::string_view kModelRole = "tool_model";
constexpr std::string_view kMountRole = "mount";
constexpr std::string_view kDefaultCollisionRole = "default_collision";
constexpr std::array<std::string_view, 4U> kGripperCapabilities{{
    "model.kinematics.fk", "model.limits.joint", "tool.gripper.command",
    "tool.gripper.feedback",
}};

[[noreturn]] void fail(const std::string &message) {
  throw std::invalid_argument("generated_component " + message);
}

const Value &member(const Value &value, std::string_view name) {
  const auto *result = value.is_object() ? value.find_object_member(name) : nullptr;
  if (result == nullptr) fail("is missing " + std::string(name));
  return *result;
}

const std::string &text(const Value &value, std::string_view name) {
  const auto &selected = member(value, name);
  if (!selected.is_string() || selected.as_string().empty())
    fail(std::string(name) + " must be a non-empty string");
  return selected.as_string();
}

const Array &rows(const Value &value, std::string_view name) {
  const auto &selected = member(value, name);
  if (!selected.is_array()) fail(std::string(name) + " must be a list");
  return selected.as_array();
}

double positive(const Value &value, std::string_view name) {
  const auto &selected = member(value, name);
  if (!selected.is_number() || !(selected.as_number() > 0.0))
    fail(std::string(name) + " must be a positive number");
  return selected.as_number();
}

std::vector<std::string> names(const Value &value, std::string_view name) {
  std::vector<std::string> result;
  for (const auto &item : rows(value, name)) {
    if (!item.is_string() || item.as_string().empty())
      fail(std::string(name) + " must list non-empty strings");
    result.push_back(item.as_string());
  }
  return result;
}

Value strings(const std::vector<std::string> &values) {
  Array result;
  for (const auto &value : values) result.emplace_back(value);
  return Value(std::move(result));
}

// A spring slider's travel on its collision body role (OD-371).
Value spring_slider_row(const GeneratedSpringSlider &slider) {
  Object row{
      {"axis", Value(Array{Value(slider.axis[0]), Value(slider.axis[1]),
                           Value(slider.axis[2])})},
      {"stroke_m", Value(slider.stroke_m)},
      {"trip_depth_m", Value(slider.trip_depth_m)},
      {"max_compression_step_m", Value(slider.max_compression_step_m)},
  };
  if (slider.release_depth_m) row.emplace_back("release_depth_m", Value(*slider.release_depth_m));
  return Value(std::move(row));
}

// A target's collision bodies, as every maintained component type lists
// them: its roles and `default_collision`, sorted; a spring slider's body
// with its travel.
Value collision_body_roles(std::vector<std::string> roles,
                           const std::vector<GeneratedSpringSlider> &sliders = {}) {
  roles.emplace_back(kDefaultCollisionRole);
  std::sort(roles.begin(), roles.end());
  roles.erase(std::unique(roles.begin(), roles.end()), roles.end());
  Array result;
  for (const auto &role : roles) {
    Object row{
        {"collision_body_role_id", Value(role)},
        {"model_role_id", Value(std::string(kModelRole))},
        {"asset_collision_body_role_id", Value(role)},
    };
    for (const auto &slider : sliders)
      if (slider.element_id == role) row.emplace_back("spring_slider", spring_slider_row(slider));
    result.emplace_back(std::move(row));
  }
  return Value(std::move(result));
}
} // namespace

GeneratedToolComponent
parse_generated_tool_component(const CanonicalValue &declared) {
  GeneratedToolComponent tool;
  tool.mount_link_name = text(declared, "mount_frame");
  const auto &bodies = member(declared, "collision_bodies");
  if (!bodies.is_object()) fail("collision_bodies must be a mapping");
  for (const auto &[role, unused] : bodies.as_object()) {
    static_cast<void>(unused);
    tool.collision_bodies.emplace(role, names(bodies, role));
  }
  for (const auto &row : rows(declared, "tcps"))
    tool.tcps.push_back({text(row, "id"), text(row, "frame"),
                         names(row, "contact_body_role_ids")});
  if (tool.tcps.empty()) fail("tcps must name the default TCP");
  for (const auto &row : rows(declared, "gripper_targets")) {
    const auto &states = member(row, "states");
    const auto &duration = member(row, "command_duration_s");
    if (!duration.is_number() || !(duration.as_number() > 0.0))
      fail("command_duration_s must be a positive number");
    tool.gripper_targets.push_back(
        {text(row, "command_target_id"), text(row, "group"), text(row, "joint"),
         names(row, "collision_body_role_ids"), text(row, "grip_body_role_id"),
         text(states, "open"), text(states, "closed"), duration.as_number(),
         text(row, "attach_frame"), text(row, "tcp_frame")});
  }
  for (const auto &row : rows(declared, "spring_sliders")) {
    GeneratedSpringSlider slider;
    slider.element_id = text(row, "collision_body_role_id");
    const auto body = tool.collision_bodies.find(slider.element_id);
    if (body == tool.collision_bodies.end() || body->second.size() != 1U)
      fail("spring slider " + slider.element_id + " needs its collision body on one link");
    const auto &axis = rows(row, "axis");
    if (axis.size() != slider.axis.size())
      fail("spring slider axis must list 3 numbers");
    for (std::size_t index = 0; index != axis.size(); ++index) {
      if (!axis[index].is_number()) fail("spring slider axis must list 3 numbers");
      slider.axis[index] = axis[index].as_number();
    }
    slider.stroke_m = positive(row, "stroke_m");
    slider.trip_depth_m = positive(row, "trip_depth_m");
    if (row.find_object_member("release_depth_m") != nullptr)
      slider.release_depth_m = positive(row, "release_depth_m");
    slider.max_compression_step_m = positive(row, "max_compression_step_m");
    tool.spring_sliders.push_back(std::move(slider));
  }
  // Declared data without a consumer in this decoder; lists as generated.
  static_cast<void>(rows(declared, "tool_ends"));
  return tool;
}

std::optional<GeneratedSpringSlider>
generated_spring_slider(const GeneratedToolComponent &tool, std::string_view element_id) {
  for (const auto &slider : tool.spring_sliders)
    if (slider.element_id == element_id) return slider;
  return std::nullopt;
}

CanonicalValue generated_tool_gripper_rows(
    const GeneratedToolComponent &tool, std::string_view component_id,
    const CanonicalValue *wiring_rows) {
  const auto refuse = [&](std::string_view code, const std::string &detail) -> void {
    throw core::TypedError(
        std::string(code),
        "Activation component profiles: " + std::string(code) + ": " +
            std::string(component_id) + " " + detail);
  };
  if (wiring_rows != nullptr && !wiring_rows->is_array())
    refuse("generated_tool_wiring_invalid", "gripper_target_configurations must be a list");
  const Array empty;
  const auto &wiring = wiring_rows == nullptr ? empty : wiring_rows->as_array();
  std::map<std::string, const Value *, std::less<>> by_end;
  for (const auto &row : wiring) {
    const auto *id = row.is_object() ? row.find_object_member("command_target_id") : nullptr;
    const auto *owner = row.is_object() ? row.find_object_member("controllable_component_id")
                                        : nullptr;
    if (id == nullptr || !id->is_string() || owner == nullptr || !owner->is_string() ||
        owner->as_string() != component_id)
      refuse("generated_tool_wiring_invalid",
             "a wiring row names its tool (controllable_component_id) and one "
             "jaw pair (command_target_id)");
    const auto known = std::any_of(
        tool.gripper_targets.begin(), tool.gripper_targets.end(),
        [&](const auto &target) { return target.command_target_id == id->as_string(); });
    if (!known || !by_end.emplace(id->as_string(), &row).second)
      refuse("generated_tool_wiring_unknown",
             "wiring row '" + id->as_string() +
                 (known ? "' repeats its jaw pair"
                        : "' names no actuated tool end (jaw pair) of its parameters"));
    for (const auto &[name, unused] : row.as_object()) {
      static_cast<void>(unused);
      if (name != "controllable_component_id" && name != "command_target_id" &&
          name != "pneumatic_io" && name != "grasp_confirmation")
        refuse("generated_tool_wiring_invalid",
               "wiring row '" + id->as_string() + "' authors " + name +
                   "; a generated tool's row holds its wiring only (pneumatic_io), "
                   "the rest comes from its parameters");
    }
    if (row.find_object_member("pneumatic_io") == nullptr)
      refuse("generated_tool_wiring_invalid",
             "wiring row '" + id->as_string() + "' needs pneumatic_io (its valve outputs)");
  }
  Array result;
  for (const auto &target : tool.gripper_targets) {
    const auto found = by_end.find(target.command_target_id);
    if (found == by_end.end())
      refuse("generated_tool_wiring_missing",
             "jaw pair '" + target.command_target_id +
                 "' has no wiring row (pneumatic_io) in gripper_target_configurations");
    const auto &wired = *found->second;
    Object row{
        {"actuation_kind", Value("pneumatic")},
        {"attach_frame", Value(target.attach_frame)},
        {"auxiliary_frames", Value(Array{})},
        {"closed_state_ref", Value(target.closed_state_ref)},
        {"command_duration_s", Value(target.command_duration_s)},
        {"command_target_id", Value(target.command_target_id)},
        {"controllable_component_id", Value(std::string(component_id))},
        {"description", Value("Jaw pair " + target.command_target_id + ".")},
        {"mount_frame", Value(tool.mount_link_name)},
        {"open_state_ref", Value(target.open_state_ref)},
        {"output_mode", Value("commanded")},
        {"pneumatic_io", *wired.find_object_member("pneumatic_io")},
        {"tcp_frame", Value(target.tcp_frame)},
    };
    if (const auto *confirmation = wired.find_object_member("grasp_confirmation"))
      row.emplace_back("grasp_confirmation", *confirmation);
    std::sort(row.begin(), row.end(),
              [](const auto &left, const auto &right) { return left.first < right.first; });
    result.emplace_back(std::move(row));
  }
  return Value(std::move(result));
}

std::string
generated_tool_resource_role_id(std::string_view command_target_id) {
  return std::string(command_target_id) + "_jaws";
}

CanonicalValue generated_tool_semantics(const GeneratedToolComponent &tool) {
  Array bodies;
  for (const auto &[role, links] : tool.collision_bodies)
    bodies.emplace_back(Object{
        {"collision_body_role_id", Value(role)},
        {"source_link_names", strings(links)},
    });
  Array frames;
  frames.emplace_back(Object{
      {"frame_role_id", Value(std::string(kMountRole))},
      {"source_link_name", Value(tool.mount_link_name)},
  });
  for (const auto &tcp : tool.tcps)
    frames.emplace_back(Object{
        {"frame_role_id", Value(tcp.tcp_id)},
        {"source_link_name", Value(tcp.link_name)},
    });
  for (const auto &slider : tool.spring_sliders)
    frames.emplace_back(Object{
        {"frame_role_id", Value(slider.element_id)},
        {"source_link_name",
         Value(tool.collision_bodies.find(slider.element_id)->second.front())},
    });
  std::sort(frames.begin(), frames.end(), [](const auto &left, const auto &right) {
    return left.find_object_member("frame_role_id")->as_string() <
           right.find_object_member("frame_role_id")->as_string();
  });
  auto targets = tool.gripper_targets;
  std::sort(targets.begin(), targets.end(), [](const auto &left, const auto &right) {
    return left.command_target_id < right.command_target_id;
  });
  Array groups;
  Array resources;
  for (const auto &target : targets) {
    groups.emplace_back(Object{
        {"model_group_role_id", Value(target.command_target_id)},
        {"source_group_name", Value(target.group_name)},
    });
    resources.emplace_back(Object{
        {"resource_role_id",
         Value(generated_tool_resource_role_id(target.command_target_id))},
        {"source_joint_names", Value(Array{Value(target.joint_name)})},
    });
  }
  return Value(Object{
      {"collision_body_role_bindings", Value(std::move(bodies))},
      {"frame_role_bindings", Value(std::move(frames))},
      {"instance_cardinality", Value("multiple_per_cell")},
      {"model_group_role_bindings", Value(std::move(groups))},
      {"resource_role_bindings", Value(std::move(resources))},
      {"root_frame_role_id", Value(std::string(kMountRole))},
  });
}

CanonicalValue generated_tool_type_shape(const GeneratedToolComponent &tool) {
  const std::string model_role(kModelRole);
  Array frames;
  for (const auto &tcp : tool.tcps) {
    Object frame{
        {"frame_role_id", Value(tcp.tcp_id)},
        {"model_role_id", Value(model_role)},
        {"asset_frame_role_id", Value(tcp.tcp_id)},
        {"tcp", Value(true)},
    };
    if (!tcp.contact_body_role_ids.empty())
      frame.emplace_back("contact_body_role_ids", strings(tcp.contact_body_role_ids));
    frames.emplace_back(std::move(frame));
  }
  // OD-371: each spring slider is a frame, so objects attach to the slider.
  for (const auto &slider : tool.spring_sliders)
    frames.emplace_back(Object{
        {"frame_role_id", Value(slider.element_id)},
        {"model_role_id", Value(model_role)},
        {"asset_frame_role_id", Value(slider.element_id)},
    });
  std::vector<std::string> every_body;
  for (const auto &[role, unused] : tool.collision_bodies) {
    static_cast<void>(unused);
    every_body.push_back(role);
  }
  Array targets;
  targets.emplace_back(Object{
      {"command_target_id", Value(std::string(kGeneratedToolBodyTarget))},
      {"model_group_roles", Value(Array{})},
      {"resource_roles", Value(Array{})},
      {"frame_roles", Value(std::move(frames))},
      {"collision_body_roles", collision_body_roles(std::move(every_body), tool.spring_sliders)},
      {"capabilities", Value(Array{})},
      {"default_tcp_frame_role_id", Value(tool.tcps.front().tcp_id)},
  });
  Array capabilities;
  for (const auto name : kGripperCapabilities) {
    const auto *builtin = core::find_builtin_capability_definition(name);
    if (builtin == nullptr)
      throw std::logic_error("gripper capability is not qualified by this build");
    capabilities.emplace_back(Object{
        {"capability_id", Value(std::string(name))},
        {"capability_definition_revision",
         Value(std::string(builtin->capability_definition_revision))},
    });
  }
  for (const auto &target : tool.gripper_targets)
    targets.emplace_back(Object{
        {"command_target_id", Value(target.command_target_id)},
        {"model_group_roles",
         Value(Array{Value(Object{
             {"model_group_role_id", Value("planning_group")},
             {"model_role_id", Value(model_role)},
             {"asset_model_group_role_id", Value(target.command_target_id)},
         })})},
        {"resource_roles",
         Value(Array{Value(Object{
             {"resource_role_id", Value("fingers")},
             {"source_kind", Value("model_role")},
             {"model_role_id", Value(model_role)},
             {"asset_resource_role_id",
              Value(generated_tool_resource_role_id(target.command_target_id))},
         })})},
        {"frame_roles", Value(Array{})},
        {"collision_body_roles", collision_body_roles(target.collision_body_role_ids)},
        {"grip_body_role_id", Value(target.grip_body_role_id)},
        {"capabilities", Value(capabilities)},
    });
  Array tags;
  if (!tool.gripper_targets.empty()) tags.emplace_back("gripper");
  tags.emplace_back("tool");
  return Value(Object{
      {"tags", Value(std::move(tags))},
      {"model_roles",
       Value(Array{Value(Object{
           {"model_role_id", Value(model_role)},
           {"cardinality", Value("exactly_one")},
       })})},
      {"component_root",
       Value(Object{
           {"model_role_id", Value(model_role)},
           {"frame_role_id", Value(std::string(kMountRole))},
       })},
      {"model_mounts", Value(Array{})},
      {"attachment_interfaces",
       Value(Array{Value(Object{
           {"attachment_interface_id", Value("mount")},
           {"model_role_id", Value(model_role)},
           {"frame_role_id", Value(std::string(kMountRole))},
           {"accepts_tags", Value(Array{Value("articulated-arm")})},
           {"max_attachments", Value(1.0)},
       })})},
      {"command_targets", Value(std::move(targets))},
  });
}

} // namespace manyforge::deployment::detail
