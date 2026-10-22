"""Persistent blackboard values owned by the Program runner.

Only values are stored here. Declarations, initial values and formulas stay
in the Program, and running never changes the Program. Rows belong to one
Scenario project folder (its canonical absolute path, as Manager's committed
Apply records it) and key name: two same-named projects in different folders
never share values, and a moved or renamed folder starts with none. The file
lives with the installation, so copying or exporting a Scenario never carries
machine state.

Stored values follow Manager's committed Scenario activations. Every
publication after the canonical-empty bootstrap is one committed named Apply,
and Manager lists all of them for its publication history. A stored value is
kept unless an observed committed Apply no longer declares its key persistent
with the same structure. The register remembers which committed Applies it
has accounted for: a repeated or late notification changes nothing, and a
committed Apply it never observed is reported as the named fault
`applied_program_not_observed` (the values are kept and reconciled against
the current Program). A new publication history (Manager restarted with new
state) is accepted the same way: rows that fit the current Program stay.
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from threading import RLock
from typing import Any, Final

from manyforge_behavior.orchestration_ports import PersistentValueUncertainError
from manyforge_behavior.program import BlackboardKeyDeclaration

_LOGGER = logging.getLogger(__name__)

SCHEMA_VERSION: Final = "manyforge.persistent_key_register.v3"
# The previous layout keyed rows by Scenario id; such a file is moved aside
# (one bounded slot, never migrated) and the register starts empty.
_SUPERSEDED_SCHEMA_VERSION: Final = "manyforge.persistent_key_register.v2"
MAX_ROWS: Final = 4_096
MAX_VALUE_BYTES: Final = 65_536
MAX_EDITS: Final = 256
# Manager bounds one publication history to 256 named Applies.
MAX_RECONCILED_APPLIES: Final = 1_024
# Declared access that lets the operator change a key's value (Set, Reset, Clear).
OPERATOR_WRITABLE_ACCESS: Final = frozenset({"write", "read_write"})
_MAX_ID_BYTES: Final = 256
_MAX_PROJECT_PATH_BYTES: Final = 4_096
_MAX_DETAIL_BYTES: Final = 512


_EDIT_FIELDS: Final = frozenset(
    {
        "sequence",
        "project_path",
        "key",
        "action",
        "request_id",
        "source_label",
        "program_run_id",
        "node_instance_id",
        "node_attempt_id",
        "reason",
        "detail",
    }
)
_DOCUMENT_FIELDS: Final = frozenset(
    {
        "schema_version",
        "sequence",
        "rows",
        "edits",
        "activation",
        "reconciled_applies",
        "unobserved_applies",
    }
)

# Why a stored value was dropped; audited with the drop.
REASON_NOT_DECLARED: Final = "key_not_declared_persistent"
REASON_DECLARATION_CHANGED: Final = "key_declaration_changed"
REASON_VALUE_INVALID: Final = "value_invalid"
# The named fault for a committed Apply the register never observed; it
# drops nothing but stays visible (register audit, values page, run audit).
FAULT_APPLY_NOT_OBSERVED: Final = "applied_program_not_observed"


class PersistentKeyRegisterError(RuntimeError):
    """The register cannot be read or written; a refused change changed nothing."""


class PersistentKeyRegisterVersionError(PersistentKeyRegisterError):
    """The file has another layout; it is left untouched, never migrated."""

    code: Final = "persistent_key_register_version_unsupported"


class PersistentKeyStorageUncertainError(
    PersistentKeyRegisterError, PersistentValueUncertainError
):
    """The change is in the file and in memory, but may not survive a crash.

    The register refuses further changes until it is reconciled with disk.
    """


@dataclass(frozen=True, slots=True)
class PersistentKeyNotice:
    """One dropped stored value; ``key`` is None when a project's rows went."""

    project_path: str
    key: str | None
    reason: str
    detail: str | None = None


