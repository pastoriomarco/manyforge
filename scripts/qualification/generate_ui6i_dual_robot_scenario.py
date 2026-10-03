#!/usr/bin/env python3
"""Generate the small UI6-I UR5e + Lite6 wall qualification scenario.

The Deployment is composed through the maintained occurrence builder from the
retained Lite6 and UR5e model families and their pinned runtime inputs.
Revision calculation remains owned by the normal Deployment and Program
authoring tools.
"""

from __future__ import annotations

import argparse
import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Final

import yaml


ZERO_DIGEST: Final = "sha256:" + "0" * 64
CELL_ID: Final = "ui6i_ur5e_lite6_wall_cell"
DEPLOYMENT_SOURCE: Final = "examples/ur5e_lite6_wall.deployment.yaml"
GROUPED_PROGRAM_SOURCE: Final = "examples/ur5e_lite6_wall_grouped.program.yaml"
ISOLATION_PROGRAM_SOURCE: Final = "examples/ur5e_lite6_wall_isolation.program.yaml"
GROUPED_SCENARIO_SOURCE: Final = "examples/ur5e_lite6_wall_grouped.scenario"
ISOLATION_SCENARIO_SOURCE: Final = "examples/ur5e_lite6_wall_isolation.scenario"
GRAPH_ID_FIELDS: Final = {
    "runtime_units": "runtime_unit_id",
    "model_instances": "model_instance_id",
    "controllable_components": "controllable_component_id",
    "control_domains": "control_domain_id",
    "control_bindings": "control_binding_id",
}


class GenerationError(ValueError):
    """Raised when the retained inputs or generated outputs are inconsistent."""


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GenerationError(f"cannot read {path}: {error}") from error
    if type(value) is not dict:
        raise GenerationError(f"{path} must contain an object")
    return value


