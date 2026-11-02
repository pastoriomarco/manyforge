"""P0 candidate: durable runtime-unit Apply, isolated from robot semantics.

This module is experimental selection evidence.  It deliberately knows only
about runtime-unit process effects; it has no component-lifecycle, controller,
or motion surface.
"""

from __future__ import annotations

from dataclasses import dataclass, fields, is_dataclass
from enum import Enum
import hashlib
import json
from pathlib import Path
import sqlite3
from typing import Any, Callable, Iterable, Protocol

from verification.composable_cell_i1a.contracts import (
    SCHEMA_ID,
    ApplyRequest,
    ApplyStatus,
    ApplyTransactionId,
    GraphRevision,
    ObservedState,
    RuntimeUnitId,
    RuntimeUnitObservation,
    RuntimeUnitSpec,
)


class DeterministicManagerCrash(RuntimeError):
    """Raised by a test crash injector at a named durable boundary."""


class ProcessAdapter(Protocol):
    """The complete effect surface available to the candidate manager."""

    def start(self, spec: RuntimeUnitSpec) -> RuntimeUnitObservation: ...

    def stop(self, runtime_unit_id: RuntimeUnitId) -> RuntimeUnitObservation: ...

    def observe(
        self, runtime_unit_id: RuntimeUnitId
    ) -> RuntimeUnitObservation | None: ...

    def observe_all(self) -> tuple[RuntimeUnitObservation, ...]: ...

    def observed_spec_fingerprint(
        self, runtime_unit_id: RuntimeUnitId
    ) -> str | None: ...


class MonotonicClock(Protocol):
    def now_ns(self) -> int: ...


CrashInjector = Callable[[str], None]
_OBSERVATION_UNAVAILABLE = object()
_OPERATIONAL_HEALTH = frozenset({"healthy", "ready", "running"})
_ABSENT_HEALTH = frozenset({"absent", "missing", "stopped"})


@dataclass(frozen=True)
class EffectJournalEntry:
    ordinal: int
    direction: str
    action: str
    runtime_unit_id: RuntimeUnitId
    state: str
    observed_epoch: str | None
    detail: str


def crash_once(checkpoint: str) -> CrashInjector:
    """Return a one-shot injector useful for deterministic restart tests."""

    armed = True

    def inject(candidate: str) -> None:
        nonlocal armed
        if armed and candidate == checkpoint:
            armed = False
            raise DeterministicManagerCrash(candidate)

    return inject


def effect_checkpoints(
    ordinal: int, action: str, runtime_unit_id: RuntimeUnitId | str
) -> tuple[str, ...]:
    """Enumerate all durable boundaries around one external process effect."""

    stem = f"effect:{ordinal}:{action}:{runtime_unit_id}"
    return (
        f"{stem}:before_intent",
        f"{stem}:after_intent",
        f"{stem}:before_effect",
        f"{stem}:after_effect",
        f"{stem}:before_effect_record",
        f"{stem}:after_effect_record",
    )


def _noop_crash(_: str) -> None:
    return


