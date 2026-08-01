"""Pure boolean/signal reduction and I6/N logic-node gates."""

from __future__ import annotations

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


class ConfirmGripperGrasp(_UnavailableLogicNode):
    pass


class WaitForGripperState(_UnavailableLogicNode):
    pass
