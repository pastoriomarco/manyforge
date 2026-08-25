"""Strict Isaac pose-observer configuration and bounded Manager dispatch.

This module contains no ROS executor and no scene/lifecycle owner.  It closes
the site configuration, translates already-normalized observations into the
one Manager ingress contract, and provides a capacity-one-per-key worker so a
ROS callback never waits on local IPC.
"""

from __future__ import annotations

from collections import OrderedDict
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
import json
import os
from pathlib import Path
import secrets
import stat
import threading
import time
from typing import Any, Protocol
import unicodedata

from manyforge_ros.manager_scene_runtime import (
    MAXIMUM_SAFE_INTEGER,
    ManagerSceneRuntimeClient,
    PoseObservationIntent,
    PoseObservationResult,
    PoseObserverBinding,
    PoseObserverSession,
    SceneRuntimeManagerError,
    SceneRuntimeOwnerSnapshot,
)
from manyforge_ros.pose_source_sidecar import (
    DetectionSelector,
    NormalizedDetectionPose,
    NormalizedJointState,
    ObservationNormalizationError,
    SimulationEntityPoseObservation,
    SimulationEntityPoseRequest,
)
from manyforge_ros.scene_mirror_probe import PoseSnapshot


SITE_CONFIG_VERSION = "manyforge.isaac_pose_observer_site.v1"
OBSERVATION_PAYLOAD_VERSION = "manyforge.ros_pose_observation.v1"
MAX_SITE_CONFIG_BYTES = 256 * 1024
MAX_CONFIGURED_OBSERVATIONS = 128

_MAX_TEXT_BYTES = 1024
_MAX_DETAIL_BYTES = 512
_REJOIN_CODES = {
    "manager_scene_runtime_unavailable",
    "manager_scene_runtime_response_invalid",
    "scene_runtime_identity_changed",
    "scene_runtime_observer_session_stale",
    "scene_runtime_owner_unavailable",
}


class IsaacPoseObserverConfigError(ValueError):
    """One closed site-configuration failure."""


@dataclass(frozen=True)
class JointStateBinding:
    topic: str
    object_id: str
    policy_id: str
    lifetime_ns: int
    eligible: bool
    required_joint_names: tuple[str, ...]

    @property
    def key(self) -> tuple[str, str]:
        return (self.object_id, self.policy_id)


@dataclass(frozen=True)
class SimulationEntityBinding:
    request: SimulationEntityPoseRequest
    frame_kind: str
    object_id: str
    policy_id: str
    lifetime_ns: int
    eligible: bool

    @property
    def key(self) -> tuple[str, str]:
        return (self.object_id, self.policy_id)


@dataclass(frozen=True)
class Detection3dBinding:
    topic: str
    frame_kind: str
    object_id: str
    policy_id: str
    lifetime_ns: int
    eligible: bool
    fallback_frame_id: str
    target_frame_id: str
    selector: DetectionSelector

    @property
    def key(self) -> tuple[str, str]:
        return (self.object_id, self.policy_id)


ObservationBinding = JointStateBinding | SimulationEntityBinding | Detection3dBinding


@dataclass(frozen=True)
class IsaacPoseObserverSiteConfig:
    cell_id: str
    source_id: str
    simulation_poll_period_ms: int
    joint_state: JointStateBinding | None
    simulation_entities: tuple[SimulationEntityBinding, ...]
    detection3d_sources: tuple[Detection3dBinding, ...]

    @property
    def bindings(self) -> tuple[ObservationBinding, ...]:
        joint = () if self.joint_state is None else (self.joint_state,)
        return joint + self.simulation_entities + self.detection3d_sources

    @property
    def keys(self) -> frozenset[tuple[str, str]]:
        return frozenset(binding.key for binding in self.bindings)

    @property
    def observer_bindings(self) -> tuple[PoseObserverBinding, ...]:
        return tuple(
            sorted(
                PoseObserverBinding(
                    object_id=binding.object_id,
                    policy_id=binding.policy_id,
                    eligible=binding.eligible,
                )
                for binding in self.bindings
            )
        )


