"""Bounded thin client for ROS ``simulation_interfaces`` control services.

The simulator remains the sole state owner and the retained ManyForge C++
execution backend remains the sole command-admission owner. This adapter only
issues typed simulator requests and returns their exact terminal result; it
does not publish a command gate or infer robot lifecycle state.
"""

from __future__ import annotations

import argparse
import importlib
import json
import math
import time
from collections.abc import Sequence
from dataclasses import asdict, dataclass
from itertools import islice
from numbers import Real
from typing import Any, Final


HISTORICAL_SOURCE_BLOB: Final = "769e97215b0df885dd7002ca20ded0e0469b6ef7"
HISTORICAL_TEST_BLOB: Final = "1d9c3d6bb93f7ccc7f377ef7e151e91f338f890f"

_MAX_ADAPTER_ROWS: Final = 4096
_MAX_IDENTIFIER_CHARS: Final = 1024
_MAX_ERROR_CHARS: Final = 4096
_MAX_STEPS: Final = 1_000_000

RESET_SERVICE_TYPE: Final = "simulation_interfaces/srv/ResetSimulation"
GET_SIMULATION_STATE_SERVICE_TYPE: Final = (
    "simulation_interfaces/srv/GetSimulationState"
)
SET_SIMULATION_STATE_SERVICE_TYPE: Final = (
    "simulation_interfaces/srv/SetSimulationState"
)
STEP_SIMULATION_SERVICE_TYPE: Final = "simulation_interfaces/srv/StepSimulation"
SET_ENTITY_STATE_SERVICE_TYPE: Final = "simulation_interfaces/srv/SetEntityState"

_RESET_SCOPE_VALUES: Final = {"state"}
_SIMULATION_STATE_VALUES: Final = {"stopped", "playing", "paused"}
_PREFERRED_SERVICES_BY_TYPE: Final = {
    RESET_SERVICE_TYPE: {
        "/reset_simulation",
        "/ResetSimulation",
        "/isaacsim/reset_simulation",
        "/isaacsim/ResetSimulation",
    },
    GET_SIMULATION_STATE_SERVICE_TYPE: {
        "/get_simulation_state",
        "/GetSimulationState",
        "/isaacsim/get_simulation_state",
        "/isaacsim/GetSimulationState",
    },
    SET_SIMULATION_STATE_SERVICE_TYPE: {
        "/set_simulation_state",
        "/SetSimulationState",
        "/isaacsim/set_simulation_state",
        "/isaacsim/SetSimulationState",
    },
    STEP_SIMULATION_SERVICE_TYPE: {
        "/step_simulation",
        "/StepSimulation",
        "/isaacsim/step_simulation",
        "/isaacsim/StepSimulation",
    },
    SET_ENTITY_STATE_SERVICE_TYPE: {
        "/set_entity_state",
        "/SetEntityState",
        "/isaacsim/set_entity_state",
        "/isaacsim/SetEntityState",
    },
}


