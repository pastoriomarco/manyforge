"""ROS ``simulation_interfaces`` lifecycle-control helpers."""

from __future__ import annotations

import argparse
import json
import time
from dataclasses import asdict, dataclass
from typing import Any


@dataclass(frozen=True)
class SimulationResetResult:
    """Structured outcome for a simulator reset request."""

    success: bool
    service_name: str | None
    scope: str
    duration_s: float
    result_code: int | None = None
    result_message: str = ""
    error: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class SimulationControlResult:
    """Structured outcome for a simulator-control request."""

    success: bool
    operation: str
    service_name: str | None
    duration_s: float
    result_code: int | None = None
    result_message: str = ""
    error: str = ""
    state: str | None = None
    steps: int | None = None
    entity: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class SimulationControlClient:
    """Client for ROS 2 ``simulation_interfaces`` control services."""

    _RESET_SERVICE_TYPE = "simulation_interfaces/srv/ResetSimulation"
    _SET_SIMULATION_STATE_SERVICE_TYPE = "simulation_interfaces/srv/SetSimulationState"
    _STEP_SIMULATION_SERVICE_TYPE = "simulation_interfaces/srv/StepSimulation"
    _SET_ENTITY_STATE_SERVICE_TYPE = "simulation_interfaces/srv/SetEntityState"
    _RESET_SCOPE_VALUES = {
        "state": "SCOPE_STATE",
    }
    _SIMULATION_STATE_VALUES = {
        "playing": "STATE_PLAYING",
        "paused": "STATE_PAUSED",
    }

    def __init__(
        self,
        node: object,
        *,
        service_prefix: str = "auto",
        service_timeout_s: float = 3.0,
    ) -> None:
        self.node = node
        self.service_prefix = (service_prefix or "auto").strip() or "auto"
        self.service_timeout_s = float(service_timeout_s)

        self._rclpy = _import_module("rclpy")
        self._srv = _import_module("simulation_interfaces.srv")
        self._msg = _import_module("simulation_interfaces.msg")
        self._std_msgs = _import_module("std_msgs.msg")
        self._qos = _import_module("rclpy.qos")

    def reset(self, *, scope: str = "state") -> SimulationResetResult:
        """Call ``ResetSimulation`` with a bounded timeout."""
        started = time.monotonic()
        service_name = self._resolve_reset_service_name()
        if service_name is None:
            return SimulationResetResult(
                success=False,
                service_name=None,
                scope=scope,
                duration_s=time.monotonic() - started,
                error="ResetSimulation service not available",
            )

        request = self._srv.ResetSimulation.Request()
        request.scope = self._reset_scope_value(scope)
        try:
            response = self._call_service(service_name, self._srv.ResetSimulation, request)
        except Exception as exc:
            return SimulationResetResult(
                success=False,
                service_name=service_name,
                scope=scope,
                duration_s=time.monotonic() - started,
                error=str(exc),
            )

        result = getattr(response, "result", None)
        if result is None:
            return SimulationResetResult(
                success=False,
                service_name=service_name,
                scope=scope,
                duration_s=time.monotonic() - started,
                error="ResetSimulation response did not include a result field",
            )
        return SimulationResetResult(
            success=self._result_ok(result),
            service_name=service_name,
            scope=scope,
            duration_s=time.monotonic() - started,
            result_code=int(getattr(result, "result", 0)),
            result_message=str(getattr(result, "error_message", "") or ""),
            error="" if self._result_ok(result) else self._result_message(result),
        )

    def set_simulation_state(self, *, state: str) -> SimulationControlResult:
        """Call ``SetSimulationState`` with a bounded timeout."""
        operation = "set_state"
        target_state = self._normalize_simulation_state(state)
        started = time.monotonic()
        service_name = self._resolve_service_name(self._SET_SIMULATION_STATE_SERVICE_TYPE)
        if service_name is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=None,
                duration_s=time.monotonic() - started,
                state=target_state,
                error="SetSimulationState service not available",
            )

        request = self._srv.SetSimulationState.Request()
        request.state.state = self._simulation_state_value(target_state)
        try:
            response = self._call_service(service_name, self._srv.SetSimulationState, request)
        except Exception as exc:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                state=target_state,
                error=str(exc),
            )
        result = getattr(response, "result", None)
        if result is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                state=target_state,
                error="SetSimulationState response did not include a result field",
            )
        success = self._result_ok(result) or int(result.result) == int(
            self._srv.SetSimulationState.Response.ALREADY_IN_TARGET_STATE
        )
        return SimulationControlResult(
            success=success,
            operation=operation,
            service_name=service_name,
            duration_s=time.monotonic() - started,
            result_code=int(getattr(result, "result", 0)),
            result_message=str(getattr(result, "error_message", "") or ""),
            error="" if success else self._result_message(result),
            state=target_state,
        )

    def step(self, *, steps: int = 1) -> SimulationControlResult:
        """Call ``StepSimulation`` with a bounded timeout."""
        operation = "step"
        step_count = int(steps)
        if step_count < 1:
            raise ValueError("StepSimulation steps must be >= 1")
        started = time.monotonic()
        service_name = self._resolve_service_name(self._STEP_SIMULATION_SERVICE_TYPE)
        if service_name is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=None,
                duration_s=time.monotonic() - started,
                steps=step_count,
                error="StepSimulation service not available",
            )

        request = self._srv.StepSimulation.Request()
        request.steps = step_count
        try:
            response = self._call_service(service_name, self._srv.StepSimulation, request)
        except Exception as exc:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                steps=step_count,
                error=str(exc),
            )
        result = getattr(response, "result", None)
        if result is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                steps=step_count,
                error="StepSimulation response did not include a result field",
            )
        return SimulationControlResult(
            success=self._result_ok(result),
            operation=operation,
            service_name=service_name,
            duration_s=time.monotonic() - started,
            result_code=int(getattr(result, "result", 0)),
            result_message=str(getattr(result, "error_message", "") or ""),
            error="" if self._result_ok(result) else self._result_message(result),
            steps=step_count,
        )

    def set_entity_state(self, *, entity: str, pose: dict[str, Any]) -> SimulationControlResult:
        """Call ``SetEntityState`` with a pose-only entity state."""
        operation = "set_entity_state"
        entity_name = str(entity or "").strip()
        if not entity_name:
            raise ValueError("SetEntityState entity is required")
        started = time.monotonic()
        service_name = self._resolve_service_name(self._SET_ENTITY_STATE_SERVICE_TYPE)
        if service_name is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=None,
                duration_s=time.monotonic() - started,
                entity=entity_name,
                error="SetEntityState service not available",
            )

        request = self._srv.SetEntityState.Request()
        request.entity = entity_name
        self._populate_entity_state(request.state, pose)
        try:
            response = self._call_service(service_name, self._srv.SetEntityState, request)
        except Exception as exc:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                entity=entity_name,
                error=str(exc),
            )
        result = getattr(response, "result", None)
        if result is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                entity=entity_name,
                error="SetEntityState response did not include a result field",
            )
        return SimulationControlResult(
            success=self._result_ok(result),
            operation=operation,
            service_name=service_name,
            duration_s=time.monotonic() - started,
            result_code=int(getattr(result, "result", 0)),
            result_message=str(getattr(result, "error_message", "") or ""),
            error="" if self._result_ok(result) else self._result_message(result),
            entity=entity_name,
        )

    def publish_command_gate(
        self,
        topic: str,
        *,
        enabled: bool,
        settle_s: float = 0.05,
    ) -> None:
        """Publish the ManyForge command-enable gate with transient-local QoS."""
        gate_topic = str(topic or "").strip()
        if not gate_topic:
            raise ValueError("command gate topic is required")
        qos = self._qos.QoSProfile(
            depth=1,
            durability=self._qos.DurabilityPolicy.TRANSIENT_LOCAL,
            reliability=self._qos.ReliabilityPolicy.RELIABLE,
        )
        publisher = self.node.create_publisher(self._std_msgs.Bool, gate_topic, qos)
        try:
            msg = self._std_msgs.Bool()
            msg.data = bool(enabled)
            publisher.publish(msg)
            if settle_s > 0.0:
                executor = getattr(self.node, "executor", None)
                if executor is not None and getattr(executor, "is_spinning", False):
                    time.sleep(settle_s)
                else:
                    deadline = time.monotonic() + settle_s
                    while time.monotonic() < deadline:
                        self._rclpy.spin_once(
                            self.node,
                            timeout_sec=min(
                                0.01,
                                max(0.0, deadline - time.monotonic()),
                            ),
                        )
        finally:
            self.node.destroy_publisher(publisher)

    def _resolve_reset_service_name(self) -> str | None:
        return self._resolve_service_name(self._RESET_SERVICE_TYPE)

    def _resolve_service_name(self, service_type: str) -> str | None:
        deadline = time.monotonic() + self.service_timeout_s
        while True:
            candidates = self._discover_service_names(service_type)
            selected = self._select_service_name(candidates)
            if selected is not None:
                return selected
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.05)

    def _discover_reset_service_names(self) -> list[str]:
        return self._discover_service_names(self._RESET_SERVICE_TYPE)

    def _discover_service_names(self, service_type: str) -> list[str]:
        services = self.node.get_service_names_and_types()
        names: list[str] = []
        for service_name, service_types in services:
            if service_type in service_types:
                names.append(service_name)
        return sorted(names, key=lambda name: _service_sort_key(name, service_type))

    def _select_service_name(self, candidates: list[str]) -> str | None:
        if not candidates:
            return None
        if self.service_prefix == "auto":
            return candidates[0]
        prefix = "/" + self.service_prefix.strip("/")
        scoped = [
            name
            for name in candidates
            if name == prefix or name.startswith(prefix + "/")
        ]
        return scoped[0] if scoped else None

    def _reset_scope_value(self, scope: str) -> int:
        scope_key = str(scope or "").strip().lower()
        constant_name = self._RESET_SCOPE_VALUES.get(scope_key)
        if constant_name is None:
            allowed = ", ".join(sorted(self._RESET_SCOPE_VALUES))
            raise ValueError(f"ResetSimulation scope must be one of: {allowed}")
        return int(getattr(self._srv.ResetSimulation.Request, constant_name))

    def _simulation_state_value(self, state: str) -> int:
        constant_name = self._SIMULATION_STATE_VALUES[state]
        return int(getattr(self._msg.SimulationState, constant_name))

    def _normalize_simulation_state(self, state: str) -> str:
        state_key = str(state or "").strip().lower()
        aliases = {
            "play": "playing",
            "playing": "playing",
            "pause": "paused",
            "paused": "paused",
        }
        normalized = aliases.get(state_key)
        if normalized is None:
            allowed = ", ".join(sorted(self._SIMULATION_STATE_VALUES))
            raise ValueError(f"SetSimulationState state must be one of: {allowed}")
        return normalized

    def _populate_entity_state(self, state_msg: object, pose: dict[str, Any]) -> None:
        if not isinstance(pose, dict):
            raise ValueError("SetEntityState pose must be a mapping")
        frame_id = str(pose.get("frame_id") or "").strip()
        if not frame_id:
            raise ValueError("SetEntityState pose.frame_id is required")
        position = pose.get("position_m")
        if not isinstance(position, list) or len(position) != 3:
            raise ValueError("SetEntityState pose.position_m must have length 3")
        orientation = pose.get("orientation_xyzw")
        if orientation is None and "orientation_rpy_deg" in pose:
            orientation = _rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
        if not isinstance(orientation, list) or len(orientation) != 4:
            raise ValueError(
                "SetEntityState pose must include orientation_xyzw or orientation_rpy_deg"
            )

        state_msg.header.frame_id = frame_id
        state_msg.pose.position.x = float(position[0])
        state_msg.pose.position.y = float(position[1])
        state_msg.pose.position.z = float(position[2])
        state_msg.pose.orientation.x = float(orientation[0])
        state_msg.pose.orientation.y = float(orientation[1])
        state_msg.pose.orientation.z = float(orientation[2])
        state_msg.pose.orientation.w = float(orientation[3])

    def _call_service(self, service_name: str, service_type: type, request: object) -> object:
        client = self.node.create_client(service_type, service_name)
        try:
            if not client.wait_for_service(timeout_sec=self.service_timeout_s):
                raise TimeoutError(f"Timed out waiting for service {service_name}")
            future = client.call_async(request)
            return self._wait_for_future(future)
        finally:
            self.node.destroy_client(client)

    def _wait_for_future(self, future: object) -> object:
        deadline = time.monotonic() + self.service_timeout_s
        executor = getattr(self.node, "executor", None)
        if executor is None or not getattr(executor, "is_spinning", False):
            remaining = max(0.0, deadline - time.monotonic())
            self._rclpy.spin_until_future_complete(
                self.node,
                future,
                timeout_sec=remaining,
            )
            if future.done():
                return future.result()
        else:
            while time.monotonic() < deadline:
                if future.done():
                    return future.result()
                time.sleep(0.01)
        raise TimeoutError("Timed out waiting for simulation_interfaces response")

    @staticmethod
    def _result_ok(result: object) -> bool:
        return int(result.result) == int(result.RESULT_OK)

    @staticmethod
    def _result_message(result: object) -> str:
        return f"{result.result}: {result.error_message}"


