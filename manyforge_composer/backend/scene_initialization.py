"""Generation-fenced scene initialization and latest-observation buffering."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import threading
import time
from typing import Any, Callable, Mapping
import uuid


@dataclass(frozen=True)
class SceneInitializationReplayBatch:
    objects: dict[tuple[str, str], dict[str, Any]]
    requests: dict[str, dict[str, Any]]


class SceneInitializationBarrier:
    """Own one runtime-generation initialization handshake.

    The class owns only bounded NRT lifecycle state. ComposerState performs
    scene I/O after taking a replay batch, so no backend call runs under this
    lock.
    """

    _PENDING_STATES = frozenset(
        {"closed", "resetting", "seeding", "replaying", "failed"}
    )

    def __init__(
        self,
        *,
        now_ns: Callable[[], int] = time.monotonic_ns,
        token_factory: Callable[[], str] | None = None,
    ) -> None:
        self._now_ns = now_ns
        self._token_factory = token_factory or (
            lambda: f"scene-init-{uuid.uuid4().hex}"
        )
        self._lock = threading.RLock()
        started_at_ns = now_ns()
        self._generation = 0
        self._token: str | None = None
        self._state = "open"
        self._deferred = False
        self._reason = "startup"
        self._error: str | None = None
        self._started_at_ns = started_at_ns
        self._updated_at_ns = started_at_ns
        self._objects: dict[tuple[str, str], dict[str, Any]] = {}
        self._requests: dict[str, dict[str, Any]] = {}
        self._counters = {
            "closed": 0,
            "buffered": 0,
            "buffer_replaced": 0,
            "replayed": 0,
            "opened": 0,
            "failed": 0,
            "stale_completion_rejected": 0,
        }

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            return {
                "schemaVersion": "manyforge.scene_initialization.v1",
                "generation": self._generation,
                "token": self._token,
                "state": self._state,
                "deferred": self._deferred,
                "reason": self._reason,
                "error": self._error,
                "startedAtNs": self._started_at_ns,
                "updatedAtNs": self._updated_at_ns,
                "bufferedObjectCount": len(self._objects),
                "bufferedPolicyCount": len(self._requests),
                "counters": dict(self._counters),
            }

    def close(self, *, deferred: bool, reason: str) -> dict[str, Any]:
        now_ns = self._now_ns()
        token = self._token_factory()
        if not token or len(token) > 128:
            raise RuntimeError("scene initialization token factory returned invalid token")
        with self._lock:
            self._generation += 1
            self._token = token
            self._state = "closed"
            self._deferred = bool(deferred)
            self._reason = str(reason or "runtime_cutover")
            self._error = None
            self._started_at_ns = now_ns
            self._updated_at_ns = now_ns
            self._objects.clear()
            self._requests.clear()
            self._counters["closed"] += 1
            return self.snapshot()

    def prepare_reset(self, token: str | None, *, reason: str) -> str | None:
        candidate = str(token or "").strip() or None
        with self._lock:
            if self._state == "open":
                if candidate is not None:
                    self._reject_stale_locked()
                    raise RuntimeError("stale scene initialization token")
                return None
            if candidate is None:
                raise RuntimeError(
                    "scene initialization token is required for this Reset"
                )
            self._require_current_locked(candidate)
            if self._state == "retired":
                self._reject_stale_locked()
                raise RuntimeError("retired scene initialization token")
            self._state = "resetting"
            self._reason = str(reason or "cycle_reset")
            self._error = None
            self._updated_at_ns = self._now_ns()
            return candidate

    def mark_seeded(self, token: str) -> dict[str, Any]:
        with self._lock:
            self._require_current_locked(token)
            if self._state not in {"closed", "resetting", "seeding"}:
                raise RuntimeError(
                    "scene initialization is not awaiting reset/seed completion"
                )
            self._state = "seeding"
            self._updated_at_ns = self._now_ns()
            return self.snapshot()

    def buffer_apply(
        self,
        *,
        source_id: str | None,
        policy: str,
        object_ids: list[str],
        require_idle: bool,
        reason: str,
        snapshots: Mapping[str, dict[str, Any]],
    ) -> dict[str, Any] | None:
        requested = {str(value) for value in object_ids if str(value)}
        source_names = (
            [str(source_id)]
            if source_id and str(source_id) in snapshots
            else list(snapshots)
        )
        candidates: list[tuple[tuple[str, str], dict[str, Any]]] = []
        for candidate_source in source_names:
            snapshot = snapshots.get(candidate_source)
            if not isinstance(snapshot, dict):
                continue
            base = {
                key: deepcopy(value)
                for key, value in snapshot.items()
                if key != "objects"
            }
            for item in snapshot.get("objects", []) or []:
                if not isinstance(item, dict):
                    continue
                object_id = str(item.get("object_id") or "")
                if not object_id or (requested and object_id not in requested):
                    continue
                if str(item.get("apply_policy") or "") != policy:
                    continue
                candidates.append(
                    (
                        (candidate_source, object_id),
                        {
                            "base": base,
                            "object": deepcopy(item),
                            "received_at_ns": int(
                                snapshot.get("received_at_ns") or 0
                            ),
                        },
                    )
                )

        buffered = 0
        replaced = 0
        with self._lock:
            if self._state == "open":
                return None
            if self._state not in self._PENDING_STATES:
                raise RuntimeError(
                    f"scene initialization cannot buffer in state {self._state}"
                )
            for key, record in candidates:
                if key in self._objects:
                    replaced += 1
                self._objects[key] = record
                buffered += 1
            request = self._requests.setdefault(
                policy,
                {
                    "policy": policy,
                    "object_ids": set(),
                    "require_idle": False,
                    "reason": reason,
                },
            )
            request["object_ids"].update(requested)
            request["require_idle"] = bool(
                request.get("require_idle") or require_idle
            )
            request["reason"] = reason or str(request.get("reason") or "")
            self._counters["buffered"] += buffered
            self._counters["buffer_replaced"] += replaced
            self._updated_at_ns = self._now_ns()
            return {
                "success": True,
                "policy": policy,
                "queued": False,
                "buffered_by_initialization": True,
                "scene_initialization_state": self._state,
                "scene_initialization_generation": self._generation,
                "scene_initialization_token": self._token,
                "object_ids": sorted(requested),
                "buffered_count": buffered,
                "replacement_count": replaced,
            }

    def take_replay_batch(self, token: str) -> SceneInitializationReplayBatch:
        with self._lock:
            self._require_current_locked(token)
            if self._state == "open":
                return SceneInitializationReplayBatch({}, {})
            if self._state == "retired":
                raise RuntimeError("retired scene initialization token")
            self._state = "replaying"
            self._updated_at_ns = self._now_ns()
            batch = SceneInitializationReplayBatch(self._objects, self._requests)
            self._objects = {}
            self._requests = {}
            return batch

    def record_replay(self, token: str, *, applied_count: int) -> None:
        with self._lock:
            self._require_current_locked(token)
            self._counters["replayed"] += max(0, int(applied_count))
            self._updated_at_ns = self._now_ns()

    def open_if_no_buffer(self, token: str, *, reason: str) -> dict[str, Any] | None:
        with self._lock:
            self._require_current_locked(token)
            if self._requests:
                return None
            self._state = "open"
            self._deferred = False
            self._reason = str(reason or "initialization")
            self._error = None
            self._updated_at_ns = self._now_ns()
            self._counters["opened"] += 1
            return self.snapshot()

    def restore_failed_batch(
        self,
        token: str,
        batch: SceneInitializationReplayBatch,
        *,
        error: str,
    ) -> dict[str, Any] | None:
        with self._lock:
            if token != self._token:
                return None
            for key, record in batch.objects.items():
                current = self._objects.get(key)
                if current is None or int(record.get("received_at_ns") or 0) > int(
                    current.get("received_at_ns") or 0
                ):
                    self._objects[key] = record
            for policy, request in batch.requests.items():
                current = self._requests.setdefault(
                    policy,
                    {
                        "policy": policy,
                        "object_ids": set(),
                        "require_idle": False,
                        "reason": request.get("reason", ""),
                    },
                )
                current["object_ids"].update(request.get("object_ids", set()))
                current["require_idle"] = bool(
                    current.get("require_idle") or request.get("require_idle")
                )
            self._set_failed_locked(error)
            return self.snapshot()

    def fail(self, token: str | None, *, error: str) -> dict[str, Any] | None:
        candidate = str(token or "").strip()
        if not candidate:
            return None
        with self._lock:
            if candidate != self._token:
                self._reject_stale_locked()
                return None
            if self._state != "failed":
                self._set_failed_locked(error)
            return self.snapshot()

    def _set_failed_locked(self, error: str) -> None:
        self._state = "failed"
        self._error = str(error or "initialization failed")
        self._updated_at_ns = self._now_ns()
        self._counters["failed"] += 1

    def _require_current_locked(self, token: str) -> None:
        if not token or token != self._token:
            self._reject_stale_locked()
            raise RuntimeError("stale scene initialization token")

    def _reject_stale_locked(self) -> None:
        self._counters["stale_completion_rejected"] += 1
