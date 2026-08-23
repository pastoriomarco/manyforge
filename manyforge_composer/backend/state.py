"""Structural Composer state plus an optional replaceable headless client.

The former Composer runtime state machine, scene cache, node discovery owner,
Runner/CycleManager construction, and robot-control authorities were removed
with the legacy scene owner. This state contains only immutable built-in
catalog identity, an optional authored Program in one in-memory Draft, and
optional HTTP clients that own no runtime truth.
"""

from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import os
from typing import Any, TYPE_CHECKING

from fastapi.responses import JSONResponse
from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_headless_api.program_binding_projection import (
    validate_program_apply_result,
)

from .draft_session import ComposerDraftSession
from .authoring_admission import AuthoringAdmissionCoordinator
from .authoring_transaction_journal import (
    AuthoringJournalError,
    AuthoringTransaction,
)
from .node_environment_client import read_active_node_environment
from .program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
    save_program_artifact,
)

if TYPE_CHECKING:
    from manyforge_headless_api.client import HeadlessHttpClient

    from .assistant_provider_client import AssistantProviderHttpClient


RUNTIME_UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RUNTIME_UNAVAILABLE_MESSAGE = (
    "Composer is not a runtime owner; configure the headless API for owner-issued "
    "status, scene, alarm/history, preview, operation, Program Apply/Run, and "
    "explicit operation/Program Stop facts. The structural-only Composer profile "
    "does not launch those owners. I6 cumulative qualification, lifecycle, full "
    "Stop, simulation, real-robot, and generalized-tool work remain at their "
    "later gates. The "
    "assistant operator workflow also requires a configured "
    "assistant bridge"
)