@dataclass(frozen=True)
class SimulationResetResult:
    """Structured outcome for one reset request."""

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
    """Structured outcome for one state, step, or entity request."""

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
    """Typed, bounded client for the selected simulator owner."""

    _RESET_SCOPE_CONSTANTS = {"state": "SCOPE_STATE"}
    _STATE_CONSTANTS = {
        "stopped": "STATE_STOPPED",
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
        if not callable(getattr(node, "get_service_names_and_types", None)):
            raise TypeError("node must expose the rclpy graph API")
        self.node = node
        self.service_prefix = _normalize_service_prefix(service_prefix)
        self.service_timeout_s = _bounded_timeout(service_timeout_s)
        self._rclpy = importlib.import_module("rclpy")
        self._srv = importlib.import_module("simulation_interfaces.srv")
        self._msg = importlib.import_module("simulation_interfaces.msg")

    def reset(self, *, scope: str = "state") -> SimulationResetResult:
        """Reset simulator state through the typed owner service."""

        normalized_scope = normalize_reset_scope(scope)
        started = time.monotonic()
        service_name = self._resolve_service_name(RESET_SERVICE_TYPE)
        if service_name is None:
            return SimulationResetResult(
                success=False,
                service_name=None,
                scope=normalized_scope,
                duration_s=time.monotonic() - started,
                error="ResetSimulation service not available",
            )
        request = self._srv.ResetSimulation.Request()
        request.scope = int(
            getattr(
                self._srv.ResetSimulation.Request,
                self._RESET_SCOPE_CONSTANTS[normalized_scope],
            )
        )
        try:
            response = self._call_service(
                service_name,
                self._srv.ResetSimulation,
                request,
            )
        except Exception as error:
            return SimulationResetResult(
                success=False,
                service_name=service_name,
                scope=normalized_scope,
                duration_s=time.monotonic() - started,
                error=_safe_error(error),
            )
        result = getattr(response, "result", None)
        if result is None:
            return SimulationResetResult(
                success=False,
                service_name=service_name,
                scope=normalized_scope,
                duration_s=time.monotonic() - started,
                error="ResetSimulation response has no result",
            )
        success = self._result_ok(result)
        return SimulationResetResult(
            success=success,
            service_name=service_name,
            scope=normalized_scope,
            duration_s=time.monotonic() - started,
            result_code=int(result.result),
            result_message=_safe_error(result.error_message),
            error="" if success else self._result_message(result),
        )

    def get_simulation_state(self) -> SimulationControlResult:
        """Read the simulator owner's exact current lifecycle state."""

        operation = "get_state"
        started = time.monotonic()
        service_name = self._resolve_service_name(
            GET_SIMULATION_STATE_SERVICE_TYPE
        )
        if service_name is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=None,
                duration_s=time.monotonic() - started,
                error="GetSimulationState service not available",
            )
        try:
            response = self._call_service(
                service_name,
                self._srv.GetSimulationState,
                self._srv.GetSimulationState.Request(),
            )
        except Exception as error:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                error=_safe_error(error),
            )
        result = getattr(response, "result", None)
        if result is None or not self._result_ok(result):
            return self._failed_control_result(
                operation=operation,
                service_name=service_name,
                started=started,
                result=result,
                missing="GetSimulationState response has no result",
            )
        state = self._state_name(int(response.state.state))
        if state is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                result_code=int(result.result),
                result_message=_safe_error(result.error_message),
                error=f"unknown SimulationState value: {response.state.state}",
            )
        return SimulationControlResult(
            success=True,
            operation=operation,
            service_name=service_name,
            duration_s=time.monotonic() - started,
            result_code=int(result.result),
            result_message=_safe_error(result.error_message),
            state=state,
        )

    def set_simulation_state(self, *, state: str) -> SimulationControlResult:
        """Set and report one stopped, playing, or paused target state."""

        operation = "set_state"
        target_state = normalize_simulation_state(state)
        started = time.monotonic()
        service_name = self._resolve_service_name(
            SET_SIMULATION_STATE_SERVICE_TYPE
        )
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
        request.state.state = int(
            getattr(
                self._msg.SimulationState,
                self._STATE_CONSTANTS[target_state],
            )
        )
        try:
            response = self._call_service(
                service_name,
                self._srv.SetSimulationState,
                request,
            )
        except Exception as error:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                state=target_state,
                error=_safe_error(error),
            )
        result = getattr(response, "result", None)
        if result is None:
            return self._failed_control_result(
                operation=operation,
                service_name=service_name,
                started=started,
                state=target_state,
                missing="SetSimulationState response has no result",
            )
        success = self._result_ok(result) or int(result.result) == int(
            self._srv.SetSimulationState.Response.ALREADY_IN_TARGET_STATE
        )
        return SimulationControlResult(
            success=success,
            operation=operation,
            service_name=service_name,
            duration_s=time.monotonic() - started,
            result_code=int(result.result),
            result_message=_safe_error(result.error_message),
            error="" if success else self._result_message(result),
            state=target_state,
        )

    def step(self, *, steps: int = 1) -> SimulationControlResult:
        """Advance the stopped/paused simulator by a bounded exact step count."""

        if type(steps) is not int or not 1 <= steps <= _MAX_STEPS:
            raise ValueError(f"StepSimulation steps must be in [1, {_MAX_STEPS}]")
        operation = "step"
        started = time.monotonic()
        service_name = self._resolve_service_name(STEP_SIMULATION_SERVICE_TYPE)
        if service_name is None:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=None,
                duration_s=time.monotonic() - started,
                steps=steps,
                error="StepSimulation service not available",
            )
        request = self._srv.StepSimulation.Request()
        request.steps = steps
        try:
            response = self._call_service(
                service_name,
                self._srv.StepSimulation,
                request,
            )
        except Exception as error:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                steps=steps,
                error=_safe_error(error),
            )
        return self._control_result_from_response(
            response,
            operation=operation,
            service_name=service_name,
            started=started,
            steps=steps,
        )

    def set_entity_state(
        self,
        *,
        entity: str,
        pose: dict[str, Any],
    ) -> SimulationControlResult:
        """Set one declared entity pose through the simulator owner."""

        entity_name = _bounded_text(entity, label="SetEntityState entity")
        frame_id, position, orientation = normalize_entity_pose(pose)
        operation = "set_entity_state"
        started = time.monotonic()
        service_name = self._resolve_service_name(SET_ENTITY_STATE_SERVICE_TYPE)
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
        request.state.header.frame_id = frame_id
        (
            request.state.pose.position.x,
            request.state.pose.position.y,
            request.state.pose.position.z,
        ) = position
        (
            request.state.pose.orientation.x,
            request.state.pose.orientation.y,
            request.state.pose.orientation.z,
            request.state.pose.orientation.w,
        ) = orientation
        try:
            response = self._call_service(
                service_name,
                self._srv.SetEntityState,
                request,
            )
        except Exception as error:
            return SimulationControlResult(
                success=False,
                operation=operation,
                service_name=service_name,
                duration_s=time.monotonic() - started,
                entity=entity_name,
                error=_safe_error(error),
            )
        return self._control_result_from_response(
            response,
            operation=operation,
            service_name=service_name,
            started=started,
            entity=entity_name,
        )

    def _resolve_service_name(self, service_type: str) -> str | None:
        deadline = time.monotonic() + self.service_timeout_s
        while True:
            candidates = _typed_service_candidates(
                self.node.get_service_names_and_types(),
                service_type=service_type,
            )
            selected = select_simulation_service_name(
                candidates,
                service_type=service_type,
                service_prefix=self.service_prefix,
            )
            if selected is not None:
                return selected
            if time.monotonic() >= deadline:
                return None
            time.sleep(0.05)

    def _call_service(
        self,
        service_name: str,
        service_type: type,
        request: object,
    ) -> object:
        client = self.node.create_client(service_type, service_name)
        try:
            if not client.wait_for_service(timeout_sec=self.service_timeout_s):
                raise TimeoutError(f"service unavailable: {service_name}")
            future = client.call_async(request)
            return self._wait_for_future(future)
        finally:
            self.node.destroy_client(client)

    def _wait_for_future(self, future: object) -> object:
        deadline = time.monotonic() + self.service_timeout_s
        executor = getattr(self.node, "executor", None)
        if executor is None or not getattr(executor, "is_spinning", False):
            self._rclpy.spin_until_future_complete(
                self.node,
                future,
                timeout_sec=max(0.0, deadline - time.monotonic()),
            )
        else:
            while time.monotonic() < deadline and not future.done():
                time.sleep(0.01)
        if not future.done():
            raise TimeoutError("simulation_interfaces response timed out")
        response = future.result()
        if response is None:
            raise RuntimeError("simulation_interfaces returned no response")
        return response

    def _state_name(self, value: int) -> str | None:
        for state, constant in self._STATE_CONSTANTS.items():
            if int(getattr(self._msg.SimulationState, constant)) == value:
                return state
        return None

    def _control_result_from_response(
        self,
        response: object,
        *,
        operation: str,
        service_name: str,
        started: float,
        state: str | None = None,
        steps: int | None = None,
        entity: str | None = None,
    ) -> SimulationControlResult:
        result = getattr(response, "result", None)
        if result is None:
            return self._failed_control_result(
                operation=operation,
                service_name=service_name,
                started=started,
                state=state,
                steps=steps,
                entity=entity,
                missing=f"{operation} response has no result",
            )
        success = self._result_ok(result)
        return SimulationControlResult(
            success=success,
            operation=operation,
            service_name=service_name,
            duration_s=time.monotonic() - started,
            result_code=int(result.result),
            result_message=_safe_error(result.error_message),
            error="" if success else self._result_message(result),
            state=state,
            steps=steps,
            entity=entity,
        )

    def _failed_control_result(
        self,
        *,
        operation: str,
        service_name: str,
        started: float,
        result: object | None = None,
        state: str | None = None,
        steps: int | None = None,
        entity: str | None = None,
        missing: str,
    ) -> SimulationControlResult:
        return SimulationControlResult(
            success=False,
            operation=operation,
            service_name=service_name,
            duration_s=time.monotonic() - started,
            result_code=(int(result.result) if result is not None else None),
            result_message=(
                _safe_error(result.error_message) if result is not None else ""
            ),
            error=(self._result_message(result) if result is not None else missing),
            state=state,
            steps=steps,
            entity=entity,
        )

    @staticmethod
    def _result_ok(result: object) -> bool:
        return int(result.result) == int(result.RESULT_OK)

    @staticmethod
    def _result_message(result: object) -> str:
        return _safe_error(f"{result.result}: {result.error_message}")


