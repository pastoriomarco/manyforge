"""Bounded Program-run values and one-shot node-attempt input resolution."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from copy import deepcopy
from dataclasses import dataclass
from math import isfinite
from threading import RLock
from types import MappingProxyType
from typing import Any

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.orchestration_ports import (
    PersistentKeyRegisterPort,
    PersistentValueUncertainError,
)
from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.program import (
    BlackboardFieldDeclaration,
    BlackboardKeyDeclaration,
    INTERRUPT_STATE_VALUES,
    NODE_ENDED_OUTCOMES,
    NODE_OUTCOME_NONE,
    Program,
    ProgramValidationError,
    active_tcp_key,
    interrupt_state_key,
    is_active_tcp_key,
    is_interrupt_state_key,
    is_node_outcome_key,
    node_outcome_key,
)
from manyforge_behavior.program_expressions import (
    CheckedExpression,
    ExpressionError,
    describe_diagnostic,
    evaluate,
    key_references,
)
from manyforge_behavior.value_path import parse_value_path, replace_at_path


class ProgramValueError(ValueError):
    """A declared Program value is missing, malformed, or inaccessible."""


NODE_INPUT_UNAVAILABLE = "node_input_unavailable"

# What was wrong with an input a node attempt could not resolve: the value
# (or a key or parameter it is read from) is absent, or it is present but
# refused (out of schema, wrong type, an expression that cannot evaluate).
INPUT_MISSING = "missing"
INPUT_INVALID = "invalid"

# Where an input value comes from: one of the Program's value-source kinds,
# or the node's own authored or catalog-default value (`parameter`).
INPUT_SOURCE_KINDS = frozenset(
    {
        "parameter",
        "program_parameter",
        "blackboard_key",
        "blackboard_field",
        "expression",
    }
)

_MAX_INPUT_MESSAGE_BYTES = 4_096


def _bounded_text(text: str, maximum_bytes: int) -> str:
    encoded = text.encode("utf-8")
    if len(encoded) <= maximum_bytes:
        return text
    return encoded[: maximum_bytes - 3].decode("utf-8", "ignore") + "..."


def input_failure_details(
    *,
    input_problem: str,
    path: str | None,
    source_kind: str,
    source_reference: str | None = None,
) -> Mapping[str, Any]:
    """The common diagnostic of one node input that could not be resolved."""

    if input_problem not in {INPUT_MISSING, INPUT_INVALID}:
        raise ValueError("input_problem must be missing or invalid")
    if source_kind not in INPUT_SOURCE_KINDS:
        raise ValueError(f"input source kind {source_kind!r} is unsupported")
    return MappingProxyType(
        {
            "input_problem": input_problem,
            "path": path,
            "source_kind": source_kind,
            "source_reference": source_reference,
        }
    )


def describe_input_failure(details: Mapping[str, Any], cause: str) -> str:
    """One bounded line naming the problem, the input path and its source.

    For example ``missing input duration_s from blackboard_key 'delay':
    blackboard key 'delay' has no value``. The failure message carries this
    line wherever the node failure is shown or recorded.
    """

    path = details.get("path") or "(node inputs)"
    source = str(details["source_kind"])
    reference = details.get("source_reference")
    if reference is not None:
        source = f"{source} {reference!r}"
    return _bounded_text(
        f"{details['input_problem']} input {path} from {source}: {cause}",
        _MAX_INPUT_MESSAGE_BYTES,
    )


class NodeInputError(ProgramValueError):
    """One input of a node attempt is missing or invalid.

    Raised before the node's own work starts; the attempt fails with
    ``node_input_unavailable`` whatever outcomes its catalog entry declares.
    ``details`` holds ``input_problem`` (``missing`` or ``invalid``),
    ``path`` (the parameter path), ``source_kind`` and ``source_reference``.
    """

    reason_code = NODE_INPUT_UNAVAILABLE

    def __init__(self, cause: str, *, details: Mapping[str, Any]) -> None:
        super().__init__(describe_input_failure(details, cause))
        self.cause = cause
        self.details = details


class ExpressionEvaluationError(ProgramValueError):
    """An expression could not be evaluated from the attempt's snapshot."""

    reason_code = "expression_evaluation_failed"

    def __init__(
        self,
        message: str,
        *,
        diagnostic: Mapping[str, Any] | None = None,
        details: Mapping[str, Any] | None = None,
    ) -> None:
        super().__init__(
            message if details is None else describe_input_failure(details, message)
        )
        # The engine's first diagnostic (code, message, name), when it failed.
        self.diagnostic = diagnostic
        self.cause = message
        # The node input it was evaluated for (see `input_failure_details`),
        # when it failed while a node attempt resolved its inputs.
        self.details = details
        # The computed key whose own expression failed, if any.
        self.computed_key: str | None = None

    @property
    def input_problem(self) -> str:
        """A key or parameter the expression reads has no value: missing."""

        code = (self.diagnostic or {}).get("code")
        return INPUT_MISSING if code == "missing_value" else INPUT_INVALID

    def for_input(
        self, *, path: str | None, source_kind: str, source_reference: str | None
    ) -> "ExpressionEvaluationError":
        """The same failure, attributed to the node input it was read for."""

        return ExpressionEvaluationError(
            self.cause,
            diagnostic=self.diagnostic,
            details=input_failure_details(
                input_problem=self.input_problem,
                path=path,
                source_kind=source_kind,
                source_reference=source_reference,
            ),
        )


class ProgramValueUncertainError(ProgramValueError):
    """A persistent change is visible and stored but may not be durable."""


# Why a run start dropped a restored value the committed Program refuses.
RESTORED_VALUE_INVALID = "value_invalid"


def input_failure_reason(error: Exception) -> str:
    """The node failure reason for an input that could not be resolved."""

    if isinstance(error, ExpressionEvaluationError):
        return ExpressionEvaluationError.reason_code
    return NODE_INPUT_UNAVAILABLE