@dataclass(frozen=True, slots=True)
class CommittedActivation:
    """Manager's current committed Scenario activation.

    ``committed_applies`` maps every committed Apply record of the current
    publication history, this one included, to its Scenario project folder.
    """

    record_digest: str
    project_path: str
    declarations: Mapping[str, BlackboardKeyDeclaration]
    committed_applies: Mapping[str, str]

    def __post_init__(self) -> None:
        _require_id(self.record_digest, field="record_digest")
        _require_project(self.project_path)
        if (
            len(self.committed_applies) > MAX_RECONCILED_APPLIES
            or self.committed_applies.get(self.record_digest) != self.project_path
        ):
            raise PersistentKeyRegisterError(
                "committed activation must be one of its history's committed Applies"
            )


@dataclass(frozen=True, slots=True)
class PersistentKeyReconciliation:
    """``reconciled``: rows now follow this activation. ``duplicate``: it is the
    activation already reconciled. ``stale``: a newer one was reconciled
    already. Only ``reconciled`` changes anything.

    ``unobserved`` names committed Applies (record digest, project folder) this
    reconciliation found that the register never observed;
    ``history_changed`` says the activation starts a new publication history.
    """

    outcome: str
    dropped: tuple[PersistentKeyNotice, ...] = field(default=())
    unobserved: tuple[tuple[str, str], ...] = field(default=())
    history_changed: bool = False


@dataclass(frozen=True, slots=True)
class PersistentKeyEdit:
    """Who made one value change; audited with the change itself.

    Operator edits name their request; writes from a run name the run and,
    when a node wrote, the node instance and attempt.
    """

    request_id: str | None
    source_label: str
    program_run_id: str | None = None
    node_instance_id: str | None = None
    node_attempt_id: str | None = None

    @classmethod
    def from_write_metadata(cls, metadata: Mapping[str, Any]) -> "PersistentKeyEdit":
        """The identity a run's value store attached to one write."""

        return cls(
            request_id=metadata.get("request_id"),
            source_label=metadata.get("value_source", "program_node"),
            program_run_id=metadata.get("program_run_id"),
            node_instance_id=metadata.get("node_instance_id"),
            node_attempt_id=metadata.get("node_attempt_id"),
        )


_RECONCILIATION_EDIT: Final = PersistentKeyEdit(None, "committed_apply")


def declaration_fingerprint(declaration: BlackboardKeyDeclaration) -> str:
    """Identify the complete declared structure, not only the type name.

    `initial:` and description are not part of it: changing them keeps the
    stored value.
    """

    structure = {
        "type": declaration.type,
        "fields": {
            name: field.type for name, field in sorted(declaration.fields.items())
        },
    }
    encoded = json.dumps(structure, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def _canonical(value: Any) -> str:
    try:
        return json.dumps(
            value, sort_keys=True, separators=(",", ":"), allow_nan=False
        )
    except (TypeError, ValueError) as error:
        raise PersistentKeyRegisterError(
            f"value is not storable JSON: {error}"
        ) from error


def _require_id(value: str, *, field: str) -> str:
    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > _MAX_ID_BYTES
    ):
        raise PersistentKeyRegisterError(f"{field} must be bounded non-empty text")
    return value


def _require_project(value: str) -> str:
    """A Scenario project folder path: the register's Scenario identity."""

    if (
        not isinstance(value, str)
        or not value
        or len(value.encode("utf-8")) > _MAX_PROJECT_PATH_BYTES
    ):
        raise PersistentKeyRegisterError(
            "project_path must be bounded non-empty text"
        )
    return value


def _bounded_detail(detail: str | None) -> str | None:
    if detail is None:
        return None
    encoded = str(detail).encode("utf-8")[:_MAX_DETAIL_BYTES]
    return encoded.decode("utf-8", errors="ignore") or None