def reset_once(
    *,
    scope: str = "state",
    service_prefix: str = "auto",
    service_timeout_s: float = 3.0,
) -> SimulationResetResult:
    """Run one typed reset without touching command admission."""

    rclpy = importlib.import_module("rclpy")
    initialized_here = not rclpy.ok()
    if initialized_here:
        rclpy.init(args=None)
    node = rclpy.create_node("manyforge_simulation_control_reset")
    try:
        return SimulationControlClient(
            node,
            service_prefix=service_prefix,
            service_timeout_s=service_timeout_s,
        ).reset(scope=scope)
    finally:
        node.destroy_node()
        if initialized_here and rclpy.ok():
            rclpy.shutdown()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scope", default="state", choices=["state"])
    parser.add_argument("--service-prefix", default="auto")
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()
    result = reset_once(
        scope=args.scope,
        service_prefix=args.service_prefix,
        service_timeout_s=args.timeout,
    )
    print(json.dumps(result.to_dict(), sort_keys=True, separators=(",", ":")))
    return 0 if result.success else 2


def select_simulation_service_name(
    candidates: Sequence[str],
    *,
    service_type: str,
    service_prefix: str = "auto",
) -> str | None:
    """Select one typed service deterministically from a bounded graph slice."""

    normalized_candidates = _bounded_service_candidates(candidates)
    normalized_service_type = _bounded_text(service_type, label="service type")
    ordered = sorted(
        normalized_candidates,
        key=lambda name: _service_sort_key(name, normalized_service_type),
    )
    if not ordered:
        return None
    normalized_prefix = _normalize_service_prefix(service_prefix)
    if normalized_prefix == "auto":
        return ordered[0]
    prefix = "/" + normalized_prefix.strip("/")
    scoped = [
        name for name in ordered if name == prefix or name.startswith(prefix + "/")
    ]
    return scoped[0] if scoped else None