@dataclass(frozen=True)
class ObservationDispatchEvent:
    kind: str
    object_id: str
    policy_id: str
    source_sequence: int | None
    code: str
    detail: str
    scene_application_status: str | None
    scene_request_id: str | None
    scene_runtime_source_generation: int | None
    scene_update_status: str | None


@dataclass(frozen=True)
class ObservationDispatchSnapshot:
    running: bool
    pending: int
    inflight: bool
    submitted: int
    superseded: int
    delivered: int
    retries: int
    expired: int
    joins: int
    last_error_code: str


class _SceneRuntimeClient(Protocol):
    def read_owner(self) -> SceneRuntimeOwnerSnapshot: ...

    def open_observer(
        self,
        owner: SceneRuntimeOwnerSnapshot,
        *,
        bindings: tuple[PoseObserverBinding, ...],
        source_id: str,
        source_epoch: str,
    ) -> PoseObserverSession: ...

    def observe(
        self,
        session: PoseObserverSession,
        *,
        source_sequence: int,
        intent: PoseObservationIntent,
    ) -> PoseObservationResult: ...


@dataclass
class _PendingObservation:
    key: tuple[str, str]
    intent: PoseObservationIntent
    enqueued_at_ns: int
    source_sequence: int


def load_isaac_pose_observer_site_config(
    path: Path,
) -> IsaacPoseObserverSiteConfig:
    """Read one bounded non-symlink strict-JSON site configuration."""

    config_path = Path(path)
    try:
        before = config_path.lstat()
    except OSError as exc:
        raise IsaacPoseObserverConfigError("site config is unavailable") from exc
    if (
        not stat.S_ISREG(before.st_mode)
        or stat.S_ISLNK(before.st_mode)
        or before.st_size <= 0
        or before.st_size > MAX_SITE_CONFIG_BYTES
    ):
        raise IsaacPoseObserverConfigError(
            "site config must be one bounded non-symlink regular file"
        )
    flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(config_path, flags)
        try:
            opened = os.fstat(descriptor)
            if (
                not stat.S_ISREG(opened.st_mode)
                or (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino)
            ):
                raise IsaacPoseObserverConfigError(
                    "site config identity changed while opening"
                )
            chunks: list[bytes] = []
            remaining = MAX_SITE_CONFIG_BYTES + 1
            while remaining:
                chunk = os.read(descriptor, min(remaining, 64 * 1024))
                if not chunk:
                    break
                chunks.append(chunk)
                remaining -= len(chunk)
            payload = b"".join(chunks)
            after = os.fstat(descriptor)
        finally:
            os.close(descriptor)
    except IsaacPoseObserverConfigError:
        raise
    except OSError as exc:
        raise IsaacPoseObserverConfigError("site config cannot be read") from exc
    if (
        len(payload) != before.st_size
        or len(payload) > MAX_SITE_CONFIG_BYTES
        or (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        )
        != (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
            before.st_ctime_ns,
        )
    ):
        raise IsaacPoseObserverConfigError(
            "site config changed or exceeded its bound while reading"
        )
    root = _strict_json_object(payload, label="site config")
    _require_fields(
        root,
        {
            "api_version",
            "cell_id",
            "detection3d_sources",
            "joint_state",
            "simulation_entities",
            "simulation_poll_period_ms",
            "source_id",
        },
        label="site config",
    )
    if root["api_version"] != SITE_CONFIG_VERSION:
        raise IsaacPoseObserverConfigError("site config api_version is unsupported")
    cell_id = _text(root["cell_id"], label="cell_id")
    source_id = _text(root["source_id"], label="source_id")
    poll_period = _integer(
        root["simulation_poll_period_ms"],
        label="simulation_poll_period_ms",
        minimum=20,
        maximum=60_000,
    )
    joint = _joint_binding(root["joint_state"])
    simulations = tuple(
        _simulation_binding(value)
        for value in _bounded_array(
            root["simulation_entities"], label="simulation_entities"
        )
    )
    detections = tuple(
        _detection_binding(value)
        for value in _bounded_array(
            root["detection3d_sources"], label="detection3d_sources"
        )
    )
    bindings: tuple[ObservationBinding, ...] = (
        (() if joint is None else (joint,)) + simulations + detections
    )
    if not bindings:
        raise IsaacPoseObserverConfigError(
            "site config must declare at least one observation binding"
        )
    if len(bindings) > MAX_CONFIGURED_OBSERVATIONS:
        raise IsaacPoseObserverConfigError(
            "site config exceeds the observation binding bound"
        )
    keys = [binding.key for binding in bindings]
    if len(set(keys)) != len(keys):
        raise IsaacPoseObserverConfigError(
            "site config observation object/policy keys must be unique"
        )
    return IsaacPoseObserverSiteConfig(
        cell_id=cell_id,
        source_id=source_id,
        simulation_poll_period_ms=poll_period,
        joint_state=joint,
        simulation_entities=simulations,
        detection3d_sources=detections,
    )


