"""Bounded ROS object-pose source sidecars.

This module implements runtime slices for the deployment ``pose_sources``
contract: read-only polling of explicitly configured simulator entities via
ROS 2 ``simulation_interfaces`` and callback-driven normalization of selected
ROS perception topics.  It does not mutate Composer or kernel scene state.
Later slices consume the same latest-wins snapshots for observe-only UI status
and safe planning-scene apply policies.
"""

from __future__ import annotations

import threading
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from math import isfinite, sqrt
from typing import Any

from manyforge_ros.scene_mirror_probe import PoseSnapshot, SimulationSceneMirrorClient


POSE_SOURCE_STATUS_DISABLED = "disabled"
POSE_SOURCE_STATUS_FRESH = "fresh"
POSE_SOURCE_STATUS_STALE = "stale"
POSE_SOURCE_STATUS_REJECTED = "rejected"


@dataclass(frozen=True)
class PoseSourceBinding:
    """Binding from one ManyForge scene object to one source-local entity."""

    object_id: str
    entity: str
    apply_policy: str = "observe_only"
    frame_id: str = ""
    confidence_min: float | None = None


@dataclass(frozen=True)
class PoseSourceMeasuredTcpBinding:
    """Optional source-level measured TCP entity."""

    entity: str
    tcp_frame_id: str
    frame_id: str = ""


@dataclass(frozen=True)
class PoseSourceUpdate:
    """Accepted pose update for a bound object."""

    pose_source_id: str
    object_id: str
    entity: str
    pose: PoseSnapshot
    monotonic_s: float
    apply_policy: str
    metadata: dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class PoseSourceRejection:
    """Rejected pose update for a bound object."""

    pose_source_id: str
    object_id: str
    entity: str
    reason: str
    monotonic_s: float
    detail: str = ""


@dataclass(frozen=True)
class PoseSourceMeasuredTcpUpdate:
    """Accepted measured TCP update sampled with source object poses."""

    pose_source_id: str
    entity: str
    tcp_frame_id: str
    frame_id: str
    pose: PoseSnapshot
    monotonic_s: float


@dataclass(frozen=True)
class PoseSourceObjectState:
    """Latest object state exposed by the sidecar."""

    object_id: str
    status: str
    update: PoseSourceUpdate | None = None
    rejection: PoseSourceRejection | None = None
    age_s: float | None = None


@dataclass(frozen=True)
class PoseSourceSnapshot:
    """Latest-wins state snapshot for all objects from one source."""

    pose_source_id: str
    status: str
    monotonic_s: float
    objects: tuple[PoseSourceObjectState, ...]
    measured_tcp: PoseSourceMeasuredTcpUpdate | None = None
    measured_tcp_age_s: float | None = None

    def to_dict(self) -> dict[str, Any]:
        payload = {
            "poseSourceId": self.pose_source_id,
            "status": self.status,
            "monotonicS": self.monotonic_s,
            "objects": [
                {
                    "objectId": obj.object_id,
                    "status": obj.status,
                    "ageS": obj.age_s,
                    "update": _update_to_dict(obj.update),
                    "rejection": _rejection_to_dict(obj.rejection),
                }
                for obj in self.objects
            ],
        }
        if self.measured_tcp is not None:
            payload["measuredTcp"] = _measured_tcp_to_dict(
                self.measured_tcp,
                age_s=self.measured_tcp_age_s,
            )
        return payload


@dataclass(frozen=True)
class PoseSourcePollResult:
    """Result of one bounded sidecar poll."""

    pose_source_id: str
    status: str
    monotonic_s: float
    updates: tuple[PoseSourceUpdate, ...] = ()
    rejections: tuple[PoseSourceRejection, ...] = ()
    measured_tcp_update: PoseSourceMeasuredTcpUpdate | None = None
    source_errors: tuple[str, ...] = ()


class PoseSourceLatestState:
    """Thread-safe latest-wins pose-source state.

    The state intentionally stores at most one accepted update and one latest
    rejection per object.  There is no frame queue and therefore no stale backlog
    for downstream Composer/UI consumers to drain.
    """

    def __init__(
        self,
        *,
        pose_source_id: str,
        object_ids: Iterable[str],
        stale_after_s: float,
        measured_tcp_enabled: bool = False,
    ) -> None:
        if stale_after_s <= 0.0:
            raise ValueError("stale_after_s must be > 0")
        self.pose_source_id = pose_source_id
        self.object_ids = tuple(object_ids)
        self.stale_after_s = float(stale_after_s)
        self.measured_tcp_enabled = bool(measured_tcp_enabled)
        self._updates: dict[str, PoseSourceUpdate] = {}
        self._rejections: dict[str, PoseSourceRejection] = {}
        self._measured_tcp: PoseSourceMeasuredTcpUpdate | None = None
        self._lock = threading.Lock()

    def apply_poll_result(self, result: PoseSourcePollResult) -> None:
        if result.pose_source_id != self.pose_source_id:
            raise ValueError(
                f"poll result source '{result.pose_source_id}' does not match "
                f"state source '{self.pose_source_id}'"
            )
        with self._lock:
            for update in result.updates:
                self._updates[update.object_id] = update
                self._rejections.pop(update.object_id, None)
            for rejection in result.rejections:
                self._updates.pop(rejection.object_id, None)
                self._rejections[rejection.object_id] = rejection
            if result.measured_tcp_update is not None:
                self._measured_tcp = result.measured_tcp_update

    def snapshot(self, *, monotonic_s: float | None = None) -> PoseSourceSnapshot:
        now = time.monotonic() if monotonic_s is None else float(monotonic_s)
        objects: list[PoseSourceObjectState] = []
        with self._lock:
            updates = dict(self._updates)
            rejections = dict(self._rejections)
            measured_tcp = self._measured_tcp

        any_fresh = False
        any_rejected = False
        any_stale = False
        for object_id in self.object_ids:
            update = updates.get(object_id)
            rejection = rejections.get(object_id)
            if update is not None:
                age_s = max(0.0, now - update.monotonic_s)
                if age_s <= self.stale_after_s:
                    status = POSE_SOURCE_STATUS_FRESH
                    any_fresh = True
                else:
                    status = POSE_SOURCE_STATUS_STALE
                    any_stale = True
                objects.append(
                    PoseSourceObjectState(
                        object_id=object_id,
                        status=status,
                        update=update,
                        rejection=rejection,
                        age_s=age_s,
                    )
                )
            elif rejection is not None:
                any_rejected = True
                objects.append(
                    PoseSourceObjectState(
                        object_id=object_id,
                        status=POSE_SOURCE_STATUS_REJECTED,
                        rejection=rejection,
                    )
                )
            else:
                any_stale = True
                objects.append(
                    PoseSourceObjectState(
                        object_id=object_id,
                        status=POSE_SOURCE_STATUS_STALE,
                    )
                )

        if any_fresh:
            status = POSE_SOURCE_STATUS_FRESH
        elif any_rejected:
            status = POSE_SOURCE_STATUS_REJECTED
        elif any_stale:
            status = POSE_SOURCE_STATUS_STALE
        else:
            status = POSE_SOURCE_STATUS_DISABLED
        return PoseSourceSnapshot(
            pose_source_id=self.pose_source_id,
            status=status,
            monotonic_s=now,
            objects=tuple(objects),
            measured_tcp=measured_tcp,
            measured_tcp_age_s=(
                max(0.0, now - measured_tcp.monotonic_s)
                if measured_tcp is not None
                else None
            ),
        )