def normalize_reset_scope(scope: str) -> str:
    normalized = _bounded_enum_token(scope, label="ResetSimulation scope")
    if normalized not in _RESET_SCOPE_VALUES:
        allowed = ", ".join(sorted(_RESET_SCOPE_VALUES))
        raise ValueError(f"ResetSimulation scope must be one of: {allowed}")
    return normalized


def normalize_simulation_state(state: str) -> str:
    state_key = _bounded_enum_token(state, label="SetSimulationState state")
    normalized = {
        "stop": "stopped",
        "stopped": "stopped",
        "play": "playing",
        "playing": "playing",
        "pause": "paused",
        "paused": "paused",
    }.get(state_key)
    if normalized not in _SIMULATION_STATE_VALUES:
        allowed = ", ".join(sorted(_SIMULATION_STATE_VALUES))
        raise ValueError(f"SetSimulationState state must be one of: {allowed}")
    return normalized


def normalize_entity_pose(
    pose: dict[str, Any],
) -> tuple[
    str,
    tuple[float, float, float],
    tuple[float, float, float, float],
]:
    if not isinstance(pose, dict):
        raise ValueError("SetEntityState pose must be a mapping")
    raw_frame_id = pose.get("frame_id")
    if raw_frame_id in (None, ""):
        raise ValueError("SetEntityState pose.frame_id is required")
    frame_id = _bounded_text(raw_frame_id, label="SetEntityState pose.frame_id")
    position = pose.get("position_m")
    if not isinstance(position, list) or len(position) != 3:
        raise ValueError("SetEntityState pose.position_m must have length 3")
    orientation = pose.get("orientation_xyzw")
    if orientation is None and "orientation_rpy_deg" in pose:
        orientation = rpy_deg_to_xyzw(pose["orientation_rpy_deg"])
    if not isinstance(orientation, list) or len(orientation) != 4:
        raise ValueError(
            "SetEntityState pose must include orientation_xyzw or orientation_rpy_deg"
        )
    normalized_position = tuple(
        _finite_float(value, label=f"SetEntityState pose.position_m[{index}]")
        for index, value in enumerate(position)
    )
    normalized_orientation = _normalized_quaternion(
        tuple(
            _finite_float(
                value,
                label=f"SetEntityState pose.orientation_xyzw[{index}]",
            )
            for index, value in enumerate(orientation)
        )
    )
    return frame_id, normalized_position, normalized_orientation


