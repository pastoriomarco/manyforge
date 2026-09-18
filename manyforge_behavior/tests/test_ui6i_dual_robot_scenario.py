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
ROLE_ASSIGNMENTS = (
    ROOT
    / "manyforge_deployment/resources/maintained/"
    "maintained_program_role_assignments.v1.json"
)


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
    assert runtime["runtime_unit_id"] == "robot_cell.native_execution"
    assert {
        row["controllable_component_id"]
        for row in runtime["configuration"]["motion_target_limits"]
    } == {"lite6_robot", "ur5e_robot"}
    grippers = {
        row["controllable_component_id"]: row
        for row in runtime["configuration"]["gripper_target_configurations"]
    }
    assert set(grippers) == {"lite6_robot", "ur5e_robot"}
    assert all(
        row["command_target_id"] == "gripper"
        and row["actuation_kind"] == "position_commanded"
        and row["open_state_ref"] == "open"
        and row["closed_state_ref"] == "closed"
        and row["mount_frame"]
        and row["attach_frame"]
        and row["tcp_frame"]
        and row["touch_links"]
        for row in grippers.values()
    )
    assert all(
        row["runtime_unit_ids"] == ["robot_cell.native_execution"]
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
            assert runtime_sources == {"robot_cell.native_execution"}
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

    policy = load_strict_json_yaml(
        ROOT
        / "manyforge_deployment/resources/maintained/catalog/runtime_policy/"
        "manyforge.runtime_motion.ur5e_lite6_wall.json"
    )
    assert "motion_speed_limits" not in policy
    retained = deployment["policies"]["retained_policy_records"]
    assert len(retained) == 1
    assert retained[0]["record_id"] == policy["runtime_policy_id"]
    assert retained[0]["record_revision"] == policy["runtime_policy_revision"]


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
    assignments = load_strict_json_yaml(ROLE_ASSIGNMENTS)["assignments"]
    selected = {
        row["scenario_id"]: row
        for row in assignments
        if row["scenario_id"].startswith("ur5e_lite6_wall_")
    }
    assert set(selected) == {
        "ur5e_lite6_wall_grouped",
        "ur5e_lite6_wall_isolation",
    }
    for scenario_id, row in selected.items():
        program = load_strict_json_yaml(PROGRAMS[scenario_id.rsplit("_", 1)[1]])
        assert row["deployment_source"] == (
            "examples/ur5e_lite6_wall.deployment.yaml"
        )
        assert row["program_source"] == (
            f"examples/{scenario_id}.program.yaml"
        )
        assert row["program_revision"] == program["program_revision"]
        assert row["component_roles"] == {
            "lite6_robot": "lite6_robot",
            "ur5e_robot": "ur5e_robot",
        }
        assert row["target_roles"] == {
            "lite6_arm": {
                "component_role": "lite6_robot",
                "command_target_id": "arm",
            },
            "ur5e_arm": {
                "component_role": "ur5e_robot",
                "command_target_id": "arm",
            },
        }
