#!/usr/bin/env python3
"""Scenario Load soak (New always works, Load = New + Apply).

Drives a running development workbench through the Composer REST API the
GUI uses and repeats each Load case per example Scenario:

  a           New -> Load -> Prepare all -> Enable all -> readiness can_execute
  b           Load of a different Scenario on a ready cell, without New
  c_finished  Load right after a finished run
  c_stopped   Load right after an operator Stop (groups held)
  c_reset     Load right after a Program Reset
  d           Load immediately after the previous Load returned
              (its controllers may still be starting)
  e           Load after a failed Apply (provoked by an unbuildable
              Deployment edit, see ``provoke_failed_apply``)
  f           Load after a component went stale: the arm's ros2_control
              process is SIGSTOPped until the component reports its fault,
              then a Scenario reusing the same component id is Loaded and
              must start clean (needs --container; host signals via
              ``docker top``)

Every attempt ends with Prepare all -> Enable all -> readiness can_execute;
any step failing fails the attempt. A failed attempt saves the container log
of its time window (bounded per file and in total) and the cell is
recovered with New before the next attempt.

Output: <out>/attempts.jsonl (one row per attempt), <out>/summary.json,
<out>/summary.txt, <out>/logs/*.log. Stdlib only.

Example:
  python3 scripts/soak/load_soak.py --base-url http://127.0.0.1:3000 \
      --container mf-load-soak --repeat 20 --out /tmp/load-soak
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time
import traceback
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

ALL_CASES = ("a", "b", "c_finished", "c_stopped", "c_reset", "d", "e", "f")
# Need a model endpoint or a connected browser viewer; not runnable headless.
NOT_RUNNABLE = {
    "mock_ur5_semantic_object_pick",
    "mock_ur5_vision_bin_pick",
    "mock_ur5_vision_pose_pick",
}
ACTIVE_RUN = {"starting", "running", "intervention_required", "stopping", "full_stopping"}

BASE = "http://127.0.0.1:3000"
HEADLESS = "http://127.0.0.1:9010"
EPOCH = "load-soak-" + uuid.uuid4().hex[:12]


class StepFailed(Exception):
    def __init__(self, step: str, info: dict[str, Any]):
        super().__init__(step)
        self.step = step
        self.info = info


# ---------------------------------------------------------------- HTTP

def call(method: str, path: str, body: Any = None, timeout: float = 60) -> tuple[int, Any]:
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(BASE + path, data=data, method=method, headers={
        "Content-Type": "application/json", "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            st, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        st, raw = e.code, e.read()
    except Exception as e:  # transport: reported as status -1
        return -1, {"transport_error": repr(e)}
    try:
        return st, json.loads(raw)
    except ValueError:
        return st, {"raw": raw[:2000].decode(errors="replace")}


def get(path: str, **k: Any) -> tuple[int, Any]:
    return call("GET", path, **k)


def post(path: str, body: Any = None, **k: Any) -> tuple[int, Any]:
    return call("POST", path, {} if body is None else body, **k)


def rid(prefix: str) -> str:
    return f"{prefix}-{uuid.uuid4().hex[:16]}"


def err(js: Any) -> Any:
    """The typed error of a response: {code, message} where present."""
    if isinstance(js, dict) and isinstance(js.get("error"), dict):
        return js["error"]
    d = js.get("detail", js) if isinstance(js, dict) else js
    if isinstance(d, dict):
        return {k: d.get(k) for k in ("code", "message") if k in d} or d
    return d


def attribution(prefix: str) -> dict[str, Any]:
    r = rid(prefix)
    return {"request_id": r, "request_owner_epoch": EPOCH, "source_label": "operator_ui",
            "correlation_id": r}


# ---------------------------------------------------------------- cell steps

def components() -> list[dict[str, Any]]:
    st, c = get("/api/components")
    return c if st == 200 and isinstance(c, list) else []


def snapshot() -> dict[str, Any]:
    out: dict[str, Any] = {}
    out["components"] = [
        {k: x.get(k) for k in ("component_id", "lifecycle", "activity", "last_action", "fault", "action")}
        for x in components()]
    out["readiness"] = get("/api/program/readiness")[1]
    out["latch"] = get("/api/cell/latch")[1]
    st, cy = get("/api/cycle/status")
    out["cycle"] = {k: cy.get(k) for k in ("state", "program_run_id", "failure")} if isinstance(cy, dict) else cy
    return out


def load_outcome(step: str, st: int, js: Any) -> None:
    if st == 200 and isinstance(js, dict) and js.get("outcome") == "succeeded":
        return
    failed = []
    if isinstance(js, dict):
        failed = [{k: c.get(k) for k in ("component_id", "outcome", "error", "disposition")}
                  for c in js.get("components", []) if c.get("outcome") not in (None, "succeeded")]
    raise StepFailed(step, {"http": st, "outcome": js.get("outcome") if isinstance(js, dict) else None,
                            "error": err(js), "failed_components": failed,
                            "body": json.dumps(js, default=str)[:3000]})


def do_new(step: str = "new") -> None:
    with teardown():
        st, js = post("/api/scenario/new", {"discard_unsaved": True}, timeout=300)
    LAST["new"] = time.time()
    load_outcome(step, st, js)


def do_load(scenario: str, step: str = "load") -> None:
    with teardown():
        st, js = post("/api/scenario/load", {
            "path": f"/workspace/examples/{scenario}.scenario", "discard_unsaved": True}, timeout=400)
    LAST["load"] = time.time()
    load_outcome(step, st, js)


def group_action(action: str, targets: list[str], want: tuple[str, ...], timeout: float) -> float:
    st, js = post("/api/components/actions", {"action": action, "component_ids": targets})
    if st != 202:
        raise StepFailed(action, {"http": st, "error": err(js), "targets": targets})
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(0.5)
        c = {x["component_id"]: x for x in components()}
        bad = [{"component_id": i, "last_action": c[i].get("last_action"), "fault": c[i].get("fault")}
               for i in targets if i in c
               and (c[i].get("last_action") or {}).get("action") == action
               and (c[i].get("last_action") or {}).get("outcome") in ("failed", "unknown")]
        if bad:
            raise StepFailed(action, {"failed": bad})
        if all(c.get(i, {}).get("lifecycle") in want for i in targets):
            return round(time.time() - t0, 2)
    raise StepFailed(action, {"timeout_s": timeout,
                              "lifecycles": {i: c.get(i, {}).get("lifecycle") for i in targets}})


def bring_up(timings: dict[str, float], timeout: float) -> None:
    """Prepare all -> Enable all -> readiness can_execute."""
    c = components()
    targets = [x["component_id"] for x in c if x["lifecycle"] in ("not_prepared", "offline")]
    timings["prepare_s"] = group_action("prepare", targets, ("prepared", "enabled"), timeout) if targets else 0.0
    c = components()
    owners = {x.get("owner_id") for x in c if x.get("owner_id")}
    targets = [x["component_id"] for x in c if x["lifecycle"] == "prepared" and x["component_id"] not in owners]
    timings["enable_s"] = group_action("enable", targets, ("enabled",), timeout) if targets else 0.0
    LAST["enable"] = time.time()
    t0 = time.time()
    rd: Any = None
    while time.time() - t0 < 20:
        st, rd = get("/api/program/readiness")
        if st == 200 and rd.get("can_execute"):
            timings["readiness_wait_s"] = round(time.time() - t0, 2)
            return
        time.sleep(0.5)
    raise StepFailed("readiness", {"readiness": rd})


# ---------------------------------------------------------------- runs

def start_run() -> str:
    st, ap = get("/api/program/applied")
    st2, ws = get("/api/program/workspace")
    if st != 200 or st2 != 200:
        raise StepFailed("execute", {"applied": (st, err(ap)), "workspace": (st2, err(ws))})
    body = {**attribution("soak-run"), "scenario_id": ap["scenario_id"],
            "source_document_digest": ap["source_document_digest"],
            "expected_applied_record_digest": ap["source"]["record_digest"],
            "draft_program_revision": ws["draft"]["programRevision"]}
    st, js = post("/api/program/runs", body, timeout=120)
    LAST["execute"] = time.time()
    if st != 200:
        raise StepFailed("execute", {"http": st, "error": err(js)})
    return js["capture"]["program_run_id"]


# When the last Load/New returned, Enable finished, Reset finished, Execute
# was accepted (timing context of a refused Run).
LAST: dict[str, float] = {}


def controls(rn: str) -> dict[str, Any] | None:
    st, cc = get(f"/api/program/runs/{rn}/runtime-controls")
    return cc if st == 200 else None


def motion_start(rn: str, cc: dict[str, Any], gids: list[str]) -> tuple[int, Any]:
    return post(f"/api/program/runs/{rn}/motion-start", {
        **attribution("soak-start"), "program_run_id": rn, "expected_owner_epoch": cc["owner_epoch"],
        "group_ids": gids,
        "expected_selection_revision": cc["execution_selections"]["selection_revision"],
        "reason": "operator start-motion"})


def drive_run(rn: str, scenario: str, timeout: float,
              until: Callable[[dict[str, Any], dict[str, Any]], bool] | None = None) -> dict[str, Any]:
    """Start every ready group and answer the example's operator prompts.
    Returns the run when it is terminal, or when ``until(run, controls)``."""
    hook = HOOKS.get(scenario)
    started: set[str] = set()
    acted: set[Any] = set()
    t0 = time.time()
    r: dict[str, Any] = {}
    while time.time() - t0 < timeout:
        st, r = get(f"/api/program/runs/{rn}")
        if r.get("state") not in ACTIVE_RUN:
            return r
        cc = controls(rn)
        if cc is not None:
            if until is not None and until(r, cc):
                return r
            gs = [g["group_id"] for g in cc["execution_selections"]["groups"]
                  if g.get("start_allowed") and g.get("state") == "ready" and g["group_id"] not in started]
            if gs:
                sst, sjs = motion_start(rn, cc, gs)
                if sst == 200:
                    started.update(gs)
                elif (err(sjs) or {}).get("code") != "program_runtime_control_stale":
                    now = time.time()
                    raise StepFailed("motion_start", {
                        "http": sst, "error": err(sjs), "detail": sjs.get("detail") if isinstance(sjs, dict) else sjs,
                        "since_s": {k: round(now - v, 3) for k, v in LAST.items()}})
            if hook is not None:
                hook(rn, r, cc, acted)
        time.sleep(0.3)
    raise StepFailed("run_timeout", {"state": r.get("state"), "node_statuses": r.get("node_statuses")})


def full_stop(rn: str) -> None:
    st, r = get(f"/api/program/runs/{rn}")
    if r.get("state") not in ACTIVE_RUN:
        return
    post(f"/api/program/runs/{rn}/full-stop", {
        **attribution("soak-halt"), "program_run_id": rn, "expected_owner_epoch": r["owner_epoch"],
        "reason": "load soak cleanup"})
    for _ in range(60):
        time.sleep(1)
        st, r = get(f"/api/program/runs/{rn}")
        if r.get("state") not in ACTIVE_RUN:
            return


def run_to_end(scenario: str, timeout: float) -> dict[str, Any]:
    rn = start_run()
    r = drive_run(rn, scenario, timeout)
    return {"run_id": rn, "state": r.get("state"), "reason_code": r.get("reason_code")}


def run_then_stop(scenario: str, timeout: float) -> dict[str, Any]:
    """Execute; once a group has moved for a second, operator Stop of all
    Program motion (motion-stop, every group); wait until no group is
    running or stopping and one is held."""
    rn = start_run()
    seen: dict[str, float] = {}

    def moving(r: dict[str, Any], cc: dict[str, Any]) -> bool:
        if any(g.get("state") == "running" for g in cc["execution_selections"]["groups"]):
            seen.setdefault("t", time.time())
            return time.time() - seen["t"] > 1.0
        return False

    r = drive_run(rn, scenario, timeout, until=moving)
    if r.get("state") not in ACTIVE_RUN:
        return {"run_id": rn, "state": r.get("state"), "note": "run ended before Stop"}
    cc = controls(rn) or {}
    st, js = post(f"/api/program/runs/{rn}/motion-stop", {
        **attribution("soak-stop"), "program_run_id": rn, "expected_owner_epoch": cc["owner_epoch"],
        "group_ids": ["all"], "reason": "operator motion Stop"})
    if st != 200:
        raise StepFailed("motion_stop", {"http": st, "error": err(js)})
    states: dict[str, Any] = {}
    for _ in range(60):
        time.sleep(0.5)
        cc = controls(rn) or {}
        states = {g["group_id"]: g.get("state") for g in cc.get("execution_selections", {}).get("groups", [])}
        if "held" in states.values() and not {"running", "stopping"} & set(states.values()):
            return {"run_id": rn, "groups": states}
    raise StepFailed("motion_stop_held", {"groups": states})


def reset_cycle() -> None:
    st, cy = get("/api/cycle/status")
    if cy.get("state") == "idle_after_reset" and cy.get("program_run_id") is None:
        return
    body = {**attribution("soak-reset"), "program_run_id": cy["program_run_id"],
            "expected_owner_epoch": cy["program_runner_owner_epoch"],
            "expected_cycle_generation": cy["cycle_generation"],
            "expected_claim_generation": cy["cycle_claim_generation"],
            "terminal_reference": cy["terminal_or_settlement_reference"],
            "settlement_evidence_digest": cy["terminal_or_settlement_reference"],
            "reload_initial_scene": False, "reset_robot_position": False}
    st, js = post("/api/program/cycle/reset", body, timeout=120)
    if st != 200:
        raise StepFailed("reset", {"http": st, "error": err(js), "cycle": cy.get("state")})
    for _ in range(120):
        st, cy = get("/api/cycle/status")
        if cy.get("state") == "idle_after_reset":
            LAST["reset"] = time.time()
            return
        time.sleep(0.5)
    raise StepFailed("reset_wait", {"cycle": cy.get("state"), "failure": cy.get("failure")})


# ---------------------------------------------------------------- failed Apply

def provoke_failed_apply() -> dict[str, Any]:
    """A failed Apply: a Deployment edit of an arm (its initial joint
    positions nudged by 0.01 rad) replaces it; the replacement's
    ros2_control_node is SIGSTOPped as soon as it appears (a driver hung at
    bring-up), so the arm's prepare fails. Raises ``e_unavailable`` when the
    Apply still succeeded."""
    st, ws = get("/api/deployment/workspace")
    doc = ws["draft"]["document"]
    target = None
    for i, comp in enumerate(doc["components"]):
        rc = (comp.get("configuration") or {}).get("runtime_configuration")
        if isinstance(rc, dict) and rc.get("initial_joint_positions_rad"):
            target = (i, comp["component_id"], rc["initial_joint_positions_rad"])
            break
    if target is None:
        raise StepFailed("e_unavailable", {"note": "no arm with initial_joint_positions_rad"})
    i, arm, original = target
    before = {p for p, _ in component_processes(arm)}
    path = ["components", i, "configuration", "runtime_configuration", "initial_joint_positions_rad"]
    mutate(path, [round(original[0] + 0.01, 4), *original[1:]])
    st, pv = post("/api/deployment/preview", {}, timeout=120)
    if st != 200:
        raise StepFailed("e_setup", {"preview": st, "error": err(pv)})
    if WATCH:
        WATCH.induced.add(arm)
    stopped: list[int] = []
    done = threading.Event()

    def hang_new_node() -> None:
        while not done.is_set() and not stopped:
            fresh = [p for p, _ in component_processes(arm) if p not in before]
            if fresh:
                signal_pids(fresh, "STOP")
                stopped.extend(fresh)
            time.sleep(0.1)

    hanger = threading.Thread(target=hang_new_node, daemon=True)
    hanger.start()
    st, ap = post("/api/deployment/apply", {"scope": pv["scope"]}, timeout=400)
    done.set()
    hanger.join()
    result = {"arm": arm, "stopped_pids": stopped, "apply_http": st,
              "apply_outcome": ap.get("outcome") if isinstance(ap, dict) else None, "apply_error": err(ap)}
    if st == 200 and ap.get("outcome") == "succeeded":
        raise StepFailed("e_unavailable", {"note": "edited Apply succeeded despite the hung node", **result})
    return result


def mutate(path: list[Any], value: Any) -> None:
    st, ws = get("/api/deployment/workspace")
    aw = ws["authoringWorkspace"]
    # The GUI receives the admission generation on its control WebSocket;
    # the soak reads the same value from the Headless API (read only).
    with urllib.request.urlopen(HEADLESS + "/api/cell/authoring-admission", timeout=10) as r:
        adm = json.loads(r.read())
    body = {"draftId": ws["draft"]["id"], "expectedDraftRevision": ws["draft"]["revision"],
            "expectedAdmissionGeneration": adm.get("admission_generation"),
            "requestOwnerEpoch": EPOCH, "requestId": rid("soak-dep"),
            "expectedWorkspaceEpoch": aw["workspaceEpoch"], "expectedWorkspaceRevision": aw["workspaceRevision"],
            "path": path, "operation": "set", "value": value}
    st, js = post("/api/deployment/draft/mutations", body)
    if st != 200:
        raise StepFailed("e_setup", {"mutation": st, "error": err(js)})


# ---------------------------------------------------------------- example hooks

def h_continue(rn: str, r: dict[str, Any], cc: dict[str, Any], acted: set[Any]) -> None:
    offers = cc.get("recovery_offers") or ([cc["recovery_offer"]] if cc.get("recovery_offer") else [])
    for o in offers:
        o = o.get("offer", o)
        key = (o.get("offer_id"), o.get("offer_revision"))
        if key in acted:
            continue
        if not any(a.get("action_id") == "continue_with_failure" and a.get("enabled") for a in o.get("actions", [])):
            continue
        acted.add(key)
        post("/api/cycle/recovery-actions", {
            **attribution("soak-recovery"), "expected_owner_epoch": cc["owner_epoch"],
            "offer_id": o["offer_id"], "offer_revision": o["offer_revision"],
            "action_id": "continue_with_failure", "reason": "operator-selected recovery action"})


def h_part_lost(rn: str, r: dict[str, Any], cc: dict[str, Any], acted: set[Any]) -> None:
    ns = r.get("node_statuses", {})
    if "w" not in acted and "running" in (ns.get("pick_retreat"), ns.get("move_to_drop_approach")):
        acted.add("w")
        post(f"/api/program/runs/{rn}/blackboard-values", {
            **attribution("soak-bb"), "program_run_id": rn, "expected_owner_epoch": cc["owner_epoch"],
            "expected_run_revision": cc["run_revision"], "key": "part_lost", "value": True})


def _find(o: Any, pred: Callable[[dict[str, Any]], bool]) -> dict[str, Any] | None:
    if isinstance(o, dict):
        if pred(o):
            return o
        o = list(o.values())
    if isinstance(o, list):
        for v in o:
            hit = _find(v, pred)
            if hit is not None:
                return hit
    return None


def set_mock(resource: str, value: Any) -> None:
    st, t = get("/api/execution/targets")
    found = _find(t, lambda d: "program_binding_id" in d and any(
        b.get("resource_role_id") == resource for b in d.get("signal_role_bindings", [])))
    if found is None:
        return
    owner = (_find(t, lambda d: "owner_epoch" in d) or {}).get("owner_epoch")
    sel = {"owner_epoch": found.get("owner_epoch", owner), "program_binding_id": found["program_binding_id"],
           "target_role_id": found["target_role_id"], "resource_role_id": resource}
    st, rd = post("/api/execution/io-signals/read", {
        "schema_version": "manyforge.target_observation_query.v1", "kind": "io_signal", **sel})
    epoch = (_find(rd, lambda d: "source_epoch" in d) or {}).get("source_epoch")
    post("/api/execution/io-signals/mock-input", {
        "schema_version": "manyforge.mock_io_input_request.v1", **sel,
        "expected_source_epoch": epoch, "value": value})


def resume_held(rn: str, cc: dict[str, Any]) -> bool:
    gs = [g["group_id"] for g in cc["execution_selections"]["groups"]
          if g.get("resume_allowed") and g.get("state") not in ("ready", "running", "unavailable")]
    if not gs:
        return False
    st, js = post(f"/api/program/runs/{rn}/resume-motion", {
        **attribution("soak-resume"), "program_run_id": rn, "expected_owner_epoch": cc["owner_epoch"],
        "group_ids": gs,
        "expected_selection_revision": cc["execution_selections"]["selection_revision"],
        "reason": "operator resume-motion"})
    return st == 200


def h_curtain(rn: str, r: dict[str, Any], cc: dict[str, Any], acted: set[Any]) -> None:
    ns = r.get("node_statuses", {})
    if "a1" not in acted and ns.get("move_a") == "running":
        acted.add("a1")
        set_mock("curtain_blocked", True)
    elif "a1" in acted and "a2" not in acted:
        time.sleep(1.5)
        acted.add("a2")
        set_mock("curtain_blocked", False)
    elif "a2" in acted and "a3" not in acted:
        if resume_held(rn, cc) or ns.get("move_a") == "success":
            acted.add("a3")
    elif "a3" in acted and "b1" not in acted and ns.get("idle_window") == "running":
        acted.add("b1")
        set_mock("curtain_blocked", True)
    elif "b1" in acted and "b2" not in acted:
        time.sleep(2)
        acted.add("b2")
        set_mock("curtain_blocked", False)
    elif "b2" in acted:
        resume_held(rn, cc)


HOOKS: dict[str, Callable[..., None]] = {
    "mock_lite6_intervention_hold": h_continue,
    "mock_lite6_part_lost_recovery": h_part_lost,
    "mock_lite6_light_curtain_rule": h_curtain,
}


# ---------------------------------------------------------------- stale component (f)

def deployment_components(examples: Path, scenario: str) -> dict[str, str | None]:
    """component_id -> runtime_kind of an example's Deployment."""
    doc = json.loads((examples / f"{scenario}.scenario" / "deployment.yaml").read_text())
    return {c["component_id"]: (c.get("configuration") or {}).get("runtime_kind") for c in doc["components"]}


