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
import hashlib
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
from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    OperationIntentRequest,
    StopIntentRequest,
    ProgramOperationQuery,
    RuntimeScaleControlRequest,
    RuntimeScaleControlSelectorRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError


def check_skill_authoring():
    """Actual Composer → Headless selection/expansion; no execution or Draft edit."""
    with HeadlessHttpClient("http://127.0.0.1:9001", timeout_seconds=5) as client:
        original = client.request("GET", "/api/program/workspace")
        selected = client.request("GET", "/api/skills")
        assert {row["skill_id"] for row in selected["skills"]} == {
            "move_to_named_config",
            "pick_object",
            "place_object",
        }, selected
        results = []
        for artifact in selected["skills"]:
            context = {
                "api_version": "manyforge.program.v2",
                "program_id": "skill_example",
                "program_profile": "composable-cell-v1",
                "name": "Skill example",
                "catalog": {
                    "api_version": "manyforge.node_catalog.v2",
                    "content_version": "2.1.0",
                },
                "component_roles": artifact["component_roles"],
                "target_roles": artifact["target_roles"],
                "parameters": [],
                "scene": [],
                **artifact["example"]["program_declarations"],
            }
            result = client.request(
                "POST",
                "/api/skills/expand",
                body={
                    "schema_version": "manyforge.skill_expansion_request.v1",
                    "expected_environment_digest": selected["environment_digest"],
                    "skill_id": artifact["skill_id"],
                    "arguments": artifact["example"]["arguments"],
                    "component_roles": {
                        key: key for key in artifact["component_roles"]
                    },
                    "target_roles": {key: key for key in artifact["target_roles"]},
                    "instance_prefix": "qualified",
                    "program_context": context,
                },
            )
            assert (
                result["environment_digest"] == selected["environment_digest"]
            ), result
            assert (
                result["tree"]["metadata"]["skill_expansions"] == result["trace"]
            ), result
            results.append(
                {
                    "skill_id": artifact["skill_id"],
                    "instances": result["trace"][0]["instance_ids"],
                }
            )
        current = client.request("GET", "/api/program/workspace")
        assert (
            current["draft"] == original["draft"]
            and current["applied"] == original["applied"]
        )
        return results


def check_group_authoring(endpoint, stack, workbench):
    """One actual browser edit against the private Composer Draft owner; no motion."""
    from urllib.request import urlopen
    from current_workbench_browser_collector import RawWebDriver, _execute

    driver = RawWebDriver(endpoint, timeout_s=15)

    def js(script):
        return _execute(driver, script)

    def wait(label, expression):
        return workbench._wait_until(
            label, lambda: js("return " + expression), stack.processes
        )

    def element(selector):
        return driver.command(
            "POST", "/element", {"using": "css selector", "value": selector}
        )["element-6066-11e4-a52e-4f735466cecf"]

    def click(selector):
        driver.command("POST", f"/element/{element(selector)}/click", {})

    def workspace():
        with urlopen(
            "http://127.0.0.1:9001/api/program/workspace", timeout=5
        ) as response:
            return json.load(response)

    try:
        driver.create_session(firefox_args=("-headless", "-no-remote"))
        driver.set_window_rect(1600, 1000)
        driver.command("POST", "/url", {"url": "http://127.0.0.1:3001/"})
        wait(
            "Program tree",
            "document.querySelector('[data-id=\"query_root\"]') !== null",
        )
        original = workspace()
        click('[data-id="query_root"]')
        selector = "#policy-query_root-execution_groups"
        wait(
            "editable group field",
            f"document.querySelector('{selector}')?.disabled === false",
        )
        click(selector)
        driver.command(
            "POST", f"/element/{element(selector)}/value", {"text": "n\ue007"}
        )
        name_selector = "#policy-query_root-execution_group_names"
        wait("named group input", f"document.querySelector('{name_selector}') !== null")
        click(name_selector)
        driver.command(
            "POST",
            f"/element/{element(name_selector)}/value",
            {"text": "robot_a, all_grippers"},
        )
        click(selector)  # Native blur commits through the real Draft API.
        result = workbench._wait_until(
            "canonical Draft group edit",
            lambda: (
                current
                if (current := workspace())["draft"]["authoring"]["forest"][0].get(
                    "execution_groups"
                )
                == ["all_grippers", "robot_a"]
                else None
            ),
            stack.processes,
        )
        click('[data-id="read_gripper"]')
        wait(
            "inherited membership",
            "document.querySelector('.program-policy-editor')?.textContent.includes('all, all_grippers, robot_a')",
        )
        wait(
            "authoring recovers after edit",
            "document.querySelector('#policy-read_gripper-execution_groups')?.disabled === false",
        )
        js(
            "document.querySelector('#policy-read_gripper-execution_groups').scrollIntoView({block: 'center'}); return true;"
        )
        assert (
            result["applied"]["programRevision"]
            == original["applied"]["programRevision"]
        )
        screenshot = workbench.STATE_ROOT.parent / "group-authoring.png"
        screenshot.write_bytes(driver.screenshot_png())
        return {
            "passed": True,
            "groups": ["all_grippers", "robot_a"],
            "draft_revision": result["draft"]["draftRevision"],
            "motion_issued": False,
            "screenshot": str(screenshot),
        }
    finally:
        driver.delete_session()


class SelectionBrowser:
    """Drive the existing scoped native check through actual GUI commands."""

    def __init__(self, endpoint, stack, workbench):
        from current_workbench_browser_collector import RawWebDriver, _execute

        self.driver = RawWebDriver(endpoint, timeout_s=15)
        self.js = lambda script: _execute(self.driver, script)
        self.wait = lambda label, read: workbench._wait_until(
            label, read, stack.processes
        )

    def open(self):
        self.driver.create_session(firefox_args=("-headless", "-no-remote"))
        self.driver.set_window_rect(1600, 1000)
        self.driver.command("POST", "/url", {"url": "http://127.0.0.1:3001/"})

    def click(self, selector, text=None):
        encoded = json.dumps(selector)
        self.wait(
            "GUI action " + selector,
            lambda: self.js(
                f"const e = document.querySelector({encoded}); return !!e && !e.disabled"
                + (f" && e.textContent.trim() === {json.dumps(text)}" if text else "")
                + ";"
            ),
        )
        element = self.driver.command(
            "POST", "/element", {"using": "css selector", "value": selector}
        )["element-6066-11e4-a52e-4f735466cecf"]
        self.driver.command("POST", f"/element/{element}/click", {})

    def selection_command(self, group, *, resume=False):
        groups = [group] if isinstance(group, str) else list(group)
        self.click(".transport-selection summary")
        selected = self.js(
            "return [...document.querySelectorAll('.transport-selection-options input:checked')]"
            ".map(e => e.value);"
        )
        for value in selected:
            if value not in groups:
                self.click(
                    f".transport-selection-options input[value={json.dumps(value)}]"
                )
        for value in groups:
            selector = f".transport-selection-options input[value={json.dumps(value)}]"
            if not self.js(
                f"return document.querySelector({json.dumps(selector)})?.checked;"
            ):
                self.click(selector)
        self.wait(
            "GUI selected groups",
            lambda: self.js(
                "return JSON.stringify([...document.querySelectorAll('.transport-selection-options input:checked')]"
                ".map(e => e.value).sort()) === "
                + json.dumps(json.dumps(sorted(groups), separators=(",", ":")))
                + ";"
            ),
        )
        self.click(".transport-selection summary")
        self.click(
            ".transport-run" if resume else ".transport-stop-primary",
            "Resume Motion" if resume else "Stop",
        )


