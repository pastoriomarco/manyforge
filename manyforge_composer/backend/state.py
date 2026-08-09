"""Structural Composer state plus an optional replaceable headless client.

The former Composer runtime state machine, scene cache, node discovery owner,
Runner/CycleManager construction, and robot-control authorities were removed
with the legacy scene owner. This state contains only immutable built-in
catalog identity, optional authored deployment/Program inputs used for
structural viewing and an in-memory Draft, and an optional HTTP client that
owns no runtime truth.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING

from manyforge_behavior.deployment import AuthoredStructuralDeployment
from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment
from manyforge_behavior.program import Program

from .draft_session import ComposerDraftSession

if TYPE_CHECKING:
    from manyforge_headless_api.client import HeadlessHttpClient

    from .assistant_provider_client import AssistantProviderHttpClient


RUNTIME_UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RUNTIME_UNAVAILABLE_MESSAGE = (
    "Composer is not a runtime owner; configure the headless API for owner-issued "
    "status, scene, alarm/history, preview, operation, and operation-Stop facts. "
    "Apply, lifecycle, full Stop, simulation, and product launch remain at their "
    "later gates. The assistant operator workflow also requires a configured "
    "assistant bridge"
)


class ComposerState:
    """Authoring state and a replaceable client, never runtime authority."""

    def __init__(
        self,
        *,
        node_environment: NodeEnvironment | None = None,
        deployment: AuthoredStructuralDeployment | None = None,
        deployment_source_path: str | None = None,
        program: Program | None = None,
        program_source_path: str | None = None,
        allowed_fs_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
        assistant_client: "AssistantProviderHttpClient | None" = None,
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
        resolved_program_source = (
            Path(program_source_path).resolve()
            if program_source_path is not None
            else None
        )
        if (
            program is not None
            and program.source_path is not None
            and resolved_program_source is not None
            and program.source_path.resolve() != resolved_program_source
        ):
            raise ValueError("authored Program source does not match program_source_path")
        self.node_environment = node_environment or build_node_environment()
        self.deployment = deployment
        self.deployment_source_path = (
            str(resolved_deployment_source)
            if resolved_deployment_source is not None
            else None
        )
        self.program_source_path = (
            str(resolved_program_source)
            if resolved_program_source is not None
            else None
        )
        self.draft_session = ComposerDraftSession(
            program,
            source_path=self.program_source_path,
        )
        self.allowed_fs_roots = list(allowed_fs_roots or [])
        self.headless_client = headless_client
        self.assistant_client = assistant_client

    @classmethod
    def from_deployment_path(
        cls,
        deployment_path: str | None,
        *,
        allowed_fs_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
        assistant_client: "AssistantProviderHttpClient | None" = None,
    ) -> "ComposerState":
        return cls.from_paths(
            deployment_path=deployment_path,
            program_path=None,
            allowed_fs_roots=allowed_fs_roots,
            headless_client=headless_client,
            assistant_client=assistant_client,
        )

    @classmethod
    def from_paths(
        cls,
        *,
        deployment_path: str | None,
        program_path: str | None,
        allowed_fs_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
        assistant_client: "AssistantProviderHttpClient | None" = None,
    ) -> "ComposerState":
        node_environment = build_node_environment()
        resolved_program = (
            str(Path(program_path).resolve()) if program_path is not None else None
        )
        program = (
            Program(resolved_program, node_environment.catalog)
            if resolved_program is not None
            else None
        )
        if deployment_path is None:
            return cls(
                node_environment=node_environment,
                program=program,
                program_source_path=resolved_program,
                allowed_fs_roots=allowed_fs_roots,
                headless_client=headless_client,
                assistant_client=assistant_client,
            )
        resolved = str(Path(deployment_path).resolve())
        return cls(
            node_environment=node_environment,
            deployment=AuthoredStructuralDeployment.from_path(resolved),
            deployment_source_path=resolved,
            program=program,
            program_source_path=resolved_program,
            allowed_fs_roots=allowed_fs_roots,
            headless_client=headless_client,
            assistant_client=assistant_client,
        )

    @property
    def node_catalog(self):
        return self.node_environment.catalog
