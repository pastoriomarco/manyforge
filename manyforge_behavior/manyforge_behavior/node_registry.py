"""Immutable v2 executable-registry identity.

The v2 catalog deliberately carries stable implementation IDs, not import
paths.  This module therefore builds the exact immutable registry projection
without inventing a parallel Python factory table.  Runtime resolution belongs
to the later headless environment/availability owner.
"""

from __future__ import annotations

from dataclasses import dataclass
from types import MappingProxyType

from manyforge_behavior.catalog import NodeCatalog


BUILTIN_EXECUTABLE_PACKAGE_ID = "manyforge_behavior"
_BUILD_TOKEN = object()


class NodeRegistryError(ValueError):
    """Executable-registry identity is invalid or unavailable at runtime."""


@dataclass(frozen=True, order=True)
class ExecutableRegistryRow:
    node_id: str
    engine_id: str
    implementation_id: str
    executable_package_id: str

    def to_dict(self) -> dict[str, str]:
        return {
            "node_id": self.node_id,
            "engine_id": self.engine_id,
            "implementation_id": self.implementation_id,
            "executable_package_id": self.executable_package_id,
        }


class NodeRegistry:
    """Read-only total node/engine/implementation/package relation."""

    def __init__(
        self,
        rows: tuple[ExecutableRegistryRow, ...],
        *,
        _token: object,
    ):
        if _token is not _BUILD_TOKEN:
            raise NodeRegistryError(
                "NodeRegistry is build-owned; use build_node_registry()"
            )
        if not rows:
            raise NodeRegistryError("executable registry must not be empty")
        if rows != tuple(sorted(rows)):
            raise NodeRegistryError("executable registry rows must be canonical")
        identities = {
            (row.node_id, row.engine_id)
            for row in rows
        }
        if len(identities) != len(rows):
            raise NodeRegistryError(
                "executable registry contains duplicate node/engine rows"
            )
        self._rows = rows
        self._by_node_engine = MappingProxyType(
            {(row.node_id, row.engine_id): row for row in rows}
        )

    def record(
        self,
        node_id: str,
        engine_id: str = "py_trees",
    ) -> ExecutableRegistryRow:
        try:
            return self._by_node_engine[(node_id, engine_id)]
        except KeyError as exc:
            raise NodeRegistryError(
                f"node {node_id!r} has no {engine_id!r} executable registry row"
            ) from exc

    def contains(self, node_id: str, engine_id: str = "py_trees") -> bool:
        return (node_id, engine_id) in self._by_node_engine

    @property
    def rows(self) -> tuple[ExecutableRegistryRow, ...]:
        return self._rows

    @property
    def node_ids(self) -> tuple[str, ...]:
        return tuple(
            row.node_id
            for row in self._rows
            if row.engine_id == "py_trees"
        )

    def canonical_projection(self) -> list[dict[str, str]]:
        return [row.to_dict() for row in self._rows]

def build_node_registry(catalog: NodeCatalog) -> NodeRegistry:
    """Derive the total canonical built-in registry from catalog runner rows."""

    rows = tuple(
        sorted(
            ExecutableRegistryRow(
                node_id=entry.id,
                engine_id=engine_id,
                implementation_id=implementation_id,
                executable_package_id=BUILTIN_EXECUTABLE_PACKAGE_ID,
            )
            for entry in catalog.entries
            for engine_id, implementation_id in entry.runner_implementation_ids.items()
        )
    )
    expected = {
        (entry.id, engine_id, implementation_id)
        for entry in catalog.entries
        for engine_id, implementation_id in entry.runner_implementation_ids.items()
    }
    observed = {
        (row.node_id, row.engine_id, row.implementation_id)
        for row in rows
    }
    if observed != expected:
        raise NodeRegistryError(
            "executable registry does not exactly cover catalog runner metadata"
        )
    return NodeRegistry(rows, _token=_BUILD_TOKEN)
