from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
from types import ModuleType

import pytest


REPO_ROOT = Path(__file__).resolve().parents[3]


def load_module(relative: str, name: str) -> ModuleType:
    path = REPO_ROOT / relative
    qualification_root = str(path.parent)
    if qualification_root not in sys.path:
        sys.path.insert(0, qualification_root)
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def live() -> ModuleType:
    return load_module(
        "scripts/qualification/run_i9_isaac_live_row.py", "i9_live_row"
    )


def base_observations(live: ModuleType, scenario):
    run_id = "program-run.1"
    runtime = {
        "schema_version": "manyforge.headless_runtime_snapshot.v1",
        "projection_owner_epoch": "headless.epoch.1",
        "projection_generation": 5,
        "sections": {
            "logical_scene": {"available": True, "freshness": "fresh"},
            "scene_runtime": {"available": True, "freshness": "fresh"},
        },
    }
    scene_before = {
        "schema_version": "manyforge.scene_projection.v1",
        "projection_generation": 3,
        "projection_owner_epoch": "headless.epoch.1",
    }
    scene_after = {
        "schema_version": "manyforge.scene_projection.v1",
        "projection_generation": 5,
        "projection_owner_epoch": "headless.epoch.1",
        "logical_scene": {
            "source_owner_id": f"logical-scene.{scenario.cell_id}",
            "source_owner_epoch": "logical.epoch.1",
            "projection_generation": 5,
            "payload": {
                "snapshot": {
                    "cell_id": scenario.cell_id,
                    "carried_object_attachments": [],
                    "objects": [],
                }
            },
        },
        "scene_runtime": {
            "source_owner_id": f"scene-runtime.{scenario.cell_id}",
            "source_owner_epoch": "scene-runtime.epoch.1",
            "projection_generation": 5,
            "payload": {"state": {"pose_sources": []}},
        },
    }
    samples = {
        topic: {"seen": True}
        for topic in {
            "/clock",
            "/isaac_joint_commands",
            "/isaac_joint_states",
            "/isaac_joint_commands_gripper",
            "/isaac_gripper_state",
            "/output",
        }
    }
    program_state = "failed" if "collision" in scenario.scenario_id else "succeeded"
    program = {
        "state": program_state,
        "capture": {
            "program_run_id": run_id,
            "scenario_id": scenario.scenario_id,
        },
        "correlation_id": f"i9.{scenario.scenario_id}",
    }
    history_events = [{"event": {"program_run_id": run_id}}]
    alarms = {"alarms": []}
    if "collision" in scenario.scenario_id:
        alarms = {
            "alarms": [
                {
                    "definition_id": "manyforge.motion.runtime_collision",
                    "lifecycle_state": "active_unacknowledged",
                    "priority": "critical",
                    "requires_motion_attention": True,
                    "subject": {
                        "subject_kind": "operation",
                        "subject_id": "operation.1",
                        "resource_ids": ["joint.1"],
                    },
                    "details": {
                        "reason": "runtime_collision_fault",
                        "source_label": "kernel.runtime_collision",
                    },
                    "correlations": {
                        "participant_id": "participant.1",
                        "program_run_id": run_id,
                        "request_id": "request.1",
                    },
                }
            ]
        }
        history_events.append(
            {
                "event": {
                    "event_type": "stop_acceptance_projected",
                    "payload": {
                        "journal_payload": {
                            "api_version": (
                                "manyforge.stop_acceptance_projection.v2"
                            ),
                            "intent": {
                                "intent_kind": "ordinary_stop",
                                "value": {
                                    "reason": "runtime_collision_fault",
                                    "request_context": {
                                        "source_label": (
                                            "kernel.runtime_collision"
                                        )
                                    },
                                },
                            },
                            "affected_state": {
                                "operations": [
                                    {"program_run_id": run_id}
                                ]
                            },
                        }
                    },
                }
            }
        )
    if "foundationpose" in scenario.scenario_id:
        scene_after["scene_runtime"]["payload"]["state"]["pose_sources"] = [
            {
                "source_id": "foundationpose_trocar",
                "object_id": "graspable",
                "policy_id": "before_planning",
                "quality": "good",
                "fresh": True,
                "eligible": True,
                "source_epoch": "pose.epoch.1",
                "source_sequence": 7,
                "payload": {
                    "api_version": "manyforge.ros_pose_observation.v1",
                    "kind": "detection3d_pose",
                    "pose": {
                        "frame_kind": "cell_frame",
                        "frame_id": "link_base",
                    },
                    "source": {"topic": "/output"},
                },
            }
        ]
        scene_after["logical_scene"]["payload"]["snapshot"]["objects"] = [
            {
                "scene_resource_id": "graspable",
                "provider_evidence": {
                    "provider_source_epoch": "pose.epoch.1",
                    "provider_source_sequence": 7,
                },
            }
        ]
    coordinate_kind = "linear_m" if scenario.profile == "lite6" else "angular_rad"
    targets = (
        live.GripperTarget("gripper", coordinate_kind, 0.0),
        live.GripperTarget(
            "gripper", coordinate_kind, 0.0081 if scenario.profile == "lite6" else 0.55
        ),
    )
    run_evidence = live.RunEvidence(
        gripper_targets=targets,
        execution_samples=3,
        execution_owner_epochs={"execution.epoch.1"},
        execution_cell_ids={scenario.cell_id},
        execution_component_ids={scenario.component_id},
        gripper_hits=set(targets),
    )
    if "object_motion_observed" in scenario.required_checks:
        run_evidence.object_initial_position = (0.0, 0.0, 0.0)
        run_evidence.object_max_displacement_m = 0.2
        run_evidence.object_pose_samples = 3
        run_evidence.object_carried_observed = True
    if "foundationpose_detection_fresh" in scenario.required_checks:
        run_evidence.foundationpose_source_sequences = {7}
        run_evidence.foundationpose_source_identities = {("pose.epoch.1", 7)}
        run_evidence.foundationpose_provider_sequences = {7}
        run_evidence.foundationpose_provider_match = True
    return {
        "barrier": {
            "world_name": scenario.world_name,
            "world_uri": scenario.world_uri,
        },
        "before_control": {
            "exact_world": True,
            "state": {"state": "stopped"},
        },
        "after_control": {
            "exact_world": True,
            "state": {"state": "stopped"},
            "reset": {"success": True},
            "stop": {"success": True},
        },
        "controllers": {
            "returncode": 0,
            "output": (
                "isaac_arm_controller joint_trajectory_controller active\n"
                "joint_state_broadcaster joint_state_broadcaster active\n"
            ),
        },
        "nodes": {
            "returncode": 0,
            "output": f"/manyforge/{scenario.component_id}/controller_manager\n",
        },
        "command_info": {"output": "Subscription count: 1\n"},
        "samples": samples,
        "topic_types": {
            topic: {
                "returncode": 0,
                "type": live.TOPIC_TYPE_BY_NAME[topic],
            }
            for topic in scenario.required_topics
        },
        "program": program,
        "run_evidence": run_evidence,
        "runtime": runtime,
        "scene_before": scene_before,
        "scene_after": scene_after,
        "alarms": alarms,
        "history": {"events": history_events},
        "tf_probe": (
            {
                "bytes": 100,
                "output": "link_base -> camera_color_optical_frame",
            }
            if "foundationpose" in scenario.scenario_id
            else None
        ),
        "assistant": (
            {
                "passed": True,
                "viewer_owner_epoch": "headless.epoch.1",
                "viewer_generation": 5,
                "model_requests": 1,
                "snapshot_completed_delta": 1,
            }
            if "assistant" in scenario.scenario_id
            else None
        ),
        "viewer_cell_id": (
            scenario.cell_id if "assistant" in scenario.scenario_id else None
        ),
    }


