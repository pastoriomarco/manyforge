"""One bounded render-only projection of execution-owner joint observations."""

from __future__ import annotations

import asyncio
import json
import math
import threading
import time
from dataclasses import dataclass
from typing import Any, Callable, Final, Mapping

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


OBSERVATION_FRAME_SCHEMA: Final = "manyforge.execution_observation_frame.v1"
OBSERVATION_CREDIT_SCHEMA: Final = "manyforge.execution_observation_credit.v1"
OBSERVATION_DEMAND_LEASE_SECONDS: Final = 45.0
MAXIMUM_OBSERVATION_CLIENTS: Final = 16
_MAXIMUM_SAFE_INTEGER: Final = 9_007_199_254_740_991
_MAXIMUM_JOINTS: Final = 1_024


class ExecutionObservationProjectionError(ValueError):
    """The execution owner returned an invalid render observation."""


def _object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ExecutionObservationProjectionError(f"{path} must be an object")
    return value


def _text(value: Any, path: str) -> str:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 2_048:
        raise ExecutionObservationProjectionError(
            f"{path} must be bounded non-empty text"
        )
    return value


def _safe_integer(value: Any, path: str) -> int:
    if (
        type(value) is not int
        or value < 0
        or value > _MAXIMUM_SAFE_INTEGER
    ):
        raise ExecutionObservationProjectionError(
            f"{path} must be a non-negative safe integer"
        )
    return value


