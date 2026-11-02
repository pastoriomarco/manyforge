"""Source-level contract checks for the bounded UI6-I dual-robot fixture."""

from __future__ import annotations

from pathlib import Path

from manyforge_behavior.canonical_artifact import load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program


ROOT = Path(__file__).resolve().parents[2]
PROGRAMS = {
    "grouped": ROOT / "examples/ur5e_lite6_wall_grouped.program.yaml",
    "isolation": ROOT / "examples/ur5e_lite6_wall_isolation.program.yaml",
}
SCENARIO_NAMES = {
    "grouped": "mock_dual_arm_shared_group",
    "isolation": "mock_dual_arm_separate_groups",
}
SCENARIOS = {
    variant: ROOT / f"examples/{name}.scenario" for variant, name in SCENARIO_NAMES.items()
}


def test_dual_deployment_has_exact_models_and_placements() -> None:
    for path in SCENARIOS.values():
        deployment = load_strict_json_yaml(path / "deployment.yaml")
        rows = {row["component_id"]: row for row in deployment["components"]}
        # AUD-5: the Lite 6 is a machine; its arm keeps the robot's id and
        # its I/O members are deactivated (the scenario uses none).
        assert {
            component: (row["component_profile_id"], row.get("owner"), row.get("active", True))
            for component, row in rows.items() if "model_source" not in row
        } == {
            "lite6": ("ufactory.lite6_machine", None, True),
            "lite6_robot_controller_io": ("ufactory.lite6_controller_io", "lite6", False),
            "lite6_robot_tool_io": ("ufactory.lite6_tool_io", "lite6", False),
        }
        assert (rows["lite6_robot"]["owner"], rows["lite6_robot"]["role"]) == ("lite6", "arm")
        components = {
            component: row for component, row in rows.items() if "model_source" in row}
        # Merge 6: each robot carries its own tool, mounted on its flange; a
        # mounted tool has no cell pose of its own.
        assert {
            component: row["root_pose_in_cell"]["position_m"]
            for component, row in components.items()
            if "root_pose_in_cell" in row
        } == {"lite6_robot": [0.0, 0.0, 0.0], "ur5e_robot": [0.0, 1.0, 0.0]}
        # Mount collision exclusions are checked by the mount-exclusion tests.
        assert [
            {key: value for key, value in row.items() if key != "collision_exclusions"}
            for row in deployment["attachments"]
        ] == [
            {"child": {"component_id": "lite_gripper", "interface": "mount"},
             "parent": {"component_id": "lite6_robot", "interface": "flange"}},
            {"child": {"component_id": "robotiq_2f_85", "interface": "mount"},
             "parent": {"component_id": "ur5e_robot", "interface": "flange"}},
        ]
        assert {
            component: row["component_profile_id"]
            for component, row in components.items()
        } == {
            "lite6_robot": "ufactory.lite6",
            "lite_gripper": "ufactory.lite_gripper",
            "robotiq_2f_85": "robotiq.robotiq_2f_85",
            "ur5e_robot": "universal_robots.ur5e",
        }
        assert {
            component: row["model_source"]["urdf"]["project_path"]
            for component, row in components.items()
        } == {
            component: f"../../manyforge_example_models/models/{model}/{model}.urdf"
            for component, model in (
                ("lite6_robot", "lite6"),
                ("lite_gripper", "lite_gripper"),
                ("robotiq_2f_85", "robotiq_2f_85"),
                ("ur5e_robot", "ur5e"),
            )
        }
        # Q63-Q68: every component runs on the offline mock lane.
        assert {rows[row.get("owner", component)]["lane"]
                for component, row in components.items()} == {"mock"}
        assert {
            component for component, row in components.items()
            if "arm" in row["target_settings"]
        } == {"lite6_robot", "ur5e_robot"}
        # CP9: each Mock robot declares its measured joints; each Mock tool its
        # measured joints and gripper feedback.
        assert sorted(
            (stream["controllable_component_id"], stream["stream_id"], stream["budget_ns"])
            for row in components.values()
            for stream in row["configuration"]["runtime_configuration"]["component_streams"]
        ) == [
            ("lite6_robot", "measured_joints", 250000000),
            ("lite_gripper", "gripper_feedback", 1000000000),
            ("lite_gripper", "measured_joints", 250000000),
            ("robotiq_2f_85", "gripper_feedback", 1000000000),
            ("robotiq_2f_85", "measured_joints", 250000000),
            ("ur5e_robot", "measured_joints", 250000000),
        ]
        # Gripper model A1: the Lite6 lite gripper is pneumatic, the Robotiq
        # 2F-85 is position commanded.
        grippers = {
            component: row["configuration"]["runtime_configuration"][
                "gripper_target_configurations"]
            for component, row in components.items()
            if "gripper_target_configurations"
            in row["configuration"]["runtime_configuration"]
        }
        assert {
            component: [gripper["actuation_kind"] for gripper in rows]
            for component, rows in grippers.items()
        } == {"lite_gripper": ["pneumatic"], "robotiq_2f_85": ["position_commanded"]}
        assert all(
            row["command_target_id"] == "gripper"
            and row["open_state_ref"] == "open"
            and row["closed_state_ref"] == "closed"
            and row["mount_frame"]
            and row["attach_frame"]
            and row["tcp_frame"]
            and row["touch_links"]
            for rows in grippers.values()
            for row in rows
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
        assert manifest["scenario_id"] == SCENARIO_NAMES[variant]
        deployment = load_strict_json_yaml(path / "deployment.yaml")
        assert deployment["api_version"] == "manyforge.deployment_intent.v2"
        assert deployment["deployment_artifact_id"] == "ur5e_lite6_wall"
        # M6 tool split: each robot's gripper is its own mounted component.
        # The Lite 6 is a machine (CONTRACTS §16): its owner and every member.
        assert {row["component_id"] for row in deployment["components"]} == {
            "lite6",
            "lite6_robot",
            "lite6_robot_controller_io",
            "lite6_robot_tool_io",
            "lite_gripper",
            "robotiq_2f_85",
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
