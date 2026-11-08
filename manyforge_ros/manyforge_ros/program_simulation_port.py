"""Program-owned task adapter for typed ``simulation_interfaces`` clients.

The port translates already-validated portable Program inputs at one focused
boundary.  It owns neither simulator state nor robot command admission.  A
single bounded worker keeps ROS service waits off the Program tick threads,
and Program-run context cleanup remains fenced until every submitted service
request has settled.
"""

from __future__ import annotations

import hashlib
import json
import math
import os
import stat
import sys
from collections.abc import Mapping, Sequence
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from threading import RLock, Thread
from types import MappingProxyType
from typing import Any, Final, Protocol

from manyforge_behavior.orchestration_ports import (
    NodeAttemptIdentity,
    OwnerNodeInputs,
    OwnerTaskSnapshot,
    OwnerTaskState,
    ProgramRunPortContext,
)

from .simulation_control import SimulationControlClient


SIMULATION_PROGRAM_SITE_CONFIG_VERSION: Final = (
    "manyforge.simulation_program_site_config.v2"
)
_NODE_IDS: Final = frozenset(
    {
        "simulation.reset",
        "simulation.set_entity_pose",
        "simulation.set_state",
        "simulation.step",
    }
)
_MAX_CONFIG_BYTES: Final = 1024 * 1024
_MAX_TEXT_BYTES: Final = 4096
_MAX_MESSAGE_BYTES: Final = 4096


