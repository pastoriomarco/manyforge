"""Qualification-only planner worker containment for composable-cell E1.

This module proves the OS/process boundary required for an opaque planner that
has no cooperative cancellation hook.  It is deliberately not a production
planner adapter and owns no Program, scene, collision, or execution meaning.
I6 may implement the same observable contract in C++ after E1 selects one
dependency baseline.
"""

from __future__ import annotations

import argparse
import array
import ctypes
import dataclasses
import fcntl
import hashlib
import json
import math
import os
import pathlib
import platform
import re
import resource
import select
import signal
import socket
import stat
import statistics
import struct
import subprocess
import sys
import threading
import time
from collections.abc import Sequence
from typing import Any, Final


_SCHEMA: Final = "manyforge.e1.planner_worker.v1"
_MAX_FRAME_BYTES: Final = 4096
_RESULT_MAGIC: Final = b"MFE1RRT1"
_RESULT_HEADER: Final = struct.Struct("<8sII")
_RESULT_SEALS: Final = (
    fcntl.F_SEAL_GROW | fcntl.F_SEAL_SHRINK | fcntl.F_SEAL_WRITE | fcntl.F_SEAL_SEAL
)
_PR_SET_PDEATHSIG: Final = 1
_EXIT_CRASH: Final = 86
_EXIT_MEMORY_LIMIT: Final = 87
_EXIT_MEMORY_LIMIT_BREACH: Final = 88
_SHA256_IDENTITY_PATTERN: Final = re.compile(r"sha256:[0-9a-f]{64}")
_PACKAGE_URI_PATTERN: Final = re.compile(rb"package://([A-Za-z0-9_.-]+)/([^\"'<>\s]+)")
_SYNTHETIC_PROJECTION_DIGEST: Final = (
    "sha256:" + hashlib.sha256(b"manyforge-e1-synthetic-projection-v1").hexdigest()
)
_THREAD_ENVIRONMENT: Final = (
    "OMP_NUM_THREADS",
    "OPENBLAS_NUM_THREADS",
    "MKL_NUM_THREADS",
    "VECLIB_MAXIMUM_THREADS",
    "NUMEXPR_NUM_THREADS",
)
_ALLOWED_OPERATIONS: Final = frozenset(
    {
        "candidate_collision",
        "candidate_rrt",
        "success",
        "slow_success",
        "noncooperative",
        "noncooperative_descendant",
        "stale_worker_generation",
        "stale_projection_generation",
        "stale_projection_digest",
        "crash",
        "memory_exhaustion",
        "oversized_result",
    }
)
_PROGRESS_RANK: Final = {
    "admitted": 0,
    "dispatched": 1,
    "running": 2,
    "cancel_requested": 3,
    "deadline_reached": 3,
    "worker_retired": 4,
    "replacement_ready": 5,
    "terminal": 6,
}


class PlannerWorkerError(RuntimeError):
    """The qualification worker protocol or supervisor failed."""


class PlannerRequestError(ValueError):
    """A request violated the ManyForge-owned pre-dispatch policy."""

    def __init__(self, reason: str, detail: str) -> None:
        super().__init__(detail)
        self.reason = reason
        self.detail = detail


@dataclasses.dataclass(frozen=True)
class PlannerWorkerPolicy:
    """Finite prototype limits; final I6 values require selected evidence."""

    minimum_sample_period_s: float = 0.0001
    maximum_sample_period_s: float = 1.0
    maximum_duration_s: float = 3600.0
    maximum_output_samples: int = 65_536
    maximum_result_dof: int = 64
    maximum_result_bytes: int = 64 * 1024 * 1024
    maximum_waypoints: int = 10_000
    maximum_work_ms: int = 60_000
    worker_memory_bytes: int = 256 * 1024 * 1024
    worker_file_bytes: int = 64 * 1024 * 1024
    worker_cpu_seconds: int = 5
    worker_start_timeout_s: float = 2.0
    kill_grace_s: float = 0.02
    replacement_timeout_s: float = 2.0
    worker_bootstrap: str = "synthetic"

    def validate(self) -> None:
        if self.worker_bootstrap not in {
            "synthetic",
            "synthetic_projection_drift",
            "roboplan_collision",
        }:
            raise ValueError("worker bootstrap is unsupported")
        finite_positive = (
            self.minimum_sample_period_s,
            self.maximum_sample_period_s,
            self.maximum_duration_s,
            self.worker_start_timeout_s,
            self.kill_grace_s,
            self.replacement_timeout_s,
        )
        if not all(math.isfinite(value) and value > 0.0 for value in finite_positive):
            raise ValueError(
                "worker policy floating limits must be finite and positive"
            )
        if self.minimum_sample_period_s > self.maximum_sample_period_s:
            raise ValueError("minimum sample period exceeds maximum")
        integer_positive = (
            self.maximum_output_samples,
            self.maximum_result_dof,
            self.maximum_result_bytes,
            self.maximum_waypoints,
            self.maximum_work_ms,
            self.worker_memory_bytes,
            self.worker_file_bytes,
            self.worker_cpu_seconds,
        )
        if any(type(value) is not int or value <= 0 for value in integer_positive):
            raise ValueError("worker policy integer limits must be positive integers")
        if self.worker_memory_bytes < 64 * 1024 * 1024:
            raise ValueError("worker memory limit is too small for the Python probe")
        required_result_bytes = (
            _RESULT_HEADER.size
            + self.maximum_output_samples * self.maximum_result_dof * 8
        )
        if required_result_bytes > self.maximum_result_bytes:
            raise ValueError(
                "result byte bound cannot represent the admitted sample/dof bound"
            )
        if self.worker_file_bytes < self.maximum_result_bytes:
            raise ValueError(
                "worker file limit is smaller than the admitted result byte bound"
            )


@dataclasses.dataclass(frozen=True)
class PlannerWorkRequest:
    """Planner-neutral bounded input used only by the E1 containment probe."""

    projection_generation: int
    deadline_monotonic_ns: int
    operation: str = "success"
    sample_period_s: float = 0.01
    requested_duration_s: float = 1.0
    velocity_scale: float = 1.0
    acceleration_scale: float = 1.0
    max_output_samples: int = 1_000
    waypoint_count: int = 2
    work_ms: int = 0


@dataclasses.dataclass(frozen=True)
class PlannerWorkerOutcome:
    ok: bool
    code: str
    reason: str
    request_id: int | None
    projection_generation: int
    worker_generation: int | None
    worker_pid: int | None
    replacement_generation: int | None
    replacement_pid: int | None
    progress: tuple[str, ...]
    wall_ms: float
    kill_ms: float | None = None
    replacement_ms: float | None = None
    projection_digest: str | None = None
    candidate_digest: str | None = None
    result_transport: str | None = None
    trajectory_positions: tuple[tuple[float, ...], ...] | None = None


@dataclasses.dataclass(frozen=True)
class ActiveWorkerObservation:
    request_id: int
    worker_generation: int
    worker_pid: int
    descendant_pid: int | None


@dataclasses.dataclass
class _WorkerHandle:
    generation: int
    process: subprocess.Popen[bytes]
    read_buffer: bytearray
    ready: dict[str, object]
    result_socket: socket.socket


@dataclasses.dataclass
class _ActiveRequest:
    request_id: int
    projection_generation: int
    worker_generation: int
    worker_pid: int
    max_output_samples: int
    progress: list[str]
    started: bool = False
    descendant_pid: int | None = None
    interrupt_reason: str | None = None
    replacement_generation: int | None = None
    replacement_pid: int | None = None
    kill_ms: float | None = None
    replacement_ms: float | None = None


@dataclasses.dataclass(frozen=True)
class _CandidateProjection:
    scene: object
    collision_query: object
    planner: object
    start: object
    goal: object
    digest: str
    native_modules: dict[str, dict[str, str]]


def _is_sha256_identity(value: object) -> bool:
    return type(value) is str and _SHA256_IDENTITY_PATTERN.fullmatch(value) is not None


def _hash_frame(digest: Any, label: str, payload: bytes) -> None:
    """Add one unambiguous logical-path/content frame to a SHA-256 digest."""

    label_bytes = label.encode("utf-8")
    digest.update(len(label_bytes).to_bytes(8, "little"))
    digest.update(label_bytes)
    digest.update(len(payload).to_bytes(8, "little"))
    digest.update(payload)


