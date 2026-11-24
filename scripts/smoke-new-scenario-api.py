#!/usr/bin/env python3
"""Build a Scenario from scratch over the API, against a running workbench.

This is the worked example of docs/authoring/NEW_SCENARIO.md over the API.
It does the GUI steps in the same order, with the same routes the GUI uses:

  1. New                 POST /api/scenario/new
  2. Add components      POST /api/deployment/draft/components
                         (rows from GET /api/deployment/component-catalog:
                         a Lite 6 machine and a 64x64 I/O bank, both Mock)
  3. Add a Program root  POST /api/program/draft/mutations
  4. Preview             POST /api/deployment/preview
  5. Apply               POST /api/deployment/apply
  6. Save as             POST /api/scenario-documents/save
  7. Load it back        POST /api/scenario/load
  8. Check components    GET /api/components (every one Prepared)

New, Apply and Load answer at once with a command row; the script then
reads that row (found by its request_key) in GET /api/deployment/workspace
until it ends.

New discards the cell's current Scenario and Drafts, so the script needs
--confirm-replace-cell. Save writes into a new folder of its own,
examples/.smoke-new-scenario-<id>/ (gitignored). After a passing run the
script runs New again and deletes that folder. After a failing run it keeps
both and prints the folder path.

Run it on the host of the development workbench, from the checkout the
workbench was started from (./scripts/compose-dev.sh start). It needs no PLC
and no simulator: every component runs on the Mock lane.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import sys
import time
from typing import Any
import urllib.error
import urllib.request
import uuid


REPOSITORY = Path(__file__).resolve().parents[1]

MACHINE_MODEL = "ufactory.lite6_machine"
IO_BANK_MODEL = "manyforge.io_bank_64x64"
LANE = "mock"
SCENARIO_NAME = "smoke_new_scenario_api"
SAVE_FOLDER_PREFIX = ".smoke-new-scenario-"

# Test limits (not product limits). They only stop this test from waiting
# forever. They are generous so that a slow or busy machine does not trip
# them. When one expires, the test fails and leaves the cell as it is.
TEST_LIMIT_WAIT_SECONDS = 900.0  # one command row, or components to be Prepared
TEST_LIMIT_HTTP_SECONDS = 120.0  # one HTTP request
POLL_INTERVAL_SECONDS = 1.0


class StepFailed(Exception):
    """A step did not give the expected result. The message names the step."""


# --------------------------------------------------------------------------
# HTTP


class Workbench:
    """The two APIs of the running workbench: Composer and Headless."""

    def __init__(self, composer_url: str, headless_url: str) -> None:
        self.composer_url = composer_url.rstrip("/")
        self.headless_url = headless_url.rstrip("/")


def send(base_url: str, method: str, path: str, body: Any) -> tuple[int, Any]:
    """Send one request; return (HTTP status, decoded JSON body)."""

    url = base_url + path
    data = None
    if body is not None:
        data = json.dumps(body).encode()
    request = urllib.request.Request(
        url, method=method, data=data, headers={"content-type": "application/json"}
    )
    try:
        with urllib.request.urlopen(request, timeout=TEST_LIMIT_HTTP_SECONDS) as reply:
            return reply.status, decode(reply.read())
    except urllib.error.HTTPError as error:
        return error.code, decode(error.read())
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        reason = getattr(error, "reason", error)
        raise StepFailed(f"{method} {url} got no answer: {reason}") from error


def decode(raw: bytes) -> Any:
    """The body as JSON, or as text when it is not JSON."""

    if not raw:
        return None
    try:
        return json.loads(raw)
    except json.JSONDecodeError:
        return raw.decode(errors="replace")


def error_code(body: Any) -> str:
    """The error code of a refused request (FastAPI puts it under "detail")."""

    if isinstance(body, dict):
        detail = body.get("detail")
        if isinstance(detail, dict) and detail.get("code"):
            return str(detail["code"])
        if body.get("code"):
            return str(body["code"])
    return "no code"


def shorten(value: Any, limit: int = 1500) -> str:
    """The value as text, cut to ``limit`` characters."""

    if isinstance(value, str):
        text = value
    else:
        text = json.dumps(value, sort_keys=True)
    if len(text) > limit:
        return text[:limit] + " ..."
    return text


def call(step: str, base_url: str, method: str, path: str, body: Any = None) -> Any:
    """Send one request that must answer HTTP 200; return its JSON body."""

    status, answer = send(base_url, method, path, body)
    if status != 200:
        raise StepFailed(
            f"{step}: {method} {path} answered HTTP {status}, "
            f"code {error_code(answer)}: {shorten(answer)}"
        )
    return answer


def composer(step: str, workbench: Workbench, method: str, path: str, body: Any = None) -> Any:
    """A Composer request (workbench port 9020) that must answer HTTP 200."""

    return call(step, workbench.composer_url, method, path, body)


def headless(step: str, workbench: Workbench, method: str, path: str) -> Any:
    """A Headless API request (port 9010) that must answer HTTP 200."""

    return call(step, workbench.headless_url, method, path)


def new_request_key() -> str:
    """A fresh request key: one per attempt, so a retry is never mistaken for it."""

    return uuid.uuid4().hex


def passed(step: str, detail: str) -> None:
    """Print one PASS line."""

    print(f"PASS {step}: {detail}", flush=True)


# --------------------------------------------------------------------------
# Fences: every Draft edit and Save carries the current revisions it expects.


def read_fence(step: str, workbench: Workbench) -> tuple[dict[str, Any], dict[str, Any]]:
    """The common fence fields and the authoringWorkspace they came from (admission, runtime, Program workspace)."""

    admission = headless(step, workbench, "GET", "/api/cell/authoring-admission")
    runtime = composer(step, workbench, "GET", "/api/runtime")
    program = composer(step, workbench, "GET", "/api/program/workspace")
    workspace = program["authoringWorkspace"]
    fence = {
        "expectedAdmissionGeneration": admission["admission_generation"],
        "applicationStartId": runtime["application_start_id"],
        "requestKey": new_request_key(),
        "expectedWorkspaceEpoch": workspace["workspaceEpoch"],
        "expectedWorkspaceRevision": workspace["workspaceRevision"],
    }
    return fence, workspace


def deployment_fence(step: str, workbench: Workbench) -> dict[str, Any]:
    """Fence of a Deployment Draft edit (authoringWorkspace.deploymentDraftId/Revision)."""

    fence, workspace = read_fence(step, workbench)
    fence["draftId"] = workspace["deploymentDraftId"]
    fence["expectedDraftRevision"] = workspace["deploymentDraftRevision"]
    return fence


def program_fence(step: str, workbench: Workbench) -> dict[str, Any]:
    """Fence of a Program Draft edit (authoringWorkspace.programDraftId/Revision)."""

    fence, workspace = read_fence(step, workbench)
    fence["draftId"] = workspace["programDraftId"]
    fence["expectedDraftRevision"] = workspace["programDraftRevision"]
    return fence


def scenario_fence(step: str, workbench: Workbench) -> dict[str, Any]:
    """Fence of a Save: both Drafts (program and deployment ids and revisions)."""

    fence, workspace = read_fence(step, workbench)
    fence["draftId"] = workspace["programDraftId"]
    fence["expectedDraftRevision"] = workspace["programDraftRevision"]
    fence["deploymentDraftId"] = workspace["deploymentDraftId"]
    fence["expectedDeploymentDraftRevision"] = workspace["deploymentDraftRevision"]
    return fence


# --------------------------------------------------------------------------
# Reading results


def find_command_row(step: str, workbench: Workbench, request_key: str) -> dict[str, Any] | None:
    """The ended command row with this request_key in apply_commands, or None."""

    deployment = composer(step, workbench, "GET", "/api/deployment/workspace")
    rows = deployment["apply_commands"]
    candidates = list(rows.get("ended") or [])
    if rows.get("in_progress"):
        candidates.append(rows["in_progress"])
    for row in candidates:
        if row.get("request_key") == request_key and row.get("end"):
            return row
    return None


def wait_for_command_row(step: str, workbench: Workbench, request_key: str, limit: float) -> None:
    """Wait until the command row ends; fail unless row.end.outcome is "succeeded"."""

    deadline = time.monotonic() + limit
    row = find_command_row(step, workbench, request_key)
    while row is None:
        if time.monotonic() > deadline:
            raise StepFailed(f"{step}: command row did not end within the test limit of {limit:.0f} s")
        time.sleep(POLL_INTERVAL_SECONDS)
        row = find_command_row(step, workbench, request_key)
    end = row["end"]
    if end.get("outcome") != "succeeded":
        raise StepFailed(
            f"{step}: command row ended {end.get('outcome')}, code {end.get('code')}: {shorten(end)}"
        )


def draft_component_ids(step: str, workbench: Workbench) -> list[str]:
    """The component ids in the Deployment Draft (draft.document.components)."""

    deployment = composer(step, workbench, "GET", "/api/deployment/workspace")
    document = deployment["draft"]["document"] or {}
    return sorted(row["component_id"] for row in document.get("components", []))


# --------------------------------------------------------------------------
# The steps, in GUI order


def step_1_new(workbench: Workbench, limit: float) -> None:
    """New: empty the cell; wait for the command row and check the Draft has no components."""

    step = "1 New"
    request_key = new_request_key()
    body = {"discard_unsaved": True, "request_key": request_key}
    composer(step, workbench, "POST", "/api/scenario/new", body)
    wait_for_command_row(step, workbench, request_key, limit)
    left = draft_component_ids(step, workbench)
    if left:
        raise StepFailed(f"{step}: the Draft still has components {left}")
    passed(step, "cell is empty")


def machine_rows(step: str, entries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Rows of the Lite 6 machine entry, as the Add panel builds them: owner_row with the lane, each member's row with owner and role."""

    machine = None
    for entry in entries:
        if entry.get("kind") == "machine" and entry.get("model_id") == MACHINE_MODEL:
            machine = entry
    if machine is None:
        raise StepFailed(f"{step}: the catalog has no machine entry for {MACHINE_MODEL}")
    if LANE not in machine["lanes"]:
        raise StepFailed(f"{step}: {MACHINE_MODEL} offers lanes {machine['lanes']}, not {LANE}")
    owner_id = machine["default_owner_id"]
    owner_row = dict(machine["owner_row"])
    owner_row["lane"] = LANE
    rows = [owner_row]
    for member in machine["members"]:
        member_row = dict(member["row"])
        member_row["owner"] = owner_id
        member_row["role"] = member["role"]
        rows.append(member_row)
    return rows


