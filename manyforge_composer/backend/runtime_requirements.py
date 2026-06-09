"""Runtime-requirements + compatibility surface for program/deployment loads.

The composer's kernel factory can fail to build a runtime when the deployment
declares a backend (e.g. ros2_control) that this composer image cannot satisfy
— the ROS adapter isn't installed, the action server isn't reachable, Isaac
isn't streaming. Today those failures bubble up as a bare RuntimeError that
the HTTP layer renders as an opaque string, leaving the operator to guess
what's missing.

This module defines a typed structured error (`RuntimeFactoryRequirementsError`)
that carries the actionable fields — the missing components, the lane this
deployment expects, the launcher command to switch to it — and a couple of
pure helpers (deployment requirement derivation, lane detection) that the
program-saved-list endpoint and the picker UI can consume.

It also defines the small DTOs the helpers return, so the same shape is used
across the typed exception, the structured 409 detail, and the picker-side
compatibility annotations on `/api/program/saved/list`.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Any, Iterable, Optional


# ---------------------------------------------------------------------------
# DTOs


@dataclass(frozen=True)
class DeploymentRequirementsDTO:
    """What a deployment expects of the composer runtime."""

    execution_backend: str  # "native" | "ros2_control" | "observed_ros2" | "unknown"
    ros_adapter_required: bool = False
    isaac_supervisor_required: bool = False
    action_servers: tuple[str, ...] = ()
    topics: tuple[str, ...] = ()

    def to_dict(self) -> dict[str, Any]:
        return {
            "execution_backend": self.execution_backend,
            "ros_adapter_required": self.ros_adapter_required,
            "isaac_supervisor_required": self.isaac_supervisor_required,
            "action_servers": list(self.action_servers),
            "topics": list(self.topics),
        }


@dataclass(frozen=True)
class RuntimeCapabilitiesDTO:
    """What this composer image / running instance can satisfy."""

    lane: str  # "manyforge-only" | "isaac-sim" | "observed-ros2" | "unknown"
    ros_adapter_available: bool
    isaac_supervisor_available: bool
    has_kernel_factory: bool

    def to_dict(self) -> dict[str, Any]:
        return {
            "lane": self.lane,
            "ros_adapter_available": self.ros_adapter_available,
            "isaac_supervisor_available": self.isaac_supervisor_available,
            "has_kernel_factory": self.has_kernel_factory,
        }


@dataclass(frozen=True)
class CompatibilityResultDTO:
    """Result of comparing a deployment's requirements against the runtime."""

    status: str  # "compatible" | "missing_components" | "wrong_lane" | "unknown"
    missing: tuple[str, ...] = ()
    expected_lane: Optional[str] = None
    current_lane: Optional[str] = None
    suggested_command: Optional[str] = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "status": self.status,
            "missing": list(self.missing),
            "expected_lane": self.expected_lane,
            "current_lane": self.current_lane,
            "suggested_command": self.suggested_command,
        }


# ---------------------------------------------------------------------------
# Typed exception surfaced as a structured 409 by the HTTP layer


class RuntimeFactoryRequirementsError(RuntimeError):
    """Raised when the kernel factory cannot satisfy a deployment.

    Extends RuntimeError so existing `except RuntimeError` clauses still catch
    it as a fallback; HTTP handlers should catch this specifically (BEFORE the
    generic RuntimeError) and surface `detail = exc.to_detail()` so the
    operator gets an actionable rejection.
    """

    def __init__(
        self,
        *,
        reason_code: str,
        message: str,
        missing: Iterable[str] = (),
        expected_lane: Optional[str] = None,
        current_lane: Optional[str] = None,
        suggested_command: Optional[str] = None,
        original_error: Optional[str] = None,
    ) -> None:
        super().__init__(message)
        self.reason_code = reason_code
        self.message = message
        self.missing = tuple(missing)
        self.expected_lane = expected_lane
        self.current_lane = current_lane
        self.suggested_command = suggested_command
        self.original_error = original_error

    def to_detail(self) -> dict[str, Any]:
        return {
            "reason_code": self.reason_code,
            "message": self.message,
            "missing": list(self.missing),
            "expected_lane": self.expected_lane,
            "current_lane": self.current_lane,
            "suggested_command": self.suggested_command,
            "original_error": self.original_error,
        }


# ---------------------------------------------------------------------------
# Pure helpers (ROS-free, lock-free) — unit-testable in isolation


_ROS2_CONTROL_EXECUTION_BACKEND_IDS: frozenset[str] = frozenset(
    {"ros2_control", "ros2_control_v1"}
)


def detect_current_lane() -> str:
    """Detect the launcher-declared lane this composer was started in.

    Reads `MANYFORGE_LANE` (set by `scripts/launch.sh`). When the env var is
    not present (composer started outside the launcher), falls back to
    "unknown" rather than guessing — the helper is honest about what it
    can't tell. Pure.
    """
    raw = os.environ.get("MANYFORGE_LANE", "").strip().lower()
    if raw in {"manyforge-only", "isaac-sim", "observed-ros2"}:
        return raw
    return "unknown"