class SimulationInterfacesPoseSource:
    """Bounded latest-wins pose source backed by ``simulation_interfaces``."""

    def __init__(
        self,
        *,
        client: SimulationSceneMirrorClient,
        pose_source_id: str,
        bindings: Iterable[PoseSourceBinding],
        measured_tcp: PoseSourceMeasuredTcpBinding | None = None,
        max_update_hz: float = 4.0,
        stale_after_s: float = 1.0,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        if max_update_hz <= 0.0:
            raise ValueError("max_update_hz must be > 0")
        self.client = client
        self.pose_source_id = pose_source_id
        self.bindings = tuple(bindings)
        self.measured_tcp = measured_tcp
        if not self.bindings:
            raise ValueError("bindings must not be empty")
        for binding in self.bindings:
            if not binding.object_id:
                raise ValueError("binding.object_id must be non-empty")
            if not binding.entity:
                raise ValueError("binding.entity must be non-empty")
        self.max_update_hz = float(max_update_hz)
        self.stale_after_s = float(stale_after_s)
        self.now_fn = now_fn or time.monotonic
        self.state = PoseSourceLatestState(
            pose_source_id=pose_source_id,
            object_ids=[binding.object_id for binding in self.bindings],
            stale_after_s=stale_after_s,
            measured_tcp_enabled=measured_tcp is not None,
        )

    @classmethod
    def from_deployment_config(
        cls,
        *,
        client: SimulationSceneMirrorClient,
        source_config: object,
        now_fn: Callable[[], float] | None = None,
    ) -> "SimulationInterfacesPoseSource":
        """Build from a deployment ``PoseSourceConfig``-like object.

        The method is intentionally duck-typed so ``manyforge_ros`` does not
        depend on ``manyforge_behavior`` at package-install time.
        """

        bindings = []
        objects = getattr(source_config, "objects")
        for object_id, binding in objects.items():
            bindings.append(
                PoseSourceBinding(
                    object_id=getattr(binding, "object_id", object_id),
                    entity=getattr(binding, "entity"),
                    apply_policy=getattr(binding, "apply_policy", "observe_only"),
                    frame_id=getattr(binding, "frame_id", ""),
                    confidence_min=getattr(binding, "confidence_min", None),
                )
            )
        measured_tcp_raw = getattr(source_config, "measured_tcp", None)
        measured_tcp = None
        if measured_tcp_raw is not None:
            measured_tcp = PoseSourceMeasuredTcpBinding(
                entity=getattr(measured_tcp_raw, "entity"),
                tcp_frame_id=getattr(measured_tcp_raw, "tcp_frame_id"),
                frame_id=getattr(measured_tcp_raw, "frame_id", ""),
            )
        return cls(
            client=client,
            pose_source_id=getattr(source_config, "source_id"),
            bindings=bindings,
            measured_tcp=measured_tcp,
            max_update_hz=getattr(source_config, "max_update_hz", 4.0),
            stale_after_s=getattr(source_config, "stale_after_s", 1.0),
            now_fn=now_fn,
        )

    @property
    def period_s(self) -> float:
        return 1.0 / self.max_update_hz

    def poll_once(self) -> PoseSourcePollResult:
        now = self.now_fn()
        entity_names = [binding.entity for binding in self.bindings]
        if self.measured_tcp is not None:
            entity_names.append(self.measured_tcp.entity)
        entity_names = list(dict.fromkeys(entity_names))
        updates: list[PoseSourceUpdate] = []
        rejections: list[PoseSourceRejection] = []
        measured_tcp_update: PoseSourceMeasuredTcpUpdate | None = None
        source_errors: tuple[str, ...] = ()
        try:
            snapshot = self.client.capture(
                entity_names=entity_names,
                include_info=False,
                include_state=True,
                include_bounds=False,
            )
            source_errors = snapshot.errors
            by_entity = {entity.name: entity for entity in snapshot.entities}
            for binding in self.bindings:
                entity = by_entity.get(binding.entity)
                if entity is None:
                    rejections.append(
                        self._reject(
                            binding,
                            reason="missing_entity",
                            detail="entity missing from simulation snapshot",
                            monotonic_s=now,
                        )
                    )
                    continue
                if entity.errors:
                    rejections.append(
                        self._reject(
                            binding,
                            reason="source_error",
                            detail="; ".join(entity.errors),
                            monotonic_s=now,
                        )
                    )
                    continue
                if entity.pose is None:
                    rejections.append(
                        self._reject(
                            binding,
                            reason="pose_missing",
                            detail="entity state did not include pose",
                            monotonic_s=now,
                        )
                    )
                    continue
                updates.append(
                    PoseSourceUpdate(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        pose=entity.pose,
                        monotonic_s=now,
                        apply_policy=binding.apply_policy,
                    )
                )
            if self.measured_tcp is not None:
                entity = by_entity.get(self.measured_tcp.entity)
                if entity is None:
                    source_errors = source_errors + (
                        "measured_tcp missing from simulation snapshot",
                    )
                elif entity.errors:
                    source_errors = source_errors + (
                        f"measured_tcp source error: {'; '.join(entity.errors)}",
                    )
                elif entity.pose is None:
                    source_errors = source_errors + (
                        "measured_tcp entity state did not include pose",
                    )
                else:
                    measured_tcp_update = PoseSourceMeasuredTcpUpdate(
                        pose_source_id=self.pose_source_id,
                        entity=self.measured_tcp.entity,
                        tcp_frame_id=self.measured_tcp.tcp_frame_id,
                        frame_id=self.measured_tcp.frame_id or entity.pose.frame_id,
                        pose=entity.pose,
                        monotonic_s=now,
                    )
        except Exception as exc:  # pragma: no cover - exercised through ROS errors.
            source_errors = (str(exc),)
            for binding in self.bindings:
                rejections.append(
                    self._reject(
                        binding,
                        reason="source_error",
                        detail=str(exc),
                        monotonic_s=now,
                    )
                )

        if updates:
            status = POSE_SOURCE_STATUS_FRESH
        elif rejections:
            status = POSE_SOURCE_STATUS_REJECTED
        else:
            status = POSE_SOURCE_STATUS_STALE
        result = PoseSourcePollResult(
            pose_source_id=self.pose_source_id,
            status=status,
            monotonic_s=now,
            updates=tuple(updates),
            rejections=tuple(rejections),
            measured_tcp_update=measured_tcp_update,
            source_errors=source_errors,
        )
        self.state.apply_poll_result(result)
        return result

    def poll_forever(
        self,
        *,
        on_result: Callable[[PoseSourcePollResult], None] | None = None,
        stop_event: threading.Event | None = None,
        max_iterations: int | None = None,
    ) -> None:
        """Run a bounded polling loop.

        The callback is invoked synchronously.  If it is slow, the loop skips
        sleep and continues with the latest simulator state; it never queues
        stale poll results behind the callback.
        """

        iterations = 0
        next_deadline = self.now_fn()
        while stop_event is None or not stop_event.is_set():
            if max_iterations is not None and iterations >= max_iterations:
                return
            result = self.poll_once()
            if on_result is not None:
                on_result(result)
            iterations += 1
            next_deadline += self.period_s
            delay_s = max(0.0, next_deadline - self.now_fn())
            if delay_s > 0.0:
                if stop_event is not None:
                    stop_event.wait(delay_s)
                else:
                    time.sleep(delay_s)

    def _reject(
        self,
        binding: PoseSourceBinding,
        *,
        reason: str,
        detail: str,
        monotonic_s: float,
    ) -> PoseSourceRejection:
        return PoseSourceRejection(
            pose_source_id=self.pose_source_id,
            object_id=binding.object_id,
            entity=binding.entity,
            reason=reason,
            detail=detail,
            monotonic_s=monotonic_s,
        )


class PoseStampedTopicPoseSource:
    """Latest-wins source backed by one ``geometry_msgs/PoseStamped`` topic.

    A plain ``PoseStamped`` message does not carry an object identifier, so this
    source intentionally supports exactly one configured object binding.  Multi-
    object topic shapes should use a message type with explicit association,
    such as ``Detection3DArray`` or a ManyForge-specific pose update message.
    """

    def __init__(
        self,
        *,
        node: Any,
        pose_source_id: str,
        binding: PoseSourceBinding,
        topic: str,
        max_update_hz: float = 30.0,
        stale_after_s: float = 1.0,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        if not pose_source_id:
            raise ValueError("pose_source_id must be non-empty")
        if not topic:
            raise ValueError("topic must be non-empty")
        if max_update_hz <= 0.0:
            raise ValueError("max_update_hz must be > 0")
        if not binding.object_id:
            raise ValueError("binding.object_id must be non-empty")
        self.node = node
        self.pose_source_id = pose_source_id
        self.binding = binding
        self.topic = topic
        self.max_update_hz = float(max_update_hz)
        self.stale_after_s = float(stale_after_s)
        self.now_fn = now_fn or time.monotonic
        self._last_emit_monotonic_s: float | None = None
        self.state = PoseSourceLatestState(
            pose_source_id=pose_source_id,
            object_ids=[binding.object_id],
            stale_after_s=stale_after_s,
        )

        from geometry_msgs.msg import PoseStamped
        from rclpy.qos import QoSProfile

        # Depth one keeps the sidecar bounded even if the downstream Composer
        # snapshot consumer is temporarily slower than the publisher.
        self._subscription = node.create_subscription(
            PoseStamped,
            topic,
            self._on_pose_stamped,
            QoSProfile(depth=1),
        )

    @property
    def period_s(self) -> float:
        return 1.0 / self.max_update_hz

    @classmethod
    def from_deployment_config(
        cls,
        *,
        node: Any,
        source_config: object,
        now_fn: Callable[[], float] | None = None,
    ) -> "PoseStampedTopicPoseSource":
        """Build from a deployment ``PoseSourceConfig``-like object."""

        if getattr(source_config, "kind") != "pose_stamped_topic":
            raise ValueError("source_config.kind must be pose_stamped_topic")
        topic = getattr(source_config, "topic")
        objects = getattr(source_config, "objects")
        if len(objects) != 1:
            raise ValueError("pose_stamped_topic sources require exactly one object")
        object_id, binding_raw = next(iter(objects.items()))
        fallback_frame_id = getattr(binding_raw, "frame_id", "") or getattr(
            source_config,
            "frame_id",
            "",
        )
        binding = PoseSourceBinding(
            object_id=getattr(binding_raw, "object_id", object_id),
            entity=topic,
            apply_policy=getattr(binding_raw, "apply_policy", "observe_only"),
            frame_id=fallback_frame_id,
            confidence_min=getattr(binding_raw, "confidence_min", None),
        )
        return cls(
            node=node,
            pose_source_id=getattr(source_config, "source_id"),
            binding=binding,
            topic=topic,
            max_update_hz=getattr(source_config, "max_update_hz", 30.0),
            stale_after_s=getattr(source_config, "stale_after_s", 1.0),
            now_fn=now_fn,
        )

    def snapshot(self, *, monotonic_s: float | None = None) -> PoseSourceSnapshot:
        return self.state.snapshot(monotonic_s=monotonic_s)

    def close(self) -> None:
        self.node.destroy_subscription(self._subscription)

    def _on_pose_stamped(self, msg: Any) -> None:
        now = self.now_fn()
        if (
            self._last_emit_monotonic_s is not None
            and now - self._last_emit_monotonic_s < self.period_s
        ):
            return
        self._last_emit_monotonic_s = now
        try:
            pose = _pose_stamped_msg_to_snapshot(
                msg,
                fallback_frame_id=self.binding.frame_id,
            )
        except ValueError as exc:
            result = PoseSourcePollResult(
                pose_source_id=self.pose_source_id,
                status=POSE_SOURCE_STATUS_REJECTED,
                monotonic_s=now,
                rejections=(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=self.binding.object_id,
                        entity=self.topic,
                        reason="invalid_pose",
                        detail=str(exc),
                        monotonic_s=now,
                    ),
                ),
            )
            self.state.apply_poll_result(result)
            return
        result = PoseSourcePollResult(
            pose_source_id=self.pose_source_id,
            status=POSE_SOURCE_STATUS_FRESH,
            monotonic_s=now,
            updates=(
                PoseSourceUpdate(
                    pose_source_id=self.pose_source_id,
                    object_id=self.binding.object_id,
                    entity=self.topic,
                    pose=pose,
                    monotonic_s=now,
                    apply_policy=self.binding.apply_policy,
                ),
            ),
        )
        self.state.apply_poll_result(result)


@dataclass(frozen=True)
class _PoseArrayObjectBinding:
    binding: PoseSourceBinding
    pose_index: int


class PoseArrayTopicPoseSource:
    """Latest-wins source backed by one ``geometry_msgs/PoseArray`` topic.

    ``PoseArray`` has no object identifiers or confidences, so object
    association is exclusively deployment-owned: each object binding must carry
    a unique ``metadata.pose_index`` value.  Missing indexes or malformed poses
    reject only the affected object while fresh indexes can still update.
    """

    def __init__(
        self,
        *,
        node: Any,
        pose_source_id: str,
        bindings: Iterable[_PoseArrayObjectBinding],
        topic: str,
        frame_id: str = "",
        max_update_hz: float = 30.0,
        stale_after_s: float = 1.0,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        if not pose_source_id:
            raise ValueError("pose_source_id must be non-empty")
        if not topic:
            raise ValueError("topic must be non-empty")
        if max_update_hz <= 0.0:
            raise ValueError("max_update_hz must be > 0")
        self.node = node
        self.pose_source_id = pose_source_id
        self.bindings = tuple(bindings)
        self.topic = topic
        self.frame_id = frame_id
        self.max_update_hz = float(max_update_hz)
        self.stale_after_s = float(stale_after_s)
        self.now_fn = now_fn or time.monotonic
        self._last_emit_monotonic_s: float | None = None
        if not self.bindings:
            raise ValueError("bindings must not be empty")
        seen: dict[int, str] = {}
        for entry in self.bindings:
            if not entry.binding.object_id:
                raise ValueError("binding.object_id must be non-empty")
            if entry.pose_index < 0:
                raise ValueError("pose_index must be non-negative")
            previous = seen.get(entry.pose_index)
            if previous is not None:
                raise ValueError(
                    f"pose_index {entry.pose_index} is used by both "
                    f"'{previous}' and '{entry.binding.object_id}'"
                )
            seen[entry.pose_index] = entry.binding.object_id
        self.state = PoseSourceLatestState(
            pose_source_id=pose_source_id,
            object_ids=[entry.binding.object_id for entry in self.bindings],
            stale_after_s=stale_after_s,
        )

        from geometry_msgs.msg import PoseArray
        from rclpy.qos import QoSProfile

        self._subscription = node.create_subscription(
            PoseArray,
            topic,
            self._on_pose_array,
            QoSProfile(depth=1),
        )

    @property
    def period_s(self) -> float:
        return 1.0 / self.max_update_hz

    @classmethod
    def from_deployment_config(
        cls,
        *,
        node: Any,
        source_config: object,
        now_fn: Callable[[], float] | None = None,
    ) -> "PoseArrayTopicPoseSource":
        """Build from a deployment ``PoseSourceConfig``-like object."""

        if getattr(source_config, "kind") != "pose_array_topic":
            raise ValueError("source_config.kind must be pose_array_topic")
        topic = getattr(source_config, "topic")
        source_frame_id = getattr(source_config, "frame_id", "")
        entries: list[_PoseArrayObjectBinding] = []
        for object_id, binding_raw in getattr(source_config, "objects").items():
            metadata = getattr(binding_raw, "metadata", {}) or {}
            pose_index = metadata.get("pose_index")
            if not isinstance(pose_index, int) or isinstance(pose_index, bool):
                raise ValueError(
                    f"pose_array_topic object '{object_id}' requires integer metadata.pose_index"
                )
            fallback_frame_id = getattr(binding_raw, "frame_id", "") or source_frame_id
            entries.append(
                _PoseArrayObjectBinding(
                    binding=PoseSourceBinding(
                        object_id=getattr(binding_raw, "object_id", object_id),
                        entity=f"{topic}[{pose_index}]",
                        apply_policy=getattr(
                            binding_raw,
                            "apply_policy",
                            "observe_only",
                        ),
                        frame_id=fallback_frame_id,
                        confidence_min=getattr(binding_raw, "confidence_min", None),
                    ),
                    pose_index=pose_index,
                )
            )
        return cls(
            node=node,
            pose_source_id=getattr(source_config, "source_id"),
            bindings=entries,
            topic=topic,
            frame_id=source_frame_id,
            max_update_hz=getattr(source_config, "max_update_hz", 30.0),
            stale_after_s=getattr(source_config, "stale_after_s", 1.0),
            now_fn=now_fn,
        )

    def snapshot(self, *, monotonic_s: float | None = None) -> PoseSourceSnapshot:
        return self.state.snapshot(monotonic_s=monotonic_s)

    def close(self) -> None:
        self.node.destroy_subscription(self._subscription)

    def _on_pose_array(self, msg: Any) -> None:
        now = self.now_fn()
        if (
            self._last_emit_monotonic_s is not None
            and now - self._last_emit_monotonic_s < self.period_s
        ):
            return
        self._last_emit_monotonic_s = now
        poses = list(getattr(msg, "poses", []) or [])
        header = getattr(msg, "header", None)
        updates: list[PoseSourceUpdate] = []
        rejections: list[PoseSourceRejection] = []
        for entry in self.bindings:
            binding = entry.binding
            if entry.pose_index >= len(poses):
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="pose_index_missing",
                        detail=(
                            f"PoseArray has {len(poses)} pose(s); "
                            f"index {entry.pose_index} is unavailable"
                        ),
                        monotonic_s=now,
                    )
                )
                continue
            try:
                pose = _pose_msg_to_snapshot(
                    poses[entry.pose_index],
                    header=header,
                    fallback_frame_id=binding.frame_id or self.frame_id,
                )
            except ValueError as exc:
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="invalid_pose",
                        detail=str(exc),
                        monotonic_s=now,
                    )
                )
                continue
            updates.append(
                PoseSourceUpdate(
                    pose_source_id=self.pose_source_id,
                    object_id=binding.object_id,
                    entity=binding.entity,
                    pose=pose,
                    monotonic_s=now,
                    apply_policy=binding.apply_policy,
                )
            )
        status = (
            POSE_SOURCE_STATUS_FRESH
            if updates
            else POSE_SOURCE_STATUS_REJECTED
            if rejections
            else POSE_SOURCE_STATUS_STALE
        )
        self.state.apply_poll_result(
            PoseSourcePollResult(
                pose_source_id=self.pose_source_id,
                status=status,
                monotonic_s=now,
                updates=tuple(updates),
                rejections=tuple(rejections),
            )
        )


