"""Composer-owned, revision-fenced Deployment document Draft."""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import json
import math
from pathlib import Path
import threading
from typing import Any, Callable, Mapping, NoReturn, Sequence
import uuid

from manyforge_behavior.canonical_artifact import (
    StrictArtifactError,
    canonical_json_bytes,
    load_strict_json_yaml_bytes,
)

from .deployment_intent import (
    DEPLOYMENT_INTENT_API_VERSION,
    DeploymentIntentError,
    validate_deployment_intent,
)
from .authoring_identity import freeze_json, thaw_json
from .stop_fallback_sequence import (
    FALLBACK_STOP_MODES_FIELD,
    fallback_candidates,
    fallback_sequence_error,
    recorded_fallback_stop_modes,
)


_MAX_DOCUMENT_BYTES = 16 * 1024 * 1024
_MAX_PATH_DEPTH = 32
_MAX_COLLECTION_INDEX = 100_000
_RETAINED_SCHEMA = "manyforge.composer_deployment_draft_state.v2"


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
    deployment_revision: str | None
    graph_revision: str | None


DeploymentCompiler = Callable[[str], CompiledDeploymentDocument]


@dataclass(frozen=True, slots=True)
class PreparedDeploymentReplacement:
    """One successor Deployment Draft state prepared outside every owner lock.

    ``successor_draft_id``/``successor_revision`` are explicit for Load, which
    adopts the retained identities; ``None`` keeps the current identity and
    advances the current revision by one (Open). ``preserve_saved`` keeps the
    Saved baseline, which only whole-project Load needs to replace explicitly.
    """

    kind: str
    compiled: CompiledDeploymentDocument | None
    successor_draft_id: str | None
    successor_revision: int | None
    saved_path: str | None
    saved_document: Mapping[str, Any] | None
    source_project_root: str | None
    preserve_saved: bool = False

    def __post_init__(self) -> None:
        if self.compiled is not None:
            object.__setattr__(
                self,
                "compiled",
                CompiledDeploymentDocument(
                    document=freeze_json(self.compiled.document),
                    canonical_json=self.compiled.canonical_json,
                    deployment_artifact_id=self.compiled.deployment_artifact_id,
                    deployment_revision=self.compiled.deployment_revision,
                    graph_revision=self.compiled.graph_revision,
                ),
            )
        object.__setattr__(
            self,
            "saved_document",
            (
                None
                if self.saved_document is None
                else freeze_json(self.saved_document)
            ),
        )


@dataclass(frozen=True, slots=True)
class PreparedDeploymentSave:
    """One Deployment Save/Save As prepared outside every owner lock.

    ``canonical_json`` is the exact bytes a writer persists and the Saved
    baseline adopted afterwards is the same document, so the file and the
    baseline cannot describe two different Drafts.
    """

    tool_id: str
    draft_id: str
    base_revision: int
    target_path: str
    resolved_target: str
    overwrite: bool
    canonical_json: str