def derive_deployment_requirements(deployment: Any) -> DeploymentRequirementsDTO:
    """Derive a deployment's runtime requirements from its existing fields.

    Honors an explicit `requires:` block on the deployment when present
    (override path); otherwise infers from `execution_backend` /
    `execution_adapter_id` and the configured controller / action surface. No
    YAML schema migration required for existing deployments.
    """
    backend = "unknown"
    ros_required = False
    supervisor_required = False
    action_servers: list[str] = []
    topics: list[str] = []

    if deployment is None:
        return DeploymentRequirementsDTO(execution_backend=backend)

    # 1) Explicit `requires:` block wins — declared intent over inference.
    requires = _attr_or_key(deployment, "requires", None)
    if isinstance(requires, dict) and requires:
        backend = str(requires.get("execution_backend", backend))
        ros_required = bool(requires.get("ros_adapter_required", False))
        supervisor_required = bool(requires.get("isaac_supervisor_required", False))
        action_servers = list(requires.get("action_servers", []) or [])
        topics = list(requires.get("topics", []) or [])
        return DeploymentRequirementsDTO(
            execution_backend=backend,
            ros_adapter_required=ros_required,
            isaac_supervisor_required=supervisor_required,
            action_servers=tuple(action_servers),
            topics=tuple(topics),
        )

    # 2) Derive from existing fields. We're deliberately permissive about the
    #    shape because Deployment objects come from a few different code paths.
    adapter_id = (
        _attr_or_key(deployment, "execution_adapter_id", None)
        or _attr_or_key(deployment, "execution_backend", None)
        or ""
    )
    if isinstance(adapter_id, str) and adapter_id.lower() in _ROS2_CONTROL_EXECUTION_BACKEND_IDS:
        backend = "ros2_control"
        ros_required = True
    elif isinstance(adapter_id, str) and adapter_id:
        backend = adapter_id.lower()

    robot_config = _attr_or_key(deployment, "robot_config", None)
    arm_action = _attr_or_key(robot_config, "arm_action_name", None) if robot_config else None
    gripper_action = (
        _attr_or_key(robot_config, "gripper_action_name", None) if robot_config else None
    )
    for name in (arm_action, gripper_action):
        if isinstance(name, str) and name:
            action_servers.append(name)

    joint_state_topic = (
        _attr_or_key(robot_config, "isaac_joint_state_topic", None) if robot_config else None
    )
    controller_state_topic = (
        _attr_or_key(robot_config, "controller_state_topic", None) if robot_config else None
    )
    for t in (joint_state_topic, controller_state_topic):
        if isinstance(t, str) and t:
            topics.append(t)

    # Isaac supervisor is implied when the deployment is the ros2_control
    # variant AND any Isaac-flavored topic is named. Conservative inference;
    # an explicit `requires.isaac_supervisor_required` always wins.
    if ros_required and any("isaac" in (t or "").lower() for t in topics):
        supervisor_required = True

    return DeploymentRequirementsDTO(
        execution_backend=backend,
        ros_adapter_required=ros_required,
        isaac_supervisor_required=supervisor_required,
        action_servers=tuple(action_servers),
        topics=tuple(topics),
    )


def assess_runtime_capabilities(
    *,
    has_kernel_factory: bool,
    ros_adapter_available: bool,
    isaac_supervisor_available: bool,
    lane: Optional[str] = None,
) -> RuntimeCapabilitiesDTO:
    """Inspect this composer's capabilities. Caller provides the booleans
    (which it can read from the kernel factory presence + runtime/status
    components) so this helper stays pure and unit-testable. Lane defaults
    to `detect_current_lane()` when not provided."""
    return RuntimeCapabilitiesDTO(
        lane=(lane or detect_current_lane()),
        ros_adapter_available=ros_adapter_available,
        isaac_supervisor_available=isaac_supervisor_available,
        has_kernel_factory=has_kernel_factory,
    )


def check_compatibility(
    requirements: DeploymentRequirementsDTO,
    capabilities: RuntimeCapabilitiesDTO,
) -> CompatibilityResultDTO:
    """Compare requirements ⊆ capabilities. Pure.

    Returns one of:
      - status=compatible: everything required is available.
      - status=missing_components: declared backend is supported in principle,
        but one or more concrete components (ros_adapter, isaac_supervisor)
        is not available right now. UI should surface `missing` and
        `suggested_command`.
      - status=wrong_lane: backend is fundamentally not supported on this lane
        (e.g. ros2_control deployment on a manyforge-only composer image with
        no kernel factory for it). UI should surface lane mismatch + command.
    """
    missing: list[str] = []
    current_lane = capabilities.lane
    expected_lane: Optional[str] = None
    suggested_command: Optional[str] = None

    if requirements.execution_backend == "ros2_control":
        expected_lane = "isaac-sim"
        if not capabilities.has_kernel_factory or not capabilities.ros_adapter_available:
            # Fundamental lane mismatch — the runtime adapter isn't even
            # present in this image.
            return CompatibilityResultDTO(
                status="wrong_lane",
                missing=("ros_adapter",),
                expected_lane=expected_lane,
                current_lane=current_lane,
                suggested_command=_suggest_launcher_command(expected_lane),
            )
        if requirements.isaac_supervisor_required and not capabilities.isaac_supervisor_available:
            missing.append("isaac_supervisor")

    if missing:
        return CompatibilityResultDTO(
            status="missing_components",
            missing=tuple(missing),
            expected_lane=expected_lane,
            current_lane=current_lane,
            suggested_command=_suggest_launcher_command(expected_lane),
        )

    return CompatibilityResultDTO(
        status="compatible",
        expected_lane=expected_lane,
        current_lane=current_lane,
    )


# ---------------------------------------------------------------------------
# Helpers (private)


def _attr_or_key(obj: Any, name: str, default: Any) -> Any:
    """Read `name` off `obj` as either an attribute or a mapping key."""
    if obj is None:
        return default
    if isinstance(obj, dict):
        return obj.get(name, default)
    return getattr(obj, name, default)


def _suggest_launcher_command(expected_lane: Optional[str]) -> Optional[str]:
    if expected_lane is None:
        return None
    if expected_lane == "isaac-sim":
        return (
            "scripts/launch.sh --lane isaac-sim "
            "--scenario isaac-ur10e-light-scene restart"
        )
    return f"scripts/launch.sh --lane {expected_lane} restart"
