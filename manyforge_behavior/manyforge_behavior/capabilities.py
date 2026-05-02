"""Capability key registry and profile definitions for ManyForge v0.0/v0.1."""

CAPABILITY_REGISTRY: frozenset[str] = frozenset({
    # Planning
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.motion.cartesian_path",
    "planning.motion.named_target",
    "planning.trajectory.time_parameterized",
    # Planning context
    "context.state.current",
    "context.kinematics.fk",
    "context.pose.resolve_reference",
    "context.scene.object_query",
    "context.limits.joint",
    "context.validation.trajectory",
    "context.collision.check",
    "context.collision.check.detailed",
    # Scene/object support
    "scene.shape.box",
    "scene.shape.sphere",
    "scene.shape.cylinder",
    "scene.shape.mesh",
    "scene.edit.add",
    "scene.edit.upsert",
    "scene.edit.remove",
    "scene.edit.update_pose",
    "scene.edit.attach_detach",
    # Execution
    "execution.trajectory.execute",
    "execution.feedback.phase_events",
    "execution.feedback.state_snapshots",
    "execution.stop.soft",
    "execution.pause",
    "execution.resume",
    "execution.reset",
    "execution.contact.geometric",
    "execution.contact.force_torque",
    "execution.contact.controller_reported",
    "execution.control.admittance",
    # IO / GPIO
    "io.gpio.set_output",
    "io.gpio.get_input",
    "io.gpio.wait_for_input",
    # Gripper / tooling
    "tool.gripper.command",
    "tool.gripper.feedback",
    # Robot state / recovery
    "robot_state.query",
    "robot_state.reset",
    # Controller management
    "controller_mgmt.switch",
    "controller_mgmt.load_unload",
    # Perception / pose-source
    "perception.pose_updates",
    # Observability
    "observability.trace.kernel",
    "observability.trace.scene",
    "observability.logging.operational",
})

# v0.0 required capability keys (from SPEC-240)
V0_0_REQUIRED_KEYS: frozenset[str] = frozenset({
    # Planning
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.trajectory.time_parameterized",
    # Context
    "context.state.current",
    "context.validation.trajectory",
    "context.collision.check",
    "context.kinematics.fk",
    "context.limits.joint",
    # Scene
    "scene.shape.box",
    "scene.shape.sphere",
    "scene.edit.add",
    "scene.edit.remove",
    "scene.edit.update_pose",
    # Execution
    "execution.trajectory.execute",
    "execution.feedback.phase_events",
    "execution.feedback.state_snapshots",
    "execution.stop.soft",
    "execution.pause",
    "execution.resume",
    "execution.reset",
})

PROFILE_REQUIRED_KEYS: dict[str, frozenset[str]] = {
    "v0.0": V0_0_REQUIRED_KEYS,
}

V0_1_REQUIRED_KEYS: frozenset[str] = frozenset(
    set(V0_0_REQUIRED_KEYS)
    | {
        "context.pose.resolve_reference",
        "context.scene.object_query",
        "scene.edit.attach_detach",
        "observability.trace.scene",
    }
)

PROFILE_REQUIRED_KEYS["v0.1"] = V0_1_REQUIRED_KEYS


def get_required_keys(profile: str) -> frozenset[str]:
    """Return the required capability keys for a program profile.

    Raises ValueError if the profile is unknown.
    """
    keys = PROFILE_REQUIRED_KEYS.get(profile)
    if keys is None:
        raise ValueError(f"Unknown program profile: '{profile}'")
    return keys


def is_known_profile(profile: str) -> bool:
    """Return True when a program/deployment profile is implemented."""
    return profile in PROFILE_REQUIRED_KEYS
