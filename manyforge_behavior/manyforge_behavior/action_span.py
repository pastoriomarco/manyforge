"""Behavior-owned action-span helpers.

Action spans are minted by the behavior layer because behavior nodes own the
program-node attempt lifecycle. Kernel and backend layers only attach the
span to events and terminal outcomes.
"""

from __future__ import annotations

from typing import Any

import py_trees

from manyforge_behavior.kernel_client import ActionSpan


def read_program_run_id(blackboard: Any) -> str | None:
    """Best-effort program-run correlation id (spec/500 §2.2)."""
    try:
        value = blackboard.get("program_run_id")
    except (KeyError, AttributeError):
        return None
    return str(value) if value else None


def tree_path_for_node(node: py_trees.behaviour.Behaviour) -> str:
    """Return a stable path with sibling indexes, not display names alone."""
    parts: list[str] = []
    current: py_trees.behaviour.Behaviour | None = node
    while current is not None:
        parent = getattr(current, "parent", None)
        index = 0
        if parent is not None:
            siblings = list(getattr(parent, "children", []) or [])
            for i, sibling in enumerate(siblings):
                if sibling is current:
                    index = i
                    break
        safe_name = str(getattr(current, "name", "node") or "node").replace("/", "_")
        parts.append(f"{safe_name}[{index}]")
        current = parent
    return "/".join(reversed(parts))


class ActionSpanMixin:
    """Mixin for py_trees nodes that dispatch kernel-visible actions."""

    _manyforge_action_attempt_counter: int
    _manyforge_action_span: ActionSpan | None

    def _reset_action_span(self) -> None:
        if not hasattr(self, "_manyforge_action_attempt_counter"):
            self._manyforge_action_attempt_counter = 0
        self._manyforge_action_attempt_counter += 1
        self._manyforge_action_span = None

    def _current_action_span(
        self,
        blackboard: Any,
        *,
        parent_node_attempt_id: str | None = None,
    ) -> ActionSpan:
        span = getattr(self, "_manyforge_action_span", None)
        if span is not None:
            return span
        if not hasattr(self, "_manyforge_action_attempt_counter"):
            self._manyforge_action_attempt_counter = 1
        program_run_id = read_program_run_id(blackboard)
        node_instance_id = tree_path_for_node(self)  # type: ignore[arg-type]
        node_attempt_id = (
            f"{program_run_id or 'standalone'}:{node_instance_id}:"
            f"{self._manyforge_action_attempt_counter}"
        )
        span = ActionSpan(
            action_source="program_node",
            action_span_id=node_attempt_id,
            program_run_id=program_run_id,
            node_instance_id=node_instance_id,
            node_attempt_id=node_attempt_id,
            parent_node_attempt_id=parent_node_attempt_id,
        )
        self._manyforge_action_span = span
        return span