@dataclass(frozen=True)
class _Detection3DObjectBinding:
    binding: PoseSourceBinding
    detection_id: str = ""
    class_id: str = ""
    max_position_variance_m2: float | None = None


@dataclass(frozen=True)
class _Detection3DMatch:
    detection_id: str
    class_id: str
    score: float
    pose_with_covariance: Any
    header: Any


class Detection3DArrayTopicPoseSource:
    """Latest-wins source backed by ``vision_msgs/Detection3DArray``.

    Object association is deployment-owned.  Each object binding must name
    exactly one detection id or class id.  Confidence gates use
    ``ObjectHypothesis.score``; covariance gates use the selected
    ``PoseWithCovariance`` position diagonal.
    """

    def __init__(
        self,
        *,
        node: Any,
        pose_source_id: str,
        bindings: Iterable[_Detection3DObjectBinding],
        topic: str,
        frame_id: str = "",
        max_update_hz: float = 30.0,
        stale_after_s: float = 1.0,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        if not pose_source_id:
            raise ValueError("pose_source_id must be non-empty")
        if not topic:
            raise ValueError("topic must be non-empty")
        if max_update_hz <= 0.0:
            raise ValueError("max_update_hz must be > 0")
        self.node = node
        self.pose_source_id = pose_source_id
        self.bindings = tuple(bindings)
        self.topic = topic
        self.frame_id = frame_id
        self.max_update_hz = float(max_update_hz)
        self.stale_after_s = float(stale_after_s)
        self.now_fn = now_fn or time.monotonic
        self._last_emit_monotonic_s: float | None = None
        if not self.bindings:
            raise ValueError("bindings must not be empty")
        for entry in self.bindings:
            if not entry.binding.object_id:
                raise ValueError("binding.object_id must be non-empty")
            if bool(entry.detection_id) == bool(entry.class_id):
                raise ValueError(
                    "detection bindings require exactly one detection_id or class_id"
                )
        self.state = PoseSourceLatestState(
            pose_source_id=pose_source_id,
            object_ids=[entry.binding.object_id for entry in self.bindings],
            stale_after_s=stale_after_s,
        )

        from rclpy.qos import QoSProfile
        from vision_msgs.msg import Detection3DArray

        self._subscription = node.create_subscription(
            Detection3DArray,
            topic,
            self._on_detection_array,
            QoSProfile(depth=1),
        )

    @property
    def period_s(self) -> float:
        return 1.0 / self.max_update_hz

    @classmethod
    def from_deployment_config(
        cls,
        *,
        node: Any,
        source_config: object,
        now_fn: Callable[[], float] | None = None,
    ) -> "Detection3DArrayTopicPoseSource":
        """Build from a deployment ``PoseSourceConfig``-like object."""

        if getattr(source_config, "kind") != "detection3d_array_topic":
            raise ValueError("source_config.kind must be detection3d_array_topic")
        topic = getattr(source_config, "topic")
        source_frame_id = getattr(source_config, "frame_id", "")
        entries: list[_Detection3DObjectBinding] = []
        for object_id, binding_raw in getattr(source_config, "objects").items():
            metadata = getattr(binding_raw, "metadata", {}) or {}
            detection_id = str(metadata.get("detection_id") or "").strip()
            class_id = str(metadata.get("class_id") or "").strip()
            max_position_variance_raw = metadata.get("max_position_variance_m2")
            max_position_variance_m2 = (
                float(max_position_variance_raw)
                if max_position_variance_raw is not None
                else None
            )
            fallback_frame_id = getattr(binding_raw, "frame_id", "") or source_frame_id
            entries.append(
                _Detection3DObjectBinding(
                    binding=PoseSourceBinding(
                        object_id=getattr(binding_raw, "object_id", object_id),
                        entity=detection_id or class_id,
                        apply_policy=getattr(
                            binding_raw,
                            "apply_policy",
                            "observe_only",
                        ),
                        frame_id=fallback_frame_id,
                        confidence_min=getattr(binding_raw, "confidence_min", None),
                    ),
                    detection_id=detection_id,
                    class_id=class_id,
                    max_position_variance_m2=max_position_variance_m2,
                )
            )
        return cls(
            node=node,
            pose_source_id=getattr(source_config, "source_id"),
            bindings=entries,
            topic=topic,
            frame_id=source_frame_id,
            max_update_hz=getattr(source_config, "max_update_hz", 30.0),
            stale_after_s=getattr(source_config, "stale_after_s", 1.0),
            now_fn=now_fn,
        )

    def snapshot(self, *, monotonic_s: float | None = None) -> PoseSourceSnapshot:
        return self.state.snapshot(monotonic_s=monotonic_s)

    def close(self) -> None:
        self.node.destroy_subscription(self._subscription)

    def _on_detection_array(self, msg: Any) -> None:
        now = self.now_fn()
        if (
            self._last_emit_monotonic_s is not None
            and now - self._last_emit_monotonic_s < self.period_s
        ):
            return
        self._last_emit_monotonic_s = now
        detections = list(getattr(msg, "detections", []) or [])
        array_header = getattr(msg, "header", None)
        updates: list[PoseSourceUpdate] = []
        rejections: list[PoseSourceRejection] = []
        for entry in self.bindings:
            binding = entry.binding
            match = _select_detection3d_match(detections, entry)
            if match is None:
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="detection_not_found",
                        detail="no detection matched configured association",
                        monotonic_s=now,
                    )
                )
                continue
            if binding.confidence_min is not None and match.score < binding.confidence_min:
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="confidence_below_min",
                        detail=(
                            f"score {match.score:.6g} < "
                            f"confidence_min {binding.confidence_min:.6g}"
                        ),
                        monotonic_s=now,
                    )
                )
                continue
            try:
                covariance_max = _pose_position_covariance_max(
                    match.pose_with_covariance,
                )
            except ValueError as exc:
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="invalid_covariance",
                        detail=str(exc),
                        monotonic_s=now,
                    )
                )
                continue
            if (
                entry.max_position_variance_m2 is not None
                and covariance_max > entry.max_position_variance_m2
            ):
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="covariance_exceeded",
                        detail=(
                            f"position covariance {covariance_max:.6g} > "
                            f"max_position_variance_m2 "
                            f"{entry.max_position_variance_m2:.6g}"
                        ),
                        monotonic_s=now,
                    )
                )
                continue
            try:
                pose = _pose_msg_to_snapshot(
                    getattr(match.pose_with_covariance, "pose", None),
                    header=_preferred_header(match.header, array_header),
                    fallback_frame_id=binding.frame_id or self.frame_id,
                )
            except ValueError as exc:
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="invalid_pose",
                        detail=str(exc),
                        monotonic_s=now,
                    )
                )
                continue
            updates.append(
                PoseSourceUpdate(
                    pose_source_id=self.pose_source_id,
                    object_id=binding.object_id,
                    entity=binding.entity,
                    pose=pose,
                    monotonic_s=now,
                    apply_policy=binding.apply_policy,
                    metadata={
                        "detectionId": match.detection_id,
                        "classId": match.class_id,
                        "confidence": match.score,
                        "maxPositionVarianceM2": covariance_max,
                    },
                )
            )
        status = (
            POSE_SOURCE_STATUS_FRESH
            if updates
            else POSE_SOURCE_STATUS_REJECTED
            if rejections
            else POSE_SOURCE_STATUS_STALE
        )
        self.state.apply_poll_result(
            PoseSourcePollResult(
                pose_source_id=self.pose_source_id,
                status=status,
                monotonic_s=now,
                updates=tuple(updates),
                rejections=tuple(rejections),
            )
        )


