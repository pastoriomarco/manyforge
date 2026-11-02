"""Per-node interrupt condition grammar (schema02 §1 + schema03 Δ1/Δ2).

A condition is closed data: ``{all:[C]} | {any:[C]} | {not:C} |
{compare:{operand, op, value}}``. An operand is ``{signal:{target_role,
resource_role}}`` or ``{key: <declared scalar key>}``; the right-hand side is
a typed literal only. Limits are per condition and checked on the authored
tree, before Track 2 lowers it into the Core AST: at most 64 authored nodes
and depth 8 (the root is depth 1). There is no separate input limit: at most
63 distinct inputs follow from the grammar (contract 137).

The structural check is context-free (the catalog schema type); the Program
validator adds the declared-key and declared-role checks and typed operator
applicability. Errors carry the frozen reason spellings.
"""

from __future__ import annotations

import math
from collections.abc import Iterator, Mapping
from typing import Any, Final

MAX_CONDITION_DEPTH: Final = 8
MAX_CONDITION_NODES: Final = 64
MAX_SAFE_INTEGER: Final = 2**53 - 1

CONDITION_INVALID: Final = "interrupt_condition_invalid"
CONDITION_KEY_REFUSED: Final = "interrupt_condition_key_refused"
CONDITION_TYPE_MISMATCH: Final = "io_signal_type_mismatch"

OPERATORS: Final = ("==", "!=", "<", "<=", ">", ">=")
EQUALITY_OPERATORS: Final = frozenset({"==", "!="})
# Declared key types a condition may compare (schema03 Δ2).
SCALAR_KEY_TYPES: Final = frozenset({"bool", "int64", "float64", "string"})
# A bound signal's value type; bool only in this version (A1, contract 45).
SIGNAL_VALUE_TYPE: Final = "bool"

_OPERATORS: Final = frozenset({"all", "any", "not", "compare"})
_PARAMETER_OPERANDS: Final = frozenset(
    {"param", "parameter", "program_parameter", "parameter_ref"}
)


class InterruptConditionError(ValueError):
    """One refused condition, with its frozen reason and authored path."""

    def __init__(self, reason: str, path: str, message: str) -> None:
        super().__init__(f"{path}: {reason}: {message}")
        self.reason = reason
        self.path = path
        self.message = message


def _fail(reason: str, path: str, message: str) -> None:
    raise InterruptConditionError(reason, path, message)


def _single(value: Any, path: str, what: str) -> tuple[str, Any]:
    if type(value) is not dict or len(value) != 1:
        _fail(CONDITION_INVALID, path, f"must be a mapping with exactly one {what}")
    ((name, item),) = value.items()
    if type(name) is not str:
        _fail(CONDITION_INVALID, path, f"must name one {what}")
    return name, item


def _closed(value: Any, fields: tuple[str, ...], path: str) -> dict[str, Any]:
    if type(value) is not dict:
        _fail(CONDITION_INVALID, path, "must be a mapping")
    unknown = sorted(str(name) for name in value if name not in fields)
    if unknown:
        _fail(CONDITION_INVALID, path, f"contains unknown fields {unknown}")
    missing = [name for name in fields if name not in value]
    if missing:
        _fail(CONDITION_INVALID, path, f"is missing fields {missing}")
    return value


def _text(value: Any, path: str) -> str:
    if type(value) is not str or not value:
        _fail(CONDITION_INVALID, path, "must be a non-empty string")
    return value


def _check_operand(value: Any, path: str) -> dict[str, Any]:
    kind, item = _single(value, path, "operand selector (signal or key)")
    if kind in _PARAMETER_OPERANDS:
        _fail(
            CONDITION_KEY_REFUSED,
            f"{path}.{kind}",
            "a parameter operand is refused (260 §12.3); compare a blackboard key",
        )
    if kind == "key":
        _text(item, f"{path}.key")
    elif kind == "signal":
        signal = _closed(item, ("target_role", "resource_role"), f"{path}.signal")
        _text(signal["target_role"], f"{path}.signal.target_role")
        _text(signal["resource_role"], f"{path}.signal.resource_role")
    else:
        _fail(CONDITION_INVALID, path, f"unknown operand selector {kind!r} (signal or key)")
    return value


