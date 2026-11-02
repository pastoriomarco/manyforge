#pragma once

#include <array>
#include <string_view>

namespace manyforge::deployment::runtime_contract_capabilities {

// Immutable semantic IDs grouped by the surface that owns their meaning.
// Runtime implementations compose these groups; catalog revisions are derived
// when the declaration is encoded and are not compatibility inputs.
inline constexpr std::array<std::string_view, 14U> kLogicalSceneServices{{
    "collision.query.detailed",
    "collision.query.pre_execution",
    "observability.trace.scene",
    "scene.edit.add",
    "scene.edit.attach_detach",
    "scene.edit.remove",
    "scene.edit.update_pose",
    "scene.edit.upsert",
    "scene.object.query",
    "scene.reference.resolve",
    "scene.shape.box",
    "scene.shape.cylinder",
    "scene.shape.mesh",
    "scene.shape.sphere",
}};

inline constexpr std::array<std::string_view, 17U> kExecutionServices{{
    "execution.contact.geometric",
    "execution.contact.geometric_signed_penetration",
    "execution.feedback.phase_events",
    "execution.feedback.state_snapshots",
    "execution.reset",
    "execution.stop.soft",
    "execution.trajectory.execute",
    "model.kinematics.fk",
    "model.limits.joint",
    "planning.motion.cartesian_path",
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.trajectory.time_parameterized",
    "state.observed.current",
    "tool.gripper.command",
    "tool.gripper.feedback",
    "validation.trajectory.non_collision",
}};

// Actual ROS host signal service/typed output surface. Keep this separate
// from generic execution declarations; another runtime needs its own provider.
inline constexpr std::array<std::string_view, 3U> kIoSignalServices{{
    "io.gpio.get_input",
    "io.gpio.set_output",
    "io.gpio.wait_for_input",
}};

inline constexpr std::array<std::string_view, 5U> kComponentLifecycleServices{{
    "component.lifecycle.disable",
    "component.lifecycle.enable",
    "component.lifecycle.observe",
    "component.lifecycle.prepare",
    "component.lifecycle.recover",
}};

}  // namespace manyforge::deployment::runtime_contract_capabilities