def _sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _projection_resource_digest(
    urdf: pathlib.Path,
    srdf: pathlib.Path,
    package_share_roots: Sequence[pathlib.Path],
) -> str:
    """Hash the exact fixture inputs and package-resource content, not host paths."""

    if not package_share_roots:
        raise PlannerWorkerError("projection package-search closure is empty")
    projection_hash = hashlib.sha256()
    projection_hash.update(b"manyforge.e1.projection-resource-closure.v1\0")
    source_payloads: dict[str, bytes] = {}
    for label, path in (("urdf", urdf), ("srdf", srdf)):
        if path.is_symlink() or not path.is_file():
            raise PlannerWorkerError(f"projection {label} is not a regular file")
        payload = path.read_bytes()
        source_payloads[label] = payload
        _hash_frame(projection_hash, f"source/{label}", payload)

    resources = sorted(
        {
            (package.decode("ascii"), relative.decode("utf-8"))
            for payload in source_payloads.values()
            for package, relative in _PACKAGE_URI_PATTERN.findall(payload)
        }
    )
    for package_name, relative_text in resources:
        relative_path = pathlib.PurePosixPath(relative_text)
        if relative_path.is_absolute() or any(
            part in {"", ".", ".."} for part in relative_path.parts
        ):
            raise PlannerWorkerError("projection package URI is not canonical")
        selected: tuple[int, pathlib.Path] | None = None
        for root_index, root in enumerate(package_share_roots):
            resource_path = root / package_name / pathlib.Path(*relative_path.parts)
            if resource_path.is_file() and not resource_path.is_symlink():
                selected = (root_index, resource_path)
                break
        if selected is None:
            raise PlannerWorkerError(
                f"projection package resource is unresolved: {package_name}/{relative_text}"
            )
        root_index, resource_path = selected
        _hash_frame(
            projection_hash,
            f"resource-resolution/{package_name}/{relative_path.as_posix()}",
            str(root_index).encode("ascii"),
        )
        _hash_frame(
            projection_hash,
            f"resource/{package_name}/{relative_path.as_posix()}",
            resource_path.read_bytes(),
        )
    return f"sha256:{projection_hash.hexdigest()}"


def _candidate_projection_paths() -> tuple[
    pathlib.Path,
    pathlib.Path,
    tuple[pathlib.Path, ...],
]:
    # This parent-safe import only locates immutable example-model resources;
    # all native planner imports and mutable state remain worker-private.
    from roboplan.example_models import (  # type: ignore[import-not-found]
        get_package_models_dir,
        get_package_share_dir,
    )

    models = pathlib.Path(get_package_models_dir())
    return (
        models / "ur_robot_model/ur5_gripper.urdf",
        models / "ur_robot_model/ur5_gripper.srdf",
        (pathlib.Path(get_package_share_dir()),),
    )


def _validated_native_modules(
    value: object,
    *,
    candidate_required: bool,
) -> dict[str, dict[str, str]]:
    if type(value) is not dict:
        raise PlannerWorkerError("worker native-module attestation is unavailable")
    if candidate_required and set(value) != {"core", "rrt"}:
        raise PlannerWorkerError("worker native-module attestation is incomplete")
    if not candidate_required and value:
        raise PlannerWorkerError(
            "synthetic worker unexpectedly loaded native planner modules"
        )
    validated: dict[str, dict[str, str]] = {}
    for name, identity in value.items():
        if (
            type(name) is not str
            or type(identity) is not dict
            or set(identity) != {"path", "sha256"}
            or type(identity.get("path")) is not str
            or not str(identity["path"])
            or type(identity.get("sha256")) is not str
            or re.fullmatch(r"[0-9a-f]{64}", str(identity["sha256"])) is None
        ):
            raise PlannerWorkerError("worker native-module attestation is invalid")
        validated[name] = {
            "path": str(identity["path"]),
            "sha256": str(identity["sha256"]),
        }
    return validated


def _validated_worker_runtime(value: object) -> dict[str, object]:
    if (
        type(value) is not dict
        or set(value) != {"architecture", "python", "executable", "cpu_affinity"}
        or not all(
            type(value.get(name)) is str and bool(value[name])
            for name in ("architecture", "python", "executable")
        )
        or type(value.get("cpu_affinity")) is not list
        or not value["cpu_affinity"]
        or any(type(cpu) is not int or cpu < 0 for cpu in value["cpu_affinity"])
        or value["cpu_affinity"] != sorted(set(value["cpu_affinity"]))
    ):
        raise PlannerWorkerError("worker runtime attestation is invalid")
    return {
        "architecture": str(value["architecture"]),
        "python": str(value["python"]),
        "executable": str(value["executable"]),
        "cpu_affinity": list(value["cpu_affinity"]),
    }


def _validated_thread_policy(value: object) -> dict[str, str]:
    if (
        type(value) is not dict
        or set(value) != set(_THREAD_ENVIRONMENT)
        or any(value.get(name) != "1" for name in _THREAD_ENVIRONMENT)
    ):
        raise PlannerWorkerError("worker thread-policy attestation is invalid")
    return {name: "1" for name in _THREAD_ENVIRONMENT}


def _bounded_json(value: object) -> bytes:
    try:
        payload = json.dumps(
            value,
            sort_keys=True,
            separators=(",", ":"),
            ensure_ascii=True,
            allow_nan=False,
        ).encode("utf-8")
    except (TypeError, ValueError) as exc:
        raise PlannerWorkerError(f"worker frame is not canonical JSON: {exc}") from exc
    if len(payload) + 1 > _MAX_FRAME_BYTES:
        raise PlannerWorkerError("worker frame exceeds the protocol bound")
    return payload + b"\n"


def _write_all(descriptor: int, payload: bytes) -> None:
    offset = 0
    while offset < len(payload):
        written = os.write(descriptor, payload[offset:])
        if written <= 0:
            raise PlannerWorkerError("worker frame write made no progress")
        offset += written


def _write_frame(descriptor: int, value: object) -> None:
    _write_all(descriptor, _bounded_json(value))


def _decode_frame(payload: bytes) -> dict[str, object]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise PlannerWorkerError(f"worker frame is invalid JSON: {exc}") from exc
    if type(value) is not dict:
        raise PlannerWorkerError("worker frame root is not an object")
    if value.get("schema") != _SCHEMA:
        raise PlannerWorkerError("worker frame schema mismatch")
    return value


def _read_worker_input() -> dict[str, object] | None:
    payload = sys.stdin.buffer.readline(_MAX_FRAME_BYTES + 1)
    if payload == b"":
        return None
    if len(payload) > _MAX_FRAME_BYTES or not payload.endswith(b"\n"):
        raise PlannerWorkerError("worker input frame exceeds the protocol bound")
    return _decode_frame(payload[:-1])


def _set_limit(kind: int, requested: int) -> None:
    _, hard = resource.getrlimit(kind)
    selected = requested
    if hard != resource.RLIM_INFINITY:
        selected = min(selected, int(hard))
    if selected <= 0:
        raise PlannerWorkerError("worker resource limit is unavailable")
    resource.setrlimit(kind, (selected, selected))


def _apply_worker_limits(policy: PlannerWorkerPolicy) -> None:
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    _set_limit(resource.RLIMIT_AS, policy.worker_memory_bytes)
    _set_limit(resource.RLIMIT_FSIZE, policy.worker_file_bytes)
    _set_limit(resource.RLIMIT_CPU, policy.worker_cpu_seconds)


def _limit_snapshot() -> dict[str, int]:
    def soft(kind: int) -> int:
        value, _ = resource.getrlimit(kind)
        return int(value)

    return {
        "address_space_bytes": soft(resource.RLIMIT_AS),
        "file_bytes": soft(resource.RLIMIT_FSIZE),
        "cpu_seconds": soft(resource.RLIMIT_CPU),
        "core_bytes": soft(resource.RLIMIT_CORE),
    }


