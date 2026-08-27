"""Complete read-only snapshots over thin owner projections."""

from __future__ import annotations

import asyncio
import json
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient

from .authoring_admission import AuthoringAdmissionCoordinator
from .draft_session import ComposerDraftSession


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
) -> None:
    """Publish fresh owner projections independently for one browser."""

    await websocket.accept()
    sequence = 0
    last_error_message: str | None = None
    last_projection_key: tuple[Any, ...] | None = None
    try:
        while True:
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
                if projection_key == last_projection_key:
                    await asyncio.sleep(POLL_INTERVAL_SECONDS)
                    continue
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
            await asyncio.sleep(POLL_INTERVAL_SECONDS)
    except (WebSocketDisconnect, asyncio.TimeoutError):
        pass


__all__ = ["serve_headless_state"]
