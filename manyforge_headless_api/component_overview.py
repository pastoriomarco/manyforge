"""Fixed presentation reduction over identified runtime-owner projections.

The Overview deliberately owns no health or execution state.  It copies the
current projections, verifies their shared retained coordinates, and reduces
them into the small operator vocabulary used by Composer and later Component
Manager clients.
"""

from __future__ import annotations

from collections.abc import Mapping
import hashlib
from typing import Any

import rfc8785


COMPONENT_OVERVIEW_VERSION = "manyforge.component_overview.v1"

_HEALTHY = "healthy"
_WAITING = "waiting"
_WARNING = "warning"
_FAULT = "fault"
_OFFLINE = "offline"


def _mapping(value: object) -> Mapping[str, Any]:
    return value if isinstance(value, Mapping) else {}


def _sequence(value: object) -> list[Any]:
    return list(value) if isinstance(value, (list, tuple)) else []


def _bounded_reason(value: object, fallback: str) -> str:
    if isinstance(value, str) and value:
        return value[:256]
    return fallback


def _source_status(source: Mapping[str, Any]) -> tuple[str, str | None]:
    if source.get("available") is not True:
        return _OFFLINE, "source_unavailable"
    freshness = source.get("freshness")
    if freshness == "stale":
        return _WAITING, "source_stale"
    if freshness != "fresh":
        return _OFFLINE, "source_freshness_unknown"
    gaps = _sequence(source.get("known_gaps"))
    if gaps:
        return _WARNING, _bounded_reason(gaps[0], "source_gap")
    return _HEALTHY, None


def _error_status(error: Mapping[str, Any]) -> str:
    code = str(error.get("code", ""))
    if any(token in code for token in ("unavailable", "not_found", "not_configured")):
        return _OFFLINE
    return _FAULT


def _component(
    component_id: str,
    label: str,
    status: str,
    detail: str,
    *,
    required: bool,
    required_for: tuple[str, ...],
    owner_id: object = None,
    owner_epoch: object = None,
    generation: object = None,
    reason_code: object = None,
    scope: str,
    contributor_ids: tuple[str, ...] = (),
) -> dict[str, Any]:
    return {
        "id": component_id,
        "label": label,
        "status": status,
        "detail": detail,
        "required": required,
        "required_for": list(required_for),
        "owner_id": owner_id if isinstance(owner_id, str) and owner_id else None,
        "owner_epoch": (
            owner_epoch if isinstance(owner_epoch, str) and owner_epoch else None
        ),
        "generation": (
            generation
            if isinstance(generation, int)
            and not isinstance(generation, bool)
            and generation >= 0
            else None
        ),
        "reason_code": (
            reason_code if isinstance(reason_code, str) and reason_code else None
        ),
        "scope": scope,
        "contributor_ids": list(contributor_ids),
    }


def _response_payload(
    response: Mapping[str, Any],
    field: str,
) -> tuple[Mapping[str, Any], Mapping[str, Any] | None]:
    if response.get("ok") is not True:
        error = _mapping(response.get("error"))
        return {}, {
            "code": _bounded_reason(error.get("code"), "owner_response_rejected"),
            "message": _bounded_reason(
                error.get("message"), "owner rejected the focused projection"
            ),
        }
    payload = response.get(field)
    if not isinstance(payload, Mapping):
        return {}, {
            "code": "owner_projection_invalid",
            "message": f"focused owner response has no {field} projection",
        }
    return payload, None


def _fact_error(
    errors: Mapping[str, Mapping[str, Any]],
    fact: str,
) -> Mapping[str, Any]:
    return _mapping(errors.get(fact))


def _scope_status(
    components_by_id: Mapping[str, Mapping[str, Any]],
    contributors: tuple[str, ...],
) -> str:
    statuses = [
        str(components_by_id[component_id]["status"])
        for component_id in contributors
        if component_id in components_by_id
    ]
    if not statuses:
        return _OFFLINE
    for candidate in (_FAULT, _OFFLINE, _WAITING, _WARNING):
        if candidate in statuses:
            return candidate
    return _HEALTHY


