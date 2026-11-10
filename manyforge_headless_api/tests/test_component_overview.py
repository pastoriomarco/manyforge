from __future__ import annotations

import copy
from typing import Any

import pytest

from manyforge_headless_api.component_model import ComponentModel, parse_component_model
from manyforge_headless_api.component_overview import build_component_overview


def _model(*states: dict[str, Any]) -> ComponentModel:
    return parse_component_model({
        "component_states": list(states),
        "cell_latch": {"latched": False, "causes": []},
        "signal_samples": [],
        "rule_holds": [],
    })


def _component(component_id: str, lifecycle: str = "enabled", **extra: Any) -> dict[str, Any]:
    return {"component_id": component_id, "lifecycle": lifecycle, "activity": "idle",
            "streams": [], "capabilities": [], "observed_at_ns": 1, **extra}


def _source(owner: str, *, generation: int = 1) -> dict[str, Any]:
    return {
        "available": True,
        "freshness": "fresh",
        "known_gaps": [],
        "source_generation": generation,
        "source_owner_epoch": f"{owner}.epoch.1",
        "source_owner_id": owner,
    }


def _facts() -> dict[str, Any]:
    graph = "sha256:" + "1" * 64
    bundle = "sha256:" + "2" * 64
    offer = "sha256:" + "3" * 64
    runtime = {
        "complete": True,
        "projection_owner_epoch": "headless.epoch.1",
        "projection_generation": 9,
        "sections": {
            "manager": {
                **_source("manager"),
                "payload": {
                    "manager_ready": True,
                    "cell_id": "cell-a",
                    "active": {
                        "graph_revision": graph,
                        "retained_bundle_id": bundle,
                        "publication": {"publication_generation": 4},
                        "cell_graph": {
                            "cell_id": "cell-a",
                            "control_bindings": [
                                {"capability_id": "execution.contact.geometric"},
                                {"capability_id": "execution.stop.soft"},
                            ],
                            "controllable_components": [{
                                "controllable_component_id": "robot-a",
                                "runtime_unit_ids": ["robot.native"],
                            }],
                        },
                    },
                    "runtime": {
                        "converged": True,
                        "desired_units": [{
                            "runtime_unit_id": "robot.native",
                            "runtime_profile_id": "manyforge.ros2_control_execution_runtime",
                            "selected_runtime_contract_offer_digest": offer,
                        }],
                        "observed_units": [{
                            "runtime_unit_id": "robot.native",
                            "runtime_epoch": "native.epoch.1",
                            "critical_progress_sequence": 7,
                            "converged": True,
                            "source_health": "healthy",
                            "observed_runtime_contract_offer_digest": offer,
                        }],
                    },
                },
            },
            "logical_scene": {**_source("logical-scene"), "payload": {}},
            "scene_runtime": {**_source("scene-runtime"), "payload": {}},
        },
    }
    alarms = {
        "owner_id": "alarms",
        "owner_epoch": "alarms.epoch.1",
        "generation": 3,
        "state_revision": 5,
        "active_count": 0,
        "motion_attention_count": 0,
        "alarms": [],
    }
    applied = {
        "graph_revision": graph,
        "retained_bundle_id": bundle,
        "program_id": "pick",
        "scenario_id": "cell-pick",
        "source": {
            "source_owner_id": "program-owner",
            "source_owner_epoch": "program-owner.epoch.1",
            "source_generation": 6,
        },
    }
    source_model = {
        "graph_revision": graph,
        "retained_bundle_id": bundle,
        "source": _source("source-model"),
    }
    identity = {
        "ok": True,
        "identity": {
            "cell_id": "cell-a",
            "graph_revision": graph,
            "retained_bundle_id": bundle,
            "owner_epoch": "execution.epoch.1",
            "active_publication_generation": 8,
        },
    }
    targets = {
        "ok": True,
        "targets": {
            "schema_version": "manyforge.execution_targets.v4",
            "components": [{
                "controllable_component_id": "robot-a",
                "availability": {"command_eligible": True, "reason": None},
                "targets": [{
                    "command_target_id": "arm",
                    "operation_capability_id": "execution.trajectory.execute",
                    "presentation": "motion",
                    "command": {
                        "planning_capability_ids": ["planning.motion.joint_goal"],
                    },
                }],
            }],
        },
    }
    control = {
        "ok": True,
        "control_state": {
            "cell_stop_action": {
                "available": True,
                "expected_stop_generation": 2,
            }
        },
    }
    return {
        "runtime": runtime,
        "alarms": alarms,
        "authoring_admission": {
            "authoring_allowed": True,
            "admission_generation": 7,
        },
        "program_cycle": {
            "program_runner_owner_epoch": "runner.epoch.1",
            "cycle_generation": 11,
            "state": "terminal_awaiting_reset",
        },
        "applied_program": applied,
        "source_model": source_model,
        "execution_identity_response": identity,
        "execution_targets_response": targets,
        "execution_control_response": control,
        "component_model": _model(_component("robot-a")),
        "errors": {},
    }