def joint_state_observation_intent(
    binding: JointStateBinding,
    observation: NormalizedJointState,
) -> PoseObservationIntent:
    if observation.topic != binding.topic:
        raise ObservationNormalizationError(
            "joint_topic_mismatch",
            "normalized JointState came from a different configured topic",
        )
    positions = dict(zip(observation.names, observation.positions))
    missing = [name for name in binding.required_joint_names if name not in positions]
    if missing:
        raise ObservationNormalizationError(
            "joint_set_incomplete",
            "JointState is missing required joints: " + ",".join(missing),
        )
    return PoseObservationIntent(
        object_id=binding.object_id,
        policy_id=binding.policy_id,
        lifetime_ns=binding.lifetime_ns,
        quality="good",
        eligible=binding.eligible,
        payload={
            "api_version": OBSERVATION_PAYLOAD_VERSION,
            "joint_names": list(binding.required_joint_names),
            "kind": "joint_state",
            "positions": [positions[name] for name in binding.required_joint_names],
            "source_stamp_s": observation.source_stamp_s,
            "topic": binding.topic,
        },
    )


def simulation_entity_observation_intent(
    binding: SimulationEntityBinding,
    observation: SimulationEntityPoseObservation,
) -> PoseObservationIntent:
    if observation.entity != binding.request.entity:
        raise ObservationNormalizationError(
            "simulation_entity_mismatch",
            "simulator observation has a different configured entity",
        )
    return _pose_intent(
        binding=binding,
        kind="simulation_entity_pose",
        pose=observation.pose,
        source={"entity": observation.entity},
    )


def detection3d_observation_intent(
    binding: Detection3dBinding,
    observation: NormalizedDetectionPose,
) -> PoseObservationIntent:
    return _pose_intent(
        binding=binding,
        kind="detection3d_pose",
        pose=observation.pose,
        source={
            "class_id": observation.class_id,
            "confidence": observation.confidence,
            "detection_id": observation.detection_id,
            "max_position_variance_m2": observation.max_position_variance_m2,
            "topic": binding.topic,
        },
    )


def unknown_observation_intent(
    binding: ObservationBinding,
    *,
    code: str,
    detail: str,
    source_key: str,
) -> PoseObservationIntent:
    kind = {
        JointStateBinding: "joint_state",
        SimulationEntityBinding: "simulation_entity_pose",
        Detection3dBinding: "detection3d_pose",
    }[type(binding)]
    return PoseObservationIntent(
        object_id=binding.object_id,
        policy_id=binding.policy_id,
        lifetime_ns=binding.lifetime_ns,
        quality="unknown",
        eligible=False,
        payload={
            "api_version": OBSERVATION_PAYLOAD_VERSION,
            "kind": kind,
            "source_error": {
                "code": _text(code, label="source error code", maximum=128),
                "detail": _text(
                    detail,
                    label="source error detail",
                    maximum=_MAX_DETAIL_BYTES,
                ),
                "source_key": _text(
                    source_key or "<source>",
                    label="source error key",
                    maximum=_MAX_TEXT_BYTES,
                ),
            },
        },
    )


