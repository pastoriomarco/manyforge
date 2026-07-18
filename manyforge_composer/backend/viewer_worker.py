"""Supervised non-real-time worker for visual FK and frame production."""
from __future__ import annotations

import hashlib
import json
import logging
import multiprocessing as mp
import os
import queue
import threading
import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Final

from .models import (
    PoseSourceStatusResponse,
    SceneGripperStateResponse,
    SceneObjectResponse,
    SceneRobotResponse,
    VisualSceneSnapshotResponse,
)
from .viewer_data_plane import (
    ViewerProtocolEncoder,
    ViewerPublication,
    ViewerSnapshotBuild,
    build_viewer_protocol_payloads,
    freeze_viewer_signature,
)
from .visual_state import (
    VisualFkModelDescriptor,
    VisualSnapshotUnavailable,
    assemble_robot_visual_snapshot,
    compose_pose_in_world,
    compute_robot_visual_link_poses_from_descriptor,
    relative_pose_from_world,
)

logger = logging.getLogger(__name__)

_REQUEST_SCHEMA: Final = "manyforge.viewer.worker.request.v1"
_RESPONSE_SCHEMA: Final = "manyforge.viewer.worker.response.v1"
_SHUTDOWN_SCHEMA: Final = "manyforge.viewer.worker.shutdown.v1"
_DEFAULT_MAX_INPUT_BYTES: Final = 8 * 1024 * 1024
_DEFAULT_MAX_OUTPUT_BYTES: Final = 16 * 1024 * 1024


class ViewerWorkerError(RuntimeError):
    """A bounded visual-worker request failed without affecting control."""

    def __init__(self, reason: str, detail: str = "") -> None:
        super().__init__(detail or reason)
        self.reason = reason
        self.detail = detail or reason


def _canonical_json_bytes(value: Any) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
        allow_nan=False,
    ).encode("utf-8")


def _encode_envelope(
    *,
    schema: str,
    request_id: int,
    payload: dict[str, Any],
    max_bytes: int,
) -> dict[str, Any]:
    encoded = _canonical_json_bytes(payload)
    if len(encoded) > max_bytes:
        raise ViewerWorkerError(
            "viewer_worker_payload_too_large",
            f"{schema} payload is {len(encoded)} bytes; limit is {max_bytes}",
        )
    return {
        "schema": schema,
        "request_id": int(request_id),
        "payload_length": len(encoded),
        "payload_sha256": hashlib.sha256(encoded).hexdigest(),
        "payload": encoded,
    }


def _decode_envelope(
    envelope: Any,
    *,
    expected_schema: str,
    max_bytes: int,
) -> tuple[int, dict[str, Any]]:
    if not isinstance(envelope, dict) or envelope.get("schema") != expected_schema:
        raise ViewerWorkerError("viewer_worker_protocol_error", "unexpected envelope schema")
    request_id = envelope.get("request_id")
    payload = envelope.get("payload")
    payload_length = envelope.get("payload_length")
    digest = envelope.get("payload_sha256")
    if not isinstance(request_id, int) or request_id < 0:
        raise ViewerWorkerError("viewer_worker_protocol_error", "invalid request id")
    if not isinstance(payload, bytes):
        raise ViewerWorkerError("viewer_worker_protocol_error", "payload is not bytes")
    if len(payload) > max_bytes or payload_length != len(payload):
        raise ViewerWorkerError("viewer_worker_protocol_error", "invalid payload length")
    if not isinstance(digest, str) or hashlib.sha256(payload).hexdigest() != digest:
        raise ViewerWorkerError("viewer_worker_protocol_error", "payload checksum mismatch")
    try:
        decoded = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ViewerWorkerError("viewer_worker_protocol_error", str(exc)) from exc
    if not isinstance(decoded, dict):
        raise ViewerWorkerError("viewer_worker_protocol_error", "payload root is not an object")
    return request_id, decoded


def _parse_cpu_set(raw: str) -> set[int]:
    cpus: set[int] = set()
    for token in raw.split(","):
        part = token.strip()
        if not part:
            continue
        if "-" in part:
            first, last = part.split("-", 1)
            start = int(first)
            end = int(last)
            if start < 0 or end < start:
                raise ValueError(part)
            cpus.update(range(start, end + 1))
        else:
            value = int(part)
            if value < 0:
                raise ValueError(part)
            cpus.add(value)
    return cpus


