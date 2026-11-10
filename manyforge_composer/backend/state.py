"""Structural Composer state plus an optional replaceable headless client.

The former Composer runtime state machine, scene cache, node discovery owner,
Runner/CycleManager construction, and robot-control authorities were removed
with the legacy scene owner. This state contains only immutable built-in
catalog identity, an optional authored Program in one in-memory Draft, and
optional HTTP clients that own no runtime truth.
"""

from __future__ import annotations

from copy import deepcopy
import json
import logging
import hashlib
from pathlib import Path
from threading import Lock
import time
from typing import Any, TYPE_CHECKING

from manyforge_behavior.node_environment import NodeEnvironment, build_node_environment
from manyforge_behavior.canonical_artifact import canonical_json_bytes
from manyforge_behavior.program import Program, ProgramValidationError
from manyforge_headless_api.client import HeadlessClientError
from manyforge_headless_api.program_binding_projection import (
    validate_prepared_activation_origin,
    validate_project_path,
)

from .draft_session import (
    ComposerDraftSession,
    DraftSessionError,
)
from .deployment_draft import (
    DeploymentDraftError,
    DeploymentDraftSession,
)
from .authoring_admission import (
    AuthoringAdmissionCoordinator,
    AuthoringOperationError,
    AuthoringMutationEnvelope,
    headless_client_http_exception,
)
from .authoring_forest import from_program
from .authoring_identity import canonical_digest
from .deployment_intent import (
    DeploymentIntentError,
    rebase_deployment_intent_sources,
)
from .assistant_mode_client import AssistantContextCache
from .assistant_provider_client import AssistantProviderHttpClient
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


RUNTIME_UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
RUNTIME_UNAVAILABLE_MESSAGE = (
    "Composer is not a runtime owner; configure the headless API for owner-issued "
    "status, scene, alarm/history, preview, operation, Program Apply/Run, and "
    "explicit operation/Program Stop facts. The structural-only Composer profile "
    "does not launch those owners"
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
    "program_outcome",
)
_PROGRAM_NON_EXECUTABLE_OUTCOME_FIELDS = frozenset(
    {
        "affected_execution_groups",
        "affected_nodes",
        "api_version",
        "graph_revision",
        "program_id",
        "program_revision",
        "retained_bundle_id",
        "unresolved_targets",
    }
)
_PROGRAM_UNRESOLVED_TARGET_FIELDS = frozenset({"target", "reason", "detail"})
_APPLIED_PROGRAM_STATUS_FIELDS = frozenset(
    {
        "deployment_artifact_id",
        "deployment_revision",
        "executable",
        "graph_revision",
        "message",
        "non_executable",
        "program",
        "program_binding_id",
        "program_id",
        "program_revision",
        "record_digest",
        "retained_bundle_id",
        "origin",
        "schema_version",
    }
)
_ProgramOutcomeIdentity = tuple[str, str]
_DeploymentPublicationIdentity = tuple[str, str, str, str | None, _ProgramOutcomeIdentity]


def canonical_project_path(path: str) -> str:
    """The Scenario project folder as Apply records it: resolved and absolute.

    Composer owns the files, so it resolves the path; Manager and the Program
    runner only compare it. Persistent values are scoped by it.
    """

    return validate_project_path(str(Path(path).expanduser().resolve()))


def _is_sha256_identity(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 71
        and value.startswith("sha256:")
        and all(character in "0123456789abcdef" for character in value[7:])
    )


def _manager_refusal_message(lead: str, response: object) -> str:
    """Keep Manager's own refusal code and message after the generic lead.

    The Composer code, HTTP status and effect handling stay those of the lead;
    only the text names Manager's precise cause when the response carries one.
    """

    error = response.get("error") if isinstance(response, dict) else None
    if not isinstance(error, dict):
        return lead
    code = error.get("code")
    message = error.get("message")
    cause = ": ".join(
        value for value in (code, message) if isinstance(value, str) and value
    )
    return f"{lead}: {cause}" if cause else lead