class ObservationDispatcher:
    """Capacity-one-per-key nonblocking adapter with exact retry identity."""

    def __init__(
        self,
        *,
        client: _SceneRuntimeClient,
        expected_cell_id: str,
        source_id: str,
        allowed_keys: frozenset[tuple[str, str]],
        observer_bindings: tuple[PoseObserverBinding, ...] | None = None,
        event_callback: Callable[[ObservationDispatchEvent], None] | None = None,
        clock_ns: Callable[[], int] = time.monotonic_ns,
        source_epoch_factory: Callable[[], str] | None = None,
        retry_initial_s: float = 0.05,
        retry_max_s: float = 1.0,
    ) -> None:
        self._client = client
        self._expected_cell_id = _text(expected_cell_id, label="expected_cell_id")
        self._source_id = _text(source_id, label="source_id")
        if not allowed_keys or len(allowed_keys) > MAX_CONFIGURED_OBSERVATIONS:
            raise ValueError("allowed_keys must be one bounded non-empty set")
        self._allowed_keys = frozenset(allowed_keys)
        self._observer_bindings = (
            tuple(
                PoseObserverBinding(
                    object_id=object_id,
                    policy_id=policy_id,
                    eligible=False,
                )
                for object_id, policy_id in sorted(self._allowed_keys)
            )
            if observer_bindings is None
            else observer_bindings
        )
        if (
            tuple(sorted(self._observer_bindings)) != self._observer_bindings
            or {
                (binding.object_id, binding.policy_id)
                for binding in self._observer_bindings
            }
            != self._allowed_keys
        ):
            raise ValueError("observer bindings must exactly cover allowed_keys")
        self._event_callback = event_callback
        self._clock_ns = clock_ns
        self._source_epoch_factory = source_epoch_factory or (
            lambda: "isaac-observer-" + secrets.token_hex(16)
        )
        if (
            retry_initial_s <= 0.0
            or retry_max_s < retry_initial_s
            or retry_max_s > 5.0
        ):
            raise ValueError("retry bounds are invalid")
        self._retry_initial_s = retry_initial_s
        self._retry_max_s = retry_max_s
        self._condition = threading.Condition()
        self._pending: OrderedDict[
            tuple[str, str], tuple[PoseObservationIntent, int]
        ] = OrderedDict()
        self._inflight: _PendingObservation | None = None
        self._session: PoseObserverSession | None = None
        self._source_epoch = self._new_source_epoch()
        self._next_sequence = 1
        self._stop_requested = False
        self._thread: threading.Thread | None = None
        self._submitted = 0
        self._superseded = 0
        self._delivered = 0
        self._retries = 0
        self._expired = 0
        self._joins = 0
        self._last_error_code = ""
        self._manager_owner_epoch: str | None = None

    @classmethod
    def for_manager_root(
        cls,
        *,
        manager_run_root: Path,
        config: IsaacPoseObserverSiteConfig,
        event_callback: Callable[[ObservationDispatchEvent], None] | None = None,
        timeout_s: float = 2.0,
    ) -> ObservationDispatcher:
        return cls(
            client=ManagerSceneRuntimeClient(
                manager_run_root,
                timeout_s=timeout_s,
            ),
            expected_cell_id=config.cell_id,
            source_id=config.source_id,
            allowed_keys=config.keys,
            observer_bindings=config.observer_bindings,
            event_callback=event_callback,
        )

    def start(self) -> None:
        with self._condition:
            if self._thread is not None:
                raise RuntimeError("observation dispatcher was already started")
            self._thread = threading.Thread(
                target=self._run,
                name="manyforge-isaac-pose-observer-dispatch",
                daemon=True,
            )
            self._thread.start()

    def submit(self, intent: PoseObservationIntent) -> None:
        if not isinstance(intent, PoseObservationIntent):
            raise TypeError("intent must be a PoseObservationIntent")
        key = (intent.object_id, intent.policy_id)
        if key not in self._allowed_keys:
            raise ValueError("observation key is not configured for this dispatcher")
        now = self._clock_ns()
        with self._condition:
            if self._stop_requested:
                raise RuntimeError("observation dispatcher is stopping")
            if key in self._pending:
                self._superseded += 1
            self._pending[key] = (intent, now)
            self._submitted += 1
            self._condition.notify()

    def close(self, *, timeout_s: float = 6.0) -> bool:
        with self._condition:
            self._stop_requested = True
            self._condition.notify_all()
            thread = self._thread
        if thread is None:
            return True
        thread.join(timeout=timeout_s)
        return not thread.is_alive()

    def snapshot(self) -> ObservationDispatchSnapshot:
        with self._condition:
            return ObservationDispatchSnapshot(
                running=self._thread is not None and self._thread.is_alive(),
                pending=len(self._pending),
                inflight=self._inflight is not None,
                submitted=self._submitted,
                superseded=self._superseded,
                delivered=self._delivered,
                retries=self._retries,
                expired=self._expired,
                joins=self._joins,
                last_error_code=self._last_error_code,
            )

    def _run(self) -> None:
        retry_delay = self._retry_initial_s
        while True:
            if self._session is None:
                try:
                    owner = self._client.read_owner()
                    if owner.cell_id != self._expected_cell_id:
                        raise SceneRuntimeManagerError(
                            "pose_observer_cell_mismatch",
                            "Manager active cell does not match the site config",
                        )
                    self._session = self._client.open_observer(
                        owner,
                        bindings=self._observer_bindings,
                        source_id=self._source_id,
                        source_epoch=self._source_epoch,
                    )
                    self._manager_owner_epoch = owner.owner_epoch
                except SceneRuntimeManagerError as exc:
                    with self._condition:
                        self._retries += 1
                        self._last_error_code = exc.code
                        if not self._stop_requested:
                            self._condition.wait(timeout=retry_delay)
                    retry_delay = min(self._retry_max_s, retry_delay * 2.0)
                    continue
                with self._condition:
                    self._joins += 1
                    self._last_error_code = ""
                retry_delay = self._retry_initial_s
            with self._condition:
                heartbeat = False
                restart_session = False
                while (
                    not self._stop_requested
                    and self._inflight is None
                    and not self._pending
                ):
                    self._condition.wait(timeout=1.0)
                    if not self._pending and not self._stop_requested:
                        heartbeat = True
                        break
                if self._stop_requested:
                    return
                if heartbeat:
                    pending = None
                else:
                    if self._inflight is None:
                        if self._next_sequence > MAXIMUM_SAFE_INTEGER:
                            self._source_epoch = self._new_source_epoch()
                            self._session = None
                            self._manager_owner_epoch = None
                            self._next_sequence = 1
                            restart_session = True
                        else:
                            key, (intent, enqueued_at) = self._pending.popitem(
                                last=False
                            )
                            self._inflight = _PendingObservation(
                                key=key,
                                intent=intent,
                                enqueued_at_ns=enqueued_at,
                                source_sequence=self._next_sequence,
                            )
                    pending = None if restart_session else self._inflight
            if restart_session:
                continue
            if heartbeat:
                try:
                    owner = self._client.read_owner()
                    if (
                        owner.cell_id != self._expected_cell_id
                        or self._session is None
                        or owner.owner_epoch != self._manager_owner_epoch
                        or owner.runtime_identity != self._session.runtime_identity
                    ):
                        self._session = None
                        self._manager_owner_epoch = None
                except SceneRuntimeManagerError as exc:
                    with self._condition:
                        self._retries += 1
                        self._last_error_code = exc.code
                    self._session = None
                    self._manager_owner_epoch = None
                continue
            assert pending is not None
            age_ns = max(0, self._clock_ns() - pending.enqueued_at_ns)
            if age_ns > pending.intent.lifetime_ns:
                with self._condition:
                    if self._inflight is pending:
                        self._inflight = None
                        self._next_sequence += 1
                        self._expired += 1
                self._emit(
                    "expired",
                    pending,
                    code="observation_expired_before_delivery",
                    detail="observation exceeded its lifetime before Manager receipt",
                )
                retry_delay = self._retry_initial_s
                continue
            try:
                assert self._session is not None
                result = self._client.observe(
                    self._session,
                    source_sequence=pending.source_sequence,
                    intent=pending.intent,
                )
            except SceneRuntimeManagerError as exc:
                with self._condition:
                    self._retries += 1
                    self._last_error_code = exc.code
                    if exc.code in _REJOIN_CODES:
                        self._session = None
                        self._manager_owner_epoch = None
                    elif exc.code == "scene_runtime_observation_request_conflict":
                        self._source_epoch = self._new_source_epoch()
                        self._session = None
                        self._manager_owner_epoch = None
                        pending.source_sequence = 1
                        self._next_sequence = 1
                self._emit(
                    "retry",
                    pending,
                    code=exc.code,
                    detail=str(exc),
                )
                remaining_s = max(
                    0.0,
                    (pending.intent.lifetime_ns - age_ns) / 1_000_000_000,
                )
                with self._condition:
                    if not self._stop_requested:
                        self._condition.wait(timeout=min(retry_delay, remaining_s))
                retry_delay = min(self._retry_max_s, retry_delay * 2.0)
                continue
            with self._condition:
                if self._inflight is pending:
                    self._inflight = None
                    self._next_sequence += 1
                    self._delivered += 1
                    self._last_error_code = ""
            self._emit(
                "delivered",
                pending,
                code=result.disposition,
                detail=("fresh" if result.fresh else "not_fresh"),
                result=result,
            )
            retry_delay = self._retry_initial_s

    def _new_source_epoch(self) -> str:
        return _text(self._source_epoch_factory(), label="source_epoch", maximum=256)

    def _emit(
        self,
        kind: str,
        pending: _PendingObservation,
        *,
        code: str,
        detail: str,
        result: PoseObservationResult | None = None,
    ) -> None:
        if self._event_callback is None:
            return
        try:
            self._event_callback(
                ObservationDispatchEvent(
                    kind=kind,
                    object_id=pending.key[0],
                    policy_id=pending.key[1],
                    source_sequence=pending.source_sequence,
                    code=code,
                    detail=detail[:_MAX_DETAIL_BYTES],
                    scene_application_status=(
                        None if result is None else result.scene_application_status
                    ),
                    scene_request_id=(
                        None if result is None else result.scene_request_id
                    ),
                    scene_runtime_source_generation=(
                        None
                        if result is None
                        else result.scene_runtime_source_generation
                    ),
                    scene_update_status=(
                        None if result is None else result.scene_update_status
                    ),
                )
            )
        except Exception:
            return