def _apply_nrt_process_policy() -> dict[str, Any]:
    """Best-effort NRT policy; never promotes the visual worker to RT."""
    scheduler_error = ""
    try:
        if hasattr(os, "sched_setscheduler"):
            os.sched_setscheduler(0, os.SCHED_OTHER, os.sched_param(0))
    except Exception as exc:
        scheduler_error = str(exc)
    requested_affinity = os.getenv("MANYFORGE_NRT_CPU_AFFINITY", "").strip()
    affinity_error = ""
    requested_cpus: list[int] = []
    if requested_affinity and hasattr(os, "sched_setaffinity"):
        try:
            cpus = _parse_cpu_set(requested_affinity)
            if cpus:
                os.sched_setaffinity(0, cpus)
                requested_cpus = sorted(cpus)
        except Exception as exc:  # deployment evidence reports the failure
            affinity_error = str(exc)
    try:
        requested_nice = max(
            0,
            min(19, int(os.getenv("MANYFORGE_VIEWER_WORKER_NICE", "5"))),
        )
    except ValueError:
        requested_nice = 5
    nice_error = ""
    try:
        current_nice = os.getpriority(os.PRIO_PROCESS, 0)
        if current_nice < requested_nice:
            os.setpriority(os.PRIO_PROCESS, 0, requested_nice)
            current_nice = os.getpriority(os.PRIO_PROCESS, 0)
    except Exception as exc:
        current_nice = os.nice(0)
        nice_error = str(exc)
    requested_cgroup = os.getenv("MANYFORGE_VIEWER_WORKER_CGROUP", "").strip()
    cgroup_error = ""
    if requested_cgroup:
        try:
            cgroup_path = os.path.realpath(requested_cgroup)
            cgroup_root = os.path.realpath("/sys/fs/cgroup")
            if not cgroup_path.startswith(cgroup_root + os.sep):
                raise ValueError("viewer worker cgroup must be below /sys/fs/cgroup")
            with open(
                os.path.join(cgroup_path, "cgroup.procs"),
                "w",
                encoding="ascii",
            ) as handle:
                handle.write(str(os.getpid()))
        except Exception as exc:
            cgroup_error = str(exc)
    actual_affinity = (
        sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else []
    )
    actual_scheduler_value = (
        os.sched_getscheduler(0) if hasattr(os, "sched_getscheduler") else None
    )
    scheduler_names = {
        getattr(os, name): name
        for name in (
            "SCHED_OTHER",
            "SCHED_FIFO",
            "SCHED_RR",
            "SCHED_BATCH",
            "SCHED_IDLE",
            "SCHED_DEADLINE",
        )
        if hasattr(os, name)
    }
    actual_scheduler = scheduler_names.get(
        actual_scheduler_value,
        str(actual_scheduler_value),
    )
    scheduler_ok = (
        actual_scheduler_value is None
        or actual_scheduler_value == getattr(os, "SCHED_OTHER", actual_scheduler_value)
    )
    nice_ok = int(current_nice) >= requested_nice
    affinity_ok = (
        not requested_affinity
        or (not affinity_error and actual_affinity == requested_cpus)
    )
    cgroup_ok = not requested_cgroup or not cgroup_error
    return {
        "pid": os.getpid(),
        "scheduler": actual_scheduler,
        "scheduler_error": scheduler_error,
        "nice": current_nice,
        "requested_nice": requested_nice,
        "cpu_affinity": actual_affinity,
        "requested_cpu_affinity": requested_affinity,
        "affinity_error": affinity_error,
        "nice_error": nice_error,
        "requested_cgroup": requested_cgroup,
        "cgroup_error": cgroup_error,
        "scheduler_ok": scheduler_ok,
        "nice_ok": nice_ok,
        "affinity_ok": affinity_ok,
        "cgroup_ok": cgroup_ok,
        "policy_ok": (
            scheduler_ok
            and nice_ok
            and affinity_ok
            and cgroup_ok
            and not scheduler_error
            and not nice_error
        ),
    }


def _source_requests_measured_robot(source: PoseSourceStatusResponse) -> bool:
    for observed in source.objects:
        config = observed.measured_state_ghost
        if not isinstance(config, dict):
            continue
        if config.get("enabled") is not True:
            continue
        if config.get("robotTcp", True) is not False:
            return True
    return False


