"""Demand-driven current-state source for the UI6-F control session.

The source is deliberately small: Program Runner wakes it through one private
revision stream, filesystem owners wake one cold-combine bit, and each browser
holds only the newest assembled value.  It is neither an event log nor a
semantic owner.
"""

from __future__ import annotations

import asyncio
from contextlib import suppress
from dataclasses import dataclass
import json
from pathlib import Path
from typing import Any, Iterable

from watchfiles import awatch
from websockets.asyncio.client import connect as connect_websocket
from websockets.exceptions import ConnectionClosed

from .control_protocol import (
    CONTROL_COLD_COMBINE_NS,
    CONTROL_MAXIMUM_CLIENTS,
    CONTROL_MAXIMUM_SNAPSHOT_BYTES,
    EXECUTION_CONTROL_PROJECTION_SCHEMA,
    ExecutionControlProjectionDTO,
)
from .execution_control import ExecutionControlIngress
from .native import HeadlessApiError, strict_json_object
from .service import HeadlessService


_PROGRAM_CONTROL_STATE_SCHEMA = "manyforge.program_control_state.v1"


@dataclass(frozen=True, slots=True)
class ControlStateFrame:
    generation: int
    event_class: str
    control: ExecutionControlProjectionDTO
    shell: dict[str, Any]


@dataclass(eq=False)
class ControlStateSubscription:
    wake: asyncio.Event
    frame: ControlStateFrame | None = None
    closed: bool = False


class ControlStateUnavailableError(RuntimeError):
    """The owner inputs do not yet form one coherent control snapshot."""


def _overview_basis_matches(
    overview: dict[str, Any],
    runtime: dict[str, Any],
    alarms: dict[str, Any],
    authoring: dict[str, Any],
    cycle: dict[str, Any],
) -> bool:
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
    return all(basis.get(key) == value for key, value in expected.items())


def _meaningful_runtime_key(runtime: dict[str, Any]) -> tuple[Any, ...]:
    """Ignore aggregate receiver heartbeats while retaining owner changes."""

    sections = runtime.get("sections")
    sections = sections if isinstance(sections, dict) else {}

    def source(name: str, generation: bool) -> tuple[Any, ...]:
        row = sections.get(name)
        row = row if isinstance(row, dict) else {}
        return (
            row.get("available"),
            row.get("freshness"),
            tuple(row.get("known_gaps", ())),
            row.get("source_owner_id"),
            row.get("source_owner_epoch"),
            row.get("source_generation") if generation else None,
            row.get("source_content_digest") if generation else None,
        )

    return (
        runtime.get("projection_owner_id"),
        runtime.get("projection_owner_epoch"),
        runtime.get("complete"),
        tuple(runtime.get("reasons", ())),
        tuple(runtime.get("unavailable_sections", ())),
        source("manager", False),
        source("logical_scene", True),
        source("scene_runtime", True),
    )


def _shell_key(shell: dict[str, Any]) -> tuple[Any, ...]:
    alarms = shell["alarms"]
    authoring = shell["authoring_admission"]
    overview = shell["component_overview"]
    return (
        _meaningful_runtime_key(shell["runtime"]),
        alarms.get("owner_epoch"),
        alarms.get("generation"),
        alarms.get("state_revision"),
        authoring.get("authoring_allowed"),
        authoring.get("admission_generation"),
        json.dumps(
            shell["execution_targets"], sort_keys=True, separators=(",", ":")
        ),
        json.dumps(
            shell["execution_control_state"],
            sort_keys=True,
            separators=(",", ":"),
        ),
        json.dumps(overview, sort_keys=True, separators=(",", ":")),
    )


def _fresh_runtime_expiry_delay_ns(
    runtime: dict[str, Any],
    freshness_budget_ns: int,
) -> int | None:
    """Return the one next semantic freshness boundary, not a poll period."""

    sections = runtime.get("sections")
    if not isinstance(sections, dict):
        return None
    remaining: list[int] = []
    for value in sections.values():
        if not isinstance(value, dict) or value.get("freshness") != "fresh":
            continue
        age_ns = value.get("receiver_age_ns")
        if type(age_ns) is not int or age_ns < 0:
            continue
        # Native freshness remains valid at the budget and becomes stale one
        # nanosecond later. A crossed boundary is reread once; it never starts
        # a periodic retry loop.
        remaining.append(max(1, freshness_budget_ns - age_ns + 1))
    return min(remaining) if remaining else None