def _pose_intent(
    *,
    binding: SimulationEntityBinding | Detection3dBinding,
    kind: str,
    pose: PoseSnapshot,
    source: dict[str, Any],
) -> PoseObservationIntent:
    target_frame_id = (
        binding.request.target_frame_id
        if isinstance(binding, SimulationEntityBinding)
        else binding.target_frame_id
    )
    if pose.frame_id != target_frame_id:
        raise ObservationNormalizationError(
            "pose_frame_mismatch",
            "normalized pose does not use the configured cell frame",
        )
    return PoseObservationIntent(
        object_id=binding.object_id,
        policy_id=binding.policy_id,
        lifetime_ns=binding.lifetime_ns,
        quality="good",
        eligible=binding.eligible,
        payload={
            "api_version": OBSERVATION_PAYLOAD_VERSION,
            "kind": kind,
            "pose": {
                "frame_kind": binding.frame_kind,
                "frame_id": pose.frame_id,
                "orientation_xyzw": list(pose.orientation_xyzw),
                "position_m": list(pose.position_xyz),
                "source_stamp_s": pose.stamp_sec,
            },
            "source": source,
        },
    )


def _joint_binding(value: object) -> JointStateBinding | None:
    if value is None:
        return None
    record = _object(value, label="joint_state")
    _require_fields(
        record,
        {
            "eligible",
            "lifetime_ns",
            "object_id",
            "policy_id",
            "required_joint_names",
            "topic",
        },
        label="joint_state",
    )
    names = tuple(
        _text(name, label="required joint name")
        for name in _bounded_array(
            record["required_joint_names"],
            label="required_joint_names",
            allow_empty=False,
        )
    )
    if len(set(names)) != len(names):
        raise IsaacPoseObserverConfigError("required_joint_names must be unique")
    return JointStateBinding(
        topic=_ros_topic(record["topic"], label="joint_state topic"),
        object_id=_text(record["object_id"], label="joint_state object_id"),
        policy_id=_text(record["policy_id"], label="joint_state policy_id"),
        lifetime_ns=_lifetime(record["lifetime_ns"]),
        eligible=_bool(record["eligible"], label="joint_state eligible"),
        required_joint_names=names,
    )


