"""Parameter resolution for ManyForge program artifacts."""

from __future__ import annotations

import re
from typing import Any

import py_trees

from manyforge_behavior.blackboard_namespace import (
    RuntimeFieldRef,
    read_blackboard_binding_value,
)


PARAM_REF_RE = re.compile(r"^\$\{param:([^}]+)\}$")


class ParameterResolver:
    """Resolves ${param:name} references using program defaults and runtime overrides."""

    def __init__(
        self,
        program_params: dict[str, Any],
        overrides: dict[str, Any] | None = None,
    ):
        self._defaults = dict(program_params)
        self._overrides = dict(overrides) if overrides else {}

    def resolve_value(self, value: Any) -> Any:
        """Resolve a single value. If it's a ${param:name} reference, look it up."""
        if not isinstance(value, str):
            return value
        m = PARAM_REF_RE.match(value)
        if not m:
            return value
        name = m.group(1)
        if name in self._overrides:
            return self._overrides[name]
        if name in self._defaults:
            return self._defaults[name]
        raise ValueError(f"Unknown parameter reference: '{name}'")

    def resolve(self, params: dict[str, Any]) -> dict[str, Any]:
        """Resolve all ${param:name} references in a params dict. Returns new dict."""
        result: dict[str, Any] = {}
        for key, value in params.items():
            result[key] = self._resolve_recursive(value)
        return result

    def _resolve_recursive(self, value: Any) -> Any:
        if isinstance(value, str):
            return self.resolve_value(value)
        if isinstance(value, list):
            return [self._resolve_recursive(v) for v in value]
        if isinstance(value, dict):
            return {k: self._resolve_recursive(v) for k, v in value.items()}
        return value


def resolve_blackboard_value(
    blackboard: py_trees.blackboard.Client,
    value: Any,
) -> Any:
    """Resolve typed runtime blackboard bindings recursively."""
    if isinstance(value, RuntimeFieldRef):
        return read_blackboard_binding_value(blackboard, value)
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        return [resolve_blackboard_value(blackboard, item) for item in value]
    if isinstance(value, dict):
        return {
            item_key: resolve_blackboard_value(blackboard, item_value)
            for item_key, item_value in value.items()
        }
    return value
