"""Disposable process fixtures for composable-cell I4 observation evidence.

This module is test-only.  It intentionally separates the effect-capable
managed adapter from the observation-only external adapter.  Child processes
mint their own opaque incarnation token so replacing an observer cannot create
a process epoch.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
import json
import select
import subprocess
import sys
from typing import Final
import uuid

from .runtime_observation_fixture import (
    FixturePresenceState,
    FixtureProcessPresence,
    fixture_runtime_unit_ids,
)

_MAX_PROCESS_REQUESTS = 256

_CHILD_SOURCE: Final = (
    "import json,os,sys,uuid\n"
    "payload={'incarnation':str(uuid.uuid4()),'pid':os.getpid()}\n"
    "sys.stdout.write(json.dumps(payload,separators=(',',':'))+'\\n')\n"
    "sys.stdout.flush()\n"
    "sys.stdin.read()\n"
)


@dataclass(slots=True)
class _SpawnedProcess:
    process: subprocess.Popen[str]
    incarnation: str
    process_id: int


class FixtureProcessOperation(StrEnum):
    START = "start"
    STOP = "stop"
    RESTART = "restart"


class FixtureProcessResult(StrEnum):
    CHANGED = "changed"
    ALREADY_SATISFIED = "already_satisfied"


@dataclass(frozen=True, slots=True)
class FixtureProcessRequest:
    request_sequence: int
    operation: FixtureProcessOperation
    runtime_unit_id: str


@dataclass(frozen=True, slots=True)
class FixtureProcessReconciliation:
    request_sequence: int
    operation: FixtureProcessOperation
    runtime_unit_id: str
    result: FixtureProcessResult
    before_state: FixturePresenceState
    before_incarnation: str
    after_state: FixturePresenceState
    after_incarnation: str


class _ProcessRegistry:
    """External test harness that owns actual disposable child processes."""

    def __init__(self, declared_runtime_unit_ids: tuple[str, ...]) -> None:
        declared = fixture_runtime_unit_ids(declared_runtime_unit_ids)
        self.declared_runtime_unit_ids = declared
        self._declared_runtime_unit_ids = frozenset(declared)
        self._processes: dict[str, _SpawnedProcess] = {}

    def __enter__(self) -> _ProcessRegistry:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def start_external(self, runtime_unit_id: str) -> str:
        self._require_declared(runtime_unit_id)
        current = self._processes.get(runtime_unit_id)
        if current is not None and current.process.poll() is None:
            return current.incarnation
        spawned = self._spawn()
        self._processes[runtime_unit_id] = spawned
        return spawned.incarnation

    def stop_external(self, runtime_unit_id: str) -> None:
        self._require_declared(runtime_unit_id)
        spawned = self._processes.pop(runtime_unit_id, None)
        if spawned is not None:
            self._stop(spawned.process)

    def restart_external(self, runtime_unit_id: str) -> str:
        self.stop_external(runtime_unit_id)
        return self.start_external(runtime_unit_id)

    def fact(self, runtime_unit_id: str) -> tuple[FixturePresenceState, str]:
        self._require_declared(runtime_unit_id)
        spawned = self._processes.get(runtime_unit_id)
        if spawned is None or spawned.process.poll() is not None:
            return FixturePresenceState.ABSENT, ""
        return FixturePresenceState.PRESENT, spawned.incarnation

    def close(self) -> None:
        for runtime_unit_id in tuple(self._processes):
            self.stop_external(runtime_unit_id)

    def _require_declared(self, runtime_unit_id: str) -> None:
        if runtime_unit_id not in self._declared_runtime_unit_ids:
            raise ValueError("runtime_unit_id is not declared for this fixture")

    @staticmethod
    def _spawn() -> _SpawnedProcess:
        process = subprocess.Popen(
            (sys.executable, "-u", "-c", _CHILD_SOURCE),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        assert process.stdout is not None
        ready, _, _ = select.select((process.stdout,), (), (), 5.0)
        if not ready:
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError("fixture child did not publish an incarnation")
        try:
            payload = json.loads(process.stdout.readline())
            incarnation = str(payload["incarnation"])
            process_id = int(payload["pid"])
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError("fixture child incarnation is malformed") from exc
        if process_id != process.pid or not incarnation:
            process.kill()
            process.wait(timeout=5)
            raise RuntimeError("fixture child incarnation identity mismatch")
        return _SpawnedProcess(
            process=process,
            incarnation=incarnation,
            process_id=process_id,
        )

    @staticmethod
    def _stop(process: subprocess.Popen[str]) -> None:
        if process.poll() is not None:
            return
        if process.stdin is not None:
            try:
                process.stdin.close()
            except BrokenPipeError:
                pass
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)


class ExternalProcessFixture:
    """A test actor outside Manager that owns an external process."""

    def __init__(self, declared_runtime_unit_ids: tuple[str, ...]) -> None:
        self._registry = _ProcessRegistry(declared_runtime_unit_ids)

    @property
    def declared_runtime_unit_ids(self) -> tuple[str, ...]:
        return self._registry.declared_runtime_unit_ids

    def __enter__(self) -> ExternalProcessFixture:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def start(self, runtime_unit_id: str) -> str:
        return self._registry.start_external(runtime_unit_id)

    def stop(self, runtime_unit_id: str) -> None:
        self._registry.stop_external(runtime_unit_id)

    def restart(self, runtime_unit_id: str) -> str:
        return self._registry.restart_external(runtime_unit_id)

    def close(self) -> None:
        self._registry.close()

    def _fact(self, runtime_unit_id: str) -> tuple[FixturePresenceState, str]:
        return self._registry.fact(runtime_unit_id)


class ObservationOnlyProcessFixture:
    """Process-presence adapter with deliberately no mutation methods."""

    def __init__(self, external_processes: ExternalProcessFixture) -> None:
        if not isinstance(external_processes, ExternalProcessFixture):
            raise TypeError("external_processes must be an ExternalProcessFixture")
        self._external_processes = external_processes
        self._observer_instance_id = f"observer-{uuid.uuid4()}"
        self._sequences: dict[str, int] = {}

    @property
    def observer_instance_id(self) -> str:
        return self._observer_instance_id

    def observe(self, runtime_unit_id: str) -> FixtureProcessPresence:
        state, incarnation = self._external_processes._fact(runtime_unit_id)
        sequence = self._sequences.get(runtime_unit_id, 0) + 1
        self._sequences[runtime_unit_id] = sequence
        return FixtureProcessPresence(
            runtime_unit_id=runtime_unit_id,
            observer_instance_id=self._observer_instance_id,
            observation_sequence=sequence,
            state=state,
            process_incarnation=incarnation,
        )


class ManagedProcessFixture:
    """Effect-capable fake adapter whose child mints each real incarnation."""

    def __init__(self, declared_runtime_unit_ids: tuple[str, ...]) -> None:
        self._registry = _ProcessRegistry(declared_runtime_unit_ids)
        self._observer_instance_id = f"managed-observer-{uuid.uuid4()}"
        self._sequences: dict[str, int] = {}
        self._requests: list[FixtureProcessRequest] = []
        self._reconciliations: list[FixtureProcessReconciliation] = []

    @property
    def requests(self) -> tuple[FixtureProcessRequest, ...]:
        return tuple(self._requests)

    @property
    def reconciliations(self) -> tuple[FixtureProcessReconciliation, ...]:
        return tuple(self._reconciliations)

    def __enter__(self) -> ManagedProcessFixture:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()

    def start(self, runtime_unit_id: str) -> FixtureProcessPresence:
        return self._request(FixtureProcessOperation.START, runtime_unit_id)

    def stop(self, runtime_unit_id: str) -> FixtureProcessPresence:
        return self._request(FixtureProcessOperation.STOP, runtime_unit_id)

    def restart(self, runtime_unit_id: str) -> FixtureProcessPresence:
        return self._request(FixtureProcessOperation.RESTART, runtime_unit_id)

    def _request(
        self,
        operation: FixtureProcessOperation,
        runtime_unit_id: str,
    ) -> FixtureProcessPresence:
        before_state, before_incarnation = self._registry.fact(runtime_unit_id)
        if len(self._requests) >= _MAX_PROCESS_REQUESTS:
            raise RuntimeError("fixture process-request limit exceeded")
        request = FixtureProcessRequest(
            request_sequence=len(self._requests) + 1,
            operation=operation,
            runtime_unit_id=runtime_unit_id,
        )
        self._requests.append(request)
        if operation is FixtureProcessOperation.START:
            self._registry.start_external(runtime_unit_id)
        elif operation is FixtureProcessOperation.STOP:
            self._registry.stop_external(runtime_unit_id)
        else:
            self._registry.restart_external(runtime_unit_id)
        after_state, after_incarnation = self._registry.fact(runtime_unit_id)
        changed = (
            before_state is not after_state
            or before_incarnation != after_incarnation
        )
        self._reconciliations.append(
            FixtureProcessReconciliation(
                request_sequence=request.request_sequence,
                operation=operation,
                runtime_unit_id=runtime_unit_id,
                result=(
                    FixtureProcessResult.CHANGED
                    if changed
                    else FixtureProcessResult.ALREADY_SATISFIED
                ),
                before_state=before_state,
                before_incarnation=before_incarnation,
                after_state=after_state,
                after_incarnation=after_incarnation,
            )
        )
        return self.observe(runtime_unit_id)

    def observe(self, runtime_unit_id: str) -> FixtureProcessPresence:
        state, incarnation = self._registry.fact(runtime_unit_id)
        sequence = self._sequences.get(runtime_unit_id, 0) + 1
        self._sequences[runtime_unit_id] = sequence
        return FixtureProcessPresence(
            runtime_unit_id=runtime_unit_id,
            observer_instance_id=self._observer_instance_id,
            observation_sequence=sequence,
            state=state,
            process_incarnation=incarnation,
        )

    def close(self) -> None:
        self._registry.close()