def _simulation_binding(value: object) -> SimulationEntityBinding:
    record = _object(value, label="simulation entity")
    _require_fields(
        record,
        {
            "eligible",
            "entity",
            "frame_kind",
            "lifetime_ns",
            "local_orientation_xyzw",
            "local_position_xyz",
            "object_id",
            "policy_id",
            "source_coordinate_frame_id",
            "source_header_frame_id",
            "target_frame_id",
        },
        label="simulation entity",
    )
    target_frame_id = _text(
        record["target_frame_id"], label="simulation target_frame_id"
    )
    try:
        request = SimulationEntityPoseRequest(
            entity=record["entity"],
            target_frame_id=target_frame_id,
            source_header_frame_id=record["source_header_frame_id"],
            source_coordinate_frame_id=record["source_coordinate_frame_id"],
            local_position_xyz=tuple(record["local_position_xyz"]),
            local_orientation_xyzw=tuple(record["local_orientation_xyzw"]),
        )
    except (TypeError, ValueError) as exc:
        raise IsaacPoseObserverConfigError(
            f"simulation entity request is invalid: {exc}"
        ) from exc
    return SimulationEntityBinding(
        request=request,
        frame_kind=_frame_kind(record["frame_kind"]),
        object_id=_text(record["object_id"], label="simulation object_id"),
        policy_id=_text(record["policy_id"], label="simulation policy_id"),
        lifetime_ns=_lifetime(record["lifetime_ns"]),
        eligible=_bool(record["eligible"], label="simulation eligible"),
    )


