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
import time
import uuid
from typing import Any, TYPE_CHECKING

from fastapi.responses import JSONResponse
from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_headless_api.program_binding_projection import (
    applied_program_belongs_to_scenario,
    applied_program_matches_scenario_selection,
    validate_program_apply_result,
)
from manyforge_headless_api.client import HeadlessClientError

from .draft_session import (
    ComposerDraftSession,
    DraftSessionError,
    draft_applicability,
)
from .authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringMutationEnvelope,
)
from .authoring_forest import from_program
from .authoring_transaction_journal import (
    AuthoringJournalError,
    AuthoringTransaction,
    canonical_digest,
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

_CURRENT_SCENARIO_FIELDS = (
    "scenario_id",
    "scenario_catalog_revision",
    "deployment_artifact_id",
    "deployment_revision",
    "retained_bundle_id",
    "graph_revision",
    "program_id",
    "program_revision",
    "program_binding_id",
)
_SCENARIO_LOAD_TIMEOUT_SECONDS = 20.0
_SCENARIO_LOAD_POLL_SECONDS = 0.1


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
        if result.get("status") != "applied":
            raise AuthoringJournalError(
                "composer_authoring_recovery_invalid",
                "Program Apply recovery returned an unsupported status",
            )
        return result

    def prepare_current_applied_scene(
        self, *, request_owner_epoch: str, request_id: str
    ) -> None:
        """Join an Applied activation to one post-claim Idle scene preparation."""

        if self.headless_client is None or self.authoring_admission is None:
            raise AuthoringJournalError(
                "program_scene_preparation_unavailable",
                "Applied Program scene preparation requires Headless",
            )
        applied = self.headless_client.program_applied()
        source = applied.get("source")
        record_digest = (
            source.get("record_digest") if isinstance(source, dict) else None
        )
        if not isinstance(record_digest, str):
            raise AuthoringJournalError(
                "program_applied_selection_invalid",
                "Applied Program omitted its exact record digest",
            )
        admission = self.headless_client.authoring_admission()
        generation = admission.get("admission_generation")
        if admission.get("authoring_allowed") is not True or type(generation) is not int:
            raise AuthoringJournalError(
                "program_scene_preparation_unavailable",
                "Applied Program scene preparation requires authoritative Idle",
            )
        preparation_request_id = (
            "program-scene-"
            + canonical_digest(
                {
                    "request_owner_epoch": request_owner_epoch,
                    "request_id": request_id,
                    "applied_record_digest": record_digest,
                }
            ).removeprefix("sha256:")[:32]
        )
        result = self.headless_client.program_scene_prepare(
            {
                "request_id": preparation_request_id,
                "request_owner_epoch": self.authoring_admission.owner_epoch,
                "source_label": "manyforge_composer_activation",
                "correlation_id": request_id,
                "expected_applied_record_digest": record_digest,
                "expected_admission_generation": generation,
            }
        )
        if (
            result.get("status") != "prepared"
            or result.get("applied_record_digest") != record_digest
            or result.get("program_revision") != applied.get("program_revision")
        ):
            raise AuthoringJournalError(
                "program_scene_preparation_invalid",
                "scene preparation returned another Applied Program identity",
            )

    @staticmethod
    def _manager_matches_scenario(
        status: dict[str, Any],
        diff: dict[str, Any],
        selection: dict[str, Any],
    ) -> bool:
        source = status.get("source")
        payload = status.get("status")
        active = payload.get("active") if isinstance(payload, dict) else None
        runtime = payload.get("runtime") if isinstance(payload, dict) else None
        diff_source = diff.get("source")
        return bool(
            isinstance(source, dict)
            and source.get("available") is True
            and source.get("freshness") == "fresh"
            and source.get("known_gaps") == []
            and isinstance(diff_source, dict)
            and diff_source.get("available") is True
            and diff_source.get("freshness") == "fresh"
            and diff_source.get("known_gaps") == []
            and isinstance(payload, dict)
            and payload.get("manager_ready") is True
            and isinstance(runtime, dict)
            and runtime.get("converged") is True
            and diff.get("converged") is True
            and isinstance(active, dict)
            and active.get("retained_bundle_id")
            == selection["retained_bundle_id"]
            and active.get("graph_revision") == selection["graph_revision"]
        )

    @staticmethod
    def _manager_current(
        status: dict[str, Any],
    ) -> tuple[str, str, str]:
        source = status.get("source")
        payload = status.get("status")
        active = payload.get("active") if isinstance(payload, dict) else None
        if (
            not isinstance(source, dict)
            or source.get("available") is not True
            or source.get("freshness") != "fresh"
            or source.get("known_gaps") != []
            or not isinstance(source.get("source_owner_epoch"), str)
            or not source["source_owner_epoch"]
            or not isinstance(payload, dict)
            or payload.get("manager_ready") is not True
            or not isinstance(active, dict)
            or not isinstance(active.get("retained_bundle_id"), str)
            or not isinstance(active.get("graph_revision"), str)
        ):
            raise DraftSessionError(
                "scenario_load_manager_unavailable",
                "Load Scenario requires one fresh Manager projection",
                status_code=503,
            )
        return (
            active["retained_bundle_id"],
            active["graph_revision"],
            source["source_owner_epoch"],
        )

    def _optional_applied(self) -> dict[str, Any] | None:
        assert self.headless_client is not None
        try:
            return self.headless_client.program_applied()
        except HeadlessClientError as error:
            if error.code == "program_applied_selection_not_found":
                return None
            raise

    def current_scenario(self) -> dict[str, Any]:
        """Derive current Scenario from owner projections; retain no copy."""

        if self.headless_client is None:
            return {
                "schema_version": "manyforge.current_scenario.v1",
                "ready": False,
                "current_scenario": None,
                "unavailable_reasons": ["headless_unavailable"],
            }
        reasons: list[str] = []
        applied = self._optional_applied()
        selection: dict[str, Any] | None = None
        if applied is None:
            reasons.append("program_not_applied")
        else:
            try:
                selection = self.headless_client.scenario_program_selection(
                    applied["scenario_id"]
                )
            except HeadlessClientError:
                reasons.append("scenario_selection_unavailable")
            else:
                if not applied_program_belongs_to_scenario(
                    applied, selection
                ):
                    reasons.append("applied_program_selection_mismatch")
        if selection is not None:
            if not self._manager_matches_scenario(
                self.headless_client.manager_status(),
                self.headless_client.manager_diff(),
                selection,
            ):
                reasons.append("deployment_not_active_and_converged")
        current = None
        if selection is not None and applied is not None and not reasons:
            current = {field: selection[field] for field in _CURRENT_SCENARIO_FIELDS}
            for field in ("program_id", "program_revision", "program_binding_id"):
                current[field] = applied[field]
        return {
            "schema_version": "manyforge.current_scenario.v1",
            "ready": current is not None,
            "current_scenario": current,
            "unavailable_reasons": reasons,
        }

    def _scenario_post_state(
        self,
        selection: dict[str, Any],
        pre_state: dict[str, Any],
        program: Program,
    ) -> dict[str, Any]:
        document = deepcopy(selection["program"])
        if pre_state["program"] == document:
            post_state = deepcopy(pre_state)
            if (
                post_state["saved_document"] == document
                and post_state["saved_path"] is not None
            ):
                return post_state
        else:
            post_state = {
                "schema_version": pre_state["schema_version"],
                "draft_id": (
                    "draft-scenario-"
                    + canonical_digest(
                        {
                            "scenario_id": selection["scenario_id"],
                            "program_revision": selection["program_revision"],
                        }
                    ).removeprefix("sha256:")[:24]
                ),
                "draft_revision": 1,
                "authoring": from_program(program),
                "program": document,
                "diagnostics": [],
                "saved_path": None,
                "saved_program_revision": None,
                "saved_document": None,
            }
        for root in sorted(Path(path).resolve() for path in self.allowed_fs_roots):
            for suffix in (".program.yaml", ".program.yml", ".program.json"):
                candidate = root / f"{selection['program_id']}{suffix}"
                if not candidate.is_file() or candidate.is_symlink():
                    continue
                try:
                    program, source_path = load_program_artifact(
                        str(candidate),
                        allowed_roots=self.allowed_fs_roots,
                        catalog=self.node_catalog,
                    )
                except ProgramArtifactError:
                    continue
                if program.to_dict() == document:
                    post_state.update(
                        {
                            "saved_path": source_path,
                            "saved_program_revision": program.program_revision,
                            "saved_document": document,
                        }
                    )
                    return post_state
        return post_state

    def prepare_scenario_load(
        self,
        *,
        scenario_id: str,
        expected_catalog_revision: str | None,
        confirm_discard: bool,
        request_id: str,
    ) -> dict[str, Any]:
        """Preflight one exact Scenario before any owner effect is recorded."""

        if self.headless_client is None or self.authoring_admission is None:
            raise DraftSessionError(
                "scenario_load_unavailable",
                "Load Scenario requires Headless-backed authoring",
                status_code=503,
            )
        selection = self.headless_client.scenario_load_selection(scenario_id)
        if (
            expected_catalog_revision is not None
            and selection["scenario_catalog_revision"]
            != expected_catalog_revision
        ):
            raise DraftSessionError(
                "scenario_catalog_revision_stale",
                "the Scenario catalog changed before Load began",
                status_code=409,
            )
        try:
            program = Program.from_dict(
                deepcopy(selection["program"]),
                self.node_catalog,
                path="<scenario-load>",
            )
        except ProgramValidationError as error:
            raise DraftSessionError(
                "scenario_load_program_invalid",
                f"the selected Scenario Program is invalid: {error}",
                status_code=409,
            ) from error
        applicability = draft_applicability(program.to_dict())
        if applicability["applyReady"] is not True:
            reason = applicability["unavailableReason"]
            raise DraftSessionError(
                reason["code"], reason["message"], status_code=409
            )
        pre_state = self.draft_session.retained_state()
        applied = self._optional_applied()
        replacing = pre_state["program"] != selection["program"]
        current_matches_saved = (
            pre_state["program"] is not None
            and pre_state["program"] == pre_state["saved_document"]
        )
        current_matches_applied = (
            applied is not None
            and pre_state["program"] == applied.get("program")
        )
        if (
            replacing
            and pre_state["program"] is not None
            and (not current_matches_saved or not current_matches_applied)
            and not confirm_discard
        ):
            raise DraftSessionError(
                "composer_draft_discard_confirmation_required",
                "loading this Scenario would discard unsaved or unapplied Draft changes",
                status_code=409,
            )
        manager_status = self.headless_client.manager_status()
        manager_diff = self.headless_client.manager_diff()
        current_bundle, current_graph, manager_owner_epoch = self._manager_current(
            manager_status
        )
        manager_matches = self._manager_matches_scenario(
            manager_status, manager_diff, selection
        )
        expected_digest: str | None = None
        if applied is not None:
            source = applied.get("source")
            if isinstance(source, dict) and isinstance(
                source.get("record_digest"), str
            ):
                expected_digest = source["record_digest"]
        manager_request_id = (
            "scenario-load-manager-"
            + canonical_digest(
                {
                    "request_id": request_id,
                    "scenario_id": scenario_id,
                    "catalog": selection["scenario_catalog_revision"],
                }
            ).removeprefix("sha256:")[:32]
        )
        return {
            "schema_version": "manyforge.composer_authoring_effect.v1",
            "operation_kind": "load_scenario",
            "effect_class": "external",
            "external_kind": "scenario_load",
            "selection": deepcopy(selection),
            "expected_applied_record_digest": expected_digest,
            "manager_apply": (
                None
                if manager_matches
                else {
                    "request_id": manager_request_id,
                    "request_owner_epoch": manager_owner_epoch,
                    "source_label": "manyforge_composer_scenario_load",
                    "correlation_id": request_id,
                    "deployment_artifact_id": selection[
                        "deployment_artifact_id"
                    ],
                    "deployment_revision": selection["deployment_revision"],
                    "expected_current": {
                        "graph_revision": current_graph,
                        "retained_bundle_id": current_bundle,
                    },
                    "reject_if_affected_busy": True,
                }
            ),
            "post_state": self._scenario_post_state(selection, pre_state, program),
        }

    def _execute_scenario_load(
        self, intent: dict[str, Any], claim: dict[str, Any]
    ) -> dict[str, Any]:
        assert self.headless_client is not None
        selection = intent["selection"]
        manager_receipt: dict[str, Any] | None = None
        status = self.headless_client.manager_status()
        diff = self.headless_client.manager_diff()
        if not self._manager_matches_scenario(status, diff, selection):
            manager_request = intent["manager_apply"]
            if not isinstance(manager_request, dict):
                raise AuthoringJournalError(
                    "scenario_load_manager_state_changed",
                    "the target Deployment stopped matching after preflight",
                )
            manager_receipt = self.headless_client.manager_apply(
                {**deepcopy(manager_request), "parent_authoring_claim": claim}
            )
            if manager_receipt.get("ok") is not True:
                raise AuthoringJournalError(
                    "scenario_load_manager_apply_rejected",
                    "Manager rejected the target Deployment",
                )
        deadline = time.monotonic() + _SCENARIO_LOAD_TIMEOUT_SECONDS
        while time.monotonic() < deadline:
            status = self.headless_client.manager_status()
            diff = self.headless_client.manager_diff()
            if self._manager_matches_scenario(status, diff, selection):
                break
            time.sleep(_SCENARIO_LOAD_POLL_SECONDS)
        else:
            raise AuthoringJournalError(
                "scenario_load_manager_convergence_timeout",
                "Manager did not publish the target converged Deployment in time",
            )
        applied = self._optional_applied()
        program_result: dict[str, Any] | None = None
        if applied is None or not applied_program_matches_scenario_selection(
            applied, selection
        ):
            program_result = self.headless_client.program_apply(
                {
                    "scenario_id": selection["scenario_id"],
                    "scenario_catalog_revision": selection[
                        "scenario_catalog_revision"
                    ],
                    "program": deepcopy(selection["program"]),
                    "expected_applied_record_digest": intent[
                        "expected_applied_record_digest"
                    ],
                    "authoring_claim": claim,
                }
            )
            validate_program_apply_result(program_result)
        applied = self.headless_client.program_applied()
        if not applied_program_matches_scenario_selection(applied, selection):
            raise AuthoringJournalError(
                "scenario_load_applied_mismatch",
                "Program Apply did not establish the selected Scenario",
            )
        if not self._manager_matches_scenario(
            self.headless_client.manager_status(),
            self.headless_client.manager_diff(),
            selection,
        ):
            raise AuthoringJournalError(
                "scenario_load_deployment_mismatch",
                "the target Deployment was no longer active after Program Apply",
            )
        self._restore_authoring_post_image(deepcopy(intent["post_state"]))
        return {
            "status": "loaded",
            "headless": {
                "scenario": {
                    field: selection[field] for field in _CURRENT_SCENARIO_FIELDS
                },
                "managerApply": manager_receipt,
                "programApply": program_result,
            },
            "workspace": self._recovery_workspace(
                applied_program_revision=selection["program_revision"]
            ),
        }

    def load_scenario(
        self,
        envelope: AuthoringMutationEnvelope,
        *,
        scenario_id: str,
        expected_catalog_revision: str | None,
        confirm_discard: bool,
    ) -> dict[str, Any]:
        """Run one durable, idempotent Deployment+Program Scenario load."""

        if self.authoring_admission is None:
            raise DraftSessionError(
                "scenario_load_unavailable",
                "Load Scenario requires Headless-backed authoring",
                status_code=503,
            )
        retained = self.draft_session.retained_state()
        prepared: dict[str, Any] = {}

        def prepare() -> dict[str, Any]:
            prepared.update(
                self.prepare_scenario_load(
                    scenario_id=scenario_id,
                    expected_catalog_revision=expected_catalog_revision,
                    confirm_discard=confirm_discard,
                    request_id=envelope.request_id,
                )
            )
            return prepared

        return self.authoring_admission.execute(
            envelope,
            operation_kind="load_scenario",
            draft_id=retained["draft_id"],
            draft_revision=retained["draft_revision"],
            payload={
                "scenario_id": scenario_id,
                "expected_catalog_revision": expected_catalog_revision,
                "confirm_discard": confirm_discard,
            },
            prepare_effect=prepare,
            effect=lambda claim: self._execute_scenario_load(prepared, claim),
            post_image=self.draft_session.retained_state,
            after_release=lambda: self.prepare_current_applied_scene(
                request_owner_epoch=envelope.request_owner_epoch,
                request_id=envelope.request_id,
            ),
        )

    def load_startup_scenario(self, scenario_id: str) -> dict[str, Any]:
        """Backend entry used by the explicit development launcher request."""

        if self.headless_client is None or self.authoring_admission is None:
            raise AuthoringJournalError(
                "scenario_load_unavailable",
                "startup Load Scenario requires Headless-backed authoring",
            )
        selection = self.headless_client.scenario_load_selection(scenario_id)
        admission = self.headless_client.authoring_admission()
        generation = admission.get("admission_generation")
        if admission.get("authoring_allowed") is not True or type(generation) is not int:
            raise AuthoringJournalError(
                "scenario_load_authoring_not_ready",
                "startup Load Scenario requires idle authoring admission",
            )
        request_id = "development-load-scenario-" + uuid.uuid4().hex
        return self.load_scenario(
            AuthoringMutationEnvelope(
                expected_admission_generation=generation,
                request_owner_epoch=self.authoring_admission.owner_epoch,
                request_id=request_id,
            ),
            scenario_id=scenario_id,
            expected_catalog_revision=selection["scenario_catalog_revision"],
            confirm_discard=False,
        )

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
        if kind in {"program_apply", "revert_program"}:
            result = self._recover_program_apply(intent, claim)
            applied = result.get("applied")
            applied_revision = (
                applied.get("program_revision")
                if isinstance(applied, dict)
                else None
            )
            self._restore_authoring_post_image(post_state)
            workspace = self._recovery_workspace(
                applied_program_revision=applied_revision
            )
            if kind == "program_apply":
                response = JSONResponse(
                    status_code=200,
                    content={
                        "status": "applied",
                        "headless": result,
                        "workspace": workspace,
                    },
                )
            else:
                response = JSONResponse(
                    status_code=200,
                    content={
                        "status": "reverted",
                        "headless": result,
                        "workspace": workspace,
                    },
                )
            return response, post_state
        if kind == "scenario_load":
            result = self._execute_scenario_load(intent, claim)
            return result, post_state
        raise AuthoringJournalError(
            "composer_authoring_recovery_invalid",
            f"unsupported external authoring recovery kind: {kind}",
        )