class ComposerState:
    """Authoring state and a replaceable client, never runtime authority."""

    def __init__(
        self,
        *,
        node_environment: NodeEnvironment | None = None,
        program: Program | None = None,
        program_source_path: str | None = None,
        allowed_fs_roots: list[str] | None = None,
        allowed_program_write_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
        authoring_state_root: str | None = None,
        assistant_client: "AssistantProviderHttpClient | None" = None,
    ) -> None:
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
        self.allowed_program_write_roots = (
            None
            if allowed_program_write_roots is None
            else list(allowed_program_write_roots)
        )
        self.headless_client = headless_client
        authoring_state_root = authoring_state_root or os.environ.get(
            "MANYFORGE_COMPOSER_AUTHORING_STATE_ROOT"
        )
        if headless_client is not None and authoring_state_root is None:
            raise ValueError(
                "authoring_state_root is required with Headless-backed authoring"
            )
        self.authoring_admission = (
            AuthoringAdmissionCoordinator(
                headless_client,
                Path(authoring_state_root),
                restore_post_image=self._restore_authoring_post_image,
                recover_effect=self._recover_authoring_effect,
            )
            if headless_client is not None
            else None
        )
        if self.authoring_admission is not None:
            self.authoring_admission.recover()
        self.assistant_client = assistant_client
        # Presentation-cache identity only. Runtime and Apply truth remain in
        # Headless/Manager; this value solely rejects an old browser mesh URL
        # after Composer has observed a successor viewer manifest.
        self.viewer_model_key: str | None = None

    @classmethod
    def from_paths(
        cls,
        *,
        program_path: str | None,
        allowed_fs_roots: list[str] | None = None,
        allowed_program_write_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
        authoring_state_root: str | None = None,
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
        return cls(
            node_environment=node_environment,
            program=program,
            program_source_path=resolved_program,
            allowed_fs_roots=allowed_fs_roots,
            allowed_program_write_roots=allowed_program_write_roots,
            headless_client=headless_client,
            authoring_state_root=authoring_state_root,
            assistant_client=assistant_client,
        )

    @property
    def node_catalog(self):
        return self.node_environment.catalog

    def _active_authoring_catalog(self):
        return (
            read_active_node_environment(self).catalog
            if self.headless_client is not None
            else self.node_catalog
        )

    def _restore_authoring_post_image(self, value: dict[str, Any]) -> None:
        self.draft_session.restore_retained_state(
            value, self._active_authoring_catalog()
        )

    def _recover_program_apply(
        self, intent: dict[str, Any], claim: dict[str, Any]
    ) -> dict[str, Any]:
        if self.headless_client is None:
            raise AuthoringJournalError(
                "composer_authoring_recovery_unavailable",
                "Program Apply recovery requires Headless",
            )
        result = self.headless_client.program_apply(
            {
                "scenario_id": intent["scenario_id"],
                "scenario_catalog_revision": intent[
                    "scenario_catalog_revision"
                ],
                "program": deepcopy(intent["program_document"]),
                "expected_applied_record_digest": intent[
                    "expected_applied_record_digest"
                ],
                "authoring_claim": claim,
            }
        )
        validate_program_apply_result(result)
        if result.get("status") not in {"applied", "pending_deployment"}:
            raise AuthoringJournalError(
                "composer_authoring_recovery_invalid",
                "Program Apply recovery returned an unsupported status",
            )
        return result

    def _recovery_workspace(
        self,
        *,
        applied_program_revision: str | None,
        applied_unavailable_reason: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        return self.draft_session.workspace_snapshot(
            applied_program_revision=applied_program_revision,
            applied_unavailable_reason=applied_unavailable_reason,
        )

    def _recover_authoring_effect(
        self, transaction: AuthoringTransaction, claim: dict[str, Any]
    ) -> tuple[Any, dict[str, Any]] | None:
        intent = transaction.effect_intent
        if (
            not isinstance(intent, dict)
            or intent.get("schema_version")
            != "manyforge.composer_authoring_effect.v1"
            or intent.get("effect_class") != "external"
            or not isinstance(intent.get("post_state"), dict)
        ):
            raise AuthoringJournalError(
                "composer_authoring_recovery_invalid",
                "external authoring effect intent is incomplete",
            )
        kind = intent.get("external_kind")
        post_state = deepcopy(intent["post_state"])
        if kind == "save_program":
            try:
                program = Program.from_dict(
                    deepcopy(intent["program_document"]),
                    self._active_authoring_catalog(),
                    path="<composer-save-recovery>",
                )
            except ProgramValidationError as error:
                raise AuthoringJournalError(
                    "composer_authoring_recovery_invalid",
                    f"retained Save Program is invalid: {error}",
                ) from error
            exact_already_present = False
            try:
                present, present_path = load_program_artifact(
                    intent["target_path"],
                    allowed_roots=(
                        self.allowed_program_write_roots
                        if self.allowed_program_write_roots is not None
                        else self.allowed_fs_roots
                    ),
                    catalog=self._active_authoring_catalog(),
                )
                exact_already_present = (
                    present.to_dict() == program.to_dict()
                    and present_path == post_state["saved_path"]
                )
            except ProgramArtifactError as error:
                if error.code != "program_artifact_not_found" and not intent[
                    "overwrite"
                ]:
                    raise AuthoringJournalError(
                        "composer_authoring_save_recovery_conflict",
                        "Save target cannot be proven exact without overwrite authority",
                    ) from error
            if not exact_already_present:
                save_program_artifact(
                    program,
                    intent["target_path"],
                    allowed_roots=(
                        self.allowed_program_write_roots
                        if self.allowed_program_write_roots is not None
                        else self.allowed_fs_roots
                    ),
                    overwrite=bool(intent["overwrite"]),
                )
            self._restore_authoring_post_image(post_state)
            applied = intent.get("applied_context", {})
            return (
                self._recovery_workspace(
                    applied_program_revision=(
                        applied.get("program_revision")
                        if applied.get("available") is True
                        else None
                    ),
                    applied_unavailable_reason=(
                        applied.get("reason")
                        if applied.get("available") is not True
                        else None
                    ),
                ),
                post_state,
            )
        if kind in {"program_apply", "load_program", "revert_program"}:
            result = self._recover_program_apply(intent, claim)
            applied = result.get("applied")
            applied_revision = (
                applied.get("program_revision")
                if isinstance(applied, dict)
                else None
            )
            if result.get("status") == "applied":
                self._restore_authoring_post_image(post_state)
            else:
                post_state = deepcopy(intent["pre_state"])
                prior = intent.get("applied_context", {})
                applied_revision = (
                    prior.get("program_revision")
                    if prior.get("available") is True
                    else None
                )
            workspace = self._recovery_workspace(
                applied_program_revision=applied_revision
            )
            if kind == "program_apply":
                status = result["status"]
                response = JSONResponse(
                    status_code=200 if status == "applied" else 202,
                    content={
                        "status": status,
                        "headless": result,
                        "workspace": workspace,
                    },
                )
            elif kind == "load_program":
                loaded = result.get("status") == "applied"
                response = JSONResponse(
                    status_code=200 if loaded else 202,
                    content={
                        "status": "loaded" if loaded else "pending_deployment",
                        "headless": result,
                        "workspace": workspace,
                    },
                )
            else:
                reverted = result.get("status") == "applied"
                response = JSONResponse(
                    status_code=200 if reverted else 202,
                    content={
                        "status": "reverted" if reverted else "pending_deployment",
                        "headless": result,
                        "workspace": workspace,
                    },
                )
            return response, post_state
        raise AuthoringJournalError(
            "composer_authoring_recovery_invalid",
            f"unsupported external authoring recovery kind: {kind}",
        )
