"""Bounded ROS object-pose source sidecars.

This module implements the first runtime slice for the deployment
``pose_sources`` contract: read-only polling of explicitly configured simulator
entities via ROS 2 ``simulation_interfaces``.  It does not mutate Composer or
kernel scene state.  Later slices consume the same latest-wins snapshots for
observe-only UI status and safe planning-scene apply policies.
"""

from __future__ import annotations

import threading
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
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
class PoseSourceUpdate:
    """Accepted pose update for a bound object."""

    pose_source_id: str
    object_id: str
    entity: str
    pose: PoseSnapshot
    monotonic_s: float
    apply_policy: str


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

    def to_dict(self) -> dict[str, Any]:
        return {
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


@dataclass(frozen=True)
class PoseSourcePollResult:
    """Result of one bounded sidecar poll."""

    pose_source_id: str
    status: str
    monotonic_s: float
    updates: tuple[PoseSourceUpdate, ...] = ()
    rejections: tuple[PoseSourceRejection, ...] = ()
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
    ) -> None:
        if stale_after_s <= 0.0:
            raise ValueError("stale_after_s must be > 0")
        self.pose_source_id = pose_source_id
        self.object_ids = tuple(object_ids)
        self.stale_after_s = float(stale_after_s)
        self._updates: dict[str, PoseSourceUpdate] = {}
        self._rejections: dict[str, PoseSourceRejection] = {}
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
                self._rejections[rejection.object_id] = rejection

    def snapshot(self, *, monotonic_s: float | None = None) -> PoseSourceSnapshot:
        now = time.monotonic() if monotonic_s is None else float(monotonic_s)
        objects: list[PoseSourceObjectState] = []
        with self._lock:
            updates = dict(self._updates)
            rejections = dict(self._rejections)

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
        )


class SimulationInterfacesPoseSource:
    """Bounded latest-wins pose source backed by ``simulation_interfaces``."""

    def __init__(
        self,
        *,
        client: SimulationSceneMirrorClient,
        pose_source_id: str,
        bindings: Iterable[PoseSourceBinding],
        max_update_hz: float = 4.0,
        stale_after_s: float = 1.0,
        now_fn: Callable[[], float] | None = None,
    ) -> None:
        if max_update_hz <= 0.0:
            raise ValueError("max_update_hz must be > 0")
        self.client = client
        self.pose_source_id = pose_source_id
        self.bindings = tuple(bindings)
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
        return cls(
            client=client,
            pose_source_id=getattr(source_config, "source_id"),
            bindings=bindings,
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
        updates: list[PoseSourceUpdate] = []
        rejections: list[PoseSourceRejection] = []
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


def _update_to_dict(update: PoseSourceUpdate | None) -> dict[str, Any] | None:
    if update is None:
        return None
    return {
        "poseSourceId": update.pose_source_id,
        "objectId": update.object_id,
        "entity": update.entity,
        "monotonicS": update.monotonic_s,
        "applyPolicy": update.apply_policy,
        "pose": {
            "frameId": update.pose.frame_id,
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