def build_control_projection(
    cycle: dict[str, Any], ingress: ExecutionControlIngress
) -> ExecutionControlProjectionDTO:
    owner_epoch = cycle.get("program_runner_owner_epoch")
    revision = cycle.get("execution_control_revision")
    if not isinstance(owner_epoch, str) or not owner_epoch:
        raise HeadlessApiError(
            "execution_control_projection_invalid",
            "Program owner omitted its owner epoch",
        )
    if type(revision) is not int or revision < 1:
        raise HeadlessApiError(
            "execution_control_projection_invalid",
            "Program owner omitted its control revision",
        )
    failure = cycle.get("failure")
    state = cycle.get("state")
    return ExecutionControlProjectionDTO.model_validate(
        {
            "schema_version": EXECUTION_CONTROL_PROJECTION_SCHEMA,
            "receiver_incarnation": ingress.receiver_incarnation,
            "semantic_owner_epoch": owner_epoch,
            "execution_control_revision": revision,
            "cycle_state": state,
            "cycle_generation": cycle.get("cycle_generation"),
            "program_run_id": cycle.get("program_run_id"),
            "run_revision": cycle.get("run_revision"),
            "allowed_commands": cycle.get("allowed_commands"),
            "causal_correlation_id": cycle.get("causal_correlation_id"),
            "causal_session_id": cycle.get("causal_session_id"),
            "causal_client_sequence": cycle.get("causal_client_sequence"),
            "current_rejection": None,
            "current_failure": failure,
            "recovery_required": state == "recovery_required",
            "freshness": ingress.freshness_for(owner_epoch, revision),
        }
    )