def build_component_overview(
    *,
    runtime: Mapping[str, Any] | None,
    alarms: Mapping[str, Any] | None,
    authoring_admission: Mapping[str, Any] | None,
    program_cycle: Mapping[str, Any] | None,
    applied_program: Mapping[str, Any] | None,
    source_model: Mapping[str, Any] | None,
    execution_identity_response: Mapping[str, Any] | None,
    execution_targets_response: Mapping[str, Any] | None,
    execution_control_response: Mapping[str, Any] | None,
    errors: Mapping[str, Mapping[str, Any]] | None = None,
) -> dict[str, Any]:
    """Build one bounded, read-only Component Overview projection.

    Inputs remain the authoritative owner snapshots.  This function performs
    only schema-safe selection, exact coordinate comparison, and a fixed
    presentation reduction; it never fills in a missing capability.
    """

    errors = errors or {}
    runtime = _mapping(runtime)
    alarms = _mapping(alarms)
    authoring_admission = _mapping(authoring_admission)
    program_cycle = _mapping(program_cycle)
    applied_program = _mapping(applied_program)
    source_model = _mapping(source_model)
    execution_identity_response = _mapping(execution_identity_response)
    execution_targets_response = _mapping(execution_targets_response)
    execution_control_response = _mapping(execution_control_response)

    components: list[dict[str, Any]] = []
    sections = _mapping(runtime.get("sections"))
    manager_section = _mapping(sections.get("manager"))
    manager_source_status, manager_source_reason = _source_status(manager_section)
    manager_payload = _mapping(manager_section.get("payload"))
    manager_runtime = _mapping(manager_payload.get("runtime"))
    active = _mapping(manager_payload.get("active"))
    active_graph = _mapping(active.get("cell_graph"))
    active_graph_revision = active.get("graph_revision")
    active_bundle_id = active.get("retained_bundle_id")
    manager_status = manager_source_status
    manager_reason = manager_source_reason
    if _fact_error(errors, "runtime"):
        manager_status = _error_status(_fact_error(errors, "runtime"))
        manager_reason = _bounded_reason(
            _fact_error(errors, "runtime").get("code"), "runtime_projection_unavailable"
        )
    elif manager_status == _HEALTHY and manager_payload.get("manager_ready") is not True:
        manager_status = _WAITING
        manager_reason = "manager_not_ready"
    elif manager_status == _HEALTHY and manager_runtime.get("converged") is not True:
        manager_status = _WAITING
        manager_reason = "manager_runtime_converging"
    components.append(
        _component(
            "manager",
            "Manager",
            manager_status,
            (
                "active graph and required runtime units are converged"
                if manager_status == _HEALTHY
                else "Manager graph or runtime convergence is not current"
            ),
            required=True,
            required_for=("Observe / Render", "Plan / Preview", "Execute / Control"),
            owner_id=manager_section.get("source_owner_id"),
            owner_epoch=manager_section.get("source_owner_epoch"),
            generation=_mapping(active.get("publication")).get(
                "publication_generation"
            ),
            reason_code=manager_reason,
            scope="composition",
        )
    )

    observed_units = {
        row.get("runtime_unit_id"): row
        for row in _sequence(manager_runtime.get("observed_units"))
        if isinstance(row, Mapping) and isinstance(row.get("runtime_unit_id"), str)
    }
    runtime_profiles: list[str] = []
    for desired in _sequence(manager_runtime.get("desired_units")):
        if not isinstance(desired, Mapping):
            continue
        runtime_unit_id = desired.get("runtime_unit_id")
        if not isinstance(runtime_unit_id, str) or not runtime_unit_id:
            continue
        profile = desired.get("runtime_profile_id")
        if isinstance(profile, str) and profile and profile not in runtime_profiles:
            runtime_profiles.append(profile)
        observed = _mapping(observed_units.get(runtime_unit_id))
        converged = observed.get("converged") is True
        observed_offer = observed.get("observed_runtime_contract_offer_digest")
        desired_offer = desired.get("selected_runtime_contract_offer_digest")
        offer_matches = (
            isinstance(desired_offer, str)
            and desired_offer
            and observed_offer == desired_offer
        )
        unit_status = _HEALTHY if converged and offer_matches else _WAITING
        reason_codes = [
            reason
            for reason in _sequence(observed.get("reason_codes"))
            if isinstance(reason, str) and reason
        ]
        if observed and observed.get("source_health") not in (None, "healthy"):
            unit_status = _FAULT
        elif reason_codes and not converged:
            unit_status = _WARNING
        components.append(
            _component(
                f"runtime_unit:{runtime_unit_id}",
                runtime_unit_id,
                unit_status,
                (
                    "required runtime contract is present and converged"
                    if unit_status == _HEALTHY
                    else "required runtime unit has not converged to its selected contract"
                ),
                required=True,
                required_for=("Plan / Preview", "Execute / Control"),
                owner_id=runtime_unit_id,
                owner_epoch=observed.get("runtime_epoch"),
                generation=observed.get("critical_progress_sequence"),
                reason_code=(reason_codes[0] if reason_codes else "runtime_unit_converging"),
                scope="component",
            )
        )

    applied_error = _fact_error(errors, "applied_program")
    applied_source = _mapping(applied_program.get("source"))
    applied_status = _HEALTHY
    applied_reason: str | None = None
    if applied_error:
        applied_status = _error_status(applied_error)
        applied_reason = _bounded_reason(
            applied_error.get("code"), "applied_program_unavailable"
        )
    elif not applied_program:
        applied_status = _OFFLINE
        applied_reason = "applied_program_unavailable"
    elif (
        active_graph_revision is not None
        and active_bundle_id is not None
        and (
            applied_program.get("graph_revision") != active_graph_revision
            or applied_program.get("retained_bundle_id") != active_bundle_id
        )
    ):
        applied_status = _FAULT
        applied_reason = "applied_program_composition_mismatch"
    components.append(
        _component(
            "applied_program",
            "Applied Program and admission",
            applied_status,
            (
                f"{applied_program.get('program_id')} · {applied_program.get('scenario_id')}"
                if applied_status == _HEALTHY
                else "no exact Applied Program matches the active composition"
            ),
            required=True,
            required_for=("Execute / Control",),
            owner_id=applied_source.get("source_owner_id"),
            owner_epoch=applied_source.get("source_owner_epoch"),
            generation=applied_source.get("source_generation"),
            reason_code=applied_reason,
            scope="program",
        )
    )

    admission_error = _fact_error(errors, "authoring_admission")
    admission_status = _HEALTHY if authoring_admission else _error_status(admission_error)
    components.append(
        _component(
            "authoring_admission",
            "Authoring admission",
            admission_status,
            (
                "authoring is currently available"
                if authoring_admission.get("authoring_allowed") is True
                else "authoring is currently locked by an owner-issued claim"
            ),
            required=False,
            required_for=("Program and Deployment editing",),
            generation=authoring_admission.get("admission_generation"),
            reason_code=(
                None
                if authoring_admission
                else _bounded_reason(admission_error.get("code"), "admission_unavailable")
            ),
            scope="authoring",
        )
    )

    for section_id, label in (
        ("logical_scene", "Logical scene"),
        ("scene_runtime", "Scene runtime"),
    ):
        section = _mapping(sections.get(section_id))
        status, reason = _source_status(section)
        components.append(
            _component(
                section_id,
                label,
                status,
                (
                    "owner projection is current"
                    if status == _HEALTHY
                    else "owner projection is missing, stale, or degraded"
                ),
                required=True,
                required_for=("Observe / Render", "Plan / Preview"),
                owner_id=section.get("source_owner_id"),
                owner_epoch=section.get("source_owner_epoch"),
                generation=section.get("source_generation"),
                reason_code=reason,
                scope="scene",
            )
        )

    source_model_error = _fact_error(errors, "source_model")
    source_model_source = _mapping(source_model.get("source"))
    viewer_status, viewer_reason = _source_status(source_model_source)
    if source_model_error:
        viewer_status = _error_status(source_model_error)
        viewer_reason = _bounded_reason(
            source_model_error.get("code"), "source_model_unavailable"
        )
    elif source_model and (
        active_graph_revision is not None
        and active_bundle_id is not None
        and (
            source_model.get("graph_revision") != active_graph_revision
            or source_model.get("retained_bundle_id") != active_bundle_id
        )
    ):
        viewer_status = _FAULT
        viewer_reason = "source_model_composition_mismatch"
    components.append(
        _component(
            "viewer_model",
            "Viewer and model",
            viewer_status,
            (
                "current retained source model is available"
                if viewer_status == _HEALTHY
                else "current retained source model is unavailable or mismatched"
            ),
            required=True,
            required_for=("Observe / Render", "Plan / Preview"),
            owner_id=source_model_source.get("source_owner_id"),
            owner_epoch=source_model_source.get("source_owner_epoch"),
            generation=source_model_source.get("source_generation"),
            reason_code=viewer_reason,
            scope="model",
        )
    )

    identity, identity_response_error = _response_payload(
        execution_identity_response, "identity"
    )
    identity_error = _fact_error(errors, "execution_identity") or _mapping(
        identity_response_error
    )
    execution_status = _HEALTHY
    execution_reason: str | None = None
    if identity_error:
        execution_status = _error_status(identity_error)
        execution_reason = _bounded_reason(
            identity_error.get("code"), "execution_owner_unavailable"
        )
    elif not identity:
        execution_status = _OFFLINE
        execution_reason = "execution_owner_unavailable"
    elif (
        active_graph_revision is not None
        and active_bundle_id is not None
        and (
            identity.get("graph_revision") != active_graph_revision
            or identity.get("retained_bundle_id") != active_bundle_id
        )
    ):
        execution_status = _FAULT
        execution_reason = "execution_owner_composition_mismatch"
    components.append(
        _component(
            "execution_owner",
            "Execution owner",
            execution_status,
            (
                f"addressed owner for {identity.get('cell_id')} is current"
                if execution_status == _HEALTHY
                else "addressed execution owner is unavailable or mismatched"
            ),
            required=True,
            required_for=("Plan / Preview", "Execute / Control"),
            owner_epoch=identity.get("owner_epoch"),
            generation=identity.get("active_publication_generation"),
            reason_code=execution_reason,
            scope="execution",
        )
    )

    targets, targets_response_error = _response_payload(
        execution_targets_response, "targets"
    )
    targets_error = _fact_error(errors, "execution_targets") or _mapping(
        targets_response_error
    )
    direct_control = _mapping(targets.get("direct_control"))
    planning_capabilities = {
        value
        for value in _sequence(direct_control.get("planning_capability_ids"))
        if isinstance(value, str)
    }
    target_status = _HEALTHY if direct_control else _OFFLINE
    target_reason: str | None = None
    if targets_error:
        target_status = _error_status(targets_error)
        target_reason = _bounded_reason(
            targets_error.get("code"), "execution_target_unavailable"
        )
    elif not direct_control:
        target_reason = "execution_target_unavailable"
    components.append(
        _component(
            "execution_target",
            "Execution target",
            target_status,
            (
                f"{direct_control.get('controllable_component_id')} / {direct_control.get('command_target_id')}"
                if target_status == _HEALTHY
                else "no addressed motion target is currently offered"
            ),
            required=True,
            required_for=("Plan / Preview", "Execute / Control"),
            owner_epoch=identity.get("owner_epoch"),
            generation=identity.get("active_publication_generation"),
            reason_code=target_reason,
            scope="execution",
        )
    )

    capability_ids = {
        row.get("capability_id")
        for row in _sequence(active_graph.get("control_bindings"))
        if isinstance(row, Mapping) and isinstance(row.get("capability_id"), str)
    }
    collision_available = any(
        capability.startswith("execution.contact.")
        for capability in capability_ids
        if isinstance(capability, str)
    )
    planner_available = bool(planning_capabilities)
    planner_status = (
        _HEALTHY
        if planner_available and collision_available and target_status == _HEALTHY
        else _OFFLINE
    )
    components.append(
        _component(
            "planner_collision",
            "Planner and collision",
            planner_status,
            (
                "addressed planning and collision contracts are available"
                if planner_status == _HEALTHY
                else "planning or collision contract is unavailable"
            ),
            required=True,
            required_for=("Plan / Preview",),
            owner_epoch=identity.get("owner_epoch"),
            generation=identity.get("active_publication_generation"),
            reason_code=(None if planner_status == _HEALTHY else "planning_contract_unavailable"),
            scope="planning",
        )
    )

    control_state, control_response_error = _response_payload(
        execution_control_response, "control_state"
    )
    control_error = _fact_error(errors, "execution_control") or _mapping(
        control_response_error
    )
    cell_stop_action = _mapping(control_state.get("cell_stop_action"))
    stop_offered = "execution.stop.soft" in capability_ids
    stop_status = _HEALTHY if stop_offered and not control_error else _OFFLINE
    stop_reason: str | None = None
    if control_error:
        stop_status = _error_status(control_error)
        stop_reason = _bounded_reason(control_error.get("code"), "stop_state_unavailable")
    elif not stop_offered:
        stop_reason = "controlled_stop_capability_unavailable"
    components.append(
        _component(
            "stop_capability",
            "Controlled Stop",
            stop_status,
            (
                "controlled-Stop contract is present"
                if stop_status == _HEALTHY
                else "controlled-Stop contract or current owner state is unavailable"
            ),
            required=True,
            required_for=("Execute / Control",),
            owner_epoch=identity.get("owner_epoch"),
            generation=(
                cell_stop_action.get("expected_stop_generation")
                or identity.get("active_publication_generation")
            ),
            reason_code=stop_reason,
            scope="stop",
        )
    )

    cycle_error = _fact_error(errors, "program_cycle")
    cycle_status = _HEALTHY if program_cycle else _error_status(cycle_error)
    cycle_reason = None if program_cycle else _bounded_reason(
        cycle_error.get("code"), "program_runner_unavailable"
    )
    cycle_state = program_cycle.get("state")
    components.append(
        _component(
            "program_runner",
            "Program Runner",
            cycle_status,
            (
                f"cycle state {str(cycle_state).replace('_', ' ')}"
                if cycle_status == _HEALTHY
                else "Program cycle owner is unavailable"
            ),
            required=True,
            required_for=("Execute / Control",),
            owner_epoch=program_cycle.get("program_runner_owner_epoch"),
            generation=program_cycle.get("cycle_generation"),
            reason_code=cycle_reason,
            scope="program",
        )
    )

    alarm_error = _fact_error(errors, "alarms")
    active_alarms = [
        alarm
        for alarm in _sequence(alarms.get("alarms"))
        if isinstance(alarm, Mapping)
        and alarm.get("lifecycle_state")
        in {"active_unacknowledged", "active_acknowledged"}
    ]
    active_count = alarms.get("active_count")
    if not isinstance(active_count, int) or isinstance(active_count, bool):
        active_count = len(active_alarms)
    motion_attention = alarms.get("motion_attention_count")
    if not isinstance(motion_attention, int) or isinstance(motion_attention, bool):
        motion_attention = sum(
            1 for alarm in active_alarms if alarm.get("requires_motion_attention") is True
        )
    if alarm_error:
        alarm_status = _error_status(alarm_error)
        alarm_reason = _bounded_reason(alarm_error.get("code"), "alarm_owner_unavailable")
    elif active_count > 0:
        severe = motion_attention > 0 or any(
            alarm.get("priority") in {"critical", "high"} for alarm in active_alarms
        )
        alarm_status = _FAULT if severe else _WARNING
        alarm_reason = "active_functional_alarm"
    else:
        alarm_status = _HEALTHY
        alarm_reason = None
    components.append(
        _component(
            "operator_alarms",
            "Current functional alarms",
            alarm_status,
            (
                "no active functional alarm"
                if alarm_status == _HEALTHY
                else f"{active_count} active owner-issued condition(s)"
            ),
            required=True,
            required_for=("scope named by each alarm",),
            owner_id=alarms.get("owner_id"),
            owner_epoch=alarms.get("owner_epoch"),
            generation=alarms.get("generation"),
            reason_code=alarm_reason,
            scope="alarms",
        )
    )

    components_by_id = {component["id"]: component for component in components}
    scopes = (
        (
            "observe_render",
            "Observe / Render",
            ("manager", "logical_scene", "scene_runtime", "viewer_model"),
        ),
        (
            "plan_preview",
            "Plan / Preview",
            (
                "manager",
                "logical_scene",
                "viewer_model",
                "execution_owner",
                "execution_target",
                "planner_collision",
            ),
        ),
        (
            "execute_control",
            "Execute / Control",
            (
                "manager",
                "applied_program",
                "program_runner",
                "execution_owner",
                "execution_target",
                "stop_capability",
            ),
        ),
    )
    scope_components: list[dict[str, Any]] = []
    for component_id, label, contributors in scopes:
        status = _scope_status(components_by_id, contributors)
        scope_components.append(
            _component(
                component_id,
                label,
                status,
                (
                    f"{label} prerequisites are current"
                    if status == _HEALTHY
                    else f"one or more {label} prerequisites are not current"
                ),
                required=True,
                required_for=(label,),
                reason_code=(None if status == _HEALTHY else f"{component_id}_unavailable"),
                scope="functional_scope",
                contributor_ids=contributors,
            )
        )

    functional_statuses = [component["status"] for component in scope_components]
    if alarm_status == _FAULT or _FAULT in functional_statuses:
        readiness = "fault"
    elif all(status == _OFFLINE for status in functional_statuses):
        readiness = "offline"
    elif _WAITING in functional_statuses:
        readiness = "waiting"
    elif _OFFLINE in functional_statuses or _WARNING in functional_statuses:
        readiness = "degraded"
    else:
        readiness = "ready"

    labels = {
        "ready": "Application surfaces ready",
        "waiting": "Runtime convergence in progress",
        "degraded": "Some functional scopes unavailable",
        "fault": "Functional intervention required",
        "offline": "Required runtime is off",
    }
    messages = {
        "ready": "Observe, plan, and execute scopes are current for the active composition.",
        "waiting": "Required owners are converging automatically; no operator retry is required.",
        "degraded": "A named scope is unavailable while the remaining healthy scopes stay usable.",
        "fault": "An owner-issued fault or exact-coordinate mismatch affects a required scope.",
        "offline": "No complete current projection exists for the required runtime scopes.",
    }
    profile_label = ", ".join(runtime_profiles)
    if not profile_label:
        profile_label = str(active_graph.get("cell_id") or manager_payload.get("cell_id") or "No active profile")

    overview = {
        "schema_version": COMPONENT_OVERVIEW_VERSION,
        "readiness": readiness,
        "summary_label": labels[readiness],
        "profile_label": profile_label,
        "authority_label": "owner-issued scoped facts",
        "user_message": messages[readiness],
        "capabilities": [label for _component_id, label, _contributors in scopes],
        "components": [*scope_components, *components],
        "basis": {
            "runtime_projection_owner_epoch": runtime.get("projection_owner_epoch"),
            "runtime_projection_generation": runtime.get("projection_generation"),
            "manager_source_owner_epoch": manager_section.get("source_owner_epoch"),
            "logical_scene_source_owner_epoch": _mapping(
                sections.get("logical_scene")
            ).get("source_owner_epoch"),
            "logical_scene_source_generation": _mapping(
                sections.get("logical_scene")
            ).get("source_generation"),
            "scene_runtime_source_owner_epoch": _mapping(
                sections.get("scene_runtime")
            ).get("source_owner_epoch"),
            "scene_runtime_source_generation": _mapping(
                sections.get("scene_runtime")
            ).get("source_generation"),
            "alarm_owner_epoch": alarms.get("owner_epoch"),
            "alarm_generation": alarms.get("generation"),
            "alarm_state_revision": alarms.get("state_revision"),
            "admission_generation": authoring_admission.get("admission_generation"),
            "program_runner_owner_epoch": program_cycle.get("program_runner_owner_epoch"),
            "program_cycle_generation": program_cycle.get("cycle_generation"),
            "applied_program_owner_epoch": applied_source.get("source_owner_epoch"),
            "applied_program_generation": applied_source.get("source_generation"),
            "execution_owner_epoch": identity.get("owner_epoch"),
            "execution_publication_generation": identity.get("active_publication_generation"),
        },
    }
    overview["overview_revision"] = "sha256:" + hashlib.sha256(
        rfc8785.dumps(overview)
    ).hexdigest()
    return overview


__all__ = ["COMPONENT_OVERVIEW_VERSION", "build_component_overview"]
