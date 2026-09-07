"""Qualify target queries and optional control Programs against native owners.

Run only in the pinned image, with an already checked maintained package and
current native/Headless builds. Uses the maintained workbench's real startup
and shutdown, but a private package, private state and container-local ports.
The default Program is read-only; --controls adds simulated native motion,
Pause/scale/Resume. No fixture substitutes for an owner response.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    compute_content_identity,
    load_strict_json_yaml,
)
from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--prepared-root", type=Path, required=True)
    parser.add_argument(
        "--controls",
        action="store_true",
        help="Also qualify Program-owned Pause/scale/Resume during native motion",
    )
    args = parser.parse_args()
    workspace = Path("/workspace")
    compiler = (
        args.build_root
        / "roboplan/manyforge_component_manager/manyforge_deployment/manyforge_maintained_deployment_occurrences"
    )
    prepared = tuple(
        (name, args.prepared_root / directory)
        for name, directory in (
            ("lite6_lite_gripper", "lite6_lite_gripper_structural"),
            ("ur10e_robotiq_2f_140", "ur10e_robotiq_2f_140_structural"),
            ("ur5_roboplan", "ur5_roboplan_structural"),
        )
    )
    catalog = json.loads(
        (args.package_root / "maintained.scenario-catalog.yaml").read_text()
    )
    scenario = next(
        row
        for row in catalog["scenarios"]
        if "lite6_pick_and_place" in [row["scenario_id"], *row.get("aliases", [])]
    )
    state = Path(tempfile.mkdtemp(prefix="h5q-"))
    package = state / "package"
    shutil.copytree(args.package_root, package)
    program = load_strict_json_yaml(
        workspace / "examples/lite6_lite_gripper_pick_and_place.program.yaml"
    )
    authored = deepcopy(program)
    # Keep the exact authored component/target assignments, but deliberately
    # remove execution, model/resource and collision requirements from reads.
    for role, capabilities in (
        ("arm", ["model.kinematics.fk", "state.observed.current"]),
        ("gripper", ["tool.gripper.feedback"]),
    ):
        program["target_roles"][role] = {
            "component_role": "robot" if role == "arm" else "tool",
            "required_capabilities": [
                {"capability_id": value, "parameters": {}, "disposition": "required"}
                for value in capabilities
            ],
            "required_frame_roles": ["base", "tcp"] if role == "arm" else [],
        }
    program.pop("scene", None)
    program.pop("parameters", None)
    program["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": "observed_pose",
                "type": "stored_pose",
                "access": "read_write",
                "persistence": "run",
            }
        ],
    }
    program["tree"] = {
        "node_id": "sequence",
        "instance_id": "query_root",
        "parameters": {},
        "children": [
            {
                "node_id": "model.get_frame_pose",
                "instance_id": "read_pose",
                "parameters": {
                    "target_role": "arm",
                    "frame_role": "tcp",
                    "output_key": "observed_pose",
                },
                "children": [],
            },
            {
                "node_id": "gripper.check_status",
                "instance_id": "read_gripper",
                "parameters": {"target_role": "gripper", "expected_state": "idle"},
                "children": [],
            },
        ],
    }
    if args.controls:
        program["target_roles"] = authored["target_roles"]
        program["target_roles"]["arm"]["required_capabilities"].extend(
            {"capability_id": name, "parameters": {}, "disposition": "required"}
            for name in (
                "execution.pause",
                "execution.resume",
                "model.kinematics.fk",
                "execution.runtime_scale.operation_scoped",
            )
        )
        program["target_roles"]["arm"]["required_capabilities"].sort(
            key=lambda row: row["capability_id"]
        )
        motion = next(
            row
            for row in authored["tree"]["children"]
            if row["node_id"] == "motion.move_to_joint_target"
        )
        motion.pop("value_sources", None)
        motion["instance_id"] = "controlled_motion"
        motion["parameters"].update(
            goal=[
                {"joint_name": f"joint{index + 1}", "position_rad": value}
                for index, value in enumerate(
                    (1.3, 0.0, 1.5707963267948966, 0.0, 0.0, 0.0)
                )
            ],
            velocity_scale=0.1,
            acceleration_scale=0.25,
        )
        controls = {
            "node_id": "sequence",
            "instance_id": "controls",
            "parameters": {},
            "children": [
                {
                    "node_id": "timer",
                    "instance_id": "motion_setup",
                    "children": [],
                    "parameters": {"duration_s": 0.5},
                },
                {
                    "node_id": "kernel.pause_operation",
                    "instance_id": "pause",
                    "children": [],
                    "parameters": {"target_role": "arm"},
                },
                {
                    "node_id": "timer",
                    "instance_id": "pause_hold",
                    "children": [],
                    "parameters": {"duration_s": 0.25},
                },
                {
                    "node_id": "kernel.set_runtime_scale",
                    "instance_id": "scale",
                    "children": [],
                    "parameters": {"target_role": "arm", "scale": 0.4},
                },
                {
                    "node_id": "kernel.resume_operation",
                    "instance_id": "resume",
                    "children": [],
                    "parameters": {"target_role": "arm"},
                },
            ],
        }
        program["tree"] = {
            "node_id": "parallel",
            "instance_id": "control_root",
            "parameters": {"success_policy": "all"},
            "children": [motion, controls],
        }
    program["program_revision"] = compute_content_identity(
        program,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v1",
    )
    program_path = state / "query.program.json"
    program_path.write_bytes(canonical_json_bytes(program))
    subprocess.run(
        [
            str(compiler),
            "--compile-candidate",
            "--workspace-root",
            str(workspace),
            "--base-package-root",
            str(args.package_root),
            "--candidate-package-root",
            str(package),
            "--candidate-deployment",
            str(
                workspace / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
            ),
            "--candidate-program",
            str(program_path),
            "--scenario-id",
            scenario["scenario_id"],
            "--expected-scenario-catalog-revision",
            catalog["scenario_catalog_revision"],
            *(
                arg
                for name, path in prepared
                for arg in ("--prepared", f"{name}={path}")
            ),
        ],
        check=True,
    )

    spec = importlib.util.spec_from_file_location(
        "query_workbench", workspace / "scripts/dev-workbench.py"
    )
    assert spec is not None and spec.loader is not None
    workbench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(workbench)
    workbench.STATE_ROOT = state / "owners"
    workbench.PACKAGE_ROOT = package
    workbench.HEADLESS_BUILD = args.build_root / "headless"
    workbench.ROBOPLAN_BUILD = args.build_root / "roboplan"
    workbench.MANAGER_BINARY = workbench.HEADLESS_BUILD / "manyforge_manager_daemon"
    workbench.NATIVE_EXECUTION_HOST = (
        workbench.ROBOPLAN_BUILD / "manyforge_native_execution_host"
    )
    workbench.NATIVE_CANDIDATE_WORKER = (
        workbench.ROBOPLAN_BUILD / "manyforge_roboplan_candidate_worker"
    )
    # These only locate already qualified build/package inputs. Normal owner
    # construction, exact tuple checks, lifecycle convergence and teardown run.
    workbench._import_package = lambda: None
    workbench._ensure_deployment_compiler = lambda: (compiler, prepared)
    load_scenario = workbench._load_scenario
    workbench._load_scenario = lambda name: {
        **load_scenario(name),
        "program_path": str(program_path),
    }
    os.environ["MANYFORGE_DEV_BUILD_JOBS"] = "2"
    stack = workbench.Workbench(
        scenario_id=scenario["scenario_id"],
        headless_port=9101,
        runner_port=9102,
        composer_port=9001,
        frontend_port=3001,
    )
    try:
        stack.start()
        native = ExecutionCommandClient(workbench.STATE_ROOT / "execution-run")
        identity = native.identity()["identity"]
        before = native.events(identity["owner_epoch"], 0)
        common = {
            "schema_version": "manyforge.target_observation_query.v1",
            "owner_epoch": identity["owner_epoch"],
            "program_binding_id": stack.metadata["program_binding_id"],
        }
        frames = {}
        for role in ("base", "tcp"):
            frames[role] = native.query_target(
                TargetObservationQuery.model_validate(
                    {
                        **common,
                        "target_role_id": "arm",
                        "kind": "frame_pose",
                        "frame_role_id": role,
                    }
                )
            )["target_observation"]
        gripper = native.query_target(
            TargetObservationQuery.model_validate(
                {
                    **common,
                    "target_role_id": "gripper",
                    "kind": "gripper_status",
                }
            )
        )["target_observation"]
        assert gripper["value"]["observed_position"]["kind"] == "linear_m"
        assert gripper["value"]["grasp_confirmed"] is None
        for change, expected_code in (
            ({"owner_epoch": "replaced.owner"}, "execution_target_owner_stale"),
            ({"target_role_id": "missing"}, "execution_target_binding_unavailable"),
            (
                {"program_binding_id": "sha256:" + "0" * 64},
                "execution_target_binding_unavailable",
            ),
            ({"frame_role_id": "undeclared"}, "execution_target_frame_unavailable"),
        ):
            try:
                native.query_target(
                    TargetObservationQuery.model_validate(
                        {
                            **common,
                            "target_role_id": "arm",
                            "kind": "frame_pose",
                            "frame_role_id": "tcp",
                            **change,
                        }
                    )
                )
            except HeadlessApiError as error:
                assert error.code == expected_code, (change, error.code)
            else:
                raise AssertionError(
                    f"query accepted stale/undeclared selector: {change}"
                )
        assert native.events(identity["owner_epoch"], 0) == before
        if args.controls:
            # Address the unselected-Program/direct-control scope explicitly:
            # its representatives include both trajectory and scale bindings.
            # A selected Program's trajectory-only alias can hide ambiguity.
            direct = native.targets()["targets"]["direct_control"]
            scale_selector = RuntimeScaleControlSelectorRequest.model_validate(
                {
                    key: direct[key]
                    for key in (
                        "program_binding_id",
                        "component_role_id",
                        "target_role_id",
                    )
                }
            )
            scale_before = native.get_runtime_scale_control(scale_selector)[
                "runtime_scale_control"
            ]
            assert (
                scale_before["supported"] and not scale_before["frozen"]
            ), scale_before
            configured = native.set_runtime_scale_control(
                RuntimeScaleControlRequest.model_validate(
                    {
                        "request_context": {
                            "request_id": "control-scope.initial",
                            "request_owner_epoch": identity["owner_epoch"],
                            "source_label": "native_control_qualification",
                        },
                        "selector": scale_selector.model_dump(),
                        "expected_control_generation": scale_before[
                            "control_generation"
                        ],
                        "scale": 0.8,
                    }
                ),
                trusted_principal_id=None,
            )["accepted"]
            assert configured["requested_scale"] == 0.8, configured
        with HeadlessHttpClient("http://127.0.0.1:9101", timeout_seconds=3) as client:
            applied = client.program_applied()
            admission = client.authoring_admission()
            started = client.program_run_start(
                {
                    "request_id": "query-only.run.1",
                    "request_owner_epoch": "query-only.client",
                    "source_label": "query_only_qualification",
                    "scenario_id": scenario["scenario_id"],
                    "scenario_catalog_revision": stack.metadata[
                        "scenario_catalog_revision"
                    ],
                    "expected_applied_record_digest": applied["source"][
                        "record_digest"
                    ],
                    "draft_program_revision": program["program_revision"],
                    "expected_admission_generation": admission["admission_generation"],
                }
            )
            run_id = started["capture"]["program_run_id"]

            def terminal():
                current = client.program_run(run_id)
                return current if current["state"] != "running" else None

            result = workbench._wait_until(
                "query-only Program terminal", terminal, stack.processes
            )
            assert result["state"] == "succeeded", result
            expected_statuses = (
                {
                    "query_root": "success",
                    "read_pose": "success",
                    "read_gripper": "success",
                }
                if not args.controls
                else {
                    key: "success"
                    for key in (
                        "control_root",
                        "controlled_motion",
                        "controls",
                        "motion_setup",
                        "pause",
                        "pause_hold",
                        "scale",
                        "resume",
                    )
                }
            )
            assert result["node_statuses"] == expected_statuses, result
        if args.controls:
            persisted = native.get_runtime_scale_control(scale_selector)[
                "runtime_scale_control"
            ]
            assert persisted["supported"] and not persisted["frozen"], persisted
            assert persisted["requested_scale"] == 0.8, persisted
            assert (
                persisted["control_generation"] == configured["control_generation"]
            ), persisted
            assert persisted["active_operation_id"] is None, persisted
        print(
            json.dumps(
                {
                    "passed": True,
                    "frames": frames,
                    "gripper": gripper,
                    "program_run": result,
                    "private_state": str(state),
                },
                sort_keys=True,
            )
        )
    finally:
        stack.close()


if __name__ == "__main__":
    main()
