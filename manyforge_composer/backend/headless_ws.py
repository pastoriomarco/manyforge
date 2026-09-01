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

    sections = runtime.get("sections")
    sections = sections if isinstance(sections, dict) else {}
    manager = sections.get("manager")
    manager = manager if isinstance(manager, dict) else {}
    manager_payload = manager.get("payload")
    manager_payload = manager_payload if isinstance(manager_payload, dict) else {}
    active = manager_payload.get("active")
    active = active if isinstance(active, dict) else {}
    manager_runtime = manager_payload.get("runtime")
    manager_runtime = manager_runtime if isinstance(manager_runtime, dict) else {}

    def runtime_units(name: str) -> tuple[str, ...]:
        rows = manager_runtime.get(name)
        if not isinstance(rows, list):
            return ()
        stable_rows = []
        retained_fields = (
            "runtime_unit_id",
            "selected_runtime_contract_offer_digest",
            "observed_runtime_contract_offer_digest",
            "observed_configuration_identity",
            "observed_product_identity",
            "converged",
            "derived_state",
            "runtime_epoch",
            "process_presence",
            "source_health",
            "source_phase",
            "command_surface_state",
            "critical_progress_sequence",
            "reason_codes",
            "ready_runtime_role_ids",
        )
        for row in rows:
            if not isinstance(row, dict):
                continue
            stable_rows.append(
                json.dumps(
                    {field: row.get(field) for field in retained_fields if field in row},
                    sort_keys=True,
                    separators=(",", ":"),
                )
            )
        return tuple(sorted(stable_rows))

    def source_key(name: str, *, include_generation: bool) -> tuple[Any, ...]:
        section = sections.get(name)
        section = section if isinstance(section, dict) else {}
        return (
            section.get("available"),
            section.get("freshness"),
            tuple(section.get("known_gaps", ())),
            section.get("source_owner_id"),
            section.get("source_owner_epoch"),
            section.get("source_generation") if include_generation else None,
            section.get("source_content_digest") if include_generation else None,
        )

    return (
        runtime.get("projection_owner_epoch"),
        runtime.get("projection_owner_id"),
        runtime.get("complete"),
        tuple(runtime.get("reasons", ())),
        tuple(runtime.get("unavailable_sections", ())),
        source_key("manager", include_generation=False),
        active.get("graph_revision"),
        active.get("retained_bundle_id"),
        manager_payload.get("manager_ready"),
        manager_runtime.get("converged"),
        runtime_units("desired_units"),
        runtime_units("observed_units"),
        source_key("logical_scene", include_generation=True),
        source_key("scene_runtime", include_generation=True),
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


def _overview_matches_snapshot(
    overview: dict[str, Any],
    runtime: dict[str, Any],
    alarms: dict[str, Any],
    authoring: dict[str, Any],
    cycle: dict[str, Any],
) -> bool:
    """Reject a reducer result that crossed a meaningful owner revision."""

    if overview.get("schema_version") != "manyforge.component_overview.v1":
        return False
    basis = overview.get("basis")
    sections = runtime.get("sections")
    if not isinstance(basis, dict) or not isinstance(sections, dict):
        return False

    def source(name: str) -> dict[str, Any]:
        value = sections.get(name)
        return value if isinstance(value, dict) else {}

    expected = {
        "runtime_projection_owner_epoch": runtime.get("projection_owner_epoch"),
        "manager_source_owner_epoch": source("manager").get("source_owner_epoch"),
        "logical_scene_source_owner_epoch": source("logical_scene").get(
            "source_owner_epoch"
        ),
        "logical_scene_source_generation": source("logical_scene").get(
            "source_generation"
        ),
        "scene_runtime_source_owner_epoch": source("scene_runtime").get(
            "source_owner_epoch"
        ),
        "scene_runtime_source_generation": source("scene_runtime").get(
            "source_generation"
        ),
        "alarm_owner_epoch": alarms.get("owner_epoch"),
        "alarm_generation": alarms.get("generation"),
        "alarm_state_revision": alarms.get("state_revision"),
        "admission_generation": authoring.get("admission_generation"),
        "program_runner_owner_epoch": cycle.get("program_runner_owner_epoch"),
        "program_cycle_generation": cycle.get("cycle_generation"),
    }
    return all(basis.get(name) == value for name, value in expected.items())


async def serve_headless_state(
    websocket: WebSocket,
    client: HeadlessHttpClient,
    *,
    admission: AuthoringAdmissionCoordinator,
    draft_session: ComposerDraftSession,
) -> None:
    """Publish the low-rate shell/control projection for one browser."""

    await websocket.accept()
    sequence = 0
    last_error_message: str | None = None
    last_projection_key: tuple[Any, ...] | None = None
    next_shell_at = 0.0

    async def send(encoded: str) -> None:
        await asyncio.wait_for(
            websocket.send_text(encoded),
            timeout=SEND_TIMEOUT_SECONDS,
        )

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
                        overview = await asyncio.to_thread(client.component_overview)
                        if not _overview_matches_snapshot(
                            overview, runtime, alarms, authoring, cycle
                        ):
                            raise ValueError(
                                "Component Overview crossed an owner revision"
                            )
                        sequence += 1
                        payload = {
                            "schema_version": "manyforge.composer_headless_state.v2",
                            "runtime": runtime,
                            "alarms": alarms,
                            "component_overview": overview,
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
                        await send(encoded)
                        last_projection_key = projection_key
                    last_error_message = None
                except (HeadlessClientError, KeyError, TypeError, ValueError) as error:
                    last_projection_key = None
                    error_message = _encode(
                        {
                            "schema_version": (
                                "manyforge.composer_headless_state.v2"
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
                        await send(error_message)
                        last_error_message = error_message

            delay = max(0.0, next_shell_at - time.monotonic())
            await asyncio.sleep(delay)
    except (WebSocketDisconnect, asyncio.TimeoutError):
        pass


__all__ = ["serve_headless_state"]
