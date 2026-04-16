"""Helpers for validating direct-control intervention subtrees."""

from __future__ import annotations

from typing import Any

from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import BlackboardKey, TreeNode


def merged_intervention_blackboard_keys(
    tree: TreeNode,
    catalog: NodeCatalog,
    *,
    base_keys: dict[str, BlackboardKey] | None,
) -> dict[str, BlackboardKey]:
    """Return program keys plus subtree-local writer keys for intervention validation.

    Interventions are temporary subtrees. They should be able to introduce local
    blackboard keys without requiring edits to `program.blackboard.keys`, while
    still validating against the normal catalog blackboard metadata.
    """
    merged = dict(base_keys or {})

    def visit(node: TreeNode) -> None:
        try:
            entry = catalog.get(node.id)
        except KeyError:
            return

        parameters_by_name = {parameter.name: parameter for parameter in entry.parameters}
        for param_name in entry.bb_writer_params:
            parameter = parameters_by_name.get(param_name)
            if parameter is None or parameter.blackboard_role != "key_writer":
                continue
            raw_value: Any = node.params.get(param_name)
            if not isinstance(raw_value, dict):
                continue
            key_id = raw_value.get("key_ref")
            if not isinstance(key_id, str) or not key_id:
                continue
            if key_id in merged:
                continue
            merged[key_id] = BlackboardKey(
                id=key_id,
                type=parameter.blackboard_value_type or parameter.type,
                key=key_id.lstrip("/").replace(".", "/"),
                description="Ephemeral intervention-local blackboard key",
            )

        for child in node.children:
            visit(child)

    visit(tree)
    return merged