@dataclass
class _WorkerBuildState:
    epoch: str = field(default_factory=lambda: uuid.uuid4().hex)
    encoder: ViewerProtocolEncoder = field(init=False)
    sequence: int = 0
    canonical_signature: Any = None
    canonical_link_poses: dict[str, dict[str, Any]] | None = None
    measured_cache: dict[str, tuple[Any, dict[str, dict[str, Any]]]] = field(
        default_factory=dict
    )

    def __post_init__(self) -> None:
        self.encoder = ViewerProtocolEncoder(producer_epoch=self.epoch)


def _build_worker_payload(
    payload: dict[str, Any],
    *,
    state: _WorkerBuildState,
) -> dict[str, Any]:
    started = time.monotonic()
    descriptor = VisualFkModelDescriptor(**payload["model_descriptor"])
    robot = SceneRobotResponse.model_validate(payload["robot"])
    grippers = [
        SceneGripperStateResponse.model_validate(item)
        for item in payload.get("grippers", [])
    ]
    pose_sources = [
        PoseSourceStatusResponse.model_validate(item)
        for item in payload.get("pose_sources", [])
    ]
    joint_names = [str(item) for item in payload["visual_joint_names"]]
    joint_positions = [float(item) for item in payload["visual_joint_positions"]]
    link_names = [str(item) for item in payload["link_names"]]
    world_frame = str(payload["world_frame"])
    base_frame = str(payload["base_frame"])
    base_pose = dict(payload.get("base_pose", {}))

    fk_started = time.monotonic()
    canonical_signature = freeze_viewer_signature(
        (
            descriptor.model_key,
            joint_names,
            joint_positions,
            link_names,
            world_frame,
            base_frame,
            base_pose,
        )
    )
    canonical_reused = (
        state.canonical_link_poses is not None
        and state.canonical_signature == canonical_signature
    )
    if canonical_reused:
        link_poses = state.canonical_link_poses
    else:
        link_poses = compute_robot_visual_link_poses_from_descriptor(
            descriptor,
            joint_names=joint_names,
            joint_positions_rad=joint_positions,
            link_names=link_names,
            reference_frame=world_frame,
            base_frame=base_frame,
            base_pose=base_pose,
        )
        state.canonical_signature = canonical_signature
        state.canonical_link_poses = link_poses
    canonical_fk_ms = (time.monotonic() - fk_started) * 1000.0

    if base_frame in link_poses and robot.tcp_frame in link_poses:
        robot.tcp_pose_in_base = relative_pose_from_world(
            child_pose=link_poses[robot.tcp_frame],
            parent_pose=link_poses[base_frame],
        )

    measured_started = time.monotonic()
    measured_reused = 0
    measured_enabled = "measured_robot" in set(payload.get("demanded_layers", []))
    active_measured_sources: set[str] = set()
    for source in pose_sources:
        measured = source.measured_robot
        if measured is None:
            continue
        measured.link_poses = {}
        if not measured_enabled or not _source_requests_measured_robot(source):
            continue
        if not measured.joint_names or len(measured.joint_names) != len(
            measured.joint_positions_rad
        ):
            continue
        active_measured_sources.add(source.pose_source_id)
        signature = freeze_viewer_signature(
            (
                descriptor.model_key,
                measured.joint_names,
                measured.joint_positions_rad,
                link_names,
                world_frame,
                base_frame,
                base_pose,
            )
        )
        cached = state.measured_cache.get(source.pose_source_id)
        if cached is not None and cached[0] == signature:
            measured.link_poses = cached[1]
            measured_reused += 1
            continue
        try:
            measured_link_poses = compute_robot_visual_link_poses_from_descriptor(
                descriptor,
                joint_names=list(measured.joint_names),
                joint_positions_rad=list(measured.joint_positions_rad),
                link_names=link_names,
                reference_frame=world_frame,
                base_frame=base_frame,
                base_pose=base_pose,
            )
        except VisualSnapshotUnavailable:
            continue
        measured.link_poses = measured_link_poses
        state.measured_cache[source.pose_source_id] = (signature, measured_link_poses)
    for source_id in list(state.measured_cache):
        if source_id not in active_measured_sources:
            state.measured_cache.pop(source_id, None)
    measured_fk_ms = (time.monotonic() - measured_started) * 1000.0

    shape_started = time.monotonic()
    objects: list[SceneObjectResponse] = []
    derived_offsets: dict[str, dict[str, Any]] = {}
    for row in payload.get("objects", []):
        item = SceneObjectResponse.model_validate(row["response"])
        frame_id = str(row.get("frame_id") or "")
        offset = row.get("attachment_offset")
        if item.attached and frame_id:
            frame_pose = link_poses.get(frame_id)
            if offset is None and frame_pose is not None and item.pose_in_universe is not None:
                offset = relative_pose_from_world(
                    child_pose=item.pose_in_universe,
                    parent_pose=frame_pose,
                )
                derived_offsets[item.object_id] = offset
            if frame_pose is not None and isinstance(offset, dict):
                item = item.model_copy(
                    update={
                        "pose_in_universe": compose_pose_in_world(
                            parent_pose=frame_pose,
                            child_pose=offset,
                        )
                    }
                )
        objects.append(item)
    shape_ms = (time.monotonic() - shape_started) * 1000.0

    state.sequence += 1
    snapshot = assemble_robot_visual_snapshot(
        model_key=descriptor.model_key,
        robot=robot,
        reference_frame=world_frame,
        link_poses=link_poses,
        grippers=grippers,
        pose_sources=pose_sources,
        stale=bool(payload.get("stale", False)),
    )
    # The worker owns the public sequence; avoid coupling it to module-global
    # counters if multiple supervised workers are exercised in one process.
    snapshot.sequence_number = state.sequence
    snapshot.objects = objects

    encode_started = time.monotonic()
    manifest_payload, frame_payload = build_viewer_protocol_payloads(
        snapshot,
        frame_roles=payload.get("frame_roles", {}),
        frame_names=payload.get("frame_names", []),
        frame_visibility=payload.get("frame_visibility", {}),
        object_axes_gizmo_size_m=float(payload["object_axes_gizmo_size_m"]),
        frame_axes_gizmo_size_m=float(payload["frame_axes_gizmo_size_m"]),
    )
    publication = state.encoder.encode(
        manifest_payload=manifest_payload,
        frame_payload=frame_payload,
    )
    encode_ms = (time.monotonic() - encode_started) * 1000.0
    total_ms = (time.monotonic() - started) * 1000.0
    return {
        "snapshot": snapshot.model_dump(mode="json"),
        "publication": None
        if publication is None
        else {
            "producer_epoch": publication.producer_epoch,
            "manifest_revision": publication.manifest_revision,
            "frame_sequence": publication.frame_sequence,
            "manifest": publication.manifest,
            "frame": publication.frame,
            "manifest_json": publication.manifest_json,
            "frame_json": publication.frame_json,
            "encoded_bytes": publication.encoded_bytes,
            "encode_duration_ms": publication.encode_duration_ms,
        },
        "stage_durations_ms": {
            "canonical_fk": canonical_fk_ms,
            "measured_fk": measured_fk_ms,
            "shape": shape_ms,
            "encode": encode_ms,
            "worker_total": total_ms,
        },
        "canonical_fk_reused": canonical_reused,
        "measured_fk_reused": measured_reused,
        "derived_attachment_offsets": derived_offsets,
        "object_revision": int(payload.get("object_revision", 0)),
    }