def rpy_deg_to_xyzw(rpy_deg: Any) -> list[float]:
    if not isinstance(rpy_deg, list) or len(rpy_deg) != 3:
        raise ValueError("orientation_rpy_deg must have length 3")
    roll, pitch, yaw = (
        math.radians(
            _finite_float(value, label=f"orientation_rpy_deg[{index}]")
        )
        for index, value in enumerate(rpy_deg)
    )
    cr, sr = math.cos(roll * 0.5), math.sin(roll * 0.5)
    cp, sp = math.cos(pitch * 0.5), math.sin(pitch * 0.5)
    cy, sy = math.cos(yaw * 0.5), math.sin(yaw * 0.5)
    return list(
        _normalized_quaternion(
            (
                sr * cp * cy - cr * sp * sy,
                cr * sp * cy + sr * cp * sy,
                cr * cp * sy - sr * sp * cy,
                cr * cp * cy + sr * sp * sy,
            )
        )
    )


def _service_sort_key(service_name: str, service_type: str) -> tuple[int, str]:
    preferred = _PREFERRED_SERVICES_BY_TYPE.get(service_type, set())
    return (0 if service_name in preferred else 1, service_name)


def _bounded_service_candidates(candidates: Sequence[str]) -> tuple[str, ...]:
    if isinstance(candidates, (str, bytes, bytearray)):
        raise ValueError("service candidates must be a row sequence")
    try:
        rows = tuple(islice(iter(candidates), _MAX_ADAPTER_ROWS + 1))
    except TypeError as error:
        raise ValueError("service candidates must be a row sequence") from error
    if len(rows) > _MAX_ADAPTER_ROWS:
        raise ValueError("service candidates exceed the adapter bound")
    normalized = tuple(
        _bounded_text(candidate, label="service candidate") for candidate in rows
    )
    if len(set(normalized)) != len(normalized):
        raise ValueError("service candidates must be unique")
    return normalized