def _arm_parent_death_signal(expected_parent_pid: int) -> None:
    """Kill this process if its exact supervisor disappears."""

    # PID 1 is a valid exact supervisor when this qualification runs as the
    # container entrypoint.  Only non-positive values are invalid identities.
    if type(expected_parent_pid) is not int or expected_parent_pid <= 0:
        raise PlannerWorkerError("worker parent identity is invalid")
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(_PR_SET_PDEATHSIG, signal.SIGKILL, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        raise PlannerWorkerError(f"PR_SET_PDEATHSIG failed with errno {error}")
    # Close the fork/exec-to-prctl race: if reparenting already happened,
    # terminate now rather than waiting for a signal that was missed.
    if os.getppid() != expected_parent_pid:
        os.kill(os.getpid(), signal.SIGKILL)


def _validate_worker_request(
    frame: dict[str, object],
    generation: int,
    projection_generation: int,
) -> None:
    if frame.get("kind") != "request":
        raise PlannerWorkerError("worker received a non-request frame")
    if (
        type(frame.get("worker_generation")) is not int
        or frame["worker_generation"] != generation
    ):
        raise PlannerWorkerError("worker generation mismatch")
    if type(frame.get("request_id")) is not int or int(frame["request_id"]) <= 0:
        raise PlannerWorkerError("worker request id is invalid")
    if (
        type(frame.get("projection_generation")) is not int
        or frame["projection_generation"] != projection_generation
    ):
        raise PlannerWorkerError("request does not match the worker-bound projection")
    if frame.get("operation") not in _ALLOWED_OPERATIONS:
        raise PlannerWorkerError("worker operation is invalid")
    if type(frame.get("work_ms")) is not int or int(frame["work_ms"]) < 0:
        raise PlannerWorkerError("worker work duration is invalid")
    if (
        type(frame.get("max_output_samples")) is not int
        or int(frame["max_output_samples"]) <= 0
    ):
        raise PlannerWorkerError("worker output-sample bound is invalid")


def _worker_result(
    request: dict[str, object],
    *,
    worker_generation: int,
    projection_generation: int,
    projection_digest: str,
    result_metadata: dict[str, object] | None = None,
) -> dict[str, object]:
    result: dict[str, object] = {
        "schema": _SCHEMA,
        "kind": "result",
        "worker_generation": worker_generation,
        "request_id": request["request_id"],
        "projection_generation": projection_generation,
        "projection_digest": projection_digest,
        "status": "ok",
        "candidate_digest": (
            f"candidate:{worker_generation}:{request['request_id']}:"
            f"{projection_generation}"
        ),
    }
    if result_metadata is not None:
        result["result"] = result_metadata
    return result


def _bootstrap_worker(policy: PlannerWorkerPolicy) -> _CandidateProjection | None:
    if policy.worker_bootstrap != "roboplan_collision":
        return None

    # Imports remain inside the candidate-only worker path so the ordinary
    # containment proof has no RoboPlan dependency. The worker owns the Scene
    # and its serialized mutable scratch for the complete process generation.
    import numpy as np  # type: ignore[import-not-found]
    from roboplan.core import (  # type: ignore[import-not-found]
        JointConfiguration,
        Scene,
    )
    from roboplan.rrt import RRT, RRTOptions  # type: ignore[import-not-found]

    urdf, srdf, package_share_roots = _candidate_projection_paths()
    scene = Scene(
        f"e1_worker_generation_{os.getpid()}",
        urdf,
        srdf,
        list(package_share_roots),
    )
    query = scene.getCurrentJointPositions()
    # Prove readiness only after one real native query has initialized the
    # planner/collision stack; replacement time therefore means usable time.
    scene.hasCollisions(query)

    options = RRTOptions()
    options.group_name = "arm"
    options.max_nodes = 1000
    options.max_connection_distance = 1.0
    options.collision_check_step_size = 0.05
    options.max_planning_time = 0.0
    options.rrt_connect = False
    options.rrt_star = False
    options.fast_return = True
    planner = RRT(scene, options)
    joint_names = [
        "shoulder_pan_joint",
        "shoulder_lift_joint",
        "elbow_joint",
        "wrist_1_joint",
        "wrist_2_joint",
        "wrist_3_joint",
    ]
    start = JointConfiguration(
        joint_names,
        np.array(
            [
                2.261477656358459,
                1.9570617913216783,
                -1.156210802275467,
                2.0229644353394325,
                -0.8713978455862086,
                -1.3594747944952112,
            ],
            dtype=np.float64,
        ),
    )
    goal = JointConfiguration(
        joint_names,
        np.array(
            [
                2.959756700843902,
                -1.6588986483183334,
                -1.2938684506809712,
                -0.4132159894019849,
                -2.76715775824764,
                0.7740437209968403,
            ],
            dtype=np.float64,
        ),
    )
    module_paths = {
        "core": pathlib.Path(sys.modules[Scene.__module__].__file__).resolve(),
        "rrt": pathlib.Path(sys.modules[RRT.__module__].__file__).resolve(),
    }
    return _CandidateProjection(
        scene=scene,
        collision_query=query,
        planner=planner,
        start=start,
        goal=goal,
        digest=_projection_resource_digest(urdf, srdf, package_share_roots),
        native_modules={
            name: {"path": str(path), "sha256": _sha256_file(path)}
            for name, path in module_paths.items()
        },
    )


def _send_sealed_trajectory(
    result_socket: socket.socket,
    request: dict[str, object],
    candidate_state: _CandidateProjection,
    policy: PlannerWorkerPolicy,
) -> dict[str, object]:
    import numpy as np  # type: ignore[import-not-found]

    candidate_state.planner.setRngSeed(1234)  # type: ignore[attr-defined]
    path = candidate_state.planner.plan(  # type: ignore[attr-defined]
        candidate_state.start,
        candidate_state.goal,
    )
    if path is None or len(path.positions) < 2:
        raise PlannerWorkerError("candidate RRT did not produce a path")
    positions = np.asarray(path.positions, dtype="<f8", order="C")
    if positions.ndim != 2:
        raise PlannerWorkerError("candidate RRT result is not a matrix")
    sample_count, dof = (int(value) for value in positions.shape)
    if (
        sample_count > int(request["max_output_samples"])
        or sample_count > policy.maximum_output_samples
        or dof <= 0
        or dof > policy.maximum_result_dof
        or not np.isfinite(positions).all()
    ):
        raise PlannerWorkerError(
            "candidate RRT result violates the bounded result policy"
        )
    payload = (
        _RESULT_HEADER.pack(_RESULT_MAGIC, sample_count, dof) + positions.tobytes()
    )
    if len(payload) > policy.maximum_result_bytes:
        raise PlannerWorkerError("candidate RRT result exceeds the byte bound")
    descriptor = os.memfd_create(
        "manyforge-e1-planner-result",
        os.MFD_CLOEXEC | os.MFD_ALLOW_SEALING,
    )
    try:
        _write_all(descriptor, payload)
        fcntl.fcntl(descriptor, fcntl.F_ADD_SEALS, _RESULT_SEALS)
        token = (
            f"{request['worker_generation']}:{request['request_id']}:"
            f"{request['projection_generation']}"
        )
        descriptors = array.array("i", [descriptor])
        sent = result_socket.sendmsg(
            [token.encode("ascii")],
            [(socket.SOL_SOCKET, socket.SCM_RIGHTS, descriptors)],
        )
        if sent != len(token):
            raise PlannerWorkerError("sealed result descriptor transfer was incomplete")
    finally:
        os.close(descriptor)
    return {
        "transport": "sealed_memfd_v1",
        "token": token,
        "byte_count": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
        "sample_count": sample_count,
        "dof": dof,
    }


def _run_worker(
    generation: int,
    projection_generation: int,
    expected_parent_pid: int,
    result_socket_fd: int,
    policy: PlannerWorkerPolicy,
) -> int:
    for name in _THREAD_ENVIRONMENT:
        os.environ[name] = "1"
    _arm_parent_death_signal(expected_parent_pid)
    _apply_worker_limits(policy)
    result_socket = socket.socket(fileno=result_socket_fd)
    candidate_state = _bootstrap_worker(policy)
    projection_digest = (
        candidate_state.digest if candidate_state else _SYNTHETIC_PROJECTION_DIGEST
    )
    if policy.worker_bootstrap == "synthetic_projection_drift" and generation > 1:
        projection_digest = f"sha256:{hashlib.sha256(b'projection-drift').hexdigest()}"
    _write_frame(
        sys.stdout.fileno(),
        {
            "schema": _SCHEMA,
            "kind": "ready",
            "worker_generation": generation,
            "pid": os.getpid(),
            "parent_pid": os.getppid(),
            "parent_death_signal": "SIGKILL",
            "bootstrap": policy.worker_bootstrap,
            "projection_generation": projection_generation,
            "projection_digest": projection_digest,
            "native_modules": candidate_state.native_modules if candidate_state else {},
            "runtime_identity": {
                "architecture": platform.machine(),
                "python": sys.version,
                "executable": sys.executable,
                "cpu_affinity": sorted(os.sched_getaffinity(0)),
            },
            "thread_policy": {
                name: os.environ.get(name) for name in _THREAD_ENVIRONMENT
            },
            "result_transport": "sealed_memfd_v1",
            "limits": _limit_snapshot(),
        },
    )
    while True:
        request = _read_worker_input()
        if request is None:
            return 0
        _validate_worker_request(request, generation, projection_generation)
        operation = str(request["operation"])
        descendant_pid: int | None = None
        if operation == "noncooperative_descendant":
            descendant_pid = os.fork()
            if descendant_pid == 0:
                _arm_parent_death_signal(os.getppid())
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
                while True:
                    signal.pause()
        _write_frame(
            sys.stdout.fileno(),
            {
                "schema": _SCHEMA,
                "kind": "active",
                "worker_generation": generation,
                "request_id": request["request_id"],
                "projection_generation": request["projection_generation"],
                "pid": os.getpid(),
                "descendant_pid": descendant_pid,
            },
        )
        if operation in {"noncooperative", "noncooperative_descendant"}:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            while True:
                signal.pause()
        if operation == "crash":
            os._exit(_EXIT_CRASH)
        if operation == "memory_exhaustion":
            try:
                allocation = bytearray(policy.worker_memory_bytes * 2)
            except MemoryError:
                os._exit(_EXIT_MEMORY_LIMIT)
            allocation[0] = 1
            os._exit(_EXIT_MEMORY_LIMIT_BREACH)
        if operation == "oversized_result":
            _write_all(sys.stdout.fileno(), b"x" * (_MAX_FRAME_BYTES + 1) + b"\n")
            continue
        if operation == "candidate_collision":
            if candidate_state is None:
                raise PlannerWorkerError(
                    "candidate collision requested without candidate bootstrap"
                )
            candidate_state.scene.hasCollisions(  # type: ignore[attr-defined]
                candidate_state.collision_query
            )
        result_metadata: dict[str, object] | None = None
        if operation == "candidate_rrt":
            if candidate_state is None:
                raise PlannerWorkerError(
                    "candidate RRT requested without candidate bootstrap"
                )
            result_metadata = _send_sealed_trajectory(
                result_socket,
                request,
                candidate_state,
                policy,
            )
        work_ms = int(request["work_ms"])
        if operation == "slow_success" and work_ms > 0:
            time.sleep(work_ms / 1000.0)
        result_generation = generation
        projection_generation = int(request["projection_generation"])
        result_projection_digest = projection_digest
        if operation == "stale_worker_generation":
            result_generation -= 1
        if operation == "stale_projection_generation":
            projection_generation -= 1
        if operation == "stale_projection_digest":
            result_projection_digest = f"sha256:{hashlib.sha256(b'stale').hexdigest()}"
        _write_frame(
            sys.stdout.fileno(),
            _worker_result(
                request,
                worker_generation=result_generation,
                projection_generation=projection_generation,
                projection_digest=result_projection_digest,
                result_metadata=result_metadata,
            ),
        )


def _validate_request(
    request: PlannerWorkRequest,
    policy: PlannerWorkerPolicy,
    *,
    now_ns: int,
) -> int:
    if (
        type(request.projection_generation) is not int
        or request.projection_generation <= 0
    ):
        raise PlannerRequestError(
            "projection_generation_invalid",
            "projection_generation must be a positive integer",
        )
    if type(request.deadline_monotonic_ns) is not int:
        raise PlannerRequestError("deadline_invalid", "deadline must be an integer")
    if request.deadline_monotonic_ns <= now_ns:
        raise PlannerRequestError(
            "deadline_expired", "deadline expired before dispatch"
        )
    if request.operation not in _ALLOWED_OPERATIONS:
        raise PlannerRequestError("operation_invalid", "operation is not admitted")
    finite_values = {
        "sample_period_s": request.sample_period_s,
        "requested_duration_s": request.requested_duration_s,
        "velocity_scale": request.velocity_scale,
        "acceleration_scale": request.acceleration_scale,
    }
    for name, value in finite_values.items():
        if type(value) not in {int, float} or not math.isfinite(float(value)):
            raise PlannerRequestError("nonfinite_input", f"{name} must be finite")
    if not (
        policy.minimum_sample_period_s
        <= request.sample_period_s
        <= policy.maximum_sample_period_s
    ):
        raise PlannerRequestError(
            "sample_period_out_of_bounds",
            "sample_period_s is outside the admitted finite range",
        )
    if not 0.0 <= request.requested_duration_s <= policy.maximum_duration_s:
        raise PlannerRequestError(
            "duration_out_of_bounds",
            "requested_duration_s is outside the admitted finite range",
        )
    for name, value in (
        ("velocity_scale", request.velocity_scale),
        ("acceleration_scale", request.acceleration_scale),
    ):
        if not 0.0 < value <= 1.0:
            raise PlannerRequestError(
                "scale_out_of_bounds", f"{name} must be in (0, 1]"
            )
    if (
        type(request.max_output_samples) is not int
        or request.max_output_samples <= 0
        or request.max_output_samples > policy.maximum_output_samples
    ):
        raise PlannerRequestError(
            "sample_budget_out_of_bounds",
            "max_output_samples exceeds the admitted policy",
        )
    if (
        type(request.waypoint_count) is not int
        or request.waypoint_count < 2
        or request.waypoint_count > policy.maximum_waypoints
    ):
        raise PlannerRequestError(
            "waypoint_count_out_of_bounds",
            "waypoint_count exceeds the admitted policy",
        )
    if (
        type(request.work_ms) is not int
        or request.work_ms < 0
        or request.work_ms > policy.maximum_work_ms
    ):
        raise PlannerRequestError(
            "work_bound_invalid", "work_ms exceeds the admitted policy"
        )
    ratio = request.requested_duration_s / request.sample_period_s
    if not math.isfinite(ratio):
        raise PlannerRequestError(
            "sample_count_not_representable",
            "duration/sample period is not finite",
        )
    sample_count = math.ceil(ratio) + 1
    if sample_count > request.max_output_samples:
        raise PlannerRequestError(
            "sample_budget_exceeded",
            "requested duration exceeds max_output_samples before dispatch",
        )
    return sample_count


def _valid_identity(frame: dict[str, object], active: _ActiveRequest) -> bool:
    return (
        type(frame.get("worker_generation")) is int
        and frame["worker_generation"] == active.worker_generation
        and type(frame.get("request_id")) is int
        and frame["request_id"] == active.request_id
        and type(frame.get("projection_generation")) is int
        and frame["projection_generation"] == active.projection_generation
    )


def _progress_is_monotonic(progress: Sequence[str]) -> bool:
    try:
        ranks = [_PROGRESS_RANK[item] for item in progress]
    except KeyError:
        return False
    return all(previous <= current for previous, current in zip(ranks, ranks[1:]))


class PlannerWorkerSupervisor:
    """One warmed worker with synchronous kill, fencing, and replacement."""

    def __init__(
        self,
        policy: PlannerWorkerPolicy | None = None,
        *,
        projection_generation: int = 1,
        expected_projection_digest: str | None = None,
    ) -> None:
        self.policy = policy or PlannerWorkerPolicy()
        self.policy.validate()
        if type(projection_generation) is not int or projection_generation <= 0:
            raise ValueError("bound projection generation must be a positive integer")
        if expected_projection_digest is None:
            if self.policy.worker_bootstrap not in {
                "synthetic",
                "synthetic_projection_drift",
            }:
                raise ValueError(
                    "candidate worker requires a caller-bound projection digest"
                )
            expected_projection_digest = _SYNTHETIC_PROJECTION_DIGEST
        if not _is_sha256_identity(expected_projection_digest):
            raise ValueError(
                "bound projection digest must be an exact SHA-256 identity"
            )
        self._projection_generation = projection_generation
        self._expected_projection_digest = expected_projection_digest
        self._state_lock = threading.RLock()
        self._active_condition = threading.Condition(self._state_lock)
        self._request_lock = threading.Lock()
        self._worker: _WorkerHandle | None = None
        self._active: _ActiveRequest | None = None
        self._next_generation = 0
        self._next_request_id = 0
        self._bound_native_modules: dict[str, dict[str, str]] | None = None
        self._bound_worker_runtime: dict[str, object] | None = None
        self._bound_thread_policy: dict[str, str] | None = None
        self._closed = False
        self.dispatch_count = 0
        self.replacement_count = 0
        initial_start = time.monotonic()
        with self._state_lock:
            self._worker = self._spawn_worker_locked(self.policy.worker_start_timeout_s)
        self.initial_start_ms = (time.monotonic() - initial_start) * 1000.0

    def __enter__(self) -> PlannerWorkerSupervisor:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    @property
    def worker_generation(self) -> int:
        with self._state_lock:
            if self._worker is None:
                raise PlannerWorkerError("worker is unavailable")
            return self._worker.generation

    @property
    def worker_pid(self) -> int:
        with self._state_lock:
            if self._worker is None:
                raise PlannerWorkerError("worker is unavailable")
            return self._worker.process.pid

    @property
    def projection_generation(self) -> int:
        return self._projection_generation

    @property
    def projection_digest(self) -> str:
        return self._expected_projection_digest

    @property
    def worker_native_modules(self) -> dict[str, dict[str, str]]:
        with self._state_lock:
            if self._worker is None:
                raise PlannerWorkerError("worker is unavailable")
            return _validated_native_modules(
                self._worker.ready.get("native_modules"),
                candidate_required=self.policy.worker_bootstrap == "roboplan_collision",
            )

    @property
    def worker_runtime_identity(self) -> dict[str, object]:
        with self._state_lock:
            if self._worker is None:
                raise PlannerWorkerError("worker is unavailable")
            return _validated_worker_runtime(self._worker.ready.get("runtime_identity"))

    @property
    def worker_thread_policy(self) -> dict[str, str]:
        with self._state_lock:
            if self._worker is None:
                raise PlannerWorkerError("worker is unavailable")
            return _validated_thread_policy(self._worker.ready.get("thread_policy"))

    @property
    def worker_limits(self) -> dict[str, int]:
        with self._state_lock:
            if self._worker is None:
                raise PlannerWorkerError("worker is unavailable")
            limits = self._worker.ready.get("limits")
            if type(limits) is not dict:
                raise PlannerWorkerError("worker limit attestation is unavailable")
            return {str(key): int(value) for key, value in limits.items()}

    @property
    def worker_memory(self) -> dict[str, int]:
        with self._state_lock:
            if self._worker is None:
                raise PlannerWorkerError("worker is unavailable")
            status_path = pathlib.Path(f"/proc/{self._worker.process.pid}/status")
            try:
                rows = status_path.read_text(encoding="utf-8").splitlines()
            except FileNotFoundError as exc:
                raise PlannerWorkerError("worker memory status is unavailable") from exc
            selected: dict[str, int] = {}
            names = {"VmRSS": "rss_bytes", "VmHWM": "peak_rss_bytes"}
            for row in rows:
                key, separator, remainder = row.partition(":")
                if separator and key in names:
                    fields = remainder.split()
                    if len(fields) != 2 or fields[1] != "kB":
                        raise PlannerWorkerError("worker memory status is invalid")
                    selected[names[key]] = int(fields[0]) * 1024
            if set(selected) != set(names.values()):
                raise PlannerWorkerError("worker memory status is incomplete")
            return selected

    def _spawn_worker_locked(self, timeout_s: float) -> _WorkerHandle:
        if self._closed:
            raise PlannerWorkerError("supervisor is closed")
        self._next_generation += 1
        generation = self._next_generation
        parent_result_socket, child_result_socket = socket.socketpair(
            socket.AF_UNIX,
            socket.SOCK_DGRAM | socket.SOCK_CLOEXEC,
        )
        module_path = pathlib.Path(__file__).resolve()
        command = [
            sys.executable,
            "-I",
            str(module_path),
            "--worker",
            "--generation",
            str(generation),
            "--projection-generation",
            str(self._projection_generation),
            "--expected-parent-pid",
            str(os.getpid()),
            "--result-socket-fd",
            str(child_result_socket.fileno()),
            "--memory-bytes",
            str(self.policy.worker_memory_bytes),
            "--file-bytes",
            str(self.policy.worker_file_bytes),
            "--cpu-seconds",
            str(self.policy.worker_cpu_seconds),
            "--max-output-samples",
            str(self.policy.maximum_output_samples),
            "--max-result-dof",
            str(self.policy.maximum_result_dof),
            "--max-result-bytes",
            str(self.policy.maximum_result_bytes),
            "--bootstrap",
            self.policy.worker_bootstrap,
        ]
        environment = dict(os.environ)
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        for name in _THREAD_ENVIRONMENT:
            environment[name] = "1"
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                # Keep bootstrap failures observable without introducing a
                # stderr pipe that an opaque planner could fill and block on.
                stderr=None,
                close_fds=True,
                pass_fds=(child_result_socket.fileno(),),
                start_new_session=True,
                env=environment,
            )
        except Exception:
            parent_result_socket.close()
            child_result_socket.close()
            raise
        child_result_socket.close()
        if process.stdin is None or process.stdout is None:
            process.kill()
            process.wait()
            parent_result_socket.close()
            raise PlannerWorkerError("worker pipes are unavailable")
        handle = _WorkerHandle(
            generation,
            process,
            bytearray(),
            {},
            parent_result_socket,
        )
        try:
            ready = self._read_frame(handle, time.monotonic() + timeout_s)
            if (
                ready.get("kind") != "ready"
                or ready.get("worker_generation") != generation
                or ready.get("pid") != process.pid
                or ready.get("parent_pid") != os.getpid()
                or ready.get("parent_death_signal") != "SIGKILL"
                or ready.get("bootstrap") != self.policy.worker_bootstrap
                or ready.get("projection_generation") != self._projection_generation
                or ready.get("projection_digest") != self._expected_projection_digest
                or ready.get("result_transport") != "sealed_memfd_v1"
            ):
                raise PlannerWorkerError("worker readiness identity mismatch")
            native_modules = _validated_native_modules(
                ready.get("native_modules"),
                candidate_required=self.policy.worker_bootstrap == "roboplan_collision",
            )
            runtime_identity = _validated_worker_runtime(ready.get("runtime_identity"))
            thread_policy = _validated_thread_policy(ready.get("thread_policy"))
            if self._bound_native_modules is None:
                self._bound_native_modules = native_modules
                self._bound_worker_runtime = runtime_identity
                self._bound_thread_policy = thread_policy
            elif (
                native_modules != self._bound_native_modules
                or runtime_identity != self._bound_worker_runtime
                or thread_policy != self._bound_thread_policy
            ):
                raise PlannerWorkerError(
                    "replacement worker build/runtime identity drift"
                )
            handle.ready = ready
            return handle
        except Exception:
            self._terminate_handle(handle)
            raise

    def _read_frame(self, handle: _WorkerHandle, deadline: float) -> dict[str, object]:
        descriptor = handle.process.stdout.fileno()  # type: ignore[union-attr]
        while True:
            newline = handle.read_buffer.find(b"\n")
            if newline >= 0:
                payload = bytes(handle.read_buffer[:newline])
                del handle.read_buffer[: newline + 1]
                if len(payload) + 1 > _MAX_FRAME_BYTES:
                    raise PlannerWorkerError(
                        "worker output frame exceeds the protocol bound"
                    )
                return _decode_frame(payload)
            if len(handle.read_buffer) >= _MAX_FRAME_BYTES:
                raise PlannerWorkerError(
                    "worker output frame exceeds the protocol bound"
                )
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise TimeoutError("worker response deadline expired")
            try:
                readable, _, _ = select.select([descriptor], [], [], remaining)
            except (OSError, ValueError) as exc:
                raise EOFError("worker output descriptor closed") from exc
            if not readable:
                raise TimeoutError("worker response deadline expired")
            try:
                chunk = os.read(descriptor, _MAX_FRAME_BYTES)
            except OSError as exc:
                raise EOFError("worker output read failed") from exc
            if not chunk:
                raise EOFError("worker output closed")
            handle.read_buffer.extend(chunk)

    def _send_frame(self, handle: _WorkerHandle, value: object) -> None:
        if handle.process.stdin is None:
            raise PlannerWorkerError("worker input is unavailable")
        _write_all(handle.process.stdin.fileno(), _bounded_json(value))

    def _receive_sealed_trajectory(
        self,
        handle: _WorkerHandle,
        metadata: dict[str, object],
        *,
        deadline: float,
        active: _ActiveRequest,
    ) -> tuple[tuple[float, ...], ...]:
        token = metadata.get("token")
        byte_count = metadata.get("byte_count")
        digest = metadata.get("sha256")
        sample_count = metadata.get("sample_count")
        dof = metadata.get("dof")
        if (
            metadata.get("transport") != "sealed_memfd_v1"
            or type(token) is not str
            or type(byte_count) is not int
            or type(digest) is not str
            or type(sample_count) is not int
            or type(dof) is not int
            or byte_count <= _RESULT_HEADER.size
            or byte_count > self.policy.maximum_result_bytes
            or sample_count <= 0
            or sample_count > active.max_output_samples
            or sample_count > self.policy.maximum_output_samples
            or dof <= 0
            or dof > self.policy.maximum_result_dof
        ):
            raise PlannerWorkerError("sealed result metadata is invalid")
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise PlannerWorkerError("sealed result descriptor deadline expired")
        readable, _, _ = select.select([handle.result_socket], [], [], remaining)
        if not readable:
            raise PlannerWorkerError("sealed result descriptor was not delivered")
        ancillary_size = socket.CMSG_SPACE(array.array("i").itemsize)
        payload, ancillary, flags, _ = handle.result_socket.recvmsg(
            256,
            ancillary_size,
            socket.MSG_CMSG_CLOEXEC,
        )
        received: list[int] = []
        for level, message_type, data in ancillary:
            if level == socket.SOL_SOCKET and message_type == socket.SCM_RIGHTS:
                descriptors = array.array("i")
                descriptors.frombytes(
                    data[: len(data) - (len(data) % descriptors.itemsize)]
                )
                received.extend(descriptors)
        if (
            flags & (socket.MSG_TRUNC | socket.MSG_CTRUNC)
            or payload.decode("ascii", errors="replace") != token
            or len(received) != 1
        ):
            for descriptor in received:
                os.close(descriptor)
            raise PlannerWorkerError("sealed result descriptor identity is invalid")
        descriptor = received[0]
        try:
            attributes = os.fstat(descriptor)
            if not stat.S_ISREG(attributes.st_mode) or attributes.st_size != byte_count:
                raise PlannerWorkerError("sealed result object size/type is invalid")
            seals = fcntl.fcntl(descriptor, fcntl.F_GET_SEALS)
            if seals & _RESULT_SEALS != _RESULT_SEALS:
                raise PlannerWorkerError("result object is not immutably sealed")
            chunks: list[bytes] = []
            offset = 0
            while offset < byte_count:
                chunk = os.pread(
                    descriptor, min(1024 * 1024, byte_count - offset), offset
                )
                if not chunk:
                    raise PlannerWorkerError("sealed result object ended early")
                chunks.append(chunk)
                offset += len(chunk)
            result_bytes = b"".join(chunks)
        finally:
            os.close(descriptor)
        if hashlib.sha256(result_bytes).hexdigest() != digest:
            raise PlannerWorkerError("sealed result digest mismatch")
        magic, encoded_samples, encoded_dof = _RESULT_HEADER.unpack_from(result_bytes)
        expected_size = _RESULT_HEADER.size + encoded_samples * encoded_dof * 8
        if (
            magic != _RESULT_MAGIC
            or encoded_samples != sample_count
            or encoded_dof != dof
            or expected_size != len(result_bytes)
        ):
            raise PlannerWorkerError("sealed result header/shape mismatch")
        values = array.array("d")
        values.frombytes(result_bytes[_RESULT_HEADER.size :])
        if sys.byteorder != "little":
            values.byteswap()
        if len(values) != sample_count * dof or not all(
            math.isfinite(v) for v in values
        ):
            raise PlannerWorkerError("sealed trajectory values are invalid")
        return tuple(
            tuple(values[row * dof : (row + 1) * dof]) for row in range(sample_count)
        )

    def _terminate_handle(self, handle: _WorkerHandle) -> tuple[float, int | None]:
        started = time.monotonic()
        process = handle.process
        returncode = process.poll()
        if returncode is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=self.policy.kill_grace_s)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=max(0.1, self.policy.replacement_timeout_s))
                except subprocess.TimeoutExpired as exc:
                    raise PlannerWorkerError("worker survived bounded SIGKILL") from exc
        else:
            # A crashed leader can leave descendants in its session/process group.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            process.wait(timeout=max(0.1, self.policy.replacement_timeout_s))
        except subprocess.TimeoutExpired as exc:
            raise PlannerWorkerError("worker could not be reaped") from exc
        returncode = process.returncode
        if process.stdin is not None:
            process.stdin.close()
        if process.stdout is not None:
            process.stdout.close()
        handle.result_socket.close()
        return (time.monotonic() - started) * 1000.0, returncode

    def _retire_and_replace_locked(
        self,
        active: _ActiveRequest,
    ) -> tuple[int, int, float, float, int | None]:
        handle = self._worker
        if handle is None or handle.generation != active.worker_generation:
            if self._worker is None:
                raise PlannerWorkerError("worker replacement state is inconsistent")
            return (
                self._worker.generation,
                self._worker.process.pid,
                0.0,
                0.0,
                None,
            )
        self._worker = None
        kill_ms, returncode = self._terminate_handle(handle)
        replacement_started = time.monotonic()
        replacement = self._spawn_worker_locked(self.policy.replacement_timeout_s)
        replacement_ms = (time.monotonic() - replacement_started) * 1000.0
        self._worker = replacement
        self.replacement_count += 1
        active.progress.extend(("worker_retired", "replacement_ready"))
        active.replacement_generation = replacement.generation
        active.replacement_pid = replacement.process.pid
        active.kill_ms = kill_ms
        active.replacement_ms = replacement_ms
        return (
            replacement.generation,
            replacement.process.pid,
            kill_ms,
            replacement_ms,
            returncode,
        )

    @staticmethod
    def _error_outcome(
        *,
        started: float,
        code: str,
        reason: str,
        request: PlannerWorkRequest,
        active: _ActiveRequest | None = None,
    ) -> PlannerWorkerOutcome:
        progress = tuple(active.progress if active is not None else ())
        if active is not None and (not progress or progress[-1] != "terminal"):
            active.progress.append("terminal")
            progress = tuple(active.progress)
        if active is not None and not _progress_is_monotonic(progress):
            raise PlannerWorkerError(f"non-monotonic worker progress: {progress}")
        return PlannerWorkerOutcome(
            ok=False,
            code=code,
            reason=reason,
            request_id=active.request_id if active is not None else None,
            projection_generation=request.projection_generation,
            worker_generation=active.worker_generation if active is not None else None,
            worker_pid=active.worker_pid if active is not None else None,
            replacement_generation=(
                active.replacement_generation if active is not None else None
            ),
            replacement_pid=active.replacement_pid if active is not None else None,
            progress=progress,
            wall_ms=(time.monotonic() - started) * 1000.0,
            kill_ms=active.kill_ms if active is not None else None,
            replacement_ms=active.replacement_ms if active is not None else None,
        )

    def execute(self, request: PlannerWorkRequest) -> PlannerWorkerOutcome:
        started = time.monotonic()
        try:
            _validate_request(request, self.policy, now_ns=time.monotonic_ns())
        except PlannerRequestError as exc:
            return self._error_outcome(
                started=started,
                code="invalid_request",
                reason=exc.reason,
                request=request,
            )
        if request.projection_generation != self._projection_generation:
            return self._error_outcome(
                started=started,
                code="invalid_request",
                reason="projection_generation_mismatch",
                request=request,
            )
        with self._request_lock:
            with self._state_lock:
                if self._closed or self._worker is None:
                    return self._error_outcome(
                        started=started,
                        code="planner_unavailable",
                        reason="worker_unavailable",
                        request=request,
                    )
                self._next_request_id += 1
                handle = self._worker
                active = _ActiveRequest(
                    request_id=self._next_request_id,
                    projection_generation=request.projection_generation,
                    worker_generation=handle.generation,
                    worker_pid=handle.process.pid,
                    max_output_samples=request.max_output_samples,
                    progress=["admitted", "dispatched"],
                )
                self._active = active
                try:
                    self._send_frame(
                        handle,
                        {
                            "schema": _SCHEMA,
                            "kind": "request",
                            "worker_generation": active.worker_generation,
                            "request_id": active.request_id,
                            "projection_generation": active.projection_generation,
                            "operation": request.operation,
                            "work_ms": request.work_ms,
                            "max_output_samples": request.max_output_samples,
                        },
                    )
                    self.dispatch_count += 1
                except (BrokenPipeError, OSError, PlannerWorkerError):
                    _, _, _, _, returncode = self._retire_and_replace_locked(active)
                    self._active = None
                    return self._error_outcome(
                        started=started,
                        code="planner_worker_crashed",
                        reason=f"dispatch_transport_failure:{returncode}",
                        request=request,
                        active=active,
                    )
            deadline = request.deadline_monotonic_ns / 1_000_000_000.0
            while True:
                try:
                    frame = self._read_frame(handle, deadline)
                except TimeoutError:
                    with self._state_lock:
                        if self._active is active and active.interrupt_reason is None:
                            active.interrupt_reason = "deadline"
                            active.progress.append("deadline_reached")
                            self._retire_and_replace_locked(active)
                            self._active = None
                    return self._error_outcome(
                        started=started,
                        code="planner_deadline_exceeded",
                        reason="deadline_killed_worker",
                        request=request,
                        active=active,
                    )
                except PlannerWorkerError:
                    with self._state_lock:
                        if active.interrupt_reason is not None:
                            if self._active is active:
                                self._active = None
                            code = (
                                "planner_cancelled"
                                if active.interrupt_reason == "cancel"
                                else "planner_deadline_exceeded"
                            )
                            return self._error_outcome(
                                started=started,
                                code=code,
                                reason=f"{active.interrupt_reason}_killed_worker",
                                request=request,
                                active=active,
                            )
                        if self._active is active:
                            self._retire_and_replace_locked(active)
                            self._active = None
                        return self._error_outcome(
                            started=started,
                            code="planner_worker_protocol_error",
                            reason="worker_output_frame_invalid",
                            request=request,
                            active=active,
                        )
                except EOFError as exc:
                    with self._state_lock:
                        if active.interrupt_reason is not None:
                            if self._active is active:
                                self._active = None
                            code = (
                                "planner_cancelled"
                                if active.interrupt_reason == "cancel"
                                else "planner_deadline_exceeded"
                            )
                            reason = f"{active.interrupt_reason}_killed_worker"
                            return self._error_outcome(
                                started=started,
                                code=code,
                                reason=reason,
                                request=request,
                                active=active,
                            )
                        returncode = handle.process.poll()
                        if self._active is active:
                            _, _, _, _, retired_returncode = (
                                self._retire_and_replace_locked(active)
                            )
                            if returncode is None:
                                returncode = retired_returncode
                            self._active = None
                        if returncode == _EXIT_MEMORY_LIMIT:
                            code = "planner_worker_resource_exhausted"
                            reason = "worker_address_space_limit"
                        else:
                            code = "planner_worker_crashed"
                            reason = (
                                f"worker_transport_closed:{returncode}:"
                                f"{type(exc).__name__}"
                            )
                        return self._error_outcome(
                            started=started,
                            code=code,
                            reason=reason,
                            request=request,
                            active=active,
                        )
                kind = frame.get("kind")
                if kind == "active":
                    with self._state_lock:
                        if (
                            self._active is not active
                            or active.interrupt_reason is not None
                        ):
                            continue
                        if not _valid_identity(frame, active):
                            self._retire_and_replace_locked(active)
                            self._active = None
                            return self._error_outcome(
                                started=started,
                                code="planner_worker_protocol_error",
                                reason="active_identity_mismatch",
                                request=request,
                                active=active,
                            )
                        descendant_pid = frame.get("descendant_pid")
                        if (
                            descendant_pid is not None
                            and type(descendant_pid) is not int
                        ):
                            self._retire_and_replace_locked(active)
                            self._active = None
                            return self._error_outcome(
                                started=started,
                                code="planner_worker_protocol_error",
                                reason="descendant_identity_invalid",
                                request=request,
                                active=active,
                            )
                        active.descendant_pid = descendant_pid
                        active.started = True
                        active.progress.append("running")
                        self._active_condition.notify_all()
                    continue
                if kind != "result":
                    with self._state_lock:
                        if self._active is active:
                            self._retire_and_replace_locked(active)
                            self._active = None
                    return self._error_outcome(
                        started=started,
                        code="planner_worker_protocol_error",
                        reason="unknown_worker_frame",
                        request=request,
                        active=active,
                    )
                with self._state_lock:
                    if active.interrupt_reason is not None:
                        if self._active is active:
                            self._active = None
                        return self._error_outcome(
                            started=started,
                            code=(
                                "planner_cancelled"
                                if active.interrupt_reason == "cancel"
                                else "planner_deadline_exceeded"
                            ),
                            reason=f"late_result_rejected_after_{active.interrupt_reason}",
                            request=request,
                            active=active,
                        )
                    if self._active is not active or self._worker is not handle:
                        return self._error_outcome(
                            started=started,
                            code="planner_worker_protocol_error",
                            reason="result_from_retired_worker",
                            request=request,
                            active=active,
                        )
                    if not _valid_identity(frame, active):
                        self._retire_and_replace_locked(active)
                        self._active = None
                        return self._error_outcome(
                            started=started,
                            code="planner_worker_protocol_error",
                            reason="result_generation_fence",
                            request=request,
                            active=active,
                        )
                    if (
                        frame.get("status") != "ok"
                        or type(frame.get("candidate_digest")) is not str
                        or frame.get("projection_digest")
                        != self._expected_projection_digest
                    ):
                        self._retire_and_replace_locked(active)
                        self._active = None
                        return self._error_outcome(
                            started=started,
                            code="planner_worker_protocol_error",
                            reason="result_shape_invalid",
                            request=request,
                            active=active,
                        )
                    result_metadata = frame.get("result")
                    trajectory_positions: tuple[tuple[float, ...], ...] | None = None
                    if request.operation == "candidate_rrt":
                        if type(result_metadata) is not dict:
                            self._retire_and_replace_locked(active)
                            self._active = None
                            return self._error_outcome(
                                started=started,
                                code="planner_worker_protocol_error",
                                reason="sealed_result_metadata_missing",
                                request=request,
                                active=active,
                            )
                        try:
                            trajectory_positions = self._receive_sealed_trajectory(
                                handle,
                                result_metadata,
                                deadline=deadline,
                                active=active,
                            )
                        except PlannerWorkerError:
                            self._retire_and_replace_locked(active)
                            self._active = None
                            return self._error_outcome(
                                started=started,
                                code="planner_worker_protocol_error",
                                reason="sealed_result_invalid",
                                request=request,
                                active=active,
                            )
                    elif result_metadata is not None:
                        self._retire_and_replace_locked(active)
                        self._active = None
                        return self._error_outcome(
                            started=started,
                            code="planner_worker_protocol_error",
                            reason="unexpected_result_transport",
                            request=request,
                            active=active,
                        )
                    self._active = None
                    active.progress.append("terminal")
                    progress = tuple(active.progress)
                    if not _progress_is_monotonic(progress):
                        raise PlannerWorkerError(
                            f"non-monotonic worker progress: {progress}"
                        )
                    return PlannerWorkerOutcome(
                        ok=True,
                        code="ok",
                        reason="candidate_ready",
                        request_id=active.request_id,
                        projection_generation=active.projection_generation,
                        worker_generation=active.worker_generation,
                        worker_pid=active.worker_pid,
                        replacement_generation=None,
                        replacement_pid=None,
                        progress=progress,
                        wall_ms=(time.monotonic() - started) * 1000.0,
                        projection_digest=str(frame["projection_digest"]),
                        candidate_digest=str(frame["candidate_digest"]),
                        result_transport=(
                            str(result_metadata["transport"])
                            if type(result_metadata) is dict
                            else None
                        ),
                        trajectory_positions=trajectory_positions,
                    )

    def wait_until_active(self, timeout_s: float) -> ActiveWorkerObservation | None:
        deadline = time.monotonic() + timeout_s
        with self._active_condition:
            while True:
                active = self._active
                if (
                    active is not None
                    and active.started
                    and active.interrupt_reason is None
                ):
                    return ActiveWorkerObservation(
                        active.request_id,
                        active.worker_generation,
                        active.worker_pid,
                        active.descendant_pid,
                    )
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    return None
                self._active_condition.wait(remaining)

    def cancel_active(self) -> bool:
        with self._state_lock:
            active = self._active
            if active is None or active.interrupt_reason is not None:
                return False
            active.interrupt_reason = "cancel"
            active.progress.append("cancel_requested")
            self._retire_and_replace_locked(active)
            self._active_condition.notify_all()
            return True

    def close(self) -> None:
        with self._state_lock:
            if self._closed:
                return
            self._closed = True
            handle = self._worker
            self._worker = None
            self._active = None
            if handle is not None:
                self._terminate_handle(handle)


