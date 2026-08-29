"""Complete read-only snapshots over thin owner projections."""

from __future__ import annotations

import asyncio
import json
import time
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient

from .authoring_admission import AuthoringAdmissionCoordinator
from .draft_session import ComposerDraftSession
from .execution_observation_stream import ExecutionObservationSource


POLL_INTERVAL_SECONDS = 0.25
SEND_TIMEOUT_SECONDS = 0.25
ALARM_PAGE_LIMIT = 32
MAXIMUM_MESSAGE_BYTES = 16 * 1024 * 1024


class _ProjectionOversized(ValueError):
    code = "headless_websocket_projection_oversized"


def _encode(value: dict[str, Any]) -> str:
    encoded = json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    )
    if len(encoded.encode("utf-8")) > MAXIMUM_MESSAGE_BYTES:
        raise _ProjectionOversized(
            "headless WebSocket projection exceeds its byte bound"
        )
    return encoded


def _projection_key(
    runtime: dict[str, Any],
    alarms: dict[str, Any],
    authoring: dict[str, Any],
    cycle: dict[str, Any],
    draft: dict[str, Any],
) -> tuple[Any, ...]:
    """Return the owner revisions that can change the browser projection."""

    return (
        runtime.get("projection_owner_epoch"),
        runtime.get("projection_generation"),
        alarms.get("owner_epoch"),
        alarms.get("generation"),
        alarms.get("state_revision"),
        authoring.get("authoring_allowed"),
        authoring.get("admission_generation"),
        cycle.get("program_runner_owner_epoch"),
        cycle.get("cycle_generation"),
        cycle.get("state"),
        cycle.get("program_run_id"),
        cycle.get("cycle_claim_generation"),
        cycle.get("terminal_or_settlement_reference"),
        cycle.get("runtime_control_revision"),
        (
            cycle.get("recovery_offer", {}).get("offer_id")
            if isinstance(cycle.get("recovery_offer"), dict)
            else None
        ),
        (
            cycle.get("recovery_offer", {}).get("offer_revision")
            if isinstance(cycle.get("recovery_offer"), dict)
            else None
        ),
        draft.get("draft_id"),
        draft.get("draft_revision"),
    )


async def serve_headless_state(
    websocket: WebSocket,
    client: HeadlessHttpClient,
    *,
    admission: AuthoringAdmissionCoordinator,
    draft_session: ComposerDraftSession,
    observation_source: ExecutionObservationSource | None = None,
) -> None:
    """Publish fresh owner projections independently for one browser."""

    await websocket.accept()
    sequence = 0
    last_error_message: str | None = None
    last_projection_key: tuple[Any, ...] | None = None
    observation_subscription = (
        observation_source.subscribe() if observation_source is not None else None
    )
    next_shell_at = 0.0
    try:
        while True:
            now = time.monotonic()
            if now >= next_shell_at:
                next_shell_at = now + POLL_INTERVAL_SECONDS
                try:
                    if not admission.mutation_ready:
                        raise ValueError("authoring recovery is not mutation-ready")
                    runtime, alarms, authoring, cycle = await asyncio.gather(
                        asyncio.to_thread(client.runtime),
                        asyncio.to_thread(
                            client.alarms,
                            limit=ALARM_PAGE_LIMIT,
                            ordering="attention_newest",
                        ),
                        asyncio.to_thread(client.authoring_admission),
                        asyncio.to_thread(client.program_cycle_status),
                    )
                    draft = draft_session.snapshot()
                    projection_key = _projection_key(
                        runtime, alarms, authoring, cycle, draft
                    )
                    if projection_key != last_projection_key:
                        sequence += 1
                        payload = {
                            "schema_version": "manyforge.composer_headless_state.v1",
                            "runtime": runtime,
                            "alarms": alarms,
                            "authoring_admission": authoring,
                            "program_cycle": cycle,
                            "draft_identity": {
                                "draft_id": draft["draft_id"],
                                "draft_revision": draft["draft_revision"],
                            },
                            "receiver_epoch": admission.receiver_epoch,
                            "snapshot_sequence": sequence,
                            "complete": True,
                        }
                        encoded = _encode(payload)
                        await asyncio.wait_for(
                            websocket.send_text(encoded),
                            timeout=SEND_TIMEOUT_SECONDS,
                        )
                        last_projection_key = projection_key
                    last_error_message = None
                except (HeadlessClientError, KeyError, TypeError, ValueError) as error:
                    last_projection_key = None
                    error_message = _encode(
                        {
                            "schema_version": (
                                "manyforge.composer_headless_state.v1"
                            ),
                            "runtime": None,
                            "alarms": None,
                            "error": {
                                "code": getattr(
                                    error,
                                    "code",
                                    "headless_websocket_projection_invalid",
                                ),
                                "message": str(error),
                            },
                        }
                    )
                    if error_message != last_error_message:
                        await asyncio.wait_for(
                            websocket.send_text(error_message),
                            timeout=SEND_TIMEOUT_SECONDS,
                        )
                        last_error_message = error_message

            delay = max(0.0, next_shell_at - time.monotonic())
            if observation_subscription is None:
                await asyncio.sleep(delay)
                continue
            try:
                frame = await asyncio.wait_for(
                    observation_subscription.queue.get(),
                    timeout=delay,
                )
            except asyncio.TimeoutError:
                continue
            await asyncio.wait_for(
                websocket.send_text(_encode(frame)),
                timeout=SEND_TIMEOUT_SECONDS,
            )
    except (WebSocketDisconnect, asyncio.TimeoutError):
        pass
    finally:
        if observation_source is not None and observation_subscription is not None:
            observation_source.unsubscribe(observation_subscription)


__all__ = ["serve_headless_state"]