def literal_type(value: Any, path: str) -> str:
    """The literal's type: bool, int64, float64 or string (no coercion)."""

    if type(value) is bool:
        return "bool"
    if type(value) is int:
        if not -MAX_SAFE_INTEGER <= value <= MAX_SAFE_INTEGER:
            _fail(
                CONDITION_INVALID,
                path,
                "integer literal is outside the exact range ±(2^53−1)",
            )
        return "int64"
    if type(value) is float:
        if not math.isfinite(value):
            _fail(CONDITION_INVALID, path, "number literal must be finite")
        # An integral value is an int64 literal (schema02 §1 literal rules).
        if value.is_integer():
            if abs(value) > MAX_SAFE_INTEGER:
                _fail(
                    CONDITION_INVALID,
                    path,
                    "integer literal is outside the exact range ±(2^53−1)",
                )
            return "int64"
        return "float64"
    if type(value) is str:
        return "string"
    _fail(
        CONDITION_INVALID,
        path,
        "must be one bool, number or string literal (no operand-to-operand compare)",
    )
    raise AssertionError("unreachable")


def iter_compares(
    condition: Any, path: str = "interrupt_condition"
) -> Iterator[tuple[str, Mapping[str, Any]]]:
    """Each ``(path, compare)`` row of a structurally checked condition."""

    pending = [(condition, path)]
    while pending:
        node, node_path = pending.pop()
        ((kind, item),) = node.items()
        if kind == "compare":
            yield f"{node_path}.compare", item
        elif kind == "not":
            pending.append((item, f"{node_path}.not"))
        else:
            pending.extend(
                (child, f"{node_path}.{kind}[{index}]")
                for index, child in reversed(list(enumerate(item)))
            )


def check_condition_structure(value: Any, path: str = "interrupt_condition") -> None:
    """Refuse a malformed condition or one beyond the authored-tree limits."""

    nodes = 0
    pending = [(value, path, 1)]
    while pending:
        node, node_path, depth = pending.pop()
        if depth > MAX_CONDITION_DEPTH:
            _fail(CONDITION_INVALID, node_path, f"exceeds the depth bound {MAX_CONDITION_DEPTH}")
        nodes += 1
        if nodes > MAX_CONDITION_NODES:
            _fail(CONDITION_INVALID, path, f"exceeds the bound of {MAX_CONDITION_NODES} nodes")
        kind, item = _single(node, node_path, "operator (all, any, not or compare)")
        if kind not in _OPERATORS:
            _fail(CONDITION_INVALID, node_path, f"unknown operator {kind!r}")
        if kind in {"all", "any"}:
            if type(item) is not list or not item:
                _fail(CONDITION_INVALID, f"{node_path}.{kind}", "must be a non-empty list")
            if len(item) > MAX_CONDITION_NODES:
                _fail(CONDITION_INVALID, path, f"exceeds the bound of {MAX_CONDITION_NODES} nodes")
            pending.extend(
                (child, f"{node_path}.{kind}[{index}]", depth + 1)
                for index, child in reversed(list(enumerate(item)))
            )
        elif kind == "not":
            pending.append((item, f"{node_path}.not", depth + 1))
        else:
            row_path = f"{node_path}.compare"
            row = _closed(item, ("operand", "op", "value"), row_path)
            _check_operand(row["operand"], f"{row_path}.operand")
            if row["op"] not in OPERATORS:
                _fail(CONDITION_INVALID, f"{row_path}.op", f"must be one of {list(OPERATORS)}")
            literal_type(row["value"], f"{row_path}.value")


def condition_structure_issue(value: Any) -> tuple[str, str] | None:
    """``(path, message)`` of the first structural refusal, else None."""

    try:
        check_condition_structure(value, path="$")
    except InterruptConditionError as error:
        return error.path, f"{error.reason}: {error.message}"
    return None


