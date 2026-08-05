"""Structural Composer state plus an optional replaceable headless client.

The former Composer runtime state machine, scene cache, node discovery owner,
Runner/CycleManager construction, and robot-control authorities were removed
with the legacy scene owner. This state contains only immutable built-in
catalog identity, an optional authored deployment used for structural URDF
viewing, and an optional HTTP client that owns no runtime truth.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

from manyforge_behavior.deployment import AuthoredStructuralDeployment
from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment

if TYPE_CHECKING:
    from manyforge_headless_api.client import HeadlessHttpClient


RUNTIME_UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RUNTIME_UNAVAILABLE_MESSAGE = (
    "Composer is not a runtime owner; configure the headless API for H0 status, "
    "scene, alarm, and history projections, while execution, lifecycle, Stop, "
    "provider, simulation, and robot mutation remain gated by later plan steps"
)


class ComposerState:
    """Authoring state and a replaceable client, never runtime authority."""

    def __init__(
        self,
        *,
        node_environment: NodeEnvironment | None = None,
        deployment: AuthoredStructuralDeployment | None = None,
        deployment_source_path: str | None = None,
        allowed_fs_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
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
        self.headless_client = headless_client

    @classmethod
    def from_deployment_path(
        cls,
        deployment_path: str | None,
        *,
        allowed_fs_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
    ) -> "ComposerState":
        if deployment_path is None:
            return cls(
                allowed_fs_roots=allowed_fs_roots,
                headless_client=headless_client,
            )
        resolved = str(Path(deployment_path).resolve())
        return cls(
            deployment=AuthoredStructuralDeployment.from_path(resolved),
            deployment_source_path=resolved,
            allowed_fs_roots=allowed_fs_roots,
            headless_client=headless_client,
        )

    @property
    def node_catalog(self):
        return self.node_environment.catalog
