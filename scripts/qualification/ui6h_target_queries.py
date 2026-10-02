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
import math
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import time

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    compute_content_identity,
    load_strict_json_yaml,
    raw_sha256_digest,
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


def read_stop_terminal(native, owner_epoch, stop_id):
    """Read exact durable owner evidence; never infer rest from a receipt."""
    cursor = 0
    # This private short workflow has a bounded history. Exhaustion is a failed
    # qualification, not permission to use another Stop's latest projection.
    for _ in range(64):
        page = native.events(owner_epoch, cursor)
        for event in page["events"]:
            if event["record_kind"] != "stop_terminal_projected":
                continue
            terminal = event["payload"]
            if terminal["stop_id"] == stop_id:
                assert terminal["owner_epoch"] == owner_epoch, terminal
                assert not terminal["details_truncated"], terminal
                return terminal
        if not page["has_more"]:
            return None
        assert page["next_after_sequence"] > cursor, page
        cursor = page["next_after_sequence"]
    raise AssertionError(
        "Stop terminal evidence exceeds the short-workflow history bound"
    )


def check_completed_program_operations(native, composer, journal_path, run_id):
    """Join captured Program completion to its exact durable acknowledgement."""
    with sqlite3.connect(f"file:{journal_path}?mode=ro", uri=True) as journal:
        operations = journal.execute(
            "SELECT operation_id FROM operation_snapshots "
            "WHERE program_run_id=? AND state='completed' ORDER BY operation_id",
            (run_id,),
        ).fetchall()
        checked = []
        for (operation_id,) in operations:
            rows = journal.execute(
                "SELECT sequence,payload_json FROM journal_records "
                "WHERE operation_id=? AND record_kind IN "
                "('operation_accepted','control_accepted') ORDER BY sequence",
                (operation_id,),
            ).fetchall()
            records = [(sequence, json.loads(payload)) for sequence, payload in rows]
            accepted = [
                row for _, row in records if row["record_kind"] == "operation_accepted"
            ]
            assert len(accepted) == 1, (operation_id, accepted)
            intent = accepted[0]["intent"]
            scopes = [row.get("execution_scope") for row in intent["participants"]]
            assert all(scope and scope["node_attempt_id"] for scope in scopes), intent
            acknowledgements = [
                (sequence, row)
                for sequence, row in records
                if row["record_kind"] == "control_accepted"
                and row["result"]["control"] == "acknowledge_completed"
            ]
            assert len(acknowledgements) == 1, (operation_id, acknowledgements)
            sequence, acknowledgement = acknowledgements[0]
            assert acknowledgement["operation_state"]["state"] == "completed"
            observed = native.read(operation_id)
            assert observed["operation"]["state"] == "completed", observed
            assert observed["resident"] is False, observed
            assert (
                composer.operation(operation_id)["operation"] == observed["operation"]
            )
            assert (
                observed["operation"]["operation_revision"]
                == acknowledgement["result"]["operation_revision_after_acceptance"]
            ), acknowledgement
            serialized = json.dumps(intent, sort_keys=True)
            assert (
                '"robot_id"' not in serialized and '"group_id"' not in serialized
            ), intent
            assert [
                row["execution_scope"]
                for row in acknowledgement["operation_state"]["participants"]
            ] == scopes
            event = native.events(
                acknowledgement["owner_epoch"], sequence - 1, limit=1
            )["events"]
            assert len(event) == 1 and event[0]["sequence"] == sequence, event
            assert event[0]["payload"] == acknowledgement, event
            checked.append(
                {
                    "operation_id": operation_id,
                    "scopes": scopes,
                    "acknowledgement_sequence": sequence,
                    "accepted_payloads": [
                        row["payload"] for row in intent["participants"]
                    ],
                }
            )
    assert {
        scope["node_instance_id"] for row in checked for scope in row["scopes"]
    } == {"controlled_motion", "moving_gripper"}, checked
    return checked


def check_stop_history(native, journal_path, terminal):
    """Join the actual selected Stop to its durable acceptance and terminal."""
    with sqlite3.connect(f"file:{journal_path}?mode=ro", uri=True) as journal:
        rows = journal.execute(
            "SELECT sequence,record_kind,payload_json,payload_digest "
            "FROM journal_records WHERE stop_id=? AND record_kind IN "
            "('stop_acceptance_projected','stop_terminal_projected') "
            "ORDER BY sequence",
            (terminal["stop_id"],),
        ).fetchall()
    assert [row[1] for row in rows] == [
        "stop_acceptance_projected",
        "stop_terminal_projected",
    ], rows
    for sequence, kind, payload_json, digest in rows:
        page = native.events(terminal["owner_epoch"], sequence - 1, limit=1)
        assert len(page["events"]) == 1, page
        event = page["events"][0]
        assert event["sequence"] == sequence, event
        assert event["record_kind"] == kind, event
        assert event["operation_id"] is None, event
        assert event["operation_revision"] is None, event
        if kind == "stop_acceptance_projected":
            assert event["request_id"] == terminal["request_id"], event
            assert event["payload"] == json.loads(payload_json), event
            assert event["payload"]["intent"]["value"]["request_attribution"] == {
                "assurance": "unverified",
                "principal_id": None,
                "transport": "internal",
            }, event
        else:
            assert event["request_id"] is None, event
            assert event["payload"] == terminal, event
            assert terminal["api_version"] == "manyforge.stop_terminal_projection.v1"
            assert terminal["durable_projection_digest"] == digest, terminal
            assert terminal["details_truncated"] is False, terminal


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
                    "content_version": "2.4.0",
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


