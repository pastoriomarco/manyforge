"""Pure boolean/signal reduction and I6/N logic-node gates."""

from __future__ import annotations

import math
from typing import Final, Literal


LOGIC_NODE_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_logic_node_binding_unavailable"
)
LOGIC_NODE_RESTORATION_GATES: Final = ("I6", "N")
BooleanEdge = Literal["rising", "falling", "either"]


class LogicNodeBindingUnavailableError(RuntimeError):
    """Raised before blackboard, gripper, Kernel, or pause state is accessed."""


def boolean_condition_matches(value: object, *, expected: bool) -> bool:
    """Match only actual booleans; integers are not signal truth."""
    if type(expected) is not bool:
        raise TypeError("expected must be a bool")
    return type(value) is bool and value is expected


EQUALITY_OPERATORS: Final = frozenset({"==", "!="})
ORDERING_OPERATORS: Final = frozenset({"<", "<=", ">", ">="})


def comparison_supported(
    value_type: str,
    operator: str,
    *,
    has_tolerance: bool,
) -> str | None:
    """Return why a key type cannot use an operator/tolerance, else None."""
    numeric = value_type in {"int64", "float64"}
    if value_type not in {"bool", "float64", "int64", "string"}:
        return f"{value_type} keys cannot be compared; use Check pose distance for poses"
    if operator not in EQUALITY_OPERATORS | ORDERING_OPERATORS:
        return f"operator {operator!r} is unsupported"
    if not numeric and operator not in EQUALITY_OPERATORS:
        return f"{value_type} keys support only == and !="
    if has_tolerance and (not numeric or operator not in EQUALITY_OPERATORS):
        return "tolerance applies only to == and != on numeric keys"
    return None


def compare_values(
    actual: object,
    operator: str,
    expected: object,
    *,
    value_type: str,
    tolerance: float | None = None,
) -> bool:
    """Compare one typed key value; mismatched types raise instead of coercing."""
    reason = comparison_supported(value_type, operator, has_tolerance=tolerance is not None)
    if reason is not None:
        raise ValueError(reason)
    if value_type in {"bool", "string"}:
        kind = bool if value_type == "bool" else str
        if type(actual) is not kind or type(expected) is not kind:
            raise TypeError(f"compared values are not both {value_type}")
        return (actual == expected) is (operator == "==")
    if any(
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        for value in (actual, expected)
    ):
        raise TypeError("compared values are not both finite numbers")
    if value_type == "int64" and (type(actual) is not int or type(expected) is not int):
        raise TypeError("compared values are not both int64")
    if operator in EQUALITY_OPERATORS:
        equal = (
            actual == expected
            if tolerance is None
            else abs(float(actual) - float(expected)) <= tolerance
        )
        return equal is (operator == "==")
    return {
        "<": actual < expected,
        "<=": actual <= expected,
        ">": actual > expected,
        ">=": actual >= expected,
    }[operator]


def detect_boolean_edge(
    previous: bool,
    current: bool,
    *,
    edge: BooleanEdge,
) -> bool:
    """Reduce one pair of strict boolean samples without storing state."""
    if type(previous) is not bool or type(current) is not bool:
        raise TypeError("edge samples must be bool values")
    if edge == "rising":
        return previous is False and current is True
    if edge == "falling":
        return previous is True and current is False
    if edge == "either":
        return previous is not current
    raise ValueError("edge must be rising, falling, or either")


class _UnavailableLogicNode:
    def __init__(self, *_args: object, **_kwargs: object) -> None:
        raise LogicNodeBindingUnavailableError(
            f"{LOGIC_NODE_UNAVAILABLE_REASON_CODE}: bound logic and gripper "
            "nodes return at I6/N"
        )


class CheckKeyBoolValue(_UnavailableLogicNode):
    pass


class SetKeyBoolValue(_UnavailableLogicNode):
    pass


class WaitForKeyBool(_UnavailableLogicNode):
    pass


class CheckSignalBool(CheckKeyBoolValue):
    pass


class SetSignalBool(SetKeyBoolValue):
    pass


class WaitForSignalBool(WaitForKeyBool):
    pass


class WaitForSignalEdge(_UnavailableLogicNode):
    pass


class CheckGripperState(_UnavailableLogicNode):
    pass


class SetGripperState(_UnavailableLogicNode):
    pass


class CommandGripper(_UnavailableLogicNode):
    pass


class WaitForGripperState(_UnavailableLogicNode):
    pass