def _copy_runtime_value(value: Any) -> Any:
    """Thaw immutable catalog/Program JSON without leaking shared containers."""

    if isinstance(value, Mapping):
        return {key: _copy_runtime_value(nested) for key, nested in value.items()}
    if isinstance(value, (list, tuple)):
        return [_copy_runtime_value(nested) for nested in value]
    return deepcopy(value)


@dataclass(frozen=True, slots=True)
class NodeAttemptInputSnapshot:
    """One atomic node-attempt view of authored and Program-run values."""

    parameters: Mapping[str, Any]
    blackboard: Mapping[str, Any]


@dataclass(frozen=True, slots=True)
class ProgramValueRecord:
    """One atomic logical payload plus owner-issued diagnostic metadata."""

    status: str
    value: Any
    metadata: Mapping[str, Any]

    def __post_init__(self) -> None:
        if not isinstance(self.status, str) or not self.status:
            raise ProgramValueError("Program value status must be non-empty text")
        if not isinstance(self.metadata, Mapping):
            raise TypeError("Program value metadata must be a mapping")
        if any(not isinstance(key, str) or not key for key in self.metadata):
            raise ProgramValueError("Program value metadata keys must be non-empty text")
        if "status" in self.metadata:
            raise ProgramValueError("Program value metadata cannot replace the record status")
        object.__setattr__(self, "value", _copy_runtime_value(self.value))
        object.__setattr__(
            self,
            "metadata",
            MappingProxyType(_copy_runtime_value(self.metadata)),
        )

    def to_bundle(self) -> dict[str, Any]:
        return {
            "meta": {"status": self.status, **_copy_runtime_value(self.metadata)},
            "value": _copy_runtime_value(self.value),
        }


# Key types a Run may publish to its host BB source (contract 52).
PUBLISHED_KEY_TYPES = frozenset({"bool", "int64", "float64", "string"})


@dataclass(frozen=True, slots=True)
class PublishedKeyChange:
    """One committed change of a subscribed key; ``present`` is false when a
    reset left the key without a value."""

    key: str
    value_type: str
    value: Any
    present: bool


def _matches_scalar_type(value: object, declared_type: str) -> bool:
    if declared_type == "bool":
        return type(value) is bool
    if declared_type == "float64":
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return False
        try:
            return isfinite(float(value))
        except OverflowError:
            return False
    if declared_type == "int64":
        return type(value) is int and -(2**53 - 1) <= value <= 2**53 - 1
    if declared_type in {"scene_resource", "string"}:
        return isinstance(value, str) and bool(value)
    if declared_type == "string_list":
        return isinstance(value, list) and all(isinstance(item, str) for item in value)
    if declared_type == "cartesian_path":
        return isinstance(value, list) and len(value) >= 2 and all(
            isinstance(item, Mapping) for item in value
        )
    if declared_type == "coordinate_position":
        return isinstance(value, Mapping)
    return isinstance(value, Mapping)


def _validate_fields(
    value: object,
    fields: Mapping[str, BlackboardFieldDeclaration],
    *,
    label: str,
    parent_type: str,
) -> None:
    if not fields:
        return
    if not isinstance(value, Mapping) or set(value) != set(fields):
        raise ProgramValueError(f"{label} must contain exactly declared fields {sorted(fields)}")
    for field_name, declaration in fields.items():
        field_value = value[field_name]
        empty_selection_identity = (
            parent_type in {"model_response", "scene_selection_result"}
            and field_name == "selected_object_id"
            and declaration.type == "scene_resource"
            and field_value == ""
            and value.get("outcome") in {"no_match", "ambiguous"}
        )
        empty_model_reason = (
            parent_type in {"model_response", "scene_selection_result"}
            and field_name == "reason"
            and declaration.type == "string"
            and field_value == ""
        )
        if (
            not empty_selection_identity
            and not empty_model_reason
            and not _matches_scalar_type(field_value, declaration.type)
        ):
            raise ProgramValueError(f"{label}.{field_name} does not match {declaration.type}")


# Types a Program can author (defaults, `initial:`); their values are checked
# by the Program's own declared-type validator, pose frames included.
_AUTHORED_VALUE_TYPES = frozenset(
    {"bool", "float64", "int64", "scene_resource", "stored_pose", "string", "string_list", "tcp_entry"}
)


def validate_blackboard_value(
    program: Program,
    declaration: BlackboardKeyDeclaration,
    value: object,
    *,
    label: str,
) -> None:
    """The one check for every value a key receives: loads and writes alike."""

    if declaration.type in _AUTHORED_VALUE_TYPES:
        try:
            program.validate_key_value(declaration.key, value, path=label)
        except ProgramValidationError as error:
            raise ProgramValueError(str(error)) from error
        if declaration.type == "int64" and not _matches_scalar_type(value, "int64"):
            raise ProgramValueError(f"{label} exceeds the exact JSON integer range")
        return
    if not _matches_scalar_type(value, declaration.type):
        raise ProgramValueError(f"{label} does not match {declaration.type}")
    _validate_fields(
        value,
        declaration.fields,
        label=label,
        parent_type=declaration.type,
    )
    _validate_pose_frames(program, declaration.type, value, label=label)
    for field_name, field in declaration.fields.items():
        _validate_pose_frames(program, field.type, value[field_name], label=f"{label}.{field_name}")


def _validate_pose_frames(
    program: Program,
    declared_type: str,
    value: Any,
    *,
    label: str,
) -> None:
    """Poses inside a value that cannot be authored still name declared frames."""

    if declared_type == "stored_pose":
        if "frame_ref" not in value:
            raise ProgramValueError(f"{label} must contain frame_ref")
        poses = [(label, value)]
    elif declared_type == "cartesian_path":
        poses = [
            (f"{label}[{index}]", pose)
            for index, pose in enumerate(value)
            if "frame_ref" in pose
        ]
    else:
        return
    for path, pose in poses:
        try:
            program.validate_pose_frame(pose["frame_ref"], path=f"{path}.frame_ref")
        except ProgramValidationError as error:
            raise ProgramValueError(str(error)) from error