def _audit(
    edit: PersistentKeyEdit | Mapping[str, Any] | None,
    action: str,
    project_path: str,
    key: str | None,
    *,
    reason: str | None = None,
    detail: str | None = None,
) -> dict[str, Any] | None:
    if edit is None:
        return None
    if isinstance(edit, Mapping):
        edit = PersistentKeyEdit.from_write_metadata(edit)
    record: dict[str, Any] = {
        "project_path": project_path,
        "key": key,
        "action": action,
        "source_label": _require_id(edit.source_label, field="source_label"),
        "reason": reason,
        "detail": _bounded_detail(detail),
    }
    for field_name in (
        "request_id",
        "program_run_id",
        "node_instance_id",
        "node_attempt_id",
    ):
        value = getattr(edit, field_name)
        record[field_name] = (
            None if value is None else _require_id(value, field=field_name)
        )
    return record


def _fault_audit(record_digest: str, project_path: str) -> dict[str, Any]:
    record = _audit(
        _RECONCILIATION_EDIT,
        "fault",
        project_path,
        None,
        reason=FAULT_APPLY_NOT_OBSERVED,
        detail=f"committed Apply {record_digest} was not observed",
    )
    assert record is not None
    return record


def _drop_audit(notice: PersistentKeyNotice, edit: PersistentKeyEdit) -> dict[str, Any]:
    record = _audit(
        edit,
        "dropped",
        notice.project_path,
        notice.key,
        reason=notice.reason,
        detail=notice.detail,
    )
    assert record is not None
    return record