_PREPARED_ACTIVATION_FIELDS = (
    "schema_version",
    "prepared_activation_id",
    "origin",
    "captured_input_digest",
    "deployment_artifact_id",
    "deployment_revision",
    "retained_bundle_id",
    "graph_revision",
    "program_id",
    "program_revision",
    # Bound binding id or named non-executable result; part of the
    # confirmation digest, so a changed Program impact is a stale confirmation.
    "program_outcome",
    "node_environment_digest",
    "assistant_mode",
)
_LOGGER = logging.getLogger(__name__)
# F-6b (item 40): process-lifetime diagnostics of the rare
# `manager_projection_unavailable` refusal; they decide nothing.
_MANAGER_PROJECTION_GAPS: dict[str, Any] = {"count": 0, "last_current_monotonic": None}
_MAX_DIAGNOSTIC_GAPS = 8
_MAX_DIAGNOSTIC_TEXT = 128
_MAX_DIAGNOSTIC_LOG_LINE = 1_024


def _manager_projection_diagnostics(
    phase: str,
    failed: list[str],
    source: dict[str, Any],
    payload: Any,
) -> dict[str, Any]:
    """F-6b (item 40): what one `manager_projection_unavailable` saw, bounded."""

    gaps = _MANAGER_PROJECTION_GAPS
    gaps["count"] += 1
    last_current = gaps["last_current_monotonic"]
    age_ns = source.get("receiver_age_ns")
    age_ns = age_ns if isinstance(age_ns, int) and not isinstance(age_ns, bool) else None
    known_gaps = source.get("known_gaps")
    owner = source.get("source_owner_id")
    return {
        "phase": phase,
        "failed_checks": failed,
        "available": source.get("available"),
        "freshness": source.get("freshness"),
        "known_gaps": (
            [str(gap)[:_MAX_DIAGNOSTIC_TEXT] for gap in known_gaps[:_MAX_DIAGNOSTIC_GAPS]]
            if isinstance(known_gaps, list)
            else None
        ),
        # Age of the Manager publication when Headless read it, and the
        # publication time it implies (this host's wall clock).
        "receiver_age_ms": None if age_ns is None else age_ns // 1_000_000,
        "source_last_update_unix_ms": (
            None if age_ns is None else (time.time_ns() - age_ns) // 1_000_000
        ),
        "source_owner_id": (
            owner[:_MAX_DIAGNOSTIC_TEXT] if isinstance(owner, str) else None
        ),
        "source_generation": source.get("source_generation"),
        "projection_generation": source.get("projection_generation"),
        "manager_ready": payload.get("manager_ready") if isinstance(payload, dict) else None,
        # Refusals in this process, and time since a current projection was
        # last seen here (the length of the gap so far).
        "unavailable_count": gaps["count"],
        "since_last_current_ms": (
            None if last_current is None else int((time.monotonic() - last_current) * 1000)
        ),
    }


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
        assistant_client: AssistantProviderHttpClient | None = None,
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
        # Set when the startup project was not opened; cleared by the next
        # successful Load or New. Shown to the GUI through /api/health.
        self.startup_scenario_notice: dict[str, str] | None = None
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
        # The bridge URL is resolved per request from the assistant
        # component's published endpoint; one HTTP client serves every call.
        self.assistant_client = (
            assistant_client or AssistantProviderHttpClient()
            if headless_client is not None
            else None
        )
        self.assistant_context_cache = AssistantContextCache()
        # Composer's in-flight chat turns (request ids answered `running`);
        # Execute admission refuses while one is still running.
        self.assistant_turns: set[str] = set()
        self.assistant_turns_lock = Lock()
        # Presentation-cache identity only. Runtime and Apply truth remain in
        # Headless/Manager; this value solely rejects an old browser mesh URL
        # after Composer has observed a successor viewer manifest.
        self.viewer_model_key: str | None = None
        # Keep only one pending candidate beside the last verified Applied
        # authoring input. Manager/Program publication decides promotion.
        self._applied_deployment_lock = Lock()
        # Headless runs one activation preparation at a time; previews and
        # Apply preparations wait here for their turn instead of being refused.
        self._activation_preparation_lock = Lock()
        self.applied_deployment_document: dict[str, Any] | None = None
        self._applied_activation_context: dict[str, Any] | None = None
        self._applied_deployment_identity: _DeploymentPublicationIdentity | None = None
        self._pending_applied_deployment: tuple[
            _DeploymentPublicationIdentity, dict[str, Any]
        ] | None = None

    @staticmethod
    def _program_outcome_identity(
        publication: dict[str, Any],
    ) -> _ProgramOutcomeIdentity:
        outcome = publication.get("program_outcome")
        if not isinstance(outcome, dict):
            raise ValueError("publication has no typed Program outcome")
        kind = outcome.get("kind")
        if kind == "no_program":
            if (set(outcome) != {"kind"} or publication.get("program_id") is not None
                    or publication.get("program_revision") is not None):
                raise ValueError("no_program must not carry Program identity")
            return ("no_program", "")
        if kind == "bound":
            if set(outcome) != {"kind", "program_binding_id"}:
                raise ValueError("bound Program outcome has unexpected fields")
            binding_id = outcome.get("program_binding_id")
            if not _is_sha256_identity(binding_id):
                raise ValueError("bound Program outcome lacks its binding identity")
            return ("bound", binding_id)
        if kind != "non_executable" or set(outcome) != {"kind", "outcome"}:
            raise ValueError("Program outcome must be bound or non_executable")

        detail = outcome.get("outcome")
        if (
            not isinstance(detail, dict)
            or set(detail) != _PROGRAM_NON_EXECUTABLE_OUTCOME_FIELDS
            or detail.get("api_version") != "manyforge.program-binding-outcome.v2"
            or detail.get("program_id") != publication.get("program_id")
            or detail.get("program_revision") != publication.get("program_revision")
            or detail.get("retained_bundle_id")
            != publication.get("retained_bundle_id")
            or detail.get("graph_revision") != publication.get("graph_revision")
        ):
            raise ValueError("non-executable Program outcome does not match its publication")
        for name in (
            "graph_revision",
            "program_revision",
            "retained_bundle_id",
        ):
            value = detail.get(name)
            if not isinstance(value, str) or not value:
                raise ValueError(f"non-executable Program outcome has invalid {name}")
        for name in (
            "graph_revision",
            "program_revision",
            "retained_bundle_id",
        ):
            if not _is_sha256_identity(detail[name]):
                raise ValueError(f"non-executable Program outcome has invalid {name}")
        unresolved = detail.get("unresolved_targets")
        if not isinstance(unresolved, list) or not unresolved:
            raise ValueError("non-executable Program outcome has no unresolved targets")
        for row in unresolved:
            if (
                not isinstance(row, dict)
                or set(row) != _PROGRAM_UNRESOLVED_TARGET_FIELDS
                or any(
                    not isinstance(row.get(name), str) or not row[name]
                    for name in ("detail", "reason")
                )
                or not isinstance(row.get("target"), dict)
                or set(row["target"]) != {"component_id", "command_target_id"}
                or any(not isinstance(row["target"].get(name), str)
                       or not row["target"][name]
                       for name in ("component_id", "command_target_id"))
            ):
                raise ValueError("non-executable Program outcome has an invalid target row")
        for name in ("affected_nodes", "affected_execution_groups"):
            values = detail.get(name)
            if not isinstance(values, list) or any(
                not isinstance(item, str) or not item for item in values
            ):
                raise ValueError(f"non-executable Program outcome has invalid {name}")
        canonical_outcome = canonical_json_bytes(detail).decode("utf-8")
        return ("non_executable", canonical_outcome)

    @classmethod
    def _deployment_publication_identity(
        cls, publication: dict[str, Any]
    ) -> _DeploymentPublicationIdentity:
        outcome_identity = cls._program_outcome_identity(publication)
        identity_fields = (
            "retained_bundle_id",
            "graph_revision",
            "deployment_revision",
            "program_revision",
        )
        identity_values = tuple(publication.get(name) for name in identity_fields)
        required_values = identity_values[:3] if outcome_identity[0] == "no_program" else identity_values
        if any(not isinstance(value, str) or not value for value in required_values):
            raise ValueError("publication lacks a complete activation identity")
        if any(not _is_sha256_identity(value) for value in required_values):
            raise ValueError("publication has an invalid activation digest")
        return (*identity_values, outcome_identity)

    @classmethod
    def _published_program_projection(
        cls, status: dict[str, Any]
    ) -> dict[str, Any]:
        if (
            set(status) != _APPLIED_PROGRAM_STATUS_FIELDS
            or status.get("schema_version") != "manyforge.applied_program_status.v2"
            or type(status.get("executable")) is not bool
        ):
            raise ValueError("Applied Program status has an unsupported shape")
        origin = validate_prepared_activation_origin(status.get("origin"))
        executable = status["executable"]
        binding_id = status.get("program_binding_id")
        non_executable = status.get("non_executable")
        no_program = status.get("program_id") is None
        if no_program:
            if (executable or binding_id is not None or non_executable is not None
                    or status.get("program") is not None
                    or status.get("program_revision") is not None):
                raise ValueError("no_program Applied status is contradictory")
            outcome: dict[str, Any] = {"kind": "no_program"}
        elif executable:
            if (
                not _is_sha256_identity(binding_id)
                or non_executable is not None
                or status.get("program") is not None
                or status.get("message") is not None
            ):
                raise ValueError("executable Applied Program status is contradictory")
            outcome = {
                "kind": "bound",
                "program_binding_id": binding_id,
            }
        else:
            if (
                binding_id is not None
                or not isinstance(non_executable, dict)
                or not isinstance(status.get("program"), dict)
                or not isinstance(status.get("message"), str)
            ):
                raise ValueError("non-executable Applied Program status is contradictory")
            outcome = {"kind": "non_executable", "outcome": non_executable}

        projection = {
            name: status.get(name)
            for name in (
                "deployment_artifact_id",
                "deployment_revision",
                "retained_bundle_id",
                "graph_revision",
                "program_id",
                "program_revision",
            )
        }
        if (
            any(not isinstance(value, str) or not value
                for name, value in projection.items()
                if not (no_program and name in {"program_id", "program_revision"}))
            or not _is_sha256_identity(projection["deployment_revision"])
            or not _is_sha256_identity(projection["retained_bundle_id"])
            or not _is_sha256_identity(projection["graph_revision"])
            or (not no_program and not _is_sha256_identity(projection["program_revision"]))
            or not _is_sha256_identity(status.get("record_digest"))
        ):
            raise ValueError("Applied Program status lacks activation identity fields")
        projection.update(scenario_id=origin["scenario_id"],
                          source_document_digest=origin["source_document_digest"])
        projection["program_outcome"] = outcome
        cls._deployment_publication_identity(projection)
        return projection

    def applied_deployment_for_publication(
        self, publication: dict[str, Any]
    ) -> dict[str, Any] | None:
        """Return authored input only after its exact Manager publication."""

        context = self.applied_activation_for_publication(publication)
        return context["deployment"] if context is not None else None

    def applied_activation_for_publication(
        self, publication: dict[str, Any]
    ) -> dict[str, Any] | None:
        """Return one captured activation input verified against publication."""

        identity = self._deployment_publication_identity(publication)
        with self._applied_deployment_lock:
            pending = self._pending_applied_deployment
            if pending is not None and pending[0] == identity:
                self._applied_deployment_identity = identity
                self._applied_activation_context = pending[1]
                self.applied_deployment_document = pending[1]["deployment"]
                self._pending_applied_deployment = None
            if self._applied_deployment_identity != identity:
                return None
            return deepcopy(self._applied_activation_context)

    def _captured_deployment_context(
        self, intent: dict[str, Any], prepared_activation: dict[str, Any]
    ) -> tuple[_DeploymentPublicationIdentity, dict[str, Any]]:
        deployment = intent.get("deployment")
        if not isinstance(deployment, dict):
            raise AuthoringOperationError(
                "activation_deployment_missing",
                "activation has no captured Deployment authoring input",
            )
        origin = prepared_activation.get("origin")
        if (
            not isinstance(origin, dict) or origin.get("kind") != "scenario"
            or origin.get("scenario_id") != intent.get("scenario_id")
            or origin.get("source_document_digest")
            != intent.get("source_document_digest")
            or origin.get("project_path") != intent.get("project_path")
        ):
            raise AuthoringOperationError(
                "activation_authoring_capture_mismatch",
                "prepared activation does not match its captured Scenario input",
            )
        identity = self._deployment_publication_identity(prepared_activation)
        context = {
            "deployment": deepcopy(deployment),
            "program_authoring": deepcopy(intent.get("program_authoring")),
            "model_source_closure": deepcopy(intent["model_source_closure"]),
            "source_document_digest": intent["source_document_digest"],
            "scenario_id": intent["scenario_id"],
            "scenario_project_path": intent["project_path"],
            "project_path": self.deployment_source_project_path,
        }
        return identity, context

    def _retain_submitted_deployment(
        self, captured: tuple[_DeploymentPublicationIdentity, dict[str, Any]]
    ) -> None:
        with self._applied_deployment_lock:
            self._pending_applied_deployment = captured

    @classmethod
    def from_paths(
        cls,
        *,
        program_path: str | None,
        allowed_fs_roots: list[str] | None = None,
        allowed_program_write_roots: list[str] | None = None,
        allowed_deployment_write_roots: list[str] | None = None,
        headless_client: "HeadlessHttpClient | None" = None,
    ) -> "ComposerState":
        node_environment = build_node_environment()
        resolved_program = (
            str(Path(program_path).resolve()) if program_path is not None else None
        )
        program = (
            Program(
                resolved_program,
                node_environment.catalog,
                authoring_load=True,
            )
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
    def scenario_project_path(self) -> str | None:
        """The loaded Scenario project folder, canonical as Apply records it."""

        path = self.scenario_document_path
        return None if path is None else canonical_project_path(path)

    @property
    def deployment_source_project_path(self) -> str | None:
        """Base directory for relative assets in the current Deployment Draft."""

        return (
            self.deployment_draft_session.source_project_root
            or self.scenario_document_path
        )

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
        deployment = self.deployment_draft_session.retained_state().get("document")
        source_root = self.deployment_source_project_path
        document_path = facts["document_path"]
        if (
            isinstance(deployment, dict)
            and isinstance(source_root, str)
            and isinstance(document_path, str)
            and Path(source_root).resolve() != Path(document_path).resolve()
        ):
            try:
                deployment = rebase_deployment_intent_sources(
                    deployment,
                    source_project_root=Path(source_root),
                    destination_project_root=Path(document_path),
                )
            except DeploymentIntentError:
                # An unresolvable source cannot prove that the current Draft
                # is the saved Scenario. Keep the explicit discard guard.
                return False
        return (
            canonical_digest(
                {"value": self.draft_session.retained_state().get("authoring")}
            )
            == facts["source_program_digest"]
            and canonical_digest({"value": deployment})
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
        *,
        phase: str = "unspecified",
    ) -> tuple[str, str, str]:
        source = status.get("source")
        payload = status.get("status")
        active = payload.get("active") if isinstance(payload, dict) else None
        row = source if isinstance(source, dict) else {}
        # The same predicates as before, named so a refusal says which failed.
        failed = [
            check
            for check, current in (
                ("source_available", row.get("available") is True),
                ("source_fresh", row.get("freshness") == "fresh"),
                ("no_known_gaps", row.get("known_gaps") == []),
                (
                    "source_owner_epoch",
                    isinstance(row.get("source_owner_epoch"), str)
                    and bool(row["source_owner_epoch"]),
                ),
                (
                    "manager_ready",
                    isinstance(payload, dict) and payload.get("manager_ready") is True,
                ),
                (
                    "active_identity",
                    isinstance(active, dict)
                    and isinstance(active.get("retained_bundle_id"), str)
                    and isinstance(active.get("graph_revision"), str),
                ),
            )
            if not current
        ]
        if failed:
            diagnostics = _manager_projection_diagnostics(phase, failed, row, payload)
            _LOGGER.warning(
                "manager_projection_unavailable %s",
                json.dumps(diagnostics, sort_keys=True, default=str)[:_MAX_DIAGNOSTIC_LOG_LINE],
            )
            raise DraftSessionError(
                "manager_projection_unavailable",
                "activation requires one fresh Manager projection",
                status_code=503,
                detail=diagnostics,
            )
        _MANAGER_PROJECTION_GAPS["last_current_monotonic"] = time.monotonic()
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
            if error.code in {"program_applied_non_executable", "program_not_selected"}:
                # The Applied Program is preserved but has no executable
                # selection; its named status is read separately.
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
        if (
            current is not None
            and current["program_outcome"].get("kind") != "bound"
        ):
            reasons.append("program_not_selected" if current["program_outcome"].get("kind") == "no_program" else "program_not_executable")
        if current is not None and not self._manager_matches_scenario(
            self.headless_client.manager_status(),
            self.headless_client.manager_diff(),
            current,
        ):
            reasons.append("deployment_not_active_and_converged")
        execution_current = current if current is not None and not reasons else None
        if execution_current is not None:
            bound_outcome = execution_current["program_outcome"]
            execution_current = {
                name: value
                for name, value in execution_current.items()
                if name != "program_outcome"
            }
            execution_current["program_binding_id"] = bound_outcome[
                "program_binding_id"
            ]
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
        status_reader = getattr(self.headless_client, "program_applied_status", None)
        if not callable(status_reader):
            reasons.append("program_applied_status_unavailable")
            applied = None
        else:
            try:
                applied = self._published_program_projection(status_reader())
            except HeadlessClientError as error:
                if error.code in {
                    "program_applied_selection_not_found",
                    "program_applied_selection_not_active",
                }:
                    reasons.append("program_not_applied")
                    applied = None
                else:
                    reasons.append(error.code)
                    applied = None
            except (TypeError, ValueError):
                reasons.append("program_applied_status_invalid")
                applied = None
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
            if error.details.get("effect") == "none":
                raise AuthoringOperationError(
                    "manager_apply_rejected",
                    _manager_refusal_message(
                        "Manager rejected the exact Deployment candidate", error.response
                    ),
                ) from error
            raise
        if response.get("ok") is not True:
            raise AuthoringOperationError(
                "manager_apply_rejected",
                _manager_refusal_message(
                    "Manager rejected the exact Deployment candidate", response
                ),
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
                # The Manager's authoritative impact: a changed impact
                # makes an earlier confirmation stale.
                "impact": preview.get("impact"),
            }
        )

    def _prepare_authored_deployment_candidate(
        self,
        intent: dict[str, Any],
        claim: dict[str, Any] | None,
        *,
        apply: bool,
        confirmed: bool = False,
    ) -> dict[str, Any]:
        """Prepare one captured input and its exact next Manager request.

        ``confirmed`` means the request itself is the operator's deliberate
        confirmation of this exact candidate (Scenario Load), so a disruptive
        or destructive impact is accepted from this one preparation instead of
        a confirmation token round trip that would prepare it again.
        """

        assert self.headless_client is not None
        phases: dict[str, float] = {}
        started = time.monotonic()
        try:
            with self._activation_preparation_lock:
                response = self.headless_client.prepare_activation(
                    {
                        "source_document_digest": intent["source_document_digest"],
                        "scenario_id": intent["scenario_id"],
                        "project_path": intent["project_path"],
                        "captured_input_digest": intent["captured_input_digest"],
                        "deployment": deepcopy(intent["deployment"]),
                        "model_source_closure": deepcopy(
                            intent["model_source_closure"]
                        ),
                        "program": deepcopy(intent["program"]),
                        "expected_admission_generation": intent[
                            "expected_admission_generation"
                        ],
                    }
                )
        except HeadlessClientError as error:
            if (
                claim is None
                and error.status_code is not None
                and 400 <= error.status_code < 500
            ):
                # Unclaimed preparation is effect-free and precedes every
                # claim, workspace commit and Manager submission of its
                # request, and the preparer answered with a definite refusal:
                # nothing changed. A lost or failed answer, or any failure
                # under a claim, proves no such thing, so only this call site
                # marks it, from where it is, never from its code.
                raise headless_client_http_exception(
                    error, before_effect=True
                ) from error
            raise
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
        current_bundle, current_graph, manager_epoch = self._manager_current(
            status, phase="activation.after_preparation"
        )
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
                _manager_refusal_message(
                    "Manager rejected the prepared Deployment candidate", preview
                ),
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
        supplied_confirmation = (
            fresh_confirmation_token if confirmed else intent.get("confirmation_token")
        )
        if (
            supplied_confirmation is not None
            and supplied_confirmation != fresh_confirmation_token
        ):
            raise AuthoringOperationError(
                "confirmation_stale",
                "the captured activation changed since it was confirmed",
            )

        impact = preview.get("impact") if preview is not None else None
        destructive_impact = isinstance(impact, dict) and impact.get(
            "destructive"
        ) is True
        confirmation_required = bool(
            apply
            and supplied_confirmation is None
            and preview is not None
            and (
                destructive_impact
                or self._deployment_candidate_is_disruptive(preview.get("diff"))
            )
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
                "scene_initialization": intent.get("scene_initialization", "preserve"),
                "prepared_activation": {
                    field: deepcopy(prepared_activation[field])
                    for field in _PREPARED_ACTIVATION_FIELDS
                },
                "expected_admission_generation": intent[
                    "expected_admission_generation"
                ],
                "candidate_receipt": receipt,
                # Destructive configuration impact requires the same
                # exact confirmation; Manager revalidates its accepted digest.
                "accepted_impact_digest": (
                    receipt.get("impact_digest")
                    if destructive_impact
                    and supplied_confirmation == fresh_confirmation_token
                    else None
                ),
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
        confirmed: bool = False,
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
                intent, claim, apply=apply, confirmed=confirmed
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
        # Validate and freeze all authoring facts before the Manager effect.
        # Once Manager accepts, an unrelated Composer error must not turn an
        # accepted operation into an apparently failed submission.
        captured_deployment = self._captured_deployment_context(
            intent, prepared_activation
        )
        # Before replacing the one pending capture, recognize a predecessor
        # that Manager has already published. This matters for direct clients
        # that submit the next Apply without first reading a workspace view.
        with self._applied_deployment_lock:
            has_pending_applied = self._pending_applied_deployment is not None
        if has_pending_applied:
            prior = self.published_scenario().get("published_scenario")
            if isinstance(prior, dict):
                self.applied_activation_for_publication(prior)
        if not isinstance(manager_apply, dict):
            # The exact prepared configuration is already active. Observing
            # that fact is not a second effect and needs no synthetic Apply.
            self._retain_submitted_deployment(captured_deployment)
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
        self._retain_submitted_deployment(captured_deployment)
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