def validate_deployment_draft_document(source: str) -> CompiledDeploymentDocument:
    """Validate one authored intent or an unmigrated compiled artifact.

    Source-referenced intent is the authoritative UA2A authoring document.  The
    compiled branch is a temporary adapter for standalone Deployment consumers
    that have not yet moved to Scenario projects; it is not used as a fallback
    if intent validation fails.
    """

    encoded = source.encode("utf-8")
    if not encoded or len(encoded) > _MAX_DOCUMENT_BYTES:
        _fail(
            "deployment_draft_oversized",
            "Deployment Draft exceeds the artifact byte bound",
            status_code=413,
        )
    try:
        value = load_strict_json_yaml_bytes(encoded, source="<Deployment Draft>")
    except StrictArtifactError as error:
        _fail("deployment_draft_invalid", str(error))
    if type(value) is dict and value.get("api_version") == DEPLOYMENT_INTENT_API_VERSION:
        try:
            document = validate_deployment_intent(
                value, source="<Deployment Draft>"
            )
            canonical = canonical_json_bytes(document).decode("utf-8")
        except (DeploymentIntentError, StrictArtifactError) as error:
            _fail("deployment_draft_invalid", str(error))
        return CompiledDeploymentDocument(
            document=document,
            canonical_json=canonical,
            deployment_artifact_id=document["deployment_artifact_id"],
            deployment_revision=None,
            graph_revision=None,
        )
    return compile_deployment_document(source)


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
    if (
        not isinstance(canonical, str)
        or len(canonical.encode("utf-8")) > _MAX_DOCUMENT_BYTES
    ):
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
        if (
            isinstance(segment, str)
            and isinstance(current, dict)
            and segment in current
        ):
            current = current[segment]
        elif (
            type(segment) is int
            and 0 <= segment <= _MAX_COLLECTION_INDEX
            and isinstance(current, list)
            and segment < len(current)
        ):
            current = current[segment]
        else:
            _fail(
                "deployment_draft_path_invalid", "Deployment edit path does not exist"
            )
    final = path[-1]
    if operation == "append":
        target = (
            current.get(final)
            if isinstance(final, str) and isinstance(current, dict)
            else (
                current[final]
                if type(final) is int
                and isinstance(current, list)
                and 0 <= final < len(current)
                else None
            )
        )
        if not isinstance(target, list):
            _fail(
                "deployment_draft_path_invalid",
                "Deployment append target is not a list",
            )
        target.append(deepcopy(value))
        return candidate
    if isinstance(final, str) and isinstance(current, dict):
        if operation == "remove":
            if final not in current:
                _fail(
                    "deployment_draft_path_invalid",
                    "Deployment remove target does not exist",
                )
            del current[final]
        elif operation == "set":
            current[final] = deepcopy(value)
        else:
            _fail(
                "deployment_draft_operation_invalid",
                "Deployment edit operation is invalid",
            )
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
            _fail(
                "deployment_draft_operation_invalid",
                "Deployment edit operation is invalid",
            )
    else:
        _fail("deployment_draft_path_invalid", "Deployment edit target does not exist")
    return candidate


