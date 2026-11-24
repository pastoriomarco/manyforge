#pragma once

// Generated tools (`manyforge.tool`, OD-352/OD-357): what a tool made from
// its parameters declares beside its model, and what Load derives from it.
//
// Spring sliders (OD-371): the parameters' `spring_sliders` rows are kept in
// `GeneratedToolComponent::spring_sliders`, and the derived component type
// carries the same data on each slider's collision body role of the `tool`
// target (`ComponentTypeCollisionBodyRoleDTO::spring_slider`), so it is
// retained with the cell. One slider is read by its element id (= its
// collision body role id and its frame role id on the `tool` target):
//
//   // Retained side (Program binding): the component's qualified type, e.g.
//   // qualified_program_component_type(component_id, bundle, catalog).
//   std::optional<GeneratedSpringSlider>
//   generated_spring_slider(const core::ComponentTypeDTO &type,
//                           std::string_view element_id);
//
//   // Activation side (Mock inputs): the component's profile
//   // (`ActivationComponentProfile::generated_tool`, absent for a component
//   // that is not a generated tool).
//   std::optional<GeneratedSpringSlider>
//   generated_spring_slider(const GeneratedToolComponent &tool,
//                           std::string_view element_id);
//
// Both return nullopt when no spring slider has that id.

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/component_type.hpp>
#include <manyforge_deployment/canonical_value.hpp>

namespace manyforge::deployment::detail {

/// A TCP of a generated tool: its id (the first is the default, `tcp`), its
/// link and the collision bodies that touch the work at it.
struct GeneratedToolTcp final {
  std::string tcp_id;
  std::string link_name;
  std::vector<std::string> contact_body_role_ids;
};

/// One jaw pair of a generated tool, an actuated tool end: a gripper command
/// target under the pair's id, with its own SRDF group, commanded joint and
/// collision bodies (its fingers and, when it has them, its pads), and what
/// its gripper row holds besides its wiring (OD-357: states, frames and
/// motion time come from the parameters only).
struct GeneratedToolGripperTarget final {
  std::string command_target_id;
  std::string group_name;
  std::string joint_name;
  std::vector<std::string> collision_body_role_ids;
  /// The pair's grip body (OD-46): its pads, else its fingers.
  std::string grip_body_role_id;
  std::string open_state_ref;
  std::string closed_state_ref;
  double command_duration_s{};
  /// The pair frame and the tool end's TCP frame (its first TCP, else the
  /// tool's default TCP).
  std::string attach_frame;
  std::string tcp_frame;
};

/// One spring slider of a generated tool (OD-371): a tool end whose rod and
/// the objects it carries are pushed back together as one spring.
struct GeneratedSpringSlider final {
  /// The element id: its collision body role id and its frame role id on the
  /// `tool` target (at the slider link, its collision body's one link).
  std::string element_id;
  /// Unit, slider link frame: the rod's line, pointing out of the tool.
  std::array<double, 3> axis{};
  double stroke_m{};
  double trip_depth_m{};
  /// 0 < release < trip; absent when the parameters give none.
  std::optional<double> release_depth_m{};
  double max_compression_step_m{};
};

/// What a tool generated from its parameters declares beside its model
/// (OD-352, OD-357): the captured closure's `generated_component`. The
/// component type, the model semantics and each jaw pair's gripper row are
/// derived from it at Load; its tool ends and each target's positions are
/// declared data this decoder leaves in place.
struct GeneratedToolComponent final {
  std::string mount_link_name;
  /// Collision body role id -> its links.
  std::map<std::string, std::vector<std::string>, std::less<>> collision_bodies;
  std::vector<GeneratedToolTcp> tcps;
  std::vector<GeneratedToolGripperTarget> gripper_targets;
  std::vector<GeneratedSpringSlider> spring_sliders;
};

/// The frame-only command target of a generated tool: its body, its fixed
/// elements and its TCPs. Each jaw pair is a gripper target beside it.
inline constexpr std::string_view kGeneratedToolBodyTarget = "tool";

/// The family of tools generated from parameters (`manyforge.tool`): the
/// only family that takes parameters, and it takes nothing else.
inline constexpr std::string_view kGeneratedToolFamily = "tool";

[[nodiscard]] GeneratedToolComponent
parse_generated_tool_component(const CanonicalValue &declared);

/// The spring slider `element_id` of `tool`; nullopt when it has none.
[[nodiscard]] std::optional<GeneratedSpringSlider>
generated_spring_slider(const GeneratedToolComponent &tool, std::string_view element_id);

/// The spring slider `element_id` of a (generated tool's) component type,
/// from its collision body role; nullopt when no role of that id has one.
/// Inline: the Program binding and reference validation (verification core)
/// read it without linking the activation library.
[[nodiscard]] inline std::optional<GeneratedSpringSlider>
generated_spring_slider(const core::ComponentTypeDTO &type, std::string_view element_id) {
  for (const auto &target : type.command_targets)
    for (const auto &body : target.collision_body_roles)
      if (body.spring_slider && body.collision_body_role_id.value() == element_id)
        return GeneratedSpringSlider{std::string(element_id), body.spring_slider->axis,
                                     body.spring_slider->stroke_m,
                                     body.spring_slider->trip_depth_m,
                                     body.spring_slider->release_depth_m,
                                     body.spring_slider->max_compression_step_m};
  return std::nullopt;
}

/// The Deployment's `gripper_target_configurations` of a generated tool
/// hold its wiring only: per actuated tool end (jaw pair) exactly one row
/// `{controllable_component_id, command_target_id, pneumatic_io}` (optional
/// `grasp_confirmation`). Returns the complete gripper rows, the rest from
/// the parameters; refuses with `generated_tool_wiring_missing` (a jaw pair
/// without its row), `generated_tool_wiring_unknown` (a row naming no jaw
/// pair, or a repeated one) and `generated_tool_wiring_invalid` (a row
/// without `pneumatic_io` or with any other field).
[[nodiscard]] CanonicalValue generated_tool_gripper_rows(
    const GeneratedToolComponent &tool, std::string_view component_id,
    const CanonicalValue *wiring_rows);

/// The resource role (model semantics) of a jaw pair's commanded joint.
[[nodiscard]] std::string
generated_tool_resource_role_id(std::string_view command_target_id);

/// The model semantics of a generated tool, in the shape of a model
/// family's `semantics`: every declared collision body, the `mount` frame,
/// one frame per TCP and one per spring slider (its link), and per jaw pair
/// its group (role: the pair id) and its commanded joint
/// (`generated_tool_resource_role_id`).
[[nodiscard]] CanonicalValue
generated_tool_semantics(const GeneratedToolComponent &tool);

/// The component type of a generated tool without its identity fields: the
/// `tool` target (every collision body, a spring slider's with its travel,
/// every TCP with its contact bodies, one frame per spring slider) and one
/// gripper target per jaw pair, mounted like every tool.
[[nodiscard]] CanonicalValue
generated_tool_type_shape(const GeneratedToolComponent &tool);

} // namespace manyforge::deployment::detail