def _request(
    *,
    operation: str = "success",
    deadline_ms: int = 1000,
    projection_generation: int = 1,
    work_ms: int = 0,
) -> PlannerWorkRequest:
    return PlannerWorkRequest(
        projection_generation=projection_generation,
        deadline_monotonic_ns=time.monotonic_ns() + deadline_ms * 1_000_000,
        operation=operation,
        work_ms=work_ms,
    )


def _percentiles(values: Sequence[float]) -> dict[str, float]:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("cannot summarize empty latency input")

    def percentile(fraction: float) -> float:
        index = min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)
        return ordered[index]

    return {
        "p50": percentile(0.50),
        "p95": percentile(0.95),
        "p99": percentile(0.99),
        "mean": statistics.fmean(ordered),
        "max": ordered[-1],
    }


def _compact_outcome(outcome: PlannerWorkerOutcome) -> dict[str, object]:
    record = dataclasses.asdict(outcome)
    positions = record.pop("trajectory_positions")
    record["trajectory_shape"] = (
        {
            "samples": len(positions),
            "dof": len(positions[0]),
        }
        if positions
        else None
    )
    return record


def run_probe(iterations: int = 100) -> dict[str, object]:
    if type(iterations) is not int or not 10 <= iterations <= 10_000:
        raise ValueError("iterations must be in [10, 10000]")
    with PlannerWorkerSupervisor() as supervisor:
        first_pid = supervisor.worker_pid
        first_generation = supervisor.worker_generation
        initial_memory = supervisor.worker_memory
        first_result = supervisor.execute(_request(deadline_ms=2000))
        if not first_result.ok:
            raise PlannerWorkerError(
                f"first warmed-worker request failed: {first_result}"
            )
        latencies: list[float] = []
        for _ in range(iterations):
            result = supervisor.execute(_request(deadline_ms=2000))
            if not result.ok:
                raise PlannerWorkerError(f"warmed success failed: {result}")
            if (
                result.worker_pid != first_pid
                or result.worker_generation != first_generation
            ):
                raise PlannerWorkerError("normal path did not reuse the warmed worker")
            latencies.append(result.wall_ms)
        warmed_memory = supervisor.worker_memory

        cancel_holder: dict[str, PlannerWorkerOutcome] = {}
        cancel_thread = threading.Thread(
            target=lambda: cancel_holder.setdefault(
                "result",
                supervisor.execute(
                    _request(operation="noncooperative", deadline_ms=2000)
                ),
            ),
            daemon=True,
        )
        cancel_thread.start()
        if supervisor.wait_until_active(1.0) is None or not supervisor.cancel_active():
            raise PlannerWorkerError("cancel probe never reached active work")
        cancel_thread.join(2.0)
        if cancel_thread.is_alive():
            raise PlannerWorkerError("cancel probe did not complete")
        cancel_result = cancel_holder["result"]

        deadline_result = supervisor.execute(
            _request(operation="noncooperative", deadline_ms=80)
        )
        stale_result = supervisor.execute(
            _request(operation="stale_worker_generation", deadline_ms=1000)
        )
        memory_result = supervisor.execute(
            _request(operation="memory_exhaustion", deadline_ms=1000)
        )
        recovery_result = supervisor.execute(_request(deadline_ms=1000))
        expected = (
            (cancel_result.code, "planner_cancelled"),
            (deadline_result.code, "planner_deadline_exceeded"),
            (stale_result.code, "planner_worker_protocol_error"),
            (memory_result.code, "planner_worker_resource_exhausted"),
        )
        if (
            any(actual != wanted for actual, wanted in expected)
            or not recovery_result.ok
        ):
            raise PlannerWorkerError("containment probe outcome mismatch")
        return {
            "schema": "manyforge.e1.worker_containment_result.v1",
            "iterations": iterations,
            "warmed_latency_ms": _percentiles(latencies),
            "initial_worker": {
                "pid": first_pid,
                "generation": first_generation,
                "start_ms": supervisor.initial_start_ms,
                "first_request_ms": first_result.wall_ms,
                "memory_before_requests": initial_memory,
                "memory_after_warmup": warmed_memory,
            },
            "final_worker": {
                "pid": supervisor.worker_pid,
                "generation": supervisor.worker_generation,
                "memory": supervisor.worker_memory,
            },
            "worker_limits": supervisor.worker_limits,
            "bound_projection": {
                "generation": supervisor.projection_generation,
                "digest": supervisor.projection_digest,
            },
            "dispatch_count": supervisor.dispatch_count,
            "replacement_count": supervisor.replacement_count,
            "cancel": dataclasses.asdict(cancel_result),
            "deadline": dataclasses.asdict(deadline_result),
            "stale_result": dataclasses.asdict(stale_result),
            "memory_limit": dataclasses.asdict(memory_result),
            "post_replacement_success": _compact_outcome(recovery_result),
        }