def _edit_compiled_controlled_stop_profile(
    document: dict[str, Any],
    profile: dict[str, Any] | None,
    *,
    control_binding_id: str,
    control_binding_revision: str,
) -> dict[str, Any]:
    """Edit one exact binding's Stop profile across its derived routes.

    Deployment compilation still validates every expanded row.  This focused
    mutation prevents the browser from editing those compiler-expanded copies
    independently or leaving them inconsistent.
    """

    graph = document.get("graph", {})
    bindings = graph.get("control_bindings", []) if isinstance(graph, dict) else []
    selected = [
        row
        for row in bindings
        if isinstance(row, dict) and row.get("control_binding_id") == control_binding_id
    ]
    if (
        len(selected) != 1
        or selected[0].get("control_binding_revision") != control_binding_revision
    ):
        _fail(
            "deployment_controlled_stop_scope_mismatch",
            "Controlled Stop target no longer matches this Deployment Draft",
            status_code=409,
        )
    if profile is None:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop profile is required",
        )

    expected = {
        "allow_natural_completion_on_short_path",
        "jerk_limits_enabled",
        "deceleration_time_s",
        "per_resource_limits",
        "stop_mode",
    }
    if (
        not isinstance(profile, dict)
        or not expected <= set(profile)
        or not set(profile)
        <= expected | {"max_stop_time_s", FALLBACK_STOP_MODES_FIELD}
    ):
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop profile has an invalid shape",
        )
    if FALLBACK_STOP_MODES_FIELD in profile:
        fallback_error = fallback_sequence_error(
            profile["stop_mode"], profile[FALLBACK_STOP_MODES_FIELD]
        )
        if fallback_error is not None:
            _fail(
                "deployment_controlled_stop_invalid",
                f"Controlled Stop {FALLBACK_STOP_MODES_FIELD} {fallback_error}",
            )
    max_stop_time = profile.get("max_stop_time_s")
    if max_stop_time is not None and (
        type(max_stop_time) not in {int, float}
        or not math.isfinite(float(max_stop_time))
        or float(max_stop_time) <= 0.0
    ):
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop maximum Stop time must be positive and finite",
        )
    if profile["stop_mode"] not in {"soft_on_path", "spring_back_hold", "spring_back_fast"}:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop mode is not supported by the current contract",
        )
    if type(profile["allow_natural_completion_on_short_path"]) is not bool:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop natural-completion choice must be boolean",
        )
    if type(profile["jerk_limits_enabled"]) is not bool:
        _fail(
            "deployment_controlled_stop_invalid",
            "Choose explicitly whether Controlled Stop enforces jerk limits",
        )
    deceleration = profile["deceleration_time_s"]
    if (
        type(deceleration) not in {int, float}
        or not math.isfinite(float(deceleration))
        or not 0.0 < float(deceleration) <= 2.0
    ):
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop deceleration time must be in (0, 2] seconds",
        )
    limits = profile["per_resource_limits"]
    if not isinstance(limits, list) or not 1 <= len(limits) <= 256:
        _fail(
            "deployment_controlled_stop_invalid",
            "Controlled Stop requires a bounded per-resource limit set",
        )
    by_resource: dict[str, dict[str, Any]] = {}
    for row in limits:
        required_fields = {
            "max_deceleration",
            "resource_id",
            "settle_velocity_threshold",
        }
        if (
            not isinstance(row, dict)
            or not required_fields <= set(row)
            or not set(row) <= required_fields | {"max_jerk"}
        ):
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop resource limit has an invalid shape",
            )
        resource_id = row["resource_id"]
        if (
            not isinstance(resource_id, str)
            or not resource_id
            or resource_id in by_resource
        ):
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop resource identities must be nonempty and unique",
            )
        for field in ("max_deceleration", "settle_velocity_threshold", "max_jerk"):
            if field not in row:
                continue
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
                or (field != "settle_velocity_threshold" and float(number) <= 0.0)
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
        if (
            not isinstance(row, dict)
            or row.get("control_binding_id") != control_binding_id
            or row.get("stop_escalation_stage_id") != "motion_controlled_stop"
        ):
            continue
        if row.get("control_binding_revision") != control_binding_revision:
            _fail(
                "deployment_controlled_stop_scope_mismatch",
                "Controlled Stop route has a stale binding revision",
                status_code=409,
            )
        provider = row.get("provider_contract_ref")
        if (
            not isinstance(provider, dict)
            or provider.get("provider_contract_role_id") != "stop_contract"
        ):
            continue
        current = row.get("default_contract_parameters")
        current_limits = (
            current.get("per_resource_limits") if isinstance(current, dict) else None
        )
        if (
            not isinstance(current_limits, list)
            or {
                item.get("resource_id")
                for item in current_limits
                if isinstance(item, dict)
            }
            != expected_resource_ids
        ):
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
            if "max_jerk" in replacement:
                kind, value_field = {
                    "angular_rad_s2": ("angular_rad_s3", "value_rad_s3"),
                    "linear_m_s2": ("linear_m_s3", "value_m_s3"),
                }.get(replacement["max_deceleration"]["coordinate_kind"], (None, None))
                requested = replacement["max_jerk"]
                if kind is None or requested["coordinate_kind"] != kind:
                    _fail(
                        "deployment_controlled_stop_scope_mismatch",
                        "Jerk units differ from the compiled resource",
                        status_code=409,
                    )
                replacement["max_jerk"] = {
                    "coordinate_kind": kind,
                    value_field: float(requested["value"]),
                }
            materialized_limits.append(replacement)
        row["default_contract_parameters"] = {
            "allow_natural_completion_on_short_path": profile[
                "allow_natural_completion_on_short_path"
            ],
            "deceleration_time_s": float(deceleration),
            "jerk_limits_enabled": profile["jerk_limits_enabled"],
            "per_resource_limits": materialized_limits,
            "stop_mode": profile["stop_mode"],
        }
        if max_stop_time is not None:
            row["default_contract_parameters"]["max_stop_time_s"] = float(max_stop_time)
        # Fallback sequence (stop_fallback_sequence.py).  Writes are canonical:
        # only a non-empty `fallback_stop_modes`; a legacy Soft substitution
        # is normalized to [spring_back_hold].  A profile without the field
        # keeps the recorded sequence, minus modes no longer below the primary.
        try:
            recorded = recorded_fallback_stop_modes(current)
        except ValueError as error:
            _fail("deployment_controlled_stop_invalid", str(error))
        if FALLBACK_STOP_MODES_FIELD in profile:
            fallbacks = list(profile[FALLBACK_STOP_MODES_FIELD])
        else:
            below = fallback_candidates(profile["stop_mode"])
            fallbacks = [mode for mode in recorded if mode in below]
        if fallbacks:
            row["default_contract_parameters"][FALLBACK_STOP_MODES_FIELD] = fallbacks
        updated += 1
    if updated == 0:
        _fail(
            "deployment_controlled_stop_unavailable",
            "Deployment has no editable Controlled Stop stage",
            status_code=409,
        )
    return candidate