class _NodeRefused(ValueError):
    """A typed per-node refusal reported as one failed owner task."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(message)
        self.code = code


class _ControlClient(Protocol):
    def reset(self, *, scope: str = "state") -> object: ...

    def set_simulation_state(self, *, state: str) -> object: ...

    def step(self, *, steps: int = 1) -> object: ...

    def set_entity_state(
        self,
        *,
        entity: str,
        pose: dict[str, Any],
    ) -> object: ...


def _text(value: object, *, field_name: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > _MAX_TEXT_BYTES
        or any(character.isspace() or ord(character) < 32 for character in value)
    ):
        raise ValueError(
            f"{field_name} must be bounded non-empty identifier text"
        )
    return value


@dataclass(frozen=True, slots=True)
class SimulationProgramSiteConfig:
    """Launcher-owned machine facts of one ROS simulator owner.

    The simulator entity mapping and cell frame come from the active
    Deployment's object links (``ProgramRunPortContext.simulation_scene``).
    """

    service_prefix: str
    service_timeout_s: float

    def __post_init__(self) -> None:
        _text(self.service_prefix, field_name="service_prefix")
        if isinstance(self.service_timeout_s, bool) or not isinstance(
            self.service_timeout_s,
            (int, float),
        ):
            raise ValueError("service_timeout_s must be numeric")
        timeout = float(self.service_timeout_s)
        if not math.isfinite(timeout) or not 0.05 <= timeout <= 120.0:
            raise ValueError("service_timeout_s must be finite and in [0.05, 120]")
        object.__setattr__(self, "service_timeout_s", timeout)


def _json_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _read_plain_file(path: str | os.PathLike[str]) -> bytes:
    descriptor = -1
    try:
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW)
        status = os.fstat(descriptor)
        if (
            not stat.S_ISREG(status.st_mode)
            or status.st_size <= 0
            or status.st_size > _MAX_CONFIG_BYTES
        ):
            raise ValueError(
                "simulation Program site configuration must be one bounded regular file"
            )
        chunks: list[bytes] = []
        remaining = status.st_size
        while remaining:
            chunk = os.read(descriptor, min(remaining, 64 * 1024))
            if not chunk:
                raise ValueError(
                    "simulation Program site configuration changed while read"
                )
            chunks.append(chunk)
            remaining -= len(chunk)
        if os.read(descriptor, 1):
            raise ValueError(
                "simulation Program site configuration changed while read"
            )
        return b"".join(chunks)
    except OSError as error:
        raise ValueError(
            "simulation Program site configuration is not one readable plain file"
        ) from error
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def load_simulation_program_site_config(
    path: str | os.PathLike[str],
) -> SimulationProgramSiteConfig:
    """Load one strict, symlink-rejecting simulator site configuration."""

    try:
        root = json.loads(
            _read_plain_file(path).decode("utf-8"),
            object_pairs_hook=_json_object,
            parse_constant=lambda value: (_ for _ in ()).throw(
                ValueError(f"non-finite JSON number: {value}")
            ),
        )
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(
            "simulation Program site configuration is invalid JSON"
        ) from error
    if not isinstance(root, dict) or set(root) != {
        "api_version",
        "service_prefix",
        "service_timeout_s",
    }:
        raise ValueError(
            "simulation Program site configuration has unknown or missing fields"
        )
    if root["api_version"] != SIMULATION_PROGRAM_SITE_CONFIG_VERSION:
        raise ValueError("simulation Program site configuration version is unsupported")
    return SimulationProgramSiteConfig(
        service_prefix=root["service_prefix"],
        service_timeout_s=root["service_timeout_s"],
    )


@dataclass(frozen=True, slots=True)
class _BoundRun:
    context: ProgramRunPortContext
    # None when the active Deployment has no simulation object link.
    cell_frame_id: str | None
    # Declared scene resource -> simulator entity (Deployment links).
    scene_entities: Mapping[str, str]
    declared_scene_resources: frozenset[str]
    target_frames: Mapping[tuple[str, str], str]


@dataclass(slots=True)
class _Task:
    task_id: str
    node_id: str
    identity: NodeAttemptIdentity
    request_digest: str
    operation: str
    future: Future[object]
    cancel_requested: bool = False
    terminal: OwnerTaskSnapshot | None = None


def _declared_scene_resources(program: Mapping[str, Any]) -> frozenset[str]:
    rows = program.get("scene", ())
    if not isinstance(rows, Sequence) or isinstance(rows, (str, bytes)):
        raise ValueError("captured Program scene is invalid")
    result: set[str] = set()
    for index, row in enumerate(rows):
        if not isinstance(row, Mapping):
            raise ValueError(f"captured Program scene[{index}] is invalid")
        resource_id = _text(
            row.get("scene_resource_id"),
            field_name=f"captured Program scene[{index}].scene_resource_id",
        )
        if resource_id in result:
            raise ValueError("captured Program scene resources are not unique")
        result.add(resource_id)
    return frozenset(result)


def _simulation_scene(
    simulation_scene: Mapping[str, Any] | None,
    declared: frozenset[str],
) -> tuple[str | None, Mapping[str, str]]:
    """Cell frame and entity map of the Deployment links.

    A link to an object the Program does not declare (orphan) is ignored.
    """

    if simulation_scene is None:
        return None, MappingProxyType({})
    cell_frame_id = _text(
        simulation_scene["cell_frame_id"], field_name="simulation_scene.cell_frame_id"
    )
    entities = {
        row["scene_resource_id"]: _text(row["entity"], field_name="simulator entity")
        for row in simulation_scene["scene_entities"]
        if row["scene_resource_id"] in declared
    }
    return cell_frame_id, MappingProxyType(entities)


def _target_frames(
    program_binding: Mapping[str, Any],
) -> Mapping[tuple[str, str], str]:
    rows = program_binding.get("frame_bindings", ())
    if not isinstance(rows, Sequence) or isinstance(rows, (str, bytes)):
        raise ValueError("captured Program Binding frame bindings are invalid")
    result: dict[tuple[str, str], str] = {}
    for frame_index, row in enumerate(rows):
        if not isinstance(row, Mapping):
            raise ValueError(
                f"captured Program Binding frame_bindings[{frame_index}] is invalid"
            )
        reference = row.get("reference")
        if not isinstance(reference, Mapping):
            raise ValueError(
                f"captured Program Binding frame_bindings[{frame_index}].reference is invalid"
            )
        key = (
            _text(
                reference.get("component_id"),
                field_name="captured Program Binding frame component_id",
            ),
            _text(
                reference.get("frame_id"),
                field_name="captured Program Binding frame_id",
            ),
        )
        compiled = _text(
            row.get("compiled_frame_name"),
            field_name="captured Program Binding compiled_frame_name",
        )
        if key in result:
            raise ValueError("captured Program Binding frame reference is ambiguous")
        result[key] = compiled
    return MappingProxyType(result)


def _plain_json(value: Any) -> Any:
    if isinstance(value, Mapping):
        return {key: _plain_json(nested) for key, nested in value.items()}
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes)):
        return [_plain_json(nested) for nested in value]
    return value


def _digest(value: Mapping[str, Any]) -> str:
    encoded = json.dumps(
        _plain_json(value),
        allow_nan=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def _task_id(identity: NodeAttemptIdentity) -> str:
    return "simulation." + _digest(
        {
            "node_attempt_id": identity.node_attempt_id,
            "node_instance_id": identity.node_instance_id,
            "parent_node_attempt_id": identity.parent_node_attempt_id,
            "program_run_id": identity.program_run_id,
        }
    )


def _pose_for_simulator(
    pose: Mapping[str, Any],
    *,
    bound: _BoundRun,
) -> dict[str, Any]:
    frame = pose.get("frame_ref")
    if not isinstance(frame, Mapping):
        raise ValueError("simulation.set_entity_pose pose.frame_ref is invalid")
    kind = frame.get("kind")
    if kind == "cell_frame":
        if bound.cell_frame_id is None:
            raise _NodeRefused(
                "simulation_cell_frame_unavailable",
                "the active Deployment links no simulation entity, so the "
                "simulator cell frame is unknown",
            )
        frame_id = bound.cell_frame_id
    elif kind == "target_frame":
        reference = frame.get("frame")
        if not isinstance(reference, Mapping):
            raise ValueError("simulation.set_entity_pose pose frame_ref.frame is invalid")
        key = (
            _text(reference.get("component_id"), field_name="frame component_id"),
            _text(reference.get("frame_id"), field_name="frame frame_id"),
        )
        try:
            frame_id = bound.target_frames[key]
        except KeyError as error:
            raise ValueError(
                f"Program Binding has no exact frame {key[0]}/{key[1]}"
            ) from error
    elif kind == "scene_resource":
        resource_id = _text(
            frame.get("scene_resource_id"),
            field_name="pose frame scene_resource_id",
        )
        if resource_id not in bound.declared_scene_resources:
            raise ValueError("pose frame does not name a declared scene resource")
        try:
            frame_id = bound.scene_entities[resource_id]
        except KeyError as error:
            raise ValueError(
                f"scene resource {resource_id!r} has no simulator entity mapping"
            ) from error
    else:
        raise ValueError("pose frame kind is unsupported")
    orientation = pose.get("orientation")
    if not isinstance(orientation, Mapping):
        raise ValueError("simulation.set_entity_pose pose.orientation is invalid")
    result: dict[str, Any] = {
        "frame_id": frame_id,
        "position_m": _plain_json(pose.get("position_m")),
    }
    if "xyzw" in orientation:
        result["orientation_xyzw"] = _plain_json(orientation["xyzw"])
    elif "rpy_deg" in orientation:
        result["orientation_rpy_deg"] = _plain_json(orientation["rpy_deg"])
    else:
        raise ValueError("simulation.set_entity_pose pose orientation is absent")
    return result


class SimulationProgramTaskPort:
    """Bounded asynchronous owner port over one typed simulator client."""

    def __init__(
        self,
        control_client: _ControlClient,
        site_config: SimulationProgramSiteConfig,
        *,
        maximum_retained_tasks: int = 4096,
    ) -> None:
        for method in (
            "reset",
            "set_simulation_state",
            "step",
            "set_entity_state",
        ):
            if not callable(getattr(control_client, method, None)):
                raise TypeError("control_client does not implement the focused API")
        if not isinstance(site_config, SimulationProgramSiteConfig):
            raise TypeError("site_config must be a SimulationProgramSiteConfig")
        if (
            type(maximum_retained_tasks) is not int
            or not 1 <= maximum_retained_tasks <= 4096
        ):
            raise ValueError("maximum_retained_tasks must be in [1, 4096]")
        self._client = control_client
        self._site_config = site_config
        self._maximum_retained_tasks = maximum_retained_tasks
        self._executor = ThreadPoolExecutor(
            max_workers=1,
            thread_name_prefix="manyforge-simulation-program",
        )
        self._runs: dict[str, _BoundRun] = {}
        self._tasks: dict[str, _Task] = {}
        self._lock = RLock()
        self._closed = False

    def bind(self, context: ProgramRunPortContext) -> None:
        if not isinstance(context, ProgramRunPortContext):
            raise TypeError("simulation context must be a ProgramRunPortContext")
        declared = _declared_scene_resources(context.program)
        cell_frame_id, scene_entities = _simulation_scene(
            context.simulation_scene, declared
        )
        bound = _BoundRun(
            context=context,
            cell_frame_id=cell_frame_id,
            scene_entities=scene_entities,
            declared_scene_resources=declared,
            target_frames=_target_frames(context.program_binding),
        )
        with self._lock:
            self._require_open()
            current = self._runs.get(context.program_run_id)
            if current is not None:
                if current != bound:
                    raise ValueError("Program-run simulation context was rebound")
                return
            if len(self._runs) >= 4096:
                raise RuntimeError("Program-run simulation context retention is full")
            self._runs[context.program_run_id] = bound

    def unbind(self, program_run_id: str) -> None:
        _text(program_run_id, field_name="program_run_id")
        with self._lock:
            tasks = [
                task
                for task in self._tasks.values()
                if task.identity.program_run_id == program_run_id
            ]
            if any(not task.future.done() for task in tasks):
                raise RuntimeError(
                    "simulator request is still settling; Program run remains fenced"
                )
            for task in tasks:
                self._tasks.pop(task.task_id, None)
            self._runs.pop(program_run_id, None)

    def _require_open(self) -> None:
        if self._closed:
            raise RuntimeError("simulation Program owner is closed")

    def _prepare(
        self,
        node_id: str,
        inputs: OwnerNodeInputs,
        bound: _BoundRun,
    ) -> tuple[str, dict[str, Any]]:
        parameters = inputs.parameters
        if node_id == "simulation.reset":
            if parameters:
                raise ValueError("simulation.reset has unexpected parameters")
            return "reset", {"scope": "state"}
        if node_id == "simulation.set_state":
            if set(parameters) != {"state"} or parameters["state"] not in {
                "play",
                "pause",
            }:
                raise ValueError("simulation.set_state requires play or pause")
            return "set_state", {"state": parameters["state"]}
        if node_id == "simulation.step":
            steps = parameters.get("steps")
            if set(parameters) != {"steps"} or type(steps) is not int:
                raise ValueError("simulation.step requires one exact integer")
            return "step", {"steps": steps}
        if node_id == "simulation.set_entity_pose":
            if set(parameters) != {"pose", "scene_resource_id"}:
                raise ValueError(
                    "simulation.set_entity_pose has an invalid parameter set"
                )
            resource_id = _text(
                parameters["scene_resource_id"],
                field_name="scene_resource_id",
            )
            if resource_id not in bound.declared_scene_resources:
                raise ValueError("scene_resource_id is not declared by the Program")
            pose = parameters["pose"]
            if not isinstance(pose, Mapping):
                raise ValueError("simulation.set_entity_pose pose is invalid")
            simulator_pose = _pose_for_simulator(pose, bound=bound)
            try:
                entity = bound.scene_entities[resource_id]
            except KeyError as error:
                raise ValueError(
                    f"scene resource {resource_id!r} has no simulator entity mapping"
                ) from error
            return "set_entity_state", {"entity": entity, "pose": simulator_pose}
        raise ValueError(f"unsupported simulator Program node: {node_id}")

    def _invoke(self, operation: str, arguments: Mapping[str, Any]) -> object:
        if operation == "reset":
            return self._client.reset(scope=arguments["scope"])
        if operation == "set_state":
            return self._client.set_simulation_state(state=arguments["state"])
        if operation == "step":
            return self._client.step(steps=arguments["steps"])
        if operation == "set_entity_state":
            return self._client.set_entity_state(
                entity=arguments["entity"],
                pose=arguments["pose"],
            )
        raise RuntimeError("simulation operation dispatch is not closed")

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        if node_id not in _NODE_IDS:
            raise ValueError("node_id is outside the simulator owner family")
        if not isinstance(identity, NodeAttemptIdentity):
            raise TypeError("identity must be one NodeAttemptIdentity")
        if not isinstance(inputs, OwnerNodeInputs):
            raise TypeError("inputs must be one OwnerNodeInputs snapshot")
        task_id = _task_id(identity)
        with self._lock:
            self._require_open()
            try:
                bound = self._runs[identity.program_run_id]
            except KeyError as error:
                raise ValueError(
                    "Program-run simulation context is not bound"
                ) from error
            try:
                operation, arguments = self._prepare(node_id, inputs, bound)
            except _NodeRefused as refused:
                return OwnerTaskSnapshot(
                    task_id=task_id,
                    state=OwnerTaskState.FAILED,
                    reason_code=refused.code,
                    message=str(refused),
                )
            request_digest = _digest(
                {
                    "arguments": arguments,
                    "node_id": node_id,
                }
            )
            current = self._tasks.get(task_id)
            if current is not None:
                if (
                    current.identity != identity
                    or current.node_id != node_id
                    or current.request_digest != request_digest
                ):
                    raise ValueError("node-attempt identity was reused with new intent")
                return self._snapshot(current)
            if len(self._tasks) >= self._maximum_retained_tasks:
                # Bounded by eviction: drop the oldest finished task (its
                # outcome was already observable); only in-flight work refuses.
                finished = next(
                    (key for key, retained in self._tasks.items() if retained.future.done()),
                    None,
                )
                if finished is None:
                    raise RuntimeError("simulation task retention is full")
                del self._tasks[finished]
            future = self._executor.submit(self._invoke, operation, arguments)
            task = _Task(
                task_id=task_id,
                node_id=node_id,
                identity=identity,
                request_digest=request_digest,
                operation=operation,
                future=future,
            )
            self._tasks[task_id] = task
            return OwnerTaskSnapshot(
                task_id=task_id,
                state=OwnerTaskState.ACTIVE,
                metadata={"operation": operation},
            )

    @staticmethod
    def _message(value: object) -> str:
        text = "" if value is None else str(value)
        encoded = text.encode("utf-8", errors="replace")[:_MAX_MESSAGE_BYTES]
        return encoded.decode("utf-8", errors="ignore")

    def _result_snapshot(self, task: _Task, result: object) -> OwnerTaskSnapshot:
        success = getattr(result, "success", None)
        if type(success) is not bool:
            return OwnerTaskSnapshot(
                task_id=task.task_id,
                state=OwnerTaskState.FAILED,
                reason_code="simulation_result_invalid",
                message="simulation control result has no exact success flag",
                metadata={"operation": task.operation},
            )
        metadata: dict[str, Any] = {"operation": task.operation}
        for field_name in (
            "duration_s",
            "entity",
            "result_code",
            "service_name",
            "state",
            "steps",
        ):
            value = getattr(result, field_name, None)
            if value is not None:
                metadata[field_name] = value
        if success:
            return OwnerTaskSnapshot(
                task_id=task.task_id,
                state=OwnerTaskState.SUCCEEDED,
                metadata=metadata,
            )
        message = self._message(
            getattr(result, "error", "")
            or getattr(result, "result_message", "")
            or "simulator owner rejected the request"
        )
        return OwnerTaskSnapshot(
            task_id=task.task_id,
            state=OwnerTaskState.FAILED,
            reason_code="simulation_control_failed",
            message=message,
            metadata=metadata,
        )

    def _snapshot(self, task: _Task) -> OwnerTaskSnapshot:
        if task.terminal is not None:
            return task.terminal
        if not task.future.done():
            return OwnerTaskSnapshot(
                task_id=task.task_id,
                state=OwnerTaskState.ACTIVE,
                metadata={"operation": task.operation},
            )
        if task.cancel_requested or task.future.cancelled():
            task.terminal = OwnerTaskSnapshot(
                task_id=task.task_id,
                state=OwnerTaskState.INTERRUPTED,
                reason_code="simulation_request_cancelled",
                message="request result was fenced after Program cancellation",
                metadata={"operation": task.operation},
            )
            return task.terminal
        try:
            result = task.future.result()
        except Exception as error:
            task.terminal = OwnerTaskSnapshot(
                task_id=task.task_id,
                state=OwnerTaskState.FAILED,
                reason_code="simulation_control_exception",
                message=self._message(error) or type(error).__name__,
                metadata={"operation": task.operation},
            )
            return task.terminal
        task.terminal = self._result_snapshot(task, result)
        return task.terminal

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        _text(task_id, field_name="task_id")
        with self._lock:
            self._require_open()
            try:
                return self._snapshot(self._tasks[task_id])
            except KeyError as error:
                raise ValueError("simulation task does not exist") from error

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        with self._lock:
            self._require_open()
            try:
                task = self._tasks[task_id]
            except KeyError as error:
                raise ValueError("simulation task does not exist") from error
            if task.identity != identity:
                raise ValueError("simulation task identity does not match")
            task.cancel_requested = True
            task.future.cancel()

    def acknowledge(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        with self._lock:
            self._require_open()
            try:
                task = self._tasks[task_id]
            except KeyError as error:
                raise ValueError("simulation task does not exist") from error
            if task.identity != identity:
                raise ValueError("simulation task identity does not match")
            if self._snapshot(task).state is not OwnerTaskState.SUCCEEDED:
                raise ValueError(
                    "only a successful simulation task may be acknowledged"
                )

    def close(self) -> None:
        with self._lock:
            if self._closed:
                return
            self._closed = True
            tasks = tuple(self._tasks.values())
            for task in tasks:
                task.cancel_requested = True
                task.future.cancel()
        self._executor.shutdown(wait=True, cancel_futures=True)
        with self._lock:
            self._tasks.clear()
            self._runs.clear()


class RosSimulationProgramTaskPort:
    """Own an isolated rclpy node plus the focused Program task adapter."""

    def __init__(
        self,
        site_config: SimulationProgramSiteConfig,
        *,
        maximum_retained_tasks: int = 4096,
    ) -> None:
        if not isinstance(site_config, SimulationProgramSiteConfig):
            raise TypeError("site_config must be a SimulationProgramSiteConfig")
        import rclpy
        from rclpy.context import Context
        from rclpy.executors import SingleThreadedExecutor

        self._context = Context()
        ros_args = [*sys.argv, "--ros-args", "--disable-external-lib-logs"]
        rclpy.init(args=ros_args, context=self._context)
        self._node = rclpy.create_node(
            "manyforge_program_simulation_control",
            context=self._context,
        )
        self._ros_executor = SingleThreadedExecutor(context=self._context)
        self._ros_executor.add_node(self._node)
        self._spin_thread = Thread(
            target=self._ros_executor.spin,
            name="manyforge-simulation-rclpy",
            daemon=True,
        )
        self._spin_thread.start()
        try:
            client = SimulationControlClient(
                self._node,
                service_prefix=site_config.service_prefix,
                service_timeout_s=site_config.service_timeout_s,
            )
            self._port = SimulationProgramTaskPort(
                client,
                site_config,
                maximum_retained_tasks=maximum_retained_tasks,
            )
        except Exception:
            self._shutdown_ros(site_config.service_timeout_s)
            raise
        self._timeout_s = site_config.service_timeout_s
        self._closed = False

    def bind(self, context: ProgramRunPortContext) -> None:
        self._port.bind(context)

    def unbind(self, program_run_id: str) -> None:
        self._port.unbind(program_run_id)

    def start(
        self,
        node_id: str,
        identity: NodeAttemptIdentity,
        inputs: OwnerNodeInputs,
    ) -> OwnerTaskSnapshot:
        return self._port.start(node_id, identity, inputs)

    def observe(self, task_id: str) -> OwnerTaskSnapshot:
        return self._port.observe(task_id)

    def cancel(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self._port.cancel(task_id, identity)

    def acknowledge(self, task_id: str, identity: NodeAttemptIdentity) -> None:
        self._port.acknowledge(task_id, identity)

    def _shutdown_ros(self, timeout_s: float) -> None:
        self._ros_executor.shutdown(timeout_sec=timeout_s + 1.0)
        self._spin_thread.join(timeout=timeout_s + 1.0)
        if self._spin_thread.is_alive():
            raise RuntimeError("simulation rclpy executor did not stop")
        self._node.destroy_node()
        if self._context.ok():
            self._context.shutdown()

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        errors: list[Exception] = []
        try:
            self._port.close()
        except Exception as error:
            errors.append(error)
        try:
            self._shutdown_ros(self._timeout_s)
        except Exception as error:
            errors.append(error)
        if errors:
            raise RuntimeError(
                "; ".join(str(error) or type(error).__name__ for error in errors)
            ) from errors[0]


def create_ros_simulation_program_port(
    path: str | os.PathLike[str],
    *,
    maximum_retained_tasks: int = 4096,
) -> RosSimulationProgramTaskPort:
    """Construct the production ROS owner from one checked site config."""

    return RosSimulationProgramTaskPort(
        load_simulation_program_site_config(Path(path)),
        maximum_retained_tasks=maximum_retained_tasks,
    )


__all__ = [
    "RosSimulationProgramTaskPort",
    "SIMULATION_PROGRAM_SITE_CONFIG_VERSION",
    "SimulationProgramSiteConfig",
    "SimulationProgramTaskPort",
    "create_ros_simulation_program_port",
    "load_simulation_program_site_config",
]