def check_jerk_authoring(endpoint, stack, workbench, *, pending_scene_apply=False):
    """Real selector/Save/Apply/reload, on a private Deployment, without motion."""
    browser = SelectionBrowser(endpoint, stack, workbench)
    selector = 'select[aria-label="lite6_robot / arm ordinary motion jerk"]'
    client = HeadlessHttpClient("http://127.0.0.1:9001", timeout_seconds=5)
    apply_pending = False

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
            "//div[@aria-label='Deployment document controls']"
            "//div[@class='document-actions']/button[normalize-space(.)="
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
            ".document-dialog > button.primary-action", "Open as Draft"
        )
        browser.wait(
            "loaded Deployment", lambda: workspace()["draft"]["document"] is not None
        )
        original = workspace()
        saved_path = Path(original["saved"]["path"]).resolve()
        assert saved_path.is_relative_to(workbench.STATE_ROOT.parent), saved_path
        original_settings = settings(original["draft"]["document"])
        assert original_settings["enabled"] is True
        required = any(
            row.get("imported", {}).get("machine_required") is True
            for row in original_settings["joint_limits"]
        )
        results = []
        deadline_failures = []
        for enabled in (True,) if required else (False, True):
            reference_time = original_settings["preset_reference_time_s"] * (
                2 if required else 1
            )
            # Observe client parser refusals without changing socket behavior.
            # Reload below discards this bounded diagnostic wrapper each time.
            browser.js(
                "window.__qualificationCloses = [];"
                "const original = WebSocket.prototype.close;"
                "WebSocket.prototype.close = function(code, reason) {"
                "if (window.__qualificationCloses.length < 8) "
                "window.__qualificationCloses.push({url:this.url,code,reason});"
                "return original.call(this,code,reason);}; return true;"
            )
            if required:
                assert browser.js(
                    "return document.querySelector("
                    + json.dumps(selector)
                    + ")?.querySelector('option[value=off]')?.disabled === true;"
                )
                field = (
                    "//label[span[normalize-space(.)='Preset reference time']]//input"
                )
                click_xpath(field)
                element = browser.driver.command(
                    "POST", "/element", {"using": "xpath", "value": field}
                )["element-6066-11e4-a52e-4f735466cecf"]
                browser.driver.command("POST", f"/element/{element}/clear", {})
                browser.driver.command(
                    "POST",
                    f"/element/{element}/value",
                    {
                        "text": str(reference_time) + "\ue007",
                    },
                )
            else:
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
                    and settings(current["draft"]["document"])[
                        "preset_reference_time_s"
                    ]
                    == reference_time
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
            assert saved == {
                **original_settings,
                "enabled": enabled,
                "preset_reference_time_s": reference_time,
            }, saved
            action("Preview Draft")
            apply_pending = True
            if pending_scene_apply:
                from ui6i_shutdown_check import check_pending_scene_read_shutdown

                print(
                    json.dumps(
                        check_pending_scene_read_shutdown(
                            stack,
                            workbench.STATE_ROOT,
                            workbench,
                            apply_change=lambda: action("Apply Draft"),
                        )
                    ),
                    flush=True,
                )
            else:
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

            try:
                applied = browser.wait("applied jerk choice", applied_result)
            except workbench.WorkbenchError as error:
                failure = {"enabled": enabled, "error": str(error)}
                deadline_failures.append(failure)
                print(json.dumps({"apply_deadline_failure": failure}), flush=True)
                # Reuse the bounded post-failure observation period. A late
                # success permits the remaining functional checks, but never
                # erases this failure or changes the overall exit status.
                try:
                    applied = browser.wait(
                        "late Apply completion after failed deadline", applied_result
                    )
                finally:
                    apply_pending = False
            apply_pending = False
            closes = browser.js("return window.__qualificationCloses;")
            assert not any(row.get("code") == 4002 for row in closes), closes
            assert (
                applied["managerApplied"]["deploymentRevision"]
                == changed["draft"]["deploymentRevision"]
            )
            native = ExecutionCommandClient(workbench.STATE_ROOT / "execution-run")
            descriptor = native.diagnostics()["status"]["direct_control"][
                "target_descriptor"
            ]
            profile = descriptor["ordinary_jerk"]
            assert profile["enabled"] is enabled
            assert (
                profile["preset_reference_time_s"] == saved["preset_reference_time_s"]
            )
            authored = {row["source_joint_name"]: row for row in saved["joint_limits"]}
            assert [row["source_joint_name"] for row in profile["joint_limits"]] == [
                row["source_joint_name"]
                for row in descriptor["operational_joint_limits"]
            ]
            for joint, effective in zip(
                profile["joint_limits"],
                descriptor["operational_joint_limits"],
                strict=True,
            ):
                setting = authored.get(joint["source_joint_name"], {})
                imported = setting.get("imported")
                assert joint["imported"] == imported, joint
                if not enabled:
                    expected_source, expected_maximum = "off", None
                elif "override_rad_s3" in setting:
                    expected_source, expected_maximum = (
                        "override",
                        setting["override_rad_s3"],
                    )
                elif imported is not None and imported["enabled"]:
                    expected_source, expected_maximum = (
                        "imported",
                        imported["maximum_rad_s3"],
                    )
                else:
                    expected_source, expected_maximum = "preset", joint["preset_rad_s3"]
                    assert expected_maximum > 0
                assert (joint["source"], joint["maximum_rad_s3"]) == (
                    expected_source,
                    expected_maximum,
                ), joint
                assert (
                    effective["effective_max_jerk_rad_s3"] == expected_maximum
                ), effective
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
            browser.wait(
                "reloaded authoritative Run readiness",
                lambda: workspace()["runAdmission"]["ready"]
                and browser.js(
                    "const button = document.querySelector('button.transport-run');"
                    "return !!button && !button.disabled && button.textContent.trim() === 'Run';"
                ),
            )
            results.append(
                {
                    "enabled": enabled,
                    "machine_required": required,
                    "reload_run_available": True,
                    "deployment_revision": applied["managerApplied"][
                        "deploymentRevision"
                    ],
                    "ordinary_jerk": profile,
                }
            )
        screenshot = workbench.STATE_ROOT.parent / "jerk-authoring.png"
        screenshot.write_bytes(browser.driver.screenshot_png())
        return {
            "passed": not deadline_failures,
            "functional_complete": True,
            "deadline_failures": deadline_failures,
            "jerk_authoring": results,
            "motion_issued": False,
            "screenshot": str(screenshot),
        }
    except Exception:
        if apply_pending:
            # Keep the original failure and its deadline. Observe the already
            # submitted request before cleanup can turn it into a transport
            # error; this period cannot convert the failed check into a pass.
            try:
                terminal = browser.wait(
                    "failed Apply terminal evidence",
                    lambda: browser.js(
                        "const root = document.querySelector('.deployment-workspace');"
                        "const error = root?.querySelector('.panel-state.error-state')?.textContent;"
                        "const feedback = root?.querySelector('.program-feedback')?.textContent;"
                        "return error ? {error} : feedback?.includes('rebound to the current Program.')"
                        " ? {feedback} : null;"
                    ),
                )
                print(
                    json.dumps({"failed_apply_terminal_evidence": terminal}), flush=True
                )
            except Exception as diagnostic_error:
                print(
                    json.dumps(
                        {
                            "failed_apply_terminal_evidence_unavailable": str(
                                diagnostic_error
                            )
                        }
                    ),
                    flush=True,
                )
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
        "--pending-scene-shutdown",
        action="store_true",
        help="Hold one actual native Scene read and prove clean managed shutdown; no motion",
    )
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
        "--lost-scene-reply",
        action="store_true",
        help="With --scene-transactions, recover a committed attach receipt only after Stop",
    )
    parser.add_argument(
        "--pending-scene-apply",
        action="store_true",
        help="Hold a Scene read after the real jerk-authoring Apply fence",
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
        "--joint-subset",
        action="store_true",
        help="With --selections --recover-parallel, qualify a captured one-joint Program goal",
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
        "--ordinary-jerk-required",
        action="store_true",
        help="Retain a machine-required imported cap in the selected authoring or execution workflow",
    )
    parser.add_argument(
        "--scale-stop",
        action="store_true",
        help="Qualify repeated live scaling and Stop during a transition in the selected native workflow",
    )
    parser.add_argument(
        "--stop-mode",
        choices=("soft_on_path", "spring_back_hold"),
        help="Save and Apply this controlled Stop mode in the private Deployment",
    )
    parser.add_argument(
        "--natural-completion",
        choices=("on", "off"),
        help="Explicit saved short-tail choice with --stop-mode",
    )
    parser.add_argument(
        "--stop-jerk",
        choices=("on", "off"),
        help="Save an explicit controlled-Stop jerk choice, independently of ordinary jerk",
    )
    parser.add_argument(
        "--stop-ramp-duration",
        type=float,
        help="Save the controlled Stop ramp duration in seconds; normal compiler validation applies",
    )
    parser.add_argument(
        "--expect-stop-refusal",
        action="store_true",
        help="Require an infeasible Soft ramp to escalate, settle and retain a fault blocking Resume",
    )
    parser.add_argument(
        "--expect-natural-completion",
        choices=("on", "off"),
        help="Require the exact arm Stop certificate to report this outcome",
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
    if args.ordinary_jerk_required and not args.ordinary_jerk_import:
        parser.error("--ordinary-jerk-required requires --ordinary-jerk-import")
    if args.natural_completion is not None and args.stop_mode is None:
        parser.error("--natural-completion requires --stop-mode")
    stop_profile_edit = (
        args.stop_mode is not None
        or args.stop_jerk is not None
        or args.stop_ramp_duration is not None
    )
    if (stop_profile_edit or args.expect_natural_completion) and not args.selections:
        parser.error("Stop profile/outcome checks require --selections")
    if args.expect_stop_refusal and (
        not args.selections
        or args.stop_ramp_duration is None
        or args.recover_parallel
        or args.expect_natural_completion is not None
        or args.stop_mode == "spring_back_hold"
    ):
        parser.error(
            "--expect-stop-refusal requires --selections --stop-ramp-duration, "
            "without recovery, Spring or natural-completion outcome checks"
        )
    if args.scale_stop and not (
        args.ordinary_jerk and args.selections and args.recover_parallel
    ):
        parser.error(
            "--scale-stop requires --ordinary-jerk --selections --recover-parallel"
        )
    if args.pending_scene_apply and not args.jerk_authoring:
        parser.error("--pending-scene-apply requires --jerk-authoring")
    if args.jerk_authoring and (
        not args.ordinary_jerk
        or args.controls
        or args.selections
        or args.scene_transactions
        or args.authoring_groups
    ):
        parser.error(
            "--jerk-authoring requires --ordinary-jerk and runs separately without motion"
        )
    if args.uncertain_resume_reset and not args.scene_transactions:
        parser.error(
            "--uncertain-resume-reset requires --scene-transactions (no arm motion)"
        )
    if args.lost_scene_reply and not args.scene_transactions:
        parser.error("--lost-scene-reply requires --scene-transactions")
    # Component lifecycle and route changes are explicit idle-time
    # operations, not Program nodes; only Scene transactions run in a Program.
    transactions = int(args.scene_transactions)
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
    if args.joint_subset and (
        not args.selections or not args.recover_parallel or args.cartesian_handover
    ):
        parser.error(
            "--joint-subset requires --selections --recover-parallel without Cartesian"
        )
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
    scenario = {"scenario_id": "lite6_pick_and_place"}
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
            # The controls row additionally scales to 0.4 after Pause. Keep
            # that semantic check short without changing Stop-profile rows.
            velocity_scale=0.4 if args.controls else 0.2,
            acceleration_scale=0.25,
        )
        if args.joint_subset:
            motion["parameters"]["goal"] = motion["parameters"]["goal"][:1]
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
                "max_tcp_linear_speed_mps": 0.05,
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
    if args.ordinary_jerk or stop_profile_edit:
        from manyforge_composer.backend.deployment_draft import (
            compile_deployment_document,
        )

        candidate = load_strict_json_yaml(deployment_path)
        edited = 0
        if args.ordinary_jerk:
            for unit in candidate["graph"]["runtime_units"]:
                for target in unit.get("configuration", {}).get(
                    "motion_target_limits", []
                ):
                    target["ordinary_motion_jerk"]["enabled"] = True
                    edited += 1
            assert edited > 0, "no native ordinary jerk settings in candidate"
        if stop_profile_edit:
            stop_rows = [
                row
                for row in candidate["policies"]["stop_stage_parameters"]
                if row["provider_contract_ref"]["contract_schema_id"]
                == "manyforge.value_schema.provider_contract.motion_controlled_stop.v1"
            ]
            assert stop_rows, "no controlled Stop settings in candidate"
            for row in stop_rows:
                profile = row["default_contract_parameters"]
                if args.stop_mode:
                    profile["stop_mode"] = args.stop_mode
                    profile["allow_natural_completion_on_short_path"] = (
                        args.natural_completion == "on"
                        if args.natural_completion is not None
                        else args.stop_mode == "soft_on_path"
                    )
                if args.stop_jerk is not None:
                    profile["jerk_limits_enabled"] = args.stop_jerk == "on"
                if args.stop_ramp_duration is not None:
                    profile["deceleration_time_s"] = args.stop_ramp_duration
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
            if args.ordinary_jerk_required:
                required_candidate = json.loads(compiled)
                # Explicit private machine policy, before compilation/sealing;
                # source metadata still identifies the actual imported bytes.
                required_candidate["graph"]["runtime_units"][0]["configuration"][
                    "motion_target_limits"
                ][0]["ordinary_motion_jerk"]["joint_limits"][0]["imported"][
                    "machine_required"
                ] = True
                compiled = compile_deployment_document(
                    json.dumps(required_candidate)
                ).canonical_json
        deployment_path.write_text(compiled)
    scenario_document_path = (
        workspace / "examples" / f"{scenario['scenario_id']}.scenario"
    )
    member_digests = {
        name: raw_sha256_digest((scenario_document_path / name).read_bytes())
        for name in (
            "assignment.yaml",
            "deployment.yaml",
            "overrides.yaml",
            "program.yaml",
        )
    }
    source_document_digest = raw_sha256_digest(
        canonical_json_bytes(
            {
                "manifest": raw_sha256_digest(
                    (scenario_document_path / "scenario.yaml").read_bytes()
                ),
                "members": member_digests,
            }
        )
    )
    deployment = load_strict_json_yaml(deployment_path)
    program = load_strict_json_yaml(program_path)
    capture = {
        "source_document_digest": source_document_digest,
        "deployment": deployment,
        "program": program,
        "program_role_assignment": load_strict_json_yaml(
            scenario_document_path / "assignment.yaml"
        ),
        "typed_parameter_overrides": load_strict_json_yaml(
            scenario_document_path / "overrides.yaml"
        ),
    }
    preparation_request = {
        "schema_version": "manyforge.activation_preparation_request.v1",
        **capture,
        "captured_input_digest": raw_sha256_digest(canonical_json_bytes(capture)),
    }
    preparation_path = state / "activation-preparation.json"
    preparation_path.write_bytes(canonical_json_bytes(preparation_request))
    subprocess.run(
        [
            str(compiler),
            "--input",
            str(preparation_path),
            "--capability-root",
            str(workspace),
            "--candidate-root",
            str(package),
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
    workbench._ensure_activation_preparer = lambda: (compiler, prepared)
    if args.ordinary_jerk or stop_profile_edit:
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
    if (
        args.uncertain_resume_reset
        or args.lost_scene_reply
    ):
        # Test-only transport loss after the real owner processed a command.
        # No owner result, settlement, journal row or admission is fabricated.
        spawn = stack._spawn

        def spawn_with_one_lost_reply(name, command, **kwargs):
            if name == "Program runner":
                assert command[1:3] == ("-m", "manyforge_program_runner")
                injection = "import runpy\n"
                if args.uncertain_resume_reset:
                    injection += """
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
"""
                if args.lost_scene_reply:
                    injection += f"""
import json
from pathlib import Path
from manyforge_headless_api.client import HeadlessHttpClient
commit = HeadlessHttpClient.commit_scene_transaction
receipt = HeadlessHttpClient.scene_transaction_status
lost_scene_request = None
scene_evidence = Path({str(state / 'lost-scene-reply.json')!r})
scene_release = Path({str(state / 'release-scene-reply')!r})
def commit_once(self, body):
    global lost_scene_request
    request_id = body['request_context']['request_id']
    attaching = body['operation']['kind'] == 'attach_carried_object'
    assert not (attaching and lost_scene_request is not None), 'Scene effect was resubmitted'
    result = commit(self, body)
    if attaching:
        assert result['ok'] and result['projection_status'] == 'committed', result
        lost_scene_request = request_id
        scene_evidence.write_text(json.dumps({{'request_id': request_id, 'committed': result}}))
        print('QUALIFICATION: discarded committed Scene reply', request_id, flush=True)
        raise OSError('qualification: actual committed Scene reply lost')
    return result
def read_receipt(self, body):
    result = receipt(self, body)
    if body['request_context']['request_id'] == lost_scene_request and result.get('result'):
        if not scene_release.exists():
            raise OSError('qualification: Scene receipt unavailable until held')
        assert result['result']['request_id'] == lost_scene_request, result
        print('QUALIFICATION: recovered Scene receipt', lost_scene_request, flush=True)
        evidence = json.loads(scene_evidence.read_text())
        evidence['recovered'] = result
        scene_evidence.write_text(json.dumps(evidence))
    return result
HeadlessHttpClient.commit_scene_transaction = commit_once
HeadlessHttpClient.scene_transaction_status = read_receipt
"""
                injection += "runpy.run_module('manyforge_program_runner', run_name='__main__')\n"
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
        if args.pending_scene_shutdown:
            from ui6i_shutdown_check import check_pending_scene_read_shutdown

            print(
                json.dumps(
                    check_pending_scene_read_shutdown(
                        stack, workbench.STATE_ROOT, workbench
                    )
                )
            )
            return
        skill_authoring = check_skill_authoring()
        if args.jerk_authoring:
            result = check_jerk_authoring(
                args.jerk_authoring,
                stack,
                workbench,
                pending_scene_apply=args.pending_scene_apply,
            )
            print(json.dumps(result), flush=True)
            if not result["passed"]:
                raise SystemExit(1)
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
                    assert row["imported"]["machine_required"] is (
                        args.ordinary_jerk_required and name == "joint1"
                    )
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
            print(json.dumps({"ordinary_jerk": profile}), flush=True)
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
                "source_document_digest": applied[
                    "source_document_digest"
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
                first_node, second_node = ("attach", "detach")

                def attached_and_waiting():
                    current = client.program_run(run_id)
                    assert current["state"] in {"running", "succeeded"}, current
                    nodes = current["node_statuses"]
                    if args.lost_scene_reply:
                        return (
                            current
                            if (state / "lost-scene-reply.json").exists()
                            and nodes.get(first_node) == "running"
                            else None
                        )
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
                if args.lost_scene_reply:
                    unknown = client.program_run(run_id)
                    assert unknown["node_statuses"][first_node] == "running", unknown
                    assert unknown["node_statuses"][second_node] == "idle", unknown
                    unknown_controls = client.program_runtime_controls(run_id)
                    assert not next(
                        row["resume_allowed"]
                        for row in unknown_controls["execution_selections"]["groups"]
                        if row["group_id"] == "all"
                    ), unknown_controls
                    (state / "release-scene-reply").touch()
                previous_held_revision = None

                def held_ready():
                    nonlocal previous_held_revision
                    current = client.program_run(run_id)
                    assert current["node_statuses"][first_node] in {
                        "running",
                        "success",
                    }, current
                    assert current["node_statuses"][second_node] == "idle", current
                    controls = client.program_runtime_controls(run_id)
                    stable = previous_held_revision == controls["run_revision"]
                    previous_held_revision = controls["run_revision"]
                    return (
                        controls
                        if current["node_statuses"][first_node] == "success"
                        and stable
                        and any(
                            row["group_id"] == "all" and row["resume_allowed"]
                            for row in controls["execution_selections"]["groups"]
                        )
                        else None
                    )

                controls = workbench._wait_until(
                    "transaction branch held", held_ready, stack.processes
                )
                if args.lost_scene_reply:
                    evidence = json.loads((state / "lost-scene-reply.json").read_text())
                    recovered = evidence["recovered"]
                    assert recovered["ok"] and recovered["result"]["ok"], recovered
                    assert recovered["result"] == {
                        **evidence["committed"],
                        "replay_status": "replayed_exact",
                    }, evidence
                    assert (
                        recovered["result"]["request_id"] == evidence["request_id"]
                    ), evidence
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
                            "lost_scene_reply_recovered": args.lost_scene_reply,
                            "qualification": (
                                "scene_unknown_resume_reset"
                                if args.uncertain_resume_reset
                                else "scene_stop_resume_reset"
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

                scale_transitions = []
                if args.scale_stop:
                    direct = native.diagnostics()["status"]["direct_control"]
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
                    for index, target_scale in enumerate((0.4, 0.9, 0.6)):
                        before_scale = native.get_runtime_scale_control(scale_selector)[
                            "runtime_scale_control"
                        ]
                        assert (
                            before_scale["active_operation_id"]
                            == operation_for("arm")["operation_id"]
                        ), before_scale
                        started_ns = time.monotonic_ns()
                        accepted_scale = native.set_runtime_scale_control(
                            RuntimeScaleControlRequest.model_validate(
                                {
                                    "request_context": {
                                        "request_id": f"selection.scale.{index}",
                                        "request_owner_epoch": identity["owner_epoch"],
                                        "source_label": "native_scale_stop_qualification",
                                        "correlation_id": run_id,
                                    },
                                    "selector": scale_selector.model_dump(),
                                    "expected_control_generation": before_scale[
                                        "control_generation"
                                    ],
                                    "scale": target_scale,
                                }
                            ),
                            trusted_principal_id=None,
                        )["accepted"]
                        accepted_ns = time.monotonic_ns()

                        def scale_observed():
                            current = native.get_runtime_scale_control(scale_selector)[
                                "runtime_scale_control"
                            ]
                            assert (
                                current["control_generation"]
                                == accepted_scale["control_generation"]
                            ), current
                            assert current["requested_scale"] == target_scale, current
                            effective = current["effective_scale"]
                            reached = (
                                effective == target_scale
                                if index < 2
                                else (
                                    effective is not None
                                    and target_scale < effective < 0.9
                                )
                            )
                            return current if reached else None

                        observed_scale = workbench._wait_until(
                            (
                                "native scale convergence"
                                if index < 2
                                else "native scale transition before Stop"
                            ),
                            scale_observed,
                            stack.processes,
                        )
                        scale_transitions.append(
                            {
                                "target": target_scale,
                                "acceptance_latency_ns": accepted_ns - started_ns,
                                "observation_latency_ns": time.monotonic_ns()
                                - accepted_ns,
                                "observed": observed_scale,
                            }
                        )
                    print(
                        json.dumps({"scale_transitions": scale_transitions}), flush=True
                    )

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

                arm_terminal = workbench._wait_until(
                    "exact durable arm Stop certificate",
                    lambda: read_stop_terminal(
                        native, identity["owner_epoch"], arm_stop_id
                    ),
                    stack.processes,
                )
                # Retain the exact outcome even when a configured profile
                # fails; waiting for a fault-free held scope would obscure it.
                print(json.dumps({"arm_stop_terminal": arm_terminal}), flush=True)
                check_stop_history(
                    native,
                    workbench.STATE_ROOT / "execution-state/operation-journal.sqlite3",
                    arm_terminal,
                )
                assert arm_terminal["state"] == "succeeded", arm_terminal
                assert arm_terminal["failed_count"] == 0, arm_terminal
                assert arm_terminal["acceptance_to_terminal_latency_ns"] <= (
                    arm_terminal["acceptance"]["fixed_outer_duration_ns"]
                ), arm_terminal
                if args.stop_jerk is not None or args.stop_ramp_duration is not None:
                    profiles = [
                        stage["profile"]
                        for stage in arm_terminal["attempted_stages"][:1]
                        if stage["profile"] is not None
                    ]
                    assert len(profiles) == 1, arm_terminal
                    if args.stop_jerk is not None:
                        assert profiles[0]["jerk_limits_enabled"] is (
                            args.stop_jerk == "on"
                        ), profiles
                    if args.stop_ramp_duration is not None:
                        assert profiles[0]["deceleration_time_s"] == (
                            args.stop_ramp_duration
                        ), profiles
                assert arm_terminal["participant_settlement"], arm_terminal
                assert all(
                    row["facts"]["terminal_settlement"] == "settled"
                    for row in arm_terminal["participant_settlement"]
                ), arm_terminal
                if args.expect_stop_refusal:
                    assert arm_terminal["result"] == "succeeded_escalated", arm_terminal
                    assert arm_terminal["escalated"], arm_terminal
                    assert arm_terminal["hard_stop_floor_used"], arm_terminal
                    stages = arm_terminal["attempted_stages"]
                    assert len(stages) == 2, stages
                    refused, hard = stages
                    assert refused["outcome"] == "failed", refused
                    assert refused["attempt_disposition"] == "not_dispatched", refused
                    assert refused["error"]["details"]["reason"] == (
                        "soft_on_path_deceleration_time_infeasible"
                    ), refused
                    assert hard["stage"]["action_class"] == "software_hard", hard
                    assert hard["outcome"] == "succeeded", hard
                    assert hard["attempt_disposition"] == "dispatched", hard

                    def faulted_and_settled():
                        value = scope()
                        return (
                            value
                            if (
                                sorted(
                                    value["held_node_indices"]
                                    + value["faulted_node_indices"]
                                )
                                == [0, 1, 2]
                                and not value["pending_effects"]
                                and not value["stopping_node_indices"]
                            )
                            else None
                        )

                    final_scope = workbench._wait_until(
                        "failed profile containment settles with fault retained",
                        faulted_and_settled,
                        stack.processes,
                    )
                    stop_fault = native.stop_fault()["stop_fault"]
                    assert stop_fault is not None, arm_terminal
                    assert stop_fault["latch"]["state"] == "latched", stop_fault
                    assert (
                        stop_fault["latch"]["latch_id"]
                        == arm_terminal["fault_latch"]["latch_id"]
                    ), stop_fault
                    controls = client.program_runtime_controls(run_id)
                    groups = {
                        row["group_id"]: row
                        for row in controls["execution_selections"]["groups"]
                    }
                    assert not groups["arm"]["resume_allowed"], groups
                    assert not groups["all"]["resume_allowed"], groups
                    print(
                        json.dumps(
                            {
                                "passed": True,
                                "qualification": "selected_stop_profile_refusal",
                                "private_state": str(state),
                                "arm_stop_terminal": arm_terminal,
                                "stop_fault": stop_fault,
                                "final_scope": final_scope,
                                "runtime_controls": controls,
                                "unselected_gripper_before": tool_before,
                                "unselected_gripper_after": tool_after,
                            },
                            sort_keys=True,
                        )
                    )
                    return
                assert not arm_terminal["escalated"], arm_terminal
                assert not arm_terminal["hard_stop_floor_used"], arm_terminal
                final_scope = workbench._wait_until(
                    "selected native effects settle", settled, stack.processes
                )
                # Reset can legitimately recover a containment fault. Prove
                # nominal Stop separately before that recovery can clear it.
                stop_fault = native.stop_fault()["stop_fault"]
                assert stop_fault is None, stop_fault
                rest_evidence = [
                    item["evidence"]
                    for stage in arm_terminal["attempted_stages"]
                    for item in stage["evidence_items"]
                    if item["required_class"] == "motion_rest"
                    and item["state"] == "satisfied"
                ]
                assert len(rest_evidence) == 1, arm_terminal
                if (
                    args.stop_ramp_duration is not None
                    and not rest_evidence[0]["completed_by_natural_completion"]
                ):
                    assert math.isclose(
                        float(
                            rest_evidence[0]["provider_diagnostics"]["effective_ramp_s"]
                        ),
                        args.stop_ramp_duration,
                        rel_tol=1e-9,
                    ), rest_evidence
                if args.expect_natural_completion is not None:
                    assert rest_evidence[0]["completed_by_natural_completion"] is (
                        args.expect_natural_completion == "on"
                    ), arm_terminal
                if args.stop_mode:
                    assert (
                        rest_evidence[0]["requested_stop_mode"] == args.stop_mode
                    ), arm_terminal
                if args.scale_stop:
                    handoff = rest_evidence[0]["provider_diagnostics"]
                    assert (
                        float(handoff["runtime_scale_rate_per_s_at_trip"]) < 0
                    ), handoff
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
                    # Exercise the browser-facing read route, not just Headless:
                    # a missing Composer forwarder must fail this practical row.
                    with HeadlessHttpClient(
                        "http://127.0.0.1:9001", timeout_seconds=3
                    ) as composer:
                        proof = composer.read_manual_handover(manual)
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
                    with HeadlessHttpClient(
                        "http://127.0.0.1:9001", timeout_seconds=3
                    ) as composer:
                        proof = composer.read_manual_handover(manual)
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
                        # Both motion variants and the gripper declare a
                        # 30-second execution/completion budget. Observe that
                        # budget plus owner publication, including when the
                        # selected gripper duration itself is 20 seconds.
                        # Product deadlines and Stop budgets are unchanged.
                        timeout_seconds=35,
                    )
                    assert recovery_result["node_statuses"] == {
                        "control_root": "success",
                        "controlled_motion": "success",
                        "moving_gripper": "success",
                    }, recovery_result
                    with HeadlessHttpClient(
                        "http://127.0.0.1:9001", timeout_seconds=3
                    ) as composer:
                        recovery_result["qualified_completed_operations"] = (
                            check_completed_program_operations(
                                native,
                                composer,
                                workbench.STATE_ROOT
                                / "execution-state/operation-journal.sqlite3",
                                run_id,
                            )
                        )
                    gripper_completed = next(
                        row
                        for row in recovery_result["qualified_completed_operations"]
                        if row["scopes"][0]["node_instance_id"] == "moving_gripper"
                    )
                    gripper_payload = gripper_completed["accepted_payloads"][0]
                    assert gripper_payload["kind"] == "capability_command"
                    assert (
                        gripper_payload["command_schema_id"]
                        == "gripper.command_position"
                    )
                    gripper_goal = gripper_payload["command_value"]["position"]
                    assert gripper_goal["kind"] == "linear", gripper_goal
                    gripper_observation = native.query_target(
                        TargetObservationQuery.model_validate(
                            {
                                **common,
                                "target_role_id": "gripper",
                                "kind": "gripper_status",
                            }
                        )
                    )["target_observation"]
                    gripper_position = gripper_observation["value"]["observed_position"]
                    assert gripper_position["kind"] == "linear_m", gripper_observation
                    assert math.isclose(
                        gripper_position["value_m"],
                        gripper_goal["value"],
                        rel_tol=1e-6,
                        abs_tol=1e-12,
                    ), (gripper_goal, gripper_observation)
                    recovery_result["qualified_final_gripper_observation"] = (
                        gripper_observation
                    )
                    if args.joint_subset:
                        arm_completed = next(
                            row
                            for row in recovery_result["qualified_completed_operations"]
                            if row["scopes"][0]["node_instance_id"]
                            == "controlled_motion"
                        )
                        assert arm_completed["accepted_payloads"][0]["motion"] == {
                            "kind": "joint_goal",
                            "joint_names": ["joint1"],
                            "positions_rad": [0.6],
                        }, arm_completed
                    assert native.read(arm["operation_id"])["resident"] is False
                    assert native.read(tool["operation_id"])["resident"] is False
                    if manual_handover_result is not None:
                        # Manual handover adds enough real operation events
                        # for the legacy full-page/continuation checks.
                        page = native.events(identity["owner_epoch"], 0, limit=16)
                        assert (
                            len(page["events"]) == 16 and page["has_more"] is True
                        ), page
                        assert (
                            page["next_after_sequence"]
                            == page["events"][-1]["sequence"]
                        ), page
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
                            "arm_stop_terminal": arm_terminal,
                            "scale_transitions": scale_transitions,
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
            last_program = None
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
                nonlocal queried_operation, observed_operation_scale, resume_actor, last_program
                current = client.program_run(run_id)
                last_program = current
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

            try:
                result = workbench._wait_until(
                    (
                        "controls Program terminal"
                        if args.controls
                        else "query-only Program terminal"
                    ),
                    terminal,
                    stack.processes,
                )
            except workbench.WorkbenchError:
                # Preserve the last owner result before private stack teardown.
                # This observes the failed run; it never retries its commands.
                print(
                    json.dumps(
                        {
                            "failed_program": last_program,
                            "queried_operation": queried_operation,
                            "operation": (
                                native.read(queried_operation)
                                if queried_operation
                                else None
                            ),
                        },
                        sort_keys=True,
                    ),
                    flush=True,
                )
                raise
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