def read_yaml(path: Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as error:
        raise GenerationError(f"cannot read {path}: {error}") from error
    if type(value) is not dict:
        raise GenerationError(f"{path} must contain an object")
    return value


def unique_records(records: list[dict[str, Any]]) -> list[dict[str, Any]]:
    selected: dict[tuple[str, str, str], dict[str, Any]] = {}
    for row in records:
        key = (row["record_kind"], row["record_id"], row["record_revision"])
        existing = selected.setdefault(key, row)
        if existing != row:
            raise GenerationError(f"catalog reference {key} has conflicting content")
    return sorted(
        selected.values(),
        key=lambda row: (
            row["record_kind"],
            row["record_id"],
            row["record_revision"],
        ),
    )


def target_bindings(deployment: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        copy.deepcopy(row)
        for row in deployment["capability_provider_bindings"]
        if row["scope"] == "target_binding"
    ]


def cell_bindings(deployment: dict[str, Any]) -> list[dict[str, Any]]:
    return [
        copy.deepcopy(row)
        for row in deployment["capability_provider_bindings"]
        if row["scope"] == "cell_global"
    ]


def build_deployment(root: Path) -> dict[str, Any]:
    generator_dir = root / "manyforge_deployment/tools"
    sys.path.insert(0, str(generator_dir))
    # pylint: disable-next=import-outside-toplevel
    from generate_maintained_deployment_occurrences import (
        catalog_sources,
        deployment_candidate,
    )

    description = read_json(
        root
        / "manyforge_deployment/resources/maintained/"
        "maintained_deployment_sources.v1.json"
    )
    sources, _, _ = catalog_sources(root, description)
    refs = {key: value[2] for key, value in sources.items()}
    families = {
        row["model_family_id"]: row for row in description["model_families"]
    }
    runtime_profiles = {
        row["runtime_restoration_profile_id"]: row
        for row in description["runtime_restoration_profiles"]
    }

    def occurrence(
        *, component: str, family: str, runtime_profile: str, y_m: float
    ) -> dict[str, Any]:
        definition = {
            "deployment_artifact_id": "ur5e_lite6_wall",
            "cell_id": CELL_ID,
            "name": "ur5e_lite6_wall",
            "description": "UI6-I dual-robot wall qualification occurrence",
            "component_occurrence_id": component,
            "runtime_restoration_profile_id": runtime_profile,
            # T qualifies cell-level grouped execution. Native lifecycle is
            # selected by the same verified provider product for both robots.
            "assistant_profile_id": None,
            "root_pose_in_cell": {
                "position_m": [0.0, y_m, 0.0],
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
            },
            "compatibility": {
                "lane": "manyforge-only",
                "provides": [],
                "tags": ["dual-robot"],
            },
        }
        return deployment_candidate(
            definition,
            families[family],
            {},
            runtime_profiles,
            {"profiles": []},
            refs,
            {},
        )

    lite6 = occurrence(
        component="lite6_robot",
        family="lite6_lite_gripper",
        runtime_profile="lite6_native_simulated_v0",
        y_m=0.0,
    )
    ur5e = occurrence(
        component="ur5e_robot",
        family="ur5e_robotiq_2f_85",
        runtime_profile="ur5e_robotiq_native_simulated_v0",
        y_m=1.0,
    )
    candidates = (lite6, ur5e)
    candidates_by_component = {
        candidate["graph"]["controllable_components"][0][
            "controllable_component_id"
        ]: candidate
        for candidate in candidates
    }
    if set(candidates_by_component) != {"lite6_robot", "ur5e_robot"}:
        raise GenerationError("dual native scenario has unexpected component identities")

    graph = {
        collection: [
            copy.deepcopy(row)
            for candidate in candidates
            for row in candidate["graph"][collection]
        ]
        for collection in GRAPH_ID_FIELDS
    }
    graph["component_attachments"] = []
    for collection, id_field in GRAPH_ID_FIELDS.items():
        graph[collection].sort(key=lambda row: row[id_field])

    runtime_units = graph["runtime_units"]
    if len(runtime_units) != 2:
        raise GenerationError("dual native scenario requires exactly two source runtimes")
    configurations = [
        candidates_by_component[component]["graph"]["runtime_units"][0][
            "configuration"
        ]
        for component in sorted(candidates_by_component)
    ]
    for field in ("execution_tick_hz", "collision_monitor_period_ms", "mode"):
        if configurations[0][field] != configurations[1][field]:
            raise GenerationError(f"dual native runtimes disagree on {field}")
    combined_runtime_id = "robot_cell.native_execution"
    combined_configuration = copy.deepcopy(configurations[0])
    combined_configuration["initial_joint_positions_rad"] = [
        position
        for configuration in configurations
        for position in configuration["initial_joint_positions_rad"]
    ]
    for field in (
        "motion_target_limits",
        "direct_control_joint_limits",
        "gripper_target_configurations",
    ):
        combined_configuration[field] = sorted(
            [
                copy.deepcopy(item)
                for configuration in configurations
                for item in configuration.get(field, [])
            ],
            key=lambda item: (
                item["controllable_component_id"],
                item["command_target_id"],
                item.get("source_joint_name", ""),
            ),
        )
    graph["runtime_units"] = [
        {
            "configuration": combined_configuration,
            "dependency_runtime_unit_ids": sorted(
                {
                    dependency
                    for row in runtime_units
                    for dependency in row["dependency_runtime_unit_ids"]
                }
            ),
            "runtime_profile_id": runtime_units[0]["runtime_profile_id"],
            "runtime_profile_revision": runtime_units[0]["runtime_profile_revision"],
            "runtime_unit_id": combined_runtime_id,
        }
    ]

    source_runtime_ids = {
        "lite6_robot.native_execution",
        "ur5e_robot.native_execution",
    }

    def replace_runtime_unit(value: Any) -> None:
        if isinstance(value, dict):
            if (
                value.get("source_kind") == "runtime_unit"
                and value.get("source_id") in source_runtime_ids
            ):
                value["source_id"] = combined_runtime_id
            for key, child in value.items():
                if (
                    key in {"runtime_unit_id", "provider_runtime_unit_id"}
                    and child in source_runtime_ids
                ):
                    value[key] = combined_runtime_id
                elif key in {"runtime_unit_ids", "dependency_runtime_unit_ids"} and isinstance(
                    child, list
                ):
                    value[key] = sorted(
                        {
                            combined_runtime_id
                            if item in source_runtime_ids
                            else item
                            for item in child
                        }
                    )
                else:
                    replace_runtime_unit(child)
        elif isinstance(value, list):
            for child in value:
                replace_runtime_unit(child)

    replace_runtime_unit(graph)

    records = unique_records(
        [
            copy.deepcopy(row)
            for candidate in candidates
            for row in candidate["catalogs"]["records"]
        ]
    )
    desired = [
        copy.deepcopy(row)
        for candidate in candidates
        for row in candidate["desired_control_configurations"]
    ]
    desired.sort(key=lambda row: row["control_domain_id"])

    providers = cell_bindings(lite6)
    providers.extend(target_bindings(lite6))
    providers.extend(target_bindings(ur5e))
    replace_runtime_unit(providers)
    providers.sort(key=lambda row: row["capability_provider_binding_id"])

    if lite6["policies"]["interrupt_config"] != ur5e["policies"]["interrupt_config"]:
        raise GenerationError("dual native occurrences disagree on interrupt policy")
    policies = {
        "commissioned_stop_interfaces": [],
        "interrupt_config": copy.deepcopy(lite6["policies"]["interrupt_config"]),
        "interrupt_policies": [],
        "interrupt_sources": [],
        "retained_policy_records": [],
        "stop_stage_parameters": [
            copy.deepcopy(row)
            for candidate in candidates
            for row in candidate["policies"]["stop_stage_parameters"]
        ],
        "stop_timing_parameters": [
            copy.deepcopy(row)
            for candidate in candidates
            for row in candidate["policies"]["stop_timing_parameters"]
        ],
    }
    policies["stop_stage_parameters"].sort(
        key=lambda row: (
            row["control_binding_id"],
            row["stop_escalation_stage_id"],
        )
    )
    policies["stop_timing_parameters"].sort(key=lambda row: row["control_binding_id"])
    replace_runtime_unit(desired)
    replace_runtime_unit(providers)
    replace_runtime_unit(policies)

    return {
        "api_version": "manyforge.deployment.v1",
        "deployment_artifact_id": "ur5e_lite6_wall",
        "deployment_revision": ZERO_DIGEST,
        "cell_id": CELL_ID,
        "name": "ur5e_lite6_wall",
        "description": (
            "Minimal native UR5e and Lite6 qualification cell derived from the "
            "simple ManyMove dual-robot wall layout. The robots plan independently; "
            "both are owned by one cell execution Kernel and remain independently planned."
        ),
        "catalogs": {"records": unique_records(records)},
        "graph": graph,
        "desired_control_configurations": desired,
        "capability_provider_bindings": providers,
        "policies": policies,
        "compatibility": {
            "lane": "manyforge-only",
            "provides": ["lite_gripper", "robotiq_2f_85"],
            "tags": ["dual-robot", "grouped-execution", "lite6", "ur5e"],
        },
    }


def arm_requirement(source: dict[str, Any], component_role: str) -> dict[str, Any]:
    requirement = copy.deepcopy(source["target_roles"]["arm"])
    requirement["component_role"] = component_role
    return requirement


def motion_node(
    *, instance_id: str, target_role: str, goal: list[tuple[str, float]], group: str
) -> dict[str, Any]:
    return {
        "node_id": "motion.move_to_joint_target",
        "instance_id": instance_id,
        "parameters": {
            "target_role": target_role,
            "base_frame_role": "base",
            "tcp_frame_role": "tcp",
            "goal": [
                {"joint_name": joint_name, "position_rad": position}
                for joint_name, position in goal
            ],
            "planning_timeout_s": 10.0,
            "execution_timeout_s": 30.0,
        },
        "value_sources": {
            "acceleration_scale": {
                "kind": "program_parameter",
                "reference": "acceleration_scale",
            },
            "max_tcp_linear_speed_mps": {
                "kind": "program_parameter",
                "reference": "max_tcp_linear_speed_mps",
            },
            "translation_tol_m": {
                "kind": "program_parameter",
                "reference": "translation_tol_m",
            },
            "velocity_scale": {
                "kind": "program_parameter",
                "reference": "velocity_scale",
            },
        },
        "execution_groups": [group],
        "children": [],
    }


def build_program(root: Path, *, grouped: bool) -> dict[str, Any]:
    source = read_yaml(root / "examples/lite6_lite_gripper_pick_and_place.program.yaml")
    program_id = (
        "ur5e_lite6_wall_grouped" if grouped else "ur5e_lite6_wall_isolation"
    )
    lite6_group = "robot_group" if grouped else "lite6_arm"
    ur5e_group = "robot_group" if grouped else "ur5e_arm"
    result: dict[str, Any] = {
        "api_version": "manyforge.program.v2",
        "program_id": program_id,
        "program_revision": ZERO_DIGEST,
        "program_profile": "composable-cell-v1",
        "name": program_id,
        "description": (
            "Two independent joint trajectories in the UR5e + Lite6 wall cell, "
            + (
                "in one shared control group without synchronized timing."
                if grouped
                else "in separate control groups for the isolation case."
            )
        ),
        "catalog": copy.deepcopy(source["catalog"]),
        "compatibility": {
            "lanes": ["manyforge-only"],
            "assistant": "forbidden",
            "requires": [],
            "tags": ["dual-robot", "lite6", "ur5e"],
        },
        "component_roles": {
            "lite6_robot": {
                "required_component_type_tags": ["articulated-arm"],
                "required_model_roles": ["arm_model"],
            },
            "ur5e_robot": {
                "required_component_type_tags": ["articulated-arm"],
                "required_model_roles": ["arm_model"],
            },
        },
        "target_roles": {
            "lite6_arm": arm_requirement(source, "lite6_robot"),
            "ur5e_arm": arm_requirement(source, "ur5e_robot"),
        },
        "scene": [
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
                "shape": {
                    "type": "box",
                    "box_dimensions_m": [0.8, 0.02, 0.8],
                },
                "pose": {
                    "frame_ref": {"kind": "cell_frame"},
                    "position_m": [0.0, 0.5, 0.4],
                    "orientation": {"rpy_deg": [0.0, 0.0, 0.0]},
                },
            },
        ],
        "parameters": [
            {
                "parameter_id": "acceleration_scale",
                "type": "float64",
                "default": 0.5,
                "constraints": {"min_exclusive": 0.0, "max_inclusive": 1.0},
                "description": "Acceleration scale shared by the two planned motions.",
            },
            {
                "parameter_id": "max_blend_deviation_rad",
                "type": "float64",
                "default": 0.01,
                "constraints": {"min_inclusive": 0.0},
                "description": "Default maximum Cartesian blend deviation.",
            },
            {
                "parameter_id": "max_tcp_linear_speed_mps",
                "type": "float64",
                "default": 0.15,
                "constraints": {"min_exclusive": 0.0},
                "description": "Maximum TCP linear speed shared by the two robots.",
            },
            {
                "parameter_id": "translation_tol_m",
                "type": "float64",
                "default": 0.005,
                "constraints": {"min_exclusive": 0.0},
                "description": "Default Cartesian translation tolerance.",
            },
            {
                "parameter_id": "velocity_scale",
                "type": "float64",
                "default": 0.35,
                "constraints": {"min_exclusive": 0.0, "max_inclusive": 1.0},
                "description": "Velocity scale shared by the two planned motions.",
            },
        ],
        "execution_groups": {
            name: {"execution_policy": "independent"}
            for name in sorted({lite6_group, ur5e_group})
        },
        "tree": {
            "node_id": "parallel",
            "instance_id": "paired_joint_motion",
            "parameters": {"success_policy": "all"},
            "children": [
                motion_node(
                    instance_id="lite6_joint_motion",
                    target_role="lite6_arm",
                    group=lite6_group,
                    goal=[
                        ("joint1", 0.3),
                        ("joint2", -0.55),
                        ("joint3", 1.05),
                        ("joint4", 0.0),
                        ("joint5", 1.35),
                        ("joint6", 0.0),
                    ],
                ),
                motion_node(
                    instance_id="ur5e_joint_motion",
                    target_role="ur5e_arm",
                    group=ur5e_group,
                    goal=[
                        ("shoulder_pan_joint", -0.45),
                        ("shoulder_lift_joint", -1.35),
                        ("elbow_joint", 1.2),
                        ("wrist_1_joint", -1.4),
                        ("wrist_2_joint", -1.57),
                        ("wrist_3_joint", 0.25),
                    ],
                ),
            ],
        },
    }
    return result


