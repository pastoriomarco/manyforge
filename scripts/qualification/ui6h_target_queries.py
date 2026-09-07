"""Qualify target queries and optional control Programs against native owners.

Run only in the pinned image, with an already checked maintained package and
current native/Headless builds. Uses the maintained workbench's real startup
and shutdown, but a private package, private state and container-local ports.
The default Program is read-only; --controls adds simulated native motion,
Pause/scale/Resume and two short manual gripper moves before the Program.
No fixture substitutes for an owner response.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sqlite3
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
    OperationIntentRequest,
    ProgramOperationQuery,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError


def check_group_authoring(endpoint, stack, workbench):
    """One actual browser edit against the private Composer Draft owner; no motion."""
    from urllib.request import urlopen
    from current_workbench_browser_collector import RawWebDriver, _execute

    driver = RawWebDriver(endpoint, timeout_s=15)

    def js(script):
        return _execute(driver, script)

    def wait(label, expression):
        return workbench._wait_until(label, lambda: js("return " + expression), stack.processes)

    def element(selector):
        return driver.command("POST", "/element", {"using": "css selector", "value": selector})[
            "element-6066-11e4-a52e-4f735466cecf"
        ]

    def click(selector):
        driver.command("POST", f"/element/{element(selector)}/click", {})

    def workspace():
        with urlopen("http://127.0.0.1:9001/api/program/workspace", timeout=5) as response:
            return json.load(response)

    try:
        driver.create_session(firefox_args=("-headless", "-no-remote"))
        driver.set_window_rect(1600, 1000)
        driver.command("POST", "/url", {"url": "http://127.0.0.1:3001/"})
        wait("Program tree", "document.querySelector('[data-id=\"query_root\"]') !== null")
        original = workspace()
        click('[data-id="query_root"]')
        selector = "#policy-query_root-execution_groups"
        wait("editable group field", f"document.querySelector('{selector}')?.disabled === false")
        click(selector)
        driver.command("POST", f"/element/{element(selector)}/value", {"text": "n\ue007"})
        name_selector = "#policy-query_root-execution_group_names"
        wait("named group input", f"document.querySelector('{name_selector}') !== null")
        click(name_selector)
        driver.command("POST", f"/element/{element(name_selector)}/value", {"text": "robot_a, all_grippers"})
        click(selector)  # Native blur commits through the real Draft API.
        result = workbench._wait_until(
            "canonical Draft group edit",
            lambda: (current if (current := workspace())["draft"]["authoring"]["forest"][0].get("execution_groups")
                     == ["all_grippers", "robot_a"] else None),
            stack.processes,
        )
        click('[data-id="read_gripper"]')
        wait("inherited membership", "document.querySelector('.program-policy-editor')?.textContent.includes('all, all_grippers, robot_a')")
        wait("authoring recovers after edit", "document.querySelector('#policy-read_gripper-execution_groups')?.disabled === false")
        js("document.querySelector('#policy-read_gripper-execution_groups').scrollIntoView({block: 'center'}); return true;")
        assert result["applied"]["programRevision"] == original["applied"]["programRevision"]
        screenshot = workbench.STATE_ROOT.parent / "group-authoring.png"
        screenshot.write_bytes(driver.screenshot_png())
        return {"passed": True, "groups": ["all_grippers", "robot_a"],
                "draft_revision": result["draft"]["draftRevision"], "motion_issued": False,
                "screenshot": str(screenshot)}
    finally:
        driver.delete_session()


class SelectionBrowser:
    """Drive the existing scoped native check through actual GUI commands."""

    def __init__(self, endpoint, stack, workbench):
        from current_workbench_browser_collector import RawWebDriver, _execute

        self.driver = RawWebDriver(endpoint, timeout_s=15)
        self.js = lambda script: _execute(self.driver, script)
        self.wait = lambda label, read: workbench._wait_until(label, read, stack.processes)

    def open(self):
        self.driver.create_session(firefox_args=("-headless", "-no-remote"))
        self.driver.set_window_rect(1600, 1000)
        self.driver.command("POST", "/url", {"url": "http://127.0.0.1:3001/"})

    def click(self, selector, text=None):
        encoded = json.dumps(selector)
        self.wait("GUI action " + selector, lambda: self.js(
            f"const e = document.querySelector({encoded}); return !!e && !e.disabled"
            + (f" && e.textContent.trim() === {json.dumps(text)}" if text else "") + ";"
        ))
        element = self.driver.command("POST", "/element", {"using": "css selector", "value": selector})[
            "element-6066-11e4-a52e-4f735466cecf"
        ]
        self.driver.command("POST", f"/element/{element}/click", {})

    def selection_command(self, group, *, resume=False):
        self.click(f'.transport-selection option[value="{group}"]')
        self.wait("GUI group " + group, lambda: self.js(
            "return document.querySelector('.transport-selection select')?.value === "
            + json.dumps(group) + ";"
        ))
        self.click(".transport-run" if resume else ".transport-stop-primary",
                   "Resume Motion" if resume else "Stop")


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
    parser.add_argument("--selections", action="store_true",
                        help="Short native arm/gripper selected Stop followed by owner recovery Reset")
    parser.add_argument("--recover-parallel", action="store_true",
                        help="Extend --selections with Runner-owned recovery of both stopped attempts")
    parser.add_argument("--stop-progress", type=float, default=0.25,
                        help="Observed arm progress at selected Stop (default: 0.25)")
    parser.add_argument("--authoring-groups", metavar="WEBDRIVER_URL",
                        help="Check group editing in a real browser against private Composer, without motion")
    parser.add_argument("--runtime-groups", metavar="WEBDRIVER_URL",
                        help="Drive --selections --recover-parallel through the actual GUI/WebSocket")
    args = parser.parse_args()
    if args.recover_parallel and not args.selections:
        parser.error("--recover-parallel requires --selections")
    if not 0.0 <= args.stop_progress < 1.0:
        parser.error("--stop-progress must be in [0, 1)")
    if args.authoring_groups and (args.controls or args.selections):
        parser.error("--authoring-groups is a separate no-motion check")
    if args.runtime_groups and not args.recover_parallel:
        parser.error("--runtime-groups requires --selections --recover-parallel")
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
    if args.controls or args.selections:
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
                    (0.6, 0.0, 1.5707963267948966, 0.0, 0.0, 0.0)
                )
            ],
            velocity_scale=0.2,
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
        if args.selections:
            tool = deepcopy(next(row for row in authored["tree"]["children"]
                                 if row["node_id"] == "gripper.command_position"))
            tool["instance_id"] = "moving_gripper"
            tool["parameters"]["command_duration_s"] = 4.0
            tool["execution_groups"] = ["all_grippers"]
            motion["execution_groups"] = ["arm"]
            program["tree"]["execution_groups"] = ["supervision"]
            program["tree"]["children"] = [motion, tool]
    program["program_revision"] = compute_content_identity(
        program,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
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
    browser = None
    try:
        stack.start()
        if args.authoring_groups:
            print(json.dumps(check_group_authoring(args.authoring_groups, stack, workbench)))
            return
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
        manual_operations = []
        if args.controls:
            status = native.diagnostics()["status"]
            direct_tool = status["direct_control_gripper"]
            for attempt, endpoint in enumerate(("max_position", "min_position")):
                request = OperationIntentRequest.model_validate({
                    "request_context": {
                        "request_id": f"manual-before-program.{attempt}",
                        "request_owner_epoch": identity["owner_epoch"],
                        "source_label": "native_control_qualification",
                    },
                    **{key: direct_tool[key] for key in (
                        "program_id", "program_revision", "program_binding_id", "program_run_id"
                    )},
                    **{f"expected_{key}": status[key] for key in (
                        "graph_revision", "retained_bundle_id", "scene_revision"
                    )},
                    "participants": [{
                        "component_role_id": direct_tool["component_role_id"],
                        "target_role_id": direct_tool["target_role_id"],
                        "required_capability_id": direct_tool["operation_capability_id"],
                        "action_span": None,
                        "payload": {
                            "kind": "capability_command",
                            "command_schema_id": direct_tool["command_schema_id"],
                            "command_schema_revision": direct_tool["command_schema_revision"],
                            "command_value": {
                                "kind": "gripper_position",
                                "position": {"kind": "linear_m", "value_m": direct_tool["resource"][endpoint]},
                                "max_effort": None,
                            },
                            "completion_timeout_s": 2.0,
                            "caller_options": [{"option_id": "command_duration_s", "value_s": 0.5}],
                        },
                    }],
                    "coordination_policy": "independent",
                })
                accepted = native.submit(request, trusted_principal_id=None)
                assert accepted["ok"], accepted
                operation_id = accepted["accepted"]["operation_id"]
                held = native._exchange({
                    "api_version": "manyforge.execution_command.v1",
                    "command": "cell_admission_get",
                })["authoring_admission"]
                assert not held["authoring_allowed"], held

                def manual_retired():
                    current = native.read(operation_id)
                    return current if current.get("resident") is False else None

                retired = workbench._wait_until("manual operation retirement", manual_retired, stack.processes)
                assert retired["operation"]["state"] == "completed", retired
                manual_operations.append(operation_id)
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

            def ready_for_run():
                value = client.authoring_admission()
                return value if value["authoring_allowed"] else None

            admission = workbench._wait_until(
                "owner admission after manual operation retirement",
                ready_for_run, stack.processes,
            )
            start_request = {
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
            if args.runtime_groups:
                browser = SelectionBrowser(args.runtime_groups, stack, workbench)
                browser.open()
                browser.click(".transport-run", "Run")
                started = workbench._wait_until(
                    "GUI Run reached the owner", lambda: client.program_cycle_status().get("run"), stack.processes,
                )
            else:
                started = client.program_run_start(start_request)
            run_id = started["capture"]["program_run_id"]
            if args.selections:
                api = "manyforge.execution_command.v1"

                def scope():
                    return native._exchange({"api_version": api, "command": "read_program_execution_scope",
                                             "program_run_id": run_id})["scope"]

                def operation_for(target):
                    try:
                        return native.query_operation(ProgramOperationQuery.model_validate({
                            "schema_version": "manyforge.program_operation_query.v1",
                            "owner_epoch": identity["owner_epoch"],
                            "program_binding_id": stack.metadata["program_binding_id"],
                            "program_run_id": run_id, "target_role_id": target,
                        }))["operation"]
                    except HeadlessApiError as error:
                        assert error.code == "execution_operation_context_unavailable", error
                        return None

                def both_moving():
                    arm, tool = operation_for("arm"), operation_for("gripper")
                    if not (arm and tool and arm["state"] == tool["state"] == "executing"):
                        return None
                    if arm["participants"][0]["progress"] < args.stop_progress:
                        return None
                    return arm, tool

                arm, tool = workbench._wait_until("independent arm and gripper execution", both_moving, stack.processes)
                print(json.dumps({"selected_stop_progress": arm["participants"][0]["progress"],
                                  "requested_stop_progress": args.stop_progress,
                                  "private_state": str(state)}, sort_keys=True), flush=True)
                def request(group, request_id):
                    return {"program_run_id": run_id, "expected_owner_epoch": started["owner_epoch"],
                            "request_id": request_id, "request_owner_epoch": "selection.client",
                            "source_label": "native_selection_qualification", "group_id": group}

                def selected_command(group, request_id, controls=None):
                    if browser is None:
                        if controls is None:
                            return client.program_run_motion_stop(run_id, request(group, request_id))
                        return client.program_run_motion_resume(run_id, {
                            **request(group, request_id), "expected_run_revision": controls["run_revision"],
                            "expected_selection_revision": controls["execution_selections"]["selection_revision"],
                        })
                    previous = scope()["selection_revision"]
                    browser.selection_command(group, resume=controls is not None)
                    workbench._wait_until(
                        "GUI selection reached Kernel", lambda: scope()["selection_revision"] > previous, stack.processes,
                    )
                    return client.program_run(run_id)

                logical = released = None
                if args.recover_parallel:
                    selected_command("supervision", "selection.logical")
                    logical = scope()
                    assert not logical["pending_effects"], logical
                    controls = client.program_runtime_controls(run_id)
                    groups = {row["group_id"]: row for row in controls["execution_selections"]["groups"]}
                    assert groups["supervision"]["resume_allowed"] and groups["all"]["state"] == "mixed", groups
                    selected_command("supervision", "selection.logical.release", controls)
                    released = scope()
                    assert not any(released[key] for key in (
                        "held_node_indices", "stopping_node_indices", "faulted_node_indices")), released
                first = selected_command("arm", "selection.arm")
                arm_stop_id = operation_for("arm")["active_stop_id"]
                assert arm_stop_id, first
                assert operation_for("gripper")["active_stop_id"] is None, first
                tool_before = native.query_target(TargetObservationQuery.model_validate({
                    **common, "target_role_id": "gripper", "kind": "gripper_status"}))["target_observation"]

                def tool_advances():
                    value = native.query_target(TargetObservationQuery.model_validate({
                        **common, "target_role_id": "gripper", "kind": "gripper_status"}))["target_observation"]
                    return value if value["value"]["observed_position"] != tool_before["value"]["observed_position"] else None

                tool_after = workbench._wait_until("unselected gripper continues", tool_advances, stack.processes)
                # Default GUI/API Stop uses this same scoped owner path.
                all_request = request("all", "selection.all")
                del all_request["group_id"]
                both = (selected_command("all", "selection.all") if browser is not None
                        else client.program_run_motion_stop(run_id, all_request))
                assert operation_for("gripper")["active_stop_id"], both
                assert operation_for("arm")["active_stop_id"] == arm_stop_id, both

                def settled():
                    value = scope()
                    return value if value["held_node_indices"] == [0, 1, 2] and not any(
                        value[key] for key in ("pending_effects", "stopping_node_indices", "faulted_node_indices")) else None

                final_scope = workbench._wait_until("selected native effects settle", settled, stack.processes)
                # Reset can legitimately recover a containment fault. Prove
                # nominal Stop separately before that recovery can clear it.
                stop_fault = native.stop_fault()["stop_fault"]
                assert stop_fault is None, stop_fault
                recovery_result = None
                reset_result = None
                if args.recover_parallel:
                    def recoverable():
                        controls = client.program_runtime_controls(run_id)
                        return controls if any(
                            row["group_id"] == "all" and row["resume_allowed"]
                            for row in controls["execution_selections"]["groups"]
                        ) else None

                    controls = workbench._wait_until("selected Resume eligibility", recoverable, stack.processes)
                    if browser is not None:
                        (state / "group-controls.png").write_bytes(browser.driver.screenshot_png())
                    selected_command("arm", "parallel.resume.arm", controls)
                    controls = client.program_runtime_controls(run_id)
                    groups = {row["group_id"]: row for row in controls["execution_selections"]["groups"]}
                    assert groups["arm"]["state"] == "running" and groups["all_grippers"]["state"] == "held", groups
                    assert groups["all"]["state"] == "mixed" and groups["all"]["resume_allowed"], groups
                    selected_command("all", "parallel.resume.remaining", controls)

                    def recovered():
                        value = client.program_run(run_id)
                        assert value["state"] in {"running", "finalizing", "succeeded"}, value
                        return value if value["state"] == "succeeded" else None

                    recovery_result = workbench._wait_until("fresh arm and gripper attempts complete", recovered, stack.processes)
                    assert recovery_result["node_statuses"] == {
                        "control_root": "success", "controlled_motion": "success", "moving_gripper": "success",
                    }, recovery_result
                    assert native.read(arm["operation_id"])["resident"] is False
                    assert native.read(tool["operation_id"])["resident"] is False
                else:
                    def reset_offer():
                        offer = client.program_runtime_controls(run_id)["recovery_offer"]
                        return offer if offer and any(
                            row["action_id"] == "reset_cycle" and row["enabled"]
                            for row in offer["actions"]
                        ) else None

                    offer = workbench._wait_until("selected Reset eligibility", reset_offer, stack.processes)
                    reset_result = client.cycle_recovery_action({
                        "request_id": "selection.reset", "request_owner_epoch": "selection.client",
                        "source_label": "native_selection_qualification",
                        "expected_owner_epoch": started["owner_epoch"],
                        "offer_id": offer["offer_id"], "offer_revision": offer["offer_revision"],
                        "action_id": "reset_cycle",
                    })
                    assert reset_result["accepted"] and reset_result["run"]["state"] == "stopped", reset_result
                    cycle = client.program_cycle_status()
                    assert cycle["state"] == "idle_after_reset", cycle
                    assert cycle["run"] is None and cycle["runtime_controls"] is None, cycle
                    assert native.read(arm["operation_id"])["resident"] is False
                    assert native.read(tool["operation_id"])["resident"] is False
                print(json.dumps({"passed": True, "qualification": (
                    "runner_scoped_stop_resume" if args.recover_parallel else "runner_scoped_stop_reset"),
                    "gui_commands": browser is not None,
                    "logical_hold": logical, "logical_release": released, "arm_stop": first, "overlap_stop": both,
                    "unselected_gripper_before": tool_before, "unselected_gripper_after": tool_after,
                    "final_scope": final_scope, "recovered_program": recovery_result,
                    "reset_result": reset_result,
                    "private_state": str(state)}, sort_keys=True))
                return
            queried_operation: str | None = None
            observed_operation_scale = False
            resume_actor = None
            operation_query = ProgramOperationQuery.model_validate({
                "schema_version": "manyforge.program_operation_query.v1",
                "owner_epoch": identity["owner_epoch"],
                "program_binding_id": stack.metadata["program_binding_id"],
                "program_run_id": run_id,
                "target_role_id": "arm",
            })

            def terminal():
                nonlocal queried_operation, observed_operation_scale, resume_actor
                current = client.program_run(run_id)
                nodes = current.get("node_statuses", {})
                if args.controls and nodes.get("controlled_motion") == "running" and (
                    not queried_operation or (
                        nodes.get("scale") == "success" and not observed_operation_scale
                    )
                ):
                    try:
                        selected = native.query_operation(operation_query)
                    except HeadlessApiError as error:
                        # A tree status can precede native admission or trail
                        # retirement. No fixture manufactures a resident effect.
                        assert error.code == "execution_operation_context_unavailable", error
                    else:
                        operation = selected["operation"]
                        assert operation["program_run_id"] == run_id, selected
                        assert operation["program_binding_id"] == operation_query.program_binding_id
                        if not queried_operation:
                            foreign = operation_query.model_copy(update={"program_run_id": "foreign-run"})
                            try:
                                native.query_operation(foreign)
                            except HeadlessApiError as error:
                                assert error.code == "execution_operation_context_unavailable", error
                            else:
                                raise AssertionError("query borrowed another Program's operation")
                            # The Program arm and manual gripper use disjoint
                            # engines. Exact manual admission must still reject
                            # the latter while this Program owns the cell.
                            competing = request.model_copy(update={
                                "request_context": request.request_context.model_copy(update={
                                    "request_id": "manual-during-program"
                                })
                            })
                            rejected = native.submit(competing, trusted_principal_id=None)
                            assert rejected["ok"] is False, rejected
                            assert rejected["error"]["details"]["reason"] == "positive_admission_required", rejected
                            queried_operation = operation["operation_id"]
                        scale = selected.get("runtime_scale")
                        if nodes.get("scale") == "success" and scale and scale["state"] == "applied":
                            assert scale["effective_scale"] == 0.4, selected
                            observed_operation_scale = True
                if args.controls and queried_operation and resume_actor is None and nodes.get("resume") in {"running", "success"}:
                    # Check the actual current command record before normal
                    # completion cleanup compacts it to the latest control.
                    journal_path = workbench.STATE_ROOT / "execution-state/operation-journal.sqlite3"
                    with sqlite3.connect(f"file:{journal_path}?mode=ro", uri=True) as journal:
                        controls = [json.loads(row[0]) for row in journal.execute(
                            "SELECT payload_json FROM journal_records "
                            "WHERE record_kind = 'control_accepted' AND operation_id = ?",
                            (queried_operation,),
                        )]
                    resumes = [row for row in controls if row["result"]["control"] == "resume"]
                    if resumes:
                        assert len(resumes) == 1, controls
                        resume_actor = resumes[0]["intent"]["execution_scope"]
                        assert resume_actor["node_instance_id"] == "resume", resume_actor
                        assert resume_actor["node_attempt_id"].startswith("attempt:"), resume_actor
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
                assert queried_operation and observed_operation_scale and resume_actor
                assert native.read(queried_operation)["resident"] is False
                try:
                    native.query_operation(operation_query)
                except HeadlessApiError as error:
                    assert error.code == "execution_operation_context_unavailable", error
                else:
                    raise AssertionError("query treated retired history as current authority")
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
                    "manual_operations": manual_operations,
                    "resume_actor": resume_actor,
                    "private_state": str(state),
                },
                sort_keys=True,
            )
        )
    finally:
        if browser is not None:
            browser.driver.delete_session()
        stack.close()


if __name__ == "__main__":
    main()