class ProgramValueStore:
    """Thread-safe logical-key store; engine storage paths never become meaning.

    Typed Program reads expose only the declared payload.  The complete atomic
    record remains available for audit/diagnostics so owner metadata does not
    become an author-controlled field or interfere with declared dataflow.

    Computed keys are never stored: each read evaluates them, together with
    the computed keys they read, from one locked view of the stored keys and
    the run's Program parameter values.
    """

    def __init__(
        self,
        program: Program,
        *,
        restored: Mapping[str, Any] | None = None,
        persistent_commit: (
            Callable[[BlackboardKeyDeclaration, Any, Mapping[str, Any]], None] | None
        ) = None,
        persistent_forget: (
            Callable[[BlackboardKeyDeclaration, Mapping[str, Any]], None] | None
        ) = None,
        persistent_drop: (
            Callable[[BlackboardKeyDeclaration, str, str], None] | None
        ) = None,
    ) -> None:
        if not isinstance(program, Program):
            raise TypeError("value store requires one validated Program")
        self._program = program
        self._declarations = program.blackboard_keys
        self._parameters = MappingProxyType(_program_parameter_values(program))
        self._values: dict[str, ProgramValueRecord] = {}
        self._lock = RLock()
        self._published: frozenset[str] | None = None
        self._on_published: Callable[[PublishedKeyChange], None] | None = None
        self._published_computed: dict[str, frozenset[str]] = {}
        self._published_computed_last: dict[str, tuple[bool, Any]] = {}
        persistent = {
            key
            for key, declaration in self._declarations.items()
            if declaration.persistence == "persistent"
        }
        if persistent and persistent_commit is None:
            raise ProgramValueError(
                "persistent blackboard keys require the persistent-key register"
            )
        self._persistent_commit = persistent_commit
        self._persistent_forget = persistent_forget
        restored = dict(restored or {})
        if not set(restored) <= persistent:
            raise ProgramValueError(
                "restored values are valid only for persistent blackboard keys"
            )
        # Starting values are loaded here, not written: access rules govern
        # node and operator writes, so a `read` key still gets its value.
        for key, declaration in sorted(self._declarations.items()):
            if key in restored:
                try:
                    self._load(declaration, restored[key], "persistent_register")
                    continue
                except ProgramValueError as error:
                    # A rule of the committed Program (an allowed frame, a
                    # declared Scene resource, ...) refuses the stored value.
                    # It is dropped with a notice and the key starts as if
                    # nothing were stored; the run itself is not refused.
                    if persistent_drop is not None:
                        try:
                            persistent_drop(
                                declaration, RESTORED_VALUE_INVALID, str(error)
                            )
                        except Exception:
                            # The register keeps the notice; the refused
                            # value is not used either way.
                            pass
            if declaration.has_initial:
                self._load(declaration, declaration.initial, "initial")

    def _load(
        self, declaration: BlackboardKeyDeclaration, value: Any, source: str
    ) -> None:
        validate_blackboard_value(
            self._program,
            declaration,
            value,
            label=f"blackboard key {declaration.key!r} ({source})",
        )
        self._values[declaration.key] = ProgramValueRecord(
            status="succeeded",
            value=value,
            metadata={"value_source": source},
        )

    @classmethod
    def from_program(
        cls,
        program: Program,
        *,
        project_path: str | None = None,
        register: PersistentKeyRegisterPort | None = None,
        program_run_id: str | None = None,
    ) -> "ProgramValueStore":
        if not isinstance(program, Program):
            raise TypeError("value store requires one validated Program")
        persistent = {
            key: declaration
            for key, declaration in program.blackboard_keys.items()
            if declaration.persistence == "persistent"
        }
        if register is None or project_path is None:
            if persistent:
                raise ProgramValueError(
                    "persistent blackboard keys require the persistent-key register"
                )
            return cls(program)
        run = {} if program_run_id is None else {"program_run_id": program_run_id}
        return cls(
            program,
            restored=register.restore(project_path, persistent),
            persistent_commit=lambda declaration, value, metadata: register.commit(
                project_path, declaration, value, edit={**run, **metadata}
            ),
            persistent_forget=lambda declaration, metadata: register.remove(
                project_path, declaration.key, edit={**run, **metadata}
            ),
            persistent_drop=lambda declaration, reason, detail: register.drop(
                project_path, declaration.key, reason=reason, detail=detail, edit=run
            ),
        )

    @property
    def program(self) -> Program:
        return self._program

    @property
    def program_parameters(self) -> Mapping[str, Any]:
        """The run's Program parameter values: always the Program defaults."""

        return self._parameters

    def _readable(self, key: str) -> BlackboardKeyDeclaration:
        try:
            declaration = self._declarations[key]
        except KeyError as exc:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared") from exc
        if declaration.access not in {"read", "read_write"}:
            raise ProgramValueError(f"blackboard key {key!r} is not readable")
        return declaration

    def _computed_locked(self, keys: set[str]) -> dict[str, Any]:
        """Evaluate the computed keys in ``keys``; the first that fails raises."""

        results, failures = self._evaluate_computed_locked(keys)
        for key in sorted(keys & failures.keys()):
            raise failures[key]
        return results

    def _evaluate_computed_locked(
        self, keys: set[str]
    ) -> tuple[dict[str, Any], dict[str, "ExpressionEvaluationError"]]:
        """Evaluate the computed keys in ``keys`` and every computed key they read.

        Dependencies come first, from the same locked view. One that fails is
        left out of the results and reported in the failures, so an expression
        that never reads it (an untaken branch) still evaluates; one that does
        read it fails with that failure.
        """

        expressions = self._program.expressions
        needed: set[str] = set()
        pending = [key for key in keys if key in expressions.computed_keys]
        while pending:
            key = pending.pop()
            if key in needed:
                continue
            needed.add(key)
            pending.extend(
                name
                for name in key_references(expressions.computed_keys[key])
                if name in expressions.computed_keys
            )
        results: dict[str, Any] = {}
        failures: dict[str, ExpressionEvaluationError] = {}
        for key in expressions.computed_key_order:
            if key not in needed:
                continue
            checked = expressions.computed_keys[key]
            values = {
                name: self._values[name].value
                for name in key_references(checked)
                if name in self._values
            }
            values.update(
                (name, results[name])
                for name in key_references(checked)
                if name in results
            )
            label = f"computed key {key!r}"
            try:
                results[key] = _evaluate(
                    checked,
                    label=label,
                    program=self._program,
                    parameters=self._parameters,
                    keys=values,
                )
            except ExpressionEvaluationError as error:
                failure = _dependency_failure(error, label, checked, failures)
                failure.computed_key = key
                failures[key] = failure
        return results, failures

    def subscribe_published_keys(
        self,
        keys: tuple[str, ...],
        *,
        baseline: Callable[[tuple[PublishedKeyChange, ...]], None],
        on_change: Callable[[PublishedKeyChange], None],
    ) -> None:
        """Fix the Run's referenced-key union once and capture its baseline.

        Under the store lock ``baseline`` receives every key's current value
        (``present`` false when it is unset, or when a computed key cannot be
        evaluated), and the subscription is installed in the same critical
        section, so no commit falls between them. A computed key is recomputed
        by the existing evaluator whenever a stored key it reads commits, and
        is reported only when its value actually changes. Both callbacks run
        under the store lock and forward synchronously (BBK-1): every writer
        commits through this store, so the forwards keep commit order and a
        write returns only after its forward.
        """

        if not keys or keys != tuple(sorted(set(keys))):
            raise ProgramValueError(
                "published blackboard keys must be non-empty, unique and canonical"
            )
        expressions = self._program.expressions
        computed_inputs: dict[str, frozenset[str]] = {}
        for key in keys:
            # Any declared key may be published, whatever its access mode
            # (schema03 Δ2): a rule reads it, not a node.
            declaration = self._declarations.get(key)
            if declaration is None:
                raise ProgramValueError(f"blackboard key {key!r} is undeclared")
            if declaration.type not in PUBLISHED_KEY_TYPES:
                raise ProgramValueError(
                    f"blackboard key {key!r} of type {declaration.type!r} "
                    "cannot be published"
                )
            if declaration.expression is None:
                continue
            # The stored keys this computed key reads, through computed keys.
            stored: set[str] = set()
            seen: set[str] = set()
            pending = [key]
            while pending:
                name = pending.pop()
                if name in seen:
                    continue
                seen.add(name)
                for reference in key_references(expressions.computed_keys[name]):
                    if reference in expressions.computed_keys:
                        pending.append(reference)
                    else:
                        stored.add(reference)
            computed_inputs[key] = frozenset(stored)
        with self._lock:
            if self._published is not None:
                raise ProgramValueError(
                    "the Run's published blackboard keys are already fixed"
                )
            self._published_computed = computed_inputs
            baseline(self._published_baseline_locked(keys))
            self._published = frozenset(keys)
            self._on_published = on_change

    def read_published_baseline(
        self, baseline: Callable[[tuple[PublishedKeyChange, ...]], None]
    ) -> None:
        """Hand ``baseline`` every subscribed key's current value, under the
        store lock: a source the Kernel installs later starts from the values
        committed meanwhile (BBK-1 (a))."""

        with self._lock:
            if self._published is None:
                raise ProgramValueError("the Run's published blackboard keys are not fixed")
            baseline(self._published_baseline_locked(tuple(sorted(self._published))))

    def _published_baseline_locked(
        self, keys: tuple[str, ...]
    ) -> tuple[PublishedKeyChange, ...]:
        computed = self._published_computed_locked(set(self._published_computed))
        self._published_computed_last = computed
        changes = []
        for key in keys:
            if key in computed:
                present, value = computed[key]
            else:
                present = key in self._values
                value = _copy_runtime_value(self._values[key].value) if present else None
            changes.append(
                PublishedKeyChange(
                    key=key,
                    value_type=self._declarations[key].type,
                    value=value,
                    present=present,
                )
            )
        return tuple(changes)

    def _published_computed_locked(
        self, keys: set[str]
    ) -> dict[str, tuple[bool, Any]]:
        results, _failures = self._evaluate_computed_locked(keys)
        return {
            key: (key in results, _copy_runtime_value(results.get(key)))
            for key in keys
        }

    def _publish_locked(self, key: str, previous: ProgramValueRecord | None) -> None:
        """Report one committed change of a subscribed key (store lock held)."""

        if self._published is None:
            return
        current = self._values.get(key)
        unchanged = (current is None and previous is None) or (
            current is not None
            and previous is not None
            and type(current.value) is type(previous.value)
            and current.value == previous.value
        )
        if unchanged:
            return
        if key in self._published and key not in self._published_computed:
            self._on_published(  # type: ignore[misc]
                PublishedKeyChange(
                    key=key,
                    value_type=self._declarations[key].type,
                    value=None if current is None else _copy_runtime_value(current.value),
                    present=current is not None,
                )
            )
        dependent = {
            name for name, inputs in self._published_computed.items() if key in inputs
        }
        if not dependent:
            return
        for name, (present, value) in sorted(
            self._published_computed_locked(dependent).items()
        ):
            last = self._published_computed_last[name]
            if last[0] == present and type(last[1]) is type(value) and last[1] == value:
                continue
            self._published_computed_last[name] = (present, value)
            self._on_published(  # type: ignore[misc]
                PublishedKeyChange(
                    key=name,
                    value_type=self._declarations[name].type,
                    value=value,
                    present=present,
                )
            )

    def clear_run_values(self) -> None:
        """Reset: run-only stored keys lose their values; persistent keys keep
        theirs. Each change reaches the publication subscription."""

        with self._lock:
            for key in sorted(self._values):
                if self._declarations[key].persistence == "persistent":
                    continue
                if is_interrupt_state_key(key) or is_active_tcp_key(key):
                    # System-owned and always current across a Run reset.
                    continue
                if is_node_outcome_key(key):
                    # System start value `none` (contract 105).
                    previous = self._values[key]
                    if previous.value == NODE_OUTCOME_NONE:
                        continue
                    self._load(self._declarations[key], NODE_OUTCOME_NONE, "initial")
                    self._publish_locked(key, previous)
                    continue
                previous = self._values.pop(key)
                self._publish_locked(key, previous)

    def read(self, key: str) -> Any:
        declaration = self._readable(key)
        with self._lock:
            if declaration.expression is not None:
                return self._computed_locked({key})[key]
            if key not in self._values:
                raise KeyError(key)
            return _copy_runtime_value(self._values[key].value)

    def write(
        self,
        key: str,
        value: Any,
        *,
        status: str = "succeeded",
        metadata: Mapping[str, Any] | None = None,
    ) -> None:
        try:
            declaration = self._declarations[key]
        except KeyError as exc:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared") from exc
        if declaration.expression is not None:
            raise ProgramValueError(
                f"blackboard key {key!r} is computed from its expression and "
                "cannot be written"
            )
        if declaration.access not in {"write", "read_write"}:
            raise ProgramValueError(f"blackboard key {key!r} is not writable")
        validate_blackboard_value(
            self._program,
            declaration,
            value,
            label=f"blackboard key {key!r}",
        )
        record = ProgramValueRecord(
            status=status,
            value=value,
            metadata=metadata or {},
        )
        with self._lock:
            # The lock serializes node and operator writes; a persistent value
            # is committed before it becomes visible, so a failed commit
            # leaves the previous value in place.
            current = self._values.get(key)
            if declaration.persistence == "persistent" and (
                current is None or current.value != record.value
            ):
                try:
                    # Set whenever a persistent key is declared (see __init__).
                    self._persistent_commit(  # type: ignore[misc]
                        declaration, record.value, record.metadata
                    )
                except PersistentValueUncertainError as error:
                    # Stored, so visible: the store never contradicts the register.
                    self._values[key] = record
                    self._publish_locked(key, current)
                    raise ProgramValueUncertainError(
                        f"blackboard key {key!r} was stored but may not be "
                        f"durable: {error}"
                    ) from error
                except Exception as error:
                    raise ProgramValueError(
                        f"blackboard key {key!r} could not be stored: {error}"
                    ) from error
            self._values[key] = record
            self._publish_locked(key, current)

    def write_node_outcome(
        self,
        node_instance_id: str,
        outcome: str,
        *,
        metadata: Mapping[str, Any] | None = None,
    ) -> None:
        """Store a motion node's outcome in its system-owned key (item 42).

        The one writer of the reserved namespace: the key is read-only for
        nodes and the operator, and it keeps this value until the node ends
        again. A Run starts at `none` (the declared start value, contract 105).
        """

        key = node_outcome_key(node_instance_id)
        if key not in self._declarations:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared")
        if outcome not in NODE_ENDED_OUTCOMES:
            raise ProgramValueError(f"{outcome!r} is not a node outcome")
        with self._lock:
            current = self._values.get(key)
            self._values[key] = ProgramValueRecord(
                status="succeeded", value=outcome, metadata=metadata or {}
            )
            self._publish_locked(key, current)

    def write_interrupt_state(self, rule_id: str, state: str | None) -> bool:
        """Mirror one global rule's Kernel state into its reserved key.

        The one writer of the namespace (contract 64): read-only for nodes and
        the operator. ``None`` makes the key unavailable (not clear). Only the
        Applied global rules are declared; another rule id is ignored (False).
        """

        key = interrupt_state_key(rule_id)
        if key not in self._declarations:
            return False
        if state is not None and state not in INTERRUPT_STATE_VALUES:
            raise ProgramValueError(f"{state!r} is not an interrupt rule state")
        with self._lock:
            current = self._values.get(key)
            if state is None:
                if current is None:
                    return True
                self._values.pop(key)
            elif current is not None and current.value == state:
                return True
            else:
                self._values[key] = ProgramValueRecord(
                    status="succeeded", value=state, metadata={"writer": "interrupt_state_mirror"}
                )
            self._publish_locked(key, current)
        return True

    def write_active_tcp(self, target_role: str, entry: Mapping[str, Any] | None) -> bool:
        """Mirror the robot's confirmed TCP into its read-only role key.

        ``None`` makes the value unavailable. A role not declared in this
        Program is ignored, as with the interrupt-state mirror.
        """

        key = active_tcp_key(target_role)
        declaration = self._declarations.get(key)
        if declaration is None:
            return False
        if entry is not None:
            entry = _copy_runtime_value(entry)
            validate_blackboard_value(
                self._program, declaration, entry, label=f"blackboard key {key!r}"
            )
        with self._lock:
            current = self._values.get(key)
            if entry is None:
                if current is None:
                    return True
                self._values.pop(key)
            elif current is not None and current.value == entry:
                return True
            else:
                self._values[key] = ProgramValueRecord(
                    status="succeeded", value=entry, metadata={"writer": "active_tcp_mirror"}
                )
            self._publish_locked(key, current)
        return True

    def reset_to_initial(
        self, key: str, *, metadata: Mapping[str, Any] | None = None
    ) -> None:
        """Forget a persistent key's stored value and start it again from `initial:`.

        An operator value change like a write, so the key must be writable.
        The stored row is removed before the visible value changes; a failed
        removal leaves both intact. ``metadata`` attributes the change.
        """

        try:
            declaration = self._declarations[key]
        except KeyError as exc:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared") from exc
        if declaration.persistence != "persistent":
            raise ProgramValueError(f"blackboard key {key!r} is not persistent")
        if declaration.access not in {"write", "read_write"}:
            raise ProgramValueError(f"blackboard key {key!r} is not writable")
        if self._persistent_forget is None:
            raise ProgramValueError(
                "persistent blackboard keys require the persistent-key register"
            )
        with self._lock:
            uncertain: PersistentValueUncertainError | None = None
            try:
                self._persistent_forget(declaration, dict(metadata or {}))
            except PersistentValueUncertainError as error:
                uncertain = error
            except Exception as error:
                raise ProgramValueError(
                    f"blackboard key {key!r} could not be reset: {error}"
                ) from error
            current = self._values.get(key)
            if declaration.has_initial:
                self._load(declaration, declaration.initial, "initial")
            else:
                self._values.pop(key, None)
            self._publish_locked(key, current)
            if uncertain is not None:
                raise ProgramValueUncertainError(
                    f"blackboard key {key!r} was reset but may not be durable: "
                    f"{uncertain}"
                ) from uncertain

    def read_bundle(self, key: str) -> dict[str, Any]:
        """Return the atomic diagnostic record without changing logical reads."""

        declaration = self._readable(key)
        with self._lock:
            if declaration.expression is not None:
                return ProgramValueRecord(
                    status="succeeded",
                    value=self._computed_locked({key})[key],
                    metadata={"value_source": "expression"},
                ).to_bundle()
            if key not in self._values:
                raise KeyError(key)
            return self._values[key].to_bundle()

    def check_readable(self, key: str) -> None:
        """Raise ``ProgramValueError`` unless ``key`` is a declared readable key."""

        self._readable(key)

    def contains(self, key: str) -> bool:
        """Whether a value is stored; computed keys are never stored."""

        with self._lock:
            return key in self._values

    def diagnostic_snapshot(self) -> dict[str, Any]:
        from .program_diagnostics import diagnostic_values

        with self._lock:
            return diagnostic_values(
                ((key, record.value) for key, record in self._values.items()),
                total_count=len(self._values),
            )

    def snapshot(
        self, keys: tuple[str, ...], *, optional: tuple[str, ...] = ()
    ) -> Mapping[str, Any]:
        """One atomic view of ``keys`` (each must have a value) and of the
        ``optional`` keys that have one; computed keys are evaluated in it."""

        return self.snapshot_with_failures(keys, optional=optional)[0]

    def snapshot_with_failures(
        self, keys: tuple[str, ...], *, optional: tuple[str, ...] = ()
    ) -> tuple[Mapping[str, Any], Mapping[str, "ExpressionEvaluationError"]]:
        """``snapshot`` plus the optional computed keys that failed.

        A failing optional computed key is left out of the view like an empty
        key; an expression that actually reads it reports that failure (see
        ``_dependency_failure``). A failing required key raises.
        """

        for group in (keys, optional):
            if group != tuple(sorted(set(group))):
                raise ProgramValueError(
                    "blackboard snapshot keys must be unique and canonical"
                )
        for key in (*keys, *optional):
            self._readable(key)
        with self._lock:
            computed, failures = self._evaluate_computed_locked(
                set(keys) | set(optional)
            )
            for key in keys:
                if key in failures:
                    raise failures[key]
            missing = [
                key for key in keys if key not in self._values and key not in computed
            ]
            if missing:
                raise KeyError(missing[0])
            result = {
                key: _copy_runtime_value(self._values[key].value)
                for key in (*keys, *optional)
                if key in self._values
            }
            result.update(computed)
            return (
                MappingProxyType(
                    {
                        key: result[key]
                        for key in sorted(result)
                        if key in keys or key in optional
                    }
                ),
                MappingProxyType(
                    {key: failures[key] for key in sorted(failures) if key in optional}
                ),
            )