def reset_once(
    *,
    scope: str = "state",
    service_prefix: str = "auto",
    service_timeout_s: float = 3.0,
    command_gate_enable_topic: str = "",
    close_command_gate: bool = False,
    reopen_command_gate: bool = False,
) -> SimulationResetResult:
    """One-shot CLI/programmatic reset helper."""
    rclpy = _import_module("rclpy")
    initialized_here = False
    if not rclpy.ok():
        rclpy.init(args=None)
        initialized_here = True
    node = rclpy.create_node("manyforge_simulation_control_reset")
    try:
        client = SimulationControlClient(
            node,
            service_prefix=service_prefix,
            service_timeout_s=service_timeout_s,
        )
        if close_command_gate:
            client.publish_command_gate(command_gate_enable_topic, enabled=False)
        result = client.reset(scope=scope)
        if result.success and reopen_command_gate:
            client.publish_command_gate(command_gate_enable_topic, enabled=True)
        return result
    finally:
        node.destroy_node()
        if initialized_here:
            rclpy.shutdown()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", default="state", choices=sorted(SimulationControlClient._RESET_SCOPE_VALUES))
    parser.add_argument("--service-prefix", default="auto")
    parser.add_argument("--timeout", type=float, default=3.0)
    parser.add_argument("--command-gate-enable-topic", default="")
    parser.add_argument("--close-command-gate", action="store_true")
    parser.add_argument("--reopen-command-gate", action="store_true")
    args = parser.parse_args()

    result = reset_once(
        scope=args.scope,
        service_prefix=args.service_prefix,
        service_timeout_s=args.timeout,
        command_gate_enable_topic=args.command_gate_enable_topic,
        close_command_gate=args.close_command_gate,
        reopen_command_gate=args.reopen_command_gate,
    )
    print(json.dumps(result.to_dict(), indent=2, sort_keys=True))
    return 0 if result.success else 2