def _detection_binding(value: object) -> Detection3dBinding:
    record = _object(value, label="Detection3D source")
    _require_fields(
        record,
        {
            "eligible",
            "fallback_frame_id",
            "frame_kind",
            "lifetime_ns",
            "object_id",
            "policy_id",
            "selector",
            "target_frame_id",
            "topic",
        },
        label="Detection3D source",
    )
    selector = _object(record["selector"], label="Detection3D selector")
    _require_fields(
        selector,
        {
            "class_id",
            "confidence_min",
            "detection_id",
            "max_position_variance_m2",
            "selection",
        },
        label="Detection3D selector",
    )
    try:
        normalized_selector = DetectionSelector(
            detection_id=selector["detection_id"],
            class_id=selector["class_id"],
            selection=selector["selection"],
            confidence_min=selector["confidence_min"],
            max_position_variance_m2=selector["max_position_variance_m2"],
        )
    except (TypeError, ValueError) as exc:
        raise IsaacPoseObserverConfigError(
            f"Detection3D selector is invalid: {exc}"
        ) from exc
    target_frame_id = _text(
        record["target_frame_id"], label="Detection3D target_frame_id"
    )
    return Detection3dBinding(
        topic=_ros_topic(record["topic"], label="Detection3D topic"),
        frame_kind=_frame_kind(record["frame_kind"]),
        object_id=_text(record["object_id"], label="Detection3D object_id"),
        policy_id=_text(record["policy_id"], label="Detection3D policy_id"),
        lifetime_ns=_lifetime(record["lifetime_ns"]),
        eligible=_bool(record["eligible"], label="Detection3D eligible"),
        fallback_frame_id=_optional_text(
            record["fallback_frame_id"], label="fallback_frame_id"
        ),
        target_frame_id=target_frame_id,
        selector=normalized_selector,
    )


