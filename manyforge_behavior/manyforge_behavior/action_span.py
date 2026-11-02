"""Action-span value export and I6/N behavior-authoring gate."""

from __future__ import annotations

from typing import Final

from manyforge_behavior.kernel_client import ActionSpan


ACTION_SPAN_AUTHORING_UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_action_span_authoring_unavailable"
)
ACTION_SPAN_AUTHORING_RESTORATION_GATES: Final = ("I6", "N")


class ActionSpanAuthoringUnavailableError(RuntimeError):
    """Raised before a tree, blackboard, run identity, or counter is accessed."""


def _raise_action_span_authoring_unavailable() -> None:
    raise ActionSpanAuthoringUnavailableError(
        f"{ACTION_SPAN_AUTHORING_UNAVAILABLE_REASON_CODE}: Program-Binding "
        "node-attempt authoring returns at I6/N"
    )


def read_program_run_id(_blackboard: object) -> None:
    _raise_action_span_authoring_unavailable()


def tree_path_for_node(_node: object) -> None:
    _raise_action_span_authoring_unavailable()


class ActionSpanMixin:
    """Gate inherited by legacy action-dispatching behavior nodes."""

    def _reset_action_span(self) -> None:
        _raise_action_span_authoring_unavailable()

    def _current_action_span(self, *_args: object, **_kwargs: object) -> None:
        _raise_action_span_authoring_unavailable()


__all__ = [
    "ACTION_SPAN_AUTHORING_RESTORATION_GATES",
    "ActionSpan",
    "ActionSpanAuthoringUnavailableError",
    "ActionSpanMixin",
    "read_program_run_id",
    "tree_path_for_node",
]
