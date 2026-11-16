"""Persistent blackboard values owned by the Program runner.

Only values are stored here. Declarations, initial values and formulas stay
in the Program, and running never changes the Program. Rows belong to one
Scenario project folder (its canonical absolute path, as Manager's committed
Apply records it) and key name: two same-named projects in different folders
never share values, and a moved or renamed folder starts with none. The file
lives with the installation, so copying or exporting a Scenario never carries
machine state.

Stored values follow the Program the runner actually activates: on every
wake it reads Manager's current committed Scenario activation and reconciles
the rows to it. A stored value is kept unless that activation's Program no
longer declares its key persistent with the same structure. Applies the runner
never activated (a later one superseded them first) have no effect, and a
repeated wake, or a late read of an older activation, changes nothing.
"""

from __future__ import annotations

import hashlib
import json
import logging
import os
import re
from collections.abc import Mapping
from dataclasses import dataclass, field
from pathlib import Path
from threading import Condition, Lock, RLock, Thread
from typing import Any, Final

from manyforge_behavior.program import BlackboardKeyDeclaration

_LOGGER = logging.getLogger(__name__)

FORMAT_VERSION: Final = 5
SCHEMA_VERSION: Final = f"manyforge.persistent_key_register.v{FORMAT_VERSION}"
# The register file is named by its format version, and only that file is
# opened or created. Files of an earlier format (the unversioned name or a
# lower version) are never opened, moved, rewritten or deleted.
REGISTER_FILE_NAME: Final = f"persistent-keys.v{FORMAT_VERSION}.json"
_OLDER_FILE_NAME: Final = re.compile(r"persistent-keys(?:\.v([1-9][0-9]{0,8}))?\.json")
MAX_ROWS: Final = 4_096
MAX_VALUE_BYTES: Final = 65_536
MAX_EDITS: Final = 256
# Declared access that lets the operator change a key's value (Set, Reset, Clear).
OPERATOR_WRITABLE_ACCESS: Final = frozenset({"write", "read_write"})
_MAX_ID_BYTES: Final = 256
_MAX_PROJECT_PATH_BYTES: Final = 4_096
_MAX_DETAIL_BYTES: Final = 512
# The application start ``A`` in its only text form (16 lowercase hex, not 0).
_APPLICATION_START_ID: Final = re.compile(r"[0-9a-f]{16}")


_EDIT_FIELDS: Final = frozenset(
    {
        "sequence",
        "project_path",
        "key",
        "action",
        "application_start_id",
        "request_key",
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
    }
)

# Why a stored value was dropped; audited with the drop.
REASON_NOT_DECLARED: Final = "key_not_declared_persistent"
REASON_DECLARATION_CHANGED: Final = "key_declaration_changed"
REASON_VALUE_INVALID: Final = "value_invalid"
# The stored activation: which committed Apply the rows follow, and its order
# (application start, Apply number).
_ACTIVATION_FIELDS: Final = frozenset(
    {"record_digest", "project_path", "application_start_id", "apply_id"}
)


class PersistentKeyRegisterError(RuntimeError):
    """The register file cannot be read, or a change is refused; a refused
    change changed nothing."""