CONTAINER = ""


def in_container(*argv: str) -> subprocess.CompletedProcess[str]:
    # Signals go through `docker exec`: the host user may not signal the
    # container's processes directly (EPERM).
    return subprocess.run(["docker", "exec", CONTAINER, *argv], capture_output=True, text=True, timeout=30)


def container_processes() -> list[tuple[int, str]]:
    """(pid in the container, command line) of every container process."""
    rows = []
    for line in in_container("ps", "-eo", "pid=,args=").stdout.splitlines():
        pid, _, args = line.strip().partition(" ")
        if pid.isdigit():
            rows.append((int(pid), args.strip()))
    return rows


def component_processes(component_id: str) -> list[tuple[int, str]]:
    """The ros2_control process(es) serving one component."""
    ns = f"__ns:=/manyforge/{component_id} "
    return [(pid, a) for pid, a in container_processes()
            if "/ros2_control_node " in a and ns in a + " "]


def signal_pids(pids: list[int], sig: str) -> None:
    in_container("kill", f"-{sig}", *map(str, pids))


def pid_alive(pid: int) -> bool:
    st = in_container("ps", "-o", "stat=", "-p", str(pid)).stdout.strip()
    return bool(st) and not st.startswith("Z")


def pid_state(pid: int) -> str:
    return in_container("ps", "-o", "stat=", "-p", str(pid)).stdout.strip()


