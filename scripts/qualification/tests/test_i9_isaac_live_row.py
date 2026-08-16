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
        "active_control": {
            "exact_world": True,
            "play": {"success": True},
            "state": {"state": "playing"},
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
        "cclc_startup": {
            "schema_version": live._CCLC_STARTUP_SCHEMA,  # noqa: SLF001
            "component_id": scenario.component_id,
            "passed": True,
        },
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


def test_live_row_primes_object_motion_only_after_program_run_initializes_scene(
    live: ModuleType,
) -> None:
    source = Path(live.__file__).read_text(encoding="utf-8")
    run_source = source[
        source.index("def _run_program(") : source.index("def _json_bytes(")
    ]

    assert run_source.index("snapshot = client.program_run_start(") < (
        run_source.index("_prime_run_evidence(")
    )


def test_graph_probe_converges_with_one_participant(
    live: ModuleType, monkeypatch: pytest.MonkeyPatch
) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-ur10e-light-scene"]
    calls: list[str] = []

    class FakeNode:
        def __init__(self, name: str) -> None:
            calls.append(f"node:{name}")

        def destroy_node(self) -> None:
            calls.append("destroy")

        def get_node_names_and_namespaces(self):
            return [
                ("controller_manager", f"/manyforge/{scenario.component_id}"),
                ("ros_bridge", "/"),
            ]

        def get_topic_names_and_types(self):
            return [
                (topic, [live.TOPIC_TYPE_BY_NAME[topic]])
                for topic in scenario.required_topics
            ]

        def get_subscriptions_info_by_topic(self, topic: str):
            assert topic == "/isaac_joint_commands"
            return [object()]

    fake_rclpy = ModuleType("rclpy")
    fake_rclpy.init = lambda *, args: calls.append(f"init:{args}")
    fake_rclpy.spin_once = (
        lambda node, *, timeout_sec: calls.append(f"spin:{timeout_sec}")
    )
    fake_rclpy.ok = lambda: True
    fake_rclpy.shutdown = lambda: calls.append("shutdown")
    fake_node_module = ModuleType("rclpy.node")
    fake_node_module.Node = FakeNode
    monkeypatch.setitem(sys.modules, "rclpy", fake_rclpy)
    monkeypatch.setitem(sys.modules, "rclpy.node", fake_node_module)

    nodes, command_info, topic_types = live._read_ros_graph(  # noqa: SLF001
        scenario, timeout_s=0.1
    )

    assert calls.count("destroy") == 1
    assert calls.count("shutdown") == 1
    assert sum(call.startswith("node:") for call in calls) == 1
    assert (
        f"/manyforge/{scenario.component_id}/controller_manager\n"
        in nodes["output"]
    )
    assert "/ros_bridge\n" in nodes["output"]
    assert command_info["output"] == "Subscription count: 1\n"
    assert topic_types == {
        topic: {
            "returncode": 0,
            "type": live.TOPIC_TYPE_BY_NAME[topic],
        }
        for topic in scenario.required_topics
    }


@pytest.mark.parametrize(
    "retry_code", ("stale_revision", "scene_projection_changed")
)
def test_scene_pose_query_retries_only_exact_projection_races(
    live: ModuleType, retry_code: str,
) -> None:
    def scene(revision: int) -> dict:
        return {
            "logical_scene": {
                "payload": {
                    "access": {
                        "active_projection": {
                            "graph_revision": f"graph.{revision}",
                            "retained_bundle_id": f"bundle.{revision}",
                            "scene_revision": {
                                "content_hash": f"content.{revision}",
                                "revision_id": revision,
                                "scene_id": f"scene.{revision}",
                            },
                        }
                    }
                }
            }
        }

    class Client:
        calls: list[dict] = []

        def scene(self) -> dict:
            return scene(2)

        def resolve_scene_resources(self, body: dict) -> dict:
            self.calls.append(body)
            if len(self.calls) == 1:
                raise live.HeadlessClientError(
                    retry_code,
                    "projection advanced",
                    409,
                )
            return {
                "poses": [
                    {
                        "scene_resource_id": "isaac_cheese_box",
                        "pose_in_cell": {
                            "position_m": {"x": 1.0, "y": 2.0, "z": 3.0}
                        },
                    }
                ]
            }

    client = Client()
    assert live._resolved_position(  # noqa: SLF001
        client, scene(1), "isaac_cheese_box"
    ) == (1.0, 2.0, 3.0)
    assert [
        call["expected_projection_revision"]["graph_revision"]
        for call in client.calls
    ] == ["graph.1", "graph.2"]


def test_scene_pose_query_does_not_retry_other_failures(live: ModuleType) -> None:
    scene = {
        "logical_scene": {
            "payload": {
                "access": {
                    "active_projection": {
                        "graph_revision": "graph.1",
                        "retained_bundle_id": "bundle.1",
                        "scene_revision": {
                            "content_hash": "content.1",
                            "revision_id": 1,
                            "scene_id": "scene.1",
                        },
                    }
                }
            }
        }
    }

    class Client:
        @staticmethod
        def resolve_scene_resources(body: dict) -> dict:
            del body
            raise live.HeadlessClientError(
                "scene_pose_unavailable", "pose missing", 409
            )

    with pytest.raises(live.HeadlessClientError, match="pose missing"):
        live._resolved_position(Client(), scene, "isaac_cheese_box")  # noqa: SLF001


def test_scene_motion_sample_uses_revision_bound_snapshot_during_continuous_race(
    live: ModuleType,
) -> None:
    scene = {
        "logical_scene": {
            "payload": {
                "access": {
                    "active_projection": {
                        "graph_revision": "graph.1",
                        "retained_bundle_id": "bundle.1",
                        "scene_revision": {
                            "content_hash": "content.1",
                            "revision_id": 1,
                            "scene_id": "scene.1",
                        },
                    }
                },
                "snapshot": {
                    "objects": [
                        {
                            "scene_resource_id": "isaac_cheese_box",
                            "placement": {
                                "pose_in_cell": {
                                    "position_m": {
                                        "x": 1.0,
                                        "y": 2.0,
                                        "z": 3.0,
                                    }
                                }
                            },
                        }
                    ]
                },
            }
        }
    }

    class Client:
        calls = 0

        def resolve_scene_resources(self, body: dict) -> dict:
            del body
            self.calls += 1
            raise live.HeadlessClientError(
                "scene_projection_changed", "continuous source advanced", 409
            )

    client = Client()
    evidence = live.RunEvidence(gripper_targets=())
    assert live._sample_position(  # noqa: SLF001
        evidence, client, scene, "isaac_cheese_box"
    ) == (1.0, 2.0, 3.0)
    assert client.calls == 1
    assert evidence.pose_query_rejections == 1


def test_scene_motion_sample_holds_only_proven_pose_during_carried_ambiguity(
    live: ModuleType,
) -> None:
    scene = {
        "logical_scene": {
            "payload": {
                "access": {
                    "active_projection": {
                        "graph_revision": "graph.1",
                        "retained_bundle_id": "bundle.1",
                        "scene_revision": {
                            "content_hash": "content.1",
                            "revision_id": 1,
                            "scene_id": "scene.1",
                        },
                    }
                },
                "snapshot": {
                    "objects": [
                        {
                            "scene_resource_id": "isaac_cheese_box",
                            "placement": {
                                "kind": "carried",
                                "carried_object_attachment_id": "attachment.1",
                                "carried_object_attachment_revision": "revision.1",
                            },
                        }
                    ]
                },
            }
        }
    }

    class Client:
        @staticmethod
        def resolve_scene_resources(body: dict) -> dict:
            del body
            raise live.HeadlessClientError(
                "missing_reference",
                "active native projections disagree on scene-frame meaning",
                500,
                details={"reason": "scene_frame_resolution_ambiguous"},
            )

    evidence = live.RunEvidence(
        gripper_targets=(), object_last_position=(1.0, 2.0, 3.0)
    )
    assert live._sample_position(  # noqa: SLF001
        evidence, Client(), scene, "isaac_cheese_box"
    ) == (1.0, 2.0, 3.0)
    assert evidence.pose_query_rejections == 1


def test_scene_motion_sample_uses_bound_snapshot_during_state_reconciliation(
    live: ModuleType,
) -> None:
    scene = {
        "logical_scene": {
            "payload": {
                "access": {
                    "active_projection": {
                        "graph_revision": "graph.1",
                        "retained_bundle_id": "bundle.1",
                        "scene_revision": {
                            "content_hash": "content.1",
                            "revision_id": 1,
                            "scene_id": "scene.1",
                        },
                    }
                },
                "snapshot": {
                    "objects": [
                        {
                            "scene_resource_id": "isaac_cheese_box",
                            "placement": {
                                "pose_in_cell": {
                                    "position_m": {
                                        "x": 1.0,
                                        "y": 2.0,
                                        "z": 3.0,
                                    }
                                }
                            },
                        }
                    ]
                },
            }
        }
    }

    class Client:
        @staticmethod
        def resolve_scene_resources(body: dict) -> dict:
            del body
            raise live.HeadlessClientError(
                "invalid_request",
                "observed state differs from the retained path expectation",
                400,
                details={
                    "reason": "kinematic_state_publication_mismatch",
                    "mismatch_field": "publication_invalid:observation_stale",
                },
            )

    evidence = live.RunEvidence(gripper_targets=())
    assert live._sample_position(  # noqa: SLF001
        evidence, Client(), scene, "isaac_cheese_box"
    ) == (1.0, 2.0, 3.0)
    assert evidence.pose_query_rejections == 1


def test_scene_motion_sample_rejects_unclassified_state_mismatch(
    live: ModuleType,
) -> None:
    scene = {
        "logical_scene": {
            "payload": {
                "access": {
                    "active_projection": {
                        "graph_revision": "graph.1",
                        "retained_bundle_id": "bundle.1",
                        "scene_revision": {
                            "content_hash": "content.1",
                            "revision_id": 1,
                            "scene_id": "scene.1",
                        },
                    }
                }
            }
        }
    }

    class Client:
        @staticmethod
        def resolve_scene_resources(body: dict) -> dict:
            del body
            raise live.HeadlessClientError(
                "invalid_request",
                "observed state differs from the retained path expectation",
                400,
                details={"reason": "kinematic_state_publication_mismatch"},
            )

    evidence = live.RunEvidence(gripper_targets=())
    with pytest.raises(live.HeadlessClientError):
        live._sample_position(  # noqa: SLF001
            evidence, Client(), scene, "isaac_cheese_box"
        )
    assert evidence.pose_query_rejections == 0


def test_check_reducer_requires_source_matched_foundationpose_latch(
    live: ModuleType,
) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-lite6-trocar-foundationpose"]
    observations = base_observations(live, scenario)
    observations["run_evidence"].foundationpose_provider_match = False
    checks = live._build_checks(scenario, **observations)  # noqa: SLF001
    assert checks["foundationpose_detection_fresh"] is True
    assert checks["foundationpose_pose_latched"] is False


def test_trace_probe_uses_the_headless_history_page_bound(live: ModuleType) -> None:
    class Client:
        history_params = None

        @staticmethod
        def alarms(*, limit: int, ordering: str):
            assert limit == 128
            assert ordering == "attention_newest"
            return {"alarms": []}

        def history(self, *, params):
            self.history_params = params
            return {"events": [{"event": {"program_run_id": "program-run.1"}}]}

    client = Client()
    alarms, history = live._wait_for_trace_evidence(  # noqa: SLF001
        client,
        live.SCENARIO_BY_ID["isaac-lite6-pick-place"],
        "program-run.1",
        timeout_s=1.0,
    )

    assert client.history_params == (("limit", 32),)
    assert alarms == {"alarms": []}
    assert live._history_program_run_ids(history) == {"program-run.1"}  # noqa: SLF001


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


def test_cclc_startup_uses_fresh_owner_templates_and_full_binding_sets(
    live: ModuleType,
) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-lite6-pick-place"]

    class Client:
        def __init__(self) -> None:
            self.state = "declared"
            self.generation = 1
            self.execution_status_calls = 0
            self.lifecycle_requests = []
            self.route_requests = []
            self.lifecycle_statuses = {}
            self.route_statuses = {}
            self.active = {}
            self.bindings = {
                "domain.arm": [
                    {
                        "control_binding_id": "binding.arm.execute",
                        "control_binding_revision": "revision.arm.execute",
                        "control_route_id": "route.arm",
                    },
                    {
                        "control_binding_id": "binding.arm.plan",
                        "control_binding_revision": "revision.arm.plan",
                        "control_route_id": "route.arm",
                    },
                ],
                "domain.gripper": [
                    {
                        "control_binding_id": "binding.gripper.command",
                        "control_binding_revision": "revision.gripper.command",
                        "control_route_id": "route.gripper",
                    }
                ],
            }

        def execution_status(self):
            self.execution_status_calls += 1
            ready = self.execution_status_calls >= 3
            return {
                "ok": True,
                "command": "status",
                "status": {
                    "owner_epoch": "kernel-owner.runtime.1",
                    "ros2_control_lifecycle_observation": {
                        "schema_version": (
                            live._ROS2_CONTROL_LIFECYCLE_OBSERVATION_SCHEMA
                        ),
                        "ready": ready,
                        "domains": [
                            {
                                "component_lifecycle_state": (
                                    "active" if ready else "degraded"
                                ),
                                "control_domain_id": domain_id,
                                "exact_controller_configuration_active": ready,
                                "facts": [
                                    {
                                        "lifecycle_fact_id": fact_id,
                                        "value": value if ready else False,
                                    }
                                    for fact_id, value in sorted(
                                        live._ROS2_CONTROL_LIFECYCLE_READY_FACTS.items()
                                    )
                                ],
                                "ready": ready,
                                "source_generation": self.execution_status_calls,
                            }
                            for domain_id in sorted(self.bindings)
                        ],
                    },
                },
            }

        def cclc_component(self, component_id):
            assert component_id == scenario.component_id
            return {
                "controllable_component_id": component_id,
                "component_lifecycle_state": self.state,
                "component_lifecycle_generation": self.generation,
                "graph_revision": "graph.1",
                "domains": [
                    {
                        "active_control_configuration": self.active.get(
                            domain_id
                        ),
                        "control_domain_id": domain_id,
                        "control_domain_revision": f"revision.{domain_id}",
                        "controller_topology_id": f"topology.{domain_id}",
                        "topology_revision": f"topology-revision.{domain_id}",
                        "current_control_bindings": bindings,
                        "current_resources": [],
                    }
                    for domain_id, bindings in sorted(self.bindings.items())
                ],
            }

        def submit_component_lifecycle(self, body):
            self.lifecycle_requests.append(body)
            assert body["confirmed"] is True
            assert body["expected_component_lifecycle_generation"] == self.generation
            self.generation += 1
            self.state = {
                "prepare": "prepared",
                "enable": "active",
            }[body["intent"]]
            transaction_id = f"lifecycle.{body['intent']}"
            self.lifecycle_statuses[transaction_id] = {
                "controllable_component_id": scenario.component_id,
                "current_state": self.state,
                "terminal_result": "succeeded",
            }
            return {
                "acceptance": {
                    "component_lifecycle_generation": self.generation,
                    "lifecycle_transaction_id": transaction_id,
                    "request_id": body["request_context"]["request_id"],
                },
                "replay_status": "accepted_new",
            }

        def component_lifecycle_status(self, transaction_id):
            return {"status": self.lifecycle_statuses[transaction_id]}

        def cclc_route_templates(self, component_id):
            assert component_id == scenario.component_id
            return {
                "controllable_component_id": component_id,
                "retained_bundle_id": "bundle.1",
                "graph_revision": "graph.1",
                "templates": [
                    {
                        "desired_configuration": {
                            "control_domain_id": domain_id,
                            "control_domain_revision": f"revision.{domain_id}",
                            "controller_topology_id": f"topology.{domain_id}",
                            "topology_revision": f"topology-revision.{domain_id}",
                            "activation_set_id": f"activation.{domain_id}",
                            "binding_routes": bindings,
                        },
                        "expected_active_control_configuration": (
                            {"presence": "absent"}
                            if domain_id not in self.active
                            else {
                                "presence": "present",
                                "active_control_configuration_id": self.active[
                                    domain_id
                                ]["active_control_configuration_id"],
                                "active_control_configuration_generation": self.active[
                                    domain_id
                                ]["active_control_configuration_generation"],
                            }
                        ),
                        "component_expectations": [
                            {
                                "controllable_component_id": component_id,
                                "expected_component_lifecycle_generation": (
                                    self.generation
                                ),
                                "expected_component_lifecycle_state": self.state,
                            }
                        ],
                    }
                    for domain_id, bindings in sorted(self.bindings.items())
                ],
            }

        def submit_route_selection(self, body):
            self.route_requests.append(body)
            assert body["confirmed"] is True
            desired = body["desired_configuration"]
            domain_id = desired["control_domain_id"]
            transaction_id = f"route.{domain_id}"
            active = {
                **desired,
                "active_control_configuration_id": f"configuration.{domain_id}",
                "active_control_configuration_generation": 1,
                "source_runtime_epoch": "runtime.epoch.1",
                "source_runtime_unit_id": "runtime.unit.1",
                "source_sequence": 1,
                "state": "reconciled",
            }
            self.active[domain_id] = active
            self.route_statuses[transaction_id] = {
                "terminal_result": "succeeded",
                "resulting_active_control_configuration": active,
            }
            return {
                "acceptance": {
                    "route_selection_transaction_id": transaction_id,
                    "request_id": body["request_context"]["request_id"],
                },
                "replay_status": "accepted_new",
            }

        def route_selection_status(self, transaction_id):
            return {"status": self.route_statuses[transaction_id]}

    client = Client()
    evidence = live._prepare_and_select_cclc_routes(  # noqa: SLF001
        client, scenario, timeout_s=1.0
    )

    assert evidence["passed"] is True
    assert evidence["initial"] == {"generation": 1, "state": "declared"}
    assert evidence["final"] == {"generation": 3, "state": "active"}
    assert evidence["external_readiness"]["ready"] is True
    assert client.execution_status_calls >= 3
    assert [row["intent"] for row in evidence["lifecycle"]] == [
        "prepare",
        "enable",
    ]
    assert [row["control_domain_id"] for row in evidence["routes"]] == [
        "domain.arm",
        "domain.gripper",
    ]
    assert [row["binding_count"] for row in evidence["routes"]] == [2, 1]
    assert all(request["confirmed"] is True for request in client.route_requests)
    assert all(
        request["request_context"]["request_owner_epoch"]
        == "kernel-owner.runtime.1"
        for request in client.lifecycle_requests + client.route_requests
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
    assert "scene_service_timeout_s:=120.0" in ros
    assert "additional_required_topics:=/tf" in ros
    assert not any("DemoSupervisor" in item for item in ros)
    headless = commands["headless"]
    timeout_index = headless.index("--manager-timeout-s")
    assert headless[timeout_index + 1] == "12.0"


def test_recovery_evidence_requires_new_provider_epoch_and_bounded_outage(
    live: ModuleType,
) -> None:
    evidence = live.RecoveryEvidence(
        component_id="isaac_lite6_trocar_robot",
        provider_epoch_before="pose.epoch.1",
        provider_sequence_before=7,
        provider_epoch_after="pose.epoch.2",
        provider_sequence_after=1,
        dds_loss_elapsed_s=1.0,
        dds_rejoin_elapsed_s=2.0,
        provider_loss_elapsed_s=5.1,
        provider_recovery_elapsed_s=2.5,
        output_sample={"seen": True, "sha256": "a" * 64},
        exact_world_after_rejoin=True,
        simulation_stopped_during_outage=True,
        simulation_stopped_after_rejoin=True,
    )
    assert evidence.to_dict()["passed"] is True

    same_epoch = live.RecoveryEvidence(
        **{
            field: getattr(evidence, field)
            for field in evidence.__dataclass_fields__
            if field != "provider_epoch_after"
        },
        provider_epoch_after="pose.epoch.1",
    )
    assert same_epoch.to_dict()["passed"] is False

    late = live.RecoveryEvidence(
        **{
            field: getattr(evidence, field)
            for field in evidence.__dataclass_fields__
            if field != "dds_rejoin_elapsed_s"
        },
        dds_rejoin_elapsed_s=live._RECOVERY_BOUND_S + 0.001,  # noqa: SLF001
    )
    assert late.to_dict()["passed"] is False


def test_foundationpose_loss_rejects_fresh_or_ambiguous_sources(
    live: ModuleType,
) -> None:
    scenario = live.SCENARIO_BY_ID["isaac-lite6-trocar-foundationpose"]
    scene = base_observations(live, scenario)["scene_after"]
    assert live._foundationpose_loss_projected(  # noqa: SLF001
        scene, source_epoch="pose.epoch.1"
    ) is False

    source = scene["scene_runtime"]["payload"]["state"]["pose_sources"][0]
    source["fresh"] = False
    assert live._foundationpose_loss_projected(  # noqa: SLF001
        scene, source_epoch="pose.epoch.1"
    ) is True

    duplicate = dict(source)
    duplicate["source_epoch"] = "pose.epoch.crossed"
    scene["scene_runtime"]["payload"]["state"]["pose_sources"].append(
        duplicate
    )
    assert live._foundationpose_identity(  # noqa: SLF001
        scene, require_fresh=False
    ) is None
    assert live._foundationpose_loss_projected(  # noqa: SLF001
        scene, source_epoch="pose.epoch.1"
    ) is False