def _by_id(overview: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {component["id"]: component for component in overview["components"]}


def test_stopped_prepared_owner_is_not_projected_as_healthy() -> None:
    facts = _facts()
    stop = {"code": "estop", "message": "E-stop (C2)", "since_ns": 1, "vendor_code": "C2"}
    facts["component_model"] = _model(
        _component("machine", "prepared", stop=stop),
        _component("robot", "prepared", owner_id="machine"),
    )
    tile = _by_id(build_component_overview(**facts))["component:machine"]
    assert tile["status"] == "waiting"
    assert tile["reason_code"] == "estop"
    assert tile["detail"] == "E-stop (C2)"


def test_ready_overview_keeps_normal_terminal_cycle_out_of_health() -> None:
    facts = _facts()

    overview = build_component_overview(**facts)

    assert overview["schema_version"] == "manyforge.component_overview.v2"
    assert overview["readiness"] == "ready"
    assert overview["overview_revision"].startswith("sha256:")
    assert _by_id(overview)["program_runner"]["detail"] == (
        "cycle state terminal awaiting reset"
    )
    assert _by_id(overview)["execute_control"]["status"] == "healthy"
    assert _by_id(overview)["execution_owner"]["owner_epoch"] == (
        "execution.epoch.1"
    )
    assert build_component_overview(**facts)["overview_revision"] == (
        overview["overview_revision"]
    )


def test_exact_composition_mismatch_is_a_functional_fault() -> None:
    facts = _facts()
    facts["execution_identity_response"]["identity"]["graph_revision"] = (
        "sha256:" + "f" * 64
    )

    overview = build_component_overview(**facts)

    assert overview["readiness"] == "fault"
    assert _by_id(overview)["execution_owner"] == {
        **_by_id(overview)["execution_owner"],
        "status": "fault",
        "reason_code": "execution_owner_composition_mismatch",
    }


def test_expected_manager_convergence_is_waiting_without_operator_retry() -> None:
    facts = _facts()
    facts["runtime"]["sections"]["manager"]["payload"]["runtime"][
        "converged"
    ] = False

    overview = build_component_overview(**facts)

    assert overview["readiness"] == "waiting"
    assert "automatically" in overview["user_message"]
    assert _by_id(overview)["manager"]["reason_code"] == (
        "manager_runtime_converging"
    )


def test_admitted_maintenance_exposes_one_stable_reason_without_hiding_faults() -> None:
    facts = _facts()
    manager = facts["runtime"]["sections"]["manager"]["payload"]
    manager["manager_ready"] = False
    manager["runtime"]["converged"] = False
    manager["runtime"]["observed_units"][0].update(
        {"converged": False, "source_health": "unavailable"}
    )
    manager["apply"] = {
        "records": [{
            "apply_id": "apply-1",
            "apply_generation": 4,
            "phase": "runtime_convergence_pending",
            "incomplete": True,
            "fence": {
                "scope": {"controllable_component_ids": ["robot-a"]},
                "state": "held",
            },
        }]
    }

    overview = build_component_overview(**facts)

    assert overview["readiness"] == "waiting"
    assert overview["maintenance_operation"] == {
        "apply_id": "apply-1",
        "apply_generation": 4,
        "phase": "runtime_convergence_pending",
        "affected_runtime_unit_ids": ["robot.native"],
        "reason_code": "planned_maintenance_in_progress",
    }
    assert _by_id(overview)["manager"]["reason_code"] == (
        "planned_maintenance_in_progress"
    )
    assert "Controls remain disabled" in overview["user_message"]

    facts["source_model"]["source"]["freshness"] = "stale"
    stale_viewer = build_component_overview(**facts)
    assert _by_id(stale_viewer)["viewer_model"]["reason_code"] == "source_stale"
    assert stale_viewer["maintenance_operation"] == overview[
        "maintenance_operation"
    ]

    manager["apply"]["records"] = []
    outside_maintenance = build_component_overview(**facts)
    assert outside_maintenance["maintenance_operation"] is None

    manager["apply"]["records"] = [{
        "apply_id": "apply-2",
        "apply_generation": 1,
        "phase": "fence_acquire_pending",
        "incomplete": True,
        "fence": {"scope": {"controllable_component_ids": ["robot-a"]}, "state": "acquire_pending"},
    }]
    before_admission = build_component_overview(**facts)
    assert before_admission["maintenance_operation"] is None

    manager["apply"]["records"][0].update({
        "phase": "runtime_convergence_pending",
        "fence": {"scope": {"controllable_component_ids": ["another-robot"]}, "state": "held"},
    })
    unrelated_scope = build_component_overview(**facts)
    assert unrelated_scope["maintenance_operation"] is None


def test_returned_attention_does_not_relabel_health_but_active_fault_does() -> None:
    facts = _facts()
    facts["alarms"].update({
        "active_count": 0,
        "alarms": [{
            "lifecycle_state": "returned_unacknowledged",
            "priority": "critical",
            "requires_motion_attention": True,
        }],
    })

    returned = build_component_overview(**facts)
    assert returned["readiness"] == "ready"

    facts["alarms"].update({
        "active_count": 1,
        "motion_attention_count": 1,
        "alarms": [{
            "lifecycle_state": "active_unacknowledged",
            "priority": "critical",
            "requires_motion_attention": True,
        }],
    })
    active = build_component_overview(**facts)
    assert active["readiness"] == "fault"
    assert _by_id(active)["operator_alarms"]["reason_code"] == (
        "active_functional_alarm"
    )


def test_missing_execution_owner_degrades_only_affected_scopes() -> None:
    facts = _facts()
    facts["execution_identity_response"] = None
    facts["errors"] = {
        "execution_identity": {
            "code": "execution_owner_unavailable",
            "message": "execution owner is not configured",
        }
    }

    overview = build_component_overview(**facts)
    components = _by_id(overview)

    assert overview["readiness"] == "degraded"
    assert components["observe_render"]["status"] == "healthy"
    assert components["plan_preview"]["status"] == "offline"
    assert components["execute_control"]["status"] == "offline"


def test_no_selected_program_is_normal_not_a_fault() -> None:
    facts = _facts()
    facts["applied_program"] = None
    facts["errors"] = {
        "applied_program": {
            "code": "program_not_selected",
            "message": "No Program is selected",
        }
    }

    overview = build_component_overview(**facts)
    components = _by_id(overview)

    assert overview["readiness"] == "ready"
    assert components["applied_program"]["status"] == "healthy"
    assert components["applied_program"]["reason_code"] == "program_not_selected"
    assert components["applied_program"]["detail"] == (
        "no Program is selected; Execute is unavailable"
    )
    assert components["execute_control"]["status"] == "healthy"

    facts["errors"]["applied_program"]["code"] = "applied_program_invalid"
    assert _by_id(build_component_overview(**facts))["applied_program"][
        "status"
    ] == "fault"


def test_overview_revision_changes_only_with_semantic_content() -> None:
    facts = _facts()
    first = build_component_overview(**facts)
    heartbeat = copy.deepcopy(facts)
    heartbeat["runtime"]["projection_generation"] = 99
    heartbeat["runtime"]["sections"]["manager"]["source_generation"] = 100
    heartbeat["runtime"]["sections"]["manager"]["payload"]["runtime"][
        "observed_units"
    ][0]["process_observation_sequence"] = 200
    heartbeat["alarms"]["state_revision"] = 201
    heartbeat["runtime"]["sections"]["scene_runtime"]["source_generation"] = 202

    second = build_component_overview(**heartbeat)

    assert second["overview_revision"] == first["overview_revision"]


def _empty_facts(canonical_empty: bool = True) -> dict[str, Any]:
    facts = _facts()
    runtime = facts["runtime"]
    manager = runtime["sections"]["manager"]["payload"]
    active = manager["active"]
    active["canonical_empty"] = canonical_empty
    active["cell_graph"].update({
        "graph_revision": active["graph_revision"],
        "model_instances": [], "controllable_components": [],
        "control_bindings": [], "control_domains": [],
    })
    active["publication"]["projection_revision"] = {
        "graph_revision": active["graph_revision"],
        "retained_bundle_id": active["retained_bundle_id"],
    }
    runtime.update(active_graph_revision=active["graph_revision"],
                   active_retained_bundle_id=active["retained_bundle_id"])
    manager["apply"] = {"records": []}
    facts["component_model"] = _model()
    facts["applied_program"] = None
    facts["program_cycle"]["state"] = "idle_after_reset"
    facts["errors"]["applied_program"] = {
        "code": "program_applied_selection_not_found" if canonical_empty else "program_not_selected"
    }
    for fact in ("execution_identity", "execution_targets", "execution_control"):
        facts[f"{fact}_response"] = None
        facts["errors"][fact] = {"code": "execution_owner_not_ready"}
    return facts


def _without_empty_rule(facts: dict[str, Any]) -> dict[str, Any]:
    comparison = copy.deepcopy(facts)
    del comparison["runtime"]["sections"]["manager"]["payload"]["active"]["canonical_empty"]
    return build_component_overview(**comparison)


def _warm_empty_facts(canonical_empty: bool = True) -> dict[str, Any]:
    facts = _empty_facts(canonical_empty)
    active = facts["runtime"]["sections"]["manager"]["payload"]["active"]
    identity = copy.deepcopy(_facts()["execution_identity_response"]["identity"])
    identity["active_publication_generation"] = active["publication"]["publication_generation"]
    identity["scene_revision"] = {"scene_id": "scene-a", "revision_id": 1,
                                  "content_hash": "sha256:" + "4" * 64}
    facts["execution_identity_response"] = {"ok": True, "identity": identity}
    facts["execution_targets_response"] = {
        "ok": True, "identity": copy.deepcopy(identity),
        "targets": {"schema_version": "manyforge.execution_targets.v4", "components": []},
    }
    facts["execution_control_response"] = {
        "ok": True, "identity": copy.deepcopy(identity),
        "control_state": {
            "schema_version": "manyforge.execution_control_state.v7",
            "observed_targets": [], "manual_operations": [], "runtime_scale_control": None,
            "cell_stop_action": {"available": True, "expected_stop_generation": 0,
                                 "latest_terminal": None,
                                 "unavailable_reason": None},
        },
    }
    for name in ("execution_identity", "execution_targets", "execution_control"):
        del facts["errors"][name]
    return facts


@pytest.mark.parametrize("canonical_empty", [True, False])
@pytest.mark.parametrize("warm", [True, False])
def test_verified_empty_publication_has_no_required_equipment_faults(canonical_empty, warm) -> None:
    facts = _warm_empty_facts(canonical_empty) if warm else _empty_facts(canonical_empty)
    captured = copy.deepcopy(facts)
    overview = build_component_overview(**facts)
    rows = _by_id(overview)
    expected_absent = {
        "plan_preview", "execute_control", "applied_program", "execution_owner",
        "execution_target", "planner_collision", "stop_capability", "viewer_model",
    }
    neutral = {"execute_control", "execution_target", "planner_collision",
               "stop_capability", "viewer_model"}
    assert overview["readiness"] == "ready"
    assert overview["summary_label"] == "No components"
    assert "ready for authoring" in overview["user_message"]
    assert {key for key, row in rows.items() if row["reason_code"] == ("canonical_empty" if canonical_empty else "no_physical_components")} == expected_absent
    for key in expected_absent:
        assert rows[key]["required"] is False
        assert rows[key]["status"] == ("not_applicable" if key in neutral else "offline")
        assert rows[key]["detail"] == ("No robot in this Scenario" if key in neutral else "No components")
    assert not any(key.startswith(("component:", "runtime_unit:")) for key in rows)
    assert rows["observe_render"]["status"] == "healthy"
    assert facts == captured
    assert build_component_overview(**facts) == overview


@pytest.mark.parametrize(("path", "value"), [
    (("runtime", "complete"), False),
    (("available",), False),
    (("freshness",), "stale"),
    (("known_gaps",), ["source_gap"]),
    (("source_owner_epoch",), None),
    (("source_generation",), True),
    (("payload", "manager_ready"), False),
    (("payload", "runtime", "converged"), False),
    (("payload", "apply", "records"), None),
    (("payload", "apply", "records"), [{"incomplete": True}]),
    (("payload", "active", "canonical_empty"), "true"),
    (("payload", "active", "canonical_empty"), 0),
    (("payload", "active", "canonical_empty"), 1),
    (("payload", "active", "canonical_empty"), None),
    (("payload", "active", "cell_graph", "model_instances"), [{}]),
    (("payload", "active", "cell_graph", "control_domains"), [{}]),
    (("payload", "active", "cell_graph", "model_instances"), None),
    (("payload", "active", "cell_graph", "control_domains"), None),
    (("payload", "active", "cell_graph", "control_bindings"), [{}]),
    (("payload", "active", "cell_graph", "controllable_components"), [{}]),
    (("payload", "active", "cell_graph", "cell_id"), "another-cell"),
    (("payload", "active", "cell_graph", "graph_revision"), "another-graph"),
    (("payload", "active", "publication", "publication_generation"), True),
    (("payload", "active", "publication", "projection_revision", "retained_bundle_id"), "another-bundle"),
    (("runtime", "active_graph_revision"), "another-graph"),
])
@pytest.mark.parametrize("canonical_empty", [True, False])
@pytest.mark.parametrize("warm", [True, False])
def test_unverified_empty_keeps_the_existing_reduction(path, value, canonical_empty, warm) -> None:
    facts = _warm_empty_facts(canonical_empty) if warm else _empty_facts(canonical_empty)
    target = facts if path[0] == "runtime" else facts["runtime"]["sections"]["manager"]
    for part in path[:-1]:
        target = target[part]
    target[path[-1]] = value
    assert build_component_overview(**facts) == _without_empty_rule(facts)
    assert build_component_overview(**facts)["summary_label"] != "No components"


@pytest.mark.parametrize("fault", [
    "logical_scene", "scene_runtime", "source_model", "component",
    "program_cycle", "authoring_admission", "alarms", "applied_program",
    "execution_identity", "execution_targets", "execution_control",
])
@pytest.mark.parametrize("canonical_empty", [True, False])
@pytest.mark.parametrize("warm", [True, False])
def test_empty_publication_does_not_hide_actual_source_or_owner_errors(fault, canonical_empty, warm) -> None:
    facts = _warm_empty_facts(canonical_empty) if warm else _empty_facts(canonical_empty)
    if fault in {"logical_scene", "scene_runtime"}:
        facts["runtime"]["sections"][fault]["available"] = False
    elif fault == "source_model":
        facts[fault]["source"]["freshness"] = "stale"
    elif fault == "component":
        facts["component_model"] = _model(_component("io", "fault", fault={
            "code": "fault_communication", "message": "link lost", "clearable": True,
            "operator_action": "recover", "since_ns": 1}))
    elif fault == "alarms":
        facts["alarms"].update(active_count=1, motion_attention_count=1)
    else:
        if fault in {"program_cycle", "authoring_admission"}:
            facts[fault] = None
        facts["errors"][fault] = {"code": f"{fault}_invalid"}
    assert build_component_overview(**facts) == _without_empty_rule(facts)
    assert build_component_overview(**facts)["summary_label"] != "No components"


@pytest.mark.parametrize(("path", "value"), [
    (("execution_identity_response", "identity", "cell_id"), "foreign-cell"),
    (("execution_identity_response", "identity", "retained_bundle_id"), "foreign-bundle"),
    (("execution_identity_response", "identity", "graph_revision"), "foreign-graph"),
    (("execution_identity_response", "identity", "active_publication_generation"), 3),
    (("execution_identity_response", "identity", "active_publication_generation"), True),
    (("execution_identity_response", "identity", "owner_epoch"), ""),
    (("execution_targets_response", "identity", "owner_epoch"), "replaced-owner"),
    (("execution_control_response", "identity", "active_publication_generation"), 5),
    (("execution_control_response", "identity", "scene_revision", "revision_id"), 2),
    (("execution_targets_response", "targets", "components"), [{}]),
    (("execution_control_response", "control_state", "observed_targets"), [{}]),
    (("execution_control_response", "control_state", "manual_operations"), [{}]),
    (("execution_control_response", "control_state", "runtime_scale_control"), {}),
    (("execution_control_response", "control_state", "cell_stop_action", "available"), False),
])
def test_warm_empty_requires_current_matching_fault_free_owner_projections(path, value) -> None:
    facts = _warm_empty_facts()
    target = facts
    for field in path[:-1]:
        target = target[field]
    target[path[-1]] = value
    overview = build_component_overview(**facts)
    assert overview == _without_empty_rule(facts)
    assert overview["summary_label"] != "No components"
    assert _by_id(overview)["plan_preview"]["required"] is True


def test_loaded_owner_not_ready_remains_a_required_fault() -> None:
    facts = _facts()
    for fact in ("execution_identity", "execution_targets", "execution_control"):
        facts[f"{fact}_response"] = None
        facts["errors"][fact] = {"code": "execution_owner_not_ready"}
    overview = build_component_overview(**facts)
    assert overview["readiness"] == "fault"
    for key in ("execution_owner", "execution_target", "stop_capability"):
        assert _by_id(overview)[key]["status"] == "fault"
        assert _by_id(overview)[key]["required"] is True


@pytest.mark.parametrize("physical_array", [
    "model_instances", "controllable_components", "control_bindings", "control_domains",
])
def test_named_scenario_with_any_physical_row_keeps_required_faults(physical_array) -> None:
    facts = _empty_facts(False)
    graph = facts["runtime"]["sections"]["manager"]["payload"]["active"]["cell_graph"]
    graph[physical_array] = [{}]
    overview = build_component_overview(**facts)
    assert overview == _without_empty_rule(facts)
    assert overview["readiness"] == "fault"
    assert _by_id(overview)["execution_owner"]["required"] is True
    assert _by_id(overview)["execution_owner"]["status"] == "fault"


def test_empty_real_returned_alarm_remains_visible() -> None:
    facts = _empty_facts()
    facts["alarms"].update({
        "unacknowledged_count": 1,
        "alarms": [{"lifecycle_state": "returned_unacknowledged", "priority": "high"}],
    })
    overview = build_component_overview(**facts)
    assert _by_id(overview)["execute_control"]["status"] == "not_applicable"
    assert overview["basis"]["alarm_state_revision"] == facts["alarms"]["state_revision"]