def check_jerk_authoring(endpoint, stack, workbench):
    """Real selector/Save/Apply/reload, on a private Deployment, without motion."""
    browser = SelectionBrowser(endpoint, stack, workbench)
    selector = 'select[aria-label="lite6_robot / arm ordinary motion jerk"]'
    client = HeadlessHttpClient("http://127.0.0.1:9001", timeout_seconds=5)

    def workspace():
        return client.request("GET", "/api/deployment/workspace")

    def settings(document):
        for runtime in document["graph"]["runtime_units"]:
            for target in runtime["configuration"].get("motion_target_limits", []):
                if target["command_target_id"] == "arm":
                    return target["ordinary_motion_jerk"]
        raise AssertionError("private arm profile missing")

    def click_xpath(xpath):
        # Actual pointer click, not a direct API mutation or injected DOM event.
        browser.wait(
            xpath,
            lambda: browser.js(
                "const e = document.evaluate("
                + json.dumps(xpath)
                + ",document,null,XPathResult.FIRST_ORDERED_NODE_TYPE,null).singleNodeValue; return !!e && !e.disabled;"
            ),
        )
        element = browser.driver.command(
            "POST", "/element", {"using": "xpath", "value": xpath}
        )["element-6066-11e4-a52e-4f735466cecf"]
        browser.driver.command("POST", f"/element/{element}/click", {})

    def action(label):
        click_xpath(
            "//div[@class='deployment-document-actions']/button[normalize-space(.)="
            + json.dumps(label)
            + "]"
        )

    try:
        browser.open()
        browser.click('button[data-workspace="deployment"]')
        action("Open")
        click_xpath(
            "//button[@role='listitem'][.//strong[text()='query.deployment.yaml']]"
        )
        browser.click(
            ".program-document-dialog > button.primary-action", "Open as Draft"
        )
        browser.wait(
            "loaded Deployment", lambda: workspace()["draft"]["document"] is not None
        )
        original = workspace()
        saved_path = Path(original["saved"]["path"]).resolve()
        assert saved_path.is_relative_to(workbench.STATE_ROOT.parent), saved_path
        original_settings = settings(original["draft"]["document"])
        assert original_settings["enabled"] is True
        results = []
        for enabled in (False, True):
            browser.click(selector)
            element = browser.driver.command(
                "POST", "/element", {"using": "css selector", "value": selector}
            )["element-6066-11e4-a52e-4f735466cecf"]
            browser.driver.command(
                "POST",
                f"/element/{element}/value",
                {"text": ("e" if enabled else "o") + "\ue007"},
            )
            changed = browser.wait(
                "jerk Draft edit",
                lambda: (
                    current
                    if settings((current := workspace())["draft"]["document"])[
                        "enabled"
                    ]
                    is enabled
                    else None
                ),
            )
            assert (
                changed["managerApplied"] == original["managerApplied"]
                if not results
                else True
            )
            action("Save")
            browser.wait(
                "saved jerk choice",
                lambda: not workspace()["draft"]["differsFromSaved"],
            )
            saved = settings(load_strict_json_yaml(saved_path))
            assert saved == {**original_settings, "enabled": enabled}, saved
            action("Preview Draft")
            action("Apply Draft")

            def applied_result():
                current = workspace()
                error = browser.js(
                    "return document.querySelector('.deployment-workspace .panel-state.error-state')?.textContent ?? ''; "
                )
                if error:
                    raise AssertionError(error)
                completed = browser.js(
                    "return document.querySelector('.deployment-workspace .program-feedback')"
                    "?.textContent.includes('rebound to the current Program.') ?? false;"
                )
                if current["runAdmission"]["ready"] and completed:
                    return current
                return None

            applied = browser.wait("applied jerk choice", applied_result)
            assert (
                applied["managerApplied"]["deploymentRevision"]
                == changed["draft"]["deploymentRevision"]
            )
            native = ExecutionCommandClient(workbench.STATE_ROOT / "execution-run")
            descriptor = native.diagnostics()["status"]["direct_control"][
                "target_descriptor"
            ]
            assert descriptor["ordinary_jerk"]["enabled"] is enabled
            browser.driver.command("POST", "/refresh", {})
            browser.click('button[data-workspace="deployment"]')
            browser.wait(
                "reloaded jerk selection",
                lambda: browser.js(
                    "return document.querySelector("
                    + json.dumps(selector)
                    + ")?.value === "
                    + json.dumps("on" if enabled else "off")
                    + ";"
                ),
            )
            results.append(
                {
                    "enabled": enabled,
                    "deployment_revision": applied["managerApplied"][
                        "deploymentRevision"
                    ],
                }
            )
        screenshot = workbench.STATE_ROOT.parent / "jerk-authoring.png"
        screenshot.write_bytes(browser.driver.screenshot_png())
        return {
            "passed": True,
            "jerk_authoring": results,
            "motion_issued": False,
            "screenshot": str(screenshot),
        }
    except Exception:
        screenshot = workbench.STATE_ROOT.parent / "jerk-authoring-failed.png"
        screenshot.write_bytes(browser.driver.screenshot_png())
        print(
            json.dumps(
                {
                    "jerk_authoring_failure": browser.js(
                        "return document.body.innerText.slice(0, 18000);"
                    ),
                    "screenshot": str(screenshot),
                }
            ),
            flush=True,
        )
        raise
    finally:
        browser.driver.delete_session()
        client.close()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--prepared-root", type=Path, required=True)
    parser.add_argument(
        "--scene-transactions",
        action="store_true",
        help="Short real Scene/Stop/Resume/Reset workflow, without arm motion",
    )
    parser.add_argument(
        "--uncertain-resume-reset",
        action="store_true",
        help="With --scene-transactions, discard one actual release reply and recover by Reset",
    )
    parser.add_argument(
        "--lifecycle-transactions",
        action="store_true",
        help="Short real CCLC Enable/Stop/Resume/Reset workflow, without arm motion",
    )
    parser.add_argument(
        "--route-transactions",
        action="store_true",
        help="Short real CCLC route/Stop/Resume/Reset workflow, without arm motion",
    )
    parser.add_argument(
        "--controls",
        action="store_true",
        help="Also qualify Program-owned Pause/scale/Resume during native motion",
    )
    parser.add_argument(
        "--selections",
        action="store_true",
        help="Short native arm/gripper selected Stop followed by owner recovery Reset",
    )
    parser.add_argument(
        "--recover-parallel",
        action="store_true",
        help="Extend --selections with Runner-owned recovery of both stopped attempts",
    )
    parser.add_argument(
        "--stop-progress",
        type=float,
        default=0.25,
        help="Observed arm progress at selected Stop (default: 0.25)",
    )
    parser.add_argument(
        "--manual-handover",
        action="store_true",
        help="Extend --recover-parallel with real held-arm cleanup, manual motion/Stop and original-target Resume",
    )
    parser.add_argument(
        "--ordinary-jerk",
        action="store_true",
        help="Enable the saved native ordinary-motion jerk profile in the private candidate Deployment",
    )
    parser.add_argument(
        "--ordinary-jerk-import",
        action="store_true",
        help="Exercise the MoveIt importer, overrides and disabled source metadata with --ordinary-jerk",
    )
    parser.add_argument(
        "--jerk-authoring",
        metavar="WEBDRIVER_URL",
        help="Real ordinary jerk selector/Save/Apply/reload, without motion",
    )
    parser.add_argument(
        "--gripper-duration",
        type=float,
        default=4.0,
        help="Simulated gripper duration for concurrent selected-Stop checks; extend for late triggers",
    )
    parser.add_argument(
        "--cartesian-handover",
        action="store_true",
        help="Use a relative multi-waypoint Cartesian Program in --manual-handover",
    )
    parser.add_argument(
        "--authoring-groups",
        metavar="WEBDRIVER_URL",
        help="Check group editing in a real browser against private Composer, without motion",
    )
    parser.add_argument(
        "--runtime-groups",
        metavar="WEBDRIVER_URL",
        help="Drive --selections --recover-parallel through the actual GUI/WebSocket",
    )
    args = parser.parse_args()
    if args.ordinary_jerk_import and not args.ordinary_jerk:
        parser.error("--ordinary-jerk-import requires --ordinary-jerk")
    if args.jerk_authoring and (
        not args.ordinary_jerk
        or args.controls
        or args.selections
        or args.scene_transactions
        or args.lifecycle_transactions
        or args.route_transactions
        or args.authoring_groups
    ):
        parser.error(
            "--jerk-authoring requires --ordinary-jerk and runs separately without motion"
        )
    if args.uncertain_resume_reset and not args.scene_transactions:
        parser.error(
            "--uncertain-resume-reset requires --scene-transactions (no arm motion)"
        )
    transactions = sum(
        (args.scene_transactions, args.lifecycle_transactions, args.route_transactions)
    )
    if (
        transactions
        and (args.controls or args.selections or args.authoring_groups)
        or transactions > 1
    ):
        parser.error(
            "transaction checks run separately from each other and motion/browser checks"
        )
    if args.recover_parallel and not args.selections:
        parser.error("--recover-parallel requires --selections")
    if args.manual_handover and (not args.recover_parallel or args.runtime_groups):
        parser.error(
            "--manual-handover requires --selections --recover-parallel without browser driving"
        )
    if args.cartesian_handover and not args.manual_handover:
        parser.error("--cartesian-handover requires --manual-handover")
    if not 0.0 <= args.stop_progress < 1.0:
        parser.error("--stop-progress must be in [0, 1)")
    if not 0.0 < args.gripper_duration < 30.0:
        parser.error(
            "--gripper-duration must be positive and below the 30-second completion timeout"
        )
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
            {
                "node_id": "example.check_frame_height",
                "instance_id": "check_height",
                "parameters": {
                    "target_role": "arm",
                    "frame_role": "tcp",
                    "minimum_height_m": 0.0,
                    "maximum_height_m": 2.0,
                },
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
        if args.cartesian_handover:
            program["target_roles"]["arm"]["required_capabilities"].append(
                {
                    "capability_id": "planning.motion.cartesian_path",
                    "parameters": {},
                    "disposition": "required",
                }
            )
            program["target_roles"]["arm"]["required_capabilities"].sort(
                key=lambda row: row["capability_id"]
            )
            motion["node_id"] = "motion.follow_cartesian_path"
            motion["parameters"] = {
                "target_role": "arm",
                "base_frame_role": "base",
                "tcp_frame_role": "tcp",
                "target_mode": "relative",
                "target_reference": {"type": "frame_role", "frame_role_id": "tcp"},
                "path": [
                    {"position_m": xyz, "orientation": {"rpy_deg": [0, 0, 0]}}
                    for xyz in ([0, 0, 0], [0, 0, -0.04], [0, 0, -0.08])
                ],
                "velocity_scale": 0.3,
                "acceleration_scale": 0.25,
                "max_linear_speed_mps": 0.05,
                "execution_timeout_s": 30,
            }
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
            tool = deepcopy(
                next(
                    row
                    for row in authored["tree"]["children"]
                    if row["node_id"] == "gripper.command_position"
                )
            )
            tool["instance_id"] = "moving_gripper"
            tool["parameters"]["command_duration_s"] = args.gripper_duration
            tool["execution_groups"] = ["all_grippers"]
            motion["execution_groups"] = ["arm"]
            program["tree"]["execution_groups"] = ["supervision"]
            program["tree"]["children"] = [motion, tool]
    if args.scene_transactions:
        program["target_roles"] = authored["target_roles"]
        program["target_roles"]["arm"]["required_capabilities"].append(
            {
                "capability_id": "model.kinematics.fk",
                "parameters": {},
                "disposition": "required",
            }
        )
        program["target_roles"]["arm"]["required_capabilities"].sort(
            key=lambda row: row["capability_id"]
        )
        program["scene"] = authored["scene"]
        attach = deepcopy(
            next(
                row
                for row in authored["tree"]["children"]
                if row["node_id"] == "scene.attach_carried_object"
            )
        )
        detach = deepcopy(
            next(
                row
                for row in authored["tree"]["children"]
                if row["node_id"] == "scene.detach_carried_object"
            )
        )
        attach["instance_id"], detach["instance_id"] = "attach", "detach"
        program["tree"] = {
            "node_id": "sequence",
            "instance_id": "scene_root",
            "parameters": {},
            "children": [
                attach,
                {
                    "node_id": "timer",
                    "instance_id": "between",
                    "parameters": {"duration_s": 1.0},
                    "children": [],
                },
                detach,
            ],
        }
    if args.lifecycle_transactions or args.route_transactions:
        node_id = (
            "controller.select_route"
            if args.route_transactions
            else "lifecycle.enable_component"
        )
        parameters = (
            {"target_role": "arm", "route_role": "arm_position"}
            if args.route_transactions
            else {"component_role": "robot"}
        )
        if args.route_transactions:
            # Route selection uses the retained arm control binding. Its
            # controller-domain capabilities are not target capabilities.
            query_capabilities = program["target_roles"]["arm"]["required_capabilities"]
            program["target_roles"]["arm"] = authored["target_roles"]["arm"]
            capabilities = {
                row["capability_id"]: row
                for row in (
                    *program["target_roles"]["arm"]["required_capabilities"],
                    *query_capabilities,
                )
            }
            program["target_roles"]["arm"]["required_capabilities"] = [
                capabilities[key] for key in sorted(capabilities)
            ]
        program["tree"] = {
            "node_id": "sequence",
            "instance_id": "lifecycle_root",
            "parameters": {},
            "children": [
                {
                    "node_id": node_id,
                    "instance_id": "first",
                    "parameters": dict(parameters),
                    "children": [],
                },
                {
                    "node_id": "timer",
                    "instance_id": "between",
                    "parameters": {"duration_s": 1.0},
                    "children": [],
                },
                {
                    "node_id": node_id,
                    "instance_id": "second",
                    "parameters": dict(parameters),
                    "children": [],
                },
            ],
        }
    program["program_revision"] = compute_content_identity(
        program,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    program_path = state / "query.program.json"
    program_path.write_bytes(canonical_json_bytes(program))
    deployment_path = (
        workspace / "examples/lite6_lite_gripper_pick_and_place.deployment.yaml"
    )
    if args.ordinary_jerk:
        from manyforge_composer.backend.deployment_draft import (
            compile_deployment_document,
        )

        candidate = load_strict_json_yaml(deployment_path)
        edited = 0
        for unit in candidate["graph"]["runtime_units"]:
            for target in unit.get("configuration", {}).get("motion_target_limits", []):
                target["ordinary_motion_jerk"]["enabled"] = True
                edited += 1
        assert edited > 0, "no native ordinary jerk settings in candidate"
        deployment_path = state / "query.deployment.yaml"
        compiled = compile_deployment_document(json.dumps(candidate)).canonical_json
        if args.ordinary_jerk_import:
            from manyforge_headless_py import import_moveit_angular_jerk_limits

            assert edited == 1
            unit = candidate["graph"]["runtime_units"][0]
            target = unit["configuration"]["motion_target_limits"][0]
            target["ordinary_motion_jerk"]["joint_limits"] = [
                {"source_joint_name": "joint1", "override_rad_s3": 40.0}
            ]
            moveit_source = """joint_limits:
  joint1: {has_jerk_limits: true, max_jerk: 50}
  joint2: {has_jerk_limits: true, max_jerk: 55}
  joint3: {has_jerk_limits: false, max_jerk: 5}
"""
            (state / "joint_limits.yaml").write_text(moveit_source)
            compiled = import_moveit_angular_jerk_limits(
                json.dumps(candidate),
                moveit_source,
                "qualification_moveit_config/config/joint_limits.yaml",
                unit["runtime_unit_id"],
                target["controllable_component_id"],
                target["command_target_id"],
                [f"joint{index}" for index in range(1, 7)],
            )["canonical_json"]
        deployment_path.write_text(compiled)
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
            str(deployment_path),
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
    if args.ordinary_jerk:
        # Verify the private, compiler-produced Deployment with the same
        # revision check as the ordinary workspace source. Do not bypass it.
        require_deployment = workbench._require_current_workspace_deployment
        workbench._require_current_workspace_deployment = lambda row: (
            require_deployment(row, deployment_path)
        )
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
        artifact_root=state if args.jerk_authoring else None,
    )
    if args.uncertain_resume_reset:
        # Test-only transport loss after the real Kernel processed release.
        # No owner result, settlement, journal row or admission is fabricated.
        spawn = stack._spawn

        def spawn_with_one_lost_reply(name, command, **kwargs):
            if name == "Program runner":
                assert command[1:3] == ("-m", "manyforge_program_runner")
                injection = """
import runpy
from manyforge_program_runner.execution_transport import ProgramRunnerExecutionTransport
exchange = ProgramRunnerExecutionTransport._exchange_on
lost = False
def exchange_once(self, command, socket_path):
    global lost
    result = exchange(self, command, socket_path)
    if not lost and command.get('command') == 'release_program_selection':
        lost = True
        print('QUALIFICATION: discarded actual Kernel selection-release reply', flush=True)
        raise OSError('qualification: actual selection-release reply lost')
    return result
ProgramRunnerExecutionTransport._exchange_on = exchange_once
runpy.run_module('manyforge_program_runner', run_name='__main__')
"""
                command = (command[0], "-c", injection, *command[3:])
            spawn(name, command, **kwargs)

        stack._spawn = spawn_with_one_lost_reply
    browser = None
    try:
        try:
            stack.start()
        except Exception:
            # Capture before teardown: stopping Manager can itself expire the
            # Runner lease and must not be mistaken for the startup failure.
            for port, path in (
                (9102, "/api/program/liveness/status"),
                (9101, "/api/cell/authoring-admission"),
            ):
                try:
                    with HeadlessHttpClient(
                        f"http://127.0.0.1:{port}", timeout_seconds=1
                    ) as diagnostic_client:
                        print(
                            json.dumps(
                                {
                                    "startup_failure_projection": path,
                                    "status": diagnostic_client.request("GET", path),
                                }
                            ),
                            flush=True,
                        )
                except Exception as error:
                    print(f"Startup diagnostic {path}: {error}", flush=True)
            raise
        skill_authoring = check_skill_authoring()
        if args.jerk_authoring:
            print(
                json.dumps(check_jerk_authoring(args.jerk_authoring, stack, workbench)),
                flush=True,
            )
            return
        if args.authoring_groups:
            print(
                json.dumps(
                    check_group_authoring(args.authoring_groups, stack, workbench)
                )
            )
            return
        native = ExecutionCommandClient(workbench.STATE_ROOT / "execution-run")
        identity = native.identity()["identity"]
        if args.ordinary_jerk:
            descriptor = native.diagnostics()["status"]["direct_control"][
                "target_descriptor"
            ]
            assert (
                descriptor["schema_version"]
                == "manyforge.manual_control_motion_descriptor.v4"
            )
            profile = descriptor["ordinary_jerk"]
            assert profile["enabled"] and profile["preset_reference_time_s"] == 1.0
            assert [row["source_joint_name"] for row in profile["joint_limits"]] == [
                row["source_joint_name"] for row in descriptor["joints"]
            ]
            for row, effective in zip(
                profile["joint_limits"],
                descriptor["operational_joint_limits"],
                strict=True,
            ):
                name = row["source_joint_name"]
                if args.ordinary_jerk_import and name in {"joint1", "joint2", "joint3"}:
                    assert (
                        row["imported"]["source_digest"]
                        == "sha256:"
                        + hashlib.sha256(moveit_source.encode()).hexdigest()
                    )
                    assert row["imported"]["machine_required"] is False
                    assert row["imported"]["enabled"] is (name != "joint3")
                    if name != "joint3":
                        assert (row["source"], row["maximum_rad_s3"]) == (
                            ("override", 40) if name == "joint1" else ("imported", 55)
                        )
                    else:
                        assert row["source"] == "preset"
                        assert row["maximum_rad_s3"] == row["preset_rad_s3"] > 0
                else:
                    assert row["source"] == "preset" and row["imported"] is None
                    assert row["maximum_rad_s3"] == row["preset_rad_s3"] > 0
                assert effective["effective_max_jerk_rad_s3"] == row["maximum_rad_s3"]
        initial_tcp = (
            native.diagnostics()["status"]["direct_control"]["observed_state"][
                "current_tcp_pose"
            ]
            if args.cartesian_handover
            else None
        )
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
                request = OperationIntentRequest.model_validate(
                    {
                        "request_context": {
                            "request_id": f"manual-before-program.{attempt}",
                            "request_owner_epoch": identity["owner_epoch"],
                            "source_label": "native_control_qualification",
                        },
                        **{
                            key: direct_tool[key]
                            for key in (
                                "program_id",
                                "program_revision",
                                "program_binding_id",
                                "program_run_id",
                            )
                        },
                        **{
                            f"expected_{key}": status[key]
                            for key in (
                                "graph_revision",
                                "retained_bundle_id",
                                "scene_revision",
                            )
                        },
                        "participants": [
                            {
                                "component_role_id": direct_tool["component_role_id"],
                                "target_role_id": direct_tool["target_role_id"],
                                "required_capability_id": direct_tool[
                                    "operation_capability_id"
                                ],
                                "action_span": None,
                                "payload": {
                                    "kind": "capability_command",
                                    "command_schema_id": direct_tool[
                                        "command_schema_id"
                                    ],
                                    "command_schema_revision": direct_tool[
                                        "command_schema_revision"
                                    ],
                                    "command_value": {
                                        "kind": "gripper_position",
                                        "position": {
                                            "kind": "linear_m",
                                            "value_m": direct_tool["resource"][
                                                endpoint
                                            ],
                                        },
                                        "max_effort": None,
                                    },
                                    "completion_timeout_s": 2.0,
                                    "caller_options": [
                                        {
                                            "option_id": "command_duration_s",
                                            "value_s": 0.5,
                                        }
                                    ],
                                },
                            }
                        ],
                        "coordination_policy": "independent",
                    }
                )
                accepted = native.submit(request, trusted_principal_id=None)
                assert accepted["ok"], accepted
                operation_id = accepted["accepted"]["operation_id"]
                held = native._exchange(
                    {
                        "api_version": "manyforge.execution_command.v1",
                        "command": "cell_admission_get",
                    }
                )["authoring_admission"]
                assert not held["authoring_allowed"], held

                def manual_retired():
                    current = native.read(operation_id)
                    return current if current.get("resident") is False else None

                retired = workbench._wait_until(
                    "manual operation retirement", manual_retired, stack.processes
                )
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
            scene_baseline = (
                client.scene()["logical_scene"]["payload"]["snapshot"]["objects"]
                if args.scene_transactions
                else None
            )

            def ready_for_run():
                value = client.authoring_admission()
                return value if value["authoring_allowed"] else None

            admission = workbench._wait_until(
                "owner admission after manual operation retirement",
                ready_for_run,
                stack.processes,
            )
            start_request = {
                "request_id": "query-only.run.1",
                "request_owner_epoch": "query-only.client",
                "source_label": "query_only_qualification",
                "scenario_id": scenario["scenario_id"],
                "scenario_catalog_revision": stack.metadata[
                    "scenario_catalog_revision"
                ],
                "expected_applied_record_digest": applied["source"]["record_digest"],
                "draft_program_revision": program["program_revision"],
                "expected_admission_generation": admission["admission_generation"],
            }
            if args.runtime_groups:
                browser = SelectionBrowser(args.runtime_groups, stack, workbench)
                browser.open()
                browser.click(".transport-run", "Run")
                started = workbench._wait_until(
                    "GUI Run reached the owner",
                    lambda: client.program_cycle_status().get("run"),
                    stack.processes,
                )
            else:
                started = client.program_run_start(start_request)
            run_id = started["capture"]["program_run_id"]
            if transactions:
                first_node, second_node = (
                    ("attach", "detach")
                    if args.scene_transactions
                    else ("first", "second")
                )

                def attached_and_waiting():
                    current = client.program_run(run_id)
                    assert current["state"] in {"running", "succeeded"}, current
                    nodes = current["node_statuses"]
                    return (
                        current
                        if nodes.get(first_node) == "success"
                        and nodes.get("between") == "running"
                        else None
                    )

                workbench._wait_until(
                    "first native owner transaction",
                    attached_and_waiting,
                    stack.processes,
                )
                attached_scene = client.scene()["logical_scene"]["payload"]["snapshot"]
                if args.scene_transactions:
                    assert (
                        len(attached_scene["carried_object_attachments"]) == 1
                    ), attached_scene
                request = {
                    "program_run_id": run_id,
                    "expected_owner_epoch": started["owner_epoch"],
                    "request_id": "scene.stop",
                    "request_owner_epoch": "scene.qualification",
                    "source_label": "native_scene_qualification",
                    "group_ids": ["all"],
                }
                client.program_run_motion_stop(run_id, request)
                previous_held_revision = None

                def held_ready():
                    nonlocal previous_held_revision
                    current = client.program_run(run_id)
                    assert current["node_statuses"][first_node] == "success", current
                    assert current["node_statuses"][second_node] == "idle", current
                    controls = client.program_runtime_controls(run_id)
                    stable = previous_held_revision == controls["run_revision"]
                    previous_held_revision = controls["run_revision"]
                    return (
                        controls
                        if stable
                        and any(
                            row["group_id"] == "all" and row["resume_allowed"]
                            for row in controls["execution_selections"]["groups"]
                        )
                        else None
                    )

                controls = workbench._wait_until(
                    "transaction branch held", held_ready, stack.processes
                )
                assert (
                    client.scene()["logical_scene"]["payload"]["snapshot"][
                        "transaction_sequence"
                    ]
                    == attached_scene["transaction_sequence"]
                )
                try:
                    client.program_run_motion_resume(
                        run_id,
                        {
                            **request,
                            "request_id": "scene.resume",
                            "expected_run_revision": controls["run_revision"],
                            "expected_selection_revision": controls[
                                "execution_selections"
                            ]["selection_revision"],
                        },
                    )
                except HeadlessClientError as error:
                    assert args.uncertain_resume_reset, error
                    assert error.code == "program_cycle_reconciliation_required", error
                else:
                    assert (
                        not args.uncertain_resume_reset
                    ), "injected release loss did not reach Runner"

                def completed():
                    current = client.program_run(run_id)
                    assert current["state"] in {"running", "succeeded"}, current
                    return current if current["state"] == "succeeded" else None

                def detach_published():
                    value = client.scene()["logical_scene"]["payload"]["snapshot"]
                    return value if value["carried_object_attachments"] == [] else None

                if args.uncertain_resume_reset:
                    result = client.program_cycle_status()
                    assert result["state"] == "recovery_required", result
                    allowed = {
                        row["command_kind"]: row["allowed"]
                        for row in result["allowed_commands"]
                    }
                    assert (
                        allowed["program_reset"] and not allowed["program_run"]
                    ), result
                    assert not allowed["program_motion_resume"], result
                    with sqlite3.connect(
                        f"file:{state / 'owners/program-cycle.sqlite3'}?mode=ro",
                        uri=True,
                    ) as journal:
                        assert journal.execute(
                            "SELECT request_kind FROM program_cycle_inflight_commands"
                        ).fetchall() == [("resume_motion",)]
                else:
                    result = workbench._wait_until(
                        "second native owner transaction", completed, stack.processes
                    )
                    assert set(result["node_statuses"].values()) == {"success"}, result
                    final_scene = workbench._wait_until(
                        "Scene detach publication", detach_published, stack.processes
                    )
                    assert final_scene["carried_object_attachments"] == [], final_scene
                    assert final_scene["transaction_sequence"] == attached_scene[
                        "transaction_sequence"
                    ] + int(args.scene_transactions), final_scene

                def reset_ready():
                    value = client.program_cycle_status()
                    expected = (
                        "recovery_required"
                        if args.uncertain_resume_reset
                        else "terminal_awaiting_reset"
                    )
                    return value if value["state"] == expected else None

                cycle = workbench._wait_until(
                    "Scene cycle terminal", reset_ready, stack.processes
                )
                reset = client.program_cycle_reset(
                    {
                        "request_id": "scene.reset",
                        "request_owner_epoch": "scene.qualification",
                        "source_label": "native_scene_qualification",
                        "expected_owner_epoch": started["owner_epoch"],
                        "program_run_id": run_id,
                        "expected_cycle_generation": cycle["cycle_generation"],
                        "expected_claim_generation": cycle["cycle_claim_generation"],
                        "terminal_reference": cycle["terminal_or_settlement_reference"],
                        "settlement_evidence_digest": cycle[
                            "terminal_or_settlement_reference"
                        ],
                    }
                )
                assert (
                    client.program_cycle_status()["state"] == "idle_after_reset"
                ), reset
                if args.scene_transactions:
                    workbench._wait_until(
                        "Reset restores the authored Scene",
                        lambda: client.scene()["logical_scene"]["payload"]["snapshot"][
                            "objects"
                        ]
                        == scene_baseline,
                        stack.processes,
                    )
                if args.uncertain_resume_reset:
                    assert detach_published() is not None
                    with sqlite3.connect(
                        f"file:{state / 'owners/program-cycle.sqlite3'}?mode=ro",
                        uri=True,
                    ) as journal:
                        assert journal.execute(
                            "SELECT COUNT(*) FROM program_cycle_inflight_commands"
                        ).fetchone() == (0,)
                print(
                    json.dumps(
                        {
                            "passed": True,
                            "qualification": (
                                "scene_unknown_resume_reset"
                                if args.uncertain_resume_reset
                                else (
                                    "scene_stop_resume_reset"
                                    if args.scene_transactions
                                    else (
                                        "route_stop_resume_reset"
                                        if args.route_transactions
                                        else "lifecycle_stop_resume_reset"
                                    )
                                )
                            ),
                            "private_state": str(state),
                            "result": result,
                            "reset": reset,
                        },
                        sort_keys=True,
                    )
                )
                return
            if args.selections:
                api = "manyforge.execution_command.v1"

                def scope():
                    return native._exchange(
                        {
                            "api_version": api,
                            "command": "read_program_execution_scope",
                            "program_run_id": run_id,
                        }
                    )["scope"]

                def operation_for(target):
                    try:
                        return native.query_operation(
                            ProgramOperationQuery.model_validate(
                                {
                                    "schema_version": "manyforge.program_operation_query.v1",
                                    "owner_epoch": identity["owner_epoch"],
                                    "program_binding_id": stack.metadata[
                                        "program_binding_id"
                                    ],
                                    "program_run_id": run_id,
                                    "target_role_id": target,
                                }
                            )
                        )["operation"]
                    except HeadlessApiError as error:
                        assert (
                            error.code == "execution_operation_context_unavailable"
                        ), error
                        return None

                def both_moving():
                    arm, tool = operation_for("arm"), operation_for("gripper")
                    if not arm or arm["state"] in {"failed", "intervention_required"}:
                        run = client.program_run(run_id)
                        assert run["state"] in {"running", "starting"}, run
                    if not (
                        arm and tool and arm["state"] == tool["state"] == "executing"
                    ):
                        return None
                    return arm, tool

                workbench._wait_until(
                    "independent arm and gripper execution",
                    both_moving,
                    stack.processes,
                )

                def request(group, request_id):
                    return {
                        "program_run_id": run_id,
                        "expected_owner_epoch": started["owner_epoch"],
                        "request_id": request_id,
                        "request_owner_epoch": "selection.client",
                        "source_label": "native_selection_qualification",
                        "group_ids": [group] if isinstance(group, str) else list(group),
                    }

                def selected_command(group, request_id, controls=None):
                    if browser is None:
                        if controls is None:
                            return client.program_run_motion_stop(
                                run_id, request(group, request_id)
                            )
                        return client.program_run_motion_resume(
                            run_id,
                            {
                                **request(group, request_id),
                                "expected_run_revision": controls["run_revision"],
                                "expected_selection_revision": controls[
                                    "execution_selections"
                                ]["selection_revision"],
                            },
                        )
                    previous = scope()["selection_revision"]
                    browser.selection_command(group, resume=controls is not None)
                    workbench._wait_until(
                        "GUI selection reached Kernel",
                        lambda: scope()["selection_revision"] > previous,
                        stack.processes,
                    )
                    return client.program_run(run_id)

                logical = released = None
                if args.recover_parallel:
                    selected_command("supervision", "selection.logical")
                    logical = scope()
                    assert not logical["pending_effects"], logical
                    controls = client.program_runtime_controls(run_id)
                    groups = {
                        row["group_id"]: row
                        for row in controls["execution_selections"]["groups"]
                    }
                    assert (
                        groups["supervision"]["resume_allowed"]
                        and groups["all"]["state"] == "mixed"
                    ), groups
                    selected_command(
                        "supervision", "selection.logical.release", controls
                    )
                    released = scope()
                    assert not any(
                        released[key]
                        for key in (
                            "held_node_indices",
                            "stopping_node_indices",
                            "faulted_node_indices",
                        )
                    ), released

                def at_stop_progress():
                    moving = both_moving()
                    return (
                        moving
                        if moving
                        and moving[0]["participants"][0]["progress"]
                        >= args.stop_progress
                        else None
                    )

                # Finish the separate logical-selection check before measuring
                # the physical trigger. It must not consume a short late tail.
                arm, tool = workbench._wait_until(
                    "arm reached the Stop trigger", at_stop_progress, stack.processes
                )
                print(
                    json.dumps(
                        {
                            "selected_stop_progress": arm["participants"][0][
                                "progress"
                            ],
                            "requested_stop_progress": args.stop_progress,
                            "private_state": str(state),
                        },
                        sort_keys=True,
                    ),
                    flush=True,
                )
                first = selected_command("arm", "selection.arm")
                stopped_arm = operation_for("arm")
                assert (
                    stopped_arm is not None
                ), "arm completed before its Stop could be observed"
                arm_stop_id = stopped_arm["active_stop_id"]
                assert arm_stop_id, first
                assert operation_for("gripper")["active_stop_id"] is None, first
                tool_before = native.query_target(
                    TargetObservationQuery.model_validate(
                        {
                            **common,
                            "target_role_id": "gripper",
                            "kind": "gripper_status",
                        }
                    )
                )["target_observation"]

                def tool_advances():
                    value = native.query_target(
                        TargetObservationQuery.model_validate(
                            {
                                **common,
                                "target_role_id": "gripper",
                                "kind": "gripper_status",
                            }
                        )
                    )["target_observation"]
                    return (
                        value
                        if value["value"]["observed_position"]
                        != tool_before["value"]["observed_position"]
                        else None
                    )

                tool_after = workbench._wait_until(
                    "unselected gripper continues", tool_advances, stack.processes
                )
                # Default GUI/API Stop uses this same scoped owner path.
                all_request = request("all", "selection.all")
                del all_request["group_ids"]
                both = (
                    selected_command("all", "selection.all")
                    if browser is not None
                    else client.program_run_motion_stop(run_id, all_request)
                )
                assert operation_for("gripper")["active_stop_id"], both
                assert operation_for("arm")["active_stop_id"] == arm_stop_id, both

                def settled():
                    value = scope()
                    return (
                        value
                        if value["held_node_indices"] == [0, 1, 2]
                        and not any(
                            value[key]
                            for key in (
                                "pending_effects",
                                "stopping_node_indices",
                                "faulted_node_indices",
                            )
                        )
                        else None
                    )

                final_scope = workbench._wait_until(
                    "selected native effects settle", settled, stack.processes
                )
                # Reset can legitimately recover a containment fault. Prove
                # nominal Stop separately before that recovery can clear it.
                stop_fault = native.stop_fault()["stop_fault"]
                assert stop_fault is None, stop_fault
                recovery_result = None
                reset_result = None
                manual_handover_result = None
                if args.manual_handover:
                    # Use the exact owner-recorded motion payload as test input,
                    # not a second implementation of the native request schema.
                    # Its node authority is explicitly removed for manual work.
                    journal_path = (
                        workbench.STATE_ROOT
                        / "execution-state/operation-journal.sqlite3"
                    )
                    with sqlite3.connect(
                        f"file:{journal_path}?mode=ro", uri=True
                    ) as journal:
                        rows = journal.execute(
                            "SELECT payload_json FROM journal_records WHERE record_kind='operation_accepted' AND operation_id=?",
                            (arm["operation_id"],),
                        ).fetchall()
                    assert len(rows) == 1, rows
                    original = json.loads(rows[0][0])["intent"]
                    original_goal = deepcopy(
                        original["participants"][0]["payload"]["motion"]
                    )
                    status = native.diagnostics()["status"]
                    direct = status["direct_control"]
                    stopped_positions = list(direct["observed_state"]["positions_rad"])
                    manual = deepcopy(original)
                    manual.pop("request_attribution", None)
                    manual["request_context"] = {
                        "request_id": "handover.manual",
                        "request_owner_epoch": identity["owner_epoch"],
                        "source_label": "native_handover_qualification",
                        "correlation_id": None,
                    }
                    manual.update(
                        {
                            key: direct[key]
                            for key in (
                                "program_id",
                                "program_revision",
                                "program_binding_id",
                                "program_run_id",
                            )
                        }
                    )
                    manual["expected_scene_revision"] = status["scene_revision"]
                    participant = manual["participants"][0]
                    participant.pop("execution_scope", None)
                    participant["action_span"] = None
                    for key in ("component_role_id", "target_role_id"):
                        participant[key] = direct[key]
                    participant["required_capability_id"] = direct[
                        "operation_capability_id"
                    ]
                    motion = participant["payload"]
                    motion["move_id"] = "handover.manual"
                    motion["speed_profile"]["velocity_scale"] = 0.1
                    motion["motion"] = {
                        "kind": "joint_goal",
                        "joint_names": direct["observed_state"]["source_joint_names"],
                        "positions_rad": list(stopped_positions),
                    }
                    motion.update(
                        target_mode="absolute",
                        target_reference=None,
                        offset_reference=None,
                    )
                    motion["motion"]["positions_rad"][0] -= 0.2
                    manual = OperationIntentRequest.model_validate(manual).model_dump(
                        mode="json"
                    )
                    proof = client.read_manual_handover(manual)
                    assert proof["availability"]["available"], proof
                    assert proof["availability"]["cleanup_required"], proof
                    manual["manual_handover"] = proof["manual_handover"]
                    controls = client.program_runtime_controls(run_id)
                    client.request(
                        "POST",
                        f"/api/program/runs/{run_id}/manual-handover",
                        body={
                            "request_id": "handover.cleanup",
                            "request_owner_epoch": "selection.client",
                            "source_label": "native_handover_qualification",
                            "program_run_id": run_id,
                            "expected_owner_epoch": started["owner_epoch"],
                            "expected_run_revision": controls["run_revision"],
                            "operation": manual,
                        },
                    )
                    assert native.read(arm["operation_id"])["resident"] is False
                    proof = client.read_manual_handover(manual)
                    assert proof["availability"] == {
                        "available": True,
                        "cleanup_required": False,
                        "unavailable_reason": None,
                    }, proof
                    manual["manual_handover"] = proof["manual_handover"]
                    preview = client.preview_motion(manual)
                    assert preview["preview_receipt"]["commit_eligible"], preview
                    accepted_manual = client.execute(manual)
                    manual_id = accepted_manual["accepted"]["operation_id"]

                    def manual_moving():
                        operation = native.read(manual_id)["operation"]
                        assert operation["state"] not in {
                            "failed",
                            "intervention_required",
                        }, operation
                        return (
                            operation
                            if operation["state"] == "executing"
                            and operation["participants"][0]["progress"] >= 0.15
                            else None
                        )

                    active_manual = workbench._wait_until(
                        "held-cycle manual motion", manual_moving, stack.processes
                    )
                    controls = client.program_runtime_controls(run_id)
                    # Selection eligibility and resident manual work are
                    # separate owner projections, combined by the browser.
                    # Prove the actual backend exclusion, not a copied flag.
                    try:
                        selected_command("arm", "manual.must-not-resume", controls)
                    except HeadlessClientError as error:
                        assert (
                            error.code == "program_runtime_control_rejected"
                        ), error.response
                    else:
                        raise AssertionError("Program resumed during manual motion")
                    assert (
                        scope()["held_node_indices"] == final_scope["held_node_indices"]
                    )
                    assert not client.authoring_admission()["authoring_allowed"]
                    stop_request = StopIntentRequest.model_validate(
                        {
                            "request_context": {
                                **manual["request_context"],
                                "request_id": "handover.manual.stop",
                            },
                            "operations": [
                                {
                                    "operation_id": manual_id,
                                    "expected_operation_revision": active_manual[
                                        "operation_revision"
                                    ],
                                    "expected_stop_generation": active_manual[
                                        "active_stop_generation"
                                    ]
                                    or 0,
                                }
                            ],
                            "reason": "qualify manual Stop within held Program",
                        }
                    )
                    client.stop_operations(stop_request.model_dump(mode="json"))

                    def manual_retired():
                        value = native.read(manual_id)
                        return value if value["resident"] is False else None

                    manual_terminal = workbench._wait_until(
                        "manual Stop retirement", manual_retired, stack.processes
                    )
                    assert manual_terminal["operation"]["state"] in {
                        "stopped",
                        "completed",
                    }, manual_terminal
                    manual_stop_fault = native.stop_fault()["stop_fault"]
                    assert manual_stop_fault is None, manual_stop_fault
                    displaced = native.diagnostics()["status"]["direct_control"][
                        "observed_state"
                    ]["positions_rad"]
                    assert abs(displaced[0] - stopped_positions[0]) > 0.005, (
                        stopped_positions,
                        displaced,
                    )
                    assert (
                        scope()["held_node_indices"] == final_scope["held_node_indices"]
                    )
                    manual_handover_result = {
                        "operation_id": manual_id,
                        "stopped_positions": stopped_positions,
                        "manual_stopped_positions": displaced,
                        "original_goal": original_goal,
                    }
                if args.recover_parallel:

                    def recoverable():
                        controls = client.program_runtime_controls(run_id)
                        return (
                            controls
                            if any(
                                row["group_id"] == "all" and row["resume_allowed"]
                                for row in controls["execution_selections"]["groups"]
                            )
                            else None
                        )

                    controls = workbench._wait_until(
                        "selected Resume eligibility", recoverable, stack.processes
                    )
                    if browser is not None:
                        (state / "group-controls.png").write_bytes(
                            browser.driver.screenshot_png()
                        )
                    selected_command("arm", "parallel.resume.arm", controls)
                    controls = client.program_runtime_controls(run_id)
                    groups = {
                        row["group_id"]: row
                        for row in controls["execution_selections"]["groups"]
                    }
                    assert (
                        groups["arm"]["state"] == "running"
                        and groups["all_grippers"]["state"] == "held"
                    ), groups
                    assert (
                        groups["all"]["state"] == "mixed"
                        and groups["all"]["resume_allowed"]
                    ), groups
                    selected_command("all", "parallel.resume.remaining", controls)

                    def recovered():
                        value = client.program_run(run_id)
                        assert value["state"] in {
                            "running",
                            "finalizing",
                            "succeeded",
                        }, value
                        return value if value["state"] == "succeeded" else None

                    recovery_result = workbench._wait_until(
                        "fresh arm and gripper attempts complete",
                        recovered,
                        stack.processes,
                        # This Cartesian fixture declares a 30-second move
                        # budget; the generic workbench startup wait is 20s.
                        timeout_seconds=35 if args.cartesian_handover else 20,
                    )
                    assert recovery_result["node_statuses"] == {
                        "control_root": "success",
                        "controlled_motion": "success",
                        "moving_gripper": "success",
                    }, recovery_result
                    assert native.read(arm["operation_id"])["resident"] is False
                    assert native.read(tool["operation_id"])["resident"] is False
                    if manual_handover_result is not None:
                        final_observed = native.diagnostics()["status"][
                            "direct_control"
                        ]["observed_state"]
                        final_positions = final_observed["positions_rad"]
                        if args.cartesian_handover:
                            final_tcp = final_observed["current_tcp_pose"]
                            # The authored displacement is TCP-relative. Rotate
                            # it once using the pre-Run pose, never the manually
                            # displaced pose being used for Resume.
                            q = initial_tcp["orientation"]
                            x, y, z, w = (q[key] for key in "xyzw")
                            offset = (
                                2 * (x * z + y * w) * -0.08,
                                2 * (y * z - x * w) * -0.08,
                                (1 - 2 * (x * x + y * y)) * -0.08,
                            )
                            expected_xyz = {
                                key: initial_tcp["position_m"][key] + delta
                                for key, delta in zip(
                                    ("x_m", "y_m", "z_m"), offset, strict=True
                                )
                            }
                            assert all(
                                abs(final_tcp["position_m"][key] - value) < 0.005
                                for key, value in expected_xyz.items()
                            ), (final_tcp, expected_xyz)
                            dot = sum(
                                final_tcp["orientation"][key]
                                * initial_tcp["orientation"][key]
                                for key in "xyzw"
                            )
                            assert abs(dot) > 0.999, (initial_tcp, final_tcp)
                            manual_handover_result.update(
                                initial_tcp=initial_tcp, resumed_final_tcp=final_tcp
                            )
                        else:
                            assert all(
                                abs(actual - expected) < 0.01
                                for actual, expected in zip(
                                    final_positions,
                                    manual_handover_result["original_goal"][
                                        "positions_rad"
                                    ],
                                    strict=True,
                                )
                            ), (final_positions, manual_handover_result)
                        manual_handover_result["resumed_final_positions"] = (
                            final_positions
                        )
                else:

                    def reset_offer():
                        offer = client.program_runtime_controls(run_id)[
                            "recovery_offer"
                        ]
                        return (
                            offer
                            if offer
                            and any(
                                row["action_id"] == "reset_cycle" and row["enabled"]
                                for row in offer["actions"]
                            )
                            else None
                        )

                    offer = workbench._wait_until(
                        "selected Reset eligibility", reset_offer, stack.processes
                    )
                    reset_result = client.cycle_recovery_action(
                        {
                            "request_id": "selection.reset",
                            "request_owner_epoch": "selection.client",
                            "source_label": "native_selection_qualification",
                            "expected_owner_epoch": started["owner_epoch"],
                            "offer_id": offer["offer_id"],
                            "offer_revision": offer["offer_revision"],
                            "action_id": "reset_cycle",
                        }
                    )
                    assert (
                        reset_result["accepted"]
                        and reset_result["run"]["state"] == "stopped"
                    ), reset_result
                    cycle = client.program_cycle_status()
                    assert cycle["state"] == "idle_after_reset", cycle
                    assert (
                        cycle["run"] is None and cycle["runtime_controls"] is None
                    ), cycle
                    assert native.read(arm["operation_id"])["resident"] is False
                    assert native.read(tool["operation_id"])["resident"] is False
                print(
                    json.dumps(
                        {
                            "passed": True,
                            "qualification": (
                                "runner_scoped_stop_resume"
                                if args.recover_parallel
                                else "runner_scoped_stop_reset"
                            ),
                            "gui_commands": browser is not None,
                            "logical_hold": logical,
                            "logical_release": released,
                            "arm_stop": first,
                            "overlap_stop": both,
                            "unselected_gripper_before": tool_before,
                            "unselected_gripper_after": tool_after,
                            "final_scope": final_scope,
                            "recovered_program": recovery_result,
                            "manual_handover": manual_handover_result,
                            "reset_result": reset_result,
                            "private_state": str(state),
                        },
                        sort_keys=True,
                    )
                )
                return
            queried_operation: str | None = None
            observed_operation_scale = False
            resume_actor = None
            operation_query = ProgramOperationQuery.model_validate(
                {
                    "schema_version": "manyforge.program_operation_query.v1",
                    "owner_epoch": identity["owner_epoch"],
                    "program_binding_id": stack.metadata["program_binding_id"],
                    "program_run_id": run_id,
                    "target_role_id": "arm",
                }
            )

            def terminal():
                nonlocal queried_operation, observed_operation_scale, resume_actor
                current = client.program_run(run_id)
                nodes = current.get("node_statuses", {})
                if (
                    args.controls
                    and nodes.get("controlled_motion") == "running"
                    and (
                        not queried_operation
                        or (
                            nodes.get("scale") == "success"
                            and not observed_operation_scale
                        )
                    )
                ):
                    try:
                        selected = native.query_operation(operation_query)
                    except HeadlessApiError as error:
                        # A tree status can precede native admission or trail
                        # retirement. No fixture manufactures a resident effect.
                        assert (
                            error.code == "execution_operation_context_unavailable"
                        ), error
                    else:
                        operation = selected["operation"]
                        assert operation["program_run_id"] == run_id, selected
                        assert (
                            operation["program_binding_id"]
                            == operation_query.program_binding_id
                        )
                        if not queried_operation:
                            foreign = operation_query.model_copy(
                                update={"program_run_id": "foreign-run"}
                            )
                            try:
                                native.query_operation(foreign)
                            except HeadlessApiError as error:
                                assert (
                                    error.code
                                    == "execution_operation_context_unavailable"
                                ), error
                            else:
                                raise AssertionError(
                                    "query borrowed another Program's operation"
                                )
                            # The Program arm and manual gripper use disjoint
                            # engines. Exact manual admission must still reject
                            # the latter while this Program owns the cell.
                            competing = request.model_copy(
                                update={
                                    "request_context": request.request_context.model_copy(
                                        update={"request_id": "manual-during-program"}
                                    )
                                }
                            )
                            rejected = native.submit(
                                competing, trusted_principal_id=None
                            )
                            assert rejected["ok"] is False, rejected
                            assert (
                                rejected["error"]["details"]["reason"]
                                == "positive_admission_required"
                            ), rejected
                            queried_operation = operation["operation_id"]
                        scale = selected.get("runtime_scale")
                        if (
                            nodes.get("scale") == "success"
                            and scale
                            and scale["state"] == "applied"
                        ):
                            assert scale["effective_scale"] == 0.4, selected
                            observed_operation_scale = True
                if (
                    args.controls
                    and queried_operation
                    and resume_actor is None
                    and nodes.get("resume") in {"running", "success"}
                ):
                    # Check the actual current command record before normal
                    # completion cleanup compacts it to the latest control.
                    journal_path = (
                        workbench.STATE_ROOT
                        / "execution-state/operation-journal.sqlite3"
                    )
                    with sqlite3.connect(
                        f"file:{journal_path}?mode=ro", uri=True
                    ) as journal:
                        controls = [
                            json.loads(row[0])
                            for row in journal.execute(
                                "SELECT payload_json FROM journal_records "
                                "WHERE record_kind = 'control_accepted' AND operation_id = ?",
                                (queried_operation,),
                            )
                        ]
                    resumes = [
                        row for row in controls if row["result"]["control"] == "resume"
                    ]
                    if resumes:
                        assert len(resumes) == 1, controls
                        resume_actor = resumes[0]["intent"]["execution_scope"]
                        assert (
                            resume_actor["node_instance_id"] == "resume"
                        ), resume_actor
                        assert resume_actor["node_attempt_id"].startswith(
                            "attempt:"
                        ), resume_actor
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
                    "check_height": "success",
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
            if not args.controls:

                def forensic_export():
                    export = client.request(
                        "GET", "/api/debug/export", params=(("program_run_id", run_id),)
                    )
                    return export if export["blackboard"]["available"] else None

                evidence = workbench._wait_until(
                    "Program producer values reach Historian/export",
                    forensic_export,
                    stack.processes,
                )
                samples = evidence["blackboard"]["samples"]
                assert any(
                    "observed_pose" in row["values"] for row in samples
                ), evidence
                assert not evidence["coverage"]["truncated"], evidence
            if args.controls:
                assert queried_operation and observed_operation_scale and resume_actor
                assert native.read(queried_operation)["resident"] is False
                try:
                    native.query_operation(operation_query)
                except HeadlessApiError as error:
                    assert (
                        error.code == "execution_operation_context_unavailable"
                    ), error
                else:
                    raise AssertionError(
                        "query treated retired history as current authority"
                    )
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
                    "skill_authoring": skill_authoring,
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
    try:
        main()
    except HeadlessClientError as error:
        print(
            json.dumps(
                {
                    "code": error.code,
                    "details": error.details,
                    "response": error.response,
                },
                default=str,
            ),
            flush=True,
        )
        raise
