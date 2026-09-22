"""Structural Composer state plus an optional replaceable headless client.

The former Composer runtime state machine, scene cache, node discovery owner,
Runner/CycleManager construction, and robot-control authorities were removed
with the legacy scene owner. This state contains only immutable built-in
catalog identity, an optional authored Program in one in-memory Draft, and
optional HTTP clients that own no runtime truth.
"""

from __future__ import annotations

from copy import deepcopy
import logging
import hashlib
from pathlib import Path
import os
import time
import uuid
from typing import Any, TYPE_CHECKING

from fastapi.responses import JSONResponse
from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment
from manyforge_behavior.canonical_artifact import canonical_json_bytes
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
from .deployment_draft import (
    DeploymentDraftError,
    DeploymentDraftSession,
    compile_deployment_document,
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
from .deployment_artifacts import (
    load_deployment_artifact,
    save_deployment_artifact,
)
from .scenario_artifacts import (
    ScenarioArtifactError,
    load_scenario_artifact,
    save_scenario_artifact,
)
from .execution_observation_stream import ExecutionObservationSource

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
_PREPARED_ACTIVATION_FIELDS = (
    "prepared_activation_id",
    "source_document_digest",
    "captured_input_digest",
    "deployment_artifact_id",
    "deployment_revision",
    "retained_bundle_id",
    "graph_revision",
    "program_id",
    "program_revision",
    "program_binding_id",
    "node_environment_digest",
    "assistant_mode",
    "typed_parameter_overrides",
)
_SCENARIO_LOAD_TIMEOUT_SECONDS = 20.0
_SCENARIO_LOAD_POLL_SECONDS = 0.1
_LOGGER = logging.getLogger(__name__)


class ComposerState:
    """Authoring state and a replaceable client, never runtime authority."""

    # Absolute monotonic budget for every post-Apply observation in one
    # activation. One deadline is computed per activation and shared by all
    # its phases; a test or caller may inject a shorter budget on the instance.
    deployment_observation_timeout_seconds: float = _SCENARIO_LOAD_TIMEOUT_SECONDS

    def __init__(
        self,
        *,
        node_environment: NodeEnvironment | None = None,
        program: Program | None = None,
        program_source_path: str | None = None,
        allowed_fs_roots: list[str] | None = None,
        allowed_program_write_roots: list[str] | None = None,
        allowed_deployment_write_roots: list[str] | None = None,
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
        self.deployment_draft_session = DeploymentDraftSession()
        self.allowed_fs_roots = list(allowed_fs_roots or [])
        self.allowed_program_write_roots = (
            None
            if allowed_program_write_roots is None
            else list(allowed_program_write_roots)
        )
        self.allowed_deployment_write_roots = (
            None
            if allowed_deployment_write_roots is None
            else list(allowed_deployment_write_roots)
        )
        self.headless_client = headless_client
        self.execution_observation_source = (
            ExecutionObservationSource(headless_client)
            if headless_client is not None
            else None
        )
        authoring_state_root = authoring_state_root or os.environ.get(
            "MANYFORGE_COMPOSER_AUTHORING_STATE_ROOT"
        )
        if headless_client is not None and authoring_state_root is None:
            raise ValueError(
                "authoring_state_root is required with Headless-backed authoring"
            )
        # Editable-document presentation state only. Runtime Scenario truth
        # remains the exact Headless/Manager/Applied projection. Initialize it
        # before journal recovery so a recovered Save may restore its path.
        self.scenario_document_path: str | None = None
        self.scenario_document_id: str | None = None
        self.scenario_document_name: str | None = None
        self.scenario_document_description: str | None = None
        self.scenario_source_diagnostics: list[dict[str, str]] = []
        self.scenario_source_document_digest: str | None = None
        self.scenario_program_role_assignment: dict[str, Any] | None = None
        self.scenario_typed_parameter_overrides: dict[str, Any] = {}
        self.scenario_source_program_digest: str | None = None
        self.scenario_source_deployment_digest: str | None = None
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
        allowed_deployment_write_roots: list[str] | None = None,
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
            allowed_deployment_write_roots=allowed_deployment_write_roots,
            headless_client=headless_client,
            authoring_state_root=authoring_state_root,
            assistant_client=assistant_client,
        )

    @property
    def node_catalog(self):
        return self.node_environment.catalog

    def remember_scenario_document(
        self,
        document: dict[str, Any] | None,
        *,
        source_document_digest: str | None = None,
        source_diagnostics: list[dict[str, str]] | None = None,
    ) -> None:
        if document is None:
            self.scenario_document_id = None
            self.scenario_document_name = None
            self.scenario_document_description = None
            self.scenario_source_diagnostics = []
            self.scenario_source_document_digest = None
            self.scenario_program_role_assignment = None
            self.scenario_typed_parameter_overrides = {}
            self.scenario_source_program_digest = None
            self.scenario_source_deployment_digest = None
            return
        self.scenario_document_id = document.get("scenario_id")
        self.scenario_document_name = document.get("name")
        self.scenario_document_description = document.get("description")
        self.scenario_source_diagnostics = deepcopy(source_diagnostics or [])
        self.scenario_source_document_digest = source_document_digest
        self.scenario_program_role_assignment = deepcopy(
            document.get("program_role_assignment")
        )
        self.scenario_typed_parameter_overrides = deepcopy(
            document.get("typed_parameter_overrides", {})
        )
        self.scenario_source_program_digest = canonical_digest(
            {"value": document.get("program_draft")}
        )
        self.scenario_source_deployment_digest = canonical_digest(
            {"value": document.get("deployment_draft")}
        )

    def current_drafts_match_remembered_scenario(self) -> bool:
        if (
            self.scenario_source_program_digest is None
            or self.scenario_source_deployment_digest is None
        ):
            return False
        return (
            canonical_digest(
                {"value": self.draft_session.retained_state().get("authoring")}
            )
            == self.scenario_source_program_digest
            and canonical_digest({
                "value": self.deployment_draft_session.retained_state().get(
                    "document"
                )
            })
            == self.scenario_source_deployment_digest
        )

    def _active_authoring_catalog(self):
        return (
            read_active_node_environment(self).catalog
            if self.headless_client is not None
            else self.node_catalog
        )

    def _restore_authoring_post_image(
        self,
        value: dict[str, Any],
        *,
        catalog: Any | None = None,
    ) -> None:
        authoring_catalog = (
            self._active_authoring_catalog() if catalog is None else catalog
        )
        if value.get("schema_version") == "manyforge.composer_authoring_workspace.v2":
            if set(value) != {"schema_version", "program", "deployment"}:
                raise AuthoringJournalError(
                    "composer_authoring_recovery_invalid",
                    "retained Composer workspace has an invalid shape",
                )
            self.draft_session.restore_retained_state(
                value["program"], authoring_catalog
            )
            self.deployment_draft_session.restore_retained_state(value["deployment"])
            return
        self.draft_session.restore_retained_state(value, authoring_catalog)

    def authoring_retained_state(self) -> dict[str, Any]:
        """Return one durable post-image for the separate authored documents."""

        if not self.deployment_draft_session.loaded:
            return self.draft_session.retained_state()
        return {
            "schema_version": "manyforge.composer_authoring_workspace.v2",
            "program": self.draft_session.retained_state(),
            "deployment": self.deployment_draft_session.retained_state(),
        }

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
        started = time.monotonic()
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
        if (
            admission.get("authoring_allowed") is not True
            or type(generation) is not int
        ):
            raise AuthoringJournalError(
                "program_scene_preparation_unavailable",
                "Program is applied, but its scene is not prepared: backend "
                "authoring readiness is not established",
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
        _LOGGER.info(
            "deployment_scene_prepared",
            extra={
                "request_owner_epoch": request_owner_epoch,
                "request_id": request_id,
                "applied_record_digest": record_digest,
                "scene_prep_ms": (time.monotonic() - started) * 1000.0,
            },
        )

    @staticmethod
    def _manager_published_matches_scenario(
        status: dict[str, Any],
        selection: dict[str, Any],
    ) -> bool:
        """Return whether Manager has published the selected projection.

        Publication and runtime convergence are deliberately different
        facts.  A terminal Manager Apply makes the candidate the authoritative
        active projection before every runtime component has caught up.  The
        authoring workspace needs that identity immediately so Apply,
        Discard, and Revert buttons do not oscillate while the runtime settles.
        Execution-facing callers continue to use ``_manager_matches_scenario``
        below, which also requires convergence.
        """

        source = status.get("source")
        payload = status.get("status")
        active = payload.get("active") if isinstance(payload, dict) else None
        return bool(
            isinstance(source, dict)
            and source.get("available") is True
            and source.get("freshness") == "fresh"
            and source.get("known_gaps") == []
            and isinstance(active, dict)
            and active.get("retained_bundle_id")
            == selection.get("retained_bundle_id")
            and active.get("graph_revision") == selection.get("graph_revision")
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

    def _optional_applied(
        self, *, allow_inactive: bool = False
    ) -> dict[str, Any] | None:
        assert self.headless_client is not None
        # Lightweight authoring doubles may expose only the projections
        # needed by a local Draft test.  Absence of the optional Applied
        # Program read means there is no published Scenario identity; it is
        # not an authoring exception.
        reader = getattr(self.headless_client, "program_applied", None)
        if not callable(reader):
            return None
        try:
            return reader()
        except HeadlessClientError as error:
            if error.code == "program_applied_selection_not_found":
                return None
            if allow_inactive and error.code == "program_applied_selection_not_active":
                # An admitted Deployment handover retains the old record digest
                # for Program Apply's compare-and-set, but that old binding is
                # no longer a current executable Program.
                return None
            raise

    def current_scenario(self) -> dict[str, Any]:
        """Derive the execution-ready Scenario from owner projections."""

        published = self.published_scenario()
        if self.headless_client is None:
            return {
                "schema_version": "manyforge.current_scenario.v1",
                "ready": False,
                "current_scenario": None,
                "unavailable_reasons": ["headless_unavailable"],
            }
        current = published.get("published_scenario")
        reasons = [
            "deployment_not_active_and_converged"
            if reason == "deployment_not_published"
            else reason
            for reason in published.get("unavailable_reasons", [])
        ]
        if current is not None:
            try:
                selection = self.headless_client.scenario_program_selection(
                    current["scenario_id"]
                )
                if not self._manager_matches_scenario(
                    self.headless_client.manager_status(),
                    self.headless_client.manager_diff(),
                    selection,
                ):
                    reasons.append("deployment_not_active_and_converged")
            except HeadlessClientError:
                reasons.append("scenario_selection_unavailable")
        execution_current = current if current is not None and not reasons else None
        return {
            "schema_version": "manyforge.current_scenario.v1",
            "ready": execution_current is not None,
            "current_scenario": execution_current,
            "unavailable_reasons": reasons,
        }

    def published_scenario(self) -> dict[str, Any]:
        """Derive the authoritative Manager publication without convergence.

        This is the read model used by Deployment authoring.  It is not an
        execution admission signal: the native/runtime owners may still be
        reconciling the newly published projection.  Keeping this identity
        separate from ``current_scenario`` makes the Apply/Revert/Discard
        cycle deterministic and leaves runtime readiness to the component
        status/Program execution paths.
        """

        if self.headless_client is None:
            return {
                "schema_version": "manyforge.published_scenario.v1",
                "ready": False,
                "published_scenario": None,
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
        if selection is not None and applied is not None:
            status = self.headless_client.manager_status()
            if not self._manager_published_matches_scenario(status, selection):
                reasons.append("deployment_not_published")
        current = None
        if selection is not None and applied is not None and not reasons:
            current = {field: selection[field] for field in _CURRENT_SCENARIO_FIELDS}
            for field in ("program_id", "program_revision", "program_binding_id"):
                current[field] = applied[field]
        return {
            "schema_version": "manyforge.published_scenario.v1",
            "ready": current is not None,
            "published_scenario": current,
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
        if not (
            post_state["saved_document"] == document
            and post_state["saved_path"] is not None
        ):
            for root in sorted(
                Path(path).resolve() for path in self.allowed_fs_roots
            ):
                for suffix in (".program.yaml", ".program.yml", ".program.json"):
                    candidate = root / f"{selection['program_id']}{suffix}"
                    if not candidate.is_file() or candidate.is_symlink():
                        continue
                    try:
                        saved_program, source_path = load_program_artifact(
                            str(candidate),
                            allowed_roots=self.allowed_fs_roots,
                            catalog=self.node_catalog,
                        )
                    except ProgramArtifactError:
                        continue
                    if saved_program.to_dict() == document:
                        post_state.update(
                            {
                                "saved_path": source_path,
                                "saved_program_revision": (
                                    saved_program.program_revision
                                ),
                                "saved_document": document,
                            }
                        )
                        break
                else:
                    continue
                break
        program_post_state = post_state

        deployment_document = deepcopy(selection["deployment"])
        # Prefer the maintained Deployment artifact as the saved baseline.
        # The active Scenario selection can point at a Manager-produced
        # candidate after an Apply, while the artifact on disk remains the
        # operator's reversible source.  If no matching artifact is present,
        # the exact selected Scenario document is the baseline.
        deployment_saved_baseline = deployment_document
        deployment_saved_path: str | None = None
        deployment_artifact_id = selection.get("deployment_artifact_id")
        if isinstance(deployment_artifact_id, str) and deployment_artifact_id:
            for root in sorted(
                Path(path).resolve() for path in self.allowed_fs_roots
            ):
                for suffix in (".deployment.yaml", ".deployment.yml"):
                    candidate = root / f"{deployment_artifact_id}{suffix}"
                    if not candidate.is_file() or candidate.is_symlink():
                        continue
                    try:
                        source, source_path = load_deployment_artifact(
                            str(candidate), allowed_roots=self.allowed_fs_roots
                        )
                        compiled_baseline = (
                            self.deployment_draft_session.preview_source(source)
                        )
                    except DeploymentDraftError:
                        continue
                    if (
                        compiled_baseline.deployment_artifact_id
                        == deployment_artifact_id
                    ):
                        deployment_saved_baseline = deepcopy(
                            compiled_baseline.document
                        )
                        deployment_saved_path = source_path
                        break
                else:
                    continue
                break
        deployment_current = self.deployment_draft_session.retained_state()
        if deployment_current["document"] == deployment_document:
            deployment_post_state = deepcopy(deployment_current)
            # A Scenario's Deployment is the durable baseline.  When the
            # maintained artifact is available, retain its real path as part
            # of the Saved identity so the client can offer Revert.
            if deployment_post_state["saved_path"] is None and (
                deployment_post_state["saved_document"]
                != deployment_saved_baseline
                or deployment_saved_path is not None
            ):
                deployment_post_state["saved_document"] = deepcopy(
                    deployment_saved_baseline
                )
                deployment_post_state["saved_path"] = deployment_saved_path
        else:
            deployment_post_state = {
                "schema_version": deployment_current["schema_version"],
                "draft_id": (
                    "deployment-draft-scenario-"
                    + canonical_digest(
                        {
                            "scenario_id": selection["scenario_id"],
                            "deployment_revision": selection[
                                "deployment_revision"
                            ],
                        }
                    ).removeprefix("sha256:")[:24]
                ),
                "draft_revision": 1,
                "document": deployment_document,
                "saved_path": deployment_saved_path,
                # Scenario selection (or its maintained artifact) is the
                # saved baseline for Deployment authoring.  Apply changes the
                # runtime, while Revert returns to that exact baseline.
                "saved_document": deepcopy(deployment_saved_baseline),
            }
        return {
            "schema_version": "manyforge.composer_authoring_workspace.v2",
            "program": program_post_state,
            "deployment": deployment_post_state,
        }

    def prepare_scenario_load(
        self,
        *,
        scenario_id: str,
        expected_catalog_revision: str | None,
        confirm_discard: bool,
        request_id: str,
        expected_admission_generation: int,
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
        deployment_pre_state = self.deployment_draft_session.retained_state()
        applied = self._optional_applied()
        replacing = pre_state["program"] != selection["program"]
        replacing_deployment = (
            deployment_pre_state["document"] != selection["deployment"]
        )
        current_matches_saved = (
            pre_state["program"] is not None
            and pre_state["program"] == pre_state["saved_document"]
        )
        current_matches_applied = applied is not None and pre_state[
            "program"
        ] == applied.get("program")
        discards_program = (
            replacing
            and pre_state["program"] is not None
            and (not current_matches_saved or not current_matches_applied)
        )
        discards_deployment = (
            replacing_deployment
            and deployment_pre_state["document"] is not None
            and deployment_pre_state["document"]
            != deployment_pre_state["saved_document"]
        )
        if (discards_program or discards_deployment) and not confirm_discard:
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
        candidate_receipt: dict[str, Any] | None = None
        if not manager_matches:
            preview = self.headless_client.manager_deployment_candidate_preview(
                {
                    "deployment_artifact_id": selection[
                        "deployment_artifact_id"
                    ],
                    "deployment_revision": selection["deployment_revision"],
                    "expected_current": {
                        "graph_revision": current_graph,
                        "retained_bundle_id": current_bundle,
                    },
                    "expected_admission_generation": (
                        expected_admission_generation
                    ),
                    "request_owner_epoch": manager_owner_epoch,
                }
            )
            if preview.get("ok") is not True or not isinstance(
                preview.get("candidate_receipt"), dict
            ):
                raise DraftSessionError(
                    "scenario_load_deployment_candidate_rejected",
                    "Manager rejected the selected Scenario Deployment",
                    status_code=409,
                )
            candidate_receipt = deepcopy(preview["candidate_receipt"])
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
                    "candidate_receipt": candidate_receipt,
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
            manager_receipt = self.apply_manager_deployment_candidate(
                {"manager_apply": manager_request}, claim
            )
        while True:
            status = self.headless_client.manager_status()
            diff = self.headless_client.manager_diff()
            if self._manager_matches_scenario(status, diff, selection):
                break
            time.sleep(_SCENARIO_LOAD_POLL_SECONDS)
        applied = self._optional_applied(allow_inactive=True)
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
        # Preflight parsed this exact post-image with the immutable local
        # build-qualified catalog before the authoring claim. Program Apply
        # can refresh Runner availability asynchronously, so reparsing against
        # a second live join here would create a timing dependency inside the
        # already-admitted transaction.
        self._restore_authoring_post_image(
            deepcopy(intent["post_state"]),
            catalog=self.node_catalog,
        )
        if isinstance(intent.get("scenario_document_id"), str):
            self.scenario_document_path = intent.get("scenario_document_path")
            self.scenario_source_document_digest = intent.get(
                "scenario_source_document_digest"
            )
            self.scenario_program_role_assignment = deepcopy(
                intent.get("program_role_assignment")
            )
            self.scenario_typed_parameter_overrides = deepcopy(
                intent.get("typed_parameter_overrides", {})
            )
            self.scenario_source_program_digest = intent.get(
                "scenario_source_program_digest"
            )
            self.scenario_source_deployment_digest = intent.get(
                "scenario_source_deployment_digest"
            )
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

    def apply_manager_deployment_candidate(
        self,
        intent: dict[str, Any],
        claim: dict[str, Any],
        *,
        observation_deadline: float | None = None,
    ) -> dict[str, Any]:
        """Apply one retained Manager candidate and prove its terminal head.

        `observation_deadline` is the absolute monotonic instant that bounds
        every post-acceptance observation in a caller's sequence. A caller
        that owns one multi-phase activation passes its shared deadline so the
        status wait cannot restart the full budget for each phase; callers with
        no deadline keep the Manager's own terminal receipt as the only
        completion proof.
        """

        if self.headless_client is None:
            raise AuthoringJournalError(
                "manager_apply_unavailable",
                "Manager-backed Deployment Apply is unavailable",
            )
        request = intent.get("manager_apply")
        if not isinstance(request, dict):
            raise AuthoringJournalError(
                "manager_candidate_intent_invalid",
                "retained Deployment candidate intent is incomplete",
            )
        response = self.headless_client.manager_apply(
            {**deepcopy(request), "parent_authoring_claim": claim}
        )
        if response.get("ok") is not True:
            raise AuthoringJournalError(
                "manager_apply_rejected",
                "Manager rejected the exact Deployment candidate",
            )
        acceptance = deepcopy(response)

        def observation_timeout() -> AuthoringJournalError:
            return AuthoringJournalError(
                "manager_apply_observation_timeout",
                "Manager reported no terminal receipt for Apply "
                f"{acceptance.get('apply_id')} before the shared "
                "observation deadline",
            )

        while response.get("state") != "succeeded":
            remaining = (
                None
                if observation_deadline is None
                else observation_deadline - time.monotonic()
            )
            if remaining is not None and remaining <= 0:
                raise observation_timeout()
            time.sleep(
                _SCENARIO_LOAD_POLL_SECONDS
                if remaining is None
                else min(_SCENARIO_LOAD_POLL_SECONDS, remaining)
            )
            remaining = (
                None
                if observation_deadline is None
                else observation_deadline - time.monotonic()
            )
            if remaining is not None and remaining <= 0:
                raise observation_timeout()
            try:
                response = self.headless_client.manager_apply_status(
                    response["apply_id"],
                    response["apply_generation"],
                    timeout_seconds=remaining,
                )
            except HeadlessClientError as error:
                if error.code == "headless_http_timeout":
                    raise observation_timeout() from error
                raise AuthoringJournalError(
                    "manager_apply_observation_interrupted",
                    "Manager accepted Apply "
                    f"{acceptance.get('apply_id')}, but its terminal outcome "
                    "could not be observed; the effect remains unconfirmed",
                ) from error
            if (
                observation_deadline is not None
                and time.monotonic() >= observation_deadline
            ):
                raise observation_timeout()
            if response.get("ok") is not True:
                raise AuthoringJournalError(
                    "manager_apply_status_rejected",
                    "Manager could not observe the retained Apply identity",
                )
            if any(
                response.get(field) != acceptance.get(field)
                for field in ("apply_id", "apply_generation")
            ):
                raise AuthoringJournalError(
                    "manager_apply_status_identity_mismatch",
                    "Manager status does not name the accepted Apply",
                )
        # The Manager's own terminal Apply receipt is the single completion
        # proof of the published candidate; no second status or diff read may
        # substitute for it (UI6-I-AP AP2).
        receipt = request.get("candidate_receipt")
        projection = response.get("candidate_projection_revision")
        if (
            not isinstance(receipt, dict)
            or not isinstance(projection, dict)
            or projection.get("retained_bundle_id")
            != receipt.get("candidate_retained_bundle_id")
            or projection.get("graph_revision")
            != receipt.get("candidate_graph_revision")
        ):
            raise AuthoringJournalError(
                "manager_apply_projection_mismatch",
                "Manager terminal Apply did not publish the previewed candidate",
            )
        # Status supplies current terminal facts, not the original request's
        # acceptance/replay identity. Retain that receipt across polling.
        return {**acceptance, **response}

    @staticmethod
    def _deployment_candidate_is_disruptive(diff: Any) -> bool:
        return bool(
            isinstance(diff, dict)
            and any(
                isinstance(record_diff, dict)
                and any(
                    isinstance(record_diff.get(change), list)
                    and bool(record_diff[change])
                    for change in ("added", "changed", "removed")
                )
                for record_diff in diff.values()
            )
        )

    @staticmethod
    def _deployment_candidate_confirmation_token(
        *,
        draft_id: Any,
        draft_revision: Any,
        prepared_activation: dict[str, Any],
        preview: dict[str, Any],
    ) -> str:
        receipt = preview["candidate_receipt"]
        return canonical_digest(
            {
                "schema_version": (
                    "manyforge.deployment_apply_confirmation.v1"
                ),
                "draft_id": draft_id,
                "draft_revision": draft_revision,
                "candidate": {
                    field: prepared_activation[field]
                    for field in _PREPARED_ACTIVATION_FIELDS
                },
                "manager_current": receipt["expected_current"],
                "candidate_retained_bundle_id": receipt[
                    "candidate_retained_bundle_id"
                ],
                "candidate_graph_revision": receipt[
                    "candidate_graph_revision"
                ],
                "diff": preview.get("diff"),
            }
        )

    def validate_deployment_candidate_confirmation(
        self, intent: dict[str, Any]
    ) -> dict[str, Any]:
        """Check one retained preparation against this confirmation claim.

        This runs under the confirmation's fresh authoring claim and performs
        reads only: the durable preparation outcome, current Manager facts,
        and one fresh candidate preview. It never compiles or mutates an
        owner; the confirmation-time compile branch no longer exists.
        """

        started = time.monotonic()
        if self.headless_client is None or self.authoring_admission is None:
            raise AuthoringJournalError(
                "confirmation_expired",
                "Deployment confirmation requires retained authoring evidence",
            )
        confirmation_token = intent.get("confirmation_token")
        preparation_owner_epoch = intent.get("preparation_request_owner_epoch")
        preparation_request_id = intent.get("preparation_request_id")
        if (
            not isinstance(confirmation_token, str)
            or not isinstance(preparation_owner_epoch, str)
            or not isinstance(preparation_request_id, str)
        ):
            raise AuthoringJournalError(
                "confirmation_expired",
                "Deployment confirmation omitted its preparation identity",
            )
        transaction = self.authoring_admission.prepared_authoring_transaction(
            preparation_owner_epoch, preparation_request_id
        )
        outcome = transaction.outcome if transaction is not None else None
        preparation = (
            transaction.effect_intent if transaction is not None else None
        )
        body = outcome.get("body") if isinstance(outcome, dict) else None
        result = body.get("result") if isinstance(body, dict) else None
        prepared_activation = (
            result.get("prepared_activation") if isinstance(result, dict) else None
        )
        if (
            transaction is None
            or not transaction.complete
            or not isinstance(outcome, dict)
            or outcome.get("settlement_kind") != "owner_commit"
            or transaction.operation_kind != intent.get("operation_kind")
            or not isinstance(preparation, dict)
            or not isinstance(result, dict)
            or result.get("status") != "confirmation_required"
            or not isinstance(prepared_activation, dict)
            or any(
                field not in prepared_activation
                for field in _PREPARED_ACTIVATION_FIELDS
            )
            or not isinstance(result.get("candidate_preview"), dict)
            or not isinstance(
                result["candidate_preview"].get("candidate_receipt"), dict
            )
        ):
            raise AuthoringJournalError(
                "confirmation_expired",
                "the confirmed Deployment preparation is absent or incomplete",
            )
        candidate_receipt = result["candidate_preview"]["candidate_receipt"]
        if (
            prepared_activation["source_document_digest"]
            != preparation.get("source_document_digest")
            or prepared_activation["captured_input_digest"]
            != preparation.get("captured_input_digest")
        ):
            raise AuthoringJournalError(
                "confirmation_expired",
                "the confirmed preparation evidence is self-inconsistent",
            )
        changed = [
            label
            for label, retained, current in (
                (
                    "Deployment Draft identity",
                    (transaction.draft_id, transaction.draft_revision),
                    (intent["draft_id"], intent["draft_revision"]),
                ),
                ("source document", preparation.get("source_document_digest"),
                 intent["source_document_digest"]),
                ("captured input", preparation.get("captured_input_digest"),
                 intent["captured_input_digest"]),
                ("model source closure",
                 preparation.get("model_source_closure"),
                 intent["model_source_closure"]),
                (
                    "Applied Program",
                    preparation.get("program"),
                    intent["program"],
                ),
                (
                    "Deployment Draft document",
                    preparation.get("deployment"),
                    intent["deployment"],
                ),
                ("Program role assignment",
                 preparation.get("program_role_assignment"),
                 intent["program_role_assignment"]),
                ("typed parameter overrides",
                 preparation.get("typed_parameter_overrides"),
                 intent["typed_parameter_overrides"]),
            )
            if retained != current
        ]
        if changed:
            raise AuthoringJournalError(
                "confirmation_stale",
                "the confirmed preparation's "
                + " and ".join(changed)
                + " changed; prepare and confirm again",
            )
        status = self.headless_client.manager_status()
        current_bundle, current_graph, manager_epoch = self._manager_current(
            status
        )
        confirmed_candidate = {
            "retained_bundle_id": prepared_activation["retained_bundle_id"],
            "graph_revision": prepared_activation["graph_revision"],
        }
        if self._manager_matches_scenario(
            status, self.headless_client.manager_diff(), prepared_activation
        ):
            # The confirmed candidate is already active; the pre-apply
            # short-circuit observes it instead of restarting its runtime.
            return {
                "prepared_activation": prepared_activation,
                "preview": None,
                "receipt": None,
                "manager_epoch": manager_epoch,
                "validation_ms": (time.monotonic() - started) * 1000.0,
            }
        preview = self.headless_client.manager_deployment_candidate_preview(
            {
                "deployment_artifact_id": prepared_activation[
                    "deployment_artifact_id"
                ],
                "deployment_revision": prepared_activation["deployment_revision"],
                "expected_current": {
                    "graph_revision": current_graph,
                    "retained_bundle_id": current_bundle,
                },
                "expected_admission_generation": intent[
                    "expected_admission_generation"
                ],
                "request_owner_epoch": manager_epoch,
            }
        )
        receipt = preview.get("candidate_receipt")
        if preview.get("ok") is not True or not isinstance(receipt, dict):
            raise AuthoringJournalError(
                "deployment_candidate_rejected",
                "Manager rejected the retained Deployment candidate",
            )
        if (
            receipt.get("candidate_retained_bundle_id")
            != confirmed_candidate["retained_bundle_id"]
            or receipt.get("candidate_graph_revision")
            != confirmed_candidate["graph_revision"]
        ):
            raise AuthoringJournalError(
                "confirmation_stale",
                "the candidate identities changed since the confirmed "
                "preparation; prepare and confirm again",
            )
        if (
            self._deployment_candidate_confirmation_token(
                draft_id=intent["draft_id"],
                draft_revision=intent["draft_revision"],
                prepared_activation=prepared_activation,
                preview=preview,
            )
            != confirmation_token
        ):
            raise AuthoringJournalError(
                "confirmation_stale",
                "the candidate's semantic impact changed since the confirmed "
                "preparation; prepare and confirm again",
            )
        return {
            "prepared_activation": prepared_activation,
            "preview": preview,
            "receipt": receipt,
            "manager_epoch": manager_epoch,
            "validation_ms": (time.monotonic() - started) * 1000.0,
        }

    def retained_deployment_candidate_input(
        self, request_owner_epoch: str, request_id: str
    ) -> dict[str, Any]:
        """Return the exact captured input retained for one confirmation.

        Confirmation must validate current authored state against this value;
        it must not reopen mutable external model sources after the operator has
        reviewed the prepared candidate.
        """

        if self.authoring_admission is None:
            raise AuthoringJournalError(
                "confirmation_expired",
                "Deployment confirmation requires retained authoring evidence",
            )
        transaction = self.authoring_admission.prepared_authoring_transaction(
            request_owner_epoch, request_id
        )
        preparation = (
            transaction.effect_intent if transaction is not None else None
        )
        if (
            transaction is None
            or not transaction.complete
            or not isinstance(preparation, dict)
            or preparation.get("operation_kind")
            not in {"apply_deployment_candidate", "preview_deployment_candidate"}
            or not isinstance(preparation.get("captured_input_digest"), str)
            or not isinstance(preparation.get("model_source_closure"), dict)
        ):
            raise AuthoringJournalError(
                "confirmation_expired",
                "the confirmed Deployment input is absent or incomplete",
            )
        return deepcopy(preparation)

    def _prepare_authored_deployment_candidate(
        self,
        intent: dict[str, Any],
        claim: dict[str, Any],
        *,
        apply: bool,
    ) -> dict[str, Any]:
        """Prepare one captured input and its exact next Manager request."""

        assert self.headless_client is not None
        phases: dict[str, float] = {}
        confirmation = (
            self.validate_deployment_candidate_confirmation(intent)
            if intent.get("confirmation_token") is not None
            else None
        )
        if confirmation is not None:
            prepared_activation = confirmation["prepared_activation"]
            preview = confirmation["preview"]
            receipt = confirmation["receipt"]
            manager_epoch = confirmation["manager_epoch"]
            phases["confirmation_validation_ms"] = confirmation["validation_ms"]
        else:
            started = time.monotonic()
            response = self.headless_client.prepare_activation(
                {
                    "source_document_digest": intent["source_document_digest"],
                    "captured_input_digest": intent["captured_input_digest"],
                    "deployment": deepcopy(intent["deployment"]),
                    "model_source_closure": deepcopy(
                        intent["model_source_closure"]
                    ),
                    "program": deepcopy(intent["program"]),
                    "program_role_assignment": deepcopy(
                        intent["program_role_assignment"]
                    ),
                    "typed_parameter_overrides": deepcopy(
                        intent["typed_parameter_overrides"]
                    ),
                    "authoring_claim": claim,
                }
            )
            prepared_activation = response.get("prepared_activation")
            if (
                not isinstance(prepared_activation, dict)
                or any(
                    field not in prepared_activation
                    for field in _PREPARED_ACTIVATION_FIELDS
                )
            ):
                raise AuthoringJournalError(
                    "activation_preparer_response_invalid",
                    "Activation preparation omitted its exact result",
                )
            phases["preparation_ms"] = (time.monotonic() - started) * 1000.0
            started = time.monotonic()
            status = self.headless_client.manager_status()
            current_bundle, current_graph, manager_epoch = self._manager_current(status)
            preview = None
            receipt = None
            if not self._manager_matches_scenario(
                status, self.headless_client.manager_diff(), prepared_activation
            ):
                if intent["expected_manager_current"] != {
                    "retained_bundle_id": current_bundle,
                    "graph_revision": current_graph,
                }:
                    raise AuthoringJournalError(
                        "deployment_candidate_manager_state_changed",
                        "Manager selection changed before Deployment candidate preview",
                    )
                preview = self.headless_client.manager_deployment_candidate_preview(
                    {
                        "deployment_artifact_id": prepared_activation[
                            "deployment_artifact_id"
                        ],
                        "deployment_revision": prepared_activation[
                            "deployment_revision"
                        ],
                        "expected_current": {
                            "graph_revision": current_graph,
                            "retained_bundle_id": current_bundle,
                        },
                        "expected_admission_generation": intent[
                            "expected_admission_generation"
                        ],
                        "request_owner_epoch": manager_epoch,
                    }
                )
                receipt = preview.get("candidate_receipt")
                if preview.get("ok") is not True or not isinstance(receipt, dict):
                    raise AuthoringJournalError(
                        "deployment_candidate_rejected",
                        "Manager rejected the prepared Deployment candidate",
                    )
                if (
                    receipt.get("candidate_retained_bundle_id")
                    != prepared_activation["retained_bundle_id"]
                    or receipt.get("candidate_graph_revision")
                    != prepared_activation["graph_revision"]
                ):
                    raise AuthoringJournalError(
                        "deployment_candidate_preview_mismatch",
                        "Manager preview returned another prepared candidate",
                    )
            phases["preview_ms"] = (time.monotonic() - started) * 1000.0

        confirmation_required = bool(
            apply
            and intent.get("confirmation_token") is None
            and preview is not None
            and self._deployment_candidate_is_disruptive(preview.get("diff"))
        )
        confirmation_token = (
            self._deployment_candidate_confirmation_token(
                draft_id=intent["draft_id"],
                draft_revision=intent["draft_revision"],
                prepared_activation=prepared_activation,
                preview=preview,
            )
            if confirmation_required and preview is not None
            else None
        )
        manager_apply = None
        if apply and not confirmation_required and receipt is not None:
            manager_apply = {
                "request_id": intent["manager_request_id"],
                "request_owner_epoch": manager_epoch,
                "source_label": "manyforge_composer_deployment_apply",
                "correlation_id": intent["correlation_id"],
                "deployment_artifact_id": prepared_activation[
                    "deployment_artifact_id"
                ],
                "deployment_revision": prepared_activation["deployment_revision"],
                "expected_current": receipt["expected_current"],
                "reject_if_affected_busy": True,
                "candidate_receipt": receipt,
            }
        return {
            "prepared_activation": deepcopy(prepared_activation),
            "candidate_preview": deepcopy(preview),
            "manager_apply": deepcopy(manager_apply),
            "confirmation_required": confirmation_required,
            "confirmation_token": confirmation_token,
            "preparation": (
                {
                    "request_owner_epoch": intent.get(
                        "preparation_request_owner_epoch"
                    ),
                    "request_id": intent.get("preparation_request_id"),
                }
                if confirmation_required
                else None
            ),
            "phases_ms": phases,
        }

    def execute_authored_deployment_candidate(
        self,
        intent: dict[str, Any],
        claim: dict[str, Any],
        *,
        apply: bool,
        prepared: dict[str, Any] | None = None,
        prepare_only: bool = False,
    ) -> dict[str, Any]:
        """Prepare one candidate; UA3 owns the runtime transition."""

        if self.headless_client is None:
            raise AuthoringJournalError(
                "activation_preparer_unavailable",
                "Activation preparation requires Headless",
            )
        if prepared is None:
            retained = intent.get("prepared_deployment_candidate")
            if isinstance(retained, dict):
                prepared = deepcopy(retained)
        if prepared is None:
            prepared = self._prepare_authored_deployment_candidate(
                intent, claim, apply=apply
            )

        prepared_activation = prepared.get("prepared_activation")
        preview = prepared.get("candidate_preview")
        manager_apply = prepared.get("manager_apply")
        phases = prepared.get("phases_ms")
        confirmation_required = prepared.get("confirmation_required")
        confirmation_token = prepared.get("confirmation_token")
        preparation = prepared.get("preparation")
        if (
            not isinstance(prepared_activation, dict)
            or any(
                field not in prepared_activation
                for field in _PREPARED_ACTIVATION_FIELDS
            )
            or preview is not None and not isinstance(preview, dict)
            or manager_apply is not None and not isinstance(manager_apply, dict)
            or not isinstance(phases, dict)
            or not isinstance(confirmation_required, bool)
            or confirmation_required
            and (
                not isinstance(confirmation_token, str)
                or not isinstance(preparation, dict)
                or manager_apply is not None
            )
            or not confirmation_required
            and (confirmation_token is not None or preparation is not None)
        ):
            raise AuthoringJournalError(
                "activation_preparation_invalid",
                "retained activation preparation is incomplete",
            )
        if prepare_only:
            return deepcopy(prepared)
        if confirmation_required:
            return {
                "status": "confirmation_required",
                "confirmation_token": confirmation_token,
                "prepared_activation": prepared_activation,
                "candidate_preview": preview,
                "manager_apply": None,
                "selection": None,
                "preparation": preparation,
                "phases_ms": phases,
                "apply_id": None,
            }
        if not apply:
            return {
                "status": "previewed",
                "confirmation_token": None,
                "prepared_activation": prepared_activation,
                "candidate_preview": preview,
                "manager_apply": None,
                "selection": None,
                "preparation": None,
                "phases_ms": phases,
                "apply_id": None,
            }
        raise AuthoringJournalError(
            "activation_transition_unavailable",
            "The captured activation is prepared, but UA3 owns the single "
            "runtime transition and is not implemented yet",
        )

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
                    expected_admission_generation=(
                        envelope.expected_admission_generation
                    ),
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
            post_image=self.authoring_retained_state,
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
        while True:
            # Recovery is exact and idempotent. A replacement Manager may
            # still be completing the durable external effect when Composer
            # first starts, so retry that same recovery instead of polling a
            # permanently stale unavailable flag or imposing a host-speed
            # deadline.
            if not self.authoring_admission.mutation_ready:
                self.authoring_admission.recover()
            admission = self.headless_client.authoring_admission()
            generation = admission.get("admission_generation")
            if (
                self.authoring_admission.mutation_ready
                and admission.get("authoring_allowed") is True
                and type(generation) is int
            ):
                break
            time.sleep(_SCENARIO_LOAD_POLL_SECONDS)
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
        if kind == "save_scenario_document":
            exact_already_present = False
            try:
                present, present_path = load_scenario_artifact(
                    intent["target_path"],
                    allowed_roots=self.allowed_fs_roots,
                )
                exact_already_present = (
                    present == intent["scenario_document"]
                    and present_path
                    == str(Path(intent["target_path"]).expanduser().resolve())
                )
            except ScenarioArtifactError as error:
                if (
                    error.code != "scenario_artifact_path_invalid"
                    and not intent["overwrite"]
                ):
                    raise AuthoringJournalError(
                        "composer_authoring_scenario_save_recovery_conflict",
                        "Scenario Save target cannot be proven exact without overwrite authority",
                    ) from error
            if not exact_already_present:
                saved_path = save_scenario_artifact(
                    intent["scenario_document"],
                    intent["target_path"],
                    allowed_roots=self.allowed_fs_roots,
                    overwrite=bool(intent["overwrite"]),
                )
            else:
                saved_path = str(Path(intent["target_path"]).expanduser().resolve())
            self.scenario_document_path = saved_path
            self.remember_scenario_document(intent["scenario_document"])
            self._restore_authoring_post_image(post_state)
            return {
                "status": "saved",
                "path": saved_path,
                "scenario_id": intent["scenario_document"]["scenario_id"],
                "last_known_status": intent["scenario_document"]["saved_status"][
                    "last_known_status"
                ],
                "workspace": self.draft_session.workspace_snapshot(
                    applied_program_revision=None,
                    applied_unavailable_reason={
                        "code": "applied_state_refresh_required",
                        "message": "Applied Program status is refreshed after recovery.",
                    },
                ),
            }, post_state
        if kind == "save_deployment":
            exact_already_present = False
            try:
                source, present_path = load_deployment_artifact(
                    intent["target_path"],
                    allowed_roots=(
                        self.allowed_deployment_write_roots
                        if self.allowed_deployment_write_roots is not None
                        else self.allowed_fs_roots
                    ),
                )
                exact_already_present = (
                    compile_deployment_document(source).canonical_json
                    == intent["canonical_json"]
                    and present_path
                    == post_state["deployment"]["saved_path"]
                )
            except DeploymentDraftError as error:
                if (
                    error.code != "deployment_artifact_not_found"
                    and not intent["overwrite"]
                ):
                    raise AuthoringJournalError(
                        "composer_authoring_save_recovery_conflict",
                        "Deployment Save target cannot be proven exact without overwrite authority",
                    ) from error
            if not exact_already_present:
                save_deployment_artifact(
                    intent["canonical_json"],
                    intent["target_path"],
                    allowed_roots=(
                        self.allowed_deployment_write_roots
                        if self.allowed_deployment_write_roots is not None
                        else self.allowed_fs_roots
                    ),
                    overwrite=bool(intent["overwrite"]),
                )
            self._restore_authoring_post_image(post_state)
            return self.deployment_draft_session.snapshot(), post_state
        if kind == "revert_deployment":
            prepared = intent.get("prepared_deployment_candidate")
            if not isinstance(prepared, dict):
                raise AuthoringJournalError(
                    "activation_preparation_missing",
                    "Deployment Revert recovery has no prepared activation",
                )
            result = self.execute_authored_deployment_candidate(
                intent,
                claim,
                apply=True,
                prepared=prepared,
            )
            self._restore_authoring_post_image(post_state)
            return result, post_state
        if kind in {
            "deployment_candidate_preview",
            "deployment_candidate_apply",
        }:
            applied = kind == "deployment_candidate_apply"
            result = self.execute_authored_deployment_candidate(
                intent, claim, apply=applied
            )
            if applied:
                self._restore_authoring_post_image(post_state)
            return result, post_state
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
                if (
                    error.code != "program_artifact_not_found"
                    and not intent["overwrite"]
                ):
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