class PersistentKeyRegisterVersionError(PersistentKeyRegisterError):
    """The file has another layout; it is left untouched, never migrated."""

    code: Final = "persistent_key_register_version_unsupported"


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

    ``apply_id`` is its Manager Apply number, counted from 1 in each
    application start (contract 1.16.1): within one start a higher number is a
    later activation.
    """

    record_digest: str
    project_path: str
    declarations: Mapping[str, BlackboardKeyDeclaration]
    apply_id: int

    def __post_init__(self) -> None:
        _require_id(self.record_digest, field="record_digest")
        _require_project(self.project_path)
        _require_number(self.apply_id, field="apply_id")


@dataclass(frozen=True, slots=True)
class PersistentKeyReconciliation:
    """``reconciled``: rows now follow this activation. ``duplicate``: it is the
    activation already reconciled. ``stale``: a later activation of this
    application start was reconciled already. Only ``reconciled`` changes
    anything.
    """

    outcome: str
    dropped: tuple[PersistentKeyNotice, ...] = field(default=())


@dataclass(frozen=True, slots=True)
class PersistentKeyEdit:
    """Who made one value change; audited with the change itself.

    Operator edits name their request (its request key, 32 hex); writes from
    a run name the run number and, when a node wrote, the node instance and
    attempt number. Run and attempt numbers restart with every application
    start: the register records the start that wrote each change with it.
    """

    request_key: str | None
    source_label: str
    program_run_id: int | None = None
    node_instance_id: str | None = None
    node_attempt_id: int | None = None

    @classmethod
    def from_write_metadata(cls, metadata: Mapping[str, Any]) -> "PersistentKeyEdit":
        """The identity a run's value store attached to one write."""

        return cls(
            request_key=metadata.get("request_key"),
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


def _require_number(value: int, *, field: str) -> int:
    if type(value) is not int or value < 1:
        raise PersistentKeyRegisterError(f"{field} must be a positive integer")
    return value


def _require_start_id(value: str) -> str:
    if (
        not isinstance(value, str)
        or _APPLICATION_START_ID.fullmatch(value) is None
        or value == "0" * 16
    ):
        raise PersistentKeyRegisterError(
            "application_start_id must be 16 lowercase hex characters, not all zeros"
        )
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
    for field_name in ("request_key", "node_instance_id"):
        value = getattr(edit, field_name)
        record[field_name] = (
            None if value is None else _require_id(value, field=field_name)
        )
    for field_name in ("program_run_id", "node_attempt_id"):
        value = getattr(edit, field_name)
        record[field_name] = (
            None if value is None else _require_number(value, field=field_name)
        )
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
    """One small versioned file, always rewritten whole by write-then-rename.

    Memory is the authority while the runner runs: every change changes
    memory and returns, every reader reads memory, and the register's writer
    thread then writes the latest state to the file. The file is a copy for
    the next start, read once by the constructor.

    The file is ``REGISTER_FILE_NAME`` in ``directory``. ``application_start_id``
    is this application start ``A`` (16 hex): every change written here
    records it, so a change keeps the start that wrote it when it is read
    under a later start.
    """

    def __init__(self, directory: Path, *, application_start_id: str) -> None:
        if not isinstance(directory, Path) or not directory.is_absolute():
            raise ValueError("persistent-key register directory must be absolute")
        self._path = directory / REGISTER_FILE_NAME
        self._application_start_id = _require_start_id(application_start_id)
        # Lock order: _lock, then _writer_cv. _lock guards memory only; no
        # file call is made under either. Only the writer thread writes the
        # file, one state after the other, so a newer state is never
        # overwritten by an older one.
        self._lock = RLock()
        # Memory changes counted (under _lock), and the count the file last
        # holds (writer thread only).
        self._generation = 0
        self._written = 0
        self._writer_cv = Condition(Lock())
        self._writer_wanted = False
        self._writer_busy = False
        self._writer_closing = False
        self._write_failures = 0
        self._sequence = 0
        # (project_path, key) -> {"fingerprint", "value", "sequence"}
        self._rows: dict[tuple[str, str], dict[str, Any]] = {}
        # Recent attributed changes, oldest first, written with the change.
        # Only an audit list: the cycle journal refuses a resent request.
        self._edits: list[dict[str, Any]] = []
        # The committed activation the rows follow.
        self._activation: dict[str, str] | None = None
        # Values a run start dropped, for that run's audit (see take_notices).
        self._notices: list[PersistentKeyNotice] = []
        self._load()
        self._writer = Thread(
            target=self._writer_loop, name="persistent-key-writer", daemon=True
        )
        self._writer.start()

    @property
    def path(self) -> Path:
        return self._path

    @property
    def write_failures(self) -> int:
        """Failed file writes of the writer thread since this start."""

        with self._writer_cv:
            return self._write_failures

    def wait_written(self, timeout_s: float | None = None) -> bool:
        """Wait until the writer has handled every change made so far."""

        with self._writer_cv:
            return self._writer_cv.wait_for(
                lambda: not self._writer_wanted and not self._writer_busy, timeout_s
            )

    def close(self, timeout_s: float) -> bool:
        """Write the latest state and stop the writer, within ``timeout_s``.

        False when the writer has not finished in time: the file then holds
        the last complete state written, without the newest changes.
        """

        with self._writer_cv:
            self._writer_closing = True
            self._writer_cv.notify_all()
        self._writer.join(timeout_s)
        if self._writer.is_alive():
            _LOGGER.warning(
                "stored values: the last changes were not written within %.1f s",
                timeout_s,
            )
            return False
        return True

    def _writer_loop(self) -> None:
        while True:
            with self._writer_cv:
                while not self._writer_wanted and not self._writer_closing:
                    self._writer_cv.wait()
                self._writer_wanted = False
                self._writer_busy = True
                closing = self._writer_closing
            self._flush()
            with self._writer_cv:
                self._writer_busy = False
                self._writer_cv.notify_all()
            if closing:
                return

    def _flush(self) -> None:
        """Write the latest state if memory is ahead of the file (writer thread)."""

        with self._lock:
            generation = self._generation
            if generation == self._written:
                return
            # Memory state is replaced, never changed in place, so these
            # references stay as taken while they are encoded unlocked.
            state = (
                self._rows,
                self._edits,
                self._sequence,
                self._activation,
            )
        try:
            self._replace_file(self._encode(*state))
            self._sync_directory()
        except Exception:
            with self._writer_cv:
                self._write_failures += 1
            _LOGGER.exception(
                "stored values could not be written; the next change or "
                "shutdown writes the latest state again"
            )
            return
        self._written = generation

    def _log_older_files(self) -> None:
        """Name, in one line, register files of an earlier format left beside it."""
        try:
            names = os.listdir(self._path.parent)
        except OSError:
            return
        older = sorted(
            str(self._path.parent / name)
            for name in names
            if (match := _OLDER_FILE_NAME.fullmatch(name)) is not None
            and (match.group(1) is None or int(match.group(1)) < FORMAT_VERSION)
        )
        if older:
            _LOGGER.warning(
                "stored values start empty: %s %s in an older format and %s left in place",
                ", ".join(older),
                "is" if len(older) == 1 else "are",
                "was" if len(older) == 1 else "were",
            )

    def _load(self) -> None:
        try:
            raw = self._path.read_text(encoding="utf-8")
        except FileNotFoundError:
            self._log_older_files()
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
        if found != SCHEMA_VERSION:
            raise PersistentKeyRegisterVersionError(
                f"persistent-key register {self._path} has schema_version "
                f"{found!r}; this Program runner reads only {SCHEMA_VERSION!r}. "
                "It was left untouched: move it aside to start with an empty "
                "register."
            )
        activation = document.get("activation")
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
                    and set(activation) == _ACTIVATION_FIELDS
                )
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
            _require_start_id(activation["application_start_id"])
            _require_number(activation["apply_id"], field="apply_id")
        self._rows = rows
        self._edits = document["edits"]
        self._sequence = document["sequence"]
        self._activation = None if activation is None else dict(activation)

    def _write(
        self,
        rows: dict[tuple[str, str], dict[str, Any]],
        sequence: int,
        audits: list[dict[str, Any]] | dict[str, Any] | None = None,
        *,
        activation: dict[str, str] | None = None,
    ) -> None:
        """Change memory (caller holds _lock) and wake the writer thread,
        which writes the file afterwards."""

        if isinstance(audits, dict):
            audits = [audits]
        edits = self._edits
        if audits:
            edits = [
                *edits,
                *(
                    {
                        "sequence": sequence,
                        "application_start_id": self._application_start_id,
                        **audit,
                    }
                    for audit in audits
                ),
            ][-MAX_EDITS:]
        activation = self._activation if activation is None else activation
        self._rows = rows
        self._edits = edits
        self._sequence = sequence
        self._activation = None if activation is None else dict(activation)
        self._generation += 1
        with self._writer_cv:
            self._writer_wanted = True
            self._writer_cv.notify_all()

    @staticmethod
    def _encode(
        rows: dict[tuple[str, str], dict[str, Any]],
        edits: list[dict[str, Any]],
        sequence: int,
        activation: dict[str, str] | None,
    ) -> str:
        document = {
            "schema_version": SCHEMA_VERSION,
            "sequence": sequence,
            "rows": [
                {"project_path": row_project, "key": row_key, **row}
                for (row_project, row_key), row in sorted(rows.items())
            ],
            "edits": edits,
            "activation": activation,
        }
        return json.dumps(document, sort_keys=True, indent=1, allow_nan=False)

    def _replace_file(self, encoded: str) -> None:
        """Write a complete file beside the register, then rename it over it."""

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

    def _sync_directory(self) -> None:
        directory = os.open(self._path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)

    def reconcile(self, activation: CommittedActivation) -> PersistentKeyReconciliation:
        """Make the stored rows follow one committed Scenario activation.

        The activation's Scenario keeps every row its Program still declares
        persistent with the same structure; the others are dropped with a
        notice. All changes are made in one step. Activations are ordered by
        application start and Apply number: the Manager mints the start and
        counts Apply numbers from 1 in it, and every start follows the earlier
        ones. The activation already reconciled, or one ordered before it (a
        late read), changes nothing, so repeated or late wakes are harmless.
        """

        if not isinstance(activation, CommittedActivation):
            raise TypeError("reconcile requires one CommittedActivation")
        with self._lock:
            current = self._activation
            if current is not None and current["record_digest"] == activation.record_digest:
                return PersistentKeyReconciliation("duplicate")
            if (
                current is not None
                and current["application_start_id"] == self._application_start_id
                and activation.apply_id < current["apply_id"]
            ):
                return PersistentKeyReconciliation("stale")
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
                [_drop_audit(notice, _RECONCILIATION_EDIT) for notice in notices],
                activation={
                    "record_digest": activation.record_digest,
                    "project_path": activation.project_path,
                    "application_start_id": self._application_start_id,
                    "apply_id": activation.apply_id,
                },
            )
            return PersistentKeyReconciliation("reconciled", tuple(notices))

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
                self._activation is None
                or self._activation["project_path"] != project_path
            ):
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

        The notice is kept for the starting run's audit.
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
        An absent row is never equal, even to `initial:`. Every write
        returns once in memory and never waits for the disk; the writer
        thread stores it.
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
            self._commit_locked(identity, fingerprint, encoded, edit)

    def _commit_locked(
        self,
        identity: tuple[str, str],
        fingerprint: str,
        encoded: str,
        edit: PersistentKeyEdit | Mapping[str, Any] | None,
    ) -> None:
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

    def reset_to_initial(
        self,
        project_path: str,
        declarations: Mapping[str, BlackboardKeyDeclaration],
        *,
        edit: PersistentKeyEdit,
    ) -> dict[str, Any]:
        """Forget every stored value of the Scenario's persistent keys.

        The Program Reset that reloads the initial scene: each key, whatever
        its access, starts again from `initial:` (or empty). One change, with
        one `reset` audit per key naming the value it had. Returns the
        forgotten values by key.
        """

        _require_project(project_path)
        persistent = {
            key
            for key, declaration in declarations.items()
            if declaration.persistence == "persistent"
        }
        with self._lock:
            forgotten = {
                key: json.loads(_canonical(row["value"]))
                for (row_project, key), row in sorted(self._rows.items())
                if row_project == project_path and key in persistent
            }
            if forgotten:
                rows = {
                    identity: row
                    for identity, row in self._rows.items()
                    if identity[0] != project_path or identity[1] not in forgotten
                }
                self._write(
                    rows,
                    self._sequence + 1,
                    [
                        _audit(
                            edit,
                            "reset",
                            project_path,
                            key,
                            detail=f"Reset reloaded the initial scene; was {_canonical(value)}",
                        )
                        for key, value in forgotten.items()
                    ],
                )
            return forgotten

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
    "MAX_ROWS",
    "MAX_VALUE_BYTES",
    "PersistentKeyEdit",
    "PersistentKeyNotice",
    "PersistentKeyReconciliation",
    "PersistentKeyRegister",
    "PersistentKeyRegisterError",
    "PersistentKeyRegisterVersionError",
    "REGISTER_FILE_NAME",
    "REASON_DECLARATION_CHANGED",
    "REASON_NOT_DECLARED",
    "REASON_VALUE_INVALID",
    "declaration_fingerprint",
]