def wait_component_fault(component_id: str, timeout: float) -> dict[str, Any] | None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        c = {x["component_id"]: x for x in components()}.get(component_id) or {}
        if c.get("fault") or c.get("lifecycle") in ("faulted", "offline"):
            return {"after_s": round(time.time() - t0, 2), "lifecycle": c.get("lifecycle"), "fault": c.get("fault")}
        time.sleep(0.5)
    return None


def clean_components() -> list[dict[str, Any]]:
    """Components that carry a fault or are not enabled (after bring-up)."""
    return [{"component_id": x["component_id"], "lifecycle": x.get("lifecycle"), "fault": x.get("fault")}
            for x in components() if x.get("fault")]



def host_open_fds() -> int | None:
    """The execution host's open file descriptors (must stay flat)."""
    r = in_container("bash", "-c", 'p=$(pgrep -f "^manyforge_ros2_control_execution_host" | head -1); '
                     '[ -n "$p" ] && ls /proc/$p/fd | wc -l')
    return int(r.stdout.strip()) if r.stdout.strip().isdigit() else None


def activation_store_sizes(root: Path) -> dict[str, list[int]]:
    """{store: [files, KiB]} of the activation-root content stores (content-
    store GC: they must stay bounded). Read from the host side of the
    workbench's state mount."""
    out: dict[str, list[int]] = {}
    for store in sorted(p for p in root.iterdir() if p.is_dir()) if root.is_dir() else []:
        files = [f for f in store.rglob("*") if f.is_file()]
        out[store.name] = [len(files), sum(f.stat().st_size for f in files) // 1024]
    return out


SWITCH_UNANSWERED_MESSAGE = "did not answer the controller switch"


def host_identity() -> dict[str, Any]:
    """The execution host's pid and age (is a failure the first Load after a start?)."""
    r = in_container("bash", "-c", 'p=$(pgrep -f "^manyforge_ros2_control_execution_host" | head -1); '
                     '[ -n "$p" ] && echo $p $(ps -o etimes= -p $p)')
    parts = r.stdout.split()
    return {"pid": int(parts[0]), "age_s": int(parts[1])} if len(parts) == 2 else {}


def switch_unanswered_capture(out: Path, attempt: str, container: str, component_id: str, since: float) -> dict[str, Any]:
    """Switch request unanswered: the switch
    service's endpoints, the component's launch/CM/call times, and whether a
    retried Enable succeeds without a new Load."""
    d = out / f"{attempt}-switch-unanswered-{component_id}"
    d.mkdir(parents=True, exist_ok=True)
    service = f"/manyforge/{component_id}/controller_manager/switch_controller"
    r = subprocess.run(["docker", "exec", container, "bash", "-lc",
                        f"source /opt/ros/lyrical/setup.bash; timeout 30 ros2 service info --verbose {service} --no-daemon"],
                       capture_output=True, text=True, timeout=60)
    (d / "service-info-verbose.txt").write_text(r.stdout + r.stderr)
    stamp = datetime.fromtimestamp(since - 30, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
    logs = subprocess.run(["docker", "logs", "--timestamps", "--since", stamp, container],
                          capture_output=True, timeout=60)
    marks = (f'component="{component_id}"', f"manyforge.{component_id}.", f"component={component_id}")
    keep = [l for l in (logs.stdout + logs.stderr).decode(errors="replace").splitlines()
            if any(m in l for m in marks) and ("launch_start" in l or "decision=" in l or "Activating" in l
                                              or "manyforge_component_call" in l or "Successfully switched" in l)]
    (d / "timeline.txt").write_text("\n".join(keep))
    t0 = time.time()
    st, js = post(f"/api/components/{component_id}/actions", {"action": "enable"})
    retry: dict[str, Any] = {"http": st}
    while st == 202 and time.time() - t0 < 40:
        time.sleep(0.5)
        c = {x["component_id"]: x for x in components()}.get(component_id) or {}
        last = c.get("last_action") or {}
        if c.get("lifecycle") == "enabled" or (last.get("action") == "enable" and last.get("outcome") in ("failed", "unknown")
                                                 and last.get("started_at_ns", 0) / 1e9 >= t0 - 1):
            retry.update(lifecycle=c.get("lifecycle"), outcome=last.get("outcome"), after_s=round(time.time() - t0, 1))
            break
    (d / "retry.json").write_text(json.dumps(retry))
    print(f"{time.strftime('%H:%M:%S')} switch-unanswered captured {d} retry={retry}", flush=True)
    return {"dir": str(d), "retry_enable": retry}


PROJECTION_STALE = ("manager_projection_unavailable",)


def projection_stale_capture(out: Path, attempt: str, container: str, activation_root: str) -> dict[str, Any]:
    """S1 (Load 503 manager_projection_unavailable after the post-New Scene
    clear outlasted its bound): without pausing anything, the Manager,
    Headless and Runner threads' wchan/state, the source publications' ages,
    and the cycle status's Scene clear, now and 30 s later."""
    d = out / f"{attempt}-s1"
    d.mkdir(parents=True, exist_ok=True)
    script = ('for pat in manyforge_manager_daemon "manyforge_headless_api serve" "manyforge_program_runner" '
              '"^manyforge_ros2_control_execution_host"; do p=$(pgrep -f "$pat" | head -1); [ -z "$p" ] && continue; '
              'echo "== $pat pid=$p"; for t in /proc/$p/task/*; do echo "$(basename $t) $(cat $t/wchan 2>/dev/null) '
              '$(cut -d" " -f3 $t/stat 2>/dev/null) $(cat $t/comm 2>/dev/null)"; done; done')
    pubs = Path(activation_root).parent / "publications" if activation_root else None

    def snap(tag: str) -> None:
        r = subprocess.run(["docker", "exec", "-u", "0", container, "bash", "-c", script],
                           capture_output=True, text=True, timeout=60)
        (d / f"threads-{tag}.txt").write_text(r.stdout + r.stderr)
        ages = {}
        if pubs is not None and pubs.is_dir():
            now = time.time()
            ages = {f.name: round(now - f.stat().st_mtime, 2) for f in pubs.iterdir() if f.is_file()}
        st, cy = get("/api/cycle/status")
        (d / f"state-{tag}.json").write_text(json.dumps({"publication_age_s": ages, "cycle": cy}, default=str))

    snap("now")
    time.sleep(30)
    snap("plus30s")
    print(f"{time.strftime('%H:%M:%S')} S1 captured {d}", flush=True)
    return {"dir": str(d)}


HOST_UNAVAILABLE = ("execution_owner_unavailable",)


def host_wedge_capture(out: Path, attempt: str, container: str) -> dict[str, Any]:
    """Wedged-host evidence without pausing the execution host: per-thread stat and
    wchan, the private sockets' state (host `ss`, same network namespace),
    the last 2000 log lines; then whether the wedge clears by itself within
    3 min (Components polled every 10 s, the host left untouched). No gdb:
    a full backtrace of the host's ~575 threads outlasts its 1 s Manager
    heartbeat and kills it."""
    d = out / f"{attempt}-hostwedge"
    d.mkdir(parents=True, exist_ok=True)
    pid = in_container("pgrep", "-f", "^manyforge_ros2_control_execution_host").stdout.split()
    info: dict[str, Any] = {"dir": str(d), "host_pid": pid[0] if pid else None}
    if pid:
        script = ("p=%s; for t in /proc/$p/task/*; do echo \"$(basename $t) $(cat $t/wchan 2>/dev/null) "
                  "$(cut -d' ' -f3 $t/stat 2>/dev/null) $(cat $t/comm 2>/dev/null)\"; done" % pid[0])
        r = subprocess.run(["docker", "exec", "-u", "0", container, "bash", "-c", script],
                           capture_output=True, text=True, timeout=60)
        (d / "threads-wchan-state.txt").write_text(r.stdout + r.stderr)
    ss = subprocess.run(["ss", "-xap"], capture_output=True, text=True, timeout=30).stdout
    (d / "ss-x.txt").write_text("\n".join(l for l in ss.splitlines() if "manyforge-state" in l))
    logs = subprocess.run(["docker", "logs", "--timestamps", "--tail", "2000", container],
                          capture_output=True, timeout=60)
    (d / "last-2000.log").write_bytes(logs.stdout + logs.stderr)
    t0 = time.time()
    info["cleared_after_s"] = None
    while time.time() - t0 < 180:
        st, _ = get("/api/components")
        if st == 200:
            info["cleared_after_s"] = round(time.time() - t0, 1)
            break
        time.sleep(10)
    ss2 = subprocess.run(["ss", "-xap"], capture_output=True, text=True, timeout=30).stdout
    (d / "ss-x-after.txt").write_text("\n".join(l for l in ss2.splitlines() if "manyforge-state" in l))
    print(f"{time.strftime('%H:%M:%S')} HOSTWEDGE captured {d} cleared_after_s={info['cleared_after_s']}", flush=True)
    return info


# ---------------------------------------------------------------- freeze evidence

class FreezeWatch:
    """Evidence for frozen controller nodes: whenever a component's
    measured_joints goes stale outside a Load/New (teardown), or a launch
    prints "failed to terminate", the component's ros2_control_node is
    backtraced with gdb (in the container, as root; the container needs
    SYS_PTRACE) before anything kills it. Also counts
    "actual_installed_state_unknown". Counters are per attempt."""

    COMPONENT = re.compile(r'component="([^"]+)"')

    def __init__(self, container: str, logs: Path):
        self.container = container
        self.logs = logs
        self.lock = threading.Lock()
        self.teardown = 0  # inside a Load/New request
        self.induced: set[str] = set()  # case f: stale on purpose
        self.traced: set[int] = set()
        self.attempt = "start"
        self.counts: dict[str, int] = {}
        self.traces: list[str] = []
        self.stopping = threading.Event()
        threading.Thread(target=self._poll, daemon=True).start()
        threading.Thread(target=self._follow, daemon=True).start()

    def begin(self, attempt: str) -> None:
        with self.lock:
            self.attempt, self.counts, self.traces = attempt, {}, []

    def take(self) -> dict[str, Any]:
        with self.lock:
            return {"counts": dict(self.counts), "backtraces": list(self.traces)}

    def count(self, key: str) -> None:
        with self.lock:
            self.counts[key] = self.counts.get(key, 0) + 1

    def _poll(self) -> None:
        while not self.stopping.wait(0.5):
            if self.teardown:
                continue
            for c in components():
                cid = c.get("component_id")
                stale = any(st.get("stream_id") == "measured_joints" and st.get("freshness") == "stale"
                            for st in c.get("streams") or ())
                if stale and cid not in self.induced and not self.teardown:
                    self.capture(cid, "measured_joints_stale")

    def _follow(self) -> None:
        # Reconnects when the container is replaced (a stack restart).
        while not self.stopping.is_set():
            self._follow_once()
            time.sleep(5)

    def _follow_once(self) -> None:
        p = subprocess.Popen(["docker", "logs", "-f", "--since", "1s", self.container],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert p.stdout is not None
        for raw in p.stdout:
            line = raw.decode(errors="replace")
            if "actual_installed_state_unknown" in line:
                self.count("actual_installed_state_unknown")
            if "failed to terminate" in line:
                m = self.COMPONENT.search(line)
                if m and m.group(1) not in self.induced:
                    threading.Thread(target=self.capture, args=(m.group(1), "failed_to_terminate"),
                                     daemon=True).start()

    def capture(self, component_id: str, reason: str) -> None:
        procs = component_processes(component_id)
        for pid, _ in procs:
            with self.lock:
                if pid in self.traced:
                    continue
                self.traced.add(pid)
                attempt = self.attempt
            self.count(reason)
            out = self.logs / f"{attempt}-bt-{pid}.txt"
            try:
                bt = subprocess.run(["docker", "exec", "-u", "0", self.container, "timeout", "60", "gdb", "-p",
                                     str(pid), "-batch", "-ex", "info threads", "-ex", "thread apply all bt"],
                                    capture_output=True, timeout=90)
                data = bt.stdout + bt.stderr
            except Exception as e:
                data = f"gdb failed: {e!r}".encode()
            out.write_bytes(f"component={component_id} reason={reason} pid={pid} at={time.strftime('%H:%M:%S')}\n".encode()
                            + data[-4_000_000:])
            with self.lock:
                if self.attempt == attempt:
                    self.traces.append(str(out))
            print(f"{time.strftime('%H:%M:%S')} BACKTRACE {reason} {component_id} pid={pid} -> {out}", flush=True)


WATCH: FreezeWatch | None = None


class teardown:
    """Marks a Load/New request: its teardown makes streams stale."""

    def __enter__(self) -> None:
        if WATCH:
            WATCH.teardown += 1

    def __exit__(self, *_: Any) -> None:
        if WATCH:
            WATCH.teardown -= 1


# ---------------------------------------------------------------- soak

class Soak:
    def __init__(self, args: argparse.Namespace, scenarios: list[str]):
        self.args = args
        self.scenarios = scenarios
        self.out = Path(args.out)
        (self.out / "logs").mkdir(parents=True, exist_ok=True)
        self.rows = self.out / "attempts.jsonl"
        self.log_budget = args.log_cap_mb * 1024 * 1024 - sum(
            p.stat().st_size for p in (self.out / "logs").glob("*.log"))
        self.ready: str | None = None  # Scenario loaded and brought up, idle
        self.examples = Path(args.examples)
        self.target: dict[str, str] = {}  # case f: the Scenario actually Loaded

    def sharing(self, scenario: str) -> tuple[str, str]:
        """(arm component id, another Scenario reusing it); the Scenario
        itself when none does."""
        mine = deployment_components(self.examples, scenario)
        arms = [i for i, k in mine.items() if k == "ros2_control" and i.endswith("_robot")]
        i = self.scenarios.index(scenario)
        for other in self.scenarios[i + 1:] + self.scenarios[:i]:
            shared = [a for a in arms if a in deployment_components(self.examples, other)]
            if shared:
                return shared[0], other
        return arms[0], scenario

    def other(self, scenario: str) -> str:
        i = self.scenarios.index(scenario)
        return self.scenarios[(i + 1) % len(self.scenarios)] if len(self.scenarios) > 1 else scenario

    def ensure_ready(self, scenario: str, t: dict[str, float]) -> None:
        if self.ready == scenario:
            return
        self.ready = None
        do_load(scenario, step="setup_load")
        bring_up(t, self.args.action_timeout)
        self.ready = scenario

    def case(self, case: str, s: str, setup: dict[str, Any], t: dict[str, float]) -> None:
        tm = self.args.run_timeout
        if case == "a":
            do_new()
            self.ready = None
            mark(t, "new")
        elif case == "b":
            other = self.other(s)
            if self.ready in (None, s):
                self.ensure_ready(other, t)
            setup["previous"] = self.ready
        elif case == "c_finished":
            self.ensure_ready(s, t)
            setup["run"] = run_to_end(s, tm)
        elif case == "c_stopped":
            self.ensure_ready(s, t)
            setup["run"] = run_then_stop(s, tm)
        elif case == "c_reset":
            self.ensure_ready(s, t)
            setup["run"] = run_to_end(s, tm)
            reset_cycle()
        elif case == "d":
            other = self.other(s)
            t0 = time.time()
            do_load(other, step="load_previous")
            setup["previous"] = other
            setup["previous_load_s"] = round(time.time() - t0, 2)
        elif case == "e":
            self.ensure_ready(s, t)
            setup["failed_apply"] = provoke_failed_apply()
        elif case == "f":
            self.stale_setup(s, setup)
        self.ready = None
        mark(t, "setup")
        target = setup.get("load_target", s)
        t0 = time.time()
        do_load(target)
        t["load_s"] = round(time.time() - t0, 2)
        if case == "f":
            self.stale_check(setup)
        bring_up(t, self.args.action_timeout)
        if case == "f" and clean_components():
            raise StepFailed("f_not_clean", {"components": clean_components()})
        self.ready = target

    def stale_setup(self, s: str, setup: dict[str, Any]) -> None:
        if not self.args.container:
            raise StepFailed("f_setup", {"note": "case f needs --container"})
        arm, target = self.sharing(s)
        self.ensure_ready(s, {"_t0": time.time()})
        procs = component_processes(arm)
        if not procs:
            raise StepFailed("f_setup", {"note": f"no ros2_control process found for {arm}",
                                         "processes": [a[:200] for _, a in container_processes()]})
        pids = [p for p, _ in procs]
        if WATCH:
            WATCH.induced.add(arm)
        signal_pids(pids, "STOP")
        if not all(pid_state(p).startswith("T") for p in pids):
            raise StepFailed("f_setup", {"note": "SIGSTOP did not stop the process",
                                         "states": {p: pid_state(p) for p in pids}})
        setup.update(arm=arm, load_target=target, stopped_pids=pids, stopped=[a[:300] for _, a in procs])
        setup["fault_seen"] = wait_component_fault(arm, 20)
        if setup["fault_seen"] is None:
            signal_pids(pids, "CONT")
            raise StepFailed("f_setup", {"note": f"{arm} reported no fault within 20 s of SIGSTOP"})

    def stale_check(self, setup: dict[str, Any]) -> None:
        """After the Load: the stopped process must be gone (Load = New + Apply)."""
        survivors = [p for p in setup["stopped_pids"] if pid_alive(p)]
        if survivors:
            signal_pids(survivors, "CONT")
            raise StepFailed("f_old_process_survived", {"pids": survivors})

    def attempt(self, case: str, s: str, rep: int) -> dict[str, Any]:
        row: dict[str, Any] = {"case": case, "scenario": s, "rep": rep,
                               "start": datetime.now(timezone.utc).isoformat(timespec="milliseconds")}
        setup: dict[str, Any] = {}
        t: dict[str, float] = {"_t0": time.time()}
        start = time.time()
        n = len(self.rows.read_text().splitlines()) if self.rows.exists() else 0
        if WATCH:
            WATCH.begin(f"{n:05d}-{case}-{s}")
        try:
            self.case(case, s, setup, t)
            row["outcome"] = "pass"
        except StepFailed as f:
            row.update(outcome="skip" if f.step.endswith("_unavailable") else "fail", step=f.step, info=f.info)
        except Exception as e:  # driver bug: recorded, never hidden
            row.update(outcome="fail", step="driver_exception", info={"exception": repr(e),
                                                                     "trace": traceback.format_exc()[-2000:]})
        row["duration_s"] = round(time.time() - start, 2)
        row["setup"] = setup
        row["timings"] = {k: v for k, v in t.items() if not k.startswith("_")}
        row["end"] = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
        row["crashes"] = self.crashes(start)
        if self.args.container:
            row["host_open_fds"] = host_open_fds()
            row["host"] = host_identity()
            unanswered = [f["component_id"] for f in ((row.get("info") or {}).get("failed") or [])
                          if SWITCH_UNANSWERED_MESSAGE in str((f.get("last_action") or {}).get("error"))]
            if row["outcome"] == "fail" and unanswered:
                row["switch_unanswered"] = switch_unanswered_capture(
                    self.out / "logs", f"{n:05d}-{case}-{s}", self.args.container, unanswered[0], start)
        if self.args.activation_root:
            row["activation_stores"] = activation_store_sizes(Path(self.args.activation_root))
        if WATCH:
            row["freeze_watch"] = WATCH.take()
            WATCH.induced.clear()
        stopped = setup.get("stopped_pids", []) + (setup.get("failed_apply") or {}).get("stopped_pids", [])
        alive = [p for p in stopped if pid_alive(p)]
        if alive:  # never leave a SIGSTOPped process behind
            signal_pids(alive, "CONT")
        if row["crashes"] and row["outcome"] != "fail":
            row["log"] = self.save_log(row, start)
        code = ((row.get("info") or {}).get("error") or {}) if row["outcome"] == "fail" else {}
        if isinstance(code, dict) and code.get("code") in PROJECTION_STALE and self.args.container:
            row["s1"] = projection_stale_capture(self.out / "logs", f"{n:05d}-{case}-{s}",
                                                 self.args.container, self.args.activation_root)
        if isinstance(code, dict) and code.get("code") in HOST_UNAVAILABLE and self.args.container:
            row["host_wedge"] = host_wedge_capture(self.out / "logs", f"{n:05d}-{case}-{s}", self.args.container)
        if row["outcome"] == "fail":
            row["setup_failure"] = row["step"] in ("setup_load", "load_previous", "execute", "motion_start",
                                                  "run_timeout", "motion_stop", "motion_stop_held",
                                                  "reset", "reset_wait", "e_setup", "f_setup") and "load_s" not in t
            row["snapshot"] = snapshot()
            row["log"] = self.save_log(row, start)
            self.recover(row)
        with self.rows.open("a") as f:
            f.write(json.dumps(row, default=str) + "\n")
        return row

    def recover(self, row: dict[str, Any]) -> None:
        """Back to a known cell: New. An active run is halted first."""
        self.ready = None
        st, cy = get("/api/cycle/status")
        rn = cy.get("program_run_id") if isinstance(cy, dict) else None
        if rn:
            full_stop(rn)
        try:
            do_new("recover_new")
            row["recovered"] = True
        except StepFailed as f:
            row["recovered"] = False
            row["recover_failure"] = {"step": f.step, "info": f.info}
            gone = ((f.info or {}).get("error") or {}).get("code") in HOST_UNAVAILABLE
            if gone and self.args.restart_cmd:
                # The execution host is gone (e.g. F8): restart the stack,
                # recorded on the row; the failure stays counted.
                r = subprocess.run(self.args.restart_cmd, shell=True, capture_output=True, text=True,
                                   timeout=3600)
                row["stack_restarted"] = {"rc": r.returncode, "tail": (r.stdout + r.stderr)[-500:]}
                print(f"{time.strftime('%H:%M:%S')} STACK RESTARTED rc={r.returncode}", flush=True)
                try:
                    do_new("recover_new")
                    row["recovered"] = True
                except StepFailed as f2:
                    row["recover_failure_after_restart"] = {"step": f2.step, "info": f2.info}

    CRASH_MARKS = ("process has died", "terminate called", "Segmentation fault", "core dumped",
                   "Aborted (", "std::terminate", "malloc", "free(): ", "double free",
                   "corrupted", "AddressSanitizer", "execution host child exited")

    def crashes(self, start: float) -> list[str]:
        """Crash lines of any process in the attempt window (counted, not
        failing the Load: crashes are investigated on their own)."""
        if not self.args.container:
            return []
        since = datetime.fromtimestamp(start - 1, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
        try:
            p = subprocess.run(["docker", "logs", "--since", since, self.args.container],
                               capture_output=True, timeout=60)
        except Exception as e:
            return [f"docker logs failed: {e!r}"]
        lines = (p.stdout + p.stderr).decode(errors="replace").splitlines()
        # exit code -9 / -2: the group SIGKILL of a hung node (cases e/f, or a
        # freeze, which the FreezeWatch backtraces) or the SIGINT of a teardown
        # (a spawner still starting, case d): a kill, not a crash.
        return [line[:400] for line in lines
                if any(m in line for m in self.CRASH_MARKS)
                and "exit code -9" not in line and "exit code -2" not in line][:10]

    def save_log(self, row: dict[str, Any], start: float) -> str | None:
        if self.log_budget <= 0 or not self.args.container:
            return None
        since = datetime.fromtimestamp(start - 2, timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")
        name = f"{len(self.rows.read_text().splitlines()) if self.rows.exists() else 0:05d}-{row['case']}-{row['scenario']}.log"
        cap = min(self.args.log_file_cap_mb * 1024 * 1024, self.log_budget)
        try:
            p = subprocess.run(["docker", "logs", "--timestamps", "--since", since, self.args.container],
                               capture_output=True, timeout=60)
        except Exception as e:
            return f"docker logs failed: {e!r}"
        data = p.stdout + p.stderr
        if len(data) > cap:
            data = b"[truncated to the last %d bytes]\n" % cap + data[-cap:]
        (self.out / "logs" / name).write_bytes(data)
        self.log_budget -= len(data)
        return str(self.out / "logs" / name)

    def run(self) -> None:
        cases = self.args.cases
        for rep in range(self.args.rep_offset, self.args.rep_offset + self.args.repeat):
            for s in self.scenarios:
                for case in cases:
                    row = self.attempt(case, s, rep)
                    print(f"{time.strftime('%H:%M:%S')} rep={rep} {case:10s} {s:42s} {row['outcome']:4s} "
                          f"{row['duration_s']:7.1f}s load={row['timings'].get('load_s')} fds={row.get('host_open_fds')}"
                          + (f" CRASH-LINES={len(row['crashes'])}" if row.get("crashes") else "")
                          + ("" if row["outcome"] == "pass" else f" step={row['step']} {json.dumps(row.get('info'), default=str)[:300]}"),
                          flush=True)
            summarize(self.out)


def mark(t: dict[str, float], name: str) -> None:
    now = time.time()
    t[name + "_s"] = round(now - t["_t0"], 2)
    t["_t0"] = now


def summarize(out: Path) -> dict[str, Any]:
    rows = [json.loads(line) for line in (out / "attempts.jsonl").read_text().splitlines() if line.strip()]
    table: dict[str, dict[str, dict[str, int]]] = {}
    classes: dict[str, dict[str, Any]] = {}
    crashes: dict[str, int] = {}
    watch: dict[str, int] = {}
    for r in rows:
        for k, v in ((r.get("freeze_watch") or {}).get("counts") or {}).items():
            watch[k] = watch.get(k, 0) + v
        if r.get("crashes"):
            crashes[r["case"]] = crashes.get(r["case"], 0) + 1
        cell = table.setdefault(r["case"], {}).setdefault(r["scenario"], {"pass": 0, "fail": 0, "setup_fail": 0, "skip": 0})
        if r["outcome"] in ("pass", "skip"):
            cell[r["outcome"]] += 1
            continue
        cell["setup_fail" if r.get("setup_failure") else "fail"] += 1
        info = r.get("info") or {}
        e = info.get("error") or {}
        code = e.get("code") if isinstance(e, dict) else None
        key = f"{r['step']}:{code or (info.get('failed') and 'action_failed') or (info.get('timeout_s') and 'timeout') or '-'}"
        c = classes.setdefault(key, {"count": 0, "examples": []})
        c["count"] += 1
        if len(c["examples"]) < 3:
            c["examples"].append({"case": r["case"], "scenario": r["scenario"], "rep": r["rep"], "start": r["start"],
                                  "message": (e.get("message") if isinstance(e, dict) else None), "log": r.get("log")})
    summary = {"attempts": len(rows), "passes": sum(r["outcome"] == "pass" for r in rows),
               "table": table, "failure_classes": classes, "attempts_with_crash_lines": crashes,
               "freeze_watch_events": watch}
    (out / "summary.json").write_text(json.dumps(summary, indent=1))
    lines = [f"attempts {summary['attempts']}  passes {summary['passes']}", "", "case        pass  fail  setup_fail  skip"]
    for case, per in table.items():
        p = sum(v["pass"] for v in per.values())
        f = sum(v["fail"] for v in per.values())
        sf = sum(v["setup_fail"] for v in per.values())
        sk = sum(v["skip"] for v in per.values())
        lines.append(f"{case:10s} {p:5d} {f:5d} {sf:5d}       {sk:5d}")
    lines += ["", "failure classes:"] + [f"  {k}: {v['count']}" for k, v in classes.items()]
    lines += ["", "attempts with crash lines (per case): " + json.dumps(crashes)]
    lines += ["freeze watch events: " + json.dumps(watch)]
    (out / "summary.txt").write_text("\n".join(lines) + "\n")
    return summary


def main() -> int:
    global BASE, HEADLESS, CONTAINER
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base-url", default=BASE)
    ap.add_argument("--headless-url", default=HEADLESS, help="read-only admission generation (case e)")
    ap.add_argument("--container", default="", help="workbench container for failure logs")
    ap.add_argument("--examples", default=str(Path(__file__).resolve().parents[2] / "examples"))
    ap.add_argument("--scenarios", nargs="*", help="default: every runnable mock_* example")
    ap.add_argument("--sim", action="store_true", help="also sim_* examples (needs a reachable Isaac Sim)")
    ap.add_argument("--cases", nargs="*", default=list(ALL_CASES), choices=ALL_CASES)
    ap.add_argument("--repeat", type=int, default=20)
    ap.add_argument("--rep-offset", type=int, default=0, help="first rep index (a run resumed in the same --out)")
    ap.add_argument("--run-timeout", type=float, default=300)
    ap.add_argument("--action-timeout", type=float, default=120)
    ap.add_argument("--out", required=True)
    ap.add_argument("--log-cap-mb", type=int, default=300, help="total cap of saved failure logs")
    ap.add_argument("--log-file-cap-mb", type=int, default=8)
    ap.add_argument("--summary-only", action="store_true")
    ap.add_argument("--activation-root", default="",
                    help="host path of the session's activation root (current/artifacts): store sizes per attempt")
    ap.add_argument("--restart-cmd", default="", help="shell command restarting the stack when its host is gone")
    ap.add_argument("--no-freeze-watch", action="store_true", help="no gdb backtraces of frozen nodes")
    args = ap.parse_args()
    BASE = args.base_url.rstrip("/")
    HEADLESS = args.headless_url.rstrip("/")
    CONTAINER = args.container
    if args.summary_only:
        print((Path(args.out) / "summary.txt").read_text() if summarize(Path(args.out)) else "")
        return 0
    if args.scenarios:
        scenarios = args.scenarios
    else:
        names = sorted(p.name[: -len(".scenario")] for p in Path(args.examples).glob("*.scenario"))
        scenarios = [n for n in names if n.startswith("mock_") and n not in NOT_RUNNABLE]
        if args.sim:
            scenarios += [n for n in names if n.startswith("sim_")]
    global WATCH
    if args.container and not args.no_freeze_watch:
        WATCH = FreezeWatch(args.container, Path(args.out) / "logs")
        (Path(args.out) / "logs").mkdir(parents=True, exist_ok=True)
    Soak(args, scenarios).run()
    print((Path(args.out) / "summary.txt").read_text())
    return 0


if __name__ == "__main__":
    sys.exit(main())