def _program_parameter_values(program: Program) -> dict[str, Any]:
    return {
        parameter_id: _copy_runtime_value(declaration.default)
        for parameter_id, declaration in program.parameters.items()
    }


def _dependency_failure(
    error: "ExpressionEvaluationError",
    label: str,
    checked: CheckedExpression,
    failures: Mapping[str, "ExpressionEvaluationError"],
) -> "ExpressionEvaluationError":
    """Report a computed key the expression read but could not get as that
    key's own failure, not merely as "has no value"."""

    diagnostic = error.diagnostic or {}
    name = str(diagnostic.get("name", ""))
    dependency = (
        failures.get(name.removeprefix("key."))
        if diagnostic.get("code") == "missing_value" and name.startswith("key.")
        else None
    )
    if dependency is None:
        return error
    return ExpressionEvaluationError(
        f"{label} = {checked.text!r}: {name} could not be computed: {dependency}",
        diagnostic=dependency.diagnostic,
    )


def _evaluate(
    checked: CheckedExpression,
    *,
    label: str,
    program: Program,
    parameters: Mapping[str, Any],
    keys: Mapping[str, Any],
) -> Any:
    try:
        return evaluate(
            checked,
            parameters=parameters,
            keys=keys,
            parameter_declarations=program.parameters,
            key_declarations=program.blackboard_keys,
        )
    except ExpressionError as error:
        raise ExpressionEvaluationError(
            f"{label} = {checked.text!r}: "
            f"{describe_diagnostic(error.diagnostics[0])}",
            diagnostic=error.diagnostics[0],
        ) from None
    except (TypeError, ValueError) as error:
        raise ExpressionEvaluationError(
            f"{label} = {checked.text!r}: invalid_value: {error}"
        ) from None