def _finite_number(value: Any, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ExecutionObservationProjectionError(f"{path} must be numeric")
    result = float(value)
    if not math.isfinite(result):
        raise ExecutionObservationProjectionError(f"{path} must be finite")
    return result


def _text_array(value: Any, path: str) -> list[str]:
    if not isinstance(value, list) or not 0 < len(value) <= _MAXIMUM_JOINTS:
        raise ExecutionObservationProjectionError(
            f"{path} must contain 1..{_MAXIMUM_JOINTS} entries"
        )
    result = [_text(item, f"{path}[{index}]") for index, item in enumerate(value)]
    if len(set(result)) != len(result):
        raise ExecutionObservationProjectionError(f"{path} contains duplicates")
    return result


def project_execution_observation(
    response: dict[str, Any],
    *,
    received_at_monotonic_us: int,
) -> dict[str, Any]:
    """Project one exact render frame from the narrow owner observation."""

    if set(response) != {"api_version", "command", "observation", "ok"} or (
        response.get("api_version") != "manyforge.execution_command_response.v1"
        or response.get("command") != "observation"
        or response.get("ok") is not True
    ):
        raise ExecutionObservationProjectionError(
            "execution observation response identity is invalid"
        )
    observed = _object(response.get("observation"), "observation")
    if set(observed) != {
        "cell_id",
        "command_target_id",
        "compiled_joint_names",
        "controllable_component_id",
        "graph_revision",
        "owner_epoch",
        "positions_rad",
        "retained_bundle_id",
        "schema_version",
        "source_epoch",
        "source_joint_names",
        "source_sequence",
    } or observed.get("schema_version") != "manyforge.execution_observation.v1":
        raise ExecutionObservationProjectionError(
            "execution observation schema is unsupported"
        )
    source_joint_names = _text_array(
        observed.get("source_joint_names"),
        "observation.source_joint_names",
    )
    compiled_joint_names = _text_array(
        observed.get("compiled_joint_names"),
        "observation.compiled_joint_names",
    )
    if len(source_joint_names) != len(compiled_joint_names):
        raise ExecutionObservationProjectionError(
            "source and compiled joint orders differ"
        )
    raw_positions = observed.get("positions_rad")
    if not isinstance(raw_positions, list):
        raise ExecutionObservationProjectionError(
            "observation.positions_rad must be an array"
        )
    positions = [
        _finite_number(item, f"positions_rad[{index}]")
        for index, item in enumerate(raw_positions)
    ]
    if len(source_joint_names) != len(positions):
        raise ExecutionObservationProjectionError(
            "observed joint positions differ from the retained joint order"
        )
    received = _safe_integer(
        received_at_monotonic_us, "received_at_monotonic_us"
    )
    return {
        "schema_version": OBSERVATION_FRAME_SCHEMA,
        "owner_epoch": _text(observed.get("owner_epoch"), "observation.owner_epoch"),
        "cell_id": _text(observed.get("cell_id"), "observation.cell_id"),
        "retained_bundle_id": _text(
            observed.get("retained_bundle_id"), "observation.retained_bundle_id"
        ),
        "graph_revision": _text(
            observed.get("graph_revision"), "observation.graph_revision"
        ),
        "controllable_component_id": _text(
            observed.get("controllable_component_id"),
            "observation.controllable_component_id",
        ),
        "command_target_id": _text(
            observed.get("command_target_id"),
            "observation.command_target_id",
        ),
        "source_epoch": _text(
            observed.get("source_epoch"),
            "observation.source_epoch",
        ),
        "source_sequence": _safe_integer(
            observed.get("source_sequence"),
            "observation.source_sequence",
        ),
        "received_at_monotonic_us": received,
        "source_joint_names": source_joint_names,
        "compiled_joint_names": compiled_joint_names,
        "positions_rad": positions,
    }


def encode_execution_observation_frame(frame: dict[str, Any]) -> str:
    """Encode one validated frame once for immutable multi-client fanout."""

    return json.dumps(
        frame,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )


def _render_signature(frame: dict[str, Any]) -> tuple[Any, ...]:
    return (
        frame["owner_epoch"],
        frame["cell_id"],
        frame["retained_bundle_id"],
        frame["graph_revision"],
        frame["controllable_component_id"],
        frame["command_target_id"],
        frame["source_epoch"],
        tuple(frame["source_joint_names"]),
        tuple(frame["compiled_joint_names"]),
        tuple(frame["positions_rad"]),
    )


@dataclass(eq=False)
class ExecutionObservationSubscription:
    wake: asyncio.Event
    payload: str | None = None
    last_applied: tuple[str, str, int] | None = None
    outstanding: tuple[str, str, int] | None = None
    credit: bool = False
    closed: bool = False


def observation_identity(frame: Mapping[str, Any]) -> tuple[str, str, int]:
    return (
        str(frame["owner_epoch"]),
        str(frame["source_epoch"]),
        int(frame["source_sequence"]),
    )


def observation_cursor(
    identity: tuple[str, str, int] | None,
) -> dict[str, Any] | None:
    if identity is None:
        return None
    return {
        "owner_epoch": identity[0],
        "source_epoch": identity[1],
        "source_sequence": identity[2],
    }


def _is_newer(
    candidate: tuple[str, str, int],
    applied: tuple[str, str, int] | None,
) -> bool:
    return applied is None or candidate[:2] != applied[:2] or candidate[2] > applied[2]


class ExecutionObservationSource:
    """One change-driven source and one applied-frame credit per browser."""

    def __init__(
        self,
        client: HeadlessHttpClient,
        *,
        monotonic: Callable[[], float] = time.monotonic,
    ) -> None:
        self._client = client
        self._monotonic = monotonic
        self._subscriptions: set[ExecutionObservationSubscription] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._condition = threading.Condition()
        self._last_payload: str | None = None
        self._last_identity: tuple[str, str, int] | None = None
        self._last_render_signature: tuple[Any, ...] | None = None
        self._worker: threading.Thread | None = None
        self._shutdown = False
        self._active_upstream_cancel: threading.Event | None = None
        self._demand_generation = 0
        self._retry_after_generation = -1
        self._connections = 0
        self._source_wakes = 0
        self._published = 0
        self._credits = 0
        self._sent = 0
        self._coalesced = 0
        self._errors = 0
        self._last_wake_duration_us = 0
        self._max_wake_duration_us = 0
        self._last_error: dict[str, str] | None = None

    def subscribe(self) -> ExecutionObservationSubscription:
        loop = asyncio.get_running_loop()
        if self._loop is not None and self._loop is not loop:
            if self._subscriptions:
                raise RuntimeError("execution observation source crossed event loops")
        self._loop = loop
        subscription = ExecutionObservationSubscription(asyncio.Event())
        with self._condition:
            if len(self._subscriptions) >= MAXIMUM_OBSERVATION_CLIENTS:
                raise RuntimeError("execution observation client limit reached")
            self._subscriptions.add(subscription)
        return subscription

    def unsubscribe(self, subscription: ExecutionObservationSubscription) -> None:
        with self._condition:
            subscription.closed = True
            subscription.payload = None
            subscription.credit = False
            self._subscriptions.discard(subscription)
            if not self._subscriptions:
                # Reconnect must read the current owner frame, never replay a
                # cached frame from the preceding demand interval.
                self._last_payload = None
                self._last_identity = None
                self._last_render_signature = None
                if self._active_upstream_cancel is not None:
                    self._active_upstream_cancel.set()
            self._demand_generation += 1
            self._condition.notify_all()

    def close(self) -> None:
        with self._condition:
            self._shutdown = True
            for subscription in self._subscriptions:
                subscription.closed = True
            self._subscriptions.clear()
            if self._active_upstream_cancel is not None:
                self._active_upstream_cancel.set()
            self._condition.notify_all()
            worker = self._worker
        if worker is not None and worker is not threading.current_thread():
            worker.join(timeout=2.0)

    def grant_credit(
        self,
        subscription: ExecutionObservationSubscription,
        last_applied: tuple[str, str, int] | None,
    ) -> None:
        wakes: list[ExecutionObservationSubscription] = []
        with self._condition:
            if subscription.closed or subscription not in self._subscriptions:
                raise ValueError("execution observation subscription is closed")
            if subscription.outstanding is not None:
                if last_applied == subscription.outstanding:
                    subscription.last_applied = last_applied
                    subscription.outstanding = None
                elif last_applied == subscription.last_applied:
                    # A coarse visible-idle renewal cannot create another
                    # credit while the preceding frame is still unapplied.
                    self._credits += 1
                    return
                else:
                    raise ValueError("execution observation acknowledgement is stale")
            elif subscription.last_applied is None:
                if last_applied is not None:
                    raise ValueError("initial execution observation credit has a cursor")
            elif last_applied != subscription.last_applied:
                raise ValueError("execution observation renewal cursor is stale")
            subscription.credit = True
            self._credits += 1
            self._demand_generation += 1
            if self._offer_current_locked(subscription):
                wakes.append(subscription)
            self._ensure_worker_locked()
            self._condition.notify_all()
        self._wake_subscriptions(wakes)

    def take_payload(
        self,
        subscription: ExecutionObservationSubscription,
    ) -> str | None:
        with self._condition:
            payload = subscription.payload
            subscription.payload = None
            subscription.wake.clear()
            if payload is not None:
                self._sent += 1
            return payload

    def status(self) -> dict[str, Any]:
        with self._condition:
            last_error = (
                None if self._last_error is None else dict(self._last_error)
            )
            return {
                "schema_version": "manyforge.execution_observation_stream_status.v2",
                "source": {
                    "active": self._active_upstream_cancel is not None,
                    "mode": "change_driven",
                    "connection_count": self._connections,
                    "wake_count": self._source_wakes,
                    "published_count": self._published,
                    "error_count": self._errors,
                    "last_wake_duration_us": self._last_wake_duration_us,
                    "max_wake_duration_us": self._max_wake_duration_us,
                    "last_error": last_error,
                    "periodic_http_requests": 0,
                },
                "fanout": {
                    "client_count": len(self._subscriptions),
                    "capacity_per_client": 1,
                    "credit_count": self._credits,
                    "sent_count": self._sent,
                    "coalesced_frames_total": self._coalesced,
                },
            }

    def _ensure_worker_locked(self) -> None:
        if self._worker is not None and self._worker.is_alive():
            return
        self._worker = threading.Thread(
            target=self._run_worker,
            name="manyforge-viewer-observation",
            daemon=True,
        )
        self._worker.start()

    def _has_demand_locked(self) -> bool:
        return any(
            not item.closed and item.credit and item.outstanding is None
            for item in self._subscriptions
        )

    def _offer_current_locked(
        self,
        subscription: ExecutionObservationSubscription,
    ) -> bool:
        if (
            not subscription.credit
            or subscription.outstanding is not None
            or self._last_payload is None
            or self._last_identity is None
            or not _is_newer(self._last_identity, subscription.last_applied)
        ):
            return False
        subscription.payload = self._last_payload
        subscription.outstanding = self._last_identity
        subscription.credit = False
        return True

    def _wake_subscriptions(
        self,
        subscriptions: list[ExecutionObservationSubscription],
    ) -> None:
        loop = self._loop
        if loop is None or loop.is_closed() or not subscriptions:
            return

        def wake_open_subscriptions() -> None:
            for subscription in subscriptions:
                if not subscription.closed:
                    subscription.wake.set()

        try:
            loop.call_soon_threadsafe(wake_open_subscriptions)
        except RuntimeError:
            return

    def _admit_response(self, response: dict[str, Any]) -> None:
        frame = project_execution_observation(
            response,
            received_at_monotonic_us=int(self._monotonic() * 1_000_000),
        )
        identity = observation_identity(frame)
        signature = _render_signature(frame)
        wakes: list[ExecutionObservationSubscription] = []
        with self._condition:
            # The last unsubscribe cancels the in-flight owner wait. If the
            # response raced that cancellation, discard it: a later first
            # demand must read the owner's current frame, never this interval's
            # cached result.
            if not self._subscriptions:
                return
            if (
                self._last_identity is not None
                and identity[:2] == self._last_identity[:2]
                and identity[2] < self._last_identity[2]
            ):
                raise ExecutionObservationProjectionError(
                    "execution observation sequence regressed within one source epoch"
                )
            if (
                self._last_identity == identity
                and self._last_render_signature is not None
                and signature != self._last_render_signature
            ):
                raise ExecutionObservationProjectionError(
                    "execution observation changed without advancing its source sequence"
                )
            if self._last_identity == identity:
                self._coalesced += 1
                return
            payload = encode_execution_observation_frame(frame)
            if (
                self._last_identity is not None
                and identity[:2] == self._last_identity[:2]
                and signature == self._last_render_signature
            ):
                # Advance the exact current cursor for reconnect/new demand,
                # but do not wake an already-credited browser for a visually
                # identical freshness publication.
                self._last_identity = identity
                self._last_payload = payload
                self._coalesced += 1
                self._last_error = None
                self._condition.notify_all()
                return
            self._last_identity = identity
            self._last_render_signature = signature
            self._last_payload = payload
            self._published += 1
            self._last_error = None
            for subscription in self._subscriptions:
                if self._offer_current_locked(subscription):
                    wakes.append(subscription)
            self._condition.notify_all()
        self._wake_subscriptions(wakes)

    def _run_worker(self) -> None:
        while True:
            with self._condition:
                self._condition.wait_for(
                    lambda: self._shutdown
                    or (
                        self._has_demand_locked()
                        and self._demand_generation > self._retry_after_generation
                    )
                )
                if self._shutdown:
                    return
                cancel = threading.Event()
                self._active_upstream_cancel = cancel
                self._connections += 1
            failed = False
            try:
                with self._client.open_execution_observation_stream() as upstream:
                    while not cancel.is_set():
                        with self._condition:
                            self._condition.wait_for(
                                lambda: self._shutdown
                                or cancel.is_set()
                                or self._has_demand_locked()
                            )
                            if self._shutdown or cancel.is_set():
                                break
                            cursor = observation_cursor(self._last_identity)
                        started = self._monotonic()
                        response = upstream.next(cursor, cancel)
                        if cancel.is_set():
                            break
                        elapsed_us = max(
                            0,
                            round((self._monotonic() - started) * 1_000_000),
                        )
                        with self._condition:
                            self._source_wakes += 1
                            self._last_wake_duration_us = elapsed_us
                            self._max_wake_duration_us = max(
                                self._max_wake_duration_us,
                                elapsed_us,
                            )
                        self._admit_response(response)
            except (HeadlessClientError, ExecutionObservationProjectionError) as error:
                failed = not cancel.is_set()
                if failed:
                    with self._condition:
                        self._errors += 1
                        self._last_error = {
                            "code": getattr(
                                error,
                                "code",
                                "execution_observation_projection_invalid",
                            ),
                            "message": str(error)[:512],
                        }
            except Exception as error:
                failed = not cancel.is_set()
                if failed:
                    with self._condition:
                        self._errors += 1
                        self._last_error = {
                            "code": "execution_observation_source_failed",
                            "message": str(error)[:512],
                        }
            finally:
                with self._condition:
                    if self._active_upstream_cancel is cancel:
                        self._active_upstream_cancel = None
                    if failed:
                        # No timer-driven retry. The next explicit browser
                        # credit/renewal advances demand and permits reconnect.
                        self._retry_after_generation = self._demand_generation
                    self._condition.notify_all()


__all__ = [
    "ExecutionObservationProjectionError",
    "ExecutionObservationSource",
    "ExecutionObservationSubscription",
    "MAXIMUM_OBSERVATION_CLIENTS",
    "OBSERVATION_CREDIT_SCHEMA",
    "OBSERVATION_DEMAND_LEASE_SECONDS",
    "OBSERVATION_FRAME_SCHEMA",
    "encode_execution_observation_frame",
    "observation_cursor",
    "observation_identity",
    "project_execution_observation",
]
