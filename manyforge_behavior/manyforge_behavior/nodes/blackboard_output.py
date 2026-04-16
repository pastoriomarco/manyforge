"""Shared helpers for nodes that publish bundle-based namespace outputs."""

from __future__ import annotations

from typing import Any

import py_trees

from manyforge_behavior.blackboard_namespace import write_namespace_bundle


class BlackboardOutputMixin:
    _bb: py_trees.blackboard.Client
    _output_key_prefix: str

    def _write_namespace_output(
        self,
        value: Any,
        *,
        status: str,
        **meta: Any,
    ) -> None:
        write_namespace_bundle(
            self._bb,
            self._output_key_prefix,
            value=value,
            status=status,
            **meta,
        )