def _canonical(value: Any) -> Any:
    if is_dataclass(value):
        return {field.name: _canonical(getattr(value, field.name)) for field in fields(value)}
    if isinstance(value, Enum):
        return _canonical(value.value)
    if isinstance(value, dict):
        return {
            str(key): _canonical(item)
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        }
    if isinstance(value, (tuple, list)):
        return [_canonical(item) for item in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def _request_digest(request: ApplyRequest) -> str:
    metadata = request.metadata
    stable_metadata = {
        "request_id": str(metadata.request_id),
        "source_label": metadata.source_label,
        "transport": metadata.transport,
        "correlation_id": metadata.correlation_id,
    }
    payload = {
        "metadata": stable_metadata,
        "apply_id": str(request.apply_id),
        "expected_graph_revision": int(request.expected_graph_revision),
        "desired_graph": _canonical(request.desired_graph),
        "busy_disposition": request.busy_disposition,
    }
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def _encode_units(units: Iterable[RuntimeUnitSpec]) -> str:
    payload = [
        {
            "runtime_unit_id": str(unit.runtime_unit_id),
            "kind": _canonical(unit.kind),
            "dependencies": [str(item) for item in unit.dependencies],
            "command": list(unit.command),
        }
        for unit in sorted(units, key=lambda item: str(item.runtime_unit_id))
    ]
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _decode_units(encoded: str) -> dict[str, RuntimeUnitSpec]:
    result: dict[str, RuntimeUnitSpec] = {}
    for item in json.loads(encoded):
        spec = RuntimeUnitSpec(
            runtime_unit_id=RuntimeUnitId(item["runtime_unit_id"]),
            kind=item["kind"],
            dependencies=tuple(RuntimeUnitId(value) for value in item["dependencies"]),
            command=tuple(item["command"]),
        )
        result[str(spec.runtime_unit_id)] = spec
    return result


def _unit_signature(spec: RuntimeUnitSpec) -> tuple[Any, ...]:
    return (
        spec.kind,
        tuple(str(item) for item in spec.dependencies),
        tuple(spec.command),
    )


def _unit_fingerprint(spec: RuntimeUnitSpec) -> str:
    return hashlib.sha256(_encode_units((spec,)).encode("utf-8")).hexdigest()


def _validate_and_sort(
    units: dict[str, RuntimeUnitSpec],
) -> tuple[str, ...]:
    unknown = sorted(
        {
            str(dependency)
            for unit in units.values()
            for dependency in unit.dependencies
            if str(dependency) not in units
        }
    )
    if unknown:
        raise ValueError(f"unknown_runtime_unit_dependency:{','.join(unknown)}")

    indegree = {unit_id: 0 for unit_id in units}
    dependents = {unit_id: set() for unit_id in units}
    for unit_id, unit in units.items():
        for dependency in unit.dependencies:
            dependency_id = str(dependency)
            indegree[unit_id] += 1
            dependents[dependency_id].add(unit_id)

    ready = sorted(unit_id for unit_id, degree in indegree.items() if degree == 0)
    ordered: list[str] = []
    while ready:
        unit_id = ready.pop(0)
        ordered.append(unit_id)
        for dependent in sorted(dependents[unit_id]):
            indegree[dependent] -= 1
            if indegree[dependent] == 0:
                ready.append(dependent)
                ready.sort()
    if len(ordered) != len(units):
        raise ValueError("runtime_unit_dependency_cycle")
    return tuple(ordered)


def _dependent_closure(
    units: dict[str, RuntimeUnitSpec], seeds: set[str]
) -> set[str]:
    closure = set(seeds)
    changed = True
    while changed:
        changed = False
        for unit_id, unit in units.items():
            if unit_id in closure:
                continue
            if any(str(dependency) in closure for dependency in unit.dependencies):
                closure.add(unit_id)
                changed = True
    return closure


def _plan_effects(
    old_units: dict[str, RuntimeUnitSpec],
    new_units: dict[str, RuntimeUnitSpec],
) -> tuple[tuple[str, RuntimeUnitSpec], ...]:
    old_order = _validate_and_sort(old_units)
    new_order = _validate_and_sort(new_units)
    changed = {
        unit_id
        for unit_id in old_units.keys() & new_units.keys()
        if _unit_signature(old_units[unit_id]) != _unit_signature(new_units[unit_id])
    }
    stop_ids = _dependent_closure(
        old_units, (old_units.keys() - new_units.keys()) | changed
    )
    stops = tuple(
        ("stop", old_units[unit_id])
        for unit_id in reversed(old_order)
        if unit_id in stop_ids
    )
    # A start intent is a durable "ensure running with this exact spec"
    # operation. Retained units need that intent too: the process observed at
    # Apply time may have disappeared, stalled, crashed, or drifted away from
    # the retained specification.
    starts = tuple(
        ("start", new_units[unit_id])
        for unit_id in new_order
    )
    return stops + starts


class DurableComponentManager:
    """SQLite-backed candidate for runtime-unit convergence.

    Each transaction and each effect intent is committed before an external
    process call.  Recovery observes the actual adapter state and epoch rather
    than assuming that a previously issued process command completed.
    """

    def __init__(
        self,
        *,
        database_path: str | Path,
        manager_runtime_unit_id: RuntimeUnitId,
        process_adapter: ProcessAdapter,
        clock: MonotonicClock,
    ) -> None:
        self._database_path = str(database_path)
        self._manager_runtime_unit_id = manager_runtime_unit_id
        self._adapter = process_adapter
        self._clock = clock
        self._create_schema()

    def _connect(self) -> sqlite3.Connection:
        connection = sqlite3.connect(
            self._database_path,
            isolation_level=None,
            timeout=10.0,
        )
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA busy_timeout = 10000")
        return connection

    def _create_schema(self) -> None:
        with self._connect() as connection:
            connection.executescript(
                """
                PRAGMA journal_mode = WAL;
                CREATE TABLE IF NOT EXISTS manager_state (
                    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
                    active_revision INTEGER NOT NULL,
                    reserved_revision INTEGER NOT NULL,
                    active_units_json TEXT NOT NULL
                );
                INSERT OR IGNORE INTO manager_state
                    (
                        singleton, active_revision, reserved_revision,
                        active_units_json
                    )
                    VALUES (1, 0, 0, '[]');

                CREATE TABLE IF NOT EXISTS apply_transactions (
                    apply_id TEXT PRIMARY KEY,
                    request_digest TEXT NOT NULL,
                    from_revision INTEGER NOT NULL,
                    target_revision INTEGER NOT NULL,
                    previous_units_json TEXT NOT NULL,
                    target_units_json TEXT NOT NULL,
                    phase TEXT NOT NULL,
                    direction TEXT NOT NULL,
                    detail TEXT NOT NULL
                );

                CREATE TABLE IF NOT EXISTS apply_effects (
                    apply_id TEXT NOT NULL,
                    ordinal INTEGER NOT NULL,
                    direction TEXT NOT NULL,
                    action TEXT NOT NULL,
                    unit_json TEXT NOT NULL,
                    state TEXT NOT NULL,
                    observed_epoch TEXT,
                    detail TEXT NOT NULL,
                    PRIMARY KEY (apply_id, ordinal),
                    FOREIGN KEY (apply_id)
                        REFERENCES apply_transactions(apply_id)
                );
                """
            )

    def apply(
        self,
        request: ApplyRequest,
        *,
        crash_injector: CrashInjector = _noop_crash,
    ) -> ApplyStatus:
        """CAS-create or exactly replay one durable Apply transaction."""

        apply_id = str(request.apply_id)
        digest = _request_digest(request)
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            existing = connection.execute(
                "SELECT * FROM apply_transactions WHERE apply_id = ?",
                (apply_id,),
            ).fetchone()
            if existing is not None:
                connection.commit()
                if existing["request_digest"] != digest:
                    return self._status_from_row(
                        existing,
                        phase="rejected",
                        detail="conflicting_apply_id",
                    )
                if existing["phase"] in {
                    "committed",
                    "rejected",
                    "rolled_back",
                }:
                    return self._status_from_row(existing)
                return self.recover(request.apply_id, crash_injector=crash_injector)

            state = connection.execute(
                "SELECT * FROM manager_state WHERE singleton = 1"
            ).fetchone()
            assert state is not None
            from_revision = int(state["active_revision"])
            reserved_revision = int(state["reserved_revision"])
            target_revision = int(request.desired_graph.graph_revision)
            target_units = {
                str(unit.runtime_unit_id): unit
                for unit in request.desired_graph.runtime_units
            }
            detail = ""
            try:
                if str(self._manager_runtime_unit_id) in target_units:
                    raise ValueError("manager_self_management_forbidden")
                _validate_and_sort(target_units)
                if request.busy_disposition != "reject_if_affected_busy":
                    raise ValueError("unsupported_busy_disposition")
                if reserved_revision != from_revision:
                    raise ValueError("apply_in_progress")
                if int(request.expected_graph_revision) != from_revision:
                    raise ValueError("stale_graph_revision")
                if target_revision <= from_revision:
                    raise ValueError("non_advancing_graph_revision")
            except ValueError as error:
                detail = str(error)

            previous_units_json = state["active_units_json"]
            target_units_json = _encode_units(target_units.values())
            phase = "rejected" if detail else "applying"
            connection.execute(
                """
                INSERT INTO apply_transactions (
                    apply_id, request_digest, from_revision, target_revision,
                    previous_units_json, target_units_json, phase, direction,
                    detail
                ) VALUES (?, ?, ?, ?, ?, ?, ?, 'forward', ?)
                """,
                (
                    apply_id,
                    digest,
                    from_revision,
                    target_revision,
                    previous_units_json,
                    target_units_json,
                    phase,
                    detail,
                ),
            )
            if detail:
                connection.commit()
                row = connection.execute(
                    "SELECT * FROM apply_transactions WHERE apply_id = ?",
                    (apply_id,),
                ).fetchone()
                assert row is not None
                return self._status_from_row(row)

            updated = connection.execute(
                """
                UPDATE manager_state SET reserved_revision = ?
                WHERE singleton = 1 AND reserved_revision = ?
                """,
                (target_revision, reserved_revision),
            )
            if updated.rowcount != 1:
                connection.rollback()
                return self.apply(request, crash_injector=crash_injector)

            old_units = _decode_units(previous_units_json)
            for ordinal, (action, spec) in enumerate(
                _plan_effects(old_units, target_units)
            ):
                connection.execute(
                    """
                    INSERT INTO apply_effects (
                        apply_id, ordinal, direction, action, unit_json,
                        state, observed_epoch, detail
                    ) VALUES (?, ?, 'forward', ?, ?, 'planned', NULL, '')
                    """,
                    (
                        apply_id,
                        ordinal,
                        action,
                        _encode_units((spec,)),
                    ),
                )
            connection.commit()

        crash_injector("apply:after_transaction_intent")
        return self._drive(
            request.apply_id,
            direction="forward",
            terminal_phase="committed",
            crash_injector=crash_injector,
        )

    def recover(
        self,
        apply_id: ApplyTransactionId,
        *,
        crash_injector: CrashInjector = _noop_crash,
    ) -> ApplyStatus:
        """Reconcile one incomplete transaction from actual process state."""

        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM apply_transactions WHERE apply_id = ?",
                (str(apply_id),),
            ).fetchone()
            if row is None:
                connection.rollback()
                raise KeyError(str(apply_id))
            if row["phase"] in {"committed", "rejected", "rolled_back"}:
                connection.commit()
                return self._status_from_row(row)
            direction = row["direction"]
            if direction not in {"forward", "rollback"}:
                connection.execute(
                    """
                    UPDATE apply_transactions
                    SET phase = 'incomplete', detail = ?
                    WHERE apply_id = ?
                    """,
                    (f"unknown_recovery_direction:{direction}", str(apply_id)),
                )
                connection.commit()
                return self.apply_status(apply_id)
            active_phase = (
                "applying" if direction == "forward" else "rolling_back"
            )
            terminal_phase = (
                "committed" if direction == "forward" else "rolled_back"
            )
            connection.execute(
                """
                UPDATE apply_transactions
                SET phase = ?, detail = ''
                WHERE apply_id = ?
                """,
                (active_phase, str(apply_id)),
            )
            connection.commit()
        return self._drive(
            apply_id,
            direction=direction,
            terminal_phase=terminal_phase,
            crash_injector=crash_injector,
        )

    def rollback(
        self,
        apply_id: ApplyTransactionId,
        *,
        crash_injector: CrashInjector = _noop_crash,
    ) -> ApplyStatus:
        """Explicitly converge an incomplete Apply to its retained predecessor."""

        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            row = connection.execute(
                "SELECT * FROM apply_transactions WHERE apply_id = ?",
                (str(apply_id),),
            ).fetchone()
            if row is None:
                connection.rollback()
                raise KeyError(str(apply_id))
            if row["phase"] == "rolled_back":
                connection.commit()
                return self._status_from_row(row)
            if row["direction"] == "rollback":
                connection.commit()
                return self.recover(apply_id, crash_injector=crash_injector)
            if row["phase"] != "incomplete":
                connection.commit()
                return self._status_from_row(
                    row,
                    phase="rejected",
                    detail="rollback_requires_incomplete_apply",
                )

            previous = _decode_units(row["previous_units_json"])
            target = _decode_units(row["target_units_json"])
            rollback_plan = _plan_effects(target, previous)
            connection.execute(
                """
                UPDATE apply_effects SET state = 'abandoned'
                WHERE apply_id = ? AND direction = 'forward'
                  AND state != 'effected'
                """,
                (str(apply_id),),
            )
            next_ordinal = int(
                connection.execute(
                    """
                    SELECT COALESCE(MAX(ordinal), -1) + 1
                    FROM apply_effects WHERE apply_id = ?
                    """,
                    (str(apply_id),),
                ).fetchone()[0]
            )
            for offset, (action, spec) in enumerate(rollback_plan):
                connection.execute(
                    """
                    INSERT INTO apply_effects (
                        apply_id, ordinal, direction, action, unit_json,
                        state, observed_epoch, detail
                    ) VALUES (?, ?, 'rollback', ?, ?, 'planned', NULL, '')
                    """,
                    (
                        str(apply_id),
                        next_ordinal + offset,
                        action,
                        _encode_units((spec,)),
                    ),
                )
            connection.execute(
                """
                UPDATE apply_transactions
                SET phase = 'rolling_back', direction = 'rollback', detail = ''
                WHERE apply_id = ?
                """,
                (str(apply_id),),
            )
            connection.commit()
        return self._drive(
            apply_id,
            direction="rollback",
            terminal_phase="rolled_back",
            crash_injector=crash_injector,
        )

    def apply_status(self, apply_id: ApplyTransactionId) -> ApplyStatus:
        return self._status_from_row(self._transaction_row(apply_id))

    def observed_state(self) -> ObservedState:
        """Reconstruct live runtime epochs; never synthesize them from intent."""

        with self._connect() as connection:
            state = connection.execute(
                "SELECT active_revision FROM manager_state WHERE singleton = 1"
            ).fetchone()
        assert state is not None
        observations = tuple(
            sorted(
                self._adapter.observe_all(),
                key=lambda item: str(item.runtime_unit_id),
            )
        )
        return ObservedState(
            schema=SCHEMA_ID,
            graph_revision=GraphRevision(state["active_revision"]),
            observed_monotonic_ns=self._clock.now_ns(),
            runtime_units=observations,
        )

    def journal(
        self, apply_id: ApplyTransactionId
    ) -> tuple[EffectJournalEntry, ...]:
        with self._connect() as connection:
            rows = connection.execute(
                """
                SELECT * FROM apply_effects
                WHERE apply_id = ? ORDER BY ordinal
                """,
                (str(apply_id),),
            ).fetchall()
        return tuple(
            EffectJournalEntry(
                ordinal=row["ordinal"],
                direction=row["direction"],
                action=row["action"],
                runtime_unit_id=RuntimeUnitId(
                    next(iter(_decode_units(row["unit_json"])))
                ),
                state=row["state"],
                observed_epoch=row["observed_epoch"],
                detail=row["detail"],
            )
            for row in rows
        )

    def _drive(
        self,
        apply_id: ApplyTransactionId,
        *,
        direction: str,
        terminal_phase: str,
        crash_injector: CrashInjector,
    ) -> ApplyStatus:
        target_units = self._target_units(apply_id, direction)
        if not self._append_unexpected_stop_intents(
            apply_id, direction, target_units
        ):
            return self.apply_status(apply_id)

        rows = self._effect_rows(apply_id, direction)
        for row in rows:
            if row["state"] == "abandoned":
                continue
            spec = next(iter(_decode_units(row["unit_json"]).values()))
            action = row["action"]
            ordinal = int(row["ordinal"])
            reconcile_settled = False
            if row["state"] == "effected":
                # A retained desired unit and a removed/unexpected unit are
                # convergence predicates, not one-shot commands. Re-check them
                # on recovery so drift after a prior effect cannot be committed.
                should_remain_absent = (
                    action == "stop"
                    and str(spec.runtime_unit_id) not in target_units
                )
                if action != "start" and not should_remain_absent:
                    continue
                observation = self._safe_observe(spec.runtime_unit_id)
                if self._effect_is_observed(action, observation, spec):
                    continue
                reconcile_settled = True
            checkpoints = effect_checkpoints(
                ordinal, action, spec.runtime_unit_id
            )
            if row["state"] == "planned":
                crash_injector(checkpoints[0])
                self._intend_effect(apply_id, ordinal)
                crash_injector(checkpoints[1])
            if not self._execute_effect_serialized(
                apply_id=apply_id,
                ordinal=ordinal,
                action=action,
                spec=spec,
                checkpoints=checkpoints,
                crash_injector=crash_injector,
                reconcile_settled=reconcile_settled,
            ):
                return self.apply_status(apply_id)

        mismatch = self._convergence_mismatch(target_units)
        if mismatch:
            return self._mark_incomplete(apply_id, mismatch)

        crash_injector(f"apply:{direction}:before_commit")
        terminal_conflict = False
        already_terminal = False
        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            transaction = connection.execute(
                "SELECT * FROM apply_transactions WHERE apply_id = ?",
                (str(apply_id),),
            ).fetchone()
            assert transaction is not None
            if transaction["phase"] == terminal_phase:
                already_terminal = True
                connection.commit()
            elif transaction["direction"] != direction:
                connection.execute(
                    """
                    UPDATE apply_transactions
                    SET phase = 'incomplete', detail = ?
                    WHERE apply_id = ?
                    """,
                    (
                        "terminal_direction_conflict",
                        str(apply_id),
                    ),
                )
                connection.commit()
                terminal_conflict = True
            else:
                expected_active = transaction["from_revision"]
                expected_reserved = transaction["target_revision"]
                target_revision = (
                    transaction["target_revision"]
                    if direction == "forward"
                    else transaction["from_revision"]
                )
                target_units_json = (
                    transaction["target_units_json"]
                    if direction == "forward"
                    else transaction["previous_units_json"]
                )
                updated = connection.execute(
                    """
                    UPDATE manager_state
                    SET active_revision = ?, reserved_revision = ?,
                        active_units_json = ?
                    WHERE singleton = 1
                      AND active_revision = ?
                      AND reserved_revision = ?
                    """,
                    (
                        target_revision,
                        target_revision,
                        target_units_json,
                        expected_active,
                        expected_reserved,
                    ),
                )
                if updated.rowcount != 1:
                    connection.execute(
                        """
                        UPDATE apply_transactions
                        SET phase = 'incomplete',
                            detail = 'terminal_manager_state_conflict'
                        WHERE apply_id = ?
                        """,
                        (str(apply_id),),
                    )
                    terminal_conflict = True
                else:
                    connection.execute(
                        """
                        UPDATE apply_transactions
                        SET phase = ?, detail = ''
                        WHERE apply_id = ?
                        """,
                        (terminal_phase, str(apply_id)),
                    )
                connection.commit()
        if not terminal_conflict and not already_terminal:
            crash_injector(f"apply:{direction}:after_commit")
        return self.apply_status(apply_id)

    def _intend_effect(
        self, apply_id: ApplyTransactionId, ordinal: int
    ) -> None:
        """Persist intent once without regressing an already settled effect."""

        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            connection.execute(
                """
                UPDATE apply_effects SET state = 'intended', detail = ''
                WHERE apply_id = ? AND ordinal = ? AND state = 'planned'
                """,
                (str(apply_id), ordinal),
            )
            connection.commit()

    def _execute_effect_serialized(
        self,
        *,
        apply_id: ApplyTransactionId,
        ordinal: int,
        action: str,
        spec: RuntimeUnitSpec,
        checkpoints: tuple[str, ...],
        crash_injector: CrashInjector,
        reconcile_settled: bool = False,
    ) -> bool:
        """Issue one effect while holding SQLite's cross-process writer lock.

        The intent is committed before this method starts. Holding the writer
        transaction across observe/effect/verify prevents concurrent recoveries
        from issuing the same external effect. A process crash releases the
        SQLite lock; the next recovery observes reality from the retained intent.
        """

        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            current = connection.execute(
                """
                SELECT state FROM apply_effects
                WHERE apply_id = ? AND ordinal = ?
                """,
                (str(apply_id), ordinal),
            ).fetchone()
            assert current is not None
            if current["state"] == "abandoned" or (
                current["state"] == "effected" and not reconcile_settled
            ):
                connection.commit()
                return True

            crash_injector(checkpoints[2])
            try:
                observation = self._adapter.observe(spec.runtime_unit_id)
                if not self._effect_is_observed(action, observation, spec):
                    if action == "start":
                        # A healthy exact process is never restarted. Any
                        # present non-exact process is first stopped so adapters
                        # whose start is idempotent by identity cannot preserve
                        # a stale process or stale specification.
                        if not self._observation_is_absent(observation):
                            self._adapter.stop(spec.runtime_unit_id)
                        self._adapter.start(spec)
                    else:
                        self._adapter.stop(spec.runtime_unit_id)
            except Exception as error:
                observation = self._safe_observe(spec.runtime_unit_id)
                if not self._effect_is_observed(action, observation, spec):
                    self._set_incomplete(
                        connection,
                        apply_id,
                        f"{action}:{spec.runtime_unit_id}:"
                        f"ambiguous:{type(error).__name__}",
                    )
                    connection.commit()
                    return False
            crash_injector(checkpoints[3])
            crash_injector(checkpoints[4])
            observation = self._safe_observe(spec.runtime_unit_id)
            if not self._effect_is_observed(action, observation, spec):
                self._set_incomplete(
                    connection,
                    apply_id,
                    f"{action}:{spec.runtime_unit_id}:observation_mismatch",
                )
                connection.commit()
                return False
            epoch = (
                None if observation is None else str(observation.runtime_epoch)
            )
            connection.execute(
                """
                UPDATE apply_effects
                SET state = 'effected', observed_epoch = ?,
                    detail = 'observed_after_effect'
                WHERE apply_id = ? AND ordinal = ?
                """,
                (epoch, str(apply_id), ordinal),
            )
            connection.commit()
        crash_injector(checkpoints[5])
        return True

    @staticmethod
    def _set_incomplete(
        connection: sqlite3.Connection,
        apply_id: ApplyTransactionId,
        detail: str,
    ) -> None:
        connection.execute(
            """
            UPDATE apply_transactions
            SET phase = 'incomplete', detail = ?
            WHERE apply_id = ?
            """,
            (detail, str(apply_id)),
        )

    def _effect_is_observed(
        self,
        action: str,
        observation: RuntimeUnitObservation | object | None,
        spec: RuntimeUnitSpec,
    ) -> bool:
        if observation is _OBSERVATION_UNAVAILABLE:
            return False
        if action == "stop":
            return self._observation_is_absent(observation)
        if (
            observation is None
            or observation.health not in _OPERATIONAL_HEALTH
        ):
            return False
        try:
            observed_fingerprint = self._adapter.observed_spec_fingerprint(
                spec.runtime_unit_id
            )
        except Exception:
            return False
        return observed_fingerprint == _unit_fingerprint(spec)

    @staticmethod
    def _observation_is_absent(
        observation: RuntimeUnitObservation | object | None,
    ) -> bool:
        return (
            observation is None
            or (
                observation is not _OBSERVATION_UNAVAILABLE
                and observation.health in _ABSENT_HEALTH
            )
        )

    def _target_units(
        self,
        apply_id: ApplyTransactionId,
        direction: str,
    ) -> dict[str, RuntimeUnitSpec]:
        row = self._transaction_row(apply_id)
        encoded = (
            row["target_units_json"]
            if direction == "forward"
            else row["previous_units_json"]
        )
        return _decode_units(encoded)

    def _append_unexpected_stop_intents(
        self,
        apply_id: ApplyTransactionId,
        direction: str,
        target_units: dict[str, RuntimeUnitSpec],
    ) -> bool:
        """Durably cover every currently observed unit outside the target."""

        try:
            observations = self._adapter.observe_all()
        except Exception as error:
            self._mark_incomplete(
                apply_id,
                f"observe_all:ambiguous:{type(error).__name__}",
            )
            return False

        unexpected_ids = sorted(
            {
                str(observation.runtime_unit_id)
                for observation in observations
                if not self._observation_is_absent(observation)
            }
            - set(target_units)
            - {str(self._manager_runtime_unit_id)}
        )
        if not unexpected_ids:
            return True

        with self._connect() as connection:
            connection.execute("BEGIN IMMEDIATE")
            existing_rows = connection.execute(
                """
                SELECT unit_json FROM apply_effects
                WHERE apply_id = ? AND direction = ? AND action = 'stop'
                """,
                (str(apply_id), direction),
            ).fetchall()
            existing_stop_ids = {
                next(iter(_decode_units(row["unit_json"])))
                for row in existing_rows
            }
            next_ordinal = int(
                connection.execute(
                    """
                    SELECT COALESCE(MAX(ordinal), -1) + 1
                    FROM apply_effects WHERE apply_id = ?
                    """,
                    (str(apply_id),),
                ).fetchone()[0]
            )
            for runtime_unit_id in unexpected_ids:
                if runtime_unit_id in existing_stop_ids:
                    continue
                spec = RuntimeUnitSpec(
                    runtime_unit_id=RuntimeUnitId(runtime_unit_id),
                    kind="observed_unexpected_process",
                )
                connection.execute(
                    """
                    INSERT INTO apply_effects (
                        apply_id, ordinal, direction, action, unit_json,
                        state, observed_epoch, detail
                    ) VALUES (?, ?, ?, 'stop', ?, 'planned', NULL, '')
                    """,
                    (
                        str(apply_id),
                        next_ordinal,
                        direction,
                        _encode_units((spec,)),
                    ),
                )
                next_ordinal += 1
            connection.commit()
        return True

    def _convergence_mismatch(
        self,
        target_units: dict[str, RuntimeUnitSpec],
    ) -> str:
        """Return a fail-closed reason unless observation equals the target."""

        try:
            observations = self._adapter.observe_all()
        except Exception as error:
            return f"observe_all:ambiguous:{type(error).__name__}"
        observed_by_id = {
            str(observation.runtime_unit_id): observation
            for observation in observations
            if not self._observation_is_absent(observation)
        }
        unexpected = sorted(
            set(observed_by_id)
            - set(target_units)
            - {str(self._manager_runtime_unit_id)}
        )
        if unexpected:
            return f"unexpected_runtime_units:{','.join(unexpected)}"
        for unit_id in _validate_and_sort(target_units):
            spec = target_units[unit_id]
            if not self._effect_is_observed(
                "start", observed_by_id.get(unit_id), spec
            ):
                return f"runtime_unit_not_converged:{unit_id}"
        return ""

    def _safe_observe(
        self, runtime_unit_id: RuntimeUnitId
    ) -> RuntimeUnitObservation | object | None:
        try:
            return self._adapter.observe(runtime_unit_id)
        except Exception:
            return _OBSERVATION_UNAVAILABLE

    def _mark_incomplete(
        self, apply_id: ApplyTransactionId, detail: str
    ) -> ApplyStatus:
        with self._connect() as connection:
            connection.execute(
                """
                UPDATE apply_transactions
                SET phase = 'incomplete', detail = ?
                WHERE apply_id = ?
                """,
                (detail, str(apply_id)),
            )
        return self.apply_status(apply_id)

    def _effect_rows(
        self, apply_id: ApplyTransactionId, direction: str
    ) -> tuple[sqlite3.Row, ...]:
        with self._connect() as connection:
            rows = connection.execute(
                """
                SELECT * FROM apply_effects
                WHERE apply_id = ? AND direction = ?
                ORDER BY ordinal
                """,
                (str(apply_id), direction),
            ).fetchall()
        return tuple(rows)

    def _transaction_row(
        self, apply_id: ApplyTransactionId
    ) -> sqlite3.Row:
        with self._connect() as connection:
            row = connection.execute(
                "SELECT * FROM apply_transactions WHERE apply_id = ?",
                (str(apply_id),),
            ).fetchone()
        if row is None:
            raise KeyError(str(apply_id))
        return row

    def _status_from_row(
        self,
        row: sqlite3.Row,
        *,
        phase: str | None = None,
        detail: str | None = None,
    ) -> ApplyStatus:
        with self._connect() as connection:
            affected_rows = connection.execute(
                """
                SELECT unit_json FROM apply_effects
                WHERE apply_id = ? AND direction = 'forward'
                ORDER BY ordinal
                """,
                (row["apply_id"],),
            ).fetchall()
        affected = tuple(
            dict.fromkeys(
                RuntimeUnitId(next(iter(_decode_units(item["unit_json"]))))
                for item in affected_rows
            )
        )
        return ApplyStatus(
            apply_id=ApplyTransactionId(row["apply_id"]),
            phase=phase or row["phase"],
            from_graph_revision=GraphRevision(row["from_revision"]),
            target_graph_revision=GraphRevision(row["target_revision"]),
            affected_runtime_units=affected,
            detail=row["detail"] if detail is None else detail,
        )
