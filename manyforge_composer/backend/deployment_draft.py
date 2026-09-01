"""Composer-owned, revision-fenced Deployment document Draft."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import json
import math
from pathlib import Path
import threading
from typing import Any, Callable, NoReturn, Sequence
import uuid


_MAX_DOCUMENT_BYTES = 16 * 1024 * 1024
_MAX_PATH_DEPTH = 32
_MAX_COLLECTION_INDEX = 100_000
_RETAINED_SCHEMA = "manyforge.composer_deployment_draft_state.v1"


class DeploymentDraftError(RuntimeError):
    def __init__(self, code: str, message: str, *, status_code: int = 400) -> None:
        super().__init__(message)
        self.code = code
        self.status_code = status_code


def _fail(code: str, message: str, *, status_code: int = 400) -> NoReturn:
    raise DeploymentDraftError(code, message, status_code=status_code)


@dataclass(frozen=True, slots=True)
class CompiledDeploymentDocument:
    document: dict[str, Any]
    canonical_json: str
    deployment_artifact_id: str
    deployment_revision: str
    graph_revision: str


DeploymentCompiler = Callable[[str], CompiledDeploymentDocument]


def compile_deployment_document(source: str) -> CompiledDeploymentDocument:
    """Call the one C++ deployment compiler without recreating its schema."""

    try:
        import manyforge_headless_py  # type: ignore[import-not-found]

        result = manyforge_headless_py.compile_deployment_draft(source)
    except ImportError:
        _fail(
            "deployment_compiler_unavailable",
            "the build-qualified Deployment compiler is unavailable",
            status_code=503,
        )
    except Exception as error:
        _fail("deployment_draft_invalid", str(error))
    expected = {
        "canonical_json",
        "deployment_artifact_id",
        "deployment_revision",
        "graph_revision",
    }
    if not isinstance(result, dict) or set(result) != expected:
        _fail(
            "deployment_compiler_response_invalid",
            "the Deployment compiler returned an invalid result",
            status_code=502,
        )
    canonical = result["canonical_json"]
    if not isinstance(canonical, str) or len(canonical.encode("utf-8")) > _MAX_DOCUMENT_BYTES:
        _fail(
            "deployment_compiler_response_invalid",
            "the Deployment compiler returned invalid canonical bytes",
            status_code=502,
        )
    try:
        document = json.loads(canonical)
    except (TypeError, ValueError) as error:
        _fail(
            "deployment_compiler_response_invalid",
            f"the Deployment compiler returned invalid JSON: {error}",
            status_code=502,
        )
    if not isinstance(document, dict):
        _fail(
            "deployment_compiler_response_invalid",
            "the compiled Deployment root is not an object",
            status_code=502,
        )
    identities = (
        result["deployment_artifact_id"],
        result["deployment_revision"],
        result["graph_revision"],
    )
    if not all(isinstance(value, str) and value for value in identities):
        _fail(
            "deployment_compiler_response_invalid",
            "the Deployment compiler omitted an identity",
            status_code=502,
        )
    return CompiledDeploymentDocument(
        document=document,
        canonical_json=canonical,
        deployment_artifact_id=identities[0],
        deployment_revision=identities[1],
        graph_revision=identities[2],
    )


def _source(document: dict[str, Any]) -> str:
    try:
        source = json.dumps(
            document,
            ensure_ascii=False,
            allow_nan=False,
            separators=(",", ":"),
        )
    except (TypeError, ValueError) as error:
        _fail("deployment_draft_invalid", f"Deployment value is invalid: {error}")
    if len(source.encode("utf-8")) > _MAX_DOCUMENT_BYTES:
        _fail(
            "deployment_draft_oversized",
            "Deployment Draft exceeds the artifact byte bound",
            status_code=413,
        )
    return source


def _edit_at_path(
    document: dict[str, Any],
    path: Sequence[str | int],
    value: Any,
    operation: str,
) -> dict[str, Any]:
    if not path or len(path) > _MAX_PATH_DEPTH:
        _fail("deployment_draft_path_invalid", "Deployment edit path is invalid")
    candidate = deepcopy(document)
    current: Any = candidate
    for segment in path[:-1]:
        if isinstance(segment, str) and isinstance(current, dict) and segment in current:
            current = current[segment]
        elif (
            type(segment) is int
            and 0 <= segment <= _MAX_COLLECTION_INDEX
            and isinstance(current, list)
            and segment < len(current)
        ):
            current = current[segment]
        else:
            _fail("deployment_draft_path_invalid", "Deployment edit path does not exist")
    final = path[-1]
    if operation == "append":
        target = current.get(final) if isinstance(final, str) and isinstance(current, dict) else (
            current[final]
            if type(final) is int and isinstance(current, list) and 0 <= final < len(current)
            else None
        )
        if not isinstance(target, list):
            _fail("deployment_draft_path_invalid", "Deployment append target is not a list")
        target.append(deepcopy(value))
        return candidate
    if isinstance(final, str) and isinstance(current, dict):
        if operation == "remove":
            if final not in current:
                _fail("deployment_draft_path_invalid", "Deployment remove target does not exist")
            del current[final]
        elif operation == "set":
            current[final] = deepcopy(value)
        else:
            _fail("deployment_draft_operation_invalid", "Deployment edit operation is invalid")
    elif (
        type(final) is int
        and 0 <= final <= _MAX_COLLECTION_INDEX
        and isinstance(current, list)
        and final < len(current)
    ):
        if operation == "remove":
            del current[final]
        elif operation == "set":
            current[final] = deepcopy(value)
        else:
            _fail("deployment_draft_operation_invalid", "Deployment edit operation is invalid")
    else:
        _fail("deployment_draft_path_invalid", "Deployment edit target does not exist")
    return candidate


def _edit_controlled_stop_profile(
    document: dict[str, Any], profile: dict[str, Any]
) -> dict[str, Any]:
    """Replace one normalized operator Stop profile across derived route rows.

    Deployment compilation still validates every expanded row.  This focused
    mutation prevents the browser from editing those compiler-expanded copies
    independently or leaving them inconsistent.
    """

    expected = {
        "allow_natural_completion_on_short_path",
        "deceleration_time_s",
        "per_resource_limits",
        "stop_mode",
    }
    if not isinstance(profile, dict) or set(profile) != expected:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop profile has an invalid shape",
        )
    if profile["stop_mode"] not in {"soft_on_path", "spring_back_hold"}:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop mode is not supported by the current contract",
        )
    if type(profile["allow_natural_completion_on_short_path"]) is not bool:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop natural-completion choice must be boolean",
        )
    deceleration = profile["deceleration_time_s"]
    if (
        type(deceleration) not in {int, float}
        or not math.isfinite(float(deceleration))
        or not 0.0 < float(deceleration) <= 60.0
    ):
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop deceleration time must be in (0, 60] seconds",
        )
    limits = profile["per_resource_limits"]
    if not isinstance(limits, list) or not 1 <= len(limits) <= 256:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop requires a bounded per-resource limit set",
        )
    by_resource: dict[str, dict[str, Any]] = {}
    for row in limits:
        if not isinstance(row, dict) or set(row) != {
            "max_deceleration",
            "resource_id",
            "settle_velocity_threshold",
        }:
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop resource limit has an invalid shape",
            )
        resource_id = row["resource_id"]
        if not isinstance(resource_id, str) or not resource_id or resource_id in by_resource:
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop resource identities must be nonempty and unique",
            )
        for field in ("max_deceleration", "settle_velocity_threshold"):
            value = row[field]
            if not isinstance(value, dict) or set(value) != {
                "coordinate_kind",
                "value",
            }:
                _fail(
                    "deployment_controlled_stop_invalid",
                    f"Controlled Stop {field.replace('_', ' ')} has an invalid shape",
                )
            number = value["value"]
            if (
                not isinstance(value["coordinate_kind"], str)
                or type(number) not in {int, float}
                or not math.isfinite(float(number))
                or float(number) < 0.0
                or (field == "max_deceleration" and float(number) <= 0.0)
            ):
                _fail(
                    "deployment_controlled_stop_invalid",
                    f"Controlled Stop {field.replace('_', ' ')} is invalid",
                )
        by_resource[resource_id] = deepcopy(row)

    policies = document.get("policies")
    rows = policies.get("stop_stage_parameters") if isinstance(policies, dict) else None
    if not isinstance(rows, list):
        _fail(
            "deployment_controlled_stop_unavailable",
            "Deployment has no Controlled Stop parameter inventory",
            status_code=409,
        )
    candidate = deepcopy(document)
    candidate_rows = candidate["policies"]["stop_stage_parameters"]
    updated = 0
    expected_resource_ids = set(by_resource)
    for row in candidate_rows:
        if not isinstance(row, dict) or row.get("stop_escalation_stage_id") != "motion_controlled_stop":
            continue
        provider = row.get("provider_contract_ref")
        if not isinstance(provider, dict) or provider.get("provider_contract_role_id") != "stop_contract":
            continue
        current = row.get("default_contract_parameters")
        current_limits = current.get("per_resource_limits") if isinstance(current, dict) else None
        if not isinstance(current_limits, list) or {
            item.get("resource_id")
            for item in current_limits
            if isinstance(item, dict)
        } != expected_resource_ids:
            _fail(
                "deployment_controlled_stop_scope_mismatch",
                "Controlled Stop resource scope differs across derived route rows",
                status_code=409,
            )
        materialized_limits: list[dict[str, Any]] = []
        for current_limit in current_limits:
            resource_id = current_limit["resource_id"]
            replacement = deepcopy(by_resource[resource_id])
            for field in ("max_deceleration", "settle_velocity_threshold"):
                existing = current_limit.get(field)
                requested = replacement[field]
                if (
                    not isinstance(existing, dict)
                    or existing.get("coordinate_kind") != requested["coordinate_kind"]
                ):
                    _fail(
                        "deployment_controlled_stop_scope_mismatch",
                        "Controlled Stop coordinate kind differs from the compiled route",
                        status_code=409,
                    )
                value_field = next(
                    (name for name in existing if name.startswith("value_")),
                    None,
                )
                if value_field is None:
                    _fail(
                        "deployment_controlled_stop_scope_mismatch",
                        "Controlled Stop coordinate value field is unavailable",
                        status_code=409,
                    )
                requested_number = requested["value"]
                replacement[field] = {
                    "coordinate_kind": requested["coordinate_kind"],
                    value_field: float(requested_number),
                }
            materialized_limits.append(replacement)
        row["default_contract_parameters"] = {
            "allow_natural_completion_on_short_path": profile[
                "allow_natural_completion_on_short_path"
            ],
            "deceleration_time_s": float(deceleration),
            "per_resource_limits": materialized_limits,
            "stop_mode": profile["stop_mode"],
        }
        updated += 1
    if updated == 0:
        _fail(
            "deployment_controlled_stop_unavailable",
            "Deployment has no editable Controlled Stop stage",
            status_code=409,
        )
    return candidate


def _edit_motion_target_limit(
    document: dict[str, Any],
    *,
    controllable_component_id: str,
    command_target_id: str,
    maximum_tcp_linear_speed_mps: float,
) -> dict[str, Any]:
    """Edit one exact Deployment-owned target ceiling.

    The immutable catalog runtime policy remains untouched.  Runtime combines
    this authored ceiling with that policy conservatively after both sources
    have been independently verified.
    """

    if (
        not isinstance(controllable_component_id, str)
        or not controllable_component_id
        or not isinstance(command_target_id, str)
        or not command_target_id
        or not math.isfinite(maximum_tcp_linear_speed_mps)
        or not 0.0 < maximum_tcp_linear_speed_mps <= 1000.0
    ):
        _fail(
            "deployment_motion_target_limit_invalid",
            "Target motion limit identity or value is invalid",
        )
    candidate = deepcopy(document)
    graph = candidate.get("graph")
    runtime_units = graph.get("runtime_units") if isinstance(graph, dict) else None
    if not isinstance(runtime_units, list):
        _fail(
            "deployment_motion_target_limit_unavailable",
            "Deployment has no runtime-unit target limit inventory",
            status_code=409,
        )
    matches: list[dict[str, Any]] = []
    for runtime_unit in runtime_units:
        configuration = (
            runtime_unit.get("configuration")
            if isinstance(runtime_unit, dict)
            else None
        )
        limits = (
            configuration.get("motion_target_limits")
            if isinstance(configuration, dict)
            else None
        )
        if not isinstance(limits, list):
            continue
        for row in limits:
            if (
                isinstance(row, dict)
                and row.get("controllable_component_id")
                == controllable_component_id
                and row.get("command_target_id") == command_target_id
            ):
                matches.append(row)
    if len(matches) != 1:
        _fail(
            "deployment_motion_target_limit_scope_mismatch",
            "Deployment must contain exactly one limit for the addressed motion target",
            status_code=409,
        )
    matches[0]["maximum_tcp_linear_speed_mps"] = float(
        maximum_tcp_linear_speed_mps
    )
    return candidate


class DeploymentDraftSession:
    """Own one optional exact Deployment Draft and Saved baseline."""

    def __init__(self, *, compiler: DeploymentCompiler = compile_deployment_document) -> None:
        self._lock = threading.RLock()
        self._compiler = compiler
        self._draft_id = f"deployment-draft-{uuid.uuid4().hex}"
        self._revision = 0
        self._compiled: CompiledDeploymentDocument | None = None
        self._saved_path: str | None = None
        self._saved_document: dict[str, Any] | None = None

    @property
    def loaded(self) -> bool:
        with self._lock:
            return self._compiled is not None

    def _require_current(self, draft_id: str, expected_revision: int) -> None:
        if draft_id != self._draft_id or expected_revision != self._revision:
            _fail(
                "deployment_draft_stale",
                "Deployment Draft changed; refresh before editing",
                status_code=409,
            )

    def require_current(self, draft_id: str, expected_revision: int) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)

    def require_open_allowed(
        self,
        draft_id: str,
        expected_revision: int,
        *,
        confirm_discard: bool,
    ) -> None:
        """Fence an Open before its intent is recorded and any effect begins."""
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if (
                self._compiled is not None
                and self._compiled.document != self._saved_document
                and not confirm_discard
            ):
                _fail(
                    "deployment_discard_confirmation_required",
                    "opening another Deployment would discard Draft changes",
                    status_code=409,
                )

    def preview_source(self, source: str) -> CompiledDeploymentDocument:
        return self._compiler(source)

    def preview_update(
        self,
        path: Sequence[str | int],
        value: Any,
        operation: str = "set",
        *,
        draft_id: str,
        expected_revision: int,
    ) -> CompiledDeploymentDocument:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before editing",
                    status_code=404,
                )
            candidate = _edit_at_path(self._compiled.document, path, value, operation)
        return self._compiler(_source(candidate))

    def preview_controlled_stop_update(
        self,
        profile: dict[str, Any],
        *,
        draft_id: str,
        expected_revision: int,
    ) -> CompiledDeploymentDocument:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before editing",
                    status_code=404,
                )
            candidate = _edit_controlled_stop_profile(
                self._compiled.document, profile
            )
        return self._compiler(_source(candidate))

    def preview_motion_target_limit_update(
        self,
        *,
        controllable_component_id: str,
        command_target_id: str,
        maximum_tcp_linear_speed_mps: float,
        draft_id: str,
        expected_revision: int,
    ) -> CompiledDeploymentDocument:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before editing",
                    status_code=404,
                )
            candidate = _edit_motion_target_limit(
                self._compiled.document,
                controllable_component_id=controllable_component_id,
                command_target_id=command_target_id,
                maximum_tcp_linear_speed_mps=maximum_tcp_linear_speed_mps,
            )
        return self._compiler(_source(candidate))

    def commit_compiled(
        self,
        compiled: CompiledDeploymentDocument,
        *,
        draft_id: str,
        expected_revision: int,
    ) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if (
                self._compiled is not None
                and self._compiled.document == compiled.document
            ):
                return
            self._compiled = compiled
            self._revision += 1

    def retained_state(self) -> dict[str, Any]:
        with self._lock:
            return {
                "schema_version": _RETAINED_SCHEMA,
                "draft_id": self._draft_id,
                "draft_revision": self._revision,
                "document": (
                    deepcopy(self._compiled.document)
                    if self._compiled is not None
                    else None
                ),
                "saved_path": self._saved_path,
                "saved_document": deepcopy(self._saved_document),
            }

    def restore_retained_state(self, state: dict[str, Any]) -> None:
        expected = {
            "schema_version",
            "draft_id",
            "draft_revision",
            "document",
            "saved_path",
            "saved_document",
        }
        if not isinstance(state, dict) or set(state) != expected:
            _fail(
                "deployment_retained_draft_invalid",
                "retained Deployment Draft has an invalid shape",
                status_code=503,
            )
        if (
            state["schema_version"] != _RETAINED_SCHEMA
            or not isinstance(state["draft_id"], str)
            or not state["draft_id"]
            or type(state["draft_revision"]) is not int
            or state["draft_revision"] < 0
            or (state["saved_path"] is not None and not isinstance(state["saved_path"], str))
            or (state["saved_document"] is not None and not isinstance(state["saved_document"], dict))
        ):
            _fail(
                "deployment_retained_draft_invalid",
                "retained Deployment Draft fields are invalid",
                status_code=503,
            )
        compiled = (
            None
            if state["document"] is None
            else self._compiler(_source(state["document"]))
        )
        if state["saved_document"] is not None:
            self._compiler(_source(state["saved_document"]))
        with self._lock:
            self._draft_id = state["draft_id"]
            self._revision = state["draft_revision"]
            self._compiled = compiled
            self._saved_path = state["saved_path"]
            self._saved_document = deepcopy(state["saved_document"])

    def commit_open(
        self,
        compiled: CompiledDeploymentDocument,
        source_path: str,
        *,
        draft_id: str,
        expected_revision: int,
        confirm_discard: bool,
    ) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            # The coordinator called require_open_allowed while holding the
            # authoring claim. Retain the argument in this boundary so callers
            # cannot accidentally bypass that explicit discard decision.
            if not confirm_discard and self._compiled is not None:
                if self._compiled.document != self._saved_document:
                    _fail(
                        "deployment_discard_confirmation_required",
                        "opening another Deployment would discard Draft changes",
                        status_code=409,
                    )
            resolved_source_path = str(Path(source_path).resolve())
            if (
                self._compiled is not None
                and self._compiled.document == compiled.document
                and self._saved_document == compiled.document
                and self._saved_path == resolved_source_path
            ):
                return
            self._compiled = compiled
            self._saved_path = resolved_source_path
            self._saved_document = deepcopy(compiled.document)
            self._revision += 1

    def mark_saved(
        self, path: str, *, draft_id: str, expected_revision: int
    ) -> None:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before saving",
                    status_code=404,
                )
            self._saved_path = str(Path(path).resolve())
            self._saved_document = deepcopy(self._compiled.document)

    def canonical_json(self, *, draft_id: str, expected_revision: int) -> str:
        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before saving",
                    status_code=404,
                )
            return self._compiled.canonical_json

    def snapshot(self) -> dict[str, Any]:
        with self._lock:
            compiled = self._compiled
            document = deepcopy(compiled.document) if compiled is not None else None
            return {
                "schemaVersion": "manyforge.composer_deployment_workspace.v1",
                "draft": {
                    "id": self._draft_id,
                    "revision": self._revision,
                    "document": document,
                    "deploymentArtifactId": (
                        compiled.deployment_artifact_id if compiled is not None else None
                    ),
                    "deploymentRevision": (
                        compiled.deployment_revision if compiled is not None else None
                    ),
                    "graphRevision": compiled.graph_revision if compiled is not None else None,
                    "differsFromSaved": (
                        document != self._saved_document if document is not None else False
                    ),
                },
                "saved": {
                    "path": self._saved_path,
                    "deploymentRevision": (
                        self._saved_document.get("deployment_revision")
                        if self._saved_document is not None
                        else None
                    ),
                },
            }