class HeadlessControlStateSource:
    """One demand-scoped Program stream and one capacity-one fanout."""

    def __init__(
        self,
        service: HeadlessService,
        ingress: ExecutionControlIngress,
        *,
        program_runner_websocket_url: str | None,
        cold_watch_paths: Iterable[Path] = (),
        cold_freshness_budget_ns: int | None = None,
    ) -> None:
        self._service = service
        self._ingress = ingress
        self._runner_url = program_runner_websocket_url
        self._watch_paths = tuple(
            dict.fromkeys(str(path) for path in cold_watch_paths if path.exists())
        )
        if cold_freshness_budget_ns is not None and cold_freshness_budget_ns < 1:
            raise ValueError("cold freshness budget must be positive")
        self._cold_freshness_budget_ns = cold_freshness_budget_ns
        self._subscriptions: set[ControlStateSubscription] = set()
        self._loop: asyncio.AbstractEventLoop | None = None
        self._runner_task: asyncio.Task[None] | None = None
        self._cold_task: asyncio.Task[None] | None = None
        self._watch_task: asyncio.Task[None] | None = None
        self._cold_expiry_handle: asyncio.TimerHandle | None = None
        self._lifecycle_lock = asyncio.Lock()
        self._closed = False
        self._generation = 0
        self._cycle: dict[str, Any] | None = None
        self._shell: dict[str, Any] | None = None
        self._shell_key: tuple[Any, ...] | None = None
        self._cold_wake: asyncio.Event | None = None
        self._program_source_current = False

    @property
    def active_clients(self) -> int:
        return len(self._subscriptions)

    async def subscribe(self) -> ControlStateSubscription:
        loop = asyncio.get_running_loop()
        async with self._lifecycle_lock:
            if self._closed:
                raise RuntimeError("control state source is closed")
            if self._loop is not None and self._loop is not loop:
                raise RuntimeError("control state source crossed event loops")
            if len(self._subscriptions) >= CONTROL_MAXIMUM_CLIENTS:
                raise RuntimeError("control state client limit reached")
            self._loop = loop
            subscription = ControlStateSubscription(asyncio.Event())
            self._subscriptions.add(subscription)
            if len(self._subscriptions) == 1:
                self._cold_wake = asyncio.Event()
                self._runner_task = loop.create_task(self._run_program_source())
                self._cold_task = loop.create_task(self._run_cold_assembler())
                if self._watch_paths:
                    self._watch_task = loop.create_task(self._run_file_wakes())
                self.notify_cold()
            elif self._is_synchronized():
                subscription.frame = self._frame("critical")
                subscription.wake.set()
            return subscription

    async def unsubscribe(self, subscription: ControlStateSubscription) -> None:
        async with self._lifecycle_lock:
            subscription.closed = True
            subscription.frame = None
            subscription.wake.set()
            self._subscriptions.discard(subscription)
            if self._subscriptions:
                return
            tasks = tuple(
                task
                for task in (self._runner_task, self._cold_task, self._watch_task)
                if task is not None
            )
            for task in tasks:
                if not task.done():
                    task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            self._runner_task = None
            self._cold_task = None
            self._watch_task = None
            if self._cold_expiry_handle is not None:
                self._cold_expiry_handle.cancel()
                self._cold_expiry_handle = None
            self._cycle = None
            self._shell = None
            self._shell_key = None
            self._cold_wake = None
            self._program_source_current = False
            self._loop = None

    def notify_cold(self) -> None:
        loop = self._loop
        wake = self._cold_wake
        if loop is not None and wake is not None:
            loop.call_soon_threadsafe(wake.set)

    def notify_control(self) -> None:
        """Wake injected-service fixtures after an in-process owner effect."""

        loop = self._loop
        if loop is not None and self._runner_url is None:
            loop.call_soon_threadsafe(
                lambda: asyncio.create_task(self._read_current_cycle("critical"))
            )

    async def close(self) -> None:
        self._closed = True
        subscriptions = tuple(self._subscriptions)
        for subscription in subscriptions:
            await self.unsubscribe(subscription)

    def take(self, subscription: ControlStateSubscription) -> ControlStateFrame | None:
        frame = subscription.frame
        subscription.frame = None
        subscription.wake.clear()
        return frame

    def current(self) -> ControlStateFrame:
        if not self._is_synchronized():
            raise ControlStateUnavailableError("control state is not synchronized")
        return self._frame("critical")

    def _is_synchronized(self) -> bool:
        cycle = self._cycle
        shell = self._shell
        if cycle is None or shell is None or not self._program_source_current:
            return False
        runtime = shell.get("runtime")
        alarms = shell.get("alarms")
        overview = shell.get("component_overview")
        authoring = shell.get("authoring_admission")
        if not all(
            isinstance(value, dict)
            for value in (runtime, alarms, overview, authoring)
        ):
            return False
        return _overview_basis_matches(
            overview,
            runtime,
            alarms,
            authoring,
            cycle,
        )

    def _frame(self, event_class: str) -> ControlStateFrame:
        assert self._cycle is not None
        assert self._shell is not None
        return ControlStateFrame(
            generation=self._generation,
            event_class=event_class,
            control=build_control_projection(self._cycle, self._ingress),
            shell={**self._shell, "program_cycle": self._cycle},
        )

    def _publish(self, event_class: str) -> None:
        if not self._is_synchronized():
            return
        self._generation += 1
        frame = self._frame(event_class)
        for subscription in self._subscriptions:
            if subscription.closed:
                continue
            subscription.frame = frame
            subscription.wake.set()

    async def _read_current_cycle(self, event_class: str) -> None:
        try:
            cycle = await asyncio.to_thread(self._service.program_cycle_status)
        except Exception:
            self._program_source_current = False
            return
        self._program_source_current = True
        self._accept_cycle(cycle, event_class)

    def _accept_cycle(self, cycle: dict[str, Any], event_class: str) -> None:
        previous = self._cycle
        if previous == cycle:
            return
        self._cycle = cycle
        if previous is None or (
            (
                previous.get("program_runner_owner_epoch"),
                previous.get("cycle_generation"),
            )
            != (
                cycle.get("program_runner_owner_epoch"),
                cycle.get("cycle_generation"),
            )
        ):
            self.notify_cold()
        self._publish(event_class)

    async def _run_program_source(self) -> None:
        if self._runner_url is None:
            await self._read_current_cycle("critical")
            return
        retry_seconds = 0.05
        while self._subscriptions and not self._closed:
            try:
                async with connect_websocket(
                    self._runner_url,
                    max_size=CONTROL_MAXIMUM_SNAPSHOT_BYTES,
                    open_timeout=1.0,
                    close_timeout=0.25,
                ) as websocket:
                    previous_sequence = 0
                    retry_seconds = 0.05
                    async for message in websocket:
                        if not isinstance(message, str):
                            raise ValueError("Program control frame is not text")
                        value = strict_json_object(
                            message,
                            label="Program control state",
                            maximum_bytes=CONTROL_MAXIMUM_SNAPSHOT_BYTES,
                        )
                        if set(value) != {
                            "program_cycle",
                            "schema_version",
                            "server_sequence",
                        } or value.get("schema_version") != _PROGRAM_CONTROL_STATE_SCHEMA:
                            raise ValueError("Program control frame has an invalid shape")
                        sequence_text = value.get("server_sequence")
                        if (
                            not isinstance(sequence_text, str)
                            or not sequence_text.isascii()
                            or not sequence_text.isdigit()
                            or sequence_text.startswith("0")
                        ):
                            raise ValueError("Program control sequence is invalid")
                        sequence = int(sequence_text)
                        if sequence <= previous_sequence:
                            raise ValueError("Program control sequence regressed")
                        previous_sequence = sequence
                        cycle = value.get("program_cycle")
                        if not isinstance(cycle, dict):
                            raise ValueError("Program control state omitted its cycle")
                        self._program_source_current = True
                        previous = self._cycle
                        event_class = (
                            "node_progress"
                            if previous is not None
                            and previous.get("execution_control_revision")
                            == cycle.get("execution_control_revision")
                            else "critical"
                        )
                        self._accept_cycle(cycle, event_class)
            except asyncio.CancelledError:
                raise
            except (ConnectionClosed, OSError, TimeoutError, ValueError):
                # Reconnection is source recovery, never command replay. Keep
                # the newest visual state but stop issuing new frames until a
                # current owner snapshot arrives.
                self._program_source_current = False
                await asyncio.sleep(retry_seconds)
                retry_seconds = min(retry_seconds * 2, 1.0)

    async def _assemble_shell(self) -> tuple[dict[str, Any], tuple[Any, ...]]:
        async def execution_projection() -> tuple[dict[str, Any] | None, dict[str, Any] | None]:
            try:
                targets, control_state = await asyncio.gather(
                    asyncio.to_thread(self._service.execution_targets),
                    asyncio.to_thread(self._service.execution_control_state),
                )
            except Exception:
                return None, None
            return targets, control_state

        runtime, alarms, authoring, execution_pair = await asyncio.gather(
            asyncio.to_thread(self._service.runtime),
            asyncio.to_thread(
                self._service.alarms,
                limit=32,
                ordering="attention_newest",
            ),
            asyncio.to_thread(self._service.authoring_admission),
            execution_projection(),
        )
        execution_targets, execution_control_state = execution_pair
        overview = await asyncio.to_thread(self._service.component_overview)
        cycle = self._cycle
        if cycle is None:
            cycle = await asyncio.to_thread(self._service.program_cycle_status)
            self._cycle = cycle
        if not _overview_basis_matches(overview, runtime, alarms, authoring, cycle):
            # A source advanced across the cold aggregate. One immediate
            # current reread converges without publishing a crossed snapshot.
            runtime = await asyncio.to_thread(self._service.runtime)
            alarms = await asyncio.to_thread(
                self._service.alarms,
                limit=32,
                ordering="attention_newest",
            )
            authoring = await asyncio.to_thread(self._service.authoring_admission)
            execution_targets, execution_control_state = await execution_projection()
            overview = await asyncio.to_thread(self._service.component_overview)
            cycle = self._cycle or cycle
            if not _overview_basis_matches(
                overview, runtime, alarms, authoring, cycle
            ):
                raise RuntimeError("cold control projection crossed owner revisions")
        shell = {
            "runtime": runtime,
            "alarms": alarms,
            "component_overview": overview,
            "authoring_admission": authoring,
            "execution_targets": execution_targets,
            "execution_control_state": execution_control_state,
        }
        return shell, _shell_key(shell)

    async def _run_cold_assembler(self) -> None:
        assert self._cold_wake is not None
        wake = self._cold_wake
        while self._subscriptions and not self._closed:
            await wake.wait()
            wake.clear()
            await asyncio.sleep(CONTROL_COLD_COMBINE_NS / 1_000_000_000)
            try:
                shell, key = await self._assemble_shell()
            except Exception:
                continue
            changed = key != self._shell_key
            self._shell = shell
            self._shell_key = key
            self._schedule_cold_expiry(shell)
            if changed or self._generation == 0:
                self._publish("cold")

    def _schedule_cold_expiry(self, shell: dict[str, Any]) -> None:
        if self._cold_expiry_handle is not None:
            self._cold_expiry_handle.cancel()
            self._cold_expiry_handle = None
        budget_ns = self._cold_freshness_budget_ns
        loop = self._loop
        runtime = shell.get("runtime")
        if (
            budget_ns is None
            or loop is None
            or not self._subscriptions
            or not isinstance(runtime, dict)
        ):
            return
        delay_ns = _fresh_runtime_expiry_delay_ns(runtime, budget_ns)
        if delay_ns is None:
            return

        def expire() -> None:
            self._cold_expiry_handle = None
            self.notify_cold()

        self._cold_expiry_handle = loop.call_later(
            delay_ns / 1_000_000_000,
            expire,
        )

    async def _run_file_wakes(self) -> None:
        try:
            async for _changes in awatch(*self._watch_paths):
                if not self._subscriptions or self._closed:
                    return
                self.notify_cold()
        except asyncio.CancelledError:
            raise
        except OSError:
            return


__all__ = [
    "ControlStateFrame",
    "ControlStateSubscription",
    "ControlStateUnavailableError",
    "HeadlessControlStateSource",
    "build_control_projection",
]