class NodeAttemptInputResolver:
    """Resolve all declared dynamic inputs exactly once for one node attempt."""

    def __init__(
        self,
        *,
        program: Program,
        catalog: NodeCatalog,
        value_store: ProgramValueStore,
    ) -> None:
        if not isinstance(program, Program):
            raise TypeError("input resolver requires one validated Program")
        if not isinstance(catalog, NodeCatalog):
            raise TypeError("input resolver requires one validated NodeCatalog")
        if not isinstance(value_store, ProgramValueStore):
            raise TypeError("input resolver requires one ProgramValueStore")
        if value_store.program is not program:
            raise TypeError("input resolver and value store need the same Program")
        self._program = program
        self._catalog = catalog
        self._value_store = value_store

    @property
    def value_store(self) -> ProgramValueStore:
        return self._value_store

    def key_type(self, key: str) -> str:
        """Return the declared type of one Program blackboard key."""

        try:
            return self._program.blackboard_keys[key].type
        except KeyError as exc:
            raise ProgramValueError(f"blackboard key {key!r} is undeclared") from exc

    def snapshot(
        self,
        *,
        node_id: str,
        node_instance_id: str,
        parameters: Mapping[str, Any],
        value_sources: Mapping[str, Mapping[str, Any]],
        blackboard_read_parameters: tuple[str, ...] = (),
        derived_active_tcp_key: str | None = None,
    ) -> NodeAttemptInputSnapshot:
        """Resolve one attempt's inputs, before the node's own work starts.

        Any node can fail here, whatever outcomes its catalog entry declares.
        Every failure names the input: a ``NodeInputError``
        (``node_input_unavailable``) or an ``ExpressionEvaluationError``
        (``expression_evaluation_failed``), each with ``details`` stating
        whether the input is missing or invalid, its parameter path, and the
        kind and reference of the source it came from.
        """

        entry = self._catalog.get(node_id)
        by_id = {parameter.id: parameter for parameter in entry.parameters}
        resolved: dict[str, Any] = {
            parameter.id: _copy_runtime_value(parameter.default)
            for parameter in entry.parameters
            if parameter.has_default
        }
        resolved.update(_copy_runtime_value(parameters))
        program_parameters = self._value_store.program_parameters

        def failure(
            cause: str,
            problem: str,
            path: str | None,
            kind: str,
            reference: str | None = None,
        ) -> NodeInputError:
            return NodeInputError(
                cause,
                details=input_failure_details(
                    input_problem=problem,
                    path=path,
                    source_kind=kind,
                    source_reference=reference,
                ),
            )

        def source_reference(path: str, source: Mapping[str, Any]) -> tuple[str, str]:
            kind = source.get("kind")
            reference = source.get("reference")
            if kind not in {
                "program_parameter",
                "blackboard_key",
                "blackboard_field",
            }:
                raise failure(
                    "value source kind is unsupported", INPUT_INVALID, path, "parameter"
                )
            if not isinstance(reference, str) or not reference:
                raise failure(
                    "value source reference is invalid", INPUT_INVALID, path, kind
                )
            return kind, reference

        expressions: dict[str, CheckedExpression] = {}
        # Each value-source path's source, as diagnostics name it.
        origins: dict[str, tuple[str, str | None]] = {}
        # The inputs that read each blackboard key, in path order.
        key_readers: dict[str, list[tuple[str, str, str | None]]] = {}
        blackboard_keys: set[str] = set()
        expression_keys: set[str] = set()
        for path, source in sorted(value_sources.items()):
            if source.get("kind") == "expression":
                checked = self._program.expressions.value_expressions.get(
                    (node_instance_id, path)
                )
                if checked is None:
                    raise failure(
                        f"expression source {path!r} of node {node_instance_id!r} "
                        "is not part of the validated Program",
                        INPUT_INVALID,
                        path,
                        "expression",
                    )
                expressions[path] = checked
                origins[path] = ("expression", checked.text)
                for key in key_references(checked):
                    expression_keys.add(key)
                    key_readers.setdefault(key, []).append(
                        (path, "expression", checked.text)
                    )
                continue
            kind, reference = source_reference(path, source)
            if kind == "blackboard_field":
                origins[path] = (kind, f"{reference}.{source.get('field')}")
            else:
                origins[path] = (kind, reference)
            if kind != "program_parameter":
                blackboard_keys.add(reference)
                key_readers.setdefault(reference, []).append((path, *origins[path]))
        for parameter_id in blackboard_read_parameters:
            if parameter_id not in resolved:
                raise failure(
                    f"blackboard key parameter {parameter_id!r} is absent",
                    INPUT_MISSING,
                    parameter_id,
                    "parameter",
                )
            key = resolved[parameter_id]
            if not isinstance(key, str) or not key:
                raise failure(
                    f"blackboard key parameter {parameter_id!r} is invalid",
                    INPUT_INVALID,
                    parameter_id,
                    "parameter",
                )
            blackboard_keys.add(key)
            key_readers.setdefault(key, []).append((parameter_id, "blackboard_key", key))

        # Current's key is fixed from the bound target role by the node
        # binder, not selected by an authored blackboard-key parameter.
        if derived_active_tcp_key is not None:
            if not isinstance(derived_active_tcp_key, str) or not is_active_tcp_key(
                derived_active_tcp_key
            ):
                raise failure(
                    "derived Active TCP key is invalid", INPUT_INVALID, None,
                    "blackboard_key", derived_active_tcp_key,
                )
            blackboard_keys.add(derived_active_tcp_key)

        def key_reader(key: str) -> tuple[str | None, str, str | None]:
            readers = key_readers.get(key)
            if not readers:
                return None, "blackboard_key", key
            return readers[0]

        for key in sorted(blackboard_keys | expression_keys):
            try:
                self._value_store.check_readable(key)
            except ProgramValueError as exc:
                raise failure(str(exc), INPUT_INVALID, *key_reader(key)) from exc
        # One atomic snapshot serves every source, expression and read; a key
        # an expression reads may still be empty (the expression then fails
        # naming it), while a key bound or read directly must have a value.
        # A computed key an expression reads but that fails is left out like
        # an empty one: only an expression that actually reads it fails, with
        # that key's own failure.
        try:
            blackboard_snapshot, computed_failures = (
                self._value_store.snapshot_with_failures(
                    tuple(sorted(blackboard_keys)),
                    optional=tuple(sorted(expression_keys - blackboard_keys)),
                )
            )
        except ExpressionEvaluationError as error:
            path, kind, reference = (
                key_reader(error.computed_key)
                if error.computed_key is not None
                else (None, "blackboard_key", None)
            )
            raise error.for_input(
                path=path, source_kind=kind, source_reference=reference
            ) from None
        except KeyError as error:
            key = str(error.args[0]) if error.args else ""
            raise failure(
                f"blackboard key {key!r} has no value", INPUT_MISSING, *key_reader(key)
            ) from None

        def resolve_source(path: str, source: Mapping[str, Any]) -> Any:
            if path in expressions:
                label = f"node {node_instance_id!r} value_sources.{path}"
                try:
                    return _evaluate(
                        expressions[path],
                        label=label,
                        program=self._program,
                        parameters=program_parameters,
                        keys=blackboard_snapshot,
                    )
                except ExpressionEvaluationError as error:
                    raise _dependency_failure(
                        error, label, expressions[path], computed_failures
                    ).for_input(
                        path=path,
                        source_kind="expression",
                        source_reference=expressions[path].text,
                    ) from None
            kind, reference = source_reference(path, source)
            if kind == "program_parameter":
                try:
                    return _copy_runtime_value(program_parameters[reference])
                except KeyError as exc:
                    raise failure(
                        f"Program parameter {reference!r} has no run value",
                        INPUT_MISSING,
                        path,
                        kind,
                        reference,
                    ) from exc
            value = blackboard_snapshot[reference]
            if kind == "blackboard_field":
                field = source.get("field")
                if not isinstance(field, str) or not field or not isinstance(
                    value, Mapping
                ):
                    raise failure(
                        f"blackboard field {reference!r}.{field} is unavailable: "
                        f"blackboard key {reference!r} holds no such field",
                        INPUT_INVALID,
                        path,
                        *origins[path],
                    )
                if field not in value:
                    raise failure(
                        f"blackboard field {reference!r}.{field} is unavailable: "
                        "the stored value has no such field",
                        INPUT_MISSING,
                        path,
                        *origins[path],
                    )
                value = value[field]
            return _copy_runtime_value(value)

        for path, source in value_sources.items():
            segments = parse_value_path(path)
            value = resolve_source(path, source)
            root = segments[0]
            if len(segments) > 1 and root not in resolved:
                raise failure(
                    f"value source {path!r} names no value of node {node_id!r}",
                    INPUT_INVALID,
                    path,
                    *origins[path],
                )
            try:
                resolved[root] = replace_at_path(resolved.get(root), segments, value)
            except KeyError as exc:
                raise failure(
                    f"value source {path!r} names no value of node {node_id!r}",
                    INPUT_INVALID,
                    path,
                    *origins[path],
                ) from exc

        def origin(path: str) -> tuple[str, str | None]:
            """The source of the value at ``path`` (a parameter value path)."""

            covering = [
                source_path
                for source_path in value_sources
                if path == source_path
                or path.startswith((f"{source_path}.", f"{source_path}["))
            ]
            if covering:
                return origins[max(covering, key=len)]
            beneath = sorted(
                source_path
                for source_path in value_sources
                if source_path.startswith((f"{path}.", f"{path}["))
            )
            if beneath:
                return origins[beneath[0]]
            return "parameter", None

        unknown = sorted(set(resolved) - set(by_id))
        if unknown:
            raise failure(
                f"node {node_id!r} has unknown resolved parameters {unknown}",
                INPUT_INVALID,
                unknown[0],
                *origin(unknown[0]),
            )
        missing = sorted(
            parameter.id
            for parameter in entry.parameters
            if parameter.required and parameter.id not in resolved
        )
        if missing:
            raise failure(
                f"node {node_id!r} is missing parameters {missing}",
                INPUT_MISSING,
                missing[0],
                "parameter",
            )
        prefix = f"node[{node_id}].parameters."
        for parameter_id, value in resolved.items():
            source = value_sources.get(parameter_id)
            if (
                node_id == "motion.set_tcp"
                and parameter_id == "tcp"
                and isinstance(source, Mapping)
                and source.get("kind") == "blackboard_key"
                and self.key_type(source.get("reference")) == "tcp_entry"
            ):
                try:
                    self._program.validate_key_value(
                        source["reference"], value, path=f"{prefix}{parameter_id}"
                    )
                except ValueError as exc:
                    raise failure(
                        str(exc), INPUT_INVALID, parameter_id, *origin(parameter_id)
                    ) from exc
                continue
            issues = validate_value(
                by_id[parameter_id].schema,
                value,
                registry=self._catalog.schema_registry,
                path=f"{prefix}{parameter_id}",
            )
            if issues:
                first = issues[0]
                path = first.path.removeprefix(prefix)
                raise failure(
                    f"{first.path}: {first.message}",
                    INPUT_INVALID,
                    path,
                    *origin(path),
                )
            # A key-typed value is checked against the named key's declared
            # type (pose frames included) for every attempt, whatever its source.
            key_parameter = self._catalog.schema_registry.resolve(
                by_id[parameter_id].schema
            ).value_type_from_key
            if key_parameter is not None:
                try:
                    self._program.validate_key_value(
                        resolved[key_parameter],
                        value,
                        path=f"{prefix}{parameter_id}",
                    )
                except ValueError as exc:
                    raise failure(
                        str(exc), INPUT_INVALID, parameter_id, *origin(parameter_id)
                    ) from exc
        declared_blackboard = {
            parameter_id: _copy_runtime_value(blackboard_snapshot[resolved[parameter_id]])
            for parameter_id in blackboard_read_parameters
        }
        if derived_active_tcp_key is not None:
            declared_blackboard["active_tcp"] = _copy_runtime_value(
                blackboard_snapshot[derived_active_tcp_key]
            )
        return NodeAttemptInputSnapshot(
            parameters=MappingProxyType(_copy_runtime_value(resolved)),
            blackboard=MappingProxyType(declared_blackboard),
        )


__all__ = [
    "ExpressionEvaluationError",
    "INPUT_INVALID",
    "INPUT_MISSING",
    "NODE_INPUT_UNAVAILABLE",
    "ProgramValueUncertainError",
    "NodeAttemptInputResolver",
    "NodeAttemptInputSnapshot",
    "NodeInputError",
    "PUBLISHED_KEY_TYPES",
    "ProgramValueError",
    "ProgramValueRecord",
    "ProgramValueStore",
    "PublishedKeyChange",
    "describe_input_failure",
    "input_failure_details",
    "input_failure_reason",
    "validate_blackboard_value",
]
