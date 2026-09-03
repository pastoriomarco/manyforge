"""Complete read-only snapshots over thin owner projections."""

from __future__ import annotations

import asyncio
from contextlib import suppress
from dataclasses import dataclass
import json
from typing import Any

from fastapi import WebSocket, WebSocketDisconnect
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient

from .authoring_admission import AuthoringAdmissionCoordinator
from .draft_session import ComposerDraftSession


# One low-rate read serves every connected browser.  Five reads per second
# keeps multi-owner control settlement responsive without restoring per-client
# polling; the worker still does no work when no browser is subscribed.
POLL_INTERVAL_SECONDS = 0.2
FULL_REFRESH_SECONDS = 1.0
SEND_TIMEOUT_SECONDS = 0.25
ALARM_PAGE_LIMIT = 32
MAXIMUM_MESSAGE_BYTES = 16 * 1024 * 1024
MAXIMUM_SHELL_CLIENTS = 16


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
        cycle.get("run_revision"),
        cycle.get("node_status_revision"),
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


@dataclass(eq=False)
class HeadlessStateSubscription:
    """One capacity-one delivery slot for a browser shell projection."""

    wake: asyncio.Event
    payload: str | None = None
    closed: bool = False


class HeadlessStateSource:
    """Read one shared shell projection while at least one browser is present."""

    def __init__(
        self,
        client: HeadlessHttpClient,
        *,
        admission: AuthoringAdmissionCoordinator,
        draft_session: ComposerDraftSession,
    ) -> None:
        self._client = client
        self._admission = admission
        self._draft_session = draft_session
        self._subscriptions: set[HeadlessStateSubscription] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._control_worker: asyncio.Task[None] | None = None
        self._full_worker: asyncio.Task[None] | None = None
        self._closed = False
        self._sequence = 0
        self._last_payload: str | None = None
        self._last_error_message: str | None = None
        self._last_projection_key: tuple[Any, ...] | None = None
        self._cycle: dict[str, Any] | None = None
        self._cycle_ready: asyncio.Event | None = None
        self._runtime: dict[str, Any] | None = None
        self._alarms: dict[str, Any] | None = None
        self._authoring: dict[str, Any] | None = None
        self._overview: dict[str, Any] | None = None

    def subscribe(self) -> HeadlessStateSubscription:
        loop = asyncio.get_running_loop()
        if self._loop is not None and self._loop is not loop and self._subscriptions:
            raise RuntimeError("headless state source crossed event loops")
        if self._closed:
            raise RuntimeError("headless state source is closed")
        if len(self._subscriptions) >= MAXIMUM_SHELL_CLIENTS:
            raise RuntimeError("headless state client limit reached")
        self._loop = loop
        if not self._subscriptions:
            self._cycle_ready = asyncio.Event()
        subscription = HeadlessStateSubscription(asyncio.Event())
        self._subscriptions.add(subscription)
        if self._last_payload is not None:
            subscription.payload = self._last_payload
            subscription.wake.set()
        if self._control_worker is None or self._control_worker.done():
            worker = loop.create_task(self._run_control())
            worker.add_done_callback(
                lambda task: self._worker_done("control", task)
            )
            self._control_worker = worker
        if self._full_worker is None or self._full_worker.done():
            worker = loop.create_task(self._run_full())
            worker.add_done_callback(lambda task: self._worker_done("full", task))
            self._full_worker = worker
        return subscription

    def unsubscribe(self, subscription: HeadlessStateSubscription) -> None:
        subscription.closed = True
        subscription.payload = None
        subscription.wake.clear()
        self._subscriptions.discard(subscription)
        if self._subscriptions:
            return
        # A later browser must receive a fresh owner read, not a cached shell
        # assembled during a previous demand interval.
        self._last_payload = None
        self._last_error_message = None
        self._last_projection_key = None
        self._cycle = None
        self._cycle_ready = None
        self._runtime = None
        self._alarms = None
        self._authoring = None
        self._overview = None
        workers = (self._control_worker, self._full_worker)
        self._control_worker = None
        self._full_worker = None
        for worker in workers:
            if worker is not None and not worker.done():
                worker.cancel()

    async def close(self) -> None:
        self._closed = True
        subscriptions = tuple(self._subscriptions)
        self._subscriptions.clear()
        for subscription in subscriptions:
            subscription.closed = True
            subscription.payload = None
            subscription.wake.set()
        workers = (self._control_worker, self._full_worker)
        self._control_worker = None
        self._full_worker = None
        for worker in workers:
            if worker is None:
                continue
            if not worker.done():
                worker.cancel()
            with suppress(asyncio.CancelledError, WebSocketDisconnect):
                await worker

    def take_payload(self, subscription: HeadlessStateSubscription) -> str | None:
        payload = subscription.payload
        subscription.payload = None
        subscription.wake.clear()
        return payload

    def current_component_overview(self) -> dict[str, Any] | None:
        """Return the shared complete overview while browser demand is active."""

        if (
            not self._subscriptions
            or self._last_projection_key is None
            or not self._has_complete_snapshot()
        ):
            return None
        return self._overview

    def _worker_done(self, role: str, worker: asyncio.Task[None]) -> None:
        if role == "control" and self._control_worker is worker:
            self._control_worker = None
        if role == "full" and self._full_worker is worker:
            self._full_worker = None
        if worker.cancelled():
            return
        try:
            worker.result()
        except WebSocketDisconnect:
            # Tests and owner disconnects use this as a deterministic source
            # shutdown.  End the sibling reader and every subscriber together.
            sibling = (
                self._full_worker if role == "control" else self._control_worker
            )
            if sibling is not None and not sibling.done():
                sibling.cancel()
            for subscription in self._subscriptions:
                subscription.closed = True
                subscription.wake.set()
        except Exception:
            sibling = (
                self._full_worker if role == "control" else self._control_worker
            )
            if sibling is not None and not sibling.done():
                sibling.cancel()
            for subscription in self._subscriptions:
                subscription.closed = True
                subscription.wake.set()
            raise

    def _publish(self, payload: str) -> None:
        self._last_payload = payload
        for subscription in self._subscriptions:
            if subscription.closed:
                continue
            # Capacity one: a slow browser receives the newest complete shell,
            # never a queue of stale revisions.
            subscription.payload = payload
            subscription.wake.set()

    def _has_complete_snapshot(self) -> bool:
        return all(
            isinstance(value, dict)
            for value in (
                self._runtime,
                self._alarms,
                self._authoring,
                self._overview,
            )
        )

    def _complete_projection(self, cycle: dict[str, Any]) -> str | None:
        if not self._has_complete_snapshot():
            return None
        runtime = self._runtime
        alarms = self._alarms
        authoring = self._authoring
        overview = self._overview
        assert isinstance(runtime, dict)
        assert isinstance(alarms, dict)
        assert isinstance(authoring, dict)
        assert isinstance(overview, dict)
        if not _overview_matches_snapshot(
            overview,
            runtime,
            alarms,
            authoring,
            cycle,
        ):
            return None
        draft = self._draft_session.snapshot()
        projection_key = _projection_key(runtime, alarms, authoring, cycle, draft)
        if projection_key == self._last_projection_key:
            self._last_error_message = None
            return None
        self._sequence += 1
        encoded = _encode(
            {
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
                "receiver_epoch": self._admission.receiver_epoch,
                "snapshot_sequence": self._sequence,
                "complete": True,
            }
        )
        self._last_projection_key = projection_key
        self._last_error_message = None
        return encoded

    def _error_projection(self, error: Exception) -> str | None:
        self._last_projection_key = None
        error_message = _encode(
            {
                "schema_version": "manyforge.composer_headless_state.v2",
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
        if error_message == self._last_error_message:
            return None
        self._last_error_message = error_message
        return error_message

    async def _refresh_control(self) -> str | None:
        try:
            if not self._admission.mutation_ready:
                raise ValueError("authoring recovery is not mutation-ready")
            cycle = await asyncio.to_thread(self._client.program_cycle_status)
            self._cycle = cycle
            if self._cycle_ready is not None:
                self._cycle_ready.set()
            return self._complete_projection(cycle)
        except (HeadlessClientError, KeyError, TypeError, ValueError) as error:
            if (
                self._last_projection_key is not None
                and self._has_complete_snapshot()
            ):
                return None
            return self._error_projection(error)

    async def _refresh_full(self) -> str | None:
        try:
            if not self._admission.mutation_ready:
                raise ValueError("authoring recovery is not mutation-ready")
            if self._cycle is None and self._cycle_ready is not None:
                await self._cycle_ready.wait()
            if self._cycle is None:
                self._cycle = await asyncio.to_thread(
                    self._client.program_cycle_status
                )
            runtime, alarms, authoring = await asyncio.gather(
                asyncio.to_thread(self._client.runtime),
                asyncio.to_thread(
                    self._client.alarms,
                    limit=ALARM_PAGE_LIMIT,
                    ordering="attention_newest",
                ),
                asyncio.to_thread(self._client.authoring_admission),
            )
            cycle = self._cycle
            if not isinstance(cycle, dict):
                raise ValueError("shared shell control projection is unavailable")
            draft = self._draft_session.snapshot()
            candidate_key = _projection_key(
                runtime,
                alarms,
                authoring,
                cycle,
                draft,
            )
            if (
                candidate_key == self._last_projection_key
                and self._has_complete_snapshot()
            ):
                # Component Overview is a cold aggregate. Its inputs are the
                # exact owner revisions above, so do not rebuild it merely
                # because the bounded diagnostic interval elapsed.
                self._runtime = runtime
                self._alarms = alarms
                self._authoring = authoring
                return None
            overview = await asyncio.to_thread(self._client.component_overview)
            # The independent control reader may have advanced while the cold
            # aggregate was in flight. Publish against its newest current
            # cycle, never the cycle captured before that slow read.
            cycle = self._cycle
            if not isinstance(cycle, dict):
                raise ValueError("shared shell control projection is unavailable")
            if not _overview_matches_snapshot(
                overview,
                runtime,
                alarms,
                authoring,
                cycle,
            ):
                raise ValueError("Component Overview crossed an owner revision")
            self._runtime = runtime
            self._alarms = alarms
            self._authoring = authoring
            self._overview = overview
            return self._complete_projection(cycle)
        except (HeadlessClientError, KeyError, TypeError, ValueError) as error:
            # Once a coherent shell exists, a slow or failed diagnostic refresh
            # must not erase live execution controls. Keep the last complete
            # snapshot and retry on the next bounded full-refresh interval.
            if (
                self._last_projection_key is not None
                and self._has_complete_snapshot()
            ):
                return None
            return self._error_projection(error)

    async def _run_control(self) -> None:
        while not self._closed and self._subscriptions:
            payload = await self._refresh_control()
            if payload is not None and self._subscriptions:
                self._publish(payload)
            await asyncio.sleep(POLL_INTERVAL_SECONDS)

    async def _run_full(self) -> None:
        while not self._closed and self._subscriptions:
            payload = await self._refresh_full()
            if payload is not None and self._subscriptions:
                self._publish(payload)
            await asyncio.sleep(FULL_REFRESH_SECONDS)


async def serve_headless_state(
    websocket: WebSocket,
    source: HeadlessStateSource,
    *,
    send_timeout_seconds: float = SEND_TIMEOUT_SECONDS,
) -> None:
    """Publish the shared low-rate shell/control projection to one browser."""

    await websocket.accept()
    try:
        subscription = source.subscribe()
    except RuntimeError:
        await websocket.close(code=1013, reason="headless state client unavailable")
        return
    receive = asyncio.create_task(websocket.receive_text())
    try:
        while not subscription.closed:
            wake = asyncio.create_task(subscription.wake.wait())
            try:
                done, _pending = await asyncio.wait(
                    (receive, wake),
                    return_when=asyncio.FIRST_COMPLETED,
                )
                if receive in done:
                    # The shell stream is server-owned and read-only. A completed
                    # receive means disconnect or unsupported client input.
                    receive.result()
                    await websocket.close(
                        code=1003,
                        reason="unsupported shell message",
                    )
                    return
            finally:
                # A disconnect may win the race while the subscription is
                # idle. Never leave that per-iteration wait behind in ASGI.
                wake.cancel()
                with suppress(asyncio.CancelledError):
                    await wake
            payload = source.take_payload(subscription)
            if payload is not None:
                await asyncio.wait_for(
                    websocket.send_text(payload),
                    timeout=send_timeout_seconds,
                )
    except (WebSocketDisconnect, asyncio.TimeoutError, RuntimeError):
        pass
    finally:
        receive.cancel()
        with suppress(asyncio.CancelledError, WebSocketDisconnect):
            await receive
        source.unsubscribe(subscription)

__all__ = [
    "HeadlessStateSource",
    "HeadlessStateSubscription",
    "serve_headless_state",
]
