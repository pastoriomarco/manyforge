"""Source-level contract checks for the bounded UI6-I dual-robot fixture."""

from __future__ import annotations

from pathlib import Path

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program


ROOT = Path(__file__).resolve().parents[2]
DEPLOYMENT = ROOT / "examples/ur5e_lite6_wall.deployment.yaml"
PROGRAMS = {
    "grouped": ROOT / "examples/ur5e_lite6_wall_grouped.program.yaml",
    "isolation": ROOT / "examples/ur5e_lite6_wall_isolation.program.yaml",
}
SCENARIOS = {
    variant: ROOT / f"examples/ur5e_lite6_wall_{variant}.scenario"
    for variant in PROGRAMS
}


def test_dual_deployment_has_exact_models_placements_and_cross_revisions() -> None:
    deployment = load_strict_json_yaml(DEPLOYMENT)
    graph = deployment["graph"]
    components = {
        row["controllable_component_id"]: row
        for row in graph["controllable_components"]
    }
    assert {
        component: row["root_pose_in_cell"]["position_m"]
        for component, row in components.items()
    } == {"lite6_robot": [0, 0, 0], "ur5e_robot": [0, 1, 0]}
    assert {
        row["model_asset_id"] for row in graph["model_instances"]
    } == {"lite6_lite_gripper", "ur5e_robotiq_2f_85"}
    assert len(graph["runtime_units"]) == 1
    runtime = graph["runtime_units"][0]
    # Q63-Q68: both robots run on the offline mock lane in one ROS runtime
    # unit, each with its own controller profile.
    assert runtime["runtime_unit_id"] == "robot_cell.ros2_control_execution"
    assert runtime["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime"
    assert runtime["configuration"]["controller_profiles"] == [
        {"controllable_component_id": "lite6_robot", "frame_prefix": "", "mock_profile": "lite6_lite_gripper"},
        {"controllable_component_id": "ur5e_robot", "frame_prefix": "", "mock_profile": "ur5e_robotiq_2f_85"},
    ]
    assert {
        row["controllable_component_id"]
        for row in runtime["configuration"]["motion_target_limits"]
    } == {"lite6_robot", "ur5e_robot"}
    grippers = {
        row["controllable_component_id"]: row
        for row in runtime["configuration"]["gripper_target_configurations"]
    }
    assert set(grippers) == {"lite6_robot", "ur5e_robot"}
    # CP9: each Mock robot declares its measured joints and gripper feedback.
    assert [
        (row["controllable_component_id"], row["stream_id"], row["budget_ns"],
         row["severity"], row["recovery"])
        for row in runtime["configuration"]["component_streams"]
    ] == [
        (component, stream, budget, "fault", "ack")
        for component in ("lite6_robot", "ur5e_robot")
        for stream, budget in (("gripper_feedback", 1000000000), ("measured_joints", 250000000))
    ]
    # Gripper model A1: the Lite6 lite gripper is pneumatic, the Robotiq
    # 2F-85 is position commanded.
    assert {
        component_id: row["actuation_kind"] for component_id, row in grippers.items()
    } == {"lite6_robot": "pneumatic", "ur5e_robot": "position_commanded"}
    assert all(
        row["command_target_id"] == "gripper"
        and row["open_state_ref"] == "open"
        and row["closed_state_ref"] == "closed"
        and row["mount_frame"]
        and row["attach_frame"]
        and row["tcp_frame"]
        and row["touch_links"]
        for row in grippers.values()
    )
    assert all(
        row["runtime_unit_ids"] == ["robot_cell.ros2_control_execution"]
        for row in components.values()
    )

    bindings = {
        row["control_binding_id"]: row for row in graph["control_bindings"]
    }
    domains = {row["control_domain_id"]: row for row in graph["control_domains"]}
    for provider in deployment["capability_provider_bindings"]:
        runtime_sources = {
            endpoint["source_id"]
            for endpoint in provider["endpoint_bindings"]
            if endpoint["source_kind"] == "runtime_unit"
        }
        if runtime_sources:
            assert runtime_sources == {"robot_cell.ros2_control_execution"}
        if provider["scope"] != "target_binding":
            continue
        target = provider["target"]
        assert target["component_type_revision"] == components[
            target["controllable_component_id"]
        ]["component_type_revision"]
        assert target["control_binding_revision"] == bindings[
            target["control_binding_id"]
        ]["control_binding_revision"]
    for desired in deployment["desired_control_configurations"]:
        assert desired["control_domain_revision"] == domains[
            desired["control_domain_id"]
        ]["control_domain_revision"]
        for route in desired["binding_routes"]:
            assert route["control_binding_revision"] == bindings[
                route["control_binding_id"]
            ]["control_binding_revision"]
    for row in deployment["policies"]["stop_stage_parameters"]:
        assert row["control_binding_revision"] == bindings[
            row["control_binding_id"]
        ]["control_binding_revision"]
        assert row["control_domain_revision"] == domains[row["control_domain_id"]][
            "control_domain_revision"
        ]

    assert deployment["policies"]["retained_policy_records"] == []
    assert all(
        row["record_kind"] != "runtime_policy"
        for row in deployment["catalogs"]["records"]
    )


def test_program_variants_keep_one_noninteracting_wall_scenario() -> None:
    catalog = NodeCatalog(ROOT / "manyforge_behavior/resources/node_catalog.yaml")
    loaded = {
        name: Program.from_dict(load_strict_json_yaml(path), catalog)
        for name, path in PROGRAMS.items()
    }
    for program in loaded.values():
        document = program.to_dict()
        assert document["scene"] == [
            {
                "scene_resource_id": "ground",
                "shape": {"type": "box", "box_dimensions_m": [0.8, 2.0, 0.1]},
                "pose": {
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [0.0, 0.5, -0.051],
                    "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
                },
            },
            {
                "scene_resource_id": "separation_wall",
                "shape": {"type": "box", "box_dimensions_m": [0.8, 0.02, 0.8]},
                "pose": {
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [0.0, 0.5, 0.4],
                    "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
                },
            },
        ]
        assert document["tree"]["node_id"] == "parallel"
        assert document["tree"]["parameters"] == {"success_policy": "all"}
        children = document["tree"]["children"]
        assert len(children) == 2
        assert {child["node_id"] for child in children} == {
            "motion.move_to_joint_target"
        }
        assert {child["parameters"]["target_role"] for child in children} == {
            "lite6_arm",
            "ur5e_arm",
        }

    assert loaded["grouped"].execution_group_policies == {
        "robot_group": "independent"
    }
    assert {
        tuple(child.execution_groups) for child in loaded["grouped"].tree.children
    } == {("robot_group",)}
    isolation_groups = {
        tuple(child.execution_groups)
        for child in loaded["isolation"].tree.children
    }
    assert isolation_groups == {("lite6_arm",), ("ur5e_arm",)}


def test_both_variants_are_registered_as_qualified_scenarios() -> None:
    for variant, path in SCENARIOS.items():
        manifest = load_strict_json_yaml(path / "scenario.yaml")
        assert manifest["scenario_id"] == f"ur5e_lite6_wall_{variant}"
        deployment = load_strict_json_yaml(path / "deployment.yaml")
        assert deployment["api_version"] == "manyforge.deployment_intent.v2"
        assert deployment["deployment_artifact_id"] == "ur5e_lite6_wall"
        assert {row["component_id"] for row in deployment["components"]} == {
            "lite6_robot",
            "ur5e_robot",
        }
        assignment = load_strict_json_yaml(path / "assignment.yaml")
        assert assignment["component_roles"] == [
            {
                "component_role_id": "lite6_robot",
                "controllable_component_id": "lite6_robot",
            },
            {
                "component_role_id": "ur5e_robot",
                "controllable_component_id": "ur5e_robot",
            },
        ]
        assert assignment["target_roles"] == [
            {"target_role_id": "lite6_arm", "command_target_id": "arm"},
            {"target_role_id": "ur5e_arm", "command_target_id": "arm"},
        ]