def _service_sort_key(service_name: str, service_type: str) -> tuple[int, str]:
    preferred_by_type = {
        "simulation_interfaces/srv/ResetSimulation": {
            "/reset_simulation",
            "/ResetSimulation",
            "/isaacsim/reset_simulation",
            "/isaacsim/ResetSimulation",
        },
        "simulation_interfaces/srv/SetSimulationState": {
            "/set_simulation_state",
            "/SetSimulationState",
            "/isaacsim/set_simulation_state",
            "/isaacsim/SetSimulationState",
        },
        "simulation_interfaces/srv/StepSimulation": {
            "/step_simulation",
            "/StepSimulation",
            "/isaacsim/step_simulation",
            "/isaacsim/StepSimulation",
        },
        "simulation_interfaces/srv/SetEntityState": {
            "/set_entity_state",
            "/SetEntityState",
            "/isaacsim/set_entity_state",
            "/isaacsim/SetEntityState",
        },
    }
    preferred = preferred_by_type.get(service_type, set())
    return (0 if service_name in preferred else 1, service_name)


def _rpy_deg_to_xyzw(rpy_deg: Any) -> list[float]:
    if not isinstance(rpy_deg, list) or len(rpy_deg) != 3:
        raise ValueError("orientation_rpy_deg must have length 3")
    roll = float(rpy_deg[0]) * 3.141592653589793 / 180.0
    pitch = float(rpy_deg[1]) * 3.141592653589793 / 180.0
    yaw = float(rpy_deg[2]) * 3.141592653589793 / 180.0
    import math

    cr = math.cos(roll * 0.5)
    sr = math.sin(roll * 0.5)
    cp = math.cos(pitch * 0.5)
    sp = math.sin(pitch * 0.5)
    cy = math.cos(yaw * 0.5)
    sy = math.sin(yaw * 0.5)
    return [
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    ]


def _import_module(module_name: str) -> Any:
    import importlib

    return importlib.import_module(module_name)


if __name__ == "__main__":
    raise SystemExit(main())