def _strict_json_object(payload: bytes, *, label: str) -> dict[str, Any]:
    def unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result: dict[str, Any] = {}
        for key, value in pairs:
            if key in result:
                raise IsaacPoseObserverConfigError(
                    f"{label} has duplicate member {key!r}"
                )
            result[key] = value
        return result

    def reject_constant(value: str) -> None:
        raise IsaacPoseObserverConfigError(
            f"{label} contains unsupported JSON constant {value}"
        )

    try:
        text = payload.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=unique_object,
            parse_constant=reject_constant,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise IsaacPoseObserverConfigError(
            f"{label} is not strict UTF-8 JSON"
        ) from exc
    if not isinstance(value, dict):
        raise IsaacPoseObserverConfigError(f"{label} root must be an object")
    return value


def _object(value: object, *, label: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise IsaacPoseObserverConfigError(f"{label} must be an object")
    return value


def _require_fields(value: Mapping[str, Any], fields: set[str], *, label: str) -> None:
    if set(value) != fields:
        raise IsaacPoseObserverConfigError(
            f"{label} has an unknown or missing field"
        )


def _bounded_array(
    value: object,
    *,
    label: str,
    allow_empty: bool = True,
) -> Sequence[Any]:
    if (
        not isinstance(value, list)
        or (not allow_empty and not value)
        or len(value) > MAX_CONFIGURED_OBSERVATIONS
    ):
        raise IsaacPoseObserverConfigError(f"{label} is not one bounded array")
    return value


def _text(value: object, *, label: str, maximum: int = _MAX_TEXT_BYTES) -> str:
    if (
        not isinstance(value, str)
        or not value
        or unicodedata.normalize("NFC", value) != value
        or len(value.encode("utf-8")) > maximum
    ):
        raise IsaacPoseObserverConfigError(
            f"{label} must be bounded non-empty NFC text"
        )
    return value


def _optional_text(value: object, *, label: str) -> str:
    if value == "":
        return ""
    return _text(value, label=label)


def _frame_kind(value: object) -> str:
    if value != "cell_frame":
        raise IsaacPoseObserverConfigError(
            "pose frame_kind must be the exact cell_frame semantic"
        )
    return "cell_frame"


def _ros_topic(value: object, *, label: str) -> str:
    topic = _text(value, label=label, maximum=256)
    if (
        not topic.startswith("/")
        or topic == "/"
        or "//" in topic
        or any(character.isspace() for character in topic)
    ):
        raise IsaacPoseObserverConfigError(f"{label} is not an absolute ROS name")
    return topic


def _integer(
    value: object,
    *,
    label: str,
    minimum: int,
    maximum: int,
) -> int:
    if type(value) is not int or value < minimum or value > maximum:
        raise IsaacPoseObserverConfigError(f"{label} is outside its integer bound")
    return value


def _lifetime(value: object) -> int:
    return _integer(
        value,
        label="lifetime_ns",
        minimum=1,
        maximum=60_000_000_000,
    )


def _bool(value: object, *, label: str) -> bool:
    if type(value) is not bool:
        raise IsaacPoseObserverConfigError(f"{label} must be an exact bool")
    return value
