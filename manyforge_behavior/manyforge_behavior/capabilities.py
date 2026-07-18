"""Capability key registry and profile definitions for ManyForge v0.0/v0.1/v0.2."""

from __future__ import annotations

from dataclasses import dataclass

CAPABILITY_REGISTRY: frozenset[str] = frozenset({
    # Planning
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.motion.cartesian_path",
    "planning.motion.named_target",
    "planning.profile.first_feasible",
    "planning.profile.balanced",
    "planning.profile.quality_bounded",
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
    # Motion-scoped interrupt outcomes.
    # `kernel.motion_interrupt_scope` is required for any node that
    # submits a `MotionInterruptScopeDTO` (MoveUntilSignal,
    # ProbeSearchMove). `kernel.motion_interrupt_precision_contact`
    # narrows that to deployments whose backend capability profile
    # supports the precision-contact node category fallback rules
    # (spec/210 §3.11).
    "kernel.motion_interrupt_scope",
    "kernel.motion_interrupt_precision_contact",
    # IO / GPIO
    "io.gpio.set_output",
    "io.gpio.get_input",
    "io.gpio.wait_for_input",
    # Deployment-owned NRT PLC resource providers. These authorize external
    # orchestration leaves; they never place PLC work in a controller or
    # RuntimeSafety lane.
    "plc.siemens.s7.read",
    "plc.siemens.s7.write",
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
    # Simulator lifecycle/control
    "simulator.control.reset",
    "simulator.control.pause",
    "simulator.control.play",
    "simulator.control.step",
    "simulator.entity_state.set",
    # Observability
    "observability.trace.kernel",
    "observability.trace.scene",
    "observability.logging.operational",
})


@dataclass(frozen=True)
class AdapterCapabilityManifest:
    """Code-owned capability manifest published by a concrete adapter."""

    adapter_id: str
    capability_keys: frozenset[str]
    gripper_capability_keys: frozenset[str] = frozenset()

    def capability_keys_for(self, *, grippers_configured: bool = False) -> frozenset[str]:
        keys = set(self.capability_keys)
        if grippers_configured:
            keys.update(self.gripper_capability_keys)
        return frozenset(keys)


_PLANNING_ROBOPLAN_CAPABILITY_KEYS = frozenset({
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.motion.cartesian_path",
    "planning.profile.first_feasible",
    "planning.profile.balanced",
    "planning.profile.quality_bounded",
    "planning.trajectory.time_parameterized",
})

_ROBOPLAN_CONTEXT_CAPABILITY_KEYS = frozenset({
    "context.state.current",
    "context.validation.trajectory",
    "context.collision.check",
    "context.collision.check.detailed",
    "context.kinematics.fk",
    "context.limits.joint",
    "context.pose.resolve_reference",
    "context.scene.object_query",
    "scene.shape.box",
    "scene.shape.sphere",
    "scene.shape.cylinder",
    "scene.shape.mesh",
    "scene.edit.add",
    "scene.edit.upsert",
    "scene.edit.remove",
    "scene.edit.update_pose",
    "scene.edit.attach_detach",
    "observability.trace.scene",
})

_EXECUTION_CORE_CAPABILITY_KEYS = frozenset({
    "execution.trajectory.execute",
    "execution.feedback.phase_events",
    "execution.feedback.state_snapshots",
    "execution.stop.soft",
    "execution.pause",
    "execution.resume",
    "execution.reset",
})

_GRIPPER_CAPABILITY_KEYS = frozenset({
    "tool.gripper.command",
    "tool.gripper.feedback",
})

ADAPTER_CAPABILITY_MANIFESTS: dict[str, AdapterCapabilityManifest] = {
    "planning.roboplan": AdapterCapabilityManifest(
        adapter_id="planning.roboplan",
        capability_keys=_PLANNING_ROBOPLAN_CAPABILITY_KEYS,
    ),
    "planning.roboplan_context": AdapterCapabilityManifest(
        adapter_id="planning.roboplan_context",
        capability_keys=_ROBOPLAN_CONTEXT_CAPABILITY_KEYS,
    ),
    "execution.roboplan_native": AdapterCapabilityManifest(
        adapter_id="execution.roboplan_native",
        capability_keys=_EXECUTION_CORE_CAPABILITY_KEYS,
        gripper_capability_keys=_GRIPPER_CAPABILITY_KEYS,
    ),
    "execution.ros2_control_follow_joint_trajectory": AdapterCapabilityManifest(
        adapter_id="execution.ros2_control_follow_joint_trajectory",
        capability_keys=_EXECUTION_CORE_CAPABILITY_KEYS,
        gripper_capability_keys=_GRIPPER_CAPABILITY_KEYS,
    ),
    # Legacy short id retained for historic fixtures/deployments.
    "execution.ros2_control_fjt": AdapterCapabilityManifest(
        adapter_id="execution.ros2_control_fjt",
        capability_keys=_EXECUTION_CORE_CAPABILITY_KEYS,
        gripper_capability_keys=_GRIPPER_CAPABILITY_KEYS,
    ),
}


def get_adapter_capability_manifest(
    adapter_id: str,
) -> AdapterCapabilityManifest | None:
    return ADAPTER_CAPABILITY_MANIFESTS.get(adapter_id)

# v0.0 required capability keys (from SPEC-240)
V0_0_REQUIRED_KEYS: frozenset[str] = frozenset({
    # Planning
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.profile.first_feasible",
    "planning.profile.balanced",
    "planning.profile.quality_bounded",
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

# v0.2 adds the motion-scoped interrupt surface. Programs
# that author MoveUntilSignal / ProbeSearchMove (catalog `min_profile:
# v0.2`) must declare `program_profile: v0.2` so deployments without
# the kernel.motion_interrupt_scope capability can be rejected at load
# time rather than at trip time.
V0_2_REQUIRED_KEYS: frozenset[str] = frozenset(
    set(V0_1_REQUIRED_KEYS)
    | {
        "kernel.motion_interrupt_scope",
    }
)

PROFILE_REQUIRED_KEYS["v0.2"] = V0_2_REQUIRED_KEYS


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