def run_tool(tool: Path, mode: str, source: Path) -> None:
    result = subprocess.run(
        [str(tool), mode, "--input", str(source)],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise GenerationError(
            f"{tool.name} {mode} failed for {source}: {result.stderr.strip()}"
        )


def sealed_deployment(
    deployment: dict[str, Any], *, tool: Path, temporary: Path
) -> bytes:
    generator_dir = Path(__file__).resolve().parents[2] / "manyforge_deployment/tools"
    sys.path.insert(0, str(generator_dir))
    # pylint: disable-next=import-outside-toplevel
    from generate_maintained_deployment_occurrences import (
        sealed_deployment_bytes,
    )

    return sealed_deployment_bytes(tool, deployment, temporary)


def sealed_program(program: dict[str, Any], *, tool: Path, temporary: Path) -> bytes:
    source = temporary / f"{program['program_id']}.program.yaml"
    source.write_text(
        yaml.safe_dump(program, sort_keys=False, width=100), encoding="utf-8"
    )
    run_tool(tool, "--update", source)
    run_tool(tool, "--check", source)
    return source.read_bytes()


def require_or_update(path: Path, expected: bytes, *, update: bool) -> None:
    if update:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(expected)
        return
    try:
        current = path.read_bytes()
    except OSError as error:
        raise GenerationError(f"generated source is unavailable: {path}: {error}") from error
    if current != expected:
        raise GenerationError(f"generated source is stale: {path}")


def require_obsolete_output_absent(path: Path, *, update: bool) -> None:
    """Remove only a named repository-owned output replaced by this generator."""

    if not path.exists():
        return
    if path.is_symlink() or not path.is_file():
        raise GenerationError(f"obsolete generated output is not a plain file: {path}")
    if not update:
        raise GenerationError(f"obsolete generated output remains: {path}")
    path.unlink()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--update", action="store_true")
    parser.add_argument("--workspace-root", type=Path, required=True)
    parser.add_argument("--deployment-revision-tool", type=Path, required=True)
    parser.add_argument("--program-revision-tool", type=Path, required=True)
    args = parser.parse_args()
    if args.check == args.update:
        parser.error("select exactly one of --check or --update")
    root = args.workspace_root.resolve()
    deployment_tool = args.deployment_revision_tool.resolve()
    program_tool = args.program_revision_tool.resolve()
    if not deployment_tool.is_file() or not program_tool.is_file():
        raise GenerationError("revision tools are unavailable")
    with tempfile.TemporaryDirectory(prefix="manyforge-ui6i-dual-") as directory:
        temporary = Path(directory)
        outputs = {
            root / DEPLOYMENT_SOURCE: sealed_deployment(
                build_deployment(root),
                tool=deployment_tool,
                temporary=temporary,
            ),
            root / GROUPED_PROGRAM_SOURCE: sealed_program(
                build_program(root, grouped=True),
                tool=program_tool,
                temporary=temporary,
            ),
            root / ISOLATION_PROGRAM_SOURCE: sealed_program(
                build_program(root, grouped=False),
                tool=program_tool,
                temporary=temporary,
            ),
        }
        generator_dir = root / "manyforge_deployment/tools"
        sys.path.insert(0, str(generator_dir))
        # pylint: disable-next=import-outside-toplevel
        from generate_maintained_deployment_occurrences import (
            _deployment_intent,
            _scenario_project_outputs,
            pretty_bytes,
        )

        description = read_json(
            root
            / "manyforge_deployment/resources/maintained/"
            "maintained_deployment_sources.v1.json"
        )
        families = {
            row["model_family_id"]: row for row in description["model_families"]
        }
        runtime_profiles = {
            row["runtime_restoration_profile_id"]: row
            for row in description["runtime_restoration_profiles"]
        }
        assignment = {
            "component_roles": {
                "lite6_robot": "lite6_robot",
                "ur5e_robot": "ur5e_robot",
            },
            "target_roles": {
                "lite6_arm": {
                    "component_role": "lite6_robot",
                    "command_target_id": "arm",
                },
                "ur5e_arm": {
                    "component_role": "ur5e_robot",
                    "command_target_id": "arm",
                },
            },
        }
        version = (root / "VERSION").read_text(encoding="utf-8").strip()
        for program_source, scenario_source in (
            (GROUPED_PROGRAM_SOURCE, GROUPED_SCENARIO_SOURCE),
            (ISOLATION_PROGRAM_SOURCE, ISOLATION_SCENARIO_SOURCE),
        ):
            program = yaml.safe_load(outputs[root / program_source])
            project_root = root / scenario_source
            component_inputs = (
                (
                    {
                        "deployment_artifact_id": "ur5e_lite6_wall",
                        "cell_id": CELL_ID,
                        "name": "ur5e_lite6_wall",
                        "description": (
                            "UI6-I dual-robot wall qualification occurrence"
                        ),
                        "component_occurrence_id": "lite6_robot",
                        "runtime_restoration_profile_id": (
                            "lite6_native_simulated_v0"
                        ),
                        "assistant_profile_id": None,
                        "root_pose_in_cell": {
                            "position_m": [0.0, 0.0, 0.0],
                            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                        },
                        "compatibility": {
                            "lane": "manyforge-only",
                            "provides": [],
                            "tags": ["dual-robot"],
                        },
                    },
                    "lite6_lite_gripper",
                    "lite6_native_simulated_v0",
                ),
                (
                    {
                        "deployment_artifact_id": "ur5e_lite6_wall",
                        "cell_id": CELL_ID,
                        "name": "ur5e_lite6_wall",
                        "description": (
                            "UI6-I dual-robot wall qualification occurrence"
                        ),
                        "component_occurrence_id": "ur5e_robot",
                        "runtime_restoration_profile_id": (
                            "ur5e_robotiq_native_simulated_v0"
                        ),
                        "assistant_profile_id": None,
                        "root_pose_in_cell": {
                            "position_m": [0.0, 1.0, 0.0],
                            "orientation_xyzw": [0.0, 0.0, 0.0, 1.0],
                        },
                        "compatibility": {
                            "lane": "manyforge-only",
                            "provides": [],
                            "tags": ["dual-robot"],
                        },
                    },
                    "ur5e_robotiq_2f_85",
                    "ur5e_robotiq_native_simulated_v0",
                ),
            )
            intents = [
                _deployment_intent(
                    root=root,
                    project_root=project_root,
                    deployment=component,
                    family=families[family_id],
                    runtime_profile=runtime_profiles[runtime_profile_id],
                )
                for component, family_id, runtime_profile_id in component_inputs
            ]
            intent = copy.deepcopy(intents[0])
            intent["components"] = sorted(
                [row for value in intents for row in value["components"]],
                key=lambda row: row["component_id"],
            )
            intent["compatibility"] = {
                "lane": "manyforge-only",
                "provides": [],
                "tags": ["dual-robot"],
            }
            for path, payload, _label in _scenario_project_outputs(
                project_root=project_root,
                scenario_id=program["program_id"],
                name=program["name"],
                description=program["description"],
                program=program,
                deployment_intent=intent,
                assignment=assignment,
                manyforge_version=version,
            ):
                outputs[path] = payload
    for path, expected in outputs.items():
        require_or_update(path, expected, update=args.update)
    for obsolete in (
        root / "examples/ur5e_lite6_wall_grouped.scenario.yaml",
        root / "examples/ur5e_lite6_wall_isolation.scenario.yaml",
    ):
        require_obsolete_output_absent(obsolete, update=args.update)
    print(
        "generate_ui6i_dual_robot_scenario: "
        + ("updated" if args.update else "current")
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GenerationError as error:
        print(f"generate_ui6i_dual_robot_scenario: {error}", file=sys.stderr)
        raise SystemExit(1) from error
