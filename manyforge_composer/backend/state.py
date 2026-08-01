"""Structural-only Composer state during the composable-cell cutover.

The former Composer runtime state machine, scene cache, node discovery owner,
Runner/CycleManager construction, and robot-control authorities were removed
with the legacy scene owner.  This state contains only immutable built-in
catalog identity and an optional authored deployment used for structural URDF
viewing.  Runtime clients are rejected by the application boundary.
"""

from __future__ import annotations

from pathlib import Path

from manyforge_behavior.deployment import AuthoredStructuralDeployment
from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment


RUNTIME_UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RUNTIME_UNAVAILABLE_MESSAGE = (
    "Composer runtime, scene, execution, lifecycle, assistant, and MCP surfaces "
    "are intentionally unavailable during the composable-cell runtime cutover"
)


class ComposerState:
    """Minimal non-runtime state for catalog and structural model inspection."""

    def __init__(
        self,
        *,
        node_environment: NodeEnvironment | None = None,
        deployment: AuthoredStructuralDeployment | None = None,
        deployment_source_path: str | None = None,
        allowed_fs_roots: list[str] | None = None,
    ) -> None:
        if deployment is not None and deployment_source_path is None:
            raise ValueError(
                "deployment_source_path is required with an authored deployment"
            )
        resolved_deployment_source = (
            Path(deployment_source_path).resolve()
            if deployment_source_path is not None
            else None
        )
        if (
            deployment is not None
            and deployment.source_path.resolve() != resolved_deployment_source
        ):
            raise ValueError(
                "authored structural projection source does not match "
                "deployment_source_path"
            )
        self.node_environment = node_environment or build_node_environment()
        self.deployment = deployment
        self.deployment_source_path = (
            str(resolved_deployment_source)
            if resolved_deployment_source is not None
            else None
        )
        self.allowed_fs_roots = list(allowed_fs_roots or [])

    @classmethod
    def from_deployment_path(
        cls,
        deployment_path: str | None,
        *,
        allowed_fs_roots: list[str] | None = None,
    ) -> "ComposerState":
        if deployment_path is None:
            return cls(allowed_fs_roots=allowed_fs_roots)
        resolved = str(Path(deployment_path).resolve())
        return cls(
            deployment=AuthoredStructuralDeployment.from_path(resolved),
            deployment_source_path=resolved,
            allowed_fs_roots=allowed_fs_roots,
        )

    @property
    def node_catalog(self):
        return self.node_environment.catalog