@pytest.mark.parametrize(
    "scenario_id",
    (
        "isaac-lite6-pick-place",
        "isaac-lite6-trocar-foundationpose",
        "isaac-ur10e-light-scene",
        "isaac-ur10e-light-scene-assistant",
        "isaac-ur10e-light-scene-collision",
    ),
)
def test_check_reducer_closes_each_exact_scenario(
    live: ModuleType, scenario_id: str
) -> None:
    scenario = live.SCENARIO_BY_ID[scenario_id]
    checks = live._build_checks(  # noqa: SLF001
        scenario, **base_observations(live, scenario)
    )
    assert checks == {key: True for key in scenario.required_checks}


def test_check_reducer_rejects_crossed_namespace_and_world(live: ModuleType) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-ur10e-light-scene"]
    observations = base_observations(live, scenario)
    observations["nodes"]["output"] += (
        "/manyforge/isaac_ur10e_collision_robot/controller_manager\n"
    )
    observations["after_control"]["exact_world"] = False
    checks = live._build_checks(  # noqa: SLF001
        scenario, **observations
    )
    assert checks["runtime_namespace_exact"] is False
    assert checks["exact_world_readback"] is False


def test_check_reducer_requires_kernel_collision_and_real_snapshot_counters(
    live: ModuleType,
) -> None:
    collision = live.SCENARIO_BY_ID["isaac-ur10e-light-scene-collision"]
    observations = base_observations(live, collision)
    observations["alarms"] = {"alarms": []}
    observations["history"] = {
        "events": [{"event": {"program_run_id": "program-run.1"}}]
    }
    checks = live._build_checks(collision, **observations)  # noqa: SLF001
    assert checks["collision_stop_terminal"] is False
    assert checks["collision_stop_kernel_owned"] is False

    assistant = live.SCENARIO_BY_ID["isaac-ur10e-light-scene-assistant"]
    observations = base_observations(live, assistant)
    observations["assistant"]["snapshot_completed_delta"] = 0
    checks = live._build_checks(assistant, **observations)  # noqa: SLF001
    assert checks["assistant_snapshot_transmitted"] is False