def check_condition(
    value: Any,
    *,
    key_types: Mapping[str, str],
    target_resource_roles: Mapping[str, frozenset[str] | set[str] | tuple[str, ...]],
    path: str = "interrupt_condition",
) -> None:
    """The full Program check: structure, declarations and operator typing.

    ``key_types`` maps every declared key (authored, computed, system
    ``node_outcome.*``) to its declared type; ``target_resource_roles`` maps
    each declared target role to the resource roles it requires.
    """

    check_condition_structure(value, path=path)
    for row_path, row in iter_compares(value, path):
        operand = row["operand"]
        if "key" in operand:
            key = operand["key"]
            declared = key_types.get(key)
            if declared is None:
                _fail(
                    CONDITION_KEY_REFUSED,
                    f"{row_path}.operand.key",
                    f"{key!r} is not a declared blackboard key",
                )
            if declared not in SCALAR_KEY_TYPES:
                _fail(
                    CONDITION_KEY_REFUSED,
                    f"{row_path}.operand.key",
                    f"{key!r} has type {declared!r}; a condition compares only "
                    "bool, int64, float64 or string keys",
                )
            operand_type = declared
        else:
            signal = operand["signal"]
            roles = target_resource_roles.get(signal["target_role"])
            if roles is None:
                _fail(
                    CONDITION_INVALID,
                    f"{row_path}.operand.signal.target_role",
                    "does not name a declared target role",
                )
            if signal["resource_role"] not in roles:
                _fail(
                    CONDITION_INVALID,
                    f"{row_path}.operand.signal.resource_role",
                    f"is not a resource role required by target role "
                    f"{signal['target_role']!r}",
                )
            operand_type = SIGNAL_VALUE_TYPE
        op = row["op"]
        if operand_type in {"bool", "string"} and op not in EQUALITY_OPERATORS:
            _fail(
                CONDITION_TYPE_MISMATCH,
                f"{row_path}.op",
                f"a {operand_type} operand accepts only == and !=",
            )
        value_type = literal_type(row["value"], f"{row_path}.value")
        numeric = {"int64", "float64"}
        if not (
            value_type == operand_type
            or (operand_type in numeric and value_type in numeric)
        ):
            _fail(
                CONDITION_TYPE_MISMATCH,
                f"{row_path}.value",
                f"a {value_type} literal does not compare with a {operand_type} "
                "operand (no coercion)",
            )


def condition_keys(value: Any) -> frozenset[str]:
    """The blackboard keys a structurally checked condition names as operands
    (a computed key's own dependencies are not included)."""

    return frozenset(
        row["operand"]["key"]
        for _path, row in iter_compares(value)
        if "key" in row["operand"]
    )


def condition_signals(value: Any) -> frozenset[tuple[str, str]]:
    """The ``(target_role, resource_role)`` signals a structurally checked
    condition names as operands."""

    return frozenset(
        (row["operand"]["signal"]["target_role"], row["operand"]["signal"]["resource_role"])
        for _path, row in iter_compares(value)
        if "signal" in row["operand"]
    )


def json_schema() -> dict[str, Any]:
    """Self-contained JSON Schema of the grammar; ``#`` is the condition."""

    child = {"$ref": "#"}
    branch = {"type": "array", "minItems": 1, "items": child}
    operand = {
        "oneOf": [
            {
                "type": "object",
                "additionalProperties": False,
                "required": ["signal"],
                "properties": {
                    "signal": {
                        "type": "object",
                        "additionalProperties": False,
                        "required": ["resource_role", "target_role"],
                        "properties": {
                            "resource_role": {"type": "string"},
                            "target_role": {"type": "string"},
                        },
                    }
                },
            },
            {
                "type": "object",
                "additionalProperties": False,
                "required": ["key"],
                "properties": {"key": {"type": "string"}},
            },
        ]
    }
    compare = {
        "type": "object",
        "additionalProperties": False,
        "required": ["operand", "op", "value"],
        "properties": {
            "op": {"type": "string", "enum": list(OPERATORS)},
            "operand": operand,
            "value": {
                "anyOf": [
                    {"type": "boolean"},
                    {"type": "integer"},
                    {"type": "number"},
                    {"type": "string"},
                ]
            },
        },
    }
    return {
        "type": "object",
        "additionalProperties": False,
        "minProperties": 1,
        "maxProperties": 1,
        "properties": {"all": branch, "any": branch, "compare": compare, "not": child},
        "x-manyforge-condition-grammar": "interrupt_condition",
    }


__all__ = [
    "CONDITION_INVALID",
    "CONDITION_KEY_REFUSED",
    "CONDITION_TYPE_MISMATCH",
    "InterruptConditionError",
    "MAX_CONDITION_DEPTH",
    "MAX_CONDITION_NODES",
    "check_condition",
    "check_condition_structure",
    "condition_keys",
    "condition_structure_issue",
    "iter_compares",
    "json_schema",
    "literal_type",
]
