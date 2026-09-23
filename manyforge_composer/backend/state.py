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
import time
from typing import Any, TYPE_CHECKING

from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment
from manyforge_behavior.canonical_artifact import canonical_json_bytes
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_headless_api.client import HeadlessClientError

from .draft_session import (
    ComposerDraftSession,
    DraftSessionError,
    draft_applicability,
)
from .deployment_draft import (
    DeploymentDraftError,
    DeploymentDraftSession,
)
from .authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringOperationError,
    AuthoringMutationEnvelope,
)
from .authoring_forest import from_program
from .authoring_identity import canonical_digest
from .authoring_workspace import AuthoringWorkspace
from .node_environment_client import read_active_node_environment
from .program_artifacts import (
    ProgramArtifactError,
    load_program_artifact,
)
from .deployment_artifacts import (
    load_deployment_artifact,
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
    "source_document_digest",
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
    "scenario_id",
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
_LOGGER = logging.getLogger(__name__)


class ComposerState:
    """Authoring state and a replaceable client, never runtime authority."""

    # Absolute monotonic budget for every post-Apply observation in one
    # activation. One deadline is computed per activation and shared by all
    # its phases; a test or caller may inject a shorter budget on the instance.

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
        # One composite in-memory authoring workspace per backend. It owns the
        # Scenario project facts beside the two Draft sessions and carries the
        # workspace epoch/revision. Runtime Scenario truth remains the exact
        # Headless/Manager/Applied projection. Unsaved state intentionally
        # survives browser reconnects but not a Composer process restart.
        self.authoring_workspace = AuthoringWorkspace(
            program_session=self.draft_session,
            deployment_session=self.deployment_draft_session,
        )
        self.authoring_admission = (
            AuthoringAdmissionCoordinator(headless_client)
            if headless_client is not None
            else None
        )
        self.assistant_client = assistant_client
        # Presentation-cache identity only. Runtime and Apply truth remain in
        # Headless/Manager; this value solely rejects an old browser mesh URL
        # after Composer has observed a successor viewer manifest.
        self.viewer_model_key: str | None = None
        # Exact document last activated through this Composer process. It is
        # an in-memory authoring baseline for Discard, never runtime truth.
        self.applied_deployment_document: dict[str, Any] | None = None

    @classmethod
    def from_paths(
        cls,
        *,
        program_path: str | None,
        allowed_fs_roots: list[str] | None = None,
        allowed_program_write_roots: list[str] | None = None,
        allowed_deployment_write_roots: list[str] | None = None,
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
        return cls(
            node_environment=node_environment,
            program=program,
            program_source_path=resolved_program,
            allowed_fs_roots=allowed_fs_roots,
            allowed_program_write_roots=allowed_program_write_roots,
            allowed_deployment_write_roots=allowed_deployment_write_roots,
            headless_client=headless_client,
            assistant_client=assistant_client,
        )

    @property
    def node_catalog(self):
        return self.node_environment.catalog

    # Scenario project facts are read-only projections of the one composite
    # authoring workspace. Writers commit through AuthoringWorkspace so one
    # logical mutation advances the shared revision exactly once.
    def _authoring_project_fact(self, name: str) -> Any:
        return self.authoring_workspace.project_facts()[name]

    @property
    def scenario_document_path(self) -> str | None:
        return self._authoring_project_fact("document_path")

    @property
    def scenario_document_id(self) -> str | None:
        return self._authoring_project_fact("document_id")

    @property
    def scenario_document_name(self) -> str | None:
        return self._authoring_project_fact("document_name")

    @property
    def scenario_document_description(self) -> str | None:
        return self._authoring_project_fact("document_description")

    @property
    def scenario_source_diagnostics(self) -> list[dict[str, str]]:
        return self._authoring_project_fact("source_diagnostics")

    @property
    def scenario_source_document_digest(self) -> str | None:
        return self._authoring_project_fact("source_document_digest")

    @property
    def scenario_program_role_assignment(self) -> dict[str, Any] | None:
        return self._authoring_project_fact("program_role_assignment")

    @property
    def scenario_typed_parameter_overrides(self) -> dict[str, Any]:
        return self._authoring_project_fact("typed_parameter_overrides")

    @property
    def scenario_source_program_digest(self) -> str | None:
        return self._authoring_project_fact("source_program_digest")

    @property
    def scenario_source_deployment_digest(self) -> str | None:
        return self._authoring_project_fact("source_deployment_digest")

    def remember_scenario_document(
        self,
        document: dict[str, Any] | None,
        *,
        source_document_digest: str | None = None,
        source_diagnostics: list[dict[str, str]] | None = None,
    ) -> None:
        """Remember one loaded Scenario project through the workspace owner."""

        self.authoring_workspace.remember_project(
            document=document,
            document_path=self.scenario_document_path,
            source_document_digest=source_document_digest,
            source_diagnostics=source_diagnostics,
        )

    def current_drafts_match_remembered_scenario(self) -> bool:
        facts = self.authoring_workspace.project_facts()
        if (
            facts["source_program_digest"] is None
            or facts["source_deployment_digest"] is None
        ):
            return False
        return (
            canonical_digest(
                {"value": self.draft_session.retained_state().get("authoring")}
            )
            == facts["source_program_digest"]
            and canonical_digest(
                {
                    "value": self.deployment_draft_session.retained_state().get(
                        "document"
                    )
                }
            )
            == facts["source_deployment_digest"]
        )

    def _active_authoring_catalog(self):
        return (
            read_active_node_environment(self).catalog
            if self.headless_client is not None
            else self.node_catalog
        )

    def authoring_retained_state(self) -> dict[str, Any]:
        """Return one snapshot of the shared in-memory authoring workspace."""

        if not self.deployment_draft_session.loaded:
            return self.draft_session.retained_state()
        return {
            "schema_version": "manyforge.composer_authoring_workspace.v2",
            "program": self.draft_session.retained_state(),
            "deployment": self.deployment_draft_session.retained_state(),
        }

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
                "manager_projection_unavailable",
                "activation requires one fresh Manager projection",
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
        if current is not None and not self._manager_matches_scenario(
            self.headless_client.manager_status(),
            self.headless_client.manager_diff(),
            current,
        ):
            reasons.append("deployment_not_active_and_converged")
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
        if applied is None:
            reasons.append("program_not_applied")
        if applied is not None:
            status = self.headless_client.manager_status()
            if not self._manager_published_matches_scenario(status, applied):
                reasons.append("deployment_not_published")
        current = None
        if applied is not None and not reasons:
            current = {
                field: applied[field] for field in _CURRENT_SCENARIO_FIELDS
            }
        return {
            "schema_version": "manyforge.published_scenario.v1",
            "ready": current is not None,
            "published_scenario": current,
            "unavailable_reasons": reasons,
        }

    def apply_manager_deployment_candidate(
        self,
        intent: dict[str, Any],
        claim: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        """Submit one exact candidate; Manager owns terminal observation."""

        if self.headless_client is None:
            raise AuthoringOperationError(
                "manager_apply_unavailable",
                "Manager-backed Deployment Apply is unavailable",
            )
        request = intent.get("manager_apply")
        if not isinstance(request, dict):
            raise AuthoringOperationError(
                "manager_candidate_intent_invalid",
                "retained Deployment candidate intent is incomplete",
            )
        request_body = deepcopy(request)
        if claim is not None:
            # The parent claim already captures the accepted admission
            # generation. Manager's contract deliberately permits exactly one
            # admission form, never both a direct generation and a parent.
            request_body.pop("expected_admission_generation", None)
            request_body["parent_authoring_claim"] = claim
        try:
            response = self.headless_client.manager_apply(request_body)
        except HeadlessClientError as error:
            if error.code in {"headless_http_timeout", "headless_transport_error"}:
                raise AuthoringOperationError(
                    "manager_apply_submission_uncertain",
                    "Manager Apply acceptance could not be determined",
                ) from error
            raise
        if response.get("ok") is not True:
            raise AuthoringOperationError(
                "manager_apply_rejected",
                "Manager rejected the exact Deployment candidate",
            )
        acceptance = deepcopy(response)
        if (
            response.get("state") != "pending"
            or not isinstance(response.get("apply_id"), str)
            or type(response.get("apply_generation")) is not int
        ):
            raise AuthoringOperationError(
                "manager_apply_acceptance_invalid",
                "Manager did not return one observable accepted operation",
            )
        return acceptance

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

    def _prepare_authored_deployment_candidate(
        self,
        intent: dict[str, Any],
        claim: dict[str, Any] | None,
        *,
        apply: bool,
    ) -> dict[str, Any]:
        """Prepare one captured input and its exact next Manager request."""

        assert self.headless_client is not None
        phases: dict[str, float] = {}
        started = time.monotonic()
        response = self.headless_client.prepare_activation(
            {
                "source_document_digest": intent["source_document_digest"],
                "scenario_id": intent["scenario_id"],
                "captured_input_digest": intent["captured_input_digest"],
                "deployment": deepcopy(intent["deployment"]),
                "model_source_closure": deepcopy(intent["model_source_closure"]),
                "program": deepcopy(intent["program"]),
                "program_role_assignment": deepcopy(
                    intent["program_role_assignment"]
                ),
                "typed_parameter_overrides": deepcopy(
                    intent["typed_parameter_overrides"]
                ),
                "expected_admission_generation": intent[
                    "expected_admission_generation"
                ],
            }
        )
        prepared_activation = response.get("prepared_activation")
        if not isinstance(prepared_activation, dict) or any(
            field not in prepared_activation for field in _PREPARED_ACTIVATION_FIELDS
        ):
            raise AuthoringOperationError(
                "activation_preparer_response_invalid",
                "Activation preparation omitted its exact result",
            )
        phases["preparation_ms"] = (time.monotonic() - started) * 1000.0
        started = time.monotonic()
        status = self.headless_client.manager_status()
        current_bundle, current_graph, manager_epoch = self._manager_current(status)
        if intent["expected_manager_current"] != {
            "retained_bundle_id": current_bundle,
            "graph_revision": current_graph,
        }:
            raise AuthoringOperationError(
                "deployment_candidate_manager_state_changed",
                "Manager selection changed before Deployment candidate preview",
            )
        preview = self.headless_client.manager_deployment_candidate_preview(
            {
                "prepared_activation_id": prepared_activation[
                    "prepared_activation_id"
                ],
                "candidate_retained_bundle_id": prepared_activation[
                    "retained_bundle_id"
                ],
                "candidate_graph_revision": prepared_activation["graph_revision"],
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
            raise AuthoringOperationError(
                "deployment_candidate_rejected",
                "Manager rejected the prepared Deployment candidate",
            )
        if (
            receipt.get("candidate_retained_bundle_id")
            != prepared_activation["retained_bundle_id"]
            or receipt.get("candidate_graph_revision")
            != prepared_activation["graph_revision"]
        ):
            raise AuthoringOperationError(
                "deployment_candidate_preview_mismatch",
                "Manager preview returned another prepared candidate",
            )
        phases["preview_ms"] = (time.monotonic() - started) * 1000.0

        fresh_confirmation_token = self._deployment_candidate_confirmation_token(
            draft_id=intent["draft_id"],
            draft_revision=intent["draft_revision"],
            prepared_activation=prepared_activation,
            preview=preview,
        )
        supplied_confirmation = intent.get("confirmation_token")
        if (
            supplied_confirmation is not None
            and supplied_confirmation != fresh_confirmation_token
        ):
            raise AuthoringOperationError(
                "confirmation_stale",
                "the captured activation changed since it was confirmed",
            )

        confirmation_required = bool(
            apply
            and intent.get("confirmation_token") is None
            and preview is not None
            and self._deployment_candidate_is_disruptive(preview.get("diff"))
        )
        confirmation_token = (
            fresh_confirmation_token if confirmation_required else None
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
                "prepared_activation": {
                    field: deepcopy(prepared_activation[field])
                    for field in _PREPARED_ACTIVATION_FIELDS
                },
                "expected_admission_generation": intent[
                    "expected_admission_generation"
                ],
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
        """Prepare or submit one candidate to Manager's transition owner."""

        if self.headless_client is None:
            raise AuthoringOperationError(
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
            raise AuthoringOperationError(
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
                "preparation": None,
                "phases_ms": phases,
                "apply_id": None,
            }
        if not isinstance(manager_apply, dict):
            # The exact prepared configuration is already active. Observing
            # that fact is not a second effect and needs no synthetic Apply.
            return {
                "status": "applied",
                "confirmation_token": None,
                "prepared_activation": prepared_activation,
                "candidate_preview": preview,
                "manager_apply": None,
                "preparation": None,
                "phases_ms": phases,
                "apply_id": None,
            }
        manager_result = self.apply_manager_deployment_candidate(
            {"manager_apply": manager_apply}, claim
        )
        return {
            "status": "accepted",
            "confirmation_token": None,
            "prepared_activation": prepared_activation,
            "candidate_preview": preview,
            "manager_apply": manager_result,
            "preparation": None,
            "phases_ms": phases,
            "apply_id": manager_result.get("apply_id"),
            "apply_generation": manager_result.get("apply_generation"),
        }

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