def _build_preview_payload(payload: dict[str, Any]) -> dict[str, Any]:
    descriptor = VisualFkModelDescriptor(**payload["model_descriptor"])
    joint_names = [str(item) for item in payload["joint_names"]]
    joint_positions = [float(item) for item in payload["joint_positions_rad"]]
    link_names = [str(item) for item in payload["link_names"]]
    if not joint_names or len(joint_names) != len(joint_positions) or not link_names:
        raise ViewerWorkerError(
            "visual_preview_shape_invalid",
            "preview requires aligned non-empty joints and link names",
        )
    started = time.monotonic()
    link_poses = compute_robot_visual_link_poses_from_descriptor(
        descriptor,
        joint_names=joint_names,
        joint_positions_rad=joint_positions,
        link_names=link_names,
        reference_frame=str(payload["reference_frame"]),
        base_frame=str(payload["base_frame"]),
        base_pose=dict(payload.get("base_pose", {})),
    )
    return {
        "link_poses": link_poses,
        "duration_ms": (time.monotonic() - started) * 1000.0,
    }


def _viewer_worker_main(
    request_queue: Any,
    response_queue: Any,
    max_input_bytes: int,
    max_output_bytes: int,
) -> None:
    policy = _apply_nrt_process_policy()
    state = _WorkerBuildState()
    while True:
        envelope = request_queue.get()
        if isinstance(envelope, dict) and envelope.get("schema") == _SHUTDOWN_SCHEMA:
            return
        request_id = 0
        try:
            request_id, payload = _decode_envelope(
                envelope,
                expected_schema=_REQUEST_SCHEMA,
                max_bytes=max_input_bytes,
            )
            operation = str(payload.get("operation") or "frame_build")
            if operation == "frame_build":
                operation_result = _build_worker_payload(payload, state=state)
            elif operation == "visual_fk_preview":
                operation_result = _build_preview_payload(payload)
            else:
                raise ViewerWorkerError(
                    "viewer_worker_operation_unsupported",
                    operation,
                )
            result = {
                "ok": True,
                "worker_epoch": state.epoch,
                "worker_policy": policy,
                "result": operation_result,
            }
        except Exception as exc:
            reason = str(getattr(exc, "reason", "") or exc.__class__.__name__)
            detail = str(getattr(exc, "detail", "") or str(exc) or reason)
            result = {
                "ok": False,
                "worker_epoch": state.epoch,
                "worker_policy": policy,
                "error": reason,
                "error_detail": detail,
            }
        try:
            response_queue.put_nowait(
                _encode_envelope(
                    schema=_RESPONSE_SCHEMA,
                    request_id=request_id,
                    payload=result,
                    max_bytes=max_output_bytes,
                )
            )
        except queue.Full:
            # The parent permits only one request in flight. A full response
            # slot therefore means the generation has been superseded or the
            # parent is retiring us; dropping is bounded and fail-contained.
            continue


