"""Capability key registry and profile definitions for ManyForge v0.0/v0.1."""

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