def run_candidate_probe(
    iterations: int = 100,
    *,
    policy: PlannerWorkerPolicy | None = None,
    evidence_identity: dict[str, str] | None = None,
) -> dict[str, object]:
    """Measure the same warmed/retirement path with a real RoboPlan query."""

    if type(iterations) is not int or not 10 <= iterations <= 10_000:
        raise ValueError("iterations must be in [10, 10000]")
    selected_policy = policy or PlannerWorkerPolicy(
        worker_memory_bytes=1024 * 1024 * 1024,
        worker_cpu_seconds=30,
        worker_start_timeout_s=5.0,
        replacement_timeout_s=5.0,
        worker_bootstrap="roboplan_collision",
    )
    if selected_policy.worker_bootstrap != "roboplan_collision":
        raise ValueError("candidate probe requires the RoboPlan bootstrap")
    if evidence_identity is not None:
        required_identity = {
            "label",
            "source_commit",
            "source_tree",
            "patch_set_sha256",
            "dependency_closure_sha256",
            "build_identity_sha256",
            "build_type",
            "compiler_id",
        }
        if set(evidence_identity) != required_identity or not all(
            evidence_identity.values()
        ):
            raise ValueError("candidate probe evidence identity is incomplete")

    urdf, srdf, package_share_roots = _candidate_projection_paths()
    expected_projection_digest = _projection_resource_digest(
        urdf,
        srdf,
        package_share_roots,
    )
    with PlannerWorkerSupervisor(
        selected_policy,
        projection_generation=1000,
        expected_projection_digest=expected_projection_digest,
    ) as supervisor:
        first_identity = (supervisor.worker_generation, supervisor.worker_pid)
        memory_before = supervisor.worker_memory
        first = supervisor.execute(
            _request(
                operation="candidate_rrt",
                projection_generation=1000,
                deadline_ms=5000,
            )
        )
        if not first.ok:
            raise PlannerWorkerError(f"first candidate request failed: {first}")
        if (
            first.result_transport != "sealed_memfd_v1"
            or first.trajectory_positions is None
            or len(first.trajectory_positions) < 2
            or len(first.trajectory_positions[0]) != 6
        ):
            raise PlannerWorkerError(
                "first candidate trajectory transfer is incomplete"
            )
        expected_start = (
            2.261477656358459,
            1.9570617913216783,
            -1.156210802275467,
            2.0229644353394325,
            -0.8713978455862086,
            -1.3594747944952112,
        )
        expected_goal = (
            2.959756700843902,
            -1.6588986483183334,
            -1.2938684506809712,
            -0.4132159894019849,
            -2.76715775824764,
            0.7740437209968403,
        )
        if any(
            not math.isclose(actual, expected, abs_tol=1e-12)
            for actual, expected in zip(first.trajectory_positions[0], expected_start)
        ) or any(
            not math.isclose(actual, expected, abs_tol=1e-12)
            for actual, expected in zip(first.trajectory_positions[-1], expected_goal)
        ):
            raise PlannerWorkerError(
                "candidate trajectory endpoints changed in transfer"
            )

        latencies: list[float] = []
        for _ in range(iterations):
            result = supervisor.execute(
                _request(
                    operation="candidate_rrt",
                    projection_generation=1000,
                    deadline_ms=5000,
                )
            )
            if not result.ok:
                raise PlannerWorkerError(f"warmed candidate request failed: {result}")
            if (result.worker_generation, result.worker_pid) != first_identity:
                raise PlannerWorkerError(
                    "candidate normal path replaced its warmed worker"
                )
            if result.result_transport != "sealed_memfd_v1":
                raise PlannerWorkerError("candidate result bypassed sealed transfer")
            latencies.append(result.wall_ms)
        memory_after = supervisor.worker_memory

        cancel_holder: dict[str, PlannerWorkerOutcome] = {}
        cancel_thread = threading.Thread(
            target=lambda: cancel_holder.setdefault(
                "result",
                supervisor.execute(
                    _request(
                        operation="noncooperative",
                        projection_generation=1000,
                        deadline_ms=5000,
                    )
                ),
            ),
            daemon=True,
        )
        cancel_thread.start()
        if supervisor.wait_until_active(2.0) is None or not supervisor.cancel_active():
            raise PlannerWorkerError("candidate cancel probe never became active")
        cancel_thread.join(10.0)
        if cancel_thread.is_alive():
            raise PlannerWorkerError("candidate cancel probe did not complete")
        cancel_result = cancel_holder["result"]
        post_cancel = supervisor.execute(
            _request(
                operation="candidate_rrt",
                projection_generation=1000,
                deadline_ms=5000,
            )
        )

        deadline_result = supervisor.execute(
            _request(
                operation="noncooperative",
                projection_generation=1000,
                deadline_ms=80,
            )
        )
        post_deadline = supervisor.execute(
            _request(
                operation="candidate_rrt",
                projection_generation=1000,
                deadline_ms=5000,
            )
        )
        if (
            cancel_result.code != "planner_cancelled"
            or deadline_result.code != "planner_deadline_exceeded"
            or not post_cancel.ok
            or not post_deadline.ok
        ):
            raise PlannerWorkerError("candidate containment outcome mismatch")

        return {
            "schema": "manyforge.e1.candidate_worker_result.v1",
            "evidence_identity": evidence_identity,
            "iterations": iterations,
            "warmed_latency_ms": _percentiles(latencies),
            "initial_worker": {
                "pid": first_identity[1],
                "generation": first_identity[0],
                "start_and_native_warm_ms": supervisor.initial_start_ms,
                "first_request_ms": first.wall_ms,
                "memory_before_requests": memory_before,
                "memory_after_requests": memory_after,
            },
            "final_worker": {
                "pid": supervisor.worker_pid,
                "generation": supervisor.worker_generation,
                "memory": supervisor.worker_memory,
            },
            "worker_limits": supervisor.worker_limits,
            "worker_native_modules": supervisor.worker_native_modules,
            "worker_runtime_identity": supervisor.worker_runtime_identity,
            "worker_thread_policy": supervisor.worker_thread_policy,
            "bound_projection": {
                "generation": supervisor.projection_generation,
                "digest": supervisor.projection_digest,
            },
            "representative_result": {
                "transport": first.result_transport,
                "candidate_digest": first.candidate_digest,
                "samples": len(first.trajectory_positions),
                "dof": len(first.trajectory_positions[0]),
                "start": first.trajectory_positions[0],
                "goal": first.trajectory_positions[-1],
            },
            "dispatch_count": supervisor.dispatch_count,
            "replacement_count": supervisor.replacement_count,
            "cancel": dataclasses.asdict(cancel_result),
            "post_cancel_success": _compact_outcome(post_cancel),
            "deadline": dataclasses.asdict(deadline_result),
            "post_deadline_success": _compact_outcome(post_deadline),
        }


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", action="store_true")
    parser.add_argument("--probe", action="store_true")
    parser.add_argument("--candidate-probe", action="store_true")
    parser.add_argument("--generation", type=int)
    parser.add_argument("--projection-generation", type=int)
    parser.add_argument("--expected-parent-pid", type=int)
    parser.add_argument("--result-socket-fd", type=int)
    parser.add_argument("--memory-bytes", type=int, default=256 * 1024 * 1024)
    parser.add_argument("--file-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--cpu-seconds", type=int, default=5)
    parser.add_argument("--max-output-samples", type=int, default=65_536)
    parser.add_argument("--max-result-dof", type=int, default=64)
    parser.add_argument("--max-result-bytes", type=int, default=64 * 1024 * 1024)
    parser.add_argument("--bootstrap", default="synthetic")
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--label")
    parser.add_argument("--source-commit")
    parser.add_argument("--source-tree")
    parser.add_argument("--patch-set-sha256")
    parser.add_argument("--dependency-closure-sha256")
    parser.add_argument("--build-identity-sha256")
    parser.add_argument("--build-type")
    parser.add_argument("--compiler-id")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    if sum((args.worker, args.probe, args.candidate_probe)) != 1:
        raise SystemExit("select exactly one worker or probe mode")
    if args.worker:
        worker_identity_values = (
            args.generation,
            args.projection_generation,
            args.expected_parent_pid,
            args.result_socket_fd,
        )
        if any(value is None or value <= 0 for value in worker_identity_values):
            raise SystemExit("worker identity/fd arguments must be positive")
        policy = PlannerWorkerPolicy(
            maximum_output_samples=args.max_output_samples,
            maximum_result_dof=args.max_result_dof,
            maximum_result_bytes=args.max_result_bytes,
            worker_memory_bytes=args.memory_bytes,
            worker_file_bytes=args.file_bytes,
            worker_cpu_seconds=args.cpu_seconds,
            worker_bootstrap=args.bootstrap,
        )
        policy.validate()
        return _run_worker(
            args.generation,
            args.projection_generation,
            args.expected_parent_pid,
            args.result_socket_fd,
            policy,
        )
    if args.candidate_probe:
        identity_fields = (
            "label",
            "source_commit",
            "source_tree",
            "patch_set_sha256",
            "dependency_closure_sha256",
            "build_identity_sha256",
            "build_type",
            "compiler_id",
        )
        if any(getattr(args, field) is None for field in identity_fields):
            raise SystemExit("candidate probe requires complete source/build identity")
        evidence_identity = {
            field: str(getattr(args, field)) for field in identity_fields
        }
        policy = PlannerWorkerPolicy(
            worker_memory_bytes=args.memory_bytes,
            worker_file_bytes=args.file_bytes,
            worker_cpu_seconds=args.cpu_seconds,
            worker_start_timeout_s=5.0,
            replacement_timeout_s=5.0,
            worker_bootstrap="roboplan_collision",
        )
        result = run_candidate_probe(
            args.iterations,
            policy=policy,
            evidence_identity=evidence_identity,
        )
    else:
        result = run_probe(args.iterations)
    json.dump(
        result, sys.stdout, sort_keys=True, separators=(",", ":"), allow_nan=False
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