class TfPoseSource:
    """Latest-wins source backed by ROS TF frame pairs.

    Each object binding maps one target frame to one source frame: the source
    frame is stored in ``binding.entity`` and the target frame is stored in
    ``binding.frame_id``.  Deployment validation requires a target frame either
    per object or at the source level.
    """

    def __init__(
        self,
        *,
        node: Any,
        pose_source_id: str,
        bindings: Iterable[PoseSourceBinding],
        max_update_hz: float = 30.0,
        stale_after_s: float = 1.0,
        buffer: Any | None = None,
        listener: Any | None = None,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        if not pose_source_id:
            raise ValueError("pose_source_id must be non-empty")
        if max_update_hz <= 0.0:
            raise ValueError("max_update_hz must be > 0")
        self.node = node
        self.pose_source_id = pose_source_id
        self.bindings = tuple(bindings)
        self.max_update_hz = float(max_update_hz)
        self.stale_after_s = float(stale_after_s)
        self.now_fn = now_fn or time.monotonic
        if not self.bindings:
            raise ValueError("bindings must not be empty")
        for binding in self.bindings:
            if not binding.object_id:
                raise ValueError("binding.object_id must be non-empty")
            if not binding.entity:
                raise ValueError("binding.entity must be non-empty")
            if not binding.frame_id:
                raise ValueError("binding.frame_id must be non-empty")

        if buffer is None:
            from tf2_ros import Buffer, TransformListener

            buffer = Buffer()
            listener = TransformListener(buffer, node)
        self.buffer = buffer
        self.listener = listener
        self.state = PoseSourceLatestState(
            pose_source_id=pose_source_id,
            object_ids=[binding.object_id for binding in self.bindings],
            stale_after_s=stale_after_s,
        )

    @property
    def period_s(self) -> float:
        return 1.0 / self.max_update_hz

    @classmethod
    def from_deployment_config(
        cls,
        *,
        node: Any,
        source_config: object,
        buffer: Any | None = None,
        listener: Any | None = None,
        now_fn: Callable[[], float] | None = None,
    ) -> "TfPoseSource":
        """Build from a deployment ``PoseSourceConfig``-like object."""

        if getattr(source_config, "kind") != "tf":
            raise ValueError("source_config.kind must be tf")
        source_frame_id = getattr(source_config, "frame_id", "")
        bindings = []
        for object_id, binding_raw in getattr(source_config, "objects").items():
            target_frame = getattr(binding_raw, "frame_id", "") or source_frame_id
            bindings.append(
                PoseSourceBinding(
                    object_id=getattr(binding_raw, "object_id", object_id),
                    entity=getattr(binding_raw, "entity"),
                    apply_policy=getattr(binding_raw, "apply_policy", "observe_only"),
                    frame_id=target_frame,
                )
            )
        return cls(
            node=node,
            pose_source_id=getattr(source_config, "source_id"),
            bindings=bindings,
            max_update_hz=getattr(source_config, "max_update_hz", 30.0),
            stale_after_s=getattr(source_config, "stale_after_s", 1.0),
            buffer=buffer,
            listener=listener,
            now_fn=now_fn,
        )

    def poll_once(self) -> PoseSourcePollResult:
        now = self.now_fn()
        updates: list[PoseSourceUpdate] = []
        rejections: list[PoseSourceRejection] = []
        for binding in self.bindings:
            try:
                transform = self.buffer.lookup_transform(
                    binding.frame_id,
                    binding.entity,
                    _tf_lookup_time(),
                )
                pose = _transform_stamped_to_snapshot(transform)
            except Exception as exc:
                rejections.append(
                    PoseSourceRejection(
                        pose_source_id=self.pose_source_id,
                        object_id=binding.object_id,
                        entity=binding.entity,
                        reason="tf_lookup_failed",
                        detail=str(exc),
                        monotonic_s=now,
                    )
                )
                continue
            updates.append(
                PoseSourceUpdate(
                    pose_source_id=self.pose_source_id,
                    object_id=binding.object_id,
                    entity=binding.entity,
                    pose=pose,
                    monotonic_s=now,
                    apply_policy=binding.apply_policy,
                )
            )
        status = (
            POSE_SOURCE_STATUS_FRESH
            if updates
            else POSE_SOURCE_STATUS_REJECTED
            if rejections
            else POSE_SOURCE_STATUS_STALE
        )
        result = PoseSourcePollResult(
            pose_source_id=self.pose_source_id,
            status=status,
            monotonic_s=now,
            updates=tuple(updates),
            rejections=tuple(rejections),
        )
        self.state.apply_poll_result(result)
        return result


def _update_to_dict(update: PoseSourceUpdate | None) -> dict[str, Any] | None:
    if update is None:
        return None
    return {
        "poseSourceId": update.pose_source_id,
        "objectId": update.object_id,
        "entity": update.entity,
        "monotonicS": update.monotonic_s,
        "applyPolicy": update.apply_policy,
        "metadata": dict(update.metadata),
        "pose": {
            "frameId": update.pose.frame_id,
            "stampSec": update.pose.stamp_sec,
            "positionXYZ": list(update.pose.position_xyz),
            "orientationXYZW": list(update.pose.orientation_xyzw),
        },
    }


def _measured_tcp_to_dict(
    update: PoseSourceMeasuredTcpUpdate,
    *,
    age_s: float | None,
) -> dict[str, Any]:
    return {
        "frameId": update.frame_id or update.pose.frame_id,
        "tcpFrameId": update.tcp_frame_id,
        "monotonicS": update.monotonic_s,
        "ageS": age_s,
        "pose": {
            "frameId": update.frame_id or update.pose.frame_id,
            "stampSec": update.pose.stamp_sec,
            "positionXYZ": list(update.pose.position_xyz),
            "orientationXYZW": list(update.pose.orientation_xyzw),
        },
    }


def _rejection_to_dict(
    rejection: PoseSourceRejection | None,
) -> dict[str, Any] | None:
    if rejection is None:
        return None
    return {
        "poseSourceId": rejection.pose_source_id,
        "objectId": rejection.object_id,
        "entity": rejection.entity,
        "reason": rejection.reason,
        "detail": rejection.detail,
        "monotonicS": rejection.monotonic_s,
    }


def _pose_stamped_msg_to_snapshot(
    msg: Any,
    *,
    fallback_frame_id: str,
) -> PoseSnapshot:
    header = getattr(msg, "header", None)
    return _pose_msg_to_snapshot(
        getattr(msg, "pose", None),
        header=header,
        fallback_frame_id=fallback_frame_id,
    )


def _pose_msg_to_snapshot(
    pose: Any,
    *,
    header: Any,
    fallback_frame_id: str,
) -> PoseSnapshot:
    frame_id = str(getattr(header, "frame_id", "") or fallback_frame_id)
    if not frame_id:
        raise ValueError("pose header.frame_id is empty")
    position = getattr(pose, "position", None)
    orientation = getattr(pose, "orientation", None)
    if position is None or orientation is None:
        raise ValueError("pose is incomplete")
    position_xyz = (
        float(getattr(position, "x", float("nan"))),
        float(getattr(position, "y", float("nan"))),
        float(getattr(position, "z", float("nan"))),
    )
    orientation_xyzw_raw = (
        float(getattr(orientation, "x", float("nan"))),
        float(getattr(orientation, "y", float("nan"))),
        float(getattr(orientation, "z", float("nan"))),
        float(getattr(orientation, "w", float("nan"))),
    )
    if not all(isfinite(value) for value in position_xyz + orientation_xyzw_raw):
        raise ValueError("pose contains non-finite pose values")
    norm = sqrt(sum(value * value for value in orientation_xyzw_raw))
    if norm <= 1e-9:
        raise ValueError("pose orientation quaternion has zero norm")
    orientation_xyzw = tuple(value / norm for value in orientation_xyzw_raw)
    return PoseSnapshot(
        frame_id=frame_id,
        stamp_sec=_stamp_to_float(getattr(header, "stamp", None)),
        position_xyz=position_xyz,
        orientation_xyzw=orientation_xyzw,  # type: ignore[arg-type]
    )


def _stamp_to_float(stamp: Any) -> float | None:
    if stamp is None:
        return None
    sec = getattr(stamp, "sec", None)
    nanosec = getattr(stamp, "nanosec", None)
    if sec is None or nanosec is None:
        return None
    return float(sec) + float(nanosec) * 1e-9


def _select_detection3d_match(
    detections: Iterable[Any],
    entry: _Detection3DObjectBinding,
) -> _Detection3DMatch | None:
    best: _Detection3DMatch | None = None
    for detection in detections:
        detection_id = str(getattr(detection, "id", "") or "")
        if entry.detection_id and detection_id != entry.detection_id:
            continue
        for result in getattr(detection, "results", []) or []:
            hypothesis = getattr(result, "hypothesis", None)
            class_id = str(getattr(hypothesis, "class_id", "") or "")
            if entry.class_id and class_id != entry.class_id:
                continue
            try:
                score = float(getattr(hypothesis, "score"))
            except (TypeError, ValueError):
                continue
            if not isfinite(score):
                continue
            candidate = _Detection3DMatch(
                detection_id=detection_id,
                class_id=class_id,
                score=score,
                pose_with_covariance=getattr(result, "pose", None),
                header=getattr(detection, "header", None),
            )
            if best is None or candidate.score > best.score:
                best = candidate
    return best


def _pose_position_covariance_max(pose_with_covariance: Any) -> float:
    covariance_raw = getattr(pose_with_covariance, "covariance", None)
    covariance = [] if covariance_raw is None else list(covariance_raw)
    if len(covariance) != 36:
        raise ValueError("pose covariance must contain 36 values")
    diagonal = [float(covariance[index]) for index in (0, 7, 14)]
    if not all(isfinite(value) for value in diagonal):
        raise ValueError("pose covariance position diagonal contains non-finite values")
    if any(value < 0.0 for value in diagonal):
        raise ValueError("pose covariance position diagonal contains negative values")
    return max(diagonal)


def _preferred_header(primary: Any, fallback: Any) -> Any:
    primary_stamp = getattr(primary, "stamp", None)
    has_primary_stamp = bool(
        getattr(primary_stamp, "sec", 0) or getattr(primary_stamp, "nanosec", 0)
    )
    if str(getattr(primary, "frame_id", "") or "") or has_primary_stamp:
        return primary
    return fallback


def _tf_lookup_time() -> Any:
    from rclpy.time import Time

    return Time()


def _transform_stamped_to_snapshot(transform: Any) -> PoseSnapshot:
    header = getattr(transform, "header", None)
    tf = getattr(transform, "transform", None)
    translation = getattr(tf, "translation", None)
    rotation = getattr(tf, "rotation", None)
    if translation is None or rotation is None:
        raise ValueError("TransformStamped transform is incomplete")
    position_xyz = (
        float(getattr(translation, "x", float("nan"))),
        float(getattr(translation, "y", float("nan"))),
        float(getattr(translation, "z", float("nan"))),
    )
    orientation_xyzw_raw = (
        float(getattr(rotation, "x", float("nan"))),
        float(getattr(rotation, "y", float("nan"))),
        float(getattr(rotation, "z", float("nan"))),
        float(getattr(rotation, "w", float("nan"))),
    )
    if not all(isfinite(value) for value in position_xyz + orientation_xyzw_raw):
        raise ValueError("TransformStamped contains non-finite pose values")
    norm = sqrt(sum(value * value for value in orientation_xyzw_raw))
    if norm <= 1e-9:
        raise ValueError("TransformStamped orientation quaternion has zero norm")
    orientation_xyzw = tuple(value / norm for value in orientation_xyzw_raw)
    frame_id = str(getattr(header, "frame_id", "") or "")
    if not frame_id:
        raise ValueError("TransformStamped header.frame_id is empty")
    return PoseSnapshot(
        frame_id=frame_id,
        stamp_sec=_stamp_to_float(getattr(header, "stamp", None)),
        position_xyz=position_xyz,
        orientation_xyzw=orientation_xyzw,  # type: ignore[arg-type]
    )
