from __future__ import annotations

import copy
from typing import Any

from manyforge_headless_api.component_overview import build_component_overview


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
                        },
                    },
                    "runtime": {
                        "converged": True,
                        "desired_units": [{
                            "runtime_unit_id": "robot.native",
                            "runtime_profile_id": "manyforge.native_execution_runtime",
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
            "direct_control": {
                "controllable_component_id": "robot-a",
                "command_target_id": "arm",
                "planning_capability_ids": ["planning.motion.joint_goal"],
            }
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
        "errors": {},
    }


def _by_id(overview: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {component["id"]: component for component in overview["components"]}


def test_ready_overview_keeps_normal_terminal_cycle_out_of_health() -> None:
    facts = _facts()

    overview = build_component_overview(**facts)

    assert overview["schema_version"] == "manyforge.component_overview.v1"
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


def test_overview_revision_changes_only_with_semantic_content() -> None:
    facts = _facts()
    first = build_component_overview(**facts)
    heartbeat = copy.deepcopy(facts)
    heartbeat["runtime"]["sections"]["manager"]["source_generation"] = 100
    heartbeat["runtime"]["sections"]["manager"]["payload"]["runtime"][
        "observed_units"
    ][0]["process_observation_sequence"] = 200

    second = build_component_overview(**heartbeat)

    assert second["overview_revision"] == first["overview_revision"]