def _edit_controlled_stop_profile(
    document: dict[str, Any],
    profile: dict[str, Any] | None,
    *,
    controllable_component_id: str,
    command_target_id: str,
) -> dict[str, Any]:
    """Edit one authored target and let preparation expand its route copies."""

    if document.get("api_version") == DEPLOYMENT_INTENT_API_VERSION:
        candidate = deepcopy(document)
        matches: list[dict[str, Any]] = []
        for component in candidate.get("components", []):
            if (
                isinstance(component, dict)
                and component.get("component_id") == controllable_component_id
                and isinstance(component.get("target_settings"), dict)
                and isinstance(
                    component["target_settings"].get(command_target_id), dict
                )
            ):
                matches.append(component["target_settings"][command_target_id])
        if len(matches) != 1 or not isinstance(matches[0].get("controlled_stop"), dict):
            _fail(
                "deployment_controlled_stop_scope_mismatch",
                "Controlled Stop target no longer matches this Deployment Draft",
                status_code=409,
            )
        if profile is None:
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop profile is required",
            )
        expected = {
            "allow_natural_completion_on_short_path",
            "jerk_limits_enabled",
            "deceleration_time_s",
            "per_resource_limits",
            "stop_mode",
        }
        if (
            not isinstance(profile, dict)
            or not expected <= set(profile)
            or not set(profile)
            <= expected | {"max_stop_time_s", FALLBACK_STOP_MODES_FIELD}
        ):
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop profile has an invalid shape",
            )
        limits = profile["per_resource_limits"]
        if not isinstance(limits, list):
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop resource limits are malformed",
            )
        authored_limits: list[dict[str, Any]] = []
        for row in limits:
            if not isinstance(row, dict):
                _fail(
                    "deployment_controlled_stop_invalid",
                    "Controlled Stop resource limit is malformed",
                )
            maximum = row.get("max_deceleration")
            threshold = row.get("settle_velocity_threshold")
            jerk = row.get("max_jerk")
            if (
                not isinstance(row.get("resource_id"), str)
                or not isinstance(maximum, dict)
                or maximum.get("coordinate_kind") != "angular_rad_s2"
                or not isinstance(threshold, dict)
                or threshold.get("coordinate_kind") != "angular_rad_s"
                or (jerk is not None and (
                    not isinstance(jerk, dict)
                    or jerk.get("coordinate_kind") != "angular_rad_s3"
                ))
            ):
                _fail(
                    "deployment_controlled_stop_invalid",
                    "Authored Stop limits must use source-joint angular units",
                )
            authored = {
                "source_joint_name": row["resource_id"],
                "max_deceleration_rad_s2": maximum.get("value"),
                "settle_velocity_threshold_rad_s": threshold.get("value"),
            }
            if jerk is not None:
                authored["max_jerk_rad_s3"] = jerk.get("value")
            authored_limits.append(authored)
        current = {
            "stop_mode": profile["stop_mode"],
            "allow_natural_completion_on_short_path": profile[
                "allow_natural_completion_on_short_path"
            ],
            "jerk_limits_enabled": profile["jerk_limits_enabled"],
            "deceleration_time_s": profile["deceleration_time_s"],
            "per_joint_limits": authored_limits,
        }
        if "max_stop_time_s" in profile:
            current["max_stop_time_s"] = profile["max_stop_time_s"]
        # Validated (fixed order, strictly below stop_mode) by the intent
        # validator below; a stale entry after a mode change is refused there.
        if FALLBACK_STOP_MODES_FIELD in profile:
            current[FALLBACK_STOP_MODES_FIELD] = profile[FALLBACK_STOP_MODES_FIELD]
        matches[0]["controlled_stop"] = current
        try:
            return validate_deployment_intent(
                candidate, source="<Deployment Draft mutation>"
            )
        except DeploymentIntentError as error:
            _fail("deployment_controlled_stop_invalid", str(error))

    graph = document.get("graph")
    bindings = graph.get("control_bindings") if isinstance(graph, dict) else None
    if not isinstance(bindings, list):
        _fail(
            "deployment_controlled_stop_unavailable",
            "Deployment has no editable Controlled Stop bindings",
            status_code=409,
        )
    selected = [
        row
        for row in bindings
        if isinstance(row, dict)
        and row.get("controllable_component_id") == controllable_component_id
        and row.get("command_target_id") == command_target_id
    ]
    if not selected:
        _fail(
            "deployment_controlled_stop_scope_mismatch",
            "Controlled Stop target no longer matches this Deployment Draft",
            status_code=409,
        )
    candidate = document
    for binding in selected:
        binding_id = binding.get("control_binding_id")
        binding_revision = binding.get("control_binding_revision")
        if not isinstance(binding_id, str) or not isinstance(binding_revision, str):
            _fail(
                "deployment_controlled_stop_invalid",
                "Controlled Stop binding identity is malformed",
            )
        candidate = _edit_compiled_controlled_stop_profile(
            candidate,
            profile,
            control_binding_id=binding_id,
            control_binding_revision=binding_revision,
        )
    return candidate


