"""One bounded render-only projection of execution-owner joint observations."""

from __future__ import annotations

import asyncio
import math
import time
from dataclasses import dataclass
from typing import Any, Final

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


OBSERVATION_FRAME_SCHEMA: Final = "manyforge.execution_observation_frame.v1"
OBSERVATION_RATE_HZ: Final = 30.0
_OBSERVATION_PERIOD_S: Final = 1.0 / OBSERVATION_RATE_HZ
_OBSERVATION_ERROR_PERIOD_S: Final = 0.25
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
    queue: asyncio.Queue[dict[str, Any]]


class ExecutionObservationSource:
    """Demand-bound source with one replaceable latest slot per client."""

    def __init__(self, client: HeadlessHttpClient) -> None:
        self._client = client
        self._subscriptions: set[ExecutionObservationSubscription] = set()
        self._task: asyncio.Task[None] | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._last_render_signature: tuple[Any, ...] | None = None
        self._last_frame: dict[str, Any] | None = None
        self._last_source_identity: tuple[str, str] | None = None
        self._last_source_sequence: int | None = None
        self._polls = 0
        self._published = 0
        self._replaced = 0
        self._errors = 0
        self._last_poll_duration_us = 0
        self._max_poll_duration_us = 0
        self._last_error: dict[str, str] | None = None

    def subscribe(self) -> ExecutionObservationSubscription:
        loop = asyncio.get_running_loop()
        if self._loop is not None and self._loop is not loop:
            if self._subscriptions:
                raise RuntimeError("execution observation source crossed event loops")
            self._task = None
        self._loop = loop
        subscription = ExecutionObservationSubscription(asyncio.Queue(maxsize=1))
        self._subscriptions.add(subscription)
        if self._last_frame is not None:
            subscription.queue.put_nowait(self._last_frame)
        if self._task is None or self._task.done():
            self._task = loop.create_task(self._run())
        return subscription

    def unsubscribe(self, subscription: ExecutionObservationSubscription) -> None:
        self._subscriptions.discard(subscription)
        if not self._subscriptions and self._task is not None:
            self._task.cancel()
            self._task = None
            self._last_render_signature = None
            self._last_frame = None
            self._last_source_identity = None
            self._last_source_sequence = None

    def status(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.execution_observation_stream_status.v1",
            "source": {
                "active": self._task is not None and not self._task.done(),
                "rate_limit_hz": OBSERVATION_RATE_HZ,
                "poll_count": self._polls,
                "published_count": self._published,
                "error_count": self._errors,
                "last_poll_duration_us": self._last_poll_duration_us,
                "max_poll_duration_us": self._max_poll_duration_us,
                "last_error": (
                    None if self._last_error is None else dict(self._last_error)
                ),
            },
            "fanout": {
                "client_count": len(self._subscriptions),
                "capacity_per_client": 1,
                "replaced_frames_total": self._replaced,
            },
        }

    async def _run(self) -> None:
        try:
            while self._subscriptions:
                started = time.monotonic()
                failed = False
                try:
                    self._polls += 1
                    response = await asyncio.to_thread(
                        self._client.execution_observation
                    )
                    frame = project_execution_observation(
                        response,
                        received_at_monotonic_us=int(time.monotonic() * 1_000_000),
                    )
                    source_identity = (frame["owner_epoch"], frame["source_epoch"])
                    source_sequence = frame["source_sequence"]
                    if (
                        self._last_source_identity == source_identity
                        and self._last_source_sequence is not None
                        and source_sequence < self._last_source_sequence
                    ):
                        raise ExecutionObservationProjectionError(
                            "execution observation sequence regressed within one source epoch"
                        )
                    signature = _render_signature(frame)
                    if (
                        self._last_source_identity == source_identity
                        and self._last_source_sequence == source_sequence
                        and self._last_render_signature is not None
                        and signature != self._last_render_signature
                    ):
                        raise ExecutionObservationProjectionError(
                            "execution observation changed without advancing its source sequence"
                        )
                    self._last_source_identity = source_identity
                    self._last_source_sequence = source_sequence
                    if signature != self._last_render_signature:
                        self._last_render_signature = signature
                        self._publish(frame)
                    self._last_error = None
                except (HeadlessClientError, ExecutionObservationProjectionError) as error:
                    failed = True
                    self._errors += 1
                    self._last_error = {
                        "code": getattr(
                            error,
                            "code",
                            "execution_observation_projection_invalid",
                        ),
                        "message": str(error)[:512],
                    }
                except Exception as error:  # keep this optional presentation source alive
                    failed = True
                    self._errors += 1
                    self._last_error = {
                        "code": "execution_observation_source_failed",
                        "message": str(error)[:512],
                    }
                elapsed = time.monotonic() - started
                elapsed_us = max(0, round(elapsed * 1_000_000))
                self._last_poll_duration_us = elapsed_us
                self._max_poll_duration_us = max(
                    self._max_poll_duration_us, elapsed_us
                )
                period = (
                    _OBSERVATION_ERROR_PERIOD_S
                    if failed
                    else _OBSERVATION_PERIOD_S
                )
                await asyncio.sleep(max(0.0, period - elapsed))
        except asyncio.CancelledError:
            pass

    def _publish(self, frame: dict[str, Any]) -> None:
        self._published += 1
        self._last_frame = frame
        for subscription in tuple(self._subscriptions):
            if subscription.queue.full():
                try:
                    subscription.queue.get_nowait()
                    self._replaced += 1
                except asyncio.QueueEmpty:
                    pass
            subscription.queue.put_nowait(frame)


__all__ = [
    "ExecutionObservationProjectionError",
    "ExecutionObservationSource",
    "ExecutionObservationSubscription",
    "OBSERVATION_FRAME_SCHEMA",
    "OBSERVATION_RATE_HZ",
    "project_execution_observation",
]