class PersistentKeyRegister:
    """One small versioned file; every change is committed before it returns."""

    def __init__(self, path: Path) -> None:
        if not isinstance(path, Path) or not path.is_absolute():
            raise ValueError("persistent-key register path must be absolute")
        self._path = path
        self._lock = RLock()
        self._sequence = 0
        # (project_path, key) -> {"fingerprint", "value", "sequence"}
        self._rows: dict[tuple[str, str], dict[str, Any]] = {}
        # Recent attributed changes, oldest first, written with the change.
        # Only an audit list: the cycle journal refuses a resent request.
        self._edits: list[dict[str, Any]] = []
        # The committed activation the rows follow, and every committed Apply
        # of its publication history the register has accounted for.
        self._activation: dict[str, str] | None = None
        self._reconciled: frozenset[str] = frozenset()
        # Committed Applies of the current history never observed, with their
        # Scenario: the `applied_program_not_observed` fault.
        self._unobserved: dict[str, str] = {}
        # Values a run start dropped, for that run's audit (see take_notices).
        self._notices: list[PersistentKeyNotice] = []
        # Set when a replaced file may not be durable; see _reconcile_disk.
        self._uncertain = False
        self._load()

    @property
    def path(self) -> Path:
        return self._path

    def _load(self) -> None:
        try:
            raw = self._path.read_text(encoding="utf-8")
        except FileNotFoundError:
            return
        except OSError as error:
            raise PersistentKeyRegisterError(
                f"persistent-key register is unreadable: {error}"
            ) from error
        try:
            document = json.loads(raw)
        except ValueError as error:
            raise PersistentKeyRegisterError(
                "persistent-key register is not valid JSON"
            ) from error
        found = document.get("schema_version") if type(document) is dict else None
        if found == _SUPERSEDED_SCHEMA_VERSION:
            superseded = self._path.with_name(f"{self._path.name}.v2.superseded")
            try:
                os.replace(self._path, superseded)
            except OSError as error:
                raise PersistentKeyRegisterError(
                    f"persistent-key register {self._path} has the superseded "
                    f"layout and could not be moved aside: {error}"
                ) from error
            _LOGGER.warning(
                "persistent-key register %s had the superseded layout %s; it "
                "was moved aside to %s and the register starts empty",
                self._path,
                found,
                superseded,
            )
            return
        if found != SCHEMA_VERSION:
            raise PersistentKeyRegisterVersionError(
                f"persistent-key register {self._path} has schema_version "
                f"{found!r}; this Program runner reads only {SCHEMA_VERSION!r}. "
                "It was left untouched: move it aside to start with an empty "
                "register."
            )
        activation = document.get("activation")
        reconciled = document.get("reconciled_applies")
        unobserved = document.get("unobserved_applies")
        if (
            set(document) != _DOCUMENT_FIELDS
            or type(document.get("sequence")) is not int
            or type(document.get("rows")) is not list
            or len(document["rows"]) > MAX_ROWS
            or type(document.get("edits")) is not list
            or len(document["edits"]) > MAX_EDITS
            or not (
                activation is None
                or (
                    type(activation) is dict
                    and set(activation) == {"record_digest", "project_path"}
                )
            )
            or type(reconciled) is not list
            or len(reconciled) > MAX_RECONCILED_APPLIES
            or reconciled != sorted(set(reconciled))
            or type(unobserved) is not list
            or len(unobserved) > MAX_RECONCILED_APPLIES
            or any(
                type(row) is not dict or set(row) != {"record_digest", "project_path"}
                for row in unobserved
            )
        ):
            raise PersistentKeyRegisterError(
                "persistent-key register has an unsupported layout"
            )
        rows: dict[tuple[str, str], dict[str, Any]] = {}
        for row in document["rows"]:
            if type(row) is not dict or set(row) != {
                "project_path",
                "key",
                "fingerprint",
                "value",
                "sequence",
            }:
                raise PersistentKeyRegisterError(
                    "persistent-key register has a malformed row"
                )
            identity = (
                _require_project(row["project_path"]),
                _require_id(row["key"], field="key"),
            )
            if identity in rows:
                raise PersistentKeyRegisterError(
                    "persistent-key register repeats a row"
                )
            rows[identity] = {
                "fingerprint": row["fingerprint"],
                "value": row["value"],
                "sequence": row["sequence"],
            }
        for edit in document["edits"]:
            if type(edit) is not dict or set(edit) != _EDIT_FIELDS:
                raise PersistentKeyRegisterError(
                    "persistent-key register has a malformed edit record"
                )
        if activation is not None:
            _require_id(activation["record_digest"], field="record_digest")
            _require_project(activation["project_path"])
            if activation["record_digest"] not in reconciled:
                raise PersistentKeyRegisterError(
                    "persistent-key register activation is not reconciled"
                )
        for digest in reconciled:
            _require_id(digest, field="record_digest")
        unobserved_applies = {
            _require_id(row["record_digest"], field="record_digest"): _require_project(row["project_path"])
            for row in unobserved
        }
        if not set(unobserved_applies) <= set(reconciled):
            raise PersistentKeyRegisterError(
                "persistent-key register reports an Apply it never accounted for"
            )
        self._rows = rows
        self._edits = document["edits"]
        self._sequence = document["sequence"]
        self._activation = None if activation is None else dict(activation)
        self._reconciled = frozenset(reconciled)
        self._unobserved = unobserved_applies

    def _write(
        self,
        rows: dict[tuple[str, str], dict[str, Any]],
        sequence: int,
        audits: list[dict[str, Any]] | dict[str, Any] | None = None,
        *,
        activation: dict[str, str] | None = None,
        reconciled: frozenset[str] | None = None,
        unobserved: dict[str, str] | None = None,
    ) -> None:
        if isinstance(audits, dict):
            audits = [audits]
        edits = self._edits
        if audits:
            edits = [*edits, *({"sequence": sequence, **audit} for audit in audits)][
                -MAX_EDITS:
            ]
        activation = self._activation if activation is None else activation
        reconciled = self._reconciled if reconciled is None else reconciled
        unobserved = self._unobserved if unobserved is None else unobserved
        document = {
            "schema_version": SCHEMA_VERSION,
            "sequence": sequence,
            "rows": [
                {"project_path": row_project, "key": row_key, **row}
                for (row_project, row_key), row in sorted(rows.items())
            ],
            "edits": edits,
            "activation": activation,
            "reconciled_applies": sorted(reconciled),
            "unobserved_applies": [
                {"record_digest": digest, "project_path": project}
                for digest, project in sorted(unobserved.items())
            ],
        }
        encoded = json.dumps(document, sort_keys=True, indent=1, allow_nan=False)
        temporary = self._path.with_name(f".{self._path.name}.tmp")
        try:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            with open(temporary, "w", encoding="utf-8") as handle:
                handle.write(encoded)
                handle.flush()
                os.fsync(handle.fileno())
            os.replace(temporary, self._path)
        except OSError as error:
            raise PersistentKeyRegisterError(
                f"persistent-key register could not be written: {error}"
            ) from error
        # The file now holds the change, so memory follows it even if the
        # directory entry cannot be made durable below.
        self._rows = rows
        self._edits = edits
        self._sequence = sequence
        self._activation = None if activation is None else dict(activation)
        self._reconciled = reconciled
        self._unobserved = dict(unobserved)
        try:
            self._sync_directory()
        except OSError as error:
            self._uncertain = True
            raise PersistentKeyStorageUncertainError(
                "persistent-key register was written but may not be durable: "
                f"{error}"
            ) from error

    def _sync_directory(self) -> None:
        directory = os.open(self._path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    def _reconcile_disk(self) -> None:
        """Before a change, re-read a possibly non-durable file and sync it.

        Until both succeed every change is refused; nothing is written.
        """

        if not self._uncertain:
            return
        try:
            self._load()
            self._sync_directory()
        except (OSError, PersistentKeyRegisterError) as error:
            raise PersistentKeyRegisterError(
                "persistent-key register is not reconciled after a write that "
                f"may not be durable: {error}"
            ) from error
        self._uncertain = False

    def reconcile(self, activation: CommittedActivation) -> PersistentKeyReconciliation:
        """Make the stored rows follow one committed Scenario activation.

        The activation's Scenario keeps every row its Program still declares
        persistent with the same structure; the others are dropped with a
        notice. Nothing is dropped because a notification was missed: a
        committed Apply of this history the register never observed is
        recorded as the `applied_program_not_observed` fault instead, and a
        new publication history is accepted. All changes are written in one
        step. An activation already accounted for changes nothing, so
        repeated or late notifications are harmless.
        """

        if not isinstance(activation, CommittedActivation):
            raise TypeError("reconcile requires one CommittedActivation")
        with self._lock:
            # Settle a write that may not be durable first: memory may count
            # an activation as reconciled that the disk does not yet hold.
            self._reconcile_disk()
            if activation.record_digest in self._reconciled:
                current = self._activation
                return PersistentKeyReconciliation(
                    "duplicate"
                    if current is not None
                    and current["record_digest"] == activation.record_digest
                    else "stale"
                )
            committed = dict(activation.committed_applies)
            history_changed = not self._reconciled <= set(committed)
            if history_changed:
                # The faults named Applies of the previous history.
                seen: frozenset[str] = frozenset()
                unobserved: dict[str, str] = {}
            else:
                seen = self._reconciled
                unobserved = dict(self._unobserved)
            missed = (
                ()
                if self._activation is None and not history_changed
                # A register that never followed any activation has nothing
                # it could have missed.
                else tuple(
                    sorted(
                        (digest, committed[digest])
                        for digest in set(committed) - seen - {activation.record_digest}
                    )
                )
            )
            unobserved.update(missed)
            rows = dict(self._rows)
            notices: list[PersistentKeyNotice] = []
            for (row_project, key), row in sorted(rows.items()):
                if row_project != activation.project_path:
                    continue
                declaration = activation.declarations.get(key)
                if declaration is None or declaration.persistence != "persistent":
                    reason = REASON_NOT_DECLARED
                elif row["fingerprint"] != declaration_fingerprint(declaration):
                    reason = REASON_DECLARATION_CHANGED
                else:
                    continue
                del rows[(row_project, key)]
                notices.append(PersistentKeyNotice(row_project, key, reason))
            self._write(
                rows,
                self._sequence + 1,
                [
                    *(_fault_audit(digest, scenario) for digest, scenario in missed),
                    *(_drop_audit(notice, _RECONCILIATION_EDIT) for notice in notices),
                ],
                activation={
                    "record_digest": activation.record_digest,
                    "project_path": activation.project_path,
                },
                reconciled=frozenset(committed),
                unobserved=unobserved,
            )
            return PersistentKeyReconciliation(
                "reconciled", tuple(notices), missed, history_changed
            )

    def unobserved_applies(self, project_path: str) -> tuple[str, ...]:
        """Record digests of this history's committed Applies of the project
        that were never observed (the `applied_program_not_observed` fault)."""

        _require_project(project_path)
        with self._lock:
            return tuple(
                digest
                for digest, project in sorted(self._unobserved.items())
                if project == project_path
            )

    def restore(
        self,
        project_path: str,
        declarations: Mapping[str, BlackboardKeyDeclaration],
    ) -> Mapping[str, Any]:
        """Return the Scenario's stored values that fit the committed Program.

        Only after the Scenario's committed activation was reconciled, so a
        row is never restored against declarations it was not checked
        against. Restoring never writes.
        """

        _require_project(project_path)
        with self._lock:
            if (
                self._uncertain
                or self._activation is None
                or self._activation["project_path"] != project_path
            ):
                # Nothing is restored while a write may not be durable.
                return {}
            restored: dict[str, Any] = {}
            for (row_project, key), row in self._rows.items():
                declaration = declarations.get(key)
                if (
                    row_project == project_path
                    and declaration is not None
                    and declaration.persistence == "persistent"
                    and row["fingerprint"] == declaration_fingerprint(declaration)
                ):
                    restored[key] = json.loads(_canonical(row["value"]))
            return restored

    def drop(
        self,
        project_path: str,
        key: str,
        *,
        reason: str,
        detail: str | None = None,
        edit: PersistentKeyEdit | Mapping[str, Any] | None = None,
    ) -> None:
        """Forget a stored value the committed Program no longer accepts.

        The notice is kept for the starting run's audit even when the row
        could not be removed; a failed removal raises.
        """

        identity = (
            _require_project(project_path),
            _require_id(key, field="key"),
        )
        notice = PersistentKeyNotice(
            project_path, key, _require_id(reason, field="reason"), _bounded_detail(detail)
        )
        if edit is None:
            edit = PersistentKeyEdit(None, "run_start")
        elif isinstance(edit, Mapping):
            edit = PersistentKeyEdit.from_write_metadata(
                {"value_source": "run_start", **edit}
            )
        with self._lock:
            self._notices.append(notice)
            self._reconcile_disk()
            if identity not in self._rows:
                return
            rows = dict(self._rows)
            del rows[identity]
            self._write(rows, self._sequence + 1, _drop_audit(notice, edit))

    def commit(
        self,
        project_path: str,
        declaration: BlackboardKeyDeclaration,
        value: Any,
        *,
        edit: PersistentKeyEdit | Mapping[str, Any] | None = None,
    ) -> None:
        """Store one value; `edit` (or a run's write metadata) is audited with it.

        A value is written only when it changes: a stored row with the same
        structure and value is kept as it is, with no new sequence or audit.
        An absent row is never equal, even to `initial:`.
        """

        identity = (
            _require_project(project_path),
            _require_id(declaration.key, field="key"),
        )
        if declaration.persistence != "persistent":
            raise PersistentKeyRegisterError(
                f"blackboard key {declaration.key!r} is not persistent"
            )
        encoded = _canonical(value)
        if len(encoded.encode("utf-8")) > MAX_VALUE_BYTES:
            raise PersistentKeyRegisterError(
                f"blackboard key {declaration.key!r} value exceeds "
                f"{MAX_VALUE_BYTES} bytes"
            )
        fingerprint = declaration_fingerprint(declaration)
        with self._lock:
            self._reconcile_disk()
            stored = self._rows.get(identity)
            if (
                stored is not None
                and stored["fingerprint"] == fingerprint
                and _canonical(stored["value"]) == encoded
            ):
                return
            rows = dict(self._rows)
            if identity not in rows and len(rows) >= MAX_ROWS:
                raise PersistentKeyRegisterError(
                    f"persistent-key register is full ({MAX_ROWS} rows)"
                )
            sequence = self._sequence + 1
            rows[identity] = {
                "fingerprint": fingerprint,
                "value": json.loads(encoded),
                "sequence": sequence,
            }
            self._write(rows, sequence, _audit(edit, "set", *identity))

    def remove(
        self,
        project_path: str,
        key: str,
        *,
        edit: PersistentKeyEdit | Mapping[str, Any] | None = None,
    ) -> bool:
        """Forget one stored value so the key starts again from `initial:`."""

        identity = (
            _require_project(project_path),
            _require_id(key, field="key"),
        )
        with self._lock:
            self._reconcile_disk()
            if identity not in self._rows:
                return False
            rows = dict(self._rows)
            del rows[identity]
            self._write(
                rows, self._sequence + 1, _audit(edit, "reset", *identity)
            )
            return True

    def clear(
        self,
        project_path: str,
        declarations: Mapping[str, BlackboardKeyDeclaration],
        *,
        edit: PersistentKeyEdit | None = None,
    ) -> int:
        """Reset every writable key of one Scenario at once.

        Exactly one Reset per key the committed `declarations` make writable
        for the operator: its stored value is forgotten, so it starts again
        from `initial:` (or empty). A read key keeps its stored value.
        """

        _require_project(project_path)
        writable = {
            key
            for key, declaration in declarations.items()
            if declaration.access in OPERATOR_WRITABLE_ACCESS
        }
        with self._lock:
            self._reconcile_disk()
            rows = {
                identity: row
                for identity, row in self._rows.items()
                if identity[0] != project_path or identity[1] not in writable
            }
            removed = len(self._rows) - len(rows)
            if removed:
                self._write(
                    rows,
                    self._sequence + 1,
                    _audit(edit, "clear", project_path, None),
                )
            return removed

    @property
    def sequence(self) -> int:
        """Advances with every change of any Scenario; never repeats."""

        with self._lock:
            return self._sequence

    @property
    def activation(self) -> Mapping[str, str] | None:
        """The committed activation the rows follow, if any was reconciled."""

        with self._lock:
            return None if self._activation is None else dict(self._activation)

    def values(self, project_path: str) -> dict[str, dict[str, Any]]:
        _require_project(project_path)
        with self._lock:
            return {
                key: {
                    "value": json.loads(_canonical(row["value"])),
                    "sequence": row["sequence"],
                    "fingerprint": row["fingerprint"],
                }
                for (row_project, key), row in sorted(self._rows.items())
                if row_project == project_path
            }

    def edits(self, project_path: str) -> list[dict[str, Any]]:
        """Recent attributed changes of one Scenario, oldest first."""

        _require_project(project_path)
        with self._lock:
            return [
                dict(edit) for edit in self._edits if edit["project_path"] == project_path
            ]

    def take_notices(self) -> tuple[PersistentKeyNotice, ...]:
        """Values dropped while a run's value store was built (see drop)."""

        with self._lock:
            notices = tuple(self._notices)
            self._notices.clear()
            return notices


__all__ = [
    "CommittedActivation",
    "MAX_EDITS",
    "MAX_RECONCILED_APPLIES",
    "MAX_ROWS",
    "MAX_VALUE_BYTES",
    "PersistentKeyEdit",
    "PersistentKeyNotice",
    "PersistentKeyReconciliation",
    "PersistentKeyRegister",
    "PersistentKeyRegisterError",
    "PersistentKeyRegisterVersionError",
    "PersistentKeyStorageUncertainError",
    "FAULT_APPLY_NOT_OBSERVED",
    "REASON_DECLARATION_CHANGED",
    "REASON_NOT_DECLARED",
    "REASON_VALUE_INVALID",
    "declaration_fingerprint",
]