def _edit_motion_target_limit(
    document: dict[str, Any],
    *,
    controllable_component_id: str,
    command_target_id: str,
    maximum_tcp_linear_speed_mps: float,
    tcp_speed_check: str | None = None,
) -> dict[str, Any]:
    """Edit one exact Deployment-owned target ceiling.

    This authored ceiling is the target's sole configured TCP-speed maximum.
    `tcp_speed_check` (None keeps the stored value) chooses the points where
    that ceiling is held: the active TCP, or the active TCP and the flange.
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
    if candidate.get("api_version") == DEPLOYMENT_INTENT_API_VERSION:
        components = candidate.get("components")
        if not isinstance(components, list):
            _fail(
                "deployment_motion_target_limit_unavailable",
                "Deployment intent has no component inventory",
                status_code=409,
            )
        matches: list[dict[str, Any]] = []
        for component in components:
            if (
                not isinstance(component, dict)
                or component.get("component_id") != controllable_component_id
            ):
                continue
            settings = component.get("target_settings")
            if isinstance(settings, dict):
                target = settings.get(command_target_id)
                if isinstance(target, dict):
                    matches.append(target)
        if len(matches) != 1:
            _fail(
                "deployment_motion_target_limit_scope_mismatch",
                "Deployment intent must contain exactly one addressed motion target",
                status_code=409,
            )
        matches[0]["maximum_tcp_linear_speed_mps"] = float(
            maximum_tcp_linear_speed_mps
        )
        if tcp_speed_check is not None:
            matches[0]["tcp_speed_check"] = tcp_speed_check
        try:
            return validate_deployment_intent(
                candidate, source="<Deployment Draft mutation>"
            )
        except DeploymentIntentError as error:
            _fail("deployment_draft_invalid", str(error))
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
                and row.get("controllable_component_id") == controllable_component_id
                and row.get("command_target_id") == command_target_id
            ):
                matches.append(row)
    if len(matches) != 1:
        _fail(
            "deployment_motion_target_limit_scope_mismatch",
            "Deployment must contain exactly one limit for the addressed motion target",
            status_code=409,
        )
    matches[0]["maximum_tcp_linear_speed_mps"] = float(maximum_tcp_linear_speed_mps)
    if tcp_speed_check is not None:
        matches[0]["tcp_speed_check"] = tcp_speed_check
    return candidate


class DeploymentDraftSession:
    """Own one optional exact Deployment Draft and Saved baseline."""

    def __init__(
        self, *, compiler: DeploymentCompiler = validate_deployment_draft_document
    ) -> None:
        self._lock = threading.RLock()
        self._compiler = compiler
        self._draft_id = f"deployment-draft-{uuid.uuid4().hex}"
        self._revision = 0
        self._compiled: CompiledDeploymentDocument | None = None
        self._saved_path: str | None = None
        self._saved_document: dict[str, Any] | None = None
        self._source_project_root: str | None = None

    @property
    def draft_id(self) -> str:
        with self._lock:
            return self._draft_id

    @property
    def revision(self) -> int:
        with self._lock:
            return self._revision

    @property
    def loaded(self) -> bool:
        with self._lock:
            return self._compiled is not None

    @property
    def source_project_root(self) -> str | None:
        """Directory against which this Draft's project-relative assets resolve."""

        with self._lock:
            return self._source_project_root

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

    def preview_component_addition(
        self,
        components: Sequence[Any],
        attachments: Sequence[Any],
        *,
        draft_id: str,
        expected_revision: int,
    ) -> CompiledDeploymentDocument:
        """Append component rows and their attachment rows (Add component)."""

        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before editing",
                    status_code=404,
                )
            if self._compiled.document.get("api_version") != DEPLOYMENT_INTENT_API_VERSION:
                _fail(
                    "deployment_component_addition_unavailable",
                    "Add component needs a source-referenced Deployment intent",
                )
            candidate = deepcopy(self._compiled.document)
            candidate["components"] = [*candidate["components"], *deepcopy(list(components))]
            candidate["attachments"] = [*candidate["attachments"], *deepcopy(list(attachments))]
        return self._compiler(_source(candidate))

    def preview_controlled_stop_update(
        self,
        profile: dict[str, Any] | None,
        *,
        controllable_component_id: str,
        command_target_id: str,
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
                self._compiled.document,
                profile,
                controllable_component_id=controllable_component_id,
                command_target_id=command_target_id,
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
        tcp_speed_check: str | None = None,
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
                tcp_speed_check=tcp_speed_check,
            )
        return self._compiler(_source(candidate))

    def commit_compiled(
        self,
        compiled: CompiledDeploymentDocument,
        *,
        draft_id: str,
        expected_revision: int,
    ) -> bool:
        """Install one already-prepared compiled document under the Draft fence.

        Returns whether the Draft changed, so the composite workspace owner can
        commit the same value atomically.
        """

        with self._lock:
            self._require_current(draft_id, expected_revision)
            if (
                self._compiled is not None
                and self._compiled.document == compiled.document
            ):
                return False
            self._compiled = compiled
            self._revision += 1
            return True

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
        self.install_prepared_replacement(self.prepare_load_replacement(state))

    def prepare_load_replacement(
        self,
        state: dict[str, Any],
        *,
        source_project_root: str | None = None,
    ) -> PreparedDeploymentReplacement:
        """Validate one retained Deployment Draft state without installing it.

        Whole-project Load takes this prepared value, so a route no longer calls
        :meth:`restore_retained_state` as a general mutation primitive.
        """

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
            or (
                state["saved_path"] is not None
                and not isinstance(state["saved_path"], str)
            )
            or (
                state["saved_document"] is not None
                and not isinstance(state["saved_document"], dict)
            )
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
        if source_project_root is None and isinstance(state["saved_path"], str):
            saved_path = Path(state["saved_path"]).resolve()
            source_project_root = str(
                saved_path if saved_path.is_dir() else saved_path.parent
            )
        elif source_project_root is not None:
            source_project_root = str(Path(source_project_root).resolve())
        return PreparedDeploymentReplacement(
            kind="load",
            compiled=compiled,
            successor_draft_id=state["draft_id"],
            successor_revision=state["draft_revision"],
            saved_path=state["saved_path"],
            saved_document=deepcopy(state["saved_document"]),
            source_project_root=source_project_root,
        )

    def prepare_open_replacement(
        self,
        compiled: CompiledDeploymentDocument,
        source_path: str,
    ) -> PreparedDeploymentReplacement:
        """Prepare one Open replacement from an independently safe read."""

        resolved_source_path = str(Path(source_path).resolve())
        return PreparedDeploymentReplacement(
            kind="open",
            compiled=compiled,
            successor_draft_id=None,
            successor_revision=None,
            saved_path=resolved_source_path,
            saved_document=deepcopy(compiled.document),
            source_project_root=str(Path(resolved_source_path).parent),
        )

    def install_prepared_replacement(
        self,
        prepared: PreparedDeploymentReplacement,
        *,
        expected_draft_id: str | None = None,
        expected_revision: int | None = None,
        before_install: Callable[[], None] | None = None,
    ) -> bool:
        """Install one prepared replacement after rechecking its exact fence.

        Returns whether a real change was installed. A replacement without an
        explicit successor revision advances the current revision by one;
        installing content that already matches returns a validated no-op.
        """

        if not isinstance(prepared, PreparedDeploymentReplacement):
            raise TypeError("prepared Deployment replacement has an invalid type")
        if (expected_draft_id is None) != (expected_revision is None):
            raise ValueError("a prepared replacement fence needs both fields")
        with self._lock:
            if expected_draft_id is not None:
                self._require_current(expected_draft_id, expected_revision)
            if before_install is not None:
                before_install()
            successor_draft_id = prepared.successor_draft_id or self._draft_id
            successor_saved_path = (
                self._saved_path if prepared.preserve_saved else prepared.saved_path
            )
            successor_saved_document = (
                self._saved_document
                if prepared.preserve_saved
                else (
                    None
                    if prepared.saved_document is None
                    else thaw_json(prepared.saved_document)
                )
            )
            successor_document = (
                thaw_json(prepared.compiled.document)
                if prepared.compiled is not None
                else None
            )
            current_document = (
                self._compiled.document if self._compiled is not None else None
            )
            unchanged = (
                successor_draft_id == self._draft_id
                and successor_document == current_document
                and successor_saved_path == self._saved_path
                and successor_saved_document == self._saved_document
                and (
                    prepared.preserve_saved
                    or prepared.source_project_root == self._source_project_root
                )
            )
            successor_revision = prepared.successor_revision
            if successor_revision is None:
                if unchanged:
                    return False
                successor_revision = self._revision + 1
            elif unchanged and successor_revision == self._revision:
                return False
            self._draft_id = successor_draft_id
            self._revision = successor_revision
            self._compiled = (
                None
                if prepared.compiled is None
                else CompiledDeploymentDocument(
                    document=successor_document,
                    canonical_json=prepared.compiled.canonical_json,
                    deployment_artifact_id=prepared.compiled.deployment_artifact_id,
                    deployment_revision=prepared.compiled.deployment_revision,
                    graph_revision=prepared.compiled.graph_revision,
                )
            )
            if not prepared.preserve_saved:
                self._saved_path = prepared.saved_path
                self._saved_document = successor_saved_document
                self._source_project_root = prepared.source_project_root
            return True

    def commit_open(
        self,
        compiled: CompiledDeploymentDocument,
        source_path: str,
        *,
        draft_id: str,
        expected_revision: int,
        confirm_discard: bool,
    ) -> bool:
        """Install one prepared Open replacement under the Draft fence."""

        return self.install_prepared_replacement(
            self.prepare_open_replacement(compiled, source_path),
            expected_draft_id=draft_id,
            expected_revision=expected_revision,
            before_install=lambda: self._require_open_confirmed_locked(
                confirm_discard
            ),
        )

    def _require_open_confirmed_locked(self, confirm_discard: bool) -> None:
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

    def prepare_save(
        self,
        *,
        draft_id: str,
        expected_revision: int,
        target_path: str | None,
        confirm_overwrite: bool,
    ) -> PreparedDeploymentSave:
        """Capture the exact content and target for one Save/Save As.

        Save names no target and keeps its existing authority; Save As resolves
        its own target and only overwrites an existing file after an explicit
        confirmation or when it names the current Saved path. The write happens
        outside every owner lock; only the returned Saved baseline is committed
        through the composite workspace fence.
        """

        with self._lock:
            self._require_current(draft_id, expected_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before saving",
                    status_code=404,
                )
            selected_path = target_path or self._saved_path
            if selected_path is None:
                _fail(
                    "deployment_saved_path_unavailable",
                    "Save requires an existing path; use Save As",
                    status_code=409,
                )
            try:
                resolved_target = str(Path(selected_path).resolve())
            except (OSError, RuntimeError, ValueError):
                resolved_target = selected_path
            return PreparedDeploymentSave(
                tool_id=(
                    "save_deployment_as"
                    if target_path is not None
                    else "save_deployment"
                ),
                draft_id=self._draft_id,
                base_revision=self._revision,
                target_path=selected_path,
                resolved_target=resolved_target,
                overwrite=confirm_overwrite or resolved_target == self._saved_path,
                canonical_json=self._compiled.canonical_json,
            )

    def install_prepared_save(
        self, prepared: PreparedDeploymentSave, *, saved_path: str
    ) -> bool:
        """Adopt one already-written Deployment artifact as the Saved baseline.

        Returns whether the Saved baseline changed. No Draft revision advances,
        because the Draft content is unchanged; the composite workspace revision
        still advances whenever the baseline changed.
        """

        if not isinstance(prepared, PreparedDeploymentSave):
            raise TypeError("prepared Deployment save has an invalid type")
        try:
            resolved = str(Path(saved_path).resolve())
        except (OSError, RuntimeError, ValueError):
            resolved = saved_path
        with self._lock:
            self._require_current(prepared.draft_id, prepared.base_revision)
            if self._compiled is None:
                _fail(
                    "deployment_draft_unavailable",
                    "open a Deployment artifact before saving",
                    status_code=404,
                )
            document = deepcopy(self._compiled.document)
            if self._saved_path == resolved and self._saved_document == document:
                return False
            self._saved_path = resolved
            self._saved_document = document
            return True

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
                        compiled.deployment_artifact_id
                        if compiled is not None
                        else None
                    ),
                    "deploymentRevision": (
                        compiled.deployment_revision if compiled is not None else None
                    ),
                    "graphRevision": (
                        compiled.graph_revision if compiled is not None else None
                    ),
                    "differsFromSaved": (
                        document != self._saved_document
                        if document is not None
                        else False
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