def test_check_reducer_rejects_wrong_topic_type_and_legacy_run_id(
    live: ModuleType,
) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-lite6-pick-place"]
    observations = base_observations(live, scenario)
    observations["topic_types"]["/clock"]["type"] = (
        "builtin_interfaces/msg/Time"
    )
    observations["program"] = {
        "state": "succeeded",
        "program_run_id": "program-run.1",
    }
    checks = live._build_checks(scenario, **observations)  # noqa: SLF001
    assert checks["required_topics_exact"] is False
    assert checks["program_terminal"] is False
    assert checks["status_trace_correlated"] is False


def test_check_reducer_requires_each_gripper_target_and_physical_object_motion(
    live: ModuleType,
) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-ur10e-light-scene"]
    observations = base_observations(live, scenario)
    evidence = observations["run_evidence"]
    evidence.gripper_hits = {evidence.gripper_targets[0]}
    evidence.object_carried_observed = False
    checks = live._build_checks(scenario, **observations)  # noqa: SLF001
    assert checks["gripper_converged"] is False
    assert checks["object_motion_observed"] is False


def test_check_reducer_requires_source_matched_foundationpose_latch(
    live: ModuleType,
) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-lite6-trocar-foundationpose"]
    observations = base_observations(live, scenario)
    observations["run_evidence"].foundationpose_provider_match = False
    checks = live._build_checks(scenario, **observations)  # noqa: SLF001
    assert checks["foundationpose_detection_fresh"] is True
    assert checks["foundationpose_pose_latched"] is False


def test_extract_gripper_targets_reads_typed_leaf_and_pick_sequence(
    live: ModuleType,
) -> None:
    program = {
        "tree": {
            "node_id": "sequence",
            "parameters": {},
            "children": [
                {
                    "node_id": "gripper.command_position",
                    "parameters": {
                        "target_role": "gripper",
                        "position": {"kind": "linear_m", "value_m": 0.0081},
                    },
                    "children": [],
                },
                {
                    "node_id": "planning.plan_pick_sequence",
                    "parameters": {
                        "tool_target_role": "gripper",
                        "tool_position": {"kind": "linear_m", "value_m": 0.0},
                    },
                    "children": [],
                },
            ],
        }
    }
    assert live._extract_gripper_targets(program) == (  # noqa: SLF001
        live.GripperTarget("gripper", "linear_m", 0.0),
        live.GripperTarget("gripper", "linear_m", 0.0081),
    )


def test_process_commands_bind_exact_component_world_and_pose_config(
    live: ModuleType, tmp_path: Path
) -> None:
    source = tmp_path / "source"
    package = tmp_path / "package"
    installed = tmp_path / "installed"
    run = tmp_path / "run"
    for path in (source, package, installed, run):
        path.mkdir()
    scenario = live.SCENARIO_BY_ID["isaac-lite6-trocar-foundationpose"]
    config = live.LiveConfig(
        spec=scenario,
        source_root=source,
        package_root=package,
        installed_root=installed,
        run_root=run,
        node_catalog=source / "nodes.yaml",
        alarm_catalog=source / "alarms.yaml",
        static_dir=source,
        startup_timeout_s=180.0,
        run_timeout_s=300.0,
        composer_port=None,
    )
    commands = live._process_commands(  # noqa: SLF001
        config,
        api_port=9010,
        runner_port=9020,
        composer_port=None,
        manager_root=run / "manager",
        runtime_root=run / "runtime",
        publication_root=run / "publications",
    )
    ros = commands["ros"]
    assert f"runtime_namespace:=/manyforge/{scenario.component_id}" in ros
    assert f"world_uri:={scenario.world_uri}" in ros
    assert f"expected_world_name:={scenario.world_name}" in ros
    assert "additional_required_topics:=/output,/tf,/tf_static" in ros
    assert not any("DemoSupervisor" in item for item in ros)