class ViewerWorkerSupervisor:
    """Own one restartable worker and one request/response generation."""

    def __init__(
        self,
        *,
        timeout_s: float | None = None,
        max_input_bytes: int = _DEFAULT_MAX_INPUT_BYTES,
        max_output_bytes: int = _DEFAULT_MAX_OUTPUT_BYTES,
        worker_target: Any | None = None,
    ) -> None:
        self._timeout_s = float(
            timeout_s
            if timeout_s is not None
            else os.getenv("MANYFORGE_VIEWER_WORKER_TIMEOUT_S", "5.0")
        )
        if self._timeout_s <= 0.0:
            raise ValueError("viewer worker timeout must be positive")
        self._max_input_bytes = int(max_input_bytes)
        self._max_output_bytes = int(max_output_bytes)
        self._ctx = mp.get_context("spawn")
        self._worker_target = worker_target or _viewer_worker_main
        self._lifecycle_lock = threading.Lock()
        self._request_lock = threading.Lock()
        self._metrics_lock = threading.Lock()
        self._stopping = threading.Event()
        self._process: Any | None = None
        self._request_queue: Any | None = None
        self._response_queue: Any | None = None
        self._next_request_id = 0
        self._metrics: dict[str, Any] = {
            "starts": 0,
            "restarts": 0,
            "requests": 0,
            "completed": 0,
            "failures": 0,
            "timeouts": 0,
            "protocol_errors": 0,
            "queue_saturation": 0,
            "input_bytes": 0,
            "output_bytes": 0,
            "worker_epoch": None,
            "worker_policy": None,
            "last_error": None,
            "last_round_trip_ms": None,
        }

    def reopen(self) -> None:
        self._stopping.clear()

    def _start_locked(self) -> tuple[Any, Any, Any]:
        process = self._process
        if process is not None and process.is_alive():
            return process, self._request_queue, self._response_queue
        if process is not None:
            self._retire_locked(force=True)
            with self._metrics_lock:
                self._metrics["restarts"] += 1
        request_queue = self._ctx.Queue(maxsize=1)
        response_queue = self._ctx.Queue(maxsize=1)
        process = self._ctx.Process(
            target=self._worker_target,
            args=(
                request_queue,
                response_queue,
                self._max_input_bytes,
                self._max_output_bytes,
            ),
            name="manyforge_viewer_nrt",
            daemon=True,
        )
        process.start()
        self._process = process
        self._request_queue = request_queue
        self._response_queue = response_queue
        with self._metrics_lock:
            self._metrics["starts"] += 1
        return process, request_queue, response_queue

    def _retire_locked(self, *, force: bool) -> None:
        process = self._process
        request_queue = self._request_queue
        response_queue = self._response_queue
        self._process = None
        self._request_queue = None
        self._response_queue = None
        if process is not None:
            if not force and process.is_alive() and request_queue is not None:
                try:
                    request_queue.put_nowait({"schema": _SHUTDOWN_SCHEMA})
                except Exception:
                    pass
                process.join(timeout=0.25)
            if process.is_alive():
                process.terminate()
                process.join(timeout=0.5)
            if process.is_alive():
                process.kill()
                process.join(timeout=0.5)
        for owned_queue in (request_queue, response_queue):
            if owned_queue is None:
                continue
            try:
                owned_queue.close()
                owned_queue.cancel_join_thread()
            except Exception:
                pass

    def _retire(self, *, force: bool) -> None:
        with self._lifecycle_lock:
            self._retire_locked(force=force)

    def _request(self, payload: dict[str, Any]) -> tuple[dict[str, Any], float]:
        started = time.monotonic()
        with self._request_lock:
            if self._stopping.is_set():
                raise ViewerWorkerError("viewer_worker_stopping")
            with self._lifecycle_lock:
                process, request_queue, response_queue = self._start_locked()
                self._next_request_id += 1
                request_id = self._next_request_id
            envelope = _encode_envelope(
                schema=_REQUEST_SCHEMA,
                request_id=request_id,
                payload=payload,
                max_bytes=self._max_input_bytes,
            )
            with self._metrics_lock:
                self._metrics["requests"] += 1
                self._metrics["input_bytes"] += envelope["payload_length"]
            try:
                request_queue.put_nowait(envelope)
            except queue.Full as exc:
                with self._metrics_lock:
                    self._metrics["queue_saturation"] += 1
                    self._metrics["last_error"] = "viewer_worker_queue_saturated"
                self._retire(force=True)
                raise ViewerWorkerError("viewer_worker_queue_saturated") from exc
            except (OSError, ValueError) as exc:
                reason = (
                    "viewer_worker_stopping"
                    if self._stopping.is_set()
                    else "viewer_worker_transport_unavailable"
                )
                with self._metrics_lock:
                    self._metrics["failures"] += 1
                    self._metrics["last_error"] = reason
                self._retire(force=True)
                raise ViewerWorkerError(reason, str(exc)) from exc

            deadline = time.monotonic() + self._timeout_s
            response_envelope = None
            while response_envelope is None:
                if self._stopping.is_set():
                    raise ViewerWorkerError("viewer_worker_stopping")
                remaining = deadline - time.monotonic()
                if remaining <= 0.0:
                    with self._metrics_lock:
                        self._metrics["timeouts"] += 1
                        self._metrics["last_error"] = "viewer_worker_timeout"
                    self._retire(force=True)
                    raise ViewerWorkerError(
                        "viewer_worker_timeout",
                        f"visual worker exceeded {self._timeout_s:.3f}s",
                    )
                try:
                    response_envelope = response_queue.get(timeout=min(0.05, remaining))
                except queue.Empty:
                    if not process.is_alive():
                        exit_code = process.exitcode
                        with self._metrics_lock:
                            self._metrics["failures"] += 1
                            self._metrics["last_error"] = "viewer_worker_exited"
                        self._retire(force=True)
                        raise ViewerWorkerError(
                            "viewer_worker_exited",
                            f"visual worker exited with code {exit_code}",
                        )
                except (EOFError, OSError, ValueError) as exc:
                    reason = (
                        "viewer_worker_stopping"
                        if self._stopping.is_set()
                        else "viewer_worker_transport_unavailable"
                    )
                    with self._metrics_lock:
                        self._metrics["failures"] += 1
                        self._metrics["last_error"] = reason
                    self._retire(force=True)
                    raise ViewerWorkerError(reason, str(exc)) from exc

            try:
                response_id, response = _decode_envelope(
                    response_envelope,
                    expected_schema=_RESPONSE_SCHEMA,
                    max_bytes=self._max_output_bytes,
                )
                if response_id != request_id:
                    raise ViewerWorkerError(
                        "viewer_worker_protocol_error",
                        f"expected response {request_id}, received {response_id}",
                    )
            except ViewerWorkerError:
                with self._metrics_lock:
                    self._metrics["protocol_errors"] += 1
                    self._metrics["last_error"] = "viewer_worker_protocol_error"
                self._retire(force=True)
                raise

            round_trip_ms = (time.monotonic() - started) * 1000.0
            with self._metrics_lock:
                self._metrics["output_bytes"] += int(
                    response_envelope["payload_length"]
                )
                self._metrics["worker_epoch"] = response.get("worker_epoch")
                self._metrics["worker_policy"] = response.get("worker_policy")
                self._metrics["last_round_trip_ms"] = round_trip_ms
            if response.get("ok") is not True:
                reason = str(response.get("error") or "viewer_worker_build_failed")
                detail = str(response.get("error_detail") or reason)
                with self._metrics_lock:
                    self._metrics["failures"] += 1
                    self._metrics["last_error"] = reason
                raise ViewerWorkerError(reason, detail)

            result = response.get("result")
            if not isinstance(result, dict):
                self._retire(force=True)
                raise ViewerWorkerError(
                    "viewer_worker_protocol_error",
                    "successful response omitted result",
                )
            with self._metrics_lock:
                self._metrics["completed"] += 1
                self._metrics["last_error"] = None
            return result, round_trip_ms

    def build(
        self,
        payload: dict[str, Any],
        *,
        input_signature: tuple[Any, ...],
        sample_duration_ms: float,
        sample_stage_durations_ms: dict[str, float] | None = None,
    ) -> ViewerSnapshotBuild:
        request_payload = {"operation": "frame_build", **payload}
        result, round_trip_ms = self._request(request_payload)
        try:
            snapshot = VisualSceneSnapshotResponse.model_validate(result["snapshot"])
            publication_payload = result.get("publication")
            publication = (
                None
                if publication_payload is None
                else ViewerPublication(**publication_payload)
            )
            stage_durations = {
                str(key): float(value)
                for key, value in result.get("stage_durations_ms", {}).items()
            }
            stage_durations.update(
                {
                    str(key): float(value)
                    for key, value in (sample_stage_durations_ms or {}).items()
                }
            )
            stage_durations["sample"] = float(sample_duration_ms)
            stage_durations["ipc_round_trip"] = round_trip_ms
            stage_durations["total_build"] = sample_duration_ms + round_trip_ms
            return ViewerSnapshotBuild(
                snapshot=snapshot,
                input_signature=input_signature,
                stage_durations_ms=stage_durations,
                canonical_fk_reused=bool(result.get("canonical_fk_reused", False)),
                measured_fk_reused=int(result.get("measured_fk_reused", 0)),
                publication=publication,
                derived_attachment_offsets=dict(
                    result.get("derived_attachment_offsets", {})
                ),
                object_revision=int(result.get("object_revision", 0)),
            )
        except Exception as exc:
            with self._metrics_lock:
                self._metrics["protocol_errors"] += 1
                self._metrics["last_error"] = "viewer_worker_protocol_error"
            self._retire(force=True)
            raise ViewerWorkerError(
                "viewer_worker_protocol_error",
                str(exc),
            ) from exc

    def compute_visual_fk_preview(
        self,
        payload: dict[str, Any],
    ) -> dict[str, dict[str, Any]]:
        result, _round_trip_ms = self._request(
            {"operation": "visual_fk_preview", **payload}
        )
        link_poses = result.get("link_poses")
        if not isinstance(link_poses, dict):
            self._retire(force=True)
            raise ViewerWorkerError(
                "viewer_worker_protocol_error",
                "preview response omitted link_poses",
            )
        return {
            str(name): dict(pose)
            for name, pose in link_poses.items()
            if isinstance(pose, dict)
        }

    def status(self) -> dict[str, Any]:
        with self._lifecycle_lock:
            process = self._process
            alive = bool(process is not None and process.is_alive())
            pid = process.pid if process is not None else None
            exit_code = process.exitcode if process is not None else None
        with self._metrics_lock:
            metrics = dict(self._metrics)
        return {
            "schema_version": "manyforge.viewer.worker_status.v1",
            "alive": alive,
            "pid": pid,
            "exit_code": exit_code,
            "timeout_s": self._timeout_s,
            "max_input_bytes": self._max_input_bytes,
            "max_output_bytes": self._max_output_bytes,
            **metrics,
        }

    def stop(self) -> None:
        self._stopping.set()
        # _request() observes _stopping at most one 50 ms queue wait later.
        # Serialize final queue closure behind that exit so multiprocessing's
        # feeder/resource-tracker cleanup never races an active Queue.get().
        # Lock order matches _request(): request -> lifecycle.
        with self._request_lock:
            self._retire(force=False)

    def __del__(self) -> None:
        try:
            self.stop()
        except Exception:
            pass


__all__ = [
    "ViewerWorkerError",
    "ViewerWorkerSupervisor",
]