def io_bank_rows(step: str, entries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Rows of the I/O bank's model entry (entryId "model/<model>", its "components")."""

    for entry in entries:
        if entry.get("entryId") == f"model/{IO_BANK_MODEL}" and entry.get("lane") == LANE:
            return [dict(row) for row in entry["components"]]
    raise StepFailed(f"{step}: the catalog has no {LANE} entry for {IO_BANK_MODEL}")


def step_2_add_components(workbench: Workbench) -> list[str]:
    """Add components: rows from the catalog's "entries", added to the Draft; returns their ids."""

    step = "2 Add components"
    catalog = composer(step, workbench, "GET", "/api/deployment/component-catalog")
    rows = machine_rows(step, catalog["entries"]) + io_bank_rows(step, catalog["entries"])
    expected = sorted(row["component_id"] for row in rows)
    body = deployment_fence(step, workbench)
    body["components"] = rows
    body["attachments"] = []
    composer(step, workbench, "POST", "/api/deployment/draft/components", body)
    found = draft_component_ids(step, workbench)
    if found != expected:
        raise StepFailed(f"{step}: the Draft has {found}, expected {expected}")
    passed(step, ", ".join(expected))
    return expected


def step_3_add_root(workbench: Workbench) -> None:
    """Add a Program root (a 1 s timer); check applicability.applyReady is true."""

    step = "3 Add Program root"
    body = program_fence(step, workbench)
    body["mutation"] = "tree_draft_insert_node"
    body["arguments"] = {
        "node": {
            "node_id": "timer",
            "instance_id": "wait",
            "parameters": {"duration_s": 1},
            "children": [],
        }
    }
    composer(step, workbench, "POST", "/api/program/draft/mutations", body)
    program = composer(step, workbench, "GET", "/api/program/workspace")
    applicability = program["applicability"]
    if applicability.get("applyReady") is not True:
        raise StepFailed(f"{step}: Apply is not ready: {shorten(applicability)}")
    passed(step, "timer root; Apply is ready")


def step_4_preview(workbench: Workbench) -> Any:
    """Preview: what Apply would change; returns the Preview, whose "scope" and
    collision rule changes the Apply sends back as confirmed."""

    step = "4 Preview"
    preview = composer(step, workbench, "POST", "/api/deployment/preview", {})
    passed(step, f"scope {shorten(preview.get('scope'), 120)}")
    return preview


def step_5_apply(workbench: Workbench, preview: Any, expected: list[str], limit: float) -> None:
    """Apply the Drafts with the Preview's scope and its collision rule changes confirmed; wait for the command row, then for every component Prepared."""

    step = "5 Apply"
    request_key = new_request_key()
    body = {
        "scope": preview.get("scope"),
        "confirmed_collision_rule_ids": [
            row["rule_id"] for row in preview.get("collision_rules") or ()
        ],
        "request_key": request_key,
    }
    composer(step, workbench, "POST", "/api/deployment/apply", body)
    wait_for_command_row(step, workbench, request_key, limit)
    passed(step, "command row succeeded")
    wait_until_prepared(workbench, step, expected, limit)


def step_6_save(workbench: Workbench, directory: str) -> str:
    """Save as <directory>/<name>.scenario; check status "saved" and last_known_status "ready"; returns "path"."""

    step = "6 Save as"
    body = scenario_fence(step, workbench)
    body["directory"] = directory
    body["name"] = SCENARIO_NAME
    body["description"] = "Built over the API by scripts/smoke-new-scenario-api.py"
    body["confirmOverwrite"] = False
    saved = composer(step, workbench, "POST", "/api/scenario-documents/save", body)
    if saved.get("status") != "saved":
        raise StepFailed(f"{step}: status is {saved.get('status')}, expected saved")
    # "ready": the saved files are exactly what is Applied.
    if saved.get("last_known_status") != "ready":
        raise StepFailed(f"{step}: last_known_status is {saved.get('last_known_status')}, expected ready")
    passed(step, saved["path"])
    return saved["path"]


def step_7_load(workbench: Workbench, path: str, expected: list[str], limit: float) -> None:
    """Load the saved project back; wait for the command row; check the Draft's component ids."""

    step = "7 Load back"
    request_key = new_request_key()
    # discard_unsaved is false: after Save nothing is unsaved, so Load must
    # be accepted without it.
    body = {"path": path, "discard_unsaved": False, "request_key": request_key}
    composer(step, workbench, "POST", "/api/scenario/load", body)
    wait_for_command_row(step, workbench, request_key, limit)
    found = draft_component_ids(step, workbench)
    if found != expected:
        raise StepFailed(f"{step}: the Draft has {found}, expected {expected}")
    passed(step, "command row succeeded; same components")


def component_lifecycles(step: str, workbench: Workbench) -> dict[str, str]:
    """Each component's "lifecycle" from GET /api/components, by component_id."""

    states = composer(step, workbench, "GET", "/api/components")
    lifecycles = {}
    for state in states:
        lifecycles[state["component_id"]] = state.get("lifecycle")
    return lifecycles


def wait_until_prepared(workbench: Workbench, step: str, expected: list[str], limit: float) -> None:
    """Wait until every expected component has lifecycle "prepared"; "fault" fails."""

    deadline = time.monotonic() + limit
    while True:
        lifecycles = component_lifecycles(step, workbench)
        missing = [c for c in expected if c not in lifecycles]
        if missing:
            raise StepFailed(f"{step}: components missing from the cell: {missing}")
        faulted = [c for c in expected if lifecycles[c] == "fault"]
        if faulted:
            raise StepFailed(f"{step}: components in fault: {faulted}")
        not_prepared = [c for c in expected if lifecycles[c] != "prepared"]
        if not not_prepared:
            passed(step, f"all {len(expected)} components Prepared")
            return
        if time.monotonic() > deadline:
            states = {c: lifecycles[c] for c in not_prepared}
            raise StepFailed(f"{step}: not Prepared within the test limit of {limit:.0f} s: {states}")
        time.sleep(POLL_INTERVAL_SECONDS)


def step_8_check_components(workbench: Workbench, expected: list[str], limit: float) -> None:
    """Check components after Load: every one Prepared (lifecycle in GET /api/components)."""

    wait_until_prepared(workbench, "8 Check components", expected, limit)


def clean_up(workbench: Workbench, host_folder: Path, limit: float) -> None:
    """After a pass: New again (the loaded project is about to go), then delete this run's folder."""

    step = "9 Clean up"
    request_key = new_request_key()
    body = {"discard_unsaved": False, "request_key": request_key}
    composer(step, workbench, "POST", "/api/scenario/new", body)
    wait_for_command_row(step, workbench, request_key, limit)
    shutil.rmtree(host_folder)
    passed(step, f"cell emptied; removed {host_folder.relative_to(REPOSITORY)}")


def check_workbench_answers(workbench: Workbench) -> None:
    """Before the walk: GET /api/runtime answers, otherwise say how to start the workbench."""

    step = "0 Workbench"
    try:
        status, _answer = send(workbench.composer_url, "GET", "/api/runtime", None)
    except StepFailed as error:
        raise StepFailed(
            f"{step}: no development workbench answers ({error}). "
            "Start it with ./scripts/compose-dev.sh start"
        ) from error
    if status != 200:
        raise StepFailed(f"{step}: GET /api/runtime answered HTTP {status}")


# --------------------------------------------------------------------------


def parse_arguments(argv: list[str] | None) -> argparse.Namespace:
    """The command-line options."""

    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--confirm-replace-cell",
        action="store_true",
        help="required: New may discard the cell's current Scenario and Drafts",
    )
    parser.add_argument(
        "--composer-url",
        default=os.environ.get("MANYFORGE_COMPOSER_URL", "http://127.0.0.1:9020"),
        help="Composer API (default: %(default)s)",
    )
    parser.add_argument(
        "--headless-url",
        default=os.environ.get("MANYFORGE_HEADLESS_API_URL", "http://127.0.0.1:9010"),
        help="Headless API, for the authoring admission (default: %(default)s)",
    )
    parser.add_argument(
        "--workbench-examples",
        default="/workspace/examples",
        help="the repository's examples/ folder as the workbench sees it (default: %(default)s)",
    )
    parser.add_argument(
        "--test-limit-seconds",
        type=float,
        default=TEST_LIMIT_WAIT_SECONDS,
        help="test limit for one command row or the Prepared wait (default: %(default)s)",
    )
    parser.add_argument(
        "--keep-saved",
        action="store_true",
        help="after a pass, keep the saved project folder and the loaded Scenario",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(argv)
    if not arguments.confirm_replace_cell:
        print("Refusing to replace the cell without --confirm-replace-cell.", file=sys.stderr)
        return 2
    workbench = Workbench(arguments.composer_url, arguments.headless_url)
    limit = arguments.test_limit_seconds

    # Save writes only under the workbench's write roots (examples/ and the
    # site's project folders). This run's own folder under examples/ keeps it
    # away from the example Scenarios; the folder must exist before Save.
    folder_name = SAVE_FOLDER_PREFIX + uuid.uuid4().hex[:12]
    host_folder = REPOSITORY / "examples" / folder_name
    workbench_folder = arguments.workbench_examples.rstrip("/") + "/" + folder_name

    try:
        check_workbench_answers(workbench)
        step_1_new(workbench, limit)
        component_ids = step_2_add_components(workbench)
        step_3_add_root(workbench)
        preview = step_4_preview(workbench)
        step_5_apply(workbench, preview, component_ids, limit)
        host_folder.mkdir()
        saved_path = step_6_save(workbench, workbench_folder)
        step_7_load(workbench, saved_path, component_ids, limit)
        step_8_check_components(workbench, component_ids, limit)
        if not arguments.keep_saved:
            clean_up(workbench, host_folder, limit)
    except StepFailed as failure:
        print(f"FAIL {failure}", file=sys.stderr)
        if host_folder.exists():
            print(f"The saved project is kept for inspection: {host_folder}", file=sys.stderr)
        return 1
    print("PASS: Scenario built from scratch over the API")
    return 0


if __name__ == "__main__":
    sys.exit(main())