def _typed_service_candidates(
    services: Any,
    *,
    service_type: str,
) -> tuple[str, ...]:
    normalized_service_type = _bounded_text(service_type, label="service type")
    if isinstance(services, (str, bytes, bytearray)):
        raise RuntimeError("simulation service graph must be a row sequence")
    try:
        graph_rows = tuple(islice(iter(services), _MAX_ADAPTER_ROWS + 1))
    except TypeError as error:
        raise RuntimeError(
            "simulation service graph must be a row sequence"
        ) from error
    if len(graph_rows) > _MAX_ADAPTER_ROWS:
        raise RuntimeError("simulation service graph exceeds its bound")
    candidates: list[str] = []
    service_type_rows = 0
    for row in graph_rows:
        try:
            service_name, service_types = row
        except (TypeError, ValueError) as error:
            raise RuntimeError(
                "simulation service graph rows must contain a name and type sequence"
            ) from error
        normalized_name = _bounded_text(service_name, label="service name")
        if isinstance(service_types, (str, bytes, bytearray)):
            raise RuntimeError("simulation service types must be a row sequence")
        try:
            type_rows = tuple(
                islice(iter(service_types), _MAX_ADAPTER_ROWS + 1)
            )
        except TypeError as error:
            raise RuntimeError(
                "simulation service types must be a row sequence"
            ) from error
        service_type_rows += len(type_rows)
        if service_type_rows > _MAX_ADAPTER_ROWS:
            raise RuntimeError("simulation service type rows exceed their bound")
        normalized_types = tuple(
            _bounded_text(value, label="service type") for value in type_rows
        )
        if normalized_service_type in normalized_types:
            candidates.append(normalized_name)
    return _bounded_service_candidates(candidates)


def _normalize_service_prefix(value: str) -> str:
    if not isinstance(value, str):
        raise ValueError("service prefix must be a string")
    if len(value) > _MAX_IDENTIFIER_CHARS:
        raise ValueError("service prefix exceeds the adapter bound")
    normalized = value.strip() or "auto"
    if normalized != "auto" and any(character.isspace() for character in normalized):
        raise ValueError("service prefix must not contain whitespace")
    return normalized


def _bounded_timeout(value: float) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError("service timeout must be numeric")
    try:
        timeout = float(value)
    except (OverflowError, TypeError, ValueError) as error:
        raise ValueError("service timeout must be numeric") from error
    if not math.isfinite(timeout) or not 0.05 <= timeout <= 120.0:
        raise ValueError("service timeout must be finite and in [0.05, 120]")
    return timeout


def _bounded_text(value: Any, *, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > _MAX_IDENTIFIER_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    text = value.strip()
    if not text:
        raise ValueError(f"{label} must be non-empty")
    return text


def _bounded_enum_token(value: Any, *, label: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    if len(value) > _MAX_IDENTIFIER_CHARS:
        raise ValueError(f"{label} exceeds the adapter bound")
    return value.strip().lower()


def _finite_float(value: Any, *, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise ValueError(f"{label} must be a numeric value, not text or boolean")
    try:
        number = float(value)
    except (OverflowError, TypeError, ValueError) as error:
        raise ValueError(f"{label} must be numeric") from error
    if not math.isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


def _normalized_quaternion(
    quaternion: tuple[float, ...],
) -> tuple[float, float, float, float]:
    if len(quaternion) != 4:
        raise ValueError("orientation quaternion must have length 4")
    norm = math.hypot(*quaternion)
    if not math.isfinite(norm) or norm <= 1e-12:
        raise ValueError("orientation quaternion must have finite nonzero norm")
    return tuple(value / norm for value in quaternion)  # type: ignore[return-value]


def _safe_error(value: object) -> str:
    return str(value or "")[:_MAX_ERROR_CHARS]


if __name__ == "__main__":
    raise SystemExit(main())
